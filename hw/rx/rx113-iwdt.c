/*
 * Renesas RX113 independent watchdog timer
 * Hardware manual: R01UH0448EJ0120 Rev.1.20, sections 6, 7, 9 and 26.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/host-utils.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/rx/rx113_iwdt.h"
#include "migration/vmstate.h"
#include "system/runstate.h"
#include "target/rx/cpu.h"

#define UNDFF 0x4000
#define REFEF 0x8000
#define IWDT_HZ 15000

struct RX113IWDTState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    MemoryRegion *flash;
    RXCPU *cpu;
    QEMUTimer *timer;
    qemu_irq wuni;
    uint16_t cr;
    uint16_t flags;
    uint16_t count;
    uint16_t config;
    uint8_t rr;
    uint8_t rcr;
    uint8_t cstpr;
    uint8_t locked;
    uint8_t pending;
    uint8_t rstsr2;
    uint8_t rstsr2_read;
    bool automatic;
    bool reset_output;
    bool sleep_stop;
    bool clock_started;
    bool active;
    bool reload;
    bool pending_error;
    bool lazy;
    bool watchdog_reset;
    bool wuni_level;
    int64_t epoch;
    uint64_t ticks;
};

static unsigned rx113_iwdt_divisor(RX113IWDTState *s)
{
    static const unsigned divisors[16] = {
        [0] = 1, [2] = 16, [3] = 32, [4] = 64, [5] = 256, [15] = 128,
    };

    return divisors[extract32(s->config, 4, 4)];
}

static unsigned rx113_iwdt_period(RX113IWDTState *s)
{
    static const unsigned periods[] = { 128, 512, 1024, 2048 };

    return periods[s->config & 3];
}

static bool rx113_iwdt_window(RX113IWDTState *s)
{
    unsigned period = rx113_iwdt_period(s);
    unsigned start = period * (extract32(s->config, 12, 2) + 1) / 4 - 1;
    unsigned end = period * (3 - extract32(s->config, 8, 2)) / 4;

    /* Figure 26.2: an end >= start is treated as an unspecified end. */
    if (end && end - 1 >= start) {
        end = 0;
    }
    return s->count <= start && s->count >= end;
}

/*
 * Repeated automatic errors are skipped arithmetically, so long clock
 * steps do not execute billions of watchdog callbacks. MMIO catches up
 * before exposing state or accepting a refresh. The skip is used even
 * with the WUNI output connected (Issue #58): the catch-up also
 * materializes the request level the skipped cycles would have
 * produced -- raised when a boundary lands exactly on the target
 * cycle, and collapsed into a single pulse when boundaries were left
 * behind -- so an edge consumer such as the ICU latches the same
 * flags it would have seen from per-cycle callbacks. A consumer that
 * clears its latch without touching the IWDT sees a repeated request
 * only at the next IWDT access; that delay is the price of the skip
 * and is recorded in the Issue #58 report. Sleep gating always uses
 * the count-edge timer.
 */
static void rx113_iwdt_catch_up(RX113IWDTState *s)
{
    uint64_t target, elapsed, until_error, phase;
    unsigned period = rx113_iwdt_period(s);
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    bool crossed, want_level;

    if (!s->lazy) {
        return;
    }
    target = muldiv64(now - s->epoch, IWDT_HZ, NANOSECONDS_PER_SECOND) /
             rx113_iwdt_divisor(s);
    elapsed = target - s->ticks;
    until_error = s->reload ? period + 1 : s->count + 1;
    crossed = elapsed >= until_error;
    if (crossed) {
        s->flags = UNDFF;
        phase = (elapsed - until_error) % (period + 1);
        s->reload = phase == 0;
        s->count = phase ? period - phase : 0;
    } else if (elapsed) {
        if (s->reload) {
            s->count = period - elapsed;
            s->reload = false;
        } else {
            s->count -= elapsed;
        }
    }
    s->ticks = target;

    /*
     * The request is a one-cycle pulse per error (26.3.1.2: the
     * counter reloads after counting one cycle). It reads as raised
     * only while the target is the boundary cycle itself; a pulse
     * materialized by an earlier catch-up has expired once time has
     * advanced past it.
     */
    want_level = crossed && phase == 0;
    if (want_level != s->wuni_level) {
        s->wuni_level = want_level;
        qemu_set_irq(s->wuni, want_level);
    } else if (crossed && !want_level) {
        /* Boundaries elapsed unobserved: deliver one collapsed pulse. */
        qemu_set_irq(s->wuni, 1);
        qemu_set_irq(s->wuni, 0);
    }
}

