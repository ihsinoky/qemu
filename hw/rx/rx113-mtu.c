/*
 * Renesas RX113 multi-function timer pulse unit 2 (MTU2a)
 * R01UH0448EJ0120 Rev.1.20, sections 11, 14 and 20.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/host-utils.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "hw/rx/rx113_mtu.h"
#include "migration/vmstate.h"

#define CHANNELS 8

/* MTU5's three independent counters occupy entries 5 (U), 6 (V), 7 (W). */
typedef struct RX113MTUChannel {
    uint16_t count;
    uint16_t tgr[6];
    uint8_t tcr;
    uint8_t mode;
    uint8_t tior[2];
    uint8_t tier;
    uint8_t tier2;
    uint8_t tbtm;
    bool down;
    uint64_t phase;
} RX113MTUChannel;

struct RX113MTUState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    QEMUTimer *timer;
    qemu_irq irq[RX113_MTU_IRQS];
    qemu_irq event[0x1b];
    qemu_irq mtioc[RX113_MTIOC_PINS];
    qemu_irq mtioc_oe[RX113_MTIOC_PINS];
    uint16_t output;
    uint8_t pending_matches[5];
    uint8_t toer;
    uint8_t tocr1;
    uint8_t tocr2;
    uint8_t tolbr;
    uint32_t elc_events;
    RX113MTUChannel ch[CHANNELS];
    uint64_t hz;
    int64_t last_ns;
    bool stopped;
    uint8_t start;
    uint8_t sync;
    uint8_t rwe;
    bool rwe_read;
    uint8_t start5;
    uint8_t tier5;
    uint8_t clear5;
    uint8_t mtclk;
    uint32_t held_irqs;
};

static const unsigned start_bit[] = { 0, 1, 2, 6, 7, 2, 1, 0 };
static const unsigned tgr_count[] = { 6, 2, 2, 4, 4, 1, 1, 1 };
static const unsigned irq_base[] = { 0, 7, 11, 15, 20, 25, 26, 27 };

static bool mtu_running(RX113MTUState *s, unsigned n)
{
    return !s->stopped && ((n < 5 ? s->start : s->start5) &
                           (1 << start_bit[n]));
}

static unsigned mtu_divisor(RX113MTUState *s, unsigned n)
{
    static const unsigned divisors[CHANNELS][8] = {
        { 1, 4, 16, 64 },
        { 1, 4, 16, 64, 0, 0, 256 },
        { 1, 4, 16, 64, 0, 0, 0, 1024 },
        { 1, 4, 16, 64, 256, 1024 },
        { 1, 4, 16, 64, 256, 1024 },
        { 1, 4, 16, 64 }, { 1, 4, 16, 64 }, { 1, 4, 16, 64 },
    };
    RX113MTUChannel *c = &s->ch[n];
    unsigned mode = c->mode & 15;
    unsigned div = divisors[n][c->tcr & 7];

    if (mode != 0 && mode != 2 && !(mode == 3 && n < 3)) {
        return 0;
    }
    /* Table 20.6-10: both-edge selection is ignored for PCLK/1. */
    if (n < 5 && div > 1 && (c->tcr & 0x10)) {
        div /= 2;
    }
    return div;
}

static unsigned mtu_io(RX113MTUChannel *c, unsigned gr)
{
    return (c->tior[gr / 2] >> (4 * (gr & 1))) & 15;
}

static const unsigned output_base[] = { 0, 4, 6, 8, 12 };

static bool mtu_compare(RX113MTUState *s, unsigned n, unsigned gr)
{
    RX113MTUChannel *c = &s->ch[n];

    if (n >= 5) {
        return !(c->tior[0] & 0x10);
    }
    if (gr == 2 && (c->mode & 0x10)) {
        return false;
    }
    if (gr == 3 && (c->mode & 0x20)) {
        return false;
    }
    return gr >= 4 || mtu_io(c, gr) < 8;
}

static int mtu_clear_gr(RX113MTUChannel *c)
{
    switch (c->tcr >> 5) {
    case 1:
        return 0;
    case 2:
        return 1;
    case 5:
        return 2;
    case 6:
        return 3;
    default:
        return -1;
    }
}

/* Tables 20.12-27 and 20.46: only MTU0..4 have output pads. */
static bool mtu_output_enabled(RX113MTUState *s, unsigned n, unsigned gr)
{
    RX113MTUChannel *c = &s->ch[n];
    unsigned mode = c->mode & 15;
    static const int oe[2][4] = { { -1, 0, -1, 3 }, { 1, 2, 4, 5 } };

    if ((n >= 3 && (s->ch[3].mode & 15) >= 8) ||
        !mtu_compare(s, n, gr) || !(mtu_io(c, gr) & 3) ||
        (mode == 2 && (gr & 1)) ||
        (mode == 3 && mtu_clear_gr(c) == gr) || mode >= 8) {
        return false;
    }
    if (n >= 3 && oe[n - 3][gr] >= 0 &&
        !(s->toer & (1 << oe[n - 3][gr]))) {
        return false;
    }
    return true;
}

