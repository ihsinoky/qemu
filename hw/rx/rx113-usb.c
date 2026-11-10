/*
 * Renesas RX113 USBc register and SIE boundary core
 * Hardware manual: R01UH0448EJ0120 Rev.1.20, section 27.
 *
 * No bus adapter is present. Only explicit SIE inputs complete transactions.
 * See docs/rx113/issue-45-usb0-report.md for the SIE fixture contract.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "hw/rx/rx113_usb.h"
#include "migration/vmstate.h"

#define SYSCFG     0x00
#define SYSSTS0    0x04
#define DVSTCTR0   0x08
#define INTENB0    0x30
#define INTENB1    0x32
#define BRDYENB    0x36
#define NRDYENB    0x38
#define BEMPENB    0x3a
#define SOFCFG     0x3c
#define INTSTS0    0x40
#define INTSTS1    0x42
#define BRDYSTS    0x46
#define NRDYSTS    0x48
#define BEMPSTS    0x4a
#define FRMNUM     0x4c
#define DCPCFG     0x5c
#define DCPMAXP    0x5e
#define DCPCTR     0x60
#define PIPESEL    0x64
#define PIPECFG    0x68
#define PIPEMAXP   0x6c
#define PIPEPERI   0x6e
#define USBMC      0xcc
#define USBE       0x0001
#define DCFM       0x0040
#define SCKE       0x0400
#define DIR        0x0010
#define SHTNAK     0x0080
#define DBLB       0x0200
#define BFRE       0x0400
#define ACLRM      0x0200
#define SQMON      0x0040
#define BRDYM      0x0040
#define BVAL       0x8000
#define FRDY       0x2000
#define BCLR       0x4000
#define TRENB      0x0200
#define R(s, a)    ((s)->regs[(a) / 2])
#define CTR(n)     ((n) ? 0x6e + 2 * (n) : DCPCTR)
#define TRE(n)     (0x8c + 4 * (n))
#define TRN(n)     (0x8e + 4 * (n))

/* Table 5.1: exactly 62 addresses; all but FIFO data ports require words. */
static const struct {
    uint16_t addr;
    uint16_t mask;
    uint16_t reset;
} usb_regs[] = {
    { SYSCFG,   0x0579, 0 },
    { SYSSTS0,  0, 0 },
    { DVSTCTR0, 0x0ff0, 0 },
    { 0x14, 0, 0 }, { 0x18, 0, 0 }, { 0x1c, 0, 0 },
    { 0x20, 0x852f, 0 }, { 0x22, 0, 0 },
    { 0x28, 0xb50f, 0 }, { 0x2a, 0, 0 },
    { 0x2c, 0xb50f, 0 }, { 0x2e, 0, 0 },
    { INTENB0, 0xff00, 0 }, { INTENB1, 0xd871, 0 },
    { BRDYENB, 0x03ff, 0 }, { NRDYENB, 0x03ff, 0 },
    { BEMPENB, 0x03ff, 0 }, { SOFCFG, 0x0140, 0 },
    { INTSTS0, 0, 0 }, { INTSTS1, 0, 0 },
    { BRDYSTS, 0, 0 }, { NRDYSTS, 0, 0 }, { BEMPSTS, 0, 0 },
    { FRMNUM, 0, 0 },
    { 0x54, 0xffff, 0 }, { 0x56, 0xffff, 0 },
    { 0x58, 0xffff, 0 }, { 0x5a, 0xffff, 0 },
    { DCPCFG, 0x0090, 0 }, { DCPMAXP, 0xf07f, 0x0040 },
    { DCPCTR, 0, 0x0040 }, { PIPESEL, 0x000f, 0 },
    { PIPECFG, 0, 0 }, { PIPEMAXP, 0, 0 }, { PIPEPERI, 0, 0 },
    { 0x70, 0, 0 }, { 0x72, 0, 0 }, { 0x74, 0, 0 },
    { 0x76, 0, 0 }, { 0x78, 0, 0 }, { 0x7a, 0, 0 },
    { 0x7c, 0, 0 }, { 0x7e, 0, 0 }, { 0x80, 0, 0 },
    { 0x90, 0, 0 }, { 0x92, 0, 0 },
    { 0x94, 0, 0 }, { 0x96, 0, 0 },
    { 0x98, 0, 0 }, { 0x9a, 0, 0 },
    { 0x9c, 0, 0 }, { 0x9e, 0, 0 },
    { 0xa0, 0, 0 }, { 0xa2, 0, 0 },
    { 0xb0, 0x00bf, 0 }, { USBMC, 0x0001, 0x0002 },
    { 0xd0, 0x00c0, 0 }, { 0xd2, 0x00c0, 0 },
    { 0xd4, 0x00c0, 0 }, { 0xd6, 0x00c0, 0 },
    { 0xd8, 0x00c0, 0 }, { 0xda, 0x00c0, 0 },
};

/* One packet per plane; RX113 has no programmable buffer allocation. */
typedef struct RX113USBPipe {
    uint16_t cfg;
    uint16_t maxp;
    uint16_t peri;
    uint16_t count;
    uint16_t length[2];
    uint16_t pos[2];
    uint8_t data[2][256];
    bool valid[2];
    bool last[2];
    uint8_t cpu;
    uint8_t sie;
    uint8_t errors;
    uint8_t interval;
    bool interval_started;
    bool seen;
    bool expected;
    bool tx_ready[2];
} RX113USBPipe;

struct RX113USBState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq[4]; /* USBI, USBR, D0FIFO, D1FIFO */
    uint16_t regs[0x100 / 2];
    RX113USBPipe pipe[10];
    uint64_t uclk;
    bool stopped;
    bool control_data;
    bool auto_address;
    uint8_t pending_stage;
    uint8_t address;
    uint8_t line;
    uint8_t overcurrent;
    bool vbus;
    bool id;
    bool pddet;
    bool sof_received;
    int64_t last_sof;
    int64_t sof_period;
    QEMUTimer *attach_timer;
    QEMUTimer *frame_timer;
    QEMUTimer *wake_timer;
    int64_t suspend_at;
    bool wake_driving;
    bool attached;
    bool irq_level[4];
};

static bool usb_active(RX113USBState *s)
{
    return !s->stopped && s->uclk == 48000000 &&
           (R(s, SYSCFG) & (SCKE | USBE)) == (SCKE | USBE);
}

static void usb_update_bus(RX113USBState *s);
static void usb_device_state(RX113USBState *s, unsigned state);

static bool usb_tx(RX113USBState *s, unsigned n)
{
    return n ? s->pipe[n].cfg & DIR : R(s, 0x20) & 0x20;
}

static bool usb_sie_is_tx(RX113USBState *s, unsigned n)
{
    if (n) {
        return s->pipe[n].cfg & DIR;
    }
    return R(s, SYSCFG) & DCFM ? R(s, DCPCFG) & DIR :
           (R(s, INTSTS0) & 7) == 1;
}

static bool usb_ready(RX113USBState *s, unsigned n)
{
    RX113USBPipe *p = &s->pipe[n];

    if (!usb_active(s) || (R(s, CTR(n)) & ACLRM && n)) {
        return false;
    }
    if (n && !(p->cfg & 0xc000)) {
        return false;
    }
    return usb_tx(s, n) ? !p->valid[p->cpu] : p->valid[p->cpu];
}

static unsigned usb_next(RX113USBPipe *p, unsigned plane)
{
    return p->cfg & DBLB ? plane ^ 1 : 0;
}

