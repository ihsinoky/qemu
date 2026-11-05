/*
 * Renesas RX113 MCU skeleton
 * Based on the RX62N model, Copyright (c) 2019 Yoshinori Sato.
 * Datasheet: R01UH0448EJ0120 Rev.1.20
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/units.h"
#include "hw/rx/rx113.h"
#include "hw/misc/unimp.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "system/system.h"
#include "qobject/qlist.h"
#include "qom/object.h"

/*
 * RX113 Internal Memory
 */
#define RX113_IRAM_BASE     0x00000000
#define RX113_DFLASH_BASE   0x00100000

/*
 * RX113 Peripheral Address
 * See users manual section 5
 */
#define RX113_ICU_BASE  0x00087000
#define RX113_TMR_BASE  0x00088200
#define RX113_CMT_BASE  0x00088000

/*
 * RX113 Peripheral IRQ
 * See users manual section 14
 */
#define RX113_TMR_IRQ   174
#define RX113_CMT_IRQ   28

/* Fixed PCLKB until SYSTEM clock control is modeled. */
#define RX113_PCLKB_HZ (32 * 1000 * 1000)

struct RX113Class {
    /*< private >*/
    DeviceClass parent_class;
    /*< public >*/
    uint64_t ram_size;
    uint64_t rom_flash_size;
    uint64_t data_flash_size;
};
typedef struct RX113Class RX113Class;

DECLARE_CLASS_CHECKERS(RX113Class, RX113_MCU,
                       TYPE_RX113_MCU)

/*
 * R01UH0448EJ0120 Rev.1.20, Table 14.3 (pp.266-272).
 * Vector -> IPR number transcribed from the table; blank merged
 * IPR cells inherit the preceding IPR. 0xff denotes an unassigned vector.
 */
static const uint8_t ipr_table[NR_IRQS] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, /* 7 */
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, /* 15 */
    0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x02, /* 23 */
    0xff, 0xff, 0xff, 0x03, 0x04, 0x05, 0x06, 0x07, /* 31 */
    0x20, 0x21, 0x22, 0xff, 0x24, 0x25, 0x26, 0xff, /* 39 */
    0xff, 0xff, 0xff, 0xff, 0x2c, 0x2c, 0x2c, 0x2c, /* 47 */
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, /* 55 */
    0xff, 0x39, 0x3a, 0x3b, 0x3c, 0x3c, 0x3c, 0x3f, /* 63 */
    0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, /* 71 */
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, /* 79 */
    0x50, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, /* 87 */
    0x58, 0x59, 0x5a, 0xff, 0x5c, 0x5d, 0xff, 0xff, /* 95 */
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x66, 0x67, /* 103 */
    0xff, 0xff, 0x6a, 0xff, 0x6c, 0x6c, 0x6c, 0xff, /* 111 */
    0xff, 0xff, 0x72, 0x72, 0x72, 0x72, 0x76, 0x76, /* 119 */
    0x76, 0x79, 0x79, 0x7b, 0x7b, 0x7d, 0x7d, 0x7f, /* 127 */
    0x7f, 0x81, 0x81, 0x81, 0x81, 0x85, 0x86, 0x86, /* 135 */
    0x86, 0x86, 0x8a, 0x8b, 0x8b, 0x8b, 0xff, 0xff, /* 143 */
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, /* 151 */
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, /* 159 */
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, /* 167 */
    0xff, 0xff, 0xaa, 0xab, 0xff, 0xff, 0xae, 0xae, /* 175 */
    0xae, 0xb1, 0xb1, 0xb1, 0xb4, 0xb4, 0xb4, 0xb7, /* 183 */
    0xb7, 0xb7, 0xba, 0xba, 0xba, 0xba, 0xff, 0xff, /* 191 */
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, /* 199 */
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, /* 207 */
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xd6, 0xd6, /* 215 */
    0xd6, 0xd6, 0xda, 0xda, 0xda, 0xda, 0xde, 0xde, /* 223 */
    0xde, 0xde, 0xe2, 0xe2, 0xe2, 0xe2, 0xe6, 0xe6, /* 231 */
    0xe6, 0xe6, 0xea, 0xea, 0xea, 0xea, 0xee, 0xee, /* 239 */
    0xee, 0xee, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, /* 247 */
    0xf8, 0xf9, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, /* 255 */
};

/*
 * Table 14.3 Level and Edge/Level rows. External IRQ0-7 default to level;
 * the ICU's IRQCR registers select their detection mode at runtime.
 */
static const uint8_t levelirq[] = {
    16, 32, 33, 34, 44, 47, 57, 64, 65, 66,
    67, 68, 69, 70, 71, 90, 108, 170, 171, 186,
    189, 214, 217, 218, 221, 222, 225, 226, 229, 230,
    233, 234, 237, 238, 241, 242, 243, 244, 245, 246,
    249,
};