static void mtu_output_update(RX113MTUState *s)
{
    for (unsigned n = 0; n < 5; n++) {
        for (unsigned gr = 0; gr < MIN(tgr_count[n], 4); gr++) {
            unsigned pin = output_base[n] + gr;

            qemu_set_irq(s->mtioc[pin], (s->output >> pin) & 1);
            qemu_set_irq(s->mtioc_oe[pin], mtu_output_enabled(s, n, gr));
        }
    }
}

static void mtu_output_initial(RX113MTUState *s, unsigned n, unsigned gr)
{
    unsigned pin = output_base[n] + gr;

    s->output = deposit32(s->output, pin, 1, !!(mtu_io(&s->ch[n], gr) & 4));
}

static uint16_t mtu_output_action(uint16_t output, unsigned pin, unsigned io)
{
    switch (io & 3) {
    case 1:
        return output & ~(1 << pin);
    case 2:
        return output | (1 << pin);
    case 3:
        return output ^ (1 << pin);
    default:
        return output;
    }
}

/* Evaluate simultaneous matches together, before changing the pad level. */
static uint16_t mtu_output_result(RX113MTUState *s, unsigned n,
                                unsigned matches, bool clear)
{
    RX113MTUChannel *c = &s->ch[n];
    unsigned mode = c->mode & 15;
    uint16_t output = s->output;

    for (unsigned gr = 0; gr < MIN(tgr_count[n], 4); gr++) {
        unsigned pin = output_base[n] + gr;

        if (!mtu_output_enabled(s, n, gr)) {
            continue;
        }
        if (mode == 2) {
            unsigned pair = (matches >> gr) & 3;

            /* Equal paired TGR values retain the previous level (20.3.5). */
            if (pair == 1 || pair == 2) {
                output = mtu_output_action(output, pin,
                                           mtu_io(c, gr + (pair == 2)));
            }
        } else if (mode == 3 && clear) {
            /* A coincident cycle/duty match retains the previous level. */
            if (!(matches & (1 << gr))) {
                output = deposit32(output, pin, 1, !!(mtu_io(c, gr) & 4));
            }
        } else if (matches & (1 << gr)) {
            output = mtu_output_action(output, pin, mtu_io(c, gr));
        }
    }
    return output;
}

static void mtu_output_matches(RX113MTUState *s, unsigned n,
                              unsigned matches, bool clear)
{
    s->output = mtu_output_result(s, n, matches, clear);
}

static void mtu_buffer(RX113MTUChannel *c, unsigned matches, bool clear)
{
    static const unsigned gr[] = { 0, 1, 4 };
    static const unsigned buf[] = { 2, 3, 5 };

    for (unsigned i = 0; i < 3; i++) {
        if ((c->mode & (0x10 << i)) &&
            ((c->tbtm & (1 << i)) ? clear : (matches & (1 << gr[i])))) {
            c->tgr[gr[i]] = c->tgr[buf[i]];
        }
    }
}

static void mtu_match(RX113MTUState *s, unsigned n, unsigned gr)
{
    RX113MTUChannel *c = &s->ch[n];
    unsigned irq = irq_base[n] + gr;
    bool enabled;

    if (n >= 5) {
        enabled = s->tier5 & (1 << start_bit[n]);
    } else if (gr >= 4) {
        enabled = c->tier2 & (1 << (gr - 4));
        irq++; /* TCIV0 precedes TGIE0/TGIF0 in Table 14.3. */
    } else {
        enabled = c->tier & (1 << gr);
    }
    if (n >= 1 && n <= 4) {
        static const unsigned base[] = { 0, 0x08, 0x0c, 0x10, 0x15 };

        qemu_irq_pulse(s->event[base[n] + gr]);
    }
    if (enabled) {
        /* Section 20.6.26: zero-period PCLK/1 requests stay asserted. */
        if (n < 5 && c->tgr[gr] == 0 && mtu_clear_gr(c) >= 0 &&
            c->tgr[mtu_clear_gr(c)] == 0 &&
            mtu_compare(s, n, mtu_clear_gr(c)) && mtu_divisor(s, n) == 1) {
            if (!(s->held_irqs & (1U << irq))) {
                s->held_irqs |= 1U << irq;
                qemu_set_irq(s->irq[irq], 1);
            }
        } else {
            qemu_irq_pulse(s->irq[irq]);
        }
    }
}

/* One count edge, with compare decisions taken before buffer transfers. */
static unsigned mtu_tick(RX113MTUState *s, unsigned n, bool down)
{
    RX113MTUChannel *c = &s->ch[n];
    unsigned matches = 0, clears = 0;
    bool clear = false;
    bool wrap;
    uint16_t compare = n >= 5 ? c->count + 1 : c->count;

    for (unsigned gr = 0; gr < tgr_count[n]; gr++) {
        if (mtu_compare(s, n, gr) && c->tgr[gr] == compare) {
            matches |= 1 << gr;
            mtu_match(s, n, gr);
            clear |= n >= 5 ? (s->clear5 & (1 << start_bit[n])) != 0 :
                     mtu_clear_gr(c) == gr;
        }
    }
    wrap = down ? c->count == 0 : c->count == UINT16_MAX;
    c->down = down;
    c->count = clear ? 0 : c->count + (down ? -1 : 1);
    if (n < 5) {
        s->pending_matches[n] = matches;
        mtu_buffer(c, matches, clear);
        if (clear) {
            clears |= 1 << n;
        }
        /* A counter clear takes precedence over overflow/underflow. */
        if (wrap && !clear) {
            clears |= 1U << (n + (down ? 16 : 8));
        }
    }
    return clears;
}