static void usb_update_irq(RX113USBState *s)
{
    bool level[4] = { false };
    uint16_t st0, st1;

    if (usb_active(s) && (R(s, SOFCFG) & BRDYM)) {
        R(s, BRDYSTS) = 0;
        for (unsigned n = 0; n < 10; n++) {
            if ((n || !usb_tx(s, n)) && usb_ready(s, n)) {
                R(s, BRDYSTS) |= 1U << n;
            }
        }
    }
    R(s, INTSTS0) &= ~0x0700;
    if (R(s, BRDYSTS) & R(s, BRDYENB)) {
        R(s, INTSTS0) |= 0x0100;
    }
    if (R(s, NRDYSTS) & R(s, NRDYENB)) {
        R(s, INTSTS0) |= 0x0200;
    }
    if (R(s, BEMPSTS) & R(s, BEMPENB)) {
        R(s, INTSTS0) |= 0x0400;
    }
    st0 = R(s, INTSTS0) & R(s, INTENB0);
    st1 = R(s, INTSTS1) & R(s, INTENB1);
    if (!s->stopped) {
        level[0] = st0 || st1;
        level[1] = (st0 & 0xc000) || (st1 & 0xc001);
        for (unsigned f = 1; f < 3; f++) {
            unsigned sel = R(s, f == 1 ? 0x28 : 0x2c);
            unsigned n = sel & 15;

            level[f + 1] = (sel & 0x1000) && n > 0 && n < 10 &&
                           usb_ready(s, n);
        }
    }
    for (unsigned i = 0; i < 4; i++) {
        /* Edge requests fire on a new aggregate assertion, USBR is level. */
        if (i == 1) {
            qemu_set_irq(s->irq[i], level[i]);
        } else if (level[i] && !s->irq_level[i]) {
            qemu_irq_pulse(s->irq[i]);
        }
        s->irq_level[i] = level[i];
    }
}

void rx113_usb_set_clock(DeviceState *dev, uint64_t uclk, bool stopped)
{
    RX113USBState *s = RX113_USB(dev);

    s->uclk = uclk;
    s->stopped = stopped;
    usb_update_bus(s);
    usb_update_irq(s);
}

/* Digital USB0_VBUS input; VBINT detection also works with SCKE = 0. */
static void usb_vbus(void *opaque, int line, int level)
{
    RX113USBState *s = opaque;

    bool changed = s->vbus != !!level;

    s->vbus = !!level;
    if (s->stopped || !changed) {
        return;
    }
    R(s, INTSTS0) = (R(s, INTSTS0) & ~0x80) | (level ? 0x80 : 0);
    R(s, INTSTS0) |= 0x8000;
    usb_update_irq(s);
}

/* External levels are retained even while the module is stopped/reset. */
static void usb_attach(void *opaque)
{
    RX113USBState *s = opaque;

    if (usb_active(s) && (R(s, SYSCFG) & DCFM) &&
        (s->line == 1 || s->line == 2)) {
        R(s, INTSTS1) |= 0x0800;
        s->attached = true;
    } else if (usb_active(s) && (R(s, SYSCFG) & DCFM) &&
               !s->line && s->attached) {
        R(s, INTSTS1) |= 0x1000;
        R(s, DVSTCTR0) &= ~0x10;
        s->attached = false;
        usb_update_bus(s);
    }
    usb_update_irq(s);
}

static void usb_frame_pipes(RX113USBState *s);

static void usb_frame(void *opaque)
{
    RX113USBState *s = opaque;

    if (R(s, SYSCFG) & DCFM) {
        R(s, FRMNUM) = (R(s, FRMNUM) & 0xc000) |
                       ((R(s, FRMNUM) + 1) & 0x7ff);
    }
    R(s, INTSTS0) |= 0x2000;
    usb_frame_pipes(s);
    timer_mod(s->frame_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              (R(s, SYSCFG) & DCFM ? 1000000 : s->sof_period));
    usb_update_irq(s);
}

static void usb_wake(void *opaque)
{
    RX113USBState *s = opaque;

    if (!usb_active(s)) {
        return;
    }
    if (R(s, SYSCFG) & DCFM) {
        unsigned k = (R(s, DVSTCTR0) & 7) == 1 ? 1 : 2;

        if ((R(s, DVSTCTR0) & 0x90) == 0x80 && s->line == k) {
            R(s, DVSTCTR0) |= 0x20;
        }
    } else if (R(s, DVSTCTR0) & 0x100) {
        if (s->wake_driving) {
            s->wake_driving = false;
            R(s, DVSTCTR0) &= ~0x100;
        } else {
            s->wake_driving = true;
            timer_mod(s->wake_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 10000000);
        }
    }
    usb_update_irq(s);
}

static void usb_update_bus(RX113USBState *s)
{
    bool host = R(s, SYSCFG) & DCFM;
    bool reset = host && (R(s, DVSTCTR0) & 0x70) == 0x40;
    bool host_running = host && (R(s, DVSTCTR0) & 0x10);
    bool running = usb_active(s) &&
                   (host_running || (!host && s->sof_received &&
                                     !(R(s, INTSTS0) & 0x40)));

    R(s, SYSSTS0) = (s->overcurrent << 14) | (s->id ? 4 : 0) |
                       (host_running && usb_active(s) ? 0x40 : 0) |
                       ((R(s, SYSCFG) & (USBE | 0x100)) ? s->line : 0);
    R(s, INTSTS0) = (R(s, INTSTS0) & ~0x80) | (s->vbus ? 0x80 : 0);
    if (reset) {
        R(s, DVSTCTR0) = (R(s, DVSTCTR0) & ~7) | 4;
    } else if (host && (R(s, DVSTCTR0) & 7) == 4) {
        R(s, DVSTCTR0) = (R(s, DVSTCTR0) & ~7) |
                           (s->line == 1 ? 2 : s->line == 2 ? 1 : 0);
    }
    if (running) {
        if (!timer_pending(s->frame_timer)) {
            timer_mod(s->frame_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000);
        }
    } else {
        timer_del(s->frame_timer);
    }
    if (!usb_active(s) || !host) {
        timer_del(s->attach_timer);
    } else if (!s->attached && (s->line == 1 || s->line == 2) &&
               !timer_pending(s->attach_timer)) {
        timer_mod(s->attach_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 2500);
    }
}

static void usb_line(void *opaque, int pin, int level)
{
    RX113USBState *s = opaque;
    uint8_t old = s->line;
    unsigned state = (R(s, INTSTS0) >> 4) & 7;
    bool host = R(s, SYSCFG) & DCFM;

    s->line = (s->line & ~(1 << pin)) | (!!level << pin);
    if (old == s->line || s->stopped) {
        return;
    }
    R(s, INTSTS1) |= 0x4000;
    if (!host && (state & 4) && !(R(s, DVSTCTR0) & 0x800) &&
        ((old == 1 && (s->line == 0 || s->line == 2)) ||
         (old == 2 && (s->line == 0 || s->line == 1)))) {
        R(s, INTSTS0) |= 0x4000;
        R(s, INTSTS0) &= ~0x40;
    }
    if (host && usb_active(s)) {
        timer_del(s->attach_timer);
        if (s->line == 1 || s->line == 2) {
            timer_mod(s->attach_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 2500);
        } else if (!s->line && s->attached) {
            timer_mod(s->attach_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 2500);
        }
        if ((R(s, DVSTCTR0) & 0xb0) == 0x80 &&
            s->line == ((R(s, DVSTCTR0) & 7) == 1 ? 1 : 2)) {
            timer_mod(s->wake_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 2500);
        }
    }
    usb_update_bus(s);
    usb_update_irq(s);
}

