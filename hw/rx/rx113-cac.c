/*
 * Renesas RX113 clock frequency accuracy measurement circuit
 * Hardware manual: R01UH0448EJ0120 Rev.1.20, section 10.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/host-utils.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "hw/rx/rx113_cac.h"
#include "migration/vmstate.h"

struct RX113CACState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq[3];
    QEMUTimer *timer;
    uint64_t hz[6];
    uint8_t cr0, cr1, cr2, icr, str;
    uint16_t upper, lower, buffer, counter;
    bool stopped, counting;
    bool raw, filtered, sample_level, reference_level;
    uint8_t samples;
    int64_t last_ns;
    /* Fractions in Hz * ns, denominators include the selected divider. */
    uint64_t count_phase, reference_phase, filter_phase;
};

static const unsigned count_div[] = { 1, 4, 8, 32 };
static const unsigned reference_div[] = { 32, 128, 1024, 8192 };
static const unsigned filter_div[] = { 0, 1, 4, 16 };

static void cac_irq(RX113CACState *s)
{
    for (unsigned i = 0; i < 3; i++) {
        qemu_set_irq(s->irq[i], (s->icr & s->str & (1 << i)) != 0);
    }
}

static uint64_t cac_source(RX113CACState *s, uint8_t cr)
{
    unsigned source = (cr >> 1) & 7;

    return source < ARRAY_SIZE(s->hz) ? s->hz[source] : 0;
}

static bool cac_external(RX113CACState *s)
{
    return !(s->cr2 & 1) && (s->cr1 & 1);
}

static void cac_edge(RX113CACState *s, bool rising)
{
    unsigned edges = s->cr1 >> 6;

    if (!(s->cr0 & 1) || edges == 3 ||
        (edges != 2 && rising != (edges == 0))) {
        return;
    }
    if (!s->counting) {
        s->counting = true;
        s->count_phase = 0;
    } else {
        s->buffer = s->counter;
        s->str |= 2;
        if (s->buffer < s->lower || s->buffer > s->upper) {
            s->str |= 1;
        }
        /* Figure 10.2: each capture starts a new measurement interval. */
        s->counter = 0;
        s->count_phase = 0;
    }
}

/* Keep fractional cycles without rounding the oscillator frequency. */
static uint64_t cac_ticks(uint64_t *phase, uint64_t dt, uint64_t hz,
                          unsigned divider)
{
    uint64_t denominator = NANOSECONDS_PER_SECOND * (uint64_t)divider;
    uint64_t lo, hi, old;

    mulu64(&lo, &hi, dt, hz);
    old = lo;
    lo += *phase;
    hi += lo < old;
    *phase = divu128(&lo, &hi, denominator);
    return lo;
}

static void cac_deadline(uint64_t *next, uint64_t phase, uint64_t hz,
                          uint64_t units)
{
    if (hz) {
        *next = MIN(*next, DIV_ROUND_UP(units - phase, hz));
    }
}

static void cac_sync(RX113CACState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t target = cac_source(s, s->cr1);
    unsigned dfs = s->cr2 >> 6;

    if (s->stopped) {
        s->last_ns = now;
        return;
    }
    /* Replay reference/filter events in order even if a callback is late. */
    while (s->last_ns < now) {
        uint64_t dt = now - s->last_ns;
        uint64_t ticks;

        if ((s->cr0 & s->cr2) & 1) {
            cac_deadline(&dt, s->reference_phase,
                         2 * cac_source(s, s->cr2),
                         NANOSECONDS_PER_SECOND *
                         (uint64_t)reference_div[(s->cr2 >> 4) & 3]);
        } else if (cac_external(s) && dfs &&
                   (s->raw != s->filtered || s->samples < 3)) {
            cac_deadline(&dt, s->filter_phase, target,
                         NANOSECONDS_PER_SECOND * (uint64_t)filter_div[dfs]);
        }
        if ((s->cr0 & 1) && s->counting) {
            ticks = cac_ticks(&s->count_phase, dt, target,
                              count_div[(s->cr1 >> 4) & 3]);
            if (ticks >= 65536U - s->counter) {
                s->str |= 4;
            }
            s->counter += ticks;
        }
        if ((s->cr0 & s->cr2) & 1) {
            ticks = cac_ticks(&s->reference_phase, dt,
                              2 * cac_source(s, s->cr2),
                              reference_div[(s->cr2 >> 4) & 3]);
            if (ticks) {
                s->reference_level = !s->reference_level;
                cac_edge(s, s->reference_level);
            }
        } else if (cac_external(s) && dfs) {
            ticks = cac_ticks(&s->filter_phase, dt, target, filter_div[dfs]);
            if (ticks) {
                if (s->sample_level != s->raw) {
                    s->sample_level = s->raw;
                    s->samples = 0;
                }
                s->samples = MIN(3, s->samples + MIN(ticks, 3));
                if (s->samples == 3 && s->filtered != s->raw) {
                    s->filtered = s->raw;
                    cac_edge(s, s->filtered);
                }
            }
        }
        s->last_ns += dt;
    }
    cac_irq(s);
}

