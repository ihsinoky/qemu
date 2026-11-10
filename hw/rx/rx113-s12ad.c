/*
 * Renesas RX113 12-bit A/D converter (S12ADb)
 * R01UH0448EJ0120 Rev.1.20, sections 9, 11, 14, 18, 19 and 36.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/bswap.h"
#include "qemu/host-utils.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/rx/rx113_s12ad.h"
#include "hw/rx/rx113_port.h"
#include "migration/vmstate.h"

#define ADCSR       0x00
#define ADANSA      0x04
#define ADANSA1     0x06
#define ADADS       0x08
#define ADADS1      0x0a
#define ADADC       0x0c
#define ADCER       0x0e
#define ADSTRGR     0x10
#define ADEXICR     0x12
#define ADANSB      0x14
#define ADANSB1     0x16
#define ADDBLDR     0x18
#define ADTSDR      0x1a
#define ADOCDR      0x1c
#define ADHVREFCNT  0x7c
#define ADST        0x8000
#define ADIE        0x1000
#define ADHSC       0x0400
#define TRGE        0x0200
#define EXTRG       0x0100
#define DBLE        0x0080
#define GBADIE      0x0040
#define ACE         0x0020
#define ADRFMT      0x8000
#define CHANNEL_MASK 0x20ffff
#define TEMP_CHANNEL 22
#define REF_CHANNEL  23
#define VOLTAGES     29
#define AVCC         24
#define AVSS         25
#define VREFH        26
#define VREFL        27
#define INTERNAL_REF 28
#define NS_PER_CYCLE 1000000000ULL

/* Each phase uses exactly one clock domain (Table 36.8). */
enum {
    IDLE, START_PCLK, START_ADCLK, SAMPLE, CONVERT, END_ADCLK, END_PCLK,
};

struct RX113S12ADState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    RX113PortState *port;
    QEMUTimer *timer;
    qemu_irq irq[2];
    qemu_irq event;
    uint8_t regs[0x100];
    uint64_t voltage[VOLTAGES];
    uint64_t hz;
    uint32_t pdiv;
    uint32_t adiv;
    /* Remaining work in fastest-clock cycles multiplied by 10^9. */
    uint64_t remaining;
    int64_t last_ns;
    int64_t ready_ns;
    int64_t discharge_ns;
    int64_t reference_ready_ns;
    uint32_t selected;
    uint16_t sample;
    uint16_t sum;
    uint16_t triggers;
    uint8_t phase;
    uint8_t channel;
    uint8_t repeat;
    bool stopped;
    bool group_b;
    bool double_scan;
    bool second;
};

/* §36.2: only documented registers and access widths are accepted. */
static const struct {
    uint8_t addr;
    uint8_t size;
    uint16_t mask;
} control[] = {
    { ADCSR, 2, 0xf7df }, { ADANSA, 2, 0xffff },
    { ADANSA1, 2, 0x20 }, { ADADS, 2, 0xffff },
    { ADADS1, 2, 0x20 }, { ADADC, 1, 3 },
    { ADCER, 2, 0x8020 }, { ADSTRGR, 2, 0x0f0f },
    { ADEXICR, 2, 0x0303 }, { ADANSB, 2, 0xffff },
    { ADANSB1, 2, 0x20 }, { ADHVREFCNT, 1, 3 },
    { 0x60, 1, 0xff }, { 0x61, 1, 0xff },
    { 0x70, 1, 0xff }, { 0x71, 1, 0xff },
    { 0x73, 1, 0xff }, { 0x74, 1, 0xff },
    { 0x75, 1, 0xff }, { 0x76, 1, 0xff },
    { 0x77, 1, 0xff }, { 0x78, 1, 0xff },
    { 0x79, 1, 0xff }, { 0x80, 1, 0xff },
};

static unsigned reg16(RX113S12ADState *s, unsigned addr)
{
    return lduw_le_p(s->regs + addr);
}

static unsigned scan_mode(RX113S12ADState *s)
{
    return (reg16(s, ADCSR) >> 13) & 3;
}

static unsigned sampling_register(unsigned ch)
{
    if (ch == 0) {
        return 0x60;
    }
    if (ch < 8) {
        return 0x72 + ch;
    }
    if (ch < 16) {
        return 0x61;
    }
    return ch == 21 ? 0x80 : ch == TEMP_CHANNEL ? 0x70 : 0x71;
}

static unsigned result_register(unsigned ch)
{
    return ch == TEMP_CHANNEL ? ADTSDR :
           ch == REF_CHANNEL ? ADOCDR : 0x20 + 2 * ch;
}