static void usb_overcurrent(void *opaque, int pin, int level)
{
    RX113USBState *s = opaque;
    uint8_t old = s->overcurrent;

    s->overcurrent = (old & ~(1 << pin)) | (!!level << pin);
    if (!s->stopped && old != s->overcurrent) {
        R(s, INTSTS1) |= 0x8000;
        usb_update_bus(s);
        usb_update_irq(s);
    }
}

static void usb_id(void *opaque, int pin, int level)
{
    RX113USBState *s = opaque;

    s->id = !!level;
    usb_update_bus(s);
}

/* Digital USB-PHY detection output only; no voltages/comparators modeled. */
static void usb_pddet(void *opaque, int pin, int level)
{
    RX113USBState *s = opaque;
    bool changed = s->pddet != !!level;

    s->pddet = !!level;
    if (!s->stopped && changed) {
        R(s, INTSTS1) |= 1;
        usb_update_irq(s);
    }
}

static int usb_reg_index(hwaddr addr)
{
    for (unsigned i = 0; i < ARRAY_SIZE(usb_regs); i++) {
        if (usb_regs[i].addr == addr) {
            return i;
        }
    }
    return -1;
}

static int usb_fifo(hwaddr addr)
{
    switch (addr) {
    case 0x14:
        return 0;
    case 0x18:
        return 1;
    case 0x1c:
        return 2;
    default:
        return -1;
    }
}

static unsigned usb_sel_addr(unsigned f)
{
    return f ? 0x24 + 4 * f : 0x20;
}

static int usb_selected(RX113USBState *s, unsigned f)
{
    unsigned n = R(s, usb_sel_addr(f)) & 15;

    return n < 10 && (n || !f) ? n : -1;
}

static void usb_clear_plane(RX113USBPipe *p, unsigned plane)
{
    p->length[plane] = 0;
    p->pos[plane] = 0;
    p->valid[plane] = false;
    p->last[plane] = false;
    p->tx_ready[plane] = false;
}

static void usb_brdy(RX113USBState *s, unsigned n)
{
    if (!(s->pipe[n].cfg & BFRE)) {
        R(s, BRDYSTS) |= 1U << n;
    }
}

static bool usb_iso(RX113USBPipe *p)
{
    return (p->cfg & 0xc000) == 0xc000;
}

static void usb_frame_pipes(RX113USBState *s)
{
    bool host = R(s, SYSCFG) & DCFM;

    for (unsigned n = 1; n < 10; n++) {
        RX113USBPipe *p = &s->pipe[n];
        unsigned period = 1U << (p->peri & 7);
        bool enabled = (R(s, CTR(n)) & 3) == 1;

        if (!usb_iso(p) && !(host && n >= 6)) {
            continue;
        }
        if (!host && p->expected && !p->seen && enabled) {
            if (usb_tx(s, n)) {
                if ((p->peri & 0x1000) && p->valid[p->sie] &&
                    p->tx_ready[p->sie]) {
                    bool ready = usb_ready(s, n);

                    usb_clear_plane(p, p->sie);
                    p->sie = usb_next(p, p->sie);
                    if (!ready) {
                        usb_brdy(s, n);
                    }
                }
            } else {
                R(s, NRDYSTS) |= 1U << n;
            }
        }
        if (!p->interval_started &&
            (host ? enabled : p->seen || (period == 1 && enabled))) {
            p->interval_started = true;
            p->interval = period - 1;
        } else if (p->interval_started) {
            p->interval = p->interval ? p->interval - 1 : period - 1;
        }
        p->expected = p->interval_started && !p->interval;
        p->seen = false;
        for (unsigned b = 0; b < 2; b++) {
            p->tx_ready[b] = p->valid[b] && (host || b == p->sie);
        }
    }
}

static void usb_release_rx(RX113USBState *s, unsigned n)
{
    RX113USBPipe *p = &s->pipe[n];

    usb_clear_plane(p, p->cpu);
    p->cpu = usb_next(p, p->cpu);
    if (p->valid[p->cpu]) {
        usb_brdy(s, n);
        if ((p->cfg & BFRE) && p->last[p->cpu] &&
            p->length[p->cpu] == 0) {
            R(s, BRDYSTS) |= 1U << n;
        }
        if (p->length[p->cpu] == 0) {
            for (unsigned f = 1; f < 3; f++) {
                if (usb_selected(s, f) == n &&
                    (R(s, usb_sel_addr(f)) & 0x2000)) {
                    usb_release_rx(s, n);
                    break;
                }
            }
        }
    }
}

static void usb_commit_tx(RX113USBState *s, unsigned n)
{
    RX113USBPipe *p = &s->pipe[n];

    p->valid[p->cpu] = true;
    p->cpu = usb_next(p, p->cpu);
    if (n && !p->valid[p->cpu]) {
        usb_brdy(s, n);
    }
}

static uint64_t usb_fifo_access(RX113USBState *s, unsigned f, unsigned size,
                                bool write, uint16_t value)
{
    int n = usb_selected(s, f);
    unsigned sel = R(s, usb_sel_addr(f));
    RX113USBPipe *p;
    unsigned plane;
    uint16_t result = 0;

    if (n < 0 || !usb_ready(s, n) ||
        (size != 1 && (size != 2 || !(sel & 0x400))) ||
        write != usb_tx(s, n)) {
        return 0;
    }
    p = &s->pipe[n];
    plane = p->cpu;
    if (write) {
        unsigned maxp = n ? p->maxp & 0x1ff : R(s, DCPMAXP) & 0x7f;

        if (!maxp || p->length[plane] + size > maxp ||
            p->length[plane] + size > sizeof(p->data[plane])) {
            return 0;
        }
        for (unsigned b = 0; b < size; b++) {
            unsigned shift = size == 2 && (sel & 0x100) ? 8 * (1 - b) : 8 * b;

            p->data[plane][p->length[plane]++] = value >> shift;
        }
        if (p->length[plane] == maxp) {
            usb_commit_tx(s, n);
        }
    } else {
        if (p->pos[plane] + size > p->length[plane]) {
            return 0;
        }
        for (unsigned b = 0; b < size; b++) {
            unsigned shift = size == 2 && (sel & 0x100) ? 8 * (1 - b) : 8 * b;

            result |= p->data[plane][p->pos[plane]++] << shift;
        }
        if (p->pos[plane] == p->length[plane]) {
            if ((p->cfg & BFRE) && p->last[plane]) {
                R(s, BRDYSTS) |= 1U << n;
            }
            if (!(p->cfg & BFRE) || !p->last[plane]) {
                usb_release_rx(s, n);
            } else if (f && (sel & 0x2000)) {
                usb_release_rx(s, n);
            }
        }
    }
    usb_update_irq(s);
    return result;
}

static uint16_t usb_fifoctr(RX113USBState *s, unsigned f)
{
    int n = usb_selected(s, f);
    RX113USBPipe *p;
    uint16_t value;

    if (n < 0) {
        return 0;
    }
    p = &s->pipe[n];
    value = usb_ready(s, n) ? FRDY : 0;
    if (usb_tx(s, n)) {
        return value | (p->valid[p->cpu] ? BVAL : 0);
    }
    if (p->valid[p->cpu]) {
        unsigned len = p->length[p->cpu];
        unsigned pos = p->pos[p->cpu];

        value |= (R(s, usb_sel_addr(f)) & 0x8000) ? len - pos : len;
    }
    return value;
}