static void cac_schedule(RX113CACState *s)
{
    uint64_t next = INT64_MAX;
    uint64_t target = cac_source(s, s->cr1);
    unsigned dfs = s->cr2 >> 6;

    timer_del(s->timer);
    if (s->stopped) {
        return;
    }
    if ((s->cr0 & 1) && s->counting && !(s->str & 4)) {
        cac_deadline(&next, s->count_phase, target,
                     (65536ULL - s->counter) * NANOSECONDS_PER_SECOND *
                     count_div[(s->cr1 >> 4) & 3]);
    }
    if ((s->cr0 & s->cr2) & 1) {
        cac_deadline(&next, s->reference_phase, 2 * cac_source(s, s->cr2),
                     NANOSECONDS_PER_SECOND *
                     (uint64_t)reference_div[(s->cr2 >> 4) & 3]);
    } else if (cac_external(s) && dfs &&
               (s->raw != s->filtered || s->samples < 3)) {
        cac_deadline(&next, s->filter_phase, target,
                     NANOSECONDS_PER_SECOND * (uint64_t)filter_div[dfs]);
    }
    if (next != INT64_MAX) {
        timer_mod(s->timer, s->last_ns + MAX(next, 1));
    }
}

static void cac_timer(void *opaque)
{
    RX113CACState *s = opaque;

    cac_sync(s);
    cac_schedule(s);
}

void rx113_cac_set_clocks(DeviceState *dev, const uint64_t hz[6], bool stopped)
{
    RX113CACState *s = RX113_CAC(dev);

    cac_sync(s);
    memcpy(s->hz, hz, sizeof(s->hz));
    s->stopped = stopped;
    cac_schedule(s);
}

static void cac_input(void *opaque, int n, int level)
{
    RX113CACState *s = opaque;
    bool changed = s->raw != !!level;

    cac_sync(s);
    s->raw = !!level;
    if (changed && !s->stopped && cac_external(s) &&
        !(s->cr2 >> 6)) {
        s->filtered = s->raw;
        cac_edge(s, s->raw);
        cac_irq(s);
    }
    cac_schedule(s);
}

static bool cac_access(hwaddr addr, unsigned size)
{
    return (addr <= 4 && size == 1) ||
           ((addr == 6 || addr == 8 || addr == 10) && size == 2);
}

static uint64_t cac_read(void *opaque, hwaddr addr, unsigned size)
{
    RX113CACState *s = opaque;

    if (s->stopped) {
        return 0;
    }
    cac_sync(s);
    cac_schedule(s);
    if (cac_access(addr, size)) {
        switch (addr) {
        case 0: return s->cr0;
        case 1: return s->cr1;
        case 2: return s->cr2;
        case 3: return s->icr;
        case 4: return s->str;
        case 6: return s->upper;
        case 8: return s->lower;
        case 10: return s->buffer;
        }
    }
    qemu_log_mask(LOG_GUEST_ERROR, "rx113-cac: invalid read at 0x%"
                  HWADDR_PRIx " size %u\n", addr, size);
    return 0;
}

