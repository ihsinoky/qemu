/*
 * Renesas RX113 12-bit D/A converter (R12DAA)
 * R01UH0448EJ0120 Rev.1.20, section 37.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/host-utils.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/core/qdev-properties.h"
#include "hw/rx/rx113_dac.h"
#include "hw/rx/rx113_s12ad.h"
#include "migration/vmstate.h"

#define DACR 4
#define DADPR 5
#define DAADSCR 6
#define DAVREFCR 7
#define NS_PER_SECOND 1000000000ULL

struct RX113DACState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    DeviceState *adc;
    QEMUTimer *timer;
    uint16_t dadr[2];
    uint16_t code[2];
    uint8_t dacr;
    uint8_t dadpr;
    uint8_t daadscr;
    uint8_t davrefcr;
    uint8_t pending;
    uint64_t frozen_uv[2];
    uint64_t hz;
    /* Remaining PCLKB work in cycles multiplied by 10^9. */
    uint64_t remaining;
    int64_t last_ns;
    bool stopped;
};

static uint16_t data_mask(RX113DACState *s)
{
    return s->dadpr ? 0xfff0 : 0x0fff;
}

static void latch(RX113DACState *s, unsigned channels)
{
    for (unsigned ch = 0; ch < 2; ch++) {
        if (channels & (1 << ch)) {
            s->code[ch] = (s->dadr[ch] & data_mask(s)) >>
                          (s->dadpr ? 4 : 0);
        }
    }
}

static void dac_sync(RX113DACState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (!s->stopped && s->hz && s->remaining) {
        uint64_t elapsed = now - s->last_ns;

        if (elapsed >= DIV_ROUND_UP(s->remaining, s->hz)) {
            latch(s, s->pending);
            s->remaining = 0;
            s->pending = 0;
        } else {
            s->remaining -= elapsed * s->hz;
        }
    }
    s->last_ns = now;
}

static void dac_schedule(RX113DACState *s)
{
    timer_del(s->timer);
    if (!s->stopped && s->hz && s->remaining) {
        timer_mod(s->timer, s->last_ns + DIV_ROUND_UP(s->remaining, s->hz));
    }
}

static void dac_timer(void *opaque)
{
    RX113DACState *s = opaque;

    dac_sync(s);
    dac_schedule(s);
}

static uint64_t output_uv(RX113DACState *s, unsigned ch)
{
    uint64_t high, low;
    const char *high_name, *low_name;

    if (!(s->dacr & (0x40 << ch)) || !s->davrefcr) {
        return 0;
    }
    /* Both converters share the board's voltage fixtures. */
    high_name = s->davrefcr == 1 ? "avcc-uv" :
                s->davrefcr == 3 ? "internal-reference-uv" : "vrefh-uv";
    low_name = s->davrefcr == 6 ? "vrefl-uv" : "avss-uv";
    high = object_property_get_uint(OBJECT(s->adc), high_name, &error_abort);
    low = object_property_get_uint(OBJECT(s->adc), low_name, &error_abort);
    /* Ideal output potential; 0 also represents disabled/high impedance. */
    return high > low ? low + (high - low) * s->code[ch] / 4096 : 0;
}

void rx113_dac_set_clock(DeviceState *dev, uint64_t pclkb, bool stopped)
{
    RX113DACState *s = RX113_DAC(dev);

    dac_sync(s);
    if (!s->stopped && stopped) {
        for (unsigned ch = 0; ch < 2; ch++) {
            s->frozen_uv[ch] = output_uv(s, ch);
        }
    }
    if (s->stopped && !stopped && s->pending && !s->remaining &&
        s->daadscr && !rx113_s12ad_active(s->adc)) {
        s->remaining = NS_PER_SECOND;
    }
    s->hz = pclkb;
    s->stopped = stopped;
    dac_schedule(s);
}