static bool data_register(hwaddr addr, unsigned size)
{
    return size == 2 && !(addr & 1) &&
           ((addr >= ADDBLDR && addr <= ADOCDR) ||
            (addr >= 0x20 && addr <= 0x3e) || addr == 0x4a);
}

static bool addition(RX113S12ADState *s)
{
    if (s->channel >= TEMP_CHANNEL) {
        return reg16(s, ADEXICR) & (1 << (s->channel - TEMP_CHANNEL));
    }
    return (reg16(s, ADADS) | (reg16(s, ADADS1) << 16)) &
           (1U << s->channel);
}

static unsigned phase_divisor(RX113S12ADState *s)
{
    return s->phase == START_PCLK || s->phase == END_PCLK ?
           s->pdiv : s->adiv;
}

static void set_phase(RX113S12ADState *s, unsigned phase, unsigned cycles)
{
    s->phase = phase;
    s->remaining = cycles * NS_PER_CYCLE * phase_divisor(s);
}

static void begin_sample(RX113S12ADState *s)
{
    set_phase(s, SAMPLE, s->regs[sampling_register(s->channel)] + 1);
}

/* Analog paths branch before the digital-input ASEL switch (§18.2). */
static unsigned quantize(RX113S12ADState *s)
{
    uint64_t low = s->voltage[AVSS], high = s->voltage[AVCC];
    uint64_t input = s->voltage[s->channel == REF_CHANNEL ?
                                INTERNAL_REF : s->channel];

    if (s->port) {
        if (s->port->regs[0x1d6] & 0x80) {
            high = s->voltage[VREFH];
        }
        if (s->port->regs[0x1d7] & 0x80) {
            low = s->voltage[VREFL];
        }
    }
    if (s->regs[ADHVREFCNT] & 2) {
        high = s->voltage[INTERNAL_REF];
    }
    if (high <= low) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "rx113-s12ad: reference high must exceed low\n");
        return 0;
    }
    if (input <= low) {
        return 0;
    }
    if (input >= high) {
        return 4095;
    }
    /* Ideal 12-bit quantization, 4096 codes over the reference span. */
    return MIN(4095, (input - low) * 4096 / (high - low));
}

static void scan_interrupt(RX113S12ADState *s)
{
    unsigned csr = reg16(s, ADCSR);
    bool notify = !s->double_scan || s->second;

    if (s->group_b) {
        if (csr & GBADIE) {
            qemu_irq_pulse(s->irq[1]);
        }
    } else if (notify) {
        if (csr & ADIE) {
            qemu_irq_pulse(s->irq[0]);
        }
        /* §36.5.1: independent of ADIE; group B produces no ELC event. */
        qemu_irq_pulse(s->event);
    }
}

static void advance_phase(RX113S12ADState *s)
{
    unsigned csr = reg16(s, ADCSR);
    unsigned result, addr, count;
    uint32_t next;

    switch (s->phase) {
    case START_PCLK:
        set_phase(s, START_ADCLK, 3);
        break;
    case START_ADCLK:
        begin_sample(s);
        break;
    case SAMPLE:
        /* Sample-and-hold captures the input at the end of sampling. */
        s->sample = quantize(s);
        set_phase(s, CONVERT, csr & ADHSC ? 23 : 29);
        break;
    case CONVERT:
        s->sum += s->sample;
        result = addition(s) ? s->sum << 2 :
                 s->sample << ((reg16(s, ADCER) & ADRFMT) ? 4 : 0);
        addr = s->double_scan && s->second ? ADDBLDR :
               result_register(s->channel);
        stw_le_p(s->regs + addr, result);
        count = addition(s) ? s->regs[ADADC] + 1 : 1;
        if (++s->repeat < count) {
            begin_sample(s);
            break;
        }
        next = s->selected & ~((1U << (s->channel + 1)) - 1);
        if (next) {
            s->channel = ctz32(next);
        } else if (scan_mode(s) == 2) {
            scan_interrupt(s);
            s->channel = ctz32(s->selected);
        } else {
            set_phase(s, END_ADCLK, 2);
            break;
        }
        s->repeat = 0;
        s->sum = 0;
        begin_sample(s);
        break;
    case END_ADCLK:
        set_phase(s, END_PCLK, 1);
        break;
    case END_PCLK:
        scan_interrupt(s);
        if (s->double_scan) {
            s->second = !s->second;
        }
        stw_le_p(s->regs + ADCSR, csr & ~ADST);
        s->phase = IDLE;
        break;
    default:
        g_assert_not_reached();
    }
}

