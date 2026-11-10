/*
 * Renesas RX113 capacitive touch sensing unit
 * R01UH0448EJ0120 Rev.1.20, section 35; fork decisions D-010 and D-011.
 * ICO inputs are adjusted frequencies, not a physical capacitance model.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/visitor.h"
#include "qemu/host-utils.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/rx/rx113_ctsu.h"
#include "hw/rx/rx113_port.h"
#include "migration/vmstate.h"

enum {
    CR0, CR1, SDPRS, SST, MCH0, MCH1, CHAC0, CHAC1,
    CHTRC0 = 0x0b, CHTRC1, DCLKC = 0x10, ST,
    SSC, SO0 = 0x14, SO1 = 0x16, SC = 0x18, RC = 0x1a, ERRS = 0x1c,
};
enum { STRT = 1, CAP = 2, INIT = 0x10, DTSR = 0x10, PS = 0x80 };

struct RX113CTSUState {
    SysBusDevice parent_obj;
    MemoryRegion iomem, trim_mem;
    qemu_irq irq[3];
    QEMUTimer *timer;
    RX113PortState *port;
    uint8_t regs[0x1e];
    uint8_t trim, state, overflow_read;
    int8_t rx, tx;
    bool second, stopped, trigger;
    uint64_t hz, ref_hz, sensor_hz[12];
    bool sensor_set[12];
    /* Remaining eighths of PCLKB cycles * 1e9, including fractions. */
    uint64_t remaining;
    uint64_t sensor_count, ref_count, sensor_phase, ref_phase;
    int64_t last_ns;
};

static void ctsu_pins(RX113CTSUState *s)
{
    rx113_port_set_ctsu(s->port, s->regs[ERRS] & 4, s->regs[CR0] & 8);
}

static unsigned ctsu_mode(RX113CTSUState *s)
{
    return s->regs[CR1] >> 6;
}

static unsigned ctsu_opdiv(RX113CTSUState *s)
{
    return 1U << ((s->regs[CR1] >> 4) & 3);
}

static unsigned ctsu_basediv(RX113CTSUState *s)
{
    return ctsu_opdiv(s) * 2 * (((lduw_le_p(s->regs + SO1) >> 8) & 31) + 1);
}

static void ctsu_state(RX113CTSUState *s, unsigned state)
{
    s->state = state;
    s->regs[ST] = (s->regs[ST] & 0xf0) | state;
}

static void ctsu_wait(RX113CTSUState *s, uint64_t cycles8)
{
    s->remaining = cycles8 * NANOSECONDS_PER_SECOND;
}

static void ctsu_stabilize(RX113CTSUState *s)
{
    ctsu_state(s, 3);
    /* D-011: SST * 34/16 base cycles (34 at the required SST=0x10). */
    ctsu_wait(s, (uint64_t)s->regs[SST] * 17 * ctsu_basediv(s));
}

static int ctsu_next(uint16_t mask, int after)
{
    for (int ch = after + 1; ch < 12; ch++) {
        if (mask & (1 << ch)) {
            return ch;
        }
    }
    return -1;
}