/* §37.3.1: an idle ADC allows the update after one PCLKB cycle. */
static void data_changed(RX113DACState *s, unsigned channels, bool adc_active)
{
    if (!s->daadscr) {
        latch(s, channels);
        return;
    }
    s->pending |= channels;
    if (!adc_active && !s->remaining) {
        s->remaining = NS_PER_SECOND;
        s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    }
    dac_schedule(s);
}

/* Brief-authorized fallback: independent S12AD scan-end, not its IRQ. */
static void sync_input(void *opaque, int n, int level)
{
    RX113DACState *s = opaque;

    if (level && !s->stopped && s->daadscr) {
        latch(s, s->pending);
        s->pending = 0;
        s->remaining = 0;
        dac_schedule(s);
    }
}

static void elc_input(void *opaque, int n, int level)
{
    RX113DACState *s = opaque;

    if (!level || s->stopped) {
        return;
    }
    if (s->daadscr) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "rx113-dac: event link prohibited with DAADST=1\n");
        return;
    }
    s->dacr |= 0x40;
}

static bool valid_access(hwaddr addr, unsigned size)
{
    return (size == 2 && (addr == 0 || addr == 2)) ||
           (size == 1 && addr >= DACR && addr <= DAVREFCR);
}

static uint64_t dac_read(void *opaque, hwaddr addr, unsigned size)
{
    RX113DACState *s = opaque;

    dac_sync(s);
    if (s->stopped) {
        return 0;
    }
    if (!valid_access(addr, size)) {
        qemu_log_mask(LOG_GUEST_ERROR, "rx113-dac: invalid read at 0x%"
                      HWADDR_PRIx " size %u\n", addr, size);
        return 0;
    }
    switch (addr) {
    case 0:
    case 2:
        return s->dadr[addr / 2] & data_mask(s);
    case DACR:
        return s->dacr | 0x1f;
    case DADPR:
        return s->dadpr;
    case DAADSCR:
        return s->daadscr;
    case DAVREFCR:
        return s->davrefcr;
    default:
        g_assert_not_reached();
    }
}

static void dac_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    RX113DACState *s = opaque;
    bool adc_active;

    dac_sync(s);
    if (s->stopped) {
        return;
    }
    if (!valid_access(addr, size)) {
        qemu_log_mask(LOG_GUEST_ERROR, "rx113-dac: invalid write at 0x%"
                      HWADDR_PRIx " size %u\n", addr, size);
        return;
    }
    /* Deliver any already-completed ADC event before changing DAC data. */
    adc_active = rx113_s12ad_active(s->adc);
    switch (addr) {
    case 0:
    case 2:
        s->dadr[addr / 2] = value & data_mask(s);
        data_changed(s, 1 << (addr / 2), adc_active);
        break;
    case DACR:
        s->dacr = value & 0xc0;
        break;
    case DADPR:
        s->dadpr = value & 0x80;
        data_changed(s, 3, adc_active);
        break;
    case DAADSCR:
        s->daadscr = value & 0x80;
        if (!s->daadscr) {
            latch(s, 3);
            s->pending = 0;
            s->remaining = 0;
            dac_schedule(s);
        }
        break;
    case DAVREFCR:
        value &= 7;
        if (value != 0 && value != 1 && value != 3 && value != 6) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "rx113-dac: prohibited reference selection\n");
            break;
        }
        s->davrefcr = value;
        break;
    }
}

static const MemoryRegionOps dac_ops = {
    .read = dac_read,
    .write = dac_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .valid.unaligned = true,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
    .impl.unaligned = true,
};

static void voltage_get(Object *obj, Visitor *v, const char *name,
                        void *opaque, Error **errp)
{
    RX113DACState *s = RX113_DAC(obj);
    unsigned ch = GPOINTER_TO_UINT(opaque);
    uint64_t value;

    dac_sync(s);
    value = s->stopped ? s->frozen_uv[ch] : output_uv(s, ch);
    visit_type_uint64(v, name, &value, errp);
}

