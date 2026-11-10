/*
 * Renesas RX113 realtime clock (RTCA)
 * Datasheet: R01UH0448EJ0120 Rev.1.20, section 24
 *
 * Models the calendar and binary count modes (24.1, 24.2.2 - 24.2.8), the
 * alarm comparison (24.2.9 - 24.2.16, 24.3.6) and the ALM, PRD and CUP
 * interrupts (24.4). The prescaler is derived from the virtual clock: one
 * 1/256 s step is 3906250 ns, so the 128 Hz reference clock, R64CNT and the
 * periodic interrupt periods (RCR1.PES) are exact.
 *
 * Register writes are applied immediately; count-source synchronization
 * delays are not modelled. RADJ changes the prescaler at sub-clock cycle
 * resolution (24.3.8). RTCOUT is observable through a read-only QOM property.
 * The CUP condition "carry to R64CNT while R64CNT is read" (24.4 (3)) is
 * not modelled; CUP is raised on the carry to the second counter/BCNT0.
 * Counters and reset-retained bits survive system reset (24.2); undefined
 * power-on values are initialized to zero.
 *
 * The ICU vectors are ALM = 92, PRD = 93 and CUP = 63 (Table 14.3). The PRD
 * is also delivered to the LCDC blinking input (34.4.2) through a second
 * output line.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "migration/vmstate.h"
#include "hw/core/qdev.h"
#include "hw/core/irq.h"
#include "hw/rx/rx113_rtc.h"

/* Register offsets from 0x0008c400, Table 5.1 and 24.2 */
#define R64CNT      0x00
#define RSECCNT     0x02    /* BCNT0 in binary count mode */
#define RMINCNT     0x04    /* BCNT1 */
#define RHRCNT      0x06    /* BCNT2 */
#define RWKCNT      0x08    /* BCNT3 */
#define RDAYCNT     0x0a
#define RMONCNT     0x0c
#define RYRCNT      0x0e
#define RSECAR      0x10    /* BCNT0AR */
#define RMINAR      0x12    /* BCNT1AR */
#define RHRAR       0x14    /* BCNT2AR */
#define RWKAR       0x16    /* BCNT3AR */
#define RDAYAR      0x18    /* BCNT0AER */
#define RMONAR      0x1a    /* BCNT1AER */
#define RYRAR       0x1c    /* BCNT2AER */
#define RYRAREN     0x1e    /* BCNT3AER */
#define RCR1        0x22
#define RCR2        0x24
#define RCR3        0x26
#define RADJ        0x2e
#define RTC_SIZE    0x100

/* 24.2.17 RCR1 */
#define RCR1_AIE        0x01
#define RCR1_CIE        0x02
#define RCR1_PIE        0x04
#define RCR1_RTCOS      0x08
#define RCR1_PES_SHIFT  4

/* 24.2.18 RCR2 */
#define RCR2_START      0x01
#define RCR2_RESET      0x02
#define RCR2_ADJ30      0x04
#define RCR2_RTCOE      0x08
#define RCR2_AADJE      0x10
#define RCR2_AADJP      0x20
#define RCR2_HR24       0x40
#define RCR2_CNTMD      0x80
/* Bits that hold a value; RESET and ADJ30 are self-clearing actions */
#define RCR2_STORED     (RCR2_START | RCR2_RTCOE | RCR2_AADJE | RCR2_AADJP | \
                         RCR2_HR24 | RCR2_CNTMD)

#define RCR3_RTCEN      0x01

#define HR_PM           0x40
#define ALARM_ENB       0x80

/* 24.1: the count source is divided into a 128 Hz reference clock */
#define TICK_NS         (NANOSECONDS_PER_SECOND / 256)
#define TICKS_PER_SEC   256
/* 1/128 ns units represent both nanoseconds and 32768-Hz cycles exactly. */
#define PHASE_SCALE     128
#define TICK_UNITS      (TICK_NS * PHASE_SCALE)
#define SUBCLK_UNITS    3906250LL

static unsigned bcd2bin(uint8_t v)
{
    return (v >> 4) * 10 + (v & 0xf);
}

static uint8_t bin2bcd(unsigned v)
{
    return ((v / 10) << 4) | (v % 10);
}

static bool rtc_binary_mode(RX113RTCState *s)
{
    return s->rcr2 & RCR2_CNTMD;
}

static bool rtc_subclock_running(RX113RTCState *s)
{
    /* 24.2.19: either SYSTEM or RTC can request sub-clock operation. */
    return s->system_subclock || (s->rcr3 & RCR3_RTCEN);
}