static uint64_t rx113_usb_read(void *opaque, hwaddr addr, unsigned size)
{
    RX113USBState *s = opaque;
    unsigned n = R(s, PIPESEL) & 15;
    int f = usb_fifo(addr);

    if (s->stopped || (addr != SYSCFG && !(R(s, SYSCFG) & SCKE))) {
        return 0;
    }
    if (f >= 0) {
        return usb_fifo_access(s, f, size, false, 0);
    }
    if (size != 2 || usb_reg_index(addr) < 0) {
        return 0;
    }
    switch (addr) {
    case 0x22:
        return usb_fifoctr(s, 0);
    case 0x2a:
        return usb_fifoctr(s, 1);
    case 0x2e:
        return usb_fifoctr(s, 2);
    case 0xb0:
        return R(s, 0xb0) | ((R(s, 0xb0) & 0x10) && s->pddet ? 0x200 : 0);
    case PIPECFG:
        return n > 0 && n < 10 ? s->pipe[n].cfg : 0;
    case PIPEMAXP:
        return n > 0 && n < 10 ? s->pipe[n].maxp : 0;
    case PIPEPERI:
        return n > 0 && n < 10 ? s->pipe[n].peri : 0;
    }
    if (addr == DCPCTR || (addr >= 0x70 && addr <= 0x80)) {
        uint16_t value = R(s, addr);

        n = addr == DCPCTR ? 0 : (addr - 0x6e) / 2;
        if (usb_ready(s, n)) {
            value |= 0x8000;
        }
        if (n > 0 && n <= 5 &&
            (usb_tx(s, n) ? s->pipe[n].valid[0] || s->pipe[n].valid[1] :
                            usb_ready(s, n))) {
            value |= 0x4000;
        }
        return value;
    }
    if (addr >= 0x92 && addr <= 0xa2 && (addr & 3) == 2) {
        n = (addr - 0x8e) / 4;
        return R(s, TRE(n)) & TRENB ? s->pipe[n].count : R(s, addr);
    }
    return R(s, addr);
}

/* Function control stages follow figure 27.12; tokens do not complete data. */
static void usb_control_stage(RX113USBState *s, unsigned stage)
{
    R(s, INTSTS0) = (R(s, INTSTS0) & ~7) | stage | 0x0800;
}

static void usb_device_state(RX113USBState *s, unsigned state)
{
    if (((R(s, INTSTS0) >> 4) & 7) != state) {
        R(s, INTSTS0) = (R(s, INTSTS0) & ~0x70) | (state << 4) | 0x1000;
    }
}

static void usb_control_end(RX113USBState *s)
{
    if (s->auto_address) {
        s->address = R(s, 0x56);
        usb_device_state(s, s->address ? 2 : 1);
        s->auto_address = false;
    } else if (R(s, 0x54) == 0x0900 && !R(s, 0x58) && !R(s, 0x5a)) {
        usb_device_state(s, R(s, 0x56) ? 3 : 2);
    }
    usb_control_stage(s, 0);
}

static void usb_function_reset(RX113USBState *s)
{
    for (unsigned a = 0x54; a <= 0x5a; a += 2) {
        R(s, a) = 0;
    }
    for (unsigned n = 0; n < 10; n++) {
        R(s, CTR(n)) &= ~3;
    }
    R(s, DCPCTR) &= ~4;
    R(s, INTSTS0) &= ~0x7f;
    usb_device_state(s, 1);
    R(s, DVSTCTR0) = (R(s, DVSTCTR0) & ~7) |
                        (R(s, SYSCFG) & 0x10 ? 2 : 1);
    s->address = 0;
    s->sof_received = false;
    timer_del(s->frame_timer);
    s->auto_address = s->control_data = false;
    s->pending_stage = 0;
}

static void usb_write_ctr(RX113USBState *s, unsigned n, uint16_t value)
{
    RX113USBPipe *p = &s->pipe[n];
    uint16_t old = R(s, CTR(n));
    uint16_t mask = n == 0 ? 0x4007 : n <= 5 ? 0x0603 : 0x0203;
    bool ready = usb_ready(s, n);

    R(s, CTR(n)) = (old & (n ? SQMON : SQMON | 0x4000)) |
                   (value & mask);
    if (value & 0x80) {
        R(s, CTR(n)) |= SQMON;
    }
    if (value & 0x100) {
        R(s, CTR(n)) &= ~SQMON;
    }
    if (n == 0) {
        if (R(s, SYSCFG) & DCFM) {
            R(s, DCPCTR) &= ~4;
            if (value & 0x800) {
                R(s, DCPCTR) &= ~0x4000;
                p->errors = 0;
            }
        } else {
            R(s, DCPCTR) &= ~0x4000;
            if (R(s, INTSTS0) & 8) {
                R(s, DCPCTR) = (R(s, DCPCTR) & ~7) | (old & 7);
            }
        }
    }
    if (n && usb_iso(p) && !(R(s, SYSCFG) & DCFM) &&
        !(p->peri & 7) && (value & 3) == 1 && !p->interval_started) {
        p->interval_started = p->expected = true;
    }
    if (n && (value & ACLRM)) {
        p->interval = 0;
        p->interval_started = p->seen = p->expected = false;
        usb_clear_plane(p, 0);
        usb_clear_plane(p, 1);
        p->cpu = p->sie = p->errors = 0;
        p->count = 0;
        if (!ready && usb_tx(s, n)) {
            usb_brdy(s, n);
        }
    }
}