static void mtu_finish_events(RX113MTUState *s, unsigned events)
{
    unsigned clears = events & 0x1f;

    for (unsigned n = 0; n < 5; n++) {
        RX113MTUChannel *c = &s->ch[n];

        if ((s->sync & (1 << start_bit[n])) && (c->tcr >> 5 & 3) == 3) {
            for (unsigned src = 0; src < 5; src++) {
                if ((events & (1 << src)) &&
                    (s->sync & (1 << start_bit[src]))) {
                    c->count = 0;
                    mtu_buffer(c, 0, true);
                    clears |= 1 << n;
                    break;
                }
            }
        }
    }
    for (unsigned n = 0; n < 5; n++) {
        mtu_output_matches(s, n, s->pending_matches[n],
                           (clears & (1 << n)) ||
                           (events & (1U << (n + 8))));
        s->pending_matches[n] = 0;
    }
    mtu_output_update(s);
    for (unsigned n = 0; n < 5; n++) {
        bool down = events & (1U << (n + 16));

        if (!(clears & (1 << n)) &&
            (events & ((1U << (n + 8)) | (1U << (n + 16))))) {
            unsigned flag = down ? 0x20 : 0x10;
            unsigned irq = irq_base[n] + (n == 1 || n == 2 ? 2 : 4);

            if (n >= 1 && n <= 4 && !(n == 3 && down)) {
                static const unsigned wrap[] = { 0, 0x0a, 0x0e, 0x14, 0x19 };

                qemu_irq_pulse(s->event[wrap[n] + down]);
            }
            if (s->ch[n].tier & flag) {
                qemu_irq_pulse(s->irq[irq + down]);
            }
            if (n == 2 && mtu_running(s, 1) &&
                (s->ch[1].tcr & 7) == 7 && (s->ch[1].mode & 15) < 4) {
                mtu_finish_events(s, mtu_tick(s, 1, down));
            }
        }
    }
}

/* Number of edges to the next compare or wrap, including disabled IRQs. */
static unsigned mtu_distance(RX113MTUState *s, unsigned n)
{
    RX113MTUChannel *c = &s->ch[n];
    unsigned distance = 65536 - c->count;

    for (unsigned gr = 0; gr < tgr_count[n]; gr++) {
        unsigned d;

        if (!mtu_compare(s, n, gr)) {
            continue;
        }
        d = (uint16_t)(c->tgr[gr] - c->count - (n >= 5));
        distance = MIN(distance, d + 1);
    }
    return distance;
}

/* A flat zero-period request cannot produce further ICU edges (20.6.26). */
static bool mtu_static_zero(RX113MTUState *s, unsigned n)
{
    RX113MTUChannel *c = &s->ch[n];
    int gr = mtu_clear_gr(c);
    unsigned matches = 0;

    if (n >= 5 || c->count || c->down || (c->mode & 0x70) ||
        (s->sync & (1 << start_bit[n])) || gr < 0 || c->tgr[gr] ||
        !mtu_compare(s, n, gr) || mtu_divisor(s, n) != 1) {
        return false;
    }
    if (n >= 1 && n <= 4) {
        static const unsigned base[] = { 0, 0x08, 0x0c, 0x10, 0x15 };

        for (unsigned i = 0; i < tgr_count[n]; i++) {
            if (c->tgr[i] == 0 && mtu_compare(s, n, i) &&
                (s->elc_events & (1U << (base[n] + i)))) {
                return false;
            }
        }
    }
    for (unsigned i = 0; i < tgr_count[n]; i++) {
        if (c->tgr[i] == 0 && mtu_compare(s, n, i)) {
            matches |= 1 << i;
        }
    }
    if (mtu_output_result(s, n, matches, true) != s->output) {
        return false;
    }
    for (unsigned i = 0; i < tgr_count[n]; i++) {
        unsigned irq = irq_base[n] + i + (i >= 4);
        bool enabled = i < 4 ? (c->tier & (1 << i)) :
                              (c->tier2 & (1 << (i - 4)));

        if (c->tgr[i] == 0 && mtu_compare(s, n, i) && enabled &&
            !(s->held_irqs & (1U << irq))) {
            return false;
        }
    }
    return true;
}