static bool rtc_running(RX113RTCState *s)
{
    return (s->rcr2 & RCR2_START) && rtc_subclock_running(s);
}

/* 24.2.17: PES = 0110b..1111b select 1/256 s..2 s; others disable PRD. */
static unsigned rtc_prd_period(RX113RTCState *s)
{
    unsigned pes = s->rcr1 >> RCR1_PES_SHIFT;

    if (!(s->rcr1 & RCR1_PIE) || pes < 6) {
        return 0;
    }
    return 1u << (pes - 6);
}

static unsigned days_in_month(unsigned mon, unsigned year)
{
    static const uint8_t dim[] = { 31, 28, 31, 30, 31, 30,
                                   31, 31, 30, 31, 30, 31 };

    if (mon < 1 || mon > 12) {
        return 31;
    }
    /* 24.2.6: years are 2000 to 2099, so divisibility by 4 is enough */
    if (mon == 2 && year % 4 == 0) {
        return 29;
    }
    return dim[mon - 1];
}

static void rtc_add_day(RX113RTCState *s)
{
    unsigned day = bcd2bin(s->day) + 1;
    unsigned mon = bcd2bin(s->mon);
    unsigned year = bcd2bin(s->year & 0xff);

    s->wday = (s->wday & 7) >= 6 ? 0 : (s->wday & 7) + 1;
    if (day > days_in_month(mon, year)) {
        day = 1;
        mon++;
        if (mon > 12) {
            mon = 1;
            year = (year + 1) % 100;
        }
    }
    s->day = bin2bcd(day);
    s->mon = bin2bcd(mon);
    s->year = bin2bcd(year);
}

static void rtc_add_hour(RX113RTCState *s)
{
    unsigned hour = bcd2bin(s->hour & 0x3f) + 1;

    if (s->rcr2 & RCR2_HR24) {
        if (hour >= 24) {
            s->hour = (s->hour & HR_PM);
            rtc_add_day(s);
        } else {
            s->hour = (s->hour & HR_PM) | bin2bcd(hour);
        }
        return;
    }
    /* 24.2.4: 12-hour mode counts 00 to 11 with the PM bit */
    if (hour >= 12) {
        bool was_pm = s->hour & HR_PM;

        s->hour = was_pm ? 0 : HR_PM;
        if (was_pm) {
            rtc_add_day(s);
        }
    } else {
        s->hour = (s->hour & HR_PM) | bin2bcd(hour);
    }
}

static void rtc_add_minute(RX113RTCState *s)
{
    unsigned min = bcd2bin(s->min) + 1;

    if (min >= 60) {
        s->min = 0;
        rtc_add_hour(s);
    } else {
        s->min = bin2bcd(min);
    }
}

static void rtc_add_second(RX113RTCState *s)
{
    unsigned sec = bcd2bin(s->sec) + 1;

    if (sec >= 60) {
        s->sec = 0;
        rtc_add_minute(s);
    } else {
        s->sec = bin2bcd(sec);
    }
}

static bool rtc_alarm_matches(RX113RTCState *s)
{
    bool any = false;

    if (rtc_binary_mode(s)) {
        /* 24.2.13 (2): compare the bits enabled by BCNTnAER */
        return s->bcntaer && !((s->bcnt ^ s->bcntar) & s->bcntaer);
    }

#define ALARM_CMP(ar, cnt, mask)                        \
    do {                                                \
        if ((ar) & ALARM_ENB) {                         \
            any = true;                                 \
            if (((ar) & (mask)) != ((cnt) & (mask))) {  \
                return false;                           \
            }                                           \
        }                                               \
    } while (0)

    ALARM_CMP(s->secar, s->sec, 0x7f);
    ALARM_CMP(s->minar, s->min, 0x7f);
    ALARM_CMP(s->hourar, s->hour,
              (s->rcr2 & RCR2_HR24) ? 0x3f : 0x7f);
    ALARM_CMP(s->wdayar, s->wday, 0x07);
    ALARM_CMP(s->dayar, s->day, 0x3f);
    ALARM_CMP(s->monar, s->mon, 0x1f);
    /* 24.2.16: RYRAR is compared when RYRAREN.ENB is set. */
    if (s->yeararen & ALARM_ENB) {
        any = true;
        if ((s->yearar & 0xff) != (s->year & 0xff)) {
            return false;
        }
    }
#undef ALARM_CMP
    return any;
}