/* Carry sub-nanosecond work across phases instead of rounding each delay. */
static void s12ad_sync(RX113S12ADState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (!s->stopped && s->hz) {
        while (s->phase != IDLE) {
            uint64_t ns = DIV_ROUND_UP(s->remaining, s->hz);
            uint64_t credit;

            if (now - s->last_ns < ns) {
                s->remaining -= (now - s->last_ns) * s->hz;
                break;
            }
            s->last_ns += ns;
            credit = ns * s->hz - s->remaining;
            advance_phase(s);
            if (s->phase != IDLE) {
                s->remaining -= credit;
            }
        }
    }
    s->last_ns = now;
}

static void s12ad_schedule(RX113S12ADState *s)
{
    timer_del(s->timer);
    if (!s->stopped && s->hz && s->phase != IDLE) {
        timer_mod(s->timer, s->last_ns +
                  DIV_ROUND_UP(s->remaining, s->hz));
    }
}

static void s12ad_timer(void *opaque)
{
    RX113S12ADState *s = opaque;

    s12ad_sync(s);
    s12ad_schedule(s);
}

void rx113_s12ad_set_clock(DeviceState *dev, uint64_t pclkb, uint64_t pclkd,
                           bool stopped)
{
    RX113S12ADState *s = RX113_S12AD(dev);
    unsigned old_div;

    s12ad_sync(s);
    old_div = phase_divisor(s);
    if (pclkb && pclkd) {
        s->hz = MAX(pclkb, pclkd);
        s->pdiv = s->hz / pclkb;
        s->adiv = s->hz / pclkd;
    }
    if (s->phase != IDLE) {
        s->remaining = muldiv64(s->remaining, phase_divisor(s), old_div);
    }
    if (s->stopped && !stopped) {
        s->ready_ns = s->last_ns + 1000;
    }
    s->stopped = stopped;
    s12ad_schedule(s);
}

static void start_scan(RX113S12ADState *s, bool group_b, bool internal,
                       bool asynchronous)
{
    unsigned csr = reg16(s, ADCSR);
    unsigned mode = scan_mode(s);
    uint32_t selected = reg16(s, group_b ? ADANSB : ADANSA) |
                        (reg16(s, group_b ? ADANSB1 : ADANSA1) << 16);
    unsigned extended = reg16(s, ADEXICR) >> 8;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (s->stopped || s->phase != IDLE) {
        return;
    }
    if (now < s->ready_ns || now < s->reference_ready_ns ||
        (s->regs[ADHVREFCNT] & 3) == 3) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "rx113-s12ad: conversion before analog stabilization\n");
        goto invalid;
    }
    if (mode == 3) {
        goto invalid;
    }
    if ((csr & DBLE) && !group_b) {
        unsigned ch = csr & 31;

        if (mode == 2 || !(CHANNEL_MASK & (1U << ch))) {
            goto invalid;
        }
        selected = 1U << ch;
    }
    if (extended) {
        if (mode != 0 || selected || (csr & DBLE) || extended == 3 ||
            (s->regs[ADHVREFCNT] & 2)) {
            goto invalid;
        }
        selected = 1U << (extended == 1 ? TEMP_CHANNEL : REF_CHANNEL);
    }
    if (!selected) {
        goto invalid;
    }
    s->selected = selected;
    s->channel = ctz32(selected);
    s->group_b = group_b;
    s->double_scan = (csr & DBLE) && !group_b && internal;
    s->repeat = 0;
    s->sum = 0;
    stw_le_p(s->regs + ADCSR, csr | ADST);
    set_phase(s, START_PCLK, asynchronous ? 4 : 2);
    s->last_ns = now;
    s12ad_schedule(s);
    return;

invalid:
    qemu_log_mask(LOG_GUEST_ERROR,
                  "rx113-s12ad: invalid conversion configuration\n");
    stw_le_p(s->regs + ADCSR, csr & ~ADST);
}

/* Inputs 1..9 match Table 36.5/6, input 0 is qualified ADTRG0#. */
static void trigger_input(void *opaque, int n, int level)
{
    RX113S12ADState *s = opaque;
    unsigned old = (s->triggers >> n) & 1;
    unsigned csr, select;

    s12ad_sync(s);
    s->triggers = deposit32(s->triggers, n, 1, !!level);
    csr = reg16(s, ADCSR);
    select = reg16(s, ADSTRGR);
    if (!s->stopped && (csr & TRGE)) {
        if (n == 0 && old && !level && (csr & EXTRG) &&
            !(select & 0x0f00) && scan_mode(s) != 1 && !(csr & DBLE)) {
            start_scan(s, false, false, true);
        } else if (n && !old && level && !(csr & EXTRG)) {
            if (n == ((select >> 8) & 15)) {
                start_scan(s, false, true, false);
            } else if (scan_mode(s) == 1 && n == (select & 15)) {
                start_scan(s, true, true, false);
            }
        }
    }
    s12ad_schedule(s);
}