static void mtu_update_held(RX113MTUState *s)
{
    if (s->stopped) {
        return;
    }
    /* Drop a continuous request only when its source is no longer active. */
    for (unsigned i = 0; i < RX113_MTU_IRQS; i++) {
        if (s->held_irqs & (1U << i)) {
            bool active = false;

            for (unsigned ch = 0; ch < 5; ch++) {
                RX113MTUChannel *cc = &s->ch[ch];
                int gr = mtu_clear_gr(cc);

                for (unsigned match = 0; match < tgr_count[ch]; match++) {
                    unsigned irq = irq_base[ch] + match + (match >= 4);
                    bool enabled = match < 4 ? (cc->tier & (1 << match)) :
                                   (cc->tier2 & (1 << (match - 4)));

                    active |= gr >= 0 && irq == i && mtu_running(s, ch) &&
                              mtu_divisor(s, ch) == 1 && cc->count == 0 &&
                              cc->tgr[gr] == 0 && cc->tgr[match] == 0 &&
                              enabled && mtu_compare(s, ch, gr) &&
                              mtu_compare(s, ch, match);
                }
            }
            if (!active) {
                s->held_irqs &= ~(1U << i);
                qemu_set_irq(s->irq[i], 0);
            }
        }
    }
}

static int64_t mtu_deadline(RX113MTUState *s)
{
    int64_t next = INT64_MAX;

    if (s->stopped || !s->hz) {
        return next;
    }
    for (unsigned n = 0; n < CHANNELS; n++) {
        unsigned div = mtu_divisor(s, n);

        if (mtu_running(s, n) && div && !mtu_static_zero(s, n)) {
            uint64_t work = mtu_distance(s, n) * (uint64_t)div *
                            NANOSECONDS_PER_SECOND - s->ch[n].phase;
            uint64_t ns = DIV_ROUND_UP(work, s->hz);

            next = MIN(next, s->last_ns + ns);
        }
    }
    return next;
}

static void mtu_advance(RX113MTUState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    /* Order delayed events by time, including synchronous clear. */
    while (s->last_ns < now) {
        int64_t end = MIN(now, mtu_deadline(s));
        uint64_t elapsed = end - s->last_ns;
        unsigned clears = 0;

        for (unsigned n = 0; n < CHANNELS; n++) {
            RX113MTUChannel *c = &s->ch[n];
            unsigned div = mtu_divisor(s, n);
            uint64_t ticks, work, unit;

            if (!mtu_running(s, n) || !div) {
                continue;
            }
            unit = div * (uint64_t)NANOSECONDS_PER_SECOND;
            if (mtu_static_zero(s, n)) {
                c->phase = ((elapsed % NANOSECONDS_PER_SECOND) * s->hz +
                            c->phase) % unit;
                continue;
            }
            work = elapsed * s->hz + c->phase;
            ticks = work / unit;
            c->phase = work % unit;
            if (ticks) {
                unsigned d = mtu_distance(s, n);

                c->down = false;
                /* The earliest event deadline bounds work to one event. */
                c->count += ticks - 1;
                if (ticks == d) {
                    clears |= mtu_tick(s, n, false);
                } else {
                    c->count++;
                }
            }
        }
        mtu_finish_events(s, clears);
        mtu_update_held(s);
        s->last_ns = end;
    }
}

static void mtu_schedule(RX113MTUState *s)
{
    int64_t next = mtu_deadline(s);

    timer_del(s->timer);
    if (next != INT64_MAX) {
        timer_mod(s->timer, next);
    }
}

static void mtu_timer(void *opaque)
{
    RX113MTUState *s = opaque;

    mtu_advance(s);
    mtu_schedule(s);
}

void rx113_mtu_set_elc_events(DeviceState *dev, uint32_t mask)
{
    RX113MTUState *s = RX113_MTU(dev);

    if (s->elc_events != mask) {
        mtu_advance(s);
        s->elc_events = mask;
        mtu_schedule(s);
    }
}

void rx113_mtu_set_clock(DeviceState *dev, uint64_t hz, bool stopped)
{
    RX113MTUState *s = RX113_MTU(dev);

    mtu_advance(s);
    s->hz = hz;
    s->stopped = stopped;
    mtu_schedule(s);
}

/* Three operation inputs per channel: start, restart, TGRA capture. */
static void mtu_elc_input(void *opaque, int input, int level)
{
    RX113MTUState *s = opaque;
    unsigned n = input / 3 + 1;
    unsigned operation = input % 3;
    RX113MTUChannel *c = &s->ch[n];

    if (!level || s->stopped) {
        return;
    }
    mtu_advance(s);
    /* Section 20.8.2 requires normal mode for ELC operations. */
    if (c->mode != 0) {
        return;
    }
    switch (operation) {
    case 0:
        s->start |= 1 << start_bit[n];
        break;
    case 1:
        c->count = 0;
        c->phase = 0;
        break;
    case 2:
        if (mtu_running(s, n) && mtu_io(c, 0) >= 8) {
            c->tgr[0] = c->count;
            if (c->tier & 1) {
                qemu_irq_pulse(s->irq[irq_base[n]]);
            }
            if (mtu_clear_gr(c) == 0) {
                c->count = 0;
                mtu_buffer(c, 1, true);
                mtu_finish_events(s, 1 << n);
            }
        }
        break;
    }
    mtu_update_held(s);
    mtu_schedule(s);
}