static void rx113_usb_write(void *opaque, hwaddr addr, uint64_t value,
                             unsigned size)
{
    RX113USBState *s = opaque;
    unsigned n = R(s, PIPESEL) & 15;
    int f = usb_fifo(addr);
    int index = usb_reg_index(addr);

    if (s->stopped || (addr != SYSCFG && !(R(s, SYSCFG) & SCKE))) {
        return;
    }
    if (f >= 0) {
        usb_fifo_access(s, f, size, true, value);
        return;
    }
    if (size != 2 || index < 0) {
        return;
    }
    switch (addr) {
    case DVSTCTR0:
        {
            uint16_t old = R(s, DVSTCTR0);

            R(s, DVSTCTR0) = (old & 7) | (value & 0xff0);
            if (!(R(s, SYSCFG) & DCFM)) {
                if ((value & 0x100) && !(old & 0x100)) {
                    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

                    timer_mod(s->wake_timer, MAX(now, s->suspend_at + 2000000));
                } else if (!(value & 0x100)) {
                    timer_del(s->wake_timer);
                    s->wake_driving = false;
                }
            }
        }
        break;
    case SYSCFG:
        if ((R(s, SYSCFG) & USBE) && !(value & USBE)) {
            R(s, DVSTCTR0) &= ~7;
            if (value & DCFM) {
                R(s, FRMNUM) &= ~0x7ff;
            } else {
                R(s, SYSSTS0) &= ~3;
                R(s, INTSTS0) &= ~0x70;
                for (unsigned a = 0x54; a <= 0x5a; a += 2) {
                    R(s, a) = 0;
                }
            }
        }
        R(s, SYSCFG) = value & 0x579;
        break;
    case DCPMAXP:
        R(s, DCPMAXP) = (R(s, DCPMAXP) & 0xf000 &
                          (R(s, DCPCTR) & 0x4000 ? 0xffff : 0)) |
                         (value & (R(s, DCPCTR) & 0x4000 ? 0x7f : 0xf07f));
        break;
    case PIPESEL:
        if ((value & 15) < 10) {
            R(s, PIPESEL) = value & 15;
        }
        break;
    case PIPECFG:
    case PIPEMAXP:
    case PIPEPERI:
        if (n > 0 && n < 10) {
            RX113USBPipe *p = &s->pipe[n];

            if (addr == PIPECFG) {
                bool tx = p->cfg & DIR;

                p->cfg = value & (n <= 5 ? 0xc69f : 0xc41f);
                if (!tx && (p->cfg & DIR) && usb_active(s)) {
                    usb_brdy(s, n);
                }
            } else if (addr == PIPEMAXP) {
                p->maxp = value & (n <= 2 ? 0xf1ff :
                                  n <= 5 ? 0xf078 : 0xf07f);
            } else {
                p->peri = value & 0x1007;
            }
        }
        break;
    case 0x54:
    case 0x56:
    case 0x58:
    case 0x5a:
        if ((R(s, SYSCFG) & DCFM) && !(R(s, DCPCTR) & 0x4000)) {
            R(s, addr) = value;
        }
        break;
    case INTSTS0:
        R(s, INTSTS0) &= value | ~0xf808;
        if (!(value & 0x800) && (R(s, INTSTS0) & 7) == 6 &&
            s->pending_stage) {
            usb_control_stage(s, s->pending_stage);
            s->pending_stage = 0;
        }
        break;
    case INTSTS1:
        R(s, INTSTS1) &= value | ~0xd871;
        break;
    case BRDYSTS:
    case NRDYSTS:
    case BEMPSTS:
        R(s, addr) &= value;
        break;
    case FRMNUM:
        R(s, FRMNUM) &= value | ~0xc000;
        break;
    case 0x20:
    case 0x28:
    case 0x2c:
        f = addr == 0x20 ? 0 : addr == 0x28 ? 1 : 2;
        if ((value & 15) < 10) {
            R(s, addr) = value & usb_regs[index].mask;
            n = value & 15;
            if ((value & 0x4000) && !usb_tx(s, n) && usb_ready(s, n)) {
                s->pipe[n].pos[s->pipe[n].cpu] = 0;
            }
        }
        break;
    case 0x22:
    case 0x2a:
    case 0x2e:
        f = addr == 0x22 ? 0 : addr == 0x2a ? 1 : 2;
        {
            int pipe = usb_selected(s, f);

            if (pipe >= 0) {
                RX113USBPipe *p = &s->pipe[pipe];
                bool ready = usb_ready(s, pipe);

                if ((value & BCLR) && (ready || pipe == 0)) {
                    if (usb_tx(s, pipe)) {
                        usb_clear_plane(p, p->cpu);
                    } else {
                        usb_release_rx(s, pipe);
                    }
                }
                if ((value & BVAL) && ready && usb_tx(s, pipe)) {
                    usb_commit_tx(s, pipe);
                }
            }
        }
        break;
    default:
        if (addr == DCPCTR || (addr >= 0x70 && addr <= 0x80)) {
            usb_write_ctr(s, addr == DCPCTR ? 0 : (addr - 0x6e) / 2, value);
        } else if (addr >= 0x90 && addr <= 0xa2) {
            n = (addr - 0x90) / 4 + 1;
            if ((addr & 3) == 0) {
                RX113USBPipe *p = &s->pipe[n];
                bool counting = (R(s, addr) & TRENB) &&
                                (R(s, CTR(n)) & 3) == 1;

                R(s, addr) = value & TRENB;
                if ((value & 0x100) && !counting &&
                    !p->valid[0] && !p->valid[1] &&
                    !p->length[0] && !p->length[1]) {
                    p->count = 0;
                }
            } else if (!(R(s, TRE(n)) & TRENB)) {
                R(s, addr) = value;
            }
        } else {
            R(s, addr) = (R(s, addr) & ~usb_regs[index].mask) |
                         (value & usb_regs[index].mask);
        }
        break;
    }
    usb_update_bus(s);
    usb_update_irq(s);
}

/*
 * D-013 input: rx:<pipe>:<hex bytes>, ack:<pipe>, nak:<pipe>,
 * stall:<pipe>, error:<pipe>. A NAK is an actual bus handshake, not a
 * register read of an empty FIFO. Errors are individual failed attempts.
 * Pipe zero additionally accepts setup:0:<eight bytes>. rx0/rx1 specify
 * the received DATA PID; rx uses the expected PID for compatible fixtures.
 * An isochronous ack denotes successful packet transmission without handshake.
 */