static void register_icu(RX113State *s)
{
    unsigned i;
    SysBusDevice *icu;
    QList *ipr_map, *trigger_level;

    object_initialize_child(OBJECT(s), "icu", &s->icu, TYPE_RX_ICU);
    icu = SYS_BUS_DEVICE(&s->icu);

    qdev_prop_set_uint32(DEVICE(icu), "ipr-count", 256);
    ipr_map = qlist_new();
    for (i = 0; i < NR_IRQS; i++) {
        qlist_append_int(ipr_map, ipr_table[i]);
    }
    qdev_prop_set_array(DEVICE(icu), "ipr-map", ipr_map);

    trigger_level = qlist_new();
    for (i = 0; i < ARRAY_SIZE(levelirq); i++) {
        qlist_append_int(trigger_level, levelirq[i]);
    }
    qdev_prop_set_array(DEVICE(icu), "trigger-level", trigger_level);
    sysbus_realize(icu, &error_abort);

    sysbus_connect_irq(icu, 0, qdev_get_gpio_in(DEVICE(&s->cpu), RX_CPU_IRQ));
    sysbus_connect_irq(icu, 1, qdev_get_gpio_in(DEVICE(&s->cpu), RX_CPU_FIR));
    sysbus_connect_irq(icu, 2, qdev_get_gpio_in(DEVICE(&s->icu), SWI));
    sysbus_mmio_map(icu, 0, RX113_ICU_BASE);
}

static void register_tmr(RX113State *s, unsigned unit)
{
    SysBusDevice *tmr;
    unsigned i, irqbase;

    object_initialize_child(OBJECT(s), "tmr[*]",
                            &s->tmr[unit], TYPE_RENESAS_TMR);
    tmr = SYS_BUS_DEVICE(&s->tmr[unit]);
    qdev_prop_set_uint64(DEVICE(tmr), "input-freq", RX113_PCLKB_HZ);
    sysbus_realize(tmr, &error_abort);

    irqbase = RX113_TMR_IRQ + TMR_NR_IRQ * unit;
    for (i = 0; i < TMR_NR_IRQ; i++) {
        sysbus_connect_irq(tmr, i,
                           qdev_get_gpio_in(DEVICE(&s->icu), irqbase + i));
    }
    sysbus_mmio_map(tmr, 0, RX113_TMR_BASE + unit * 0x10);
}

static void register_cmt(RX113State *s, unsigned unit)
{
    SysBusDevice *cmt;
    unsigned i, irqbase;

    object_initialize_child(OBJECT(s), "cmt[*]",
                            &s->cmt[unit], TYPE_RENESAS_CMT);
    cmt = SYS_BUS_DEVICE(&s->cmt[unit]);
    qdev_prop_set_uint64(DEVICE(cmt), "input-freq", RX113_PCLKB_HZ);
    sysbus_realize(cmt, &error_abort);

    irqbase = RX113_CMT_IRQ + CMT_NR_IRQ * unit;
    for (i = 0; i < CMT_NR_IRQ; i++) {
        sysbus_connect_irq(cmt, i,
                           qdev_get_gpio_in(DEVICE(&s->icu), irqbase + i));
    }
    sysbus_mmio_map(cmt, 0, RX113_CMT_BASE + unit * 0x10);
}

/* Table 5.1 addresses and Table 14.3 ERI/RXI/TXI/TEI vector bases. */
static const struct {
    hwaddr base;
    unsigned irq;
    const char *extensions;
    unsigned extension_size;
} sci_channels[] = {
    { 0x0008a000, 214, "rx113-sci0-extensions", 0x18 },
    { 0x0008a020, 218, "rx113-sci1-extensions", 0x18 },
    { 0x0008a040, 186, "rx113-sci2-extensions", 0x18 },
    { 0x0008a0a0, 222, "rx113-sci5-extensions", 0x18 },
    { 0x0008a0c0, 226, "rx113-sci6-extensions", 0x18 },
    { 0x0008b300, 238, "rx113-sci12-extensions", 0x2c },
};

static void register_sci(RX113State *s, unsigned unit)
{
    SysBusDevice *sci;
    unsigned i;

    object_initialize_child(OBJECT(s), "sci[*]",
                            &s->sci[unit], TYPE_RENESAS_SCI);
    sci = SYS_BUS_DEVICE(&s->sci[unit]);
    /* Serial backends are ordered SCI0, SCI1, SCI2, SCI5, SCI6, SCI12. */
    qdev_prop_set_chr(DEVICE(sci), "chardev", serial_hd(unit));
    qdev_prop_set_uint64(DEVICE(sci), "input-freq", RX113_PCLKB_HZ);
    sysbus_realize(sci, &error_abort);

    for (i = 0; i < SCI_NR_IRQ; i++) {
        sysbus_connect_irq(sci, i,
                          qdev_get_gpio_in(DEVICE(&s->icu),
                                           sci_channels[unit].irq + i));
    }
    sysbus_mmio_map(sci, 0, sci_channels[unit].base);
    create_unimplemented_device(sci_channels[unit].extensions,
                                sci_channels[unit].base + 8,
                                sci_channels[unit].extension_size);
}

