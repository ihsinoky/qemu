/*
 * Renesas RX113 I/O ports and multi-function pin controller
 * Hardware manual: R01UH0448EJ0120 Rev.1.20, sections 18 and 19.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/rx/rx113_port.h"
#include "hw/rx/rx113_elc.h"
#include "migration/vmstate.h"

#define PDR  0x000
#define PODR 0x020
#define PIDR 0x040
#define PMR  0x060
#define ODR  0x080
#define PCR  0x0c0
#define PWPR 0x11f
#define PSRA 0x121
#define PFS  0x140
#define B0WI 0x80
#define PFSWE 0x40
#define ASEL 0x80
#define ISEL 0x40
#define PSEL 0x1f

/* Table 18.1: logical port numbers 0..5, 9..F, J, with holes retained. */
static const uint8_t pins100[RX113_PORT_COUNT] = {
    0x94, 0xff, 0xff, 0x27, 0x5f, 0x7f, 0, 0, 0, 0x07,
    0xff, 0xff, 0xff, 0x1f, 0xff, 0xc0, 0, 0, 0xcd,
};
static const uint8_t pins64[RX113_PORT_COUNT] = {
    0, 0xf0, 0xc0, 0x27, 0x07, 0x30, 0, 0, 0, 0,
    0x5b, 0xeb, 0xfc, 0x07, 0xff, 0, 0, 0, 0xc5,
};

/* 19.2.3..19.2.13, Table 19.1: IRQ number + 1 (zero means none). */
static const uint8_t irq_map[0x130] = {
    [0x10] = 7, [0x11] = 8, [0x12] = 3, [0x13] = 4,
    [0x14] = 5, [0x15] = 6, [0x16] = 7, [0x17] = 8,
    [0x27] = 4, [0x30] = 1, [0x31] = 2, [0x32] = 3,
    [0x56] = 6, [0xa3] = 7, [0xa4] = 6, [0xa6] = 4,
    [0xb0] = 3, [0xb1] = 5, [0xc4] = 3,
    [0xd0] = 1, [0xd1] = 2, [0xd2] = 3, [0xd3] = 4, [0xd4] = 5,
    [0xe0] = 1, [0xe1] = 2, [0xe2] = 8, [0xe3] = 4,
    [0xe4] = 5, [0xe5] = 6, [0xe6] = 7, [0xe7] = 8,
};

/* GPIO array indices are port * 8 + bit, not the hexadecimal pin name. */
static unsigned pin_name(unsigned pin)
{
    return (pin / 8) * 16 + pin % 8;
}

/* Table 19.1: the twelve TS pads; TSCAP is the analog LPF pad. */
static bool ctsu_pin(unsigned name)
{
    switch (name) {
    case 0x07: case 0x04: case 0x02: case 0x123:
    case 0x25: case 0x24: case 0x23: case 0x22:
    case 0x21: case 0x20: case 0x27: case 0x32:
        return true;
    default:
        return false;
    }
}

static bool cacref_pin(unsigned name)
{
    return name == 0x15 || name == 0x27 || name == 0xa0 || name == 0xc7;
}

static uint8_t port_pins(RX113PortState *s, unsigned port)
{
    return s->package_pins == 64 ? pins64[port] : pins100[port];
}

/* 18.3.8: in 64-pin packages PC0/1 and PB6/7 share physical pads. */
static uint8_t gpio_pins(RX113PortState *s, unsigned port)
{
    uint8_t mask = port_pins(s, port);

    if (s->package_pins == 64) {
        if (port == 11) {
            mask &= ~s->regs[PSRA];
        } else if (port == 12) {
            mask |= s->regs[PSRA] >> 6;
        }
    }
    return mask;
}

static uint8_t pfs_mask(RX113PortState *s, unsigned pin)
{
    unsigned port = pin / 8;
    unsigned bit = pin % 8;
    unsigned name = pin_name(pin);
    uint8_t mask;

    if (!(port_pins(s, port) & (1 << bit)) || name == 0x35) {
        return 0;
    }
    /* 19.2.6, 19.2.8, 19.2.15: analog-only PFS fields. */
    if (port == 4 || port == 9 ||
        (port == 18 && bit != 3)) {
        return ASEL;
    }
    mask = PSEL;
    if (irq_map[name]) {
        mask |= ISEL;
    }
    /* 19.2.4, 19.2.9, 19.2.13: analog fields on mixed-function pins. */
    if (name == 0x27 || name == 0xa3 || name == 0xa4 || port == 14) {
        mask |= ASEL;
    }
    return mask;
}