static void rx113_iwdt_schedule(RX113IWDTState *s)
{
    unsigned div = rx113_iwdt_divisor(s);

    s->lazy = s->automatic && !s->reset_output && !s->sleep_stop &&
              s->active && !s->pending && !s->wuni_level && s->flags;
    if (s->lazy) {
        timer_del(s->timer);
        return;
    }
    if (s->clock_started && (s->active || s->wuni_level) && div &&
        !s->watchdog_reset) {
        /* Absolute rational deadlines avoid accumulating 15 kHz rounding. */
        uint64_t ns = muldiv64_round_up((s->ticks + 1) * div,
                                       NANOSECONDS_PER_SECOND, IWDT_HZ);

        timer_mod(s->timer, s->epoch + ns);
    }
}

static void rx113_iwdt_event(RX113IWDTState *s, uint16_t flag)
{
    /* Section 26.3.4: each new event replaces the previous source. */
    s->flags = flag;
    s->pending = 0;
    s->count = 0;
    s->active = s->automatic;
    s->reload = s->automatic;
    if (s->reset_output) {
        s->rstsr2 |= 1;
        s->watchdog_reset = true;
        timer_del(s->timer);
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
    } else {
        s->wuni_level = true;
        qemu_set_irq(s->wuni, 1);
    }
}

static void rx113_iwdt_tick(void *opaque)
{
    RX113IWDTState *s = opaque;

    s->ticks++;
    if (s->wuni_level) {
        s->wuni_level = false;
        qemu_set_irq(s->wuni, 0);
    }
    if (!s->active) {
        return;
    }
    /* RX WAIT is the core's sleep indication; count gating is independent. */
    if (s->sleep_stop && qatomic_read(&CPU(s->cpu)->halted) &&
        qatomic_read(&s->cpu->env.in_sleep)) {
        rx113_iwdt_schedule(s);
        return;
    }
    if (s->pending && --s->pending == 0) {
        if (s->pending_error || (!s->reload && !rx113_iwdt_window(s))) {
            rx113_iwdt_event(s, REFEF);
        } else {
            s->count = rx113_iwdt_period(s) - 1;
            s->reload = false;
        }
    } else if (s->reload) {
        if (!s->pending) {
            s->count = rx113_iwdt_period(s) - 1;
            s->reload = false;
        }
    } else if (s->count == 0) {
        rx113_iwdt_event(s, UNDFF);
    } else {
        s->count--;
    }
    rx113_iwdt_schedule(s);
}

static void rx113_iwdt_refresh(RX113IWDTState *s)
{
    s->locked = 7;
    if (!s->automatic) {
        s->config = s->cr;
        s->reset_output = s->rcr & 0x80;
        s->sleep_stop = s->cstpr & 0x80;
    }
    if (!rx113_iwdt_divisor(s)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "rx113-iwdt: prohibited clock divider\n");
        return;
    }
    if (!s->active) {
        s->active = true;
        s->reload = true; /* first refresh/restart bypasses the window */
        s->epoch = MAX(s->epoch, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        s->ticks = 0;
    }
    /* Section 26.3.3: model the maximum four count-cycle synchronization. */
    if (!s->pending) {
        s->pending_error = !s->reload && !rx113_iwdt_window(s);
        s->pending = 4;
    }
    rx113_iwdt_schedule(s);
}

static uint64_t rx113_iwdt_read(void *opaque, hwaddr addr, unsigned size)
{
    RX113IWDTState *s = opaque;

    rx113_iwdt_catch_up(s);
    switch (addr) {
    case 0:
        if (size == 1) {
            return s->rr;
        }
        break;
    case 2:
        if (size == 2) {
            return s->cr;
        }
        break;
    case 4:
        if (size == 2) {
            return s->flags | s->count;
        }
        break;
    case 6:
        if (size == 1) {
            return s->rcr;
        }
        break;
    case 8:
        if (size == 1) {
            return s->cstpr;
        }
        break;
    }
    qemu_log_mask(LOG_GUEST_ERROR, "rx113-iwdt: invalid read at 0x%"
                  HWADDR_PRIx " size %u\n", addr, size);
    return 0;
}