/* Figure 35.11: state 1 is combinational channel selection. */
static void ctsu_select(RX113CTSUState *s, bool first)
{
    uint16_t enabled = lduw_le_p(s->regs + CHAC0) & 0xfff;
    uint16_t transmit = lduw_le_p(s->regs + CHTRC0) & enabled;
    unsigned mode = ctsu_mode(s);
    bool found = false;

    ctsu_state(s, 1);
    if (mode == 0) {
        s->rx = s->regs[MCH0];
        found = first && s->rx < 12 && (enabled & (1 << s->rx));
    } else if (mode == 1) {
        s->rx = ctsu_next(enabled, first ? -1 : s->rx);
        found = s->rx >= 0;
    } else if (mode == 3) {
        if (!first && !s->second) {
            s->second = true;
            s->regs[ST] |= PS;
            ctsu_stabilize(s);
            return;
        }
        s->second = false;
        s->regs[ST] &= ~PS;
        s->tx = ctsu_next(transmit, first ? -1 : s->tx);
        if (first || s->tx < 0) {
            s->rx = ctsu_next(enabled & ~transmit, first ? -1 : s->rx);
            s->tx = ctsu_next(transmit, -1);
        }
        found = s->rx >= 0 && s->tx >= 0;
    }
    if (!found) {
        ctsu_state(s, 0);
        if (mode != 0) {
            s->regs[MCH0] = 15;
        }
        s->regs[MCH1] = 15;
        if (!(s->regs[CR0] & CAP)) {
            s->regs[CR0] &= ~STRT;
        }
        /* §35.4.2-3: inhibit premature software restart/external event. */
        ctsu_wait(s, (s->regs[CR0] & CAP) ? 8 * ctsu_opdiv(s) :
                     ctsu_opdiv(s) == 4 ? 24 : 0);
        qemu_irq_pulse(s->irq[2]);
        return;
    }
    s->regs[MCH0] = s->rx;
    s->regs[MCH1] = mode == 3 ? s->tx : 15;
    ctsu_state(s, 2);
    qemu_irq_pulse(s->irq[0]);
}

static void ctsu_measure(RX113CTSUState *s)
{
    static const unsigned pulses[] = { 510, 126, 62, 0 };
    unsigned n = pulses[(s->regs[SDPRS] >> 4) & 3];
    unsigned repeat = (lduw_le_p(s->regs + SO0) >> 10) + 1;
    uint64_t quarters = 4 * n * ((s->regs[SDPRS] & 15) + 1) + n - 2;

    ctsu_state(s, 4);
    stw_le_p(s->regs + SC, 0);
    stw_le_p(s->regs + RC, 0);
    s->sensor_count = s->ref_count = 0;
    s->sensor_phase = s->ref_phase = 0;
    ctsu_wait(s, quarters * 2 * repeat * ctsu_basediv(s));
}

static void ctsu_count(uint64_t *count, uint64_t *phase,
                       uint64_t dt, uint64_t hz)
{
    uint64_t lo, hi, old;

    mulu64(&lo, &hi, dt, hz);
    old = lo;
    lo += *phase;
    hi += lo < old;
    if (hi >= NANOSECONDS_PER_SECOND) {
        *count = 65536;
        *phase = 0;
        return;
    }
    *phase = divu128(&lo, &hi, NANOSECONDS_PER_SECOND);
    *count = MIN(65536, *count + MIN(lo, 65536));
}

static void ctsu_sync(RX113CTSUState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (s->stopped || !s->hz) {
        s->last_ns = now;
        return;
    }
    for (;;) {
        uint64_t dt;

        if (s->state == 3 && !s->remaining && !(s->regs[ST] & DTSR)) {
            ctsu_measure(s);
        }
        if (!s->remaining || s->last_ns >= now) {
            break;
        }
        dt = MIN((uint64_t)(now - s->last_ns),
                 DIV_ROUND_UP(s->remaining, 8 * s->hz));
        if (s->state == 4) {
            uint64_t sensor = s->sensor_hz[s->rx];
            uint64_t old_sensor = s->sensor_count, old_ref = s->ref_count;

            if (!s->sensor_set[s->rx]) {
                sensor = s->ref_hz;
            }
            ctsu_count(&s->sensor_count, &s->sensor_phase, dt, sensor);
            ctsu_count(&s->ref_count, &s->ref_phase, dt, s->ref_hz);
            if (old_sensor <= 65535 && s->sensor_count > 65535) {
                s->regs[ST] |= 0x20;
            }
            if (old_ref <= 65535 && s->ref_count > 65535) {
                s->regs[ST] |= 0x40;
            }
        }
        s->last_ns += dt;
        s->remaining -= MIN(s->remaining, dt * 8 * s->hz);
        if (!s->remaining) {
            if (s->state == 4) {
                stw_le_p(s->regs + SC, MIN(s->sensor_count, 65535));
                stw_le_p(s->regs + RC, MIN(s->ref_count, 65535));
                s->regs[ST] |= DTSR;
                ctsu_state(s, 5);
                ctsu_wait(s, 16 * ctsu_opdiv(s));
            } else if (s->state == 5) {
                ctsu_state(s, 1);
                /* CTSUPS changes at 5->1, before the read request. */
                if (ctsu_mode(s) == 3) {
                    s->regs[ST] ^= PS;
                }
                qemu_irq_pulse(s->irq[1]);
                ctsu_select(s, false);
            }
        }
    }
    /* States 2/3 can wait indefinitely for software or DTC. */
    s->last_ns = now;
}