/*
 * 24.4 (1): the flag is set on a match of the alarm registers and the
 * counters, and not again until the next fresh match.
 */
static void rtc_alarm_check(RX113RTCState *s)
{
    bool match = rtc_alarm_matches(s);

    if (match && !s->alarm_match && (s->rcr1 & RCR1_AIE)) {
        qemu_irq_pulse(s->alm);
    }
    s->alarm_match = match;
}

/* 24.2.20: PMADJ=01 adds cycles, PMADJ=10 subtracts cycles. */
static void rtc_adjust(RX113RTCState *s)
{
    int64_t units = (s->radj & 0x3f) * SUBCLK_UNITS;

    switch (s->radj >> 6) {
    case 1:
        s->phase_units += units;
        break;
    case 2:
        s->phase_units -= units;
        break;
    default:
        break;
    }
}

static bool rtc_adjustment_due(RX113RTCState *s)
{
    bool short_period = s->rcr2 & RCR2_AADJP;

    if (!(s->rcr2 & RCR2_AADJE)) {
        return false;
    }
    if (rtc_binary_mode(s)) {
        return !(s->bcnt & (short_period ? 7 : 31));
    }
    return bcd2bin(s->sec) % (short_period ? 10 : 60) == 0;
}

/* The prescaler overflows: the second counter or BCNT0 is incremented. */
static void rtc_carry_second(RX113RTCState *s)
{
    if (rtc_binary_mode(s)) {
        s->bcnt++;
    } else {
        rtc_add_second(s);
    }
    if (s->rcr1 & RCR1_CIE) {
        qemu_irq_pulse(s->cup);
    }
    rtc_alarm_check(s);
    if (rtc_adjustment_due(s)) {
        rtc_adjust(s);
    }
}

static void rtc_tick(RX113RTCState *s)
{
    unsigned period = rtc_prd_period(s);

    s->sub++;
    s->prd_phase = (s->prd_phase + 1) & 511;
    if (period && !(s->prd_phase & (period - 1))) {
        qemu_irq_pulse(s->prd);
        qemu_irq_pulse(s->prd_lcdc);
    }
    if (s->sub == TICKS_PER_SEC) {
        s->sub = 0;
        rtc_carry_second(s);
    }
}

static void rtc_advance(RX113RTCState *s, uint64_t ticks)
{
    while (ticks) {
        uint64_t left = TICKS_PER_SEC - s->sub;

        if (!rtc_prd_period(s) && ticks >= left) {
            /* No per-tick event is needed; jump to the next second. */
            s->prd_phase = (s->prd_phase + left) & 511;
            s->sub = 0;
            ticks -= left;
            rtc_carry_second(s);
        } else if (!rtc_prd_period(s)) {
            s->sub += ticks;
            s->prd_phase = (s->prd_phase + ticks) & 511;
            ticks = 0;
        } else {
            rtc_tick(s);
            ticks--;
        }
    }
}

static void rtc_update_timer(RX113RTCState *s)
{
    unsigned period = rtc_prd_period(s);
    int64_t ticks = -1;

    qemu_set_irq(s->rtc_enable, !!(s->rcr3 & RCR3_RTCEN));

    if (!rtc_running(s)) {
        timer_del(&s->timer);
        return;
    }
    if (period) {
        ticks = period - (s->prd_phase & (period - 1));
    }
    /* An automatic correction can move a later PRD deadline forward. */
    if ((s->rcr1 & (RCR1_AIE | RCR1_CIE)) ||
        (s->rcr2 & RCR2_AADJE)) {
        int64_t to_second = TICKS_PER_SEC - s->sub;

        if (ticks < 0 || to_second < ticks) {
            ticks = to_second;
        }
    }
    if (ticks < 0) {
        timer_del(&s->timer);
    } else {
        int64_t units = ticks * TICK_UNITS - s->phase_units;

        timer_mod_ns(&s->timer, s->last_ns +
                     DIV_ROUND_UP(units, PHASE_SCALE));
    }
}

/* Consume complete divider ticks, stopping at each adjustment boundary. */
static void rtc_consume_phase(RX113RTCState *s)
{
    while (s->phase_units >= TICK_UNITS) {
        unsigned ticks = MIN(s->phase_units / TICK_UNITS,
                             TICKS_PER_SEC - s->sub);

        s->phase_units -= ticks * TICK_UNITS;
        rtc_advance(s, ticks);
    }
}