/* 18.3.5/6 and Table 18.2: two-bit fields, mostly N-channel only. */
static uint8_t odr_mask(RX113PortState *s, unsigned offset)
{
    unsigned port = offset / 2;
    unsigned first = (offset % 2) * 4;
    uint8_t pins = gpio_pins(s, port);
    uint8_t mask = 0;

    if (port == 4 || port == 9 || port == 13 || port == 15) {
        return 0;
    }
    if (port == 3) {
        pins &= 7;
    } else if (port == 5) {
        pins &= 0x4f;
    } else if (port == 18) {
        pins &= 8;
    }
    for (unsigned i = 0; i < 4; i++) {
        unsigned name = port * 16 + first + i;

        if (pins & (1 << (first + i))) {
            mask |= (name == 0x10 || name == 0x14 || name == 0xe1 ?
                     3 : 1) << (i * 2);
        }
    }
    return mask;
}

static uint8_t port_mask(RX113PortState *s, hwaddr addr)
{
    unsigned port = addr & 0x1f;
    uint8_t mask;

    if (addr >= ODR && addr < ODR + RX113_PORT_COUNT * 2) {
        return odr_mask(s, addr - ODR);
    }
    if (port >= RX113_PORT_COUNT || !pins100[port]) {
        return 0;
    }
    if (addr < PODR) {
        /* 18.4: firmware must initialize even unbonded/reserved PDR bits. */
        return port == 3 ? 0xdf : 0xff;
    }
    mask = gpio_pins(s, port);
    if ((addr & ~0x1f) == PMR) {
        mask = port_pins(s, port); /* PSRA does not switch PMR. */
    }
    if (port == 3) {
        mask &= ~0x20; /* P35 is input-only. */
    }
    if ((addr & ~0x1f) == PCR) {
        if (port == 4 || port == 9) {
            return 0;
        }
        if (port == 18) {
            mask &= 0x0d;
        }
    }
    return mask;
}

static void port_update(RX113PortState *s, bool notify)
{
    uint8_t old_b = s->levels[11];
    uint8_t irq_levels = 0;
    bool cacref = false;

    memset(s->levels, 0, sizeof(s->levels));
    memset(s->driving, 0, sizeof(s->driving));
    for (unsigned pin = 0; pin < RX113_PORT_PINS; pin++) {
        unsigned port = pin / 8;
        unsigned bit = pin % 8;
        unsigned gp = port, gb = bit;
        unsigned name = pin_name(pin);
        uint8_t pfs = s->regs[PFS + pin];
        bool peripheral = s->regs[PMR + port] & (1 << bit);
        bool drive = false, level = false;
        bool output = false;

        if (port_pins(s, port) & (1 << bit)) {
            if (s->package_pins == 64 && port == 11 && bit >= 6 &&
                (s->regs[PSRA] & (1 << bit))) {
                gp = 12;
                gb = bit - 6;
            }
            if (!(pfs & ASEL)) {
                unsigned od = (s->regs[ODR + gp * 2 + gb / 4] >>
                               ((gb % 4) * 2)) & 3;

                /* 18.2: PMR overrides PDR/PODR; PSEL=0 means Hi-Z. */
                output = !peripheral && name != 0x35 &&
                        (s->regs[PDR + gp] & (1 << gb));
                drive = output;
                level = s->regs[PODR + gp] & (1 << gb);
                if ((od == 1 && level) || (od == 2 && !level)) {
                    drive = false;
                }
                if (!drive) {
                    level = s->external_mask[port] & (1 << bit) ?
                            s->external[port] & (1 << bit) :
                            (!output &&
                             (s->regs[PCR + gp] & (1 << gb)));
                }
            }
            /* Tables 19.3..19.16: CACREF uses PSEL=7 on these pads. */
            if (cacref_pin(name) && peripheral && !(pfs & ASEL) &&
                (pfs & PSEL) == 7) {
                cacref |= level;
            }
            if (s->ctsu_fixed && ctsu_pin(name) && peripheral &&
                !(pfs & ASEL) && (pfs & PSEL) == 25) {
                drive = true;
                level = s->ctsu_level;
            }
            s->levels[port] |= level << bit;
            s->driving[port] |= drive << bit;
            if (gp != port) {
                s->levels[gp] |= level << gb;
            }
            /* 19.3.2/4/5: analog, LCD and CTSU suppress IRQ input. */
            if ((pfs & ISEL) && !(pfs & ASEL) &&
                !(peripheral && ((pfs & PSEL) == 24 ||
                                 (pfs & PSEL) == 25)) && level) {
                irq_levels |= 1 << (irq_map[name] - 1);
            }
        }
        qemu_set_irq(s->pin_out[pin], level);
        qemu_set_irq(s->pin_oe[pin], drive);
    }
    if (notify && s->elc) {
        rx113_elc_port_input(s->elc, old_b, s->levels[11]);
    }
    qemu_set_irq(s->cacref, cacref);
    for (unsigned i = 0; i < 8; i++) {
        qemu_set_irq(s->irq[i], (irq_levels >> i) & 1);
    }
}