static void usb_sie_completion(Object *obj, const char *str, Error **errp)
{
    RX113USBState *s = RX113_USB(obj);
    g_auto(GStrv) words = g_strsplit(str, ":", -1);
    unsigned n;
    RX113USBPipe *p;
    unsigned plane;
    bool host = R(s, SYSCFG) & DCFM;
    bool rx;
    size_t len = 0;
    uint8_t data[256];

    if (!words[0] || !words[1] || strlen(words[1]) != 1 ||
        words[1][0] < '0' || words[1][0] > '9') {
        error_setg(errp, "SIE completion requires a pipe 0..9");
        return;
    }
    n = words[1][0] - '0';
    p = &s->pipe[n];
    rx = !strcmp(words[0], "rx") || !strcmp(words[0], "rx0") ||
         !strcmp(words[0], "rx1");
    if (rx || !strcmp(words[0], "setup")) {
        if (!words[2] || words[3] || strlen(words[2]) % 2 ||
            strlen(words[2]) > sizeof(data) * 2) {
            error_setg(errp, "SIE receive requires at most 256 hex bytes");
            return;
        }
        len = strlen(words[2]) / 2;
        for (unsigned i = 0; i < len; i++) {
            int high = g_ascii_xdigit_value(words[2][2 * i]);
            int low = g_ascii_xdigit_value(words[2][2 * i + 1]);

            if (high < 0 || low < 0) {
                error_setg(errp, "Invalid SIE receive hex byte");
                return;
            }
            data[i] = high * 16 + low;
        }
    } else if (words[2] ||
               (strcmp(words[0], "ack") && strcmp(words[0], "nak") &&
                strcmp(words[0], "stall") && strcmp(words[0], "error"))) {
        error_setg(errp, "Unknown SIE completion");
        return;
    }
    if (n && !strcmp(words[0], "setup")) {
        error_setg(errp, "Setup packets require pipe zero");
        return;
    }
    if (!n && !strcmp(words[0], "setup")) {
        unsigned stage;

        if (host || !usb_active(s) || len != 8) {
            error_setg(errp, "Setup requires eight bytes and active function");
            return;
        }
        for (unsigned i = 0; i < 4; i++) {
            R(s, 0x54 + 2 * i) = lduw_le_p(data + 2 * i);
        }
        R(s, DCPCTR) = (R(s, DCPCTR) & ~7) | SQMON;
        R(s, INTSTS0) |= 8;
        s->control_data = false;
        s->auto_address = R(s, 0x54) == 0x0500 &&
                          R(s, 0x56) <= 127 && !R(s, 0x58) &&
                          !R(s, 0x5a) && (R(s, INTSTS0) & 0x70) != 0x30;
        stage = !R(s, 0x5a) ? 5 : (R(s, 0x54) & 0x80) ? 1 : 3;
        if ((R(s, INTSTS0) & 0x807) == 0x806) {
            s->pending_stage = stage;
        } else {
            usb_control_stage(s, stage);
        }
        usb_update_irq(s);
        return;
    }
    if (!n && host && (R(s, DCPCTR) & 0x4000)) {
        if (!usb_active(s) || !(R(s, DVSTCTR0) & 0x10) || rx) {
            error_setg(errp, "Host setup completion requires an active setup");
            return;
        }
        if (!strcmp(words[0], "ack")) {
            R(s, INTSTS1) |= 0x10;
            R(s, DCPCTR) &= ~0x4000;
            p->errors = 0;
        } else if (++p->errors == 3) {
            R(s, INTSTS1) |= 0x20;
            R(s, DCPCTR) &= ~0x4000;
            p->errors = 0;
        }
        usb_update_irq(s);
        return;
    }
    if (!n && !host && usb_active(s)) {
        unsigned stage = R(s, INTSTS0) & 7;
        bool status = stage == 2 || stage == 4 || stage == 5;

        if (status && (s->auto_address ||
                       (!(R(s, INTSTS0) & 8) &&
                        (R(s, DCPCTR) & 7) == 5))) {
            if ((stage == 2 && rx) ||
                (stage != 2 && !strcmp(words[0], "ack"))) {
                if (stage == 2 && !strcmp(words[0], "rx0")) {
                    R(s, DCPCTR) |= 3;
                    usb_control_stage(s, 6);
                } else {
                    usb_control_end(s);
                }
                usb_update_irq(s);
                return;
            }
        }
    }
    if (!usb_active(s) || (R(s, CTR(n)) & 3) != 1 ||
        (host && !(R(s, DVSTCTR0) & 0x10))) {
        error_setg(errp, "SIE pipe is not enabled or UCLK is not 48 MHz");
        return;
    }
    if (n && (!(p->cfg & 15) ||
        ((p->cfg & 0xc000) != (n <= 5 ? 0x4000 : 0x8000) &&
         !(n <= 2 && usb_iso(p))) ||
        (p->cfg & BFRE && R(s, SOFCFG) & BRDYM))) {
        error_setg(errp, "SIE requires a supported transfer type and endpoint");
        return;
    }
    if (n && host && n >= 6 && (!p->expected || p->seen)) {
        error_setg(errp, "Host interrupt transaction is outside its frame");
        return;
    }
    if (n && usb_iso(p)) {
        bool tx = usb_tx(s, n);
        bool unavailable = tx ? !p->valid[p->sie] ||
                               (!host && !p->tx_ready[p->sie]) :
                               p->valid[p->sie];

        if (host && (!p->expected || p->seen)) {
            error_setg(errp, "Host periodic transaction is outside its frame");
            return;
        }
        if (!strcmp(words[0], "error")) {
            if (tx) {
                error_setg(errp, "Isochronous data CRC input requires receive");
                return;
            }
            p->seen = host;
            R(s, FRMNUM) |= 0x4000 | (unavailable ? 0x8000 : 0);
            R(s, NRDYSTS) |= 1U << n;
            usb_update_irq(s);
            return;
        }
        if ((!strcmp(words[0], "ack") && !tx) || (rx && tx) ||
            !strcmp(words[0], "stall") || !strcmp(words[0], "nak")) {
            error_setg(errp, "Isochronous transfer has no handshake");
            return;
        }
        if (unavailable) {
            p->seen = host;
            R(s, FRMNUM) |= 0x8000;
            R(s, NRDYSTS) |= 1U << n;
            usb_update_irq(s);
            return;
        }
    }
    if (n && !host && (R(s, CTR(n)) & 0x400) &&
        (p->cfg & 0xc000) == 0x4000) {
        if (!strcmp(words[0], "ack") && usb_tx(s, n)) {
            R(s, CTR(n)) ^= SQMON;
        } else if (!strcmp(words[0], "nak") && !usb_tx(s, n)) {
            R(s, NRDYSTS) |= 1U << n;
        } else {
            error_setg(errp, "Auto response expects IN completion or OUT NAK");
            return;
        }
        usb_update_irq(s);
        return;
    }
    if (!n && !host) {
        unsigned stage = R(s, INTSTS0) & 7;

        if ((stage != 1 && stage != 3) || (R(s, INTSTS0) & 8)) {
            error_setg(errp, "DCP data requires a released data stage");
            return;
        }
    }
    plane = p->sie;
    if (rx) {
        bool ready = usb_ready(s, n);
        unsigned maxp = n ? p->maxp & 0x1ff : R(s, DCPMAXP) & 0x7f;
        bool last = len < maxp;

        if (usb_sie_is_tx(s, n)) {
            error_setg(errp, "SIE receive requires a receiving pipe");
            return;
        }
        if (!n && !host && (R(s, INTSTS0) & 7) == 3 &&
            !s->control_data && !strcmp(words[0], "rx0")) {
            R(s, DCPCTR) |= 3;
            usb_control_stage(s, 6);
            usb_update_irq(s);
            return;
        }
        if (!usb_iso(p) && strcmp(words[0], "rx") &&
            (words[0][2] == '1') != !!(R(s, CTR(n)) & SQMON)) {
            usb_update_irq(s);
            return;
        }
        if (p->valid[plane]) {
            error_setg(errp, "SIE receive buffer is unavailable");
            return;
        }
        if (n && (R(s, CTR(n)) & ACLRM)) {
            p->errors = 0;
            if (!usb_iso(p)) {
                R(s, CTR(n)) ^= SQMON;
            }
            usb_update_irq(s);
            return;
        }
        if (len > maxp) {
            R(s, BEMPSTS) |= 1U << n;
            R(s, CTR(n)) = (R(s, CTR(n)) & ~3) | 3;
        } else {
            if (n && n <= 5 && (R(s, TRE(n)) & TRENB)) {
                p->count++;
                last |= p->count == R(s, TRN(n));
                if (last) {
                    p->count = 0;
                }
            }
            memcpy(p->data[plane], data, len);
            p->length[plane] = len;
            p->pos[plane] = 0;
            p->valid[plane] = true;
            p->last[plane] = last;
            p->sie = usb_next(p, plane);
            if (!ready) {
                usb_brdy(s, n);
            }
            if (!len && !ready && (p->cfg & BFRE)) {
                R(s, BRDYSTS) |= 1U << n;
            }
            if (!len && !ready) {
                for (unsigned f = 1; f < 3; f++) {
                    if (usb_selected(s, f) == n &&
                        (R(s, usb_sel_addr(f)) & 0x2000)) {
                        usb_release_rx(s, n);
                        break;
                    }
                }
            }
            if (last && !usb_iso(p) &&
                ((n ? p->cfg : R(s, DCPCFG)) & SHTNAK)) {
                R(s, CTR(n)) &= ~3;
            }
            if (!usb_iso(p)) {
                R(s, CTR(n)) ^= SQMON;
            }
            p->seen = true;
            if (!n) {
                s->control_data = true;
            }
        }
        p->errors = 0;
    } else if (!strcmp(words[0], "ack")) {
        bool ready = usb_ready(s, n);

        if (!usb_sie_is_tx(s, n) || !p->valid[plane]) {
            error_setg(errp, "No SIE transmit packet to acknowledge");
            return;
        }
        usb_clear_plane(p, plane);
        p->sie = usb_next(p, plane);
        if (!ready) {
            usb_brdy(s, n);
        }
        if (!p->valid[0] && !p->valid[1] &&
            !p->length[0] && !p->length[1]) {
            R(s, BEMPSTS) |= 1U << n;
        }
        if (!usb_iso(p)) {
            R(s, CTR(n)) ^= SQMON;
        }
        p->seen = true;
        if (!n) {
            s->control_data = true;
        }
        p->errors = 0;
    } else if (!strcmp(words[0], "nak")) {
        p->errors = 0;
        if (!host) {
            bool unavailable = usb_tx(s, n) ? !p->valid[plane] :
                                               p->valid[plane];

            if (!unavailable) {
                error_setg(errp, "Function NAK requires an unavailable buffer");
                return;
            }
            R(s, NRDYSTS) |= 1U << n;
        }
        /* Host NAK is a retry, not an NRDY event (§27.3.3.2). */
    } else if (host) {
        if (!strcmp(words[0], "stall")) {
            R(s, NRDYSTS) |= 1U << n;
            R(s, CTR(n)) = (R(s, CTR(n)) & ~3) | 3;
            p->errors = 0;
        } else if (++p->errors == 3) {
            R(s, NRDYSTS) |= 1U << n;
            R(s, CTR(n)) &= ~3;
            p->errors = 0;
        }
    } else {
        error_setg(errp, "STALL/error input requires host mode");
        return;
    }
    usb_update_irq(s);
}