/* Bring the counters up to the current virtual time. */
static void rtc_sync(RX113RTCState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t elapsed = now - s->last_ns;

    if (rtc_running(s)) {
        /* Bounded chunks avoid overflowing the scaled time accumulator. */
        while (elapsed > 0) {
            int64_t ns = MIN(elapsed, NANOSECONDS_PER_SECOND);

            s->phase_units += ns * PHASE_SCALE;
            rtc_consume_phase(s);
            elapsed -= ns;
        }
    }
    s->last_ns = now;
}

/* Fork-local observability, like the LCDC's displayed-state properties. */
static bool rtc_get_rtcout(Object *obj, Error **errp)
{
    RX113RTCState *s = RX113_RTC(obj);
    unsigned bit = (s->rcr1 & RCR1_RTCOS) ? 1 : 7;

    rtc_sync(s);
    return (s->rcr2 & RCR2_RTCOE) && (s->sub & (1u << bit));
}

static bool rtc_get_subclock_running(Object *obj, Error **errp)
{
    return rtc_subclock_running(RX113_RTC(obj));
}

static void rtc_set_system_subclock(void *opaque, int n, int level)
{
    RX113RTCState *s = opaque;

    /* Reset wiring must not emit interrupts while other devices reset. */
    if (!device_is_in_reset(DEVICE(s))) {
        rtc_sync(s);
    } else if (!rtc_running(s)) {
        s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    }
    s->system_subclock = level;
    if (rtc_running(s) && !device_is_in_reset(DEVICE(s))) {
        rtc_consume_phase(s);
    }
    rtc_update_timer(s);
}

static void rtc_timer_cb(void *opaque)
{
    RX113RTCState *s = opaque;

    rtc_sync(s);
    rtc_update_timer(s);
}

static uint8_t rtc_lane(uint32_t val, unsigned lane)
{
    return val >> (lane * 8);
}

static uint32_t rtc_set_lane(uint32_t old, unsigned lane, uint8_t val)
{
    return (old & ~(0xffu << (lane * 8))) | ((uint32_t)val << (lane * 8));
}

static uint64_t rtc_read(void *opaque, hwaddr addr, unsigned size)
{
    RX113RTCState *s = opaque;
    bool bin = rtc_binary_mode(s);

    rtc_sync(s);
    switch (addr) {
    case R64CNT:
        return (s->sub >> 1) & 0x7f;
    case RSECCNT:
        return bin ? rtc_lane(s->bcnt, 0) : s->sec;
    case RMINCNT:
        return bin ? rtc_lane(s->bcnt, 1) : s->min;
    case RHRCNT:
        return bin ? rtc_lane(s->bcnt, 2) : s->hour;
    case RWKCNT:
        return bin ? rtc_lane(s->bcnt, 3) : s->wday;
    case RDAYCNT:
        return bin ? 0 : s->day;
    case RMONCNT:
        return bin ? 0 : s->mon;
    case RYRCNT:
        return bin ? 0 : s->year & 0xff;
    case RSECAR:
        return bin ? rtc_lane(s->bcntar, 0) : s->secar;
    case RMINAR:
        return bin ? rtc_lane(s->bcntar, 1) : s->minar;
    case RHRAR:
        return bin ? rtc_lane(s->bcntar, 2) : s->hourar;
    case RWKAR:
        return bin ? rtc_lane(s->bcntar, 3) : s->wdayar;
    case RDAYAR:
        return bin ? rtc_lane(s->bcntaer, 0) : s->dayar;
    case RMONAR:
        return bin ? rtc_lane(s->bcntaer, 1) : s->monar;
    case RYRAR:
        return bin ? rtc_lane(s->bcntaer, 2) : s->yearar & 0xff;
    case RYRAREN:
        return bin ? rtc_lane(s->bcntaer, 3) : s->yeararen;
    case RCR1:
        return s->rcr1;
    case RCR2:
        return s->rcr2 & RCR2_STORED;
    case RCR3:
        return s->rcr3;
    case RADJ:
        return s->radj;
    default:
        /* Odd bytes of 16-bit registers and gaps read as 0. */
        return 0;
    }
}

/* 24.2.18 RESET: initialize the prescaler and the registers it targets. */
static void rtc_software_reset(RX113RTCState *s)
{
    s->sub = 0;
    s->prd_phase = 0;
    s->phase_units = 0;
    s->secar = s->minar = s->hourar = s->wdayar = 0;
    s->dayar = s->monar = s->yeararen = 0;
    s->yearar = 0;
    s->bcntar = 0;
    s->bcntaer = 0;
    s->radj = 0;
    s->rcr2 &= ~(RCR2_AADJE | RCR2_AADJP);
    s->alarm_match = false;
}