/* Table 20.48-51; raw device clocks, before any board PORT/MPC routing. */
static void mtu_clock_input(void *opaque, int pin, int level)
{
    RX113MTUState *s = opaque;
    unsigned old = s->mtclk;
    unsigned clears = 0;

    mtu_advance(s);
    s->mtclk = deposit32(old, pin, 1, !!level);
    if (s->mtclk == old || s->stopped) {
        return;
    }
    for (unsigned n = 0; n < 5; n++) {
        RX113MTUChannel *c = &s->ch[n];
        unsigned mode = c->mode & 15;
        bool edge = false, down = false;

        if (!mtu_running(s, n)) {
            continue;
        }
        if ((n == 1 || n == 2) && mode >= 4 && mode <= 7) {
            unsigned a_pin = (n - 1) * 2;
            bool a = (s->mtclk >> a_pin) & 1;
            bool b = (s->mtclk >> (a_pin + 1)) & 1;
            bool a_edge = pin == a_pin;
            bool b_edge = pin == a_pin + 1;

            switch (mode) {
            case 4:
                edge = a_edge || b_edge;
                down = a_edge ? a == b : a != b;
                break;
            case 5:
                edge = a_edge && a;
                down = b;
                break;
            case 6:
                edge = (a_edge && a && !b) || (b_edge && b && !a);
                down = b_edge;
                break;
            case 7:
                edge = b_edge;
                down = a != b;
                break;
            }
        } else if (mode == 0 || mode == 2 || (mode == 3 && n < 3)) {
            int clk = -1;
            unsigned sel = c->tcr & 7;
            unsigned ckeg = (c->tcr >> 3) & 3;

            if (n == 0 && sel >= 4) {
                clk = sel - 4;
            } else if ((n == 1 || n == 2) && sel >= 4 && sel <= 5) {
                clk = sel - 4;
            } else if (n == 2 && sel == 6) {
                clk = 2;
            } else if (n >= 3 && sel >= 6) {
                clk = sel - 6;
            }
            edge = pin == clk && (ckeg >= 2 || !!level == (ckeg == 0));
        }
        if (edge) {
            clears |= mtu_tick(s, n, down);
        }
    }
    mtu_finish_events(s, clears);
    mtu_update_held(s);
    mtu_schedule(s);
}

/* Register layout from sections 20.2.1-7 and 20.2.12-16. */
enum {
    TCR, TMDR, TIOR, TIER, TSR, TCNT, TGR, TIER2, TBTM,
    TSTR, TSYR, TRWER, TSTR5, TIER5, CLEAR5, TOER, TOCR1, TOCR2, TOLBR,
};

typedef struct MTURegister {
    uint16_t addr;
    uint8_t size;
    uint8_t ch;
    uint8_t kind;
    uint8_t index;
    uint8_t mask;
} MTURegister;