/* Explicit SOF reception starts the function's documented interpolation. */
static void usb_sie_sof(Object *obj, const char *str, Error **errp)
{
    RX113USBState *s = RX113_USB(obj);
    unsigned frame = 0;

    if (!*str || strlen(str) > 4 || !usb_active(s) ||
        (R(s, SYSCFG) & DCFM) || (R(s, INTSTS0) & 0x40)) {
        error_setg(errp, "SOF requires unsuspended function and 0..2047");
        return;
    }
    for (const char *c = str; *c; c++) {
        if (!g_ascii_isdigit(*c)) {
            error_setg(errp, "Invalid SOF frame number");
            return;
        }
        frame = frame * 10 + *c - '0';
    }
    if (frame > 2047) {
        error_setg(errp, "Invalid SOF frame number");
        return;
    }
    if (s->sof_received) {
        s->sof_period = MAX(1, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) -
                              s->last_sof);
    } else {
        s->sof_period = 1000000;
    }
    s->last_sof = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    R(s, FRMNUM) = (R(s, FRMNUM) & 0xc000) | frame;
    R(s, INTSTS0) |= 0x2000;
    s->sof_received = true;
    usb_frame_pipes(s);
    timer_mod(s->frame_timer, s->last_sof + s->sof_period);
    usb_update_irq(s);
}

/* Explicit token input permits status-stage entry without inventing an ACK. */
static void usb_sie_token(Object *obj, const char *str, Error **errp)
{
    RX113USBState *s = RX113_USB(obj);
    unsigned stage = R(s, INTSTS0) & 7;
    bool in = !strcmp(str, "in");
    bool out = !strcmp(str, "out");

    if (!usb_active(s) || (R(s, SYSCFG) & DCFM) || (!in && !out)) {
        error_setg(errp, "Control token requires active function and in/out");
        return;
    }
    if ((stage == 1 && out) || (stage == 3 && in)) {
        if (s->control_data) {
            usb_control_stage(s, stage == 1 ? 2 : 4);
        } else {
            usb_control_stage(s, 6);
            R(s, DCPCTR) |= 3;
        }
    } else if ((stage == 2 && in) || ((stage == 4 || stage == 5) && out)) {
        usb_control_stage(s, 6);
        R(s, DCPCTR) |= 3;
    }
    usb_update_irq(s);
}

static void usb_bus_event(Object *obj, const char *str, Error **errp)
{
    RX113USBState *s = RX113_USB(obj);

    if (!usb_active(s)) {
        error_setg(errp, "Bus event requires active USB");
        return;
    }
    if (!strcmp(str, "eof-error") && (R(s, SYSCFG) & DCFM)) {
        R(s, INTSTS1) |= 0x40;
        R(s, DVSTCTR0) &= ~0x10;
    } else if (!(R(s, SYSCFG) & DCFM) && !strcmp(str, "reset")) {
        usb_function_reset(s);
    } else if (!(R(s, SYSCFG) & DCFM) && !strcmp(str, "suspend")) {
        usb_device_state(s, ((R(s, INTSTS0) >> 4) & 7) | 4);
        s->sof_received = false;
        s->suspend_at = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    } else {
        error_setg(errp, "Unsupported bus event or controller mode");
        return;
    }
    usb_update_bus(s);
    usb_update_irq(s);
}

/* Adapter/fixture output: immutable snapshot of the next SIE-owned packet. */
static char *usb_sie_tx(Object *obj, Error **errp)
{
    RX113USBState *s = RX113_USB(obj);
    GString *out = g_string_new(NULL);

    for (unsigned n = 0; n < 10; n++) {
        RX113USBPipe *p = &s->pipe[n];
        unsigned plane = p->sie;

        if ((!n && !(R(s, SYSCFG) & DCFM) &&
             ((R(s, INTSTS0) & 7) == 4 || (R(s, INTSTS0) & 7) == 5) &&
             (s->auto_address || (!(R(s, INTSTS0) & 8) &&
                                 (R(s, DCPCTR) & 7) == 5))) ||
            (n && !(R(s, SYSCFG) & DCFM) &&
             (p->cfg & 0xc010) == 0x4010 &&
             (R(s, CTR(n)) & 0x403) == 0x401)) {
            g_string_append_printf(out, "%u:;", n);
            continue;
        }
        if (!usb_sie_is_tx(s, n) || !p->valid[plane]) {
            continue;
        }
        g_string_append_printf(out, "%u:", n);
        for (unsigned b = 0; b < p->length[plane]; b++) {
            g_string_append_printf(out, "%02x", p->data[plane][b]);
        }
        g_string_append_c(out, ';');
    }
    return g_string_free(out, false);
}

static char *usb_sie_setup(Object *obj, Error **errp)
{
    RX113USBState *s = RX113_USB(obj);
    GString *out = g_string_new(NULL);

    if ((R(s, SYSCFG) & DCFM) && (R(s, DCPCTR) & 0x4000)) {
        for (unsigned a = 0x54; a <= 0x5a; a += 2) {
            g_string_append_printf(out, "%02x%02x", R(s, a) & 255,
                                   R(s, a) >> 8);
        }
    }
    return g_string_free(out, false);
}

static char *usb_bus_output(Object *obj, Error **errp)
{
    RX113USBState *s = RX113_USB(obj);
    bool host = R(s, SYSCFG) & DCFM;
    const char *drive = "idle";

    if (usb_active(s)) {
        if (host && (R(s, DVSTCTR0) & 0x70) == 0x40) {
            drive = "se0";
        } else if ((host && (R(s, DVSTCTR0) & 0x20)) ||
                   (!host && s->wake_driving)) {
            drive = "k";
        }
    }
    return g_strdup_printf("drive=%s;vbusen=%u;exicen=%u;pullup=%u;pulldown=%u",
                           drive, !!(R(s, DVSTCTR0) & 0x200),
                           !!(R(s, DVSTCTR0) & 0x400),
                           (R(s, SYSCFG) >> 3) & 3,
                           !!(R(s, SYSCFG) & 0x20));
}