/* 24.2.18 ADJ30: round the second counter to the nearest minute. */
static void rtc_adjust_30(RX113RTCState *s)
{
    bool round_up = bcd2bin(s->sec) >= 30;

    s->sec = 0;
    s->sub = 0;
    s->prd_phase = 0;
    s->phase_units = 0;
    if (round_up) {
        rtc_add_minute(s);
    }
}

static void rtc_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    RX113RTCState *s = opaque;
    bool bin = rtc_binary_mode(s);
    uint8_t v = val;

    rtc_sync(s);
    switch (addr) {
    case RSECCNT:
        if (bin) {
            s->bcnt = rtc_set_lane(s->bcnt, 0, v);
        } else {
            s->sec = v;
        }
        break;
    case RMINCNT:
        if (bin) {
            s->bcnt = rtc_set_lane(s->bcnt, 1, v);
        } else {
            s->min = v;
        }
        break;
    case RHRCNT:
        if (bin) {
            s->bcnt = rtc_set_lane(s->bcnt, 2, v);
        } else {
            s->hour = v;
        }
        break;
    case RWKCNT:
        if (bin) {
            s->bcnt = rtc_set_lane(s->bcnt, 3, v);
        } else {
            s->wday = v;
        }
        break;
    case RDAYCNT:
        if (!bin) {
            s->day = v & 0x3f;
        }
        break;
    case RMONCNT:
        if (!bin) {
            s->mon = v & 0x1f;
        }
        break;
    case RYRCNT:
        if (!bin) {
            s->year = v;
        }
        break;
    case RSECAR:
        if (bin) {
            s->bcntar = rtc_set_lane(s->bcntar, 0, v);
        } else {
            s->secar = v;
        }
        rtc_alarm_check(s);
        break;
    case RMINAR:
        if (bin) {
            s->bcntar = rtc_set_lane(s->bcntar, 1, v);
        } else {
            s->minar = v;
        }
        rtc_alarm_check(s);
        break;
    case RHRAR:
        if (bin) {
            s->bcntar = rtc_set_lane(s->bcntar, 2, v);
        } else {
            s->hourar = v;
        }
        rtc_alarm_check(s);
        break;
    case RWKAR:
        if (bin) {
            s->bcntar = rtc_set_lane(s->bcntar, 3, v);
        } else {
            s->wdayar = v;
        }
        rtc_alarm_check(s);
        break;
    case RDAYAR:
        if (bin) {
            s->bcntaer = rtc_set_lane(s->bcntaer, 0, v);
        } else {
            s->dayar = v;
        }
        rtc_alarm_check(s);
        break;
    case RMONAR:
        if (bin) {
            s->bcntaer = rtc_set_lane(s->bcntaer, 1, v);
        } else {
            s->monar = v;
        }
        rtc_alarm_check(s);
        break;
    case RYRAR:
        if (bin) {
            s->bcntaer = rtc_set_lane(s->bcntaer, 2, v);
        } else {
            s->yearar = v;
        }
        rtc_alarm_check(s);
        break;
    case RYRAREN:
        if (bin) {
            s->bcntaer = rtc_set_lane(s->bcntaer, 3, v);
        } else {
            s->yeararen = v;
        }
        rtc_alarm_check(s);
        break;
    case RCR1:
        s->rcr1 = v;
        break;
    case RCR2:
        s->rcr2 = v & RCR2_STORED;
        if (v & RCR2_RESET) {
            rtc_software_reset(s);
        }
        if ((v & RCR2_ADJ30) && !(s->rcr2 & RCR2_CNTMD)) {
            rtc_adjust_30(s);
        }
        break;
    case RCR3:
        s->rcr3 = v & 0x0f;
        break;
    case RADJ:
        s->radj = v;
        if (!(s->rcr2 & RCR2_AADJE)) {
            rtc_adjust(s);
        }
        break;
    case R64CNT:
    default:
        /* Read-only R64CNT, odd bytes and gaps ignore writes. */
        break;
    }
    if (rtc_running(s)) {
        rtc_consume_phase(s);
    }
    rtc_update_timer(s);
}

static const MemoryRegionOps rtc_ops = {
    .read = rtc_read,
    .write = rtc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 2,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
};