static void rx113_iwdt_write(void *opaque, hwaddr addr, uint64_t value,
                             unsigned size)
{
    RX113IWDTState *s = opaque;

    rx113_iwdt_catch_up(s);
    switch (addr) {
    case 0:
        if (size == 1) {
            bool refresh = s->rr == 0 && value == 0xff;

            s->rr = value == 0 ? 0 : 0xff;
            if (refresh) {
                rx113_iwdt_refresh(s);
            }
            return;
        }
        break;
    case 2:
        if (size == 2) {
            if (!s->automatic && !(s->locked & 1)) {
                s->cr = value & 0x33f3;
                s->locked |= 1;
            }
            return;
        }
        break;
    case 4:
        if (size == 2) {
            s->flags &= value;
            return;
        }
        break;
    case 6:
    case 8:
        if (size == 1) {
            unsigned lock = addr == 6 ? 2 : 4;

            if (!s->automatic && !(s->locked & lock)) {
                if (addr == 6) {
                    s->rcr = value & 0x80;
                } else {
                    s->cstpr = value & 0x80;
                }
                s->locked |= lock;
            }
            return;
        }
        break;
    }
    qemu_log_mask(LOG_GUEST_ERROR, "rx113-iwdt: invalid write at 0x%"
                  HWADDR_PRIx " size %u\n", addr, size);
}