static const MemoryRegionOps rx113_usb_ops = {
    .read = rx113_usb_read,
    .write = rx113_usb_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .valid.unaligned = true,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
    .impl.unaligned = true,
};

static void rx113_usb_reset_hold(Object *obj, ResetType type)
{
    RX113USBState *s = RX113_USB(obj);

    s->control_data = s->auto_address = false;
    s->pending_stage = s->address = 0;
    s->sof_received = false;
    s->suspend_at = 0;
    s->wake_driving = false;
    s->attached = false;
    s->last_sof = 0;
    s->sof_period = 1000000;
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->pipe, 0, sizeof(s->pipe));
    memset(s->irq_level, 0, sizeof(s->irq_level));
    for (unsigned i = 0; i < ARRAY_SIZE(usb_regs); i++) {
        R(s, usb_regs[i].addr) = usb_regs[i].reset;
    }
    for (unsigned n = 0; n < 10; n++) {
        s->pipe[n].maxp = 64;
    }
    timer_del(s->attach_timer);
    timer_del(s->frame_timer);
    timer_del(s->wake_timer);
    usb_update_bus(s);
    usb_update_irq(s);
}

static const VMStateDescription vmstate_usb_pipe = {
    .name = "rx113-usb/pipe",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16(cfg, RX113USBPipe),
        VMSTATE_UINT16(maxp, RX113USBPipe),
        VMSTATE_UINT16(peri, RX113USBPipe),
        VMSTATE_UINT16(count, RX113USBPipe),
        VMSTATE_UINT16_ARRAY(length, RX113USBPipe, 2),
        VMSTATE_UINT16_ARRAY(pos, RX113USBPipe, 2),
        VMSTATE_UINT8_2DARRAY(data, RX113USBPipe, 2, 256),
        VMSTATE_BOOL_ARRAY(valid, RX113USBPipe, 2),
        VMSTATE_BOOL_ARRAY(last, RX113USBPipe, 2),
        VMSTATE_UINT8(cpu, RX113USBPipe),
        VMSTATE_UINT8(sie, RX113USBPipe),
        VMSTATE_UINT8(errors, RX113USBPipe),
        VMSTATE_UINT8(interval, RX113USBPipe),
        VMSTATE_BOOL(interval_started, RX113USBPipe),
        VMSTATE_BOOL(seen, RX113USBPipe),
        VMSTATE_BOOL(expected, RX113USBPipe),
        VMSTATE_BOOL_ARRAY(tx_ready, RX113USBPipe, 2),
        VMSTATE_END_OF_LIST()
    },
};

static int rx113_usb_post_load(void *opaque, int version_id)
{
    RX113USBState *s = opaque;

    if (s->pending_stage > 5 || s->address > 127 || s->line > 3 ||
        s->overcurrent > 3 || s->sof_period <= 0) {
        return -EINVAL;
    }
    for (unsigned n = 0; n < 10; n++) {
        RX113USBPipe *p = &s->pipe[n];

        if (p->cpu > 1 || p->sie > 1 || p->errors > 2 || p->interval > 127) {
            return -EINVAL;
        }
        for (unsigned b = 0; b < 2; b++) {
            if (p->pos[b] > p->length[b] || p->length[b] > 256) {
                return -EINVAL;
            }
        }
    }
    usb_update_irq(s);
    return 0;
}

static const VMStateDescription vmstate_rx113_usb = {
    .name = TYPE_RX113_USB,
    .version_id = 2,
    .minimum_version_id = 2,
    .post_load = rx113_usb_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16_ARRAY(regs, RX113USBState, 0x100 / 2),
        VMSTATE_STRUCT_ARRAY(pipe, RX113USBState, 10, 1,
                             vmstate_usb_pipe, RX113USBPipe),
        VMSTATE_UINT64(uclk, RX113USBState),
        VMSTATE_BOOL(stopped, RX113USBState),
        VMSTATE_BOOL(control_data, RX113USBState),
        VMSTATE_BOOL(auto_address, RX113USBState),
        VMSTATE_UINT8(pending_stage, RX113USBState),
        VMSTATE_UINT8(address, RX113USBState),
        VMSTATE_UINT8(line, RX113USBState),
        VMSTATE_UINT8(overcurrent, RX113USBState),
        VMSTATE_BOOL(vbus, RX113USBState),
        VMSTATE_BOOL(id, RX113USBState),
        VMSTATE_BOOL(pddet, RX113USBState),
        VMSTATE_BOOL(sof_received, RX113USBState),
        VMSTATE_INT64(last_sof, RX113USBState),
        VMSTATE_INT64(sof_period, RX113USBState),
        VMSTATE_TIMER_PTR(attach_timer, RX113USBState),
        VMSTATE_TIMER_PTR(frame_timer, RX113USBState),
        VMSTATE_TIMER_PTR(wake_timer, RX113USBState),
        VMSTATE_INT64(suspend_at, RX113USBState),
        VMSTATE_BOOL(wake_driving, RX113USBState),
        VMSTATE_BOOL(attached, RX113USBState),
        VMSTATE_BOOL_ARRAY(irq_level, RX113USBState, 4),
        VMSTATE_END_OF_LIST()
    },
};

static void rx113_usb_init(Object *obj)
{
    RX113USBState *s = RX113_USB(obj);

    s->stopped = true;
    s->attach_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, usb_attach, s);
    s->frame_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, usb_frame, s);
    s->wake_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, usb_wake, s);
    memory_region_init_io(&s->iomem, obj, &rx113_usb_ops, s,
                          TYPE_RX113_USB, 0x200);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    for (unsigned i = 0; i < 4; i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq[i]);
    }
    qdev_init_gpio_in_named(DEVICE(obj), usb_vbus, "vbus", 1);
    qdev_init_gpio_in_named(DEVICE(obj), usb_line, "line", 2);
    qdev_init_gpio_in_named(DEVICE(obj), usb_overcurrent, "overcurrent", 2);
    qdev_init_gpio_in_named(DEVICE(obj), usb_id, "id", 1);
    qdev_init_gpio_in_named(DEVICE(obj), usb_pddet, "pddet", 1);
    object_property_add_str(obj, "sie-completion", NULL, usb_sie_completion);
    object_property_add_str(obj, "bus-output", usb_bus_output, NULL);
    object_property_add_str(obj, "sie-sof", NULL, usb_sie_sof);
    object_property_add_str(obj, "sie-setup", usb_sie_setup, NULL);
    object_property_add_str(obj, "sie-token", NULL, usb_sie_token);
    object_property_add_str(obj, "bus-event", NULL, usb_bus_event);
    object_property_add_str(obj, "sie-tx", usb_sie_tx, NULL);
}

static void rx113_usb_finalize(Object *obj)
{
    RX113USBState *s = RX113_USB(obj);

    timer_free(s->attach_timer);
    timer_free(s->frame_timer);
    timer_free(s->wake_timer);
}

static void rx113_usb_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->user_creatable = false;
    dc->vmsd = &vmstate_rx113_usb;
    rc->phases.hold = rx113_usb_reset_hold;
}

static const TypeInfo rx113_usb_type = {
    .name = TYPE_RX113_USB,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RX113USBState),
    .instance_init = rx113_usb_init,
    .instance_finalize = rx113_usb_finalize,
    .class_init = rx113_usb_class_init,
};

static void rx113_usb_register_types(void)
{
    type_register_static(&rx113_usb_type);
}

type_init(rx113_usb_register_types)