/* Table 5.1 register blocks; these stubs log accesses with -d unimp. */
static void register_stubs(void)
{
    static const struct {
        const char *name;
        hwaddr base;
        hwaddr size;
    } stubs[] = {
        { "rx113-system", 0x00080000, 0x400 },
        { "rx113-dtc", 0x00082400, 0x10 },
        { "rx113-iwdt", 0x00088030, 0x10 },
        { "rx113-dac", 0x00088040, 0x10 },
        { "rx113-mtu", 0x00088600, 0x300 },
        { "rx113-s12ad", 0x00089000, 0x100 },
        { "rx113-sci8", 0x0008a100, 0x20 },
        { "rx113-sci9", 0x0008a120, 0x20 },
        { "rx113-cac", 0x0008b000, 0x20 },
        { "rx113-doc", 0x0008b080, 0x10 },
        { "rx113-elc", 0x0008b100, 0x30 },
        { "rx113-port", 0x0008c000, 0x100 },
        { "rx113-mpc", 0x0008c100, 0x100 },
        { "rx113-system-status", 0x0008c290, 0x10 },
        { "rx113-rtc", 0x0008c400, 0x100 },
        { "rx113-usb0", 0x000a0000, 0x200 },
        { "rx113-lcdc", 0x000a0800, 0x100 },
        { "rx113-ctsu", 0x000a0900, 0x100 },
    };
    unsigned i;

    for (i = 0; i < ARRAY_SIZE(stubs); i++) {
        create_unimplemented_device(stubs[i].name, stubs[i].base,
                                    stubs[i].size);
    }
}

static void rx113_realize(DeviceState *dev, Error **errp)
{
    RX113State *s = RX113_MCU(dev);
    RX113Class *rxc = RX113_MCU_GET_CLASS(dev);

    memory_region_init_ram(&s->iram, OBJECT(dev), "iram",
                           rxc->ram_size, &error_abort);
    memory_region_add_subregion(s->sysmem, RX113_IRAM_BASE, &s->iram);
    memory_region_init_rom(&s->d_flash, OBJECT(dev), "flash-data",
                           rxc->data_flash_size, &error_abort);
    memory_region_add_subregion(s->sysmem, RX113_DFLASH_BASE, &s->d_flash);
    memory_region_init_rom(&s->c_flash, OBJECT(dev), "flash-code",
                           rxc->rom_flash_size, &error_abort);
    memory_region_add_subregion(s->sysmem, (1ULL << 32) - rxc->rom_flash_size,
                                &s->c_flash);

    /* Initialize CPU */
    object_initialize_child(OBJECT(s), "cpu", &s->cpu, TYPE_RX62N_CPU);
    qdev_realize(DEVICE(&s->cpu), NULL, &error_abort);

    register_icu(s);
    s->cpu.env.ack = qdev_get_gpio_in_named(DEVICE(&s->icu), "ack", 0);
    register_tmr(s, 0);
    register_tmr(s, 1);
    register_cmt(s, 0);
    register_cmt(s, 1);
    for (unsigned i = 0; i < ARRAY_SIZE(sci_channels); i++) {
        register_sci(s, i);
    }
    register_stubs();
}

static const Property rx113_properties[] = {
    DEFINE_PROP_LINK("main-bus", RX113State, sysmem, TYPE_MEMORY_REGION,
                     MemoryRegion *),
};

static void rx113_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = rx113_realize;
    device_class_set_props(dc, rx113_properties);
}

static void r5f51138_class_init(ObjectClass *oc, const void *data)
{
    RX113Class *rxc = RX113_MCU_CLASS(oc);

    rxc->ram_size = 64 * KiB;
    rxc->rom_flash_size = 512 * KiB;
    rxc->data_flash_size = 8 * KiB;
}

static const TypeInfo rx113_types[] = {
    {
        .name = TYPE_R5F51138_MCU,
        .parent = TYPE_RX113_MCU,
        .class_init = r5f51138_class_init,
    }, {
        .name = TYPE_RX113_MCU,
        .parent = TYPE_DEVICE,
        .instance_size = sizeof(RX113State),
        .class_size = sizeof(RX113Class),
        .class_init = rx113_class_init,
        .abstract = true,
    }
};

DEFINE_TYPES(rx113_types)