uint8_t rx113_port_b_mask(RX113PortState *s)
{
    return gpio_pins(s, 11) & ~s->regs[PMR + 11];
}

uint8_t rx113_port_b_direction(RX113PortState *s)
{
    return s->regs[PDR + 11];
}

uint8_t rx113_port_b_levels(RX113PortState *s)
{
    return s->levels[11];
}

uint8_t rx113_port_b_latch(RX113PortState *s)
{
    return s->regs[PODR + 11];
}

void rx113_port_b_write(RX113PortState *s, uint8_t mask, uint8_t value)
{
    mask &= rx113_port_b_mask(s);
    s->regs[PODR + 11] = (s->regs[PODR + 11] & ~mask) | (value & mask);
    port_update(s, true);
}

void rx113_port_set_ctsu(RX113PortState *s, bool fixed, bool level)
{
    if (s) {
        s->ctsu_fixed = fixed;
        s->ctsu_level = level;
        port_update(s, true);
    }
}

static void port_input(void *opaque, int pin, int level)
{
    RX113PortState *s = opaque;
    unsigned port = pin / 8;
    uint8_t mask = 1 << (pin % 8);

    /* A negative GPIO level releases the external driver. */
    if (level < 0) {
        s->external_mask[port] &= ~mask;
    } else {
        s->external_mask[port] |= mask;
        s->external[port] = (s->external[port] & ~mask) |
                            (level != 0 ? mask : 0);
    }
    port_update(s, true);
}

static uint64_t port_read(void *opaque, hwaddr addr, unsigned size)
{
    RX113PortState *s = opaque;

    if (addr >= PIDR && addr < PIDR + RX113_PORT_COUNT) {
        unsigned port = addr - PIDR;

        /* 18.3.3, 18.3.8: PSRA switches the GPIO input register too. */
        return s->levels[port] & gpio_pins(s, port);
    }
    if (addr < ODR + RX113_PORT_COUNT * 2 ||
        (addr >= PCR && addr < PCR + RX113_PORT_COUNT)) {
        return s->regs[addr] & port_mask(s, addr);
    }
    return s->regs[addr];
}

static void port_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    RX113PortState *s = opaque;
    uint8_t mask = 0;

    if (addr < ODR || (addr >= PCR && addr < PCR + RX113_PORT_COUNT) ||
        (addr >= ODR && addr < ODR + RX113_PORT_COUNT * 2)) {
        if (addr >= PIDR && addr < PMR) {
            return; /* 18.3.3: PIDR is read-only. */
        }
        mask = port_mask(s, addr);
    } else if (addr == PWPR) {
        /* 19.2.1: PFSWE permission comes from B0WI BEFORE this write. */
        mask = B0WI | ((s->regs[PWPR] & B0WI) ? 0 : PFSWE);
    } else if (addr == PSRA) {
        mask = s->package_pins == 64 ? 0xc0 : 0;
    } else if (addr >= PFS && addr < PFS + RX113_PORT_PINS &&
               (s->regs[PWPR] & PFSWE)) {
        mask = pfs_mask(s, addr - PFS);
    }
    if (addr == PODR + 11 && s->elc) {
        mask &= rx113_elc_port_write_mask(s->elc);
    }
    s->regs[addr] = (s->regs[addr] & ~mask) | (val & mask);
    if (addr >= PMR && addr < PMR + RX113_PORT_COUNT && val & mask) {
        unsigned port = addr - PMR;
        uint8_t unsupported = val & mask;

        for (unsigned bit = 0; bit < 8; bit++) {
            if (cacref_pin(port * 16 + bit) &&
                (s->regs[PFS + port * 8 + bit] & (PSEL | ASEL)) == 7) {
                unsupported &= ~(1 << bit);
            }
        }
        if (unsupported) {
            qemu_log_mask(LOG_UNIMP,
                          "rx113-port: peripheral pad signals are unavailable "
                          "(PMR port %u = 0x%02x)\n", port, unsupported);
        }
    }
    port_update(s, true);
}