static void ctsu_schedule(RX113CTSUState *s)
{
    timer_del(s->timer);
    if (!s->stopped && s->hz && s->remaining) {
        timer_mod(s->timer, s->last_ns +
                  DIV_ROUND_UP(s->remaining, 8 * s->hz));
    }
}

static void ctsu_timer(void *opaque)
{
    ctsu_sync(opaque);
    ctsu_schedule(opaque);
}

void rx113_ctsu_set_clock(DeviceState *dev, uint64_t hz, bool stopped)
{
    RX113CTSUState *s;

    if (!dev) {
        return;
    }
    s = RX113_CTSU(dev);
    ctsu_sync(s);
    s->hz = hz;
    s->stopped = stopped;
    ctsu_schedule(s);
}

static bool ctsu_access(hwaddr addr, unsigned size)
{
    return (size == 1 && (addr <= CHAC1 || addr == CHTRC0 ||
                         addr == CHTRC1 || addr == DCLKC || addr == ST)) ||
           (size == 2 && addr >= SSC && addr <= ERRS && !(addr & 1));
}

static uint64_t ctsu_read(void *opaque, hwaddr addr, unsigned size)
{
    RX113CTSUState *s = opaque;
    uint64_t value = 0;

    if (s->stopped) {
        return 0;
    }
    ctsu_sync(s);
    if (!ctsu_access(addr, size)) {
        qemu_log_mask(LOG_GUEST_ERROR, "rx113-ctsu: invalid read at 0x%"
                      HWADDR_PRIx " size %u\n", addr, size);
    } else {
        value = size == 1 ? s->regs[addr] : lduw_le_p(s->regs + addr);
        if (addr == ST) {
            s->overflow_read |= value & 0x60;
        } else if (addr == RC) {
            s->regs[ST] &= ~DTSR;
            ctsu_sync(s);
        }
    }
    ctsu_schedule(s);
    return value;
}

static void ctsu_initialize(RX113CTSUState *s)
{
    timer_del(s->timer);
    s->remaining = 0;
    s->state = s->regs[ST] = s->overflow_read = 0;
    s->regs[MCH0] = s->regs[MCH1] = 15;
    stw_le_p(s->regs + SC, 0);
    stw_le_p(s->regs + RC, 0);
    s->rx = s->tx = -1;
    s->second = false;
    s->sensor_count = s->ref_count = s->sensor_phase = s->ref_phase = 0;
}