static void dac_reset_hold(Object *obj, ResetType type)
{
    RX113DACState *s = RX113_DAC(obj);

    timer_del(s->timer);
    memset(s->dadr, 0, sizeof(s->dadr));
    memset(s->code, 0, sizeof(s->code));
    memset(s->frozen_uv, 0, sizeof(s->frozen_uv));
    s->dacr = s->dadpr = s->daadscr = s->davrefcr = 0;
    s->pending = 0;
    s->remaining = 0;
    s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static int dac_pre_save(void *opaque)
{
    dac_sync(opaque);
    return 0;
}

static int dac_post_load(void *opaque, int version_id)
{
    RX113DACState *s = opaque;

    if ((s->dacr & ~0xc0) || (s->dadpr & ~0x80) ||
        (s->daadscr & ~0x80) || (s->pending & ~3) ||
        s->remaining > NS_PER_SECOND ||
        (s->remaining && (!s->pending || !s->daadscr)) ||
        (s->davrefcr != 0 && s->davrefcr != 1 &&
         s->davrefcr != 3 && s->davrefcr != 6)) {
        return -EINVAL;
    }
    for (unsigned ch = 0; ch < 2; ch++) {
        if (s->code[ch] > 4095 || s->frozen_uv[ch] > 10000000) {
            return -EINVAL;
        }
    }
    dac_schedule(s);
    return 0;
}

static const VMStateDescription vmstate_dac = {
    .name = TYPE_RX113_DAC,
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = dac_pre_save,
    .post_load = dac_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16_ARRAY(dadr, RX113DACState, 2),
        VMSTATE_UINT16_ARRAY(code, RX113DACState, 2),
        VMSTATE_UINT8(dacr, RX113DACState),
        VMSTATE_UINT8(dadpr, RX113DACState),
        VMSTATE_UINT8(daadscr, RX113DACState),
        VMSTATE_UINT8(davrefcr, RX113DACState),
        VMSTATE_UINT8(pending, RX113DACState),
        VMSTATE_UINT64_ARRAY(frozen_uv, RX113DACState, 2),
        VMSTATE_UINT64(hz, RX113DACState),
        VMSTATE_UINT64(remaining, RX113DACState),
        VMSTATE_INT64(last_ns, RX113DACState),
        VMSTATE_BOOL(stopped, RX113DACState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property dac_properties[] = {
    DEFINE_PROP_LINK("adc", RX113DACState, adc, TYPE_RX113_S12AD,
                     DeviceState *),
};

static void dac_init(Object *obj)
{
    RX113DACState *s = RX113_DAC(obj);

    s->stopped = true;
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, dac_timer, s);
    memory_region_init_io(&s->iomem, obj, &dac_ops, s, TYPE_RX113_DAC, 0x10);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    qdev_init_gpio_in_named(DEVICE(obj), sync_input, "sync", 1);
    qdev_init_gpio_in_named(DEVICE(obj), elc_input, "elc", 1);
    for (unsigned ch = 0; ch < 2; ch++) {
        g_autofree char *name = g_strdup_printf("da%u-uv", ch);

        object_property_add(obj, name, "uint64", voltage_get, NULL,
                             NULL, GUINT_TO_POINTER(ch));
    }
}

static void dac_finalize(Object *obj)
{
    timer_free(RX113_DAC(obj)->timer);
}

static void dac_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->user_creatable = false;
    dc->vmsd = &vmstate_dac;
    device_class_set_props(dc, dac_properties);
    rc->phases.hold = dac_reset_hold;
}

static const TypeInfo dac_info = {
    .name = TYPE_RX113_DAC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RX113DACState),
    .instance_init = dac_init,
    .instance_finalize = dac_finalize,
    .class_init = dac_class_init,
};

static void dac_register_types(void)
{
    type_register_static(&dac_info);
}

type_init(dac_register_types)