static const MemoryRegionOps port_ops = {
    .read = port_read,
    .write = port_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
};

static char *port_levels(Object *obj, Error **errp)
{
    RX113PortState *s = RX113_PORT(obj);
    GString *str = g_string_new(NULL);

    for (unsigned i = 0; i < RX113_PORT_COUNT; i++) {
        g_string_append_printf(str, "%02x", s->levels[i]);
    }
    return g_string_free(str, false);
}

static char *port_driving(Object *obj, Error **errp)
{
    RX113PortState *s = RX113_PORT(obj);
    GString *str = g_string_new(NULL);

    for (unsigned i = 0; i < RX113_PORT_COUNT; i++) {
        g_string_append_printf(str, "%02x", s->driving[i]);
    }
    return g_string_free(str, false);
}

static void port_reset_hold(Object *obj, ResetType type)
{
    RX113PortState *s = RX113_PORT(obj);

    /* 18.3, 19.2: all control registers zero except PWPR.B0WI. */
    memset(s->regs, 0, sizeof(s->regs));
    s->regs[PWPR] = B0WI;
    /* External board drivers survive an MCU reset. */
    port_update(s, false);
}

static int port_post_load(void *opaque, int version_id)
{
    port_update(opaque, false);
    return 0;
}

static const VMStateDescription vmstate_port = {
    .name = TYPE_RX113_PORT,
    .version_id = 2,
    .minimum_version_id = 1,
    .post_load = port_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, RX113PortState, 0x200),
        VMSTATE_UINT8_ARRAY(external, RX113PortState, RX113_PORT_COUNT),
        VMSTATE_UINT8_ARRAY(external_mask, RX113PortState, RX113_PORT_COUNT),
        VMSTATE_BOOL_V(ctsu_fixed, RX113PortState, 2),
        VMSTATE_BOOL_V(ctsu_level, RX113PortState, 2),
        VMSTATE_END_OF_LIST()
    },
};

static const Property port_properties[] = {
    DEFINE_PROP_UINT32("package-pins", RX113PortState, package_pins, 100),
};

static void port_realize(DeviceState *dev, Error **errp)
{
    RX113PortState *s = RX113_PORT(dev);

    if (s->package_pins != 64 && s->package_pins != 100) {
        error_setg(errp, "rx113-port: package-pins must be 64 or 100");
    }
}

static void port_init(Object *obj)
{
    RX113PortState *s = RX113_PORT(obj);
    DeviceState *dev = DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &port_ops, s, TYPE_RX113_PORT,
                          sizeof(s->regs));
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    qdev_init_gpio_out_named(dev, &s->cacref, "cacref", 1);
    qdev_init_gpio_in_named(dev, port_input, "pin", RX113_PORT_PINS);
    qdev_init_gpio_out_named(dev, s->pin_out, "pin-out", RX113_PORT_PINS);
    qdev_init_gpio_out_named(dev, s->pin_oe, "pin-oe", RX113_PORT_PINS);
    qdev_init_gpio_out_named(dev, s->irq, "irq", 8);
    object_property_add_str(obj, "levels", port_levels, NULL);
    object_property_add_str(obj, "driving", port_driving, NULL);
}

static void port_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = port_realize;
    dc->vmsd = &vmstate_port;
    dc->user_creatable = false;
    device_class_set_props(dc, port_properties);
    rc->phases.hold = port_reset_hold;
}

static const TypeInfo port_info = {
    .name = TYPE_RX113_PORT,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RX113PortState),
    .instance_init = port_init,
    .class_init = port_class_init,
};

static void port_register_types(void)
{
    type_register_static(&port_info);
}

type_init(port_register_types)