static void ctsu_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    RX113CTSUState *s = opaque;
    static const uint8_t masks[0x12] = {
        [CR1] = 0xff, [SDPRS] = 0x7f, [SST] = 0xff,
        [CHAC0] = 0xff, [CHAC1] = 0x0f,
        [CHTRC0] = 0xff, [CHTRC1] = 0x0f, [DCLKC] = 0x33,
    };

    if (s->stopped) {
        return;
    }
    ctsu_sync(s);
    if (!ctsu_access(addr, size)) {
        qemu_log_mask(LOG_GUEST_ERROR, "rx113-ctsu: invalid write at 0x%"
                      HWADDR_PRIx " size %u\n", addr, size);
    } else if (addr == CR0) {
        bool start = !(s->regs[CR0] & STRT) && (value & STRT);
        bool busy = s->regs[CR0] & STRT;

        if ((value & (INIT | STRT)) == INIT) {
            ctsu_initialize(s);
            s->regs[CR0] = value & 0x0e;
        } else {
            /* Repeated STRT writes cannot restart or stop an active scan. */
            s->regs[CR0] = (value & 0x08) |
                          (busy ? s->regs[CR0] & 7 : value & 7);
            if (start && !s->remaining && !(value & CAP)) {
                ctsu_select(s, true);
            } else if (start && s->remaining) {
                s->regs[CR0] &= ~STRT;
            }
        }
    } else if (addr == ST) {
        s->regs[ST] &= ~(s->overflow_read & ~value & 0x60);
        s->overflow_read &= s->regs[ST] & 0x60;
    } else if (addr == SSC || addr == SO0 || addr == SO1 || addr == ERRS) {
        unsigned mask = addr == SSC ? 0x0f00 : addr == SO0 ? 0xffff :
                        addr == SO1 ? 0x7fff : 0x008f;

        stw_le_p(s->regs + addr, value & mask);
        if (addr == SO1 && s->state == 2) {
            ctsu_stabilize(s);
            ctsu_sync(s);
        }
    } else if (addr == MCH0) {
        if (ctsu_mode(s) == 0 && !(s->regs[CR0] & STRT)) {
            s->regs[MCH0] = value & 15;
        }
    } else if (addr < ST && addr != MCH1) {
        s->regs[addr] = value & masks[addr];
    }
    ctsu_pins(s);
    ctsu_schedule(s);
}

static uint64_t ctsu_trim_read(void *opaque, hwaddr addr, unsigned size)
{
    RX113CTSUState *s = opaque;

    return !s->stopped && size == 1 ? s->trim : 0;
}

static void ctsu_trim_write(void *opaque, hwaddr addr, uint64_t value,
                            unsigned size)
{
    RX113CTSUState *s = opaque;

    if (!s->stopped && size == 1 && (s->regs[ERRS] & 3) == 2) {
        s->trim = value;
    }
}

static const MemoryRegionOps ctsu_ops = {
    .read = ctsu_read,
    .write = ctsu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .valid.unaligned = true,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
    .impl.unaligned = true,
};