static const MTURegister mtu_regs[] = {
    { 0x00a, 1, 3, TOER, 0, 0x3f },
    { 0x00e, 1, 3, TOCR1, 0, 0x4f },
    { 0x00f, 1, 3, TOCR2, 0, 0xff },
    { 0x036, 1, 0, TOLBR, 0, 0x3f },
    { 0x100, 1, 0, TCR, 0, 0xff },
    { 0x101, 1, 0, TMDR, 0, 0x7f },
    { 0x102, 1, 0, TIOR, 0, 0xff },
    { 0x104, 1, 0, TIER, 0, 0x9f },
    { 0x105, 1, 0, TSR, 0, 0x00 },
    { 0x106, 2, 0, TCNT, 0, 0x00 },
    { 0x108, 2, 0, TGR, 0, 0x00 },
    { 0x10a, 2, 0, TGR, 1, 0x00 },
    { 0x10c, 2, 0, TGR, 2, 0x00 },
    { 0x10e, 2, 0, TGR, 3, 0x00 },
    { 0x103, 1, 0, TIOR, 1, 0xff },
    { 0x120, 2, 0, TGR, 4, 0x00 },
    { 0x122, 2, 0, TGR, 5, 0x00 },
    { 0x124, 1, 0, TIER2, 0, 0x03 },
    { 0x126, 1, 0, TBTM, 0, 0x07 },
    { 0x180, 1, 1, TCR, 0, 0x7f },
    { 0x181, 1, 1, TMDR, 0, 0x0f },
    { 0x182, 1, 1, TIOR, 0, 0xff },
    { 0x184, 1, 1, TIER, 0, 0xb3 },
    { 0x185, 1, 1, TSR, 0, 0x00 },
    { 0x186, 2, 1, TCNT, 0, 0x00 },
    { 0x188, 2, 1, TGR, 0, 0x00 },
    { 0x18a, 2, 1, TGR, 1, 0x00 },
    { 0x200, 1, 2, TCR, 0, 0x7f },
    { 0x201, 1, 2, TMDR, 0, 0x0f },
    { 0x202, 1, 2, TIOR, 0, 0xff },
    { 0x204, 1, 2, TIER, 0, 0xb3 },
    { 0x205, 1, 2, TSR, 0, 0x00 },
    { 0x206, 2, 2, TCNT, 0, 0x00 },
    { 0x208, 2, 2, TGR, 0, 0x00 },
    { 0x20a, 2, 2, TGR, 1, 0x00 },
    { 0x000, 1, 3, TCR, 0, 0xff },
    { 0x002, 1, 3, TMDR, 0, 0x3f },
    { 0x008, 1, 3, TIER, 0, 0x9f },
    { 0x02c, 1, 3, TSR, 0, 0x00 },
    { 0x038, 1, 3, TBTM, 0, 0x03 },
    { 0x004, 1, 3, TIOR, 0, 0xff },
    { 0x005, 1, 3, TIOR, 1, 0xff },
    { 0x010, 2, 3, TCNT, 0, 0x00 },
    { 0x018, 2, 3, TGR, 0, 0x00 },
    { 0x01a, 2, 3, TGR, 1, 0x00 },
    { 0x024, 2, 3, TGR, 2, 0x00 },
    { 0x026, 2, 3, TGR, 3, 0x00 },
    { 0x001, 1, 4, TCR, 0, 0xff },
    { 0x003, 1, 4, TMDR, 0, 0x3f },
    { 0x009, 1, 4, TIER, 0, 0xdf },
    { 0x02d, 1, 4, TSR, 0, 0x00 },
    { 0x039, 1, 4, TBTM, 0, 0x03 },
    { 0x006, 1, 4, TIOR, 0, 0xff },
    { 0x007, 1, 4, TIOR, 1, 0xff },
    { 0x012, 2, 4, TCNT, 0, 0x00 },
    { 0x01c, 2, 4, TGR, 0, 0x00 },
    { 0x01e, 2, 4, TGR, 1, 0x00 },
    { 0x028, 2, 4, TGR, 2, 0x00 },
    { 0x02a, 2, 4, TGR, 3, 0x00 },
    { 0x280, 2, 5, TCNT, 0, 0x00 },
    { 0x282, 2, 5, TGR, 0, 0x00 },
    { 0x284, 1, 5, TCR, 0, 0x03 },
    { 0x286, 1, 5, TIOR, 0, 0x1f },
    { 0x290, 2, 6, TCNT, 0, 0x00 },
    { 0x292, 2, 6, TGR, 0, 0x00 },
    { 0x294, 1, 6, TCR, 0, 0x03 },
    { 0x296, 1, 6, TIOR, 0, 0x1f },
    { 0x2a0, 2, 7, TCNT, 0, 0x00 },
    { 0x2a2, 2, 7, TGR, 0, 0x00 },
    { 0x2a4, 1, 7, TCR, 0, 0x03 },
    { 0x2a6, 1, 7, TIOR, 0, 0x1f },
    { 0x080, 1, 0, TSTR, 0, 0xc7 },
    { 0x081, 1, 0, TSYR, 0, 0xc7 },
    { 0x084, 1, 0, TRWER, 0, 0x01 },
    { 0x2b2, 1, 0, TIER5, 0, 0x07 },
    { 0x2b4, 1, 0, TSTR5, 0, 0x07 },
    { 0x2b6, 1, 0, CLEAR5, 0, 0x07 },
};

static const MTURegister *mtu_register(hwaddr addr, unsigned size)
{
    for (unsigned i = 0; i < ARRAY_SIZE(mtu_regs); i++) {
        const MTURegister *r = &mtu_regs[i];

        if (addr == r->addr && (size == r->size ||
            (size == 2 && (r->kind == TSTR || r->kind == TSYR ||
                           r->kind == TRWER)))) {
            return r;
        }
    }
    return NULL;
}

static bool mtu_protected(RX113MTUState *s, const MTURegister *r)
{
    return !s->rwe && (r->ch == 3 || r->ch == 4) &&
           (r->kind == TCR || r->kind == TMDR || r->kind == TIOR ||
            r->kind == TIER || r->kind == TCNT ||
            r->kind == TOER || r->kind == TOCR1 || r->kind == TOCR2 ||
            (r->kind == TGR && r->index < 2));
}

static uint64_t mtu_read(void *opaque, hwaddr addr, unsigned size)
{
    RX113MTUState *s = opaque;
    const MTURegister *r = mtu_register(addr, size);
    RX113MTUChannel *c;

    if (s->stopped) {
        return 0;
    }
    mtu_advance(s);
    if (!r) {
        qemu_log_mask(LOG_UNIMP, "rx113-mtu: unsupported read 0x%"
                      HWADDR_PRIx " size %u\n", addr, size);
        return 0;
    }
    if (mtu_protected(s, r)) {
        return 0;
    }
    c = &s->ch[r->ch];
    switch (r->kind) {
    case TCR:
        return c->tcr;
    case TMDR:
        return c->mode;
    case TIOR:
        return c->tior[r->index];
    case TIER:
        return c->tier;
    case TSR:
        return c->down ? 0x7f : 0xff;
    case TCNT:
        return c->count;
    case TGR:
        return c->tgr[r->index];
    case TIER2:
        return c->tier2;
    case TBTM:
        return c->tbtm;
    case TSTR:
        return s->start;
    case TSYR:
        return s->sync;
    case TRWER:
        s->rwe_read = s->rwe;
        return s->rwe;
    case TSTR5:
        return s->start5;
    case TIER5:
        return s->tier5;
    case CLEAR5:
        return s->clear5;
    case TOER:
        return s->toer | 0xc0;
    case TOCR1:
        return s->tocr1;
    case TOCR2:
        return s->tocr2;
    case TOLBR:
        return s->tolbr;
    default:
        g_assert_not_reached();
    }
}