static const MemoryRegionOps rx113_iwdt_ops = {
    .read = rx113_iwdt_read,
    .write = rx113_iwdt_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

uint64_t rx113_iwdt_clock_hz(DeviceState *dev)
{
    return RX113_IWDT(dev)->clock_started ? IWDT_HZ : 0;
}

uint8_t rx113_iwdt_ilococr(DeviceState *dev)
{
    RX113IWDTState *s = RX113_IWDT(dev);

    /* In auto-start mode ILOCOCR is ineffective and retains reset readback. */
    return s->automatic || !s->clock_started;
}

void rx113_iwdt_start_clock(DeviceState *dev)
{
    RX113IWDTState *s = RX113_IWDT(dev);

    if (!s->automatic && !s->clock_started) {
        s->clock_started = true;
        /* Table 43.22: maximum tILOCO is 50 us. */
        s->epoch = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 50000;
        s->ticks = 0;
        rx113_iwdt_schedule(s);
    }
}

uint8_t rx113_iwdt_reset_status(DeviceState *dev)
{
    RX113IWDTState *s = RX113_IWDT(dev);

    s->rstsr2_read = s->rstsr2;
    return s->rstsr2;
}

void rx113_iwdt_clear_reset_status(DeviceState *dev, uint8_t value)
{
    RX113IWDTState *s = RX113_IWDT(dev);

    s->rstsr2 &= value | ~s->rstsr2_read;
    s->rstsr2_read = 0;
}

static void rx113_iwdt_reset_hold(Object *obj, ResetType type)
{
    RX113IWDTState *s = RX113_IWDT(obj);

    timer_del(s->timer);
    /* Table 6.2: watchdog reset retains IWDT registers and write protection. */
    if (!s->watchdog_reset) {
        s->cr = 0x33f3;
        s->rcr = s->cstpr = 0x80;
        s->rr = 0xff;
        s->flags = s->count = 0;
        s->locked = 0;
        s->rstsr2 = s->rstsr2_read = 0;
        s->clock_started = false;
    }
    s->count = 0;
    s->pending = 0;
    s->pending_error = false;
    s->lazy = false;
    s->active = s->reload = false;
    s->wuni_level = false;
    qemu_set_irq(s->wuni, 0);
}

static void rx113_iwdt_reset_exit(Object *obj, ResetType type)
{
    RX113IWDTState *s = RX113_IWDT(obj);
    const uint8_t *flash = memory_region_get_ram_ptr(s->flash);
    uint32_t ofs = ldl_le_p(flash + memory_region_size(s->flash) - 0x74);

    /* ROM restoration runs in reset hold, so option bytes are now loaded. */
    s->automatic = !(ofs & 2);
    if (s->automatic) {
        s->config = ((ofs >> 2) & 3) | (ofs & 0x3f0) |
                    ((ofs << 2) & 0x3000);
        s->reset_output = ofs & 0x1000;
        s->sleep_stop = ofs & 0x4000;
        s->clock_started = true;
        s->active = true;
        s->count = rx113_iwdt_period(s) - 1;
    }
    s->watchdog_reset = false;
    s->epoch = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->ticks = 0;
    rx113_iwdt_schedule(s);
}

static int rx113_iwdt_post_load(void *opaque, int version_id)
{
    RX113IWDTState *s = opaque;

    qemu_set_irq(s->wuni, s->wuni_level);
    return 0;
}

static const VMStateDescription vmstate_rx113_iwdt = {
    .name = TYPE_RX113_IWDT,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = rx113_iwdt_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16(cr, RX113IWDTState),
        VMSTATE_UINT16(flags, RX113IWDTState),
        VMSTATE_UINT16(count, RX113IWDTState),
        VMSTATE_UINT16(config, RX113IWDTState),
        VMSTATE_UINT8(rr, RX113IWDTState),
        VMSTATE_UINT8(rcr, RX113IWDTState),
        VMSTATE_UINT8(cstpr, RX113IWDTState),
        VMSTATE_UINT8(locked, RX113IWDTState),
        VMSTATE_UINT8(pending, RX113IWDTState),
        VMSTATE_UINT8(rstsr2, RX113IWDTState),
        VMSTATE_UINT8(rstsr2_read, RX113IWDTState),
        VMSTATE_BOOL(automatic, RX113IWDTState),
        VMSTATE_BOOL(reset_output, RX113IWDTState),
        VMSTATE_BOOL(sleep_stop, RX113IWDTState),
        VMSTATE_BOOL(clock_started, RX113IWDTState),
        VMSTATE_BOOL(active, RX113IWDTState),
        VMSTATE_BOOL(reload, RX113IWDTState),
        VMSTATE_BOOL(pending_error, RX113IWDTState),
        VMSTATE_BOOL(lazy, RX113IWDTState),
        VMSTATE_BOOL(watchdog_reset, RX113IWDTState),
        VMSTATE_BOOL(wuni_level, RX113IWDTState),
        VMSTATE_INT64(epoch, RX113IWDTState),
        VMSTATE_UINT64(ticks, RX113IWDTState),
        VMSTATE_TIMER_PTR(timer, RX113IWDTState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property rx113_iwdt_properties[] = {
    DEFINE_PROP_LINK("flash", RX113IWDTState, flash, TYPE_MEMORY_REGION,
                     MemoryRegion *),
    DEFINE_PROP_LINK("cpu", RX113IWDTState, cpu, TYPE_RX_CPU, RXCPU *),
};

static void rx113_iwdt_init(Object *obj)
{
    RX113IWDTState *s = RX113_IWDT(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &rx113_iwdt_ops, s,
                          TYPE_RX113_IWDT, 0x10);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->wuni);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, rx113_iwdt_tick, s);
}

static void rx113_iwdt_finalize(Object *obj)
{
    timer_free(RX113_IWDT(obj)->timer);
}

static void rx113_iwdt_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->user_creatable = false;
    dc->vmsd = &vmstate_rx113_iwdt;
    device_class_set_props(dc, rx113_iwdt_properties);
    rc->phases.hold = rx113_iwdt_reset_hold;
    rc->phases.exit = rx113_iwdt_reset_exit;
}

static const TypeInfo rx113_iwdt_type = {
    .name = TYPE_RX113_IWDT,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RX113IWDTState),
    .instance_init = rx113_iwdt_init,
    .instance_finalize = rx113_iwdt_finalize,
    .class_init = rx113_iwdt_class_init,
};

static void rx113_iwdt_register_types(void)
{
    type_register_static(&rx113_iwdt_type);
}

type_init(rx113_iwdt_register_types)