static uint64_t s12ad_read(void *opaque, hwaddr addr, unsigned size)
{
    RX113S12ADState *s = opaque;
    unsigned value;

    s12ad_sync(s);
    if (s->stopped) {
        return 0;
    }
    if (data_register(addr, size)) {
        value = reg16(s, addr);
        if (reg16(s, ADCER) & ACE) {
            stw_le_p(s->regs + addr, 0);
        }
        return value;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(control); i++) {
        if (addr == control[i].addr && size == control[i].size) {
            return size == 1 ? s->regs[addr] : reg16(s, addr);
        }
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "rx113-s12ad: invalid read at 0x%" HWADDR_PRIx " size %u\n",
                  addr, size);
    return 0;
}

static void s12ad_write(void *opaque, hwaddr addr, uint64_t value,
                         unsigned size)
{
    RX113S12ADState *s = opaque;
    unsigned old;

    s12ad_sync(s);
    if (s->stopped || data_register(addr, size)) {
        return;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(control); i++) {
        if (addr != control[i].addr || size != control[i].size) {
            continue;
        }
        value &= control[i].mask;
        old = size == 1 ? s->regs[addr] : reg16(s, addr);
        if (s->phase != IDLE && addr != ADCSR) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "rx113-s12ad: configuration changed during scan\n");
            return;
        }
        if (addr == ADCSR && s->phase != IDLE) {
            /* Starting again is ignored; allow IRQ masks and forced stop. */
            value = (old & ~(ADIE | GBADIE | ADST | TRGE)) |
                    (value & (ADIE | GBADIE | ADST | TRGE));
        }
        if (addr == ADHVREFCNT) {
            int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

            if (value == 3 && old != 3) {
                s->discharge_ns = now;
            } else if (value == 2 && old == 3 &&
                       now - s->discharge_ns >= 1000) {
                s->reference_ready_ns = now + 5000;
            } else if (value == 2 && old != 2) {
                value = old;
            }
            if (!(value & 2)) {
                s->reference_ready_ns = 0;
            }
        }
        if (size == 1) {
            s->regs[addr] = value;
        } else {
            stw_le_p(s->regs + addr, value);
        }
        if (addr == ADCSR) {
            if (!(value & ADST)) {
                s->phase = IDLE;
            }
            if (!(value & DBLE)) {
                s->second = false;
            }
            if ((value & ADST) && !(old & ADST)) {
                start_scan(s, false, false, false);
            }
        }
        s12ad_schedule(s);
        return;
    }
    qemu_log_mask(LOG_GUEST_ERROR,
                  "rx113-s12ad: invalid write at 0x%" HWADDR_PRIx " size %u\n",
                  addr, size);
}

static const MemoryRegionOps s12ad_ops = {
    .read = s12ad_read,
    .write = s12ad_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 2,
    .impl.min_access_size = 1,
    .impl.max_access_size = 2,
};

static void voltage_get(Object *obj, Visitor *v, const char *name,
                        void *opaque, Error **errp)
{
    RX113S12ADState *s = RX113_S12AD(obj);
    uint64_t value = s->voltage[GPOINTER_TO_UINT(opaque)];

    visit_type_uint64(v, name, &value, errp);
}

static void voltage_set(Object *obj, Visitor *v, const char *name,
                        void *opaque, Error **errp)
{
    RX113S12ADState *s = RX113_S12AD(obj);
    uint64_t value;

    if (!visit_type_uint64(v, name, &value, errp)) {
        return;
    }
    if (value > 10000000) {
        error_setg(errp, "rx113-s12ad: voltage must be 0..10000000 microvolts");
        return;
    }
    s12ad_sync(s);
    s->voltage[GPOINTER_TO_UINT(opaque)] = value;
    s12ad_schedule(s);
}

static void s12ad_reset_hold(Object *obj, ResetType type)
{
    RX113S12ADState *s = RX113_S12AD(obj);

    timer_del(s->timer);
    memset(s->regs, 0, sizeof(s->regs));
    for (unsigned ch = 0; ch <= REF_CHANNEL; ch++) {
        s->regs[sampling_register(ch)] = 0x14;
    }
    s->phase = IDLE;
    s->second = false;
    s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->ready_ns = s->reference_ready_ns = 0;
    /* Board voltages and trigger levels survive MCU reset. */
}