static void mtu_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    RX113MTUState *s = opaque;
    const MTURegister *r = mtu_register(addr, size);
    RX113MTUChannel *c;
    unsigned n;

    if (s->stopped) {
        return;
    }
    mtu_advance(s);
    if (!r) {
        qemu_log_mask(LOG_UNIMP, "rx113-mtu: unsupported write 0x%"
                      HWADDR_PRIx " size %u\n", addr, size);
        return;
    }
    if (mtu_protected(s, r)) {
        return;
    }
    n = r->ch;
    c = &s->ch[n];
    if (r->size == 1) {
        value &= r->mask;
    }
    switch (r->kind) {
    case TCR:
        c->tcr = value;
        c->phase = 0;
        break;
    case TMDR:
        c->mode = value;
        c->phase = 0;
        if ((value & 15) != 0 && (value & 15) != 2 &&
            !((value & 15) == 3 && n < 3) &&
            !((n == 1 || n == 2) && (value & 15) >= 4 &&
              (value & 15) <= 7)) {
            qemu_log_mask(LOG_UNIMP,
                          "rx113-mtu: unsupported mode %u on MTU%u\n",
                          (unsigned)value & 15, n);
        }
        break;
    case TIOR:
        c->tior[r->index] = value;
        if (n < 5 && !(s->start & (1 << start_bit[n]))) {
            for (unsigned gr = r->index * 2;
                 gr < r->index * 2 + 2; gr++) {
                mtu_output_initial(s, n, gr);
            }
        }
        if ((n >= 5 && (value & 0x10)) ||
            (n < 5 && (value & 0x88))) {
            qemu_log_mask(LOG_UNIMP,
                          "rx113-mtu: input capture path unavailable\n");
        }
        break;
    case TIER:
        c->tier = value;
        break;
    case TSR:
        break;
    case TCNT:
        c->count = value;
        if (n < 5 && value == 0) {
            mtu_output_matches(s, n, 0, true);
        }
        if (n < 5 && (s->sync & (1 << start_bit[n]))) {
            for (unsigned other = 0; other < 5; other++) {
                if (s->sync & (1 << start_bit[other])) {
                    s->ch[other].count = value;
                    if (value == 0) {
                        mtu_output_matches(s, other, 0, true);
                    }
                }
            }
        }
        break;
    case TGR:
        c->tgr[r->index] = value;
        /* Section 20.6.24: MTU5 can match even while TSTR is stopped. */
        if (n >= 5 && !mtu_running(s, n) && mtu_compare(s, n, 0) &&
            (uint16_t)(c->count + 1) == value) {
            mtu_match(s, n, 0);
            if (s->clear5 & (1 << start_bit[n])) {
                c->count = 0;
            }
        }
        break;
    case TIER2:
        c->tier2 = value;
        break;
    case TBTM:
        c->tbtm = value;
        break;
    case TSTR:
        s->start = value;
        break;
    case TSYR:
        s->sync = value;
        break;
    case TRWER:
        if (value || s->rwe_read) {
            s->rwe = value;
        }
        s->rwe_read = false;
        break;
    case TSTR5:
        s->start5 = value;
        break;
    case TIER5:
        s->tier5 = value;
        break;
    case CLEAR5:
        s->clear5 = value;
        break;
    case TOER:
        s->toer = value;
        break;
    case TOCR1:
        /* TOCL is set once until reset; PSYE remains writable (20.2.18). */
        s->tocr1 = (s->tocr1 & 8) ?
                   (s->tocr1 & 15) | (value & 0x40) : value;
        break;
    case TOCR2:
        s->tocr2 = value;
        break;
    case TOLBR:
        s->tolbr = value;
        break;
    default:
        g_assert_not_reached();
    }
    mtu_output_update(s);
    mtu_update_held(s);
    mtu_schedule(s);
}