static const MemoryRegionOps ctsu_trim_ops = {
    .read = ctsu_trim_read,
    .write = ctsu_trim_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

static void ctsu_trigger(void *opaque, int n, int level)
{
    RX113CTSUState *s = opaque;
    bool rising = level && !s->trigger;

    ctsu_sync(s);
    s->trigger = !!level;
    if (rising && !s->stopped && s->hz && s->state == 0 &&
        !s->remaining && (s->regs[CR0] & 3) == 3) {
        ctsu_select(s, true);
    }
    ctsu_schedule(s);
}

static void ctsu_reset(Object *obj, ResetType type)
{
    RX113CTSUState *s = RX113_CTSU(obj);

    memset(s->regs, 0, sizeof(s->regs));
    ctsu_initialize(s);
    s->trim = 0; /* D-011 factory fixture. */
    ctsu_pins(s);
    s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static int ctsu_pre_save(void *opaque)
{
    ctsu_sync(opaque);
    ctsu_schedule(opaque);
    return 0;
}

static int ctsu_post_load(void *opaque, int version_id)
{
    RX113CTSUState *s = opaque;

    if (s->state > 5 || s->rx < -1 || s->rx > 15 ||
        s->tx < -1 || s->tx > 11 ||
        (s->state >= 2 && (s->rx < 0 || s->rx >= 12))) {
        return -EINVAL;
    }
    ctsu_pins(s);
    ctsu_schedule(s);
    return 0;
}

static const VMStateDescription vmstate_ctsu = {
    .name = TYPE_RX113_CTSU,
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = ctsu_pre_save,
    .post_load = ctsu_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, RX113CTSUState, 0x1e),
        VMSTATE_UINT8(trim, RX113CTSUState),
        VMSTATE_UINT8(state, RX113CTSUState),
        VMSTATE_UINT8(overflow_read, RX113CTSUState),
        VMSTATE_INT8(rx, RX113CTSUState),
        VMSTATE_INT8(tx, RX113CTSUState),
        VMSTATE_BOOL(second, RX113CTSUState),
        VMSTATE_BOOL(stopped, RX113CTSUState),
        VMSTATE_BOOL(trigger, RX113CTSUState),
        VMSTATE_UINT64(hz, RX113CTSUState),
        VMSTATE_UINT64(ref_hz, RX113CTSUState),
        VMSTATE_UINT64_ARRAY(sensor_hz, RX113CTSUState, 12),
        VMSTATE_BOOL_ARRAY(sensor_set, RX113CTSUState, 12),
        VMSTATE_UINT64(remaining, RX113CTSUState),
        VMSTATE_UINT64(sensor_count, RX113CTSUState),
        VMSTATE_UINT64(ref_count, RX113CTSUState),
        VMSTATE_UINT64(sensor_phase, RX113CTSUState),
        VMSTATE_UINT64(ref_phase, RX113CTSUState),
        VMSTATE_INT64(last_ns, RX113CTSUState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property ctsu_properties[] = {
    DEFINE_PROP_LINK("port", RX113CTSUState, port, TYPE_RX113_PORT,
                     RX113PortState *),
    DEFINE_PROP_UINT64("ref-ico-hz", RX113CTSUState, ref_hz, 1000000),
};

static void ctsu_sensor_get(Object *obj, Visitor *v, const char *name,
                            void *opaque, Error **errp)
{
    RX113CTSUState *s = RX113_CTSU(obj);
    unsigned ch = GPOINTER_TO_UINT(opaque);
    uint64_t value = s->sensor_set[ch] ? s->sensor_hz[ch] : s->ref_hz;

    visit_type_uint64(v, name, &value, errp);
}

static void ctsu_sensor_set(Object *obj, Visitor *v, const char *name,
                            void *opaque, Error **errp)
{
    RX113CTSUState *s = RX113_CTSU(obj);
    unsigned ch = GPOINTER_TO_UINT(opaque);
    uint64_t value;

    if (visit_type_uint64(v, name, &value, errp)) {
        if (s->timer) {
            ctsu_sync(s);
        }
        s->sensor_hz[ch] = value;
        s->sensor_set[ch] = true;
        if (s->timer) {
            ctsu_schedule(s);
        }
    }
}

static void ctsu_init(Object *obj)
{
    RX113CTSUState *s = RX113_CTSU(obj);

    for (unsigned ch = 0; ch < 12; ch++) {
        g_autofree char *name = g_strdup_printf("ts%02u-ico-hz", ch);

        object_property_add(obj, name, "uint64", ctsu_sensor_get,
                            ctsu_sensor_set, NULL, GUINT_TO_POINTER(ch));
    }
    s->stopped = true;
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ctsu_timer, s);
    memory_region_init_io(&s->iomem, obj, &ctsu_ops, s, TYPE_RX113_CTSU, 0x100);
    memory_region_init_io(&s->trim_mem, obj, &ctsu_trim_ops, s,
                          TYPE_RX113_CTSU "-trim", 1);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->trim_mem);
    for (unsigned i = 0; i < 3; i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq[i]);
    }
    qdev_init_gpio_in_named(DEVICE(obj), ctsu_trigger, "start", 1);
}

static void ctsu_finalize(Object *obj)
{
    timer_free(RX113_CTSU(obj)->timer);
}

static void ctsu_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->user_creatable = false;
    dc->vmsd = &vmstate_ctsu;
    rc->phases.hold = ctsu_reset;
    device_class_set_props(dc, ctsu_properties);
}

static const TypeInfo ctsu_type = {
    .name = TYPE_RX113_CTSU,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RX113CTSUState),
    .instance_init = ctsu_init,
    .instance_finalize = ctsu_finalize,
    .class_init = ctsu_class_init,
};

static void ctsu_register_types(void)
{
    type_register_static(&ctsu_type);
}

type_init(ctsu_register_types)