static int s12ad_pre_save(void *opaque)
{
    s12ad_sync(opaque);
    return 0;
}

static int s12ad_post_load(void *opaque, int version_id)
{
    RX113S12ADState *s = opaque;

    if (s->phase > END_PCLK || !s->pdiv || !s->adiv ||
        s->pdiv > 64 || s->adiv > 64 || s->channel > REF_CHANNEL ||
        (s->phase != IDLE && (!s->selected || !s->remaining))) {
        return -EINVAL;
    }
    s12ad_schedule(s);
    return 0;
}

static const VMStateDescription vmstate_s12ad = {
    .name = TYPE_RX113_S12AD,
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = s12ad_pre_save,
    .post_load = s12ad_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, RX113S12ADState, 0x100),
        VMSTATE_UINT64_ARRAY(voltage, RX113S12ADState, VOLTAGES),
        VMSTATE_UINT64(hz, RX113S12ADState),
        VMSTATE_UINT32(pdiv, RX113S12ADState),
        VMSTATE_UINT32(adiv, RX113S12ADState),
        VMSTATE_UINT64(remaining, RX113S12ADState),
        VMSTATE_INT64(last_ns, RX113S12ADState),
        VMSTATE_INT64(ready_ns, RX113S12ADState),
        VMSTATE_INT64(discharge_ns, RX113S12ADState),
        VMSTATE_INT64(reference_ready_ns, RX113S12ADState),
        VMSTATE_UINT32(selected, RX113S12ADState),
        VMSTATE_UINT16(sample, RX113S12ADState),
        VMSTATE_UINT16(sum, RX113S12ADState),
        VMSTATE_UINT16(triggers, RX113S12ADState),
        VMSTATE_UINT8(phase, RX113S12ADState),
        VMSTATE_UINT8(channel, RX113S12ADState),
        VMSTATE_UINT8(repeat, RX113S12ADState),
        VMSTATE_BOOL(stopped, RX113S12ADState),
        VMSTATE_BOOL(group_b, RX113S12ADState),
        VMSTATE_BOOL(double_scan, RX113S12ADState),
        VMSTATE_BOOL(second, RX113S12ADState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property s12ad_properties[] = {
    DEFINE_PROP_LINK("port", RX113S12ADState, port, TYPE_RX113_PORT,
                     RX113PortState *),
};

static void s12ad_init(Object *obj)
{
    RX113S12ADState *s = RX113_S12AD(obj);
    static const char *const supply[] = {
        "avcc-uv", "avss-uv", "vrefh-uv", "vrefl-uv", "internal-reference-uv",
    };

    s->hz = 500000;
    s->pdiv = s->adiv = 1;
    s->stopped = true;
    s->triggers = 1;
    s->voltage[AVCC] = s->voltage[VREFH] = 3300000;
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, s12ad_timer, s);
    memory_region_init_io(&s->iomem, obj, &s12ad_ops, s,
                          TYPE_RX113_S12AD, sizeof(s->regs));
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq[0]);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq[1]);
    qdev_init_gpio_in_named(DEVICE(obj), trigger_input, "trigger", 10);
    qdev_init_gpio_out_named(DEVICE(obj), &s->event, "scan-end", 1);
    for (unsigned i = 0; i < VOLTAGES; i++) {
        g_autofree char *name = NULL;

        if (i < 16 || i == 21) {
            name = g_strdup_printf("an%03u-uv", i);
        } else if (i == TEMP_CHANNEL) {
            name = g_strdup("temperature-uv");
        } else if (i >= AVCC) {
            name = g_strdup(supply[i - AVCC]);
        } else {
            continue;
        }
        object_property_add(obj, name, "uint64", voltage_get, voltage_set,
                             NULL, GUINT_TO_POINTER(i));
    }
}

static void s12ad_finalize(Object *obj)
{
    timer_free(RX113_S12AD(obj)->timer);
}

static void s12ad_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->vmsd = &vmstate_s12ad;
    dc->user_creatable = false;
    device_class_set_props(dc, s12ad_properties);
    rc->phases.hold = s12ad_reset_hold;
}

static const TypeInfo s12ad_info = {
    .name = TYPE_RX113_S12AD,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RX113S12ADState),
    .instance_init = s12ad_init,
    .instance_finalize = s12ad_finalize,
    .class_init = s12ad_class_init,
};

static void s12ad_register_types(void)
{
    type_register_static(&s12ad_info);
}

type_init(s12ad_register_types)