static const MemoryRegionOps mtu_ops = {
    .read = mtu_read,
    .write = mtu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void mtu_reset_hold(Object *obj, ResetType type)
{
    RX113MTUState *s = RX113_MTU(obj);

    timer_del(s->timer);
    memset(s->ch, 0, sizeof(s->ch));
    for (unsigned n = 0; n < CHANNELS; n++) {
        for (unsigned gr = 0; gr < 6; gr++) {
            s->ch[n].tgr[gr] = UINT16_MAX;
        }
    }
    s->start = s->sync = s->start5 = s->tier5 = s->clear5 = 0;
    for (unsigned i = 0; i < RX113_MTU_IRQS; i++) {
        qemu_set_irq(s->irq[i], 0);
    }
    s->output = 0;
    memset(s->pending_matches, 0, sizeof(s->pending_matches));
    s->toer = s->tocr1 = s->tocr2 = s->tolbr = 0;
    mtu_output_update(s);
    s->held_irqs = 0;
    s->elc_events = 0;
    s->rwe = 1;
    s->rwe_read = false;
    s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static const VMStateDescription vmstate_mtu_channel = {
    .name = "rx113-mtu/channel",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16(count, RX113MTUChannel),
        VMSTATE_UINT16_ARRAY(tgr, RX113MTUChannel, 6),
        VMSTATE_UINT8(tcr, RX113MTUChannel),
        VMSTATE_UINT8(mode, RX113MTUChannel),
        VMSTATE_UINT8_ARRAY(tior, RX113MTUChannel, 2),
        VMSTATE_UINT8(tier, RX113MTUChannel),
        VMSTATE_UINT8(tier2, RX113MTUChannel),
        VMSTATE_UINT8(tbtm, RX113MTUChannel),
        VMSTATE_BOOL(down, RX113MTUChannel),
        VMSTATE_UINT64(phase, RX113MTUChannel),
        VMSTATE_END_OF_LIST()
    },
};

static int mtu_pre_save(void *opaque)
{
    mtu_advance(opaque);
    return 0;
}

static int mtu_post_load(void *opaque, int version_id)
{
    RX113MTUState *s = opaque;

    if (!s->hz || s->hz > 160000000) {
        return -EINVAL;
    }
    for (unsigned n = 0; n < CHANNELS; n++) {
        unsigned div = mtu_divisor(s, n);

        if (s->ch[n].phase >= (uint64_t)MAX(div, 1) *
                              NANOSECONDS_PER_SECOND) {
            return -EINVAL;
        }
    }
    mtu_output_update(s);
    /* Edge requests are already in ICU.IR; loading must not create an edge. */
    mtu_schedule(s);
    return 0;
}

static const VMStateDescription vmstate_mtu = {
    .name = TYPE_RX113_MTU,
    .version_id = 3,
    .minimum_version_id = 1,
    .pre_save = mtu_pre_save,
    .post_load = mtu_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(ch, RX113MTUState, CHANNELS, 1,
                             vmstate_mtu_channel, RX113MTUChannel),
        VMSTATE_UINT64(hz, RX113MTUState),
        VMSTATE_INT64(last_ns, RX113MTUState),
        VMSTATE_BOOL(stopped, RX113MTUState),
        VMSTATE_UINT8(start, RX113MTUState),
        VMSTATE_UINT8(sync, RX113MTUState),
        VMSTATE_UINT8(rwe, RX113MTUState),
        VMSTATE_BOOL(rwe_read, RX113MTUState),
        VMSTATE_UINT8(start5, RX113MTUState),
        VMSTATE_UINT8(tier5, RX113MTUState),
        VMSTATE_UINT8(clear5, RX113MTUState),
        VMSTATE_UINT8(mtclk, RX113MTUState),
        VMSTATE_UINT32(held_irqs, RX113MTUState),
        VMSTATE_UINT32_V(elc_events, RX113MTUState, 2),
        VMSTATE_UINT16_V(output, RX113MTUState, 3),
        VMSTATE_UINT8_V(toer, RX113MTUState, 3),
        VMSTATE_UINT8_V(tocr1, RX113MTUState, 3),
        VMSTATE_UINT8_V(tocr2, RX113MTUState, 3),
        VMSTATE_UINT8_V(tolbr, RX113MTUState, 3),
        VMSTATE_END_OF_LIST()
    },
};

static void mtu_init(Object *obj)
{
    RX113MTUState *s = RX113_MTU(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    s->hz = 500000;
    s->stopped = true;
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, mtu_timer, s);
    memory_region_init_io(&s->iomem, obj, &mtu_ops, s, TYPE_RX113_MTU, 0x300);
    sysbus_init_mmio(sbd, &s->iomem);
    for (unsigned i = 0; i < RX113_MTU_IRQS; i++) {
        sysbus_init_irq(sbd, &s->irq[i]);
    }
    qdev_init_gpio_out_named(DEVICE(obj), s->mtioc, "mtioc", RX113_MTIOC_PINS);
    qdev_init_gpio_out_named(DEVICE(obj), s->mtioc_oe, "mtioc-oe",
                             RX113_MTIOC_PINS);
    qdev_init_gpio_out_named(DEVICE(obj), s->event, "event", 0x1b);
    qdev_init_gpio_in_named(DEVICE(obj), mtu_elc_input, "elc", 12);
    qdev_init_gpio_in_named(DEVICE(obj), mtu_clock_input, "mtclk", 4);
}

static void mtu_finalize(Object *obj)
{
    timer_free(RX113_MTU(obj)->timer);
}

static void mtu_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->vmsd = &vmstate_mtu;
    dc->user_creatable = false;
    rc->phases.hold = mtu_reset_hold;
}

static const TypeInfo mtu_info = {
    .name = TYPE_RX113_MTU,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RX113MTUState),
    .instance_init = mtu_init,
    .instance_finalize = mtu_finalize,
    .class_init = mtu_class_init,
};

static void mtu_register_types(void)
{
    type_register_static(&mtu_info);
}

type_init(mtu_register_types)