static void cac_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    RX113CACState *s = opaque;

    if (s->stopped) {
        return;
    }
    cac_sync(s);
    if (!cac_access(addr, size)) {
        qemu_log_mask(LOG_GUEST_ERROR, "rx113-cac: invalid write at 0x%"
                      HWADDR_PRIx " size %u\n", addr, size);
    } else if ((s->cr0 & 1) &&
               (addr == 1 || addr == 2 || addr == 6 || addr == 8)) {
        /* D-009: an emulation convention, not specified silicon behavior. */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "rx113-cac: configuration write while CFME=1\n");
    } else {
        switch (addr) {
        case 0:
            if (!(value & 1) || !(s->cr0 & 1)) {
                s->counter = 0;
                s->counting = false;
                s->count_phase = 0;
                s->reference_phase = 0;
                s->reference_level = false;
            }
            /* D-009: CFME reflects immediately (no specified delay). */
            s->cr0 = value & 1;
            break;
        case 1:
        case 2:
            if (addr == 1) {
                s->cr1 = value;
            } else {
                s->cr2 = value;
            }
            /* A new filter clock establishes a new sampling phase. */
            s->filter_phase = 0;
            s->samples = 0;
            s->sample_level = s->raw;
            if (!(s->cr2 >> 6)) {
                s->filtered = s->raw;
            }
            break;
        case 3:
            s->icr = value & 7;
            s->str &= ~((value >> 4) & 7);
            break;
        case 6:
            s->upper = value;
            break;
        case 8:
            s->lower = value;
            break;
        }
    }
    cac_irq(s);
    cac_schedule(s);
}

static const MemoryRegionOps cac_ops = {
    .read = cac_read,
    .write = cac_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .valid.unaligned = true,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
    .impl.unaligned = true,
};

static void cac_reset(Object *obj, ResetType type)
{
    RX113CACState *s = RX113_CAC(obj);

    timer_del(s->timer);
    s->cr0 = s->cr1 = s->cr2 = s->icr = s->str = 0;
    s->upper = s->lower = s->buffer = s->counter = 0;
    s->counting = s->reference_level = false;
    s->filtered = s->sample_level = s->raw;
    s->samples = 0;
    s->count_phase = s->reference_phase = s->filter_phase = 0;
    s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    cac_irq(s);
}

static int cac_pre_save(void *opaque)
{
    cac_sync(opaque);
    cac_schedule(opaque);
    return 0;
}

static int cac_post_load(void *opaque, int version_id)
{
    cac_irq(opaque);
    cac_schedule(opaque);
    return 0;
}

static const VMStateDescription vmstate_cac = {
    .name = TYPE_RX113_CAC,
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = cac_pre_save,
    .post_load = cac_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64_ARRAY(hz, RX113CACState, 6),
        VMSTATE_UINT8(cr0, RX113CACState),
        VMSTATE_UINT8(cr1, RX113CACState),
        VMSTATE_UINT8(cr2, RX113CACState),
        VMSTATE_UINT8(icr, RX113CACState),
        VMSTATE_UINT8(str, RX113CACState),
        VMSTATE_UINT16(upper, RX113CACState),
        VMSTATE_UINT16(lower, RX113CACState),
        VMSTATE_UINT16(buffer, RX113CACState),
        VMSTATE_UINT16(counter, RX113CACState),
        VMSTATE_BOOL(stopped, RX113CACState),
        VMSTATE_BOOL(counting, RX113CACState),
        VMSTATE_BOOL(raw, RX113CACState),
        VMSTATE_BOOL(filtered, RX113CACState),
        VMSTATE_BOOL(sample_level, RX113CACState),
        VMSTATE_BOOL(reference_level, RX113CACState),
        VMSTATE_UINT8(samples, RX113CACState),
        VMSTATE_INT64(last_ns, RX113CACState),
        VMSTATE_UINT64(count_phase, RX113CACState),
        VMSTATE_UINT64(reference_phase, RX113CACState),
        VMSTATE_UINT64(filter_phase, RX113CACState),
        VMSTATE_END_OF_LIST()
    },
};

static void cac_init(Object *obj)
{
    RX113CACState *s = RX113_CAC(obj);

    s->stopped = true;
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, cac_timer, s);
    memory_region_init_io(&s->iomem, obj, &cac_ops, s, TYPE_RX113_CAC, 0x20);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    for (unsigned i = 0; i < 3; i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq[i]);
    }
    qdev_init_gpio_in_named(DEVICE(obj), cac_input, "cacref", 1);
}

static void cac_finalize(Object *obj)
{
    timer_free(RX113_CAC(obj)->timer);
}

static void cac_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->user_creatable = false;
    dc->vmsd = &vmstate_cac;
    rc->phases.hold = cac_reset;
}

static const TypeInfo cac_type = {
    .name = TYPE_RX113_CAC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RX113CACState),
    .instance_init = cac_init,
    .instance_finalize = cac_finalize,
    .class_init = cac_class_init,
};

static void cac_register_types(void)
{
    type_register_static(&cac_type);
}

type_init(cac_register_types)