static void rtc_reset_hold(Object *obj, ResetType type)
{
    RX113RTCState *s = RX113_RTC(obj);

    /*
     * 24.2: x bits are not initialized by a reset, and counting continues.
     * Only CIE, RTCOS (24.2.17) and RTCOE (24.2.18) are cleared here; the
     * action bits RESET/ADJ30 already read as zero. Keep last_ns and the
     * prescaler phases so that the next sync includes time across reset.
     */
    s->rcr1 &= ~(RCR1_CIE | RCR1_RTCOS);
    s->rcr2 &= ~RCR2_RTCOE;
    rtc_update_timer(s);
}

static void rtc_init(Object *obj)
{
    RX113RTCState *s = RX113_RTC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &rtc_ops, s, TYPE_RX113_RTC,
                          RTC_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->alm);
    sysbus_init_irq(sbd, &s->prd);
    sysbus_init_irq(sbd, &s->cup);
    sysbus_init_irq(sbd, &s->prd_lcdc);
    qdev_init_gpio_out_named(DEVICE(obj), &s->rtc_enable, "rtc-enable", 1);
    timer_init_ns(&s->timer, QEMU_CLOCK_VIRTUAL, rtc_timer_cb, s);
    /* SOSCCR.SOSTP resets to 0: SYSTEM initially requests the sub-clock. */
    s->system_subclock = true;
    qdev_init_gpio_in_named(DEVICE(obj), rtc_set_system_subclock,
                           "system-subclock", 1);
    object_property_add_bool(obj, "rtcout", rtc_get_rtcout, NULL);
    object_property_add_bool(obj, "subclock-running",
                             rtc_get_subclock_running, NULL);
    /* QOM zero-initializes the registers, including undefined power-on bits. */
    s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static int rtc_post_load(void *opaque, int version_id)
{
    RX113RTCState *s = opaque;

    if (version_id == 1) {
        s->system_subclock = true;
        /* Version 1's fractional tick remains in now - last_ns. */
        s->phase_units = 0;
    }
    rtc_update_timer(s);
    return 0;
}

static const VMStateDescription vmstate_rx113_rtc = {
    .name = TYPE_RX113_RTC,
    .version_id = 2,
    .minimum_version_id = 1,
    .post_load = rtc_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(sec, RX113RTCState),
        VMSTATE_UINT8(min, RX113RTCState),
        VMSTATE_UINT8(hour, RX113RTCState),
        VMSTATE_UINT8(wday, RX113RTCState),
        VMSTATE_UINT8(day, RX113RTCState),
        VMSTATE_UINT8(mon, RX113RTCState),
        VMSTATE_UINT16(year, RX113RTCState),
        VMSTATE_UINT32(bcnt, RX113RTCState),
        VMSTATE_UINT8(secar, RX113RTCState),
        VMSTATE_UINT8(minar, RX113RTCState),
        VMSTATE_UINT8(hourar, RX113RTCState),
        VMSTATE_UINT8(wdayar, RX113RTCState),
        VMSTATE_UINT8(dayar, RX113RTCState),
        VMSTATE_UINT8(monar, RX113RTCState),
        VMSTATE_UINT16(yearar, RX113RTCState),
        VMSTATE_UINT8(yeararen, RX113RTCState),
        VMSTATE_UINT32(bcntar, RX113RTCState),
        VMSTATE_UINT32(bcntaer, RX113RTCState),
        VMSTATE_UINT8(rcr1, RX113RTCState),
        VMSTATE_UINT8(rcr2, RX113RTCState),
        VMSTATE_UINT8(rcr3, RX113RTCState),
        VMSTATE_UINT8(radj, RX113RTCState),
        VMSTATE_UINT16(sub, RX113RTCState),
        VMSTATE_UINT16(prd_phase, RX113RTCState),
        VMSTATE_BOOL(alarm_match, RX113RTCState),
        VMSTATE_INT64(last_ns, RX113RTCState),
        VMSTATE_INT64_V(phase_units, RX113RTCState, 2),
        VMSTATE_BOOL_V(system_subclock, RX113RTCState, 2),
        VMSTATE_END_OF_LIST()
    },
};

static void rtc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->vmsd = &vmstate_rx113_rtc;
    dc->user_creatable = false;
    rc->phases.hold = rtc_reset_hold;
}

static const TypeInfo rx113_rtc_info = {
    .name = TYPE_RX113_RTC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RX113RTCState),
    .instance_init = rtc_init,
    .class_init = rtc_class_init,
};

static void rx113_rtc_register_types(void)
{
    type_register_static(&rx113_rtc_info);
}

type_init(rx113_rtc_register_types)
