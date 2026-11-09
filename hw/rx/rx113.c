/*
 * Renesas RX113 MCU skeleton
 * Based on the RX62N model, Copyright (c) 2019 Yoshinori Sato.
 * Datasheet: R01UH0448EJ0120 Rev.1.20
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/units.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "migration/vmstate.h"
#include "hw/rx/rx113.h"
#include "hw/rx/rx113_lcdc.h"
#include "hw/rx/rx113_rtc.h"
#include "hw/misc/unimp.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
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
#define RX113_LCDC_BASE 0x000a0800
#define RX113_RTC_BASE  0x0008c400

/*
 * RX113 Peripheral IRQ
 * See users manual section 14
 */
#define RX113_TMR_IRQ   174
#define RX113_CMT_IRQ   28
#define RTC_CUP         63
#define RTC_ALM         92
#define RTC_PRD         93

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
 * Minimal RX113 SYSTEM startup registers, R01UH0448EJ0120 Rev.1.20.
 * Clock selections and module-stop bits are retained for readback only:
 * peripheral clocks remain fixed and module-stop does not gate devices.
 */
#define TYPE_RX113_SYSTEM "rx113-system"
OBJECT_DECLARE_SIMPLE_TYPE(RX113SystemState, RX113_SYSTEM)

/* Indices also define the migration array layout. */
enum {
    SYS_MSTPCRB, SYS_SCKCR, SYS_SCKCR3, SYS_PLLCR, SYS_PLLCR2,
    SYS_UPLLCR, SYS_UPLLCR2, SYS_MOSCCR, SYS_SOSCCR, SYS_LOCOCR,
    SYS_HOCOCR, SYS_MOSCWTCR, SYS_HOCOWTCR, SYS_PRCR, SYS_MOFCR,
    SYS_REG_COUNT,
};

static const struct {
    hwaddr offset;
    unsigned size;
    uint32_t reset;
    uint32_t mask;
    unsigned protect;
} system_regs[SYS_REG_COUNT] = {
    /* §11.2.3: reserved bits read as one; PRC1 protects module stop. */
    [SYS_MSTPCRB] = { 0x14, 4, 0xffffffff, 0xe6aa0650, 2 },
    /* §9.2.1-6: clock selections; PRC0 protects these registers. */
    [SYS_SCKCR] = { 0x20, 4, 0x33000303, 0xff000f0f, 1 },
    [SYS_SCKCR3] = { 0x26, 2, 0, 0x700, 1 },
    [SYS_PLLCR] = { 0x28, 2, 0x0f00, 0x3f03, 1 },
    [SYS_PLLCR2] = { 0x2a, 1, 1, 1, 1 },
    [SYS_UPLLCR] = { 0x2c, 2, 0x0f00, 0x3f13, 1 },
    [SYS_UPLLCR2] = { 0x2e, 1, 1, 1, 1 },
    /* §9.2.7-11: stop controls; HOCO reset depends on OFS1.HOCOEN. */
    [SYS_MOSCCR] = { 0x32, 1, 1, 1, 1 },
    [SYS_SOSCCR] = { 0x33, 1, 0, 1, 1 },
    [SYS_LOCOCR] = { 0x34, 1, 0, 1, 1 },
    [SYS_HOCOCR] = { 0x36, 1, 1, 1, 1 },
    /* §9.2.17-18: LOCO-based oscillator wait settings. */
    [SYS_MOSCWTCR] = { 0xa2, 1, 4, 0x1f, 2 },
    [SYS_HOCOWTCR] = { 0xa5, 1, 5, 0x1f, 4 },
    /* §12.1.1: key not retained, reset protection enabled. */
    [SYS_PRCR] = { 0x3fe, 2, 0, 0xf, 0 },
    /* §9.2.20: main oscillator source/drive control; PRC1. */
    [SYS_MOFCR] = { 0xc293, 1, 0, 0x60, 2 },
};

enum {
    OSC_MAIN, OSC_HOCO, OSC_PLL, OSC_UPLL, OSC_COUNT,
};

static const unsigned oscillator_stop[OSC_COUNT] = {
    SYS_MOSCCR, SYS_HOCOCR, SYS_PLLCR2, SYS_UPLLCR2,
};

static const uint8_t oscillator_flag[OSC_COUNT] = { 1, 8, 4, 0x20 };

struct RX113SystemState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    MemoryRegion status;
    MemoryRegion *flash;
    uint32_t regs[SYS_REG_COUNT];
    uint8_t oscovfsr;
    int64_t deadline[OSC_COUNT];
    QEMUTimer *osc_timer;
    qemu_irq subclock;
};

/* Nominal LOCO is 4 MHz, even when LOCOCR.LOSTP is set (§9.2.17-18). */
static int64_t rx113_system_wait_ns(RX113SystemState *s, unsigned osc)
{
    static const unsigned main_cycles[] = {
        2, 1024, 2048, 4096, 8192, 16384, 32768, 65536,
    };
    unsigned wait;

    switch (osc) {
    case OSC_MAIN:
        wait = s->regs[SYS_MOSCWTCR];
        if (wait < ARRAY_SIZE(main_cycles)) {
            return main_cycles[wait] * 250LL;
        }
        break;
    case OSC_HOCO:
        wait = s->regs[SYS_HOCOWTCR];
        if (wait == 5 || wait == 6) {
            return (wait == 5 ? 138 : 266) * 250LL;
        }
        break;
    default:
        /* Table 43.22: tPLL maximum is 50 us for both PLLs. */
        return 50000;
    }
    qemu_log_mask(LOG_GUEST_ERROR, "%s: prohibited oscillator wait setting\n",
                  TYPE_RX113_SYSTEM);
    /* Prohibited encodings never report stability; stop/restart recovers. */
    return -1;
}

static void rx113_system_update_oscillators(void *opaque)
{
    RX113SystemState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t next = INT64_MAX;
    unsigned i;

    timer_del(s->osc_timer);
    /* Main precedes PLLs, which start their tPLL wait only after MOOVF. */
    for (i = 0; i < OSC_COUNT; i++) {
        uint8_t flag = oscillator_flag[i];

        if ((s->regs[oscillator_stop[i]] & 1) ||
            (i >= OSC_PLL && !(s->oscovfsr & 1))) {
            s->oscovfsr &= ~flag;
            s->deadline[i] = -1;
        } else if (!(s->oscovfsr & flag)) {
            if (s->deadline[i] == -1) {
                int64_t wait = rx113_system_wait_ns(s, i);

                /* -2 denotes a prohibited wait, latched until stop. */
                s->deadline[i] = wait < 0 ? -2 : now + wait;
            }
            if (s->deadline[i] >= 0) {
                if (s->deadline[i] <= now) {
                    s->oscovfsr |= flag;
                    s->deadline[i] = -1;
                } else {
                    next = MIN(next, s->deadline[i]);
                }
            }
        }
    }
    if (next != INT64_MAX) {
        timer_mod(s->osc_timer, next);
    }
}

static uint64_t rx113_system_read(void *opaque, hwaddr addr, unsigned size)
{
    RX113SystemState *s = opaque;
    unsigned i;

    if (addr == 0x3c && size == 1) {
        return s->oscovfsr;
    }
    for (i = 0; i < SYS_REG_COUNT; i++) {
        if (addr == system_regs[i].offset && size == system_regs[i].size) {
            return s->regs[i];
        }
    }
    qemu_log_mask(LOG_UNIMP, "%s: unimplemented read at 0x%" HWADDR_PRIx
                  " size %u\n", TYPE_RX113_SYSTEM, addr, size);
    return 0;
}

static void rx113_system_write(void *opaque, hwaddr addr, uint64_t value,
                               unsigned size)
{
    RX113SystemState *s = opaque;
    unsigned i;

    if (addr == 0x3c && size == 1) {
        return; /* §9.2.12: all oscillator status bits are read-only. */
    }
    for (i = 0; i < SYS_REG_COUNT; i++) {
        if (addr != system_regs[i].offset || size != system_regs[i].size) {
            continue;
        }
        if (i == SYS_PRCR) {
            /* §12.1.1: only a 16-bit write with PRKEY=A5 is accepted. */
            if ((value & 0xff00) == 0xa500) {
                s->regs[i] = value & 0xf;
            }
        } else if (s->regs[SYS_PRCR] & system_regs[i].protect) {
            uint32_t mask = system_regs[i].mask;

            s->regs[i] = (value & mask) | (system_regs[i].reset & ~mask);
            rx113_system_update_oscillators(s);
            if (i == SYS_SOSCCR) {
                qemu_set_irq(s->subclock, !(s->regs[SYS_SOSCCR] & 1));
            }
        }
        return;
    }
    qemu_log_mask(LOG_UNIMP, "%s: unimplemented write at 0x%" HWADDR_PRIx
                  " size %u value 0x%" PRIx64 "\n",
                  TYPE_RX113_SYSTEM, addr, size, value);
}

static uint64_t rx113_system_status_read(void *opaque, hwaddr addr,
                                         unsigned size)
{
    return rx113_system_read(opaque, 0xc290 + addr, size);
}

static void rx113_system_status_write(void *opaque, hwaddr addr, uint64_t value,
                                      unsigned size)
{
    rx113_system_write(opaque, 0xc290 + addr, value, size);
}

static const MemoryRegionOps rx113_system_ops = {
    .read = rx113_system_read,
    .write = rx113_system_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static const MemoryRegionOps rx113_system_status_ops = {
    .read = rx113_system_status_read,
    .write = rx113_system_status_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void rx113_system_reset_hold(Object *obj, ResetType type)
{
    RX113SystemState *s = RX113_SYSTEM(obj);
    unsigned i;

    timer_del(s->osc_timer);
    s->oscovfsr = 0;
    for (i = 0; i < OSC_COUNT; i++) {
        s->deadline[i] = -1;
    }
    for (i = 0; i < SYS_REG_COUNT; i++) {
        s->regs[i] = system_regs[i].reset;
    }
}

static void rx113_system_reset_exit(Object *obj, ResetType type)
{
    RX113SystemState *s = RX113_SYSTEM(obj);
    const uint8_t *flash = memory_region_get_ram_ptr(s->flash);
    uint64_t flash_size = memory_region_size(s->flash);

    /* Read option bytes after the ROM loader's reset hold phase. */
    /* §7.2.2, §9.2.11: OFS1 bit 8 enables HOCO at reset when zero. */
    s->regs[SYS_HOCOCR] = (ldl_le_p(flash + flash_size - 0x78) >> 8) & 1;
    /* §9.2.18 note 2: hardware secures tHOCO before CPU reset release. */
    s->oscovfsr = s->regs[SYS_HOCOCR] ? 0 : 8;
    qemu_set_irq(s->subclock, !(s->regs[SYS_SOSCCR] & 1));
}

static void rx113_system_init(Object *obj)
{
    RX113SystemState *s = RX113_SYSTEM(obj);

    qdev_init_gpio_out_named(DEVICE(obj), &s->subclock, "subclock", 1);
    s->osc_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                rx113_system_update_oscillators, s);
    memory_region_init_io(&s->iomem, obj, &rx113_system_ops, s,
                          TYPE_RX113_SYSTEM, 0x400);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    memory_region_init_io(&s->status, obj, &rx113_system_status_ops, s,
                          "rx113-system-status", 0x10);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->status);
}

static void rx113_system_finalize(Object *obj)
{
    RX113SystemState *s = RX113_SYSTEM(obj);

    timer_free(s->osc_timer);
}

static int rx113_system_post_load(void *opaque, int version_id)
{
    RX113SystemState *s = opaque;
    unsigned i;

    if (version_id == 1) {
        /* Version 1 reported enabled oscillators as immediately stable. */
        s->oscovfsr = 0;
        timer_del(s->osc_timer);
        for (i = 0; i < OSC_COUNT; i++) {
            s->deadline[i] = -1;
            if (!(s->regs[oscillator_stop[i]] & 1) &&
                (i < OSC_PLL || (s->oscovfsr & 1))) {
                s->oscovfsr |= oscillator_flag[i];
            }
        }
    }
    qemu_set_irq(s->subclock, !(s->regs[SYS_SOSCCR] & 1));
    return 0;
}

static const VMStateDescription vmstate_rx113_system = {
    .name = TYPE_RX113_SYSTEM,
    .version_id = 2,
    .minimum_version_id = 1,
    .post_load = rx113_system_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, RX113SystemState, SYS_REG_COUNT),
        VMSTATE_UINT8_V(oscovfsr, RX113SystemState, 2),
        VMSTATE_INT64_ARRAY_V(deadline, RX113SystemState, OSC_COUNT, 2),
        VMSTATE_TIMER_PTR_V(osc_timer, RX113SystemState, 2),
        VMSTATE_END_OF_LIST()
    },
};

static void rx113_system_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->vmsd = &vmstate_rx113_system;
    rc->phases.hold = rx113_system_reset_hold;
    rc->phases.exit = rx113_system_reset_exit;
    dc->user_creatable = false;
}

static void register_system(RX113State *s)
{
    DeviceState *dev = qdev_new(TYPE_RX113_SYSTEM);
    RX113SystemState *sys = RX113_SYSTEM(dev);

    object_property_add_child(OBJECT(s), "system", OBJECT(dev));
    sys->flash = &s->c_flash;
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_abort);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, 0x00080000);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 1, 0x0008c290);
}

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

/* Section 34; the LCDC defines no interrupt request (Table 14.3). */
static DeviceState *register_lcdc(RX113State *s)
{
    DeviceState *dev = qdev_new(TYPE_RX113_LCDC);

    object_property_add_child(OBJECT(s), "lcdc", OBJECT(dev));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_abort);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, RX113_LCDC_BASE);
    return dev;
}

/*
 * Section 24; Table 14.3 gives CUP vector 63, ALM vector 92 and PRD vector
 * 93. The PRD also drives the LCDC blinking input (34.4.2).
 */
static void register_rtc(RX113State *s, DeviceState *lcdc)
{
    DeviceState *dev = qdev_new(TYPE_RX113_RTC);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    DeviceState *sys = DEVICE(object_resolve_path_component(OBJECT(s),
                                                           "system"));

    object_property_add_child(OBJECT(s), "rtc", OBJECT(dev));
    qdev_connect_gpio_out_named(sys, "subclock", 0,
                               qdev_get_gpio_in_named(dev,
                                                      "system-subclock", 0));
    sysbus_realize_and_unref(sbd, &error_abort);
    sysbus_mmio_map(sbd, 0, RX113_RTC_BASE);
    sysbus_connect_irq(sbd, RX113_RTC_IRQ_ALM,
                       qdev_get_gpio_in(DEVICE(&s->icu), RTC_ALM));
    sysbus_connect_irq(sbd, RX113_RTC_IRQ_PRD,
                       qdev_get_gpio_in(DEVICE(&s->icu), RTC_PRD));
    sysbus_connect_irq(sbd, RX113_RTC_IRQ_CUP,
                       qdev_get_gpio_in(DEVICE(&s->icu), RTC_CUP));
    sysbus_connect_irq(sbd, RX113_RTC_IRQ_PRD_LCDC,
                       qdev_get_gpio_in_named(lcdc, "prd", 0));
}

/* Table 5.1 register blocks; these stubs log accesses with -d unimp. */
static void register_stubs(void)
{
    static const struct {
        const char *name;
        hwaddr base;
        hwaddr size;
    } stubs[] = {
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
        { "rx113-usb0", 0x000a0000, 0x200 },
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
    object_initialize_child(OBJECT(s), "cpu", &s->cpu, TYPE_RX113_CPU);
    qdev_realize(DEVICE(&s->cpu), NULL, &error_abort);

    register_system(s);
    register_icu(s);
    s->cpu.env.ack = qdev_get_gpio_in_named(DEVICE(&s->icu), "ack", 0);
    register_tmr(s, 0);
    register_tmr(s, 1);
    register_cmt(s, 0);
    register_cmt(s, 1);
    for (unsigned i = 0; i < ARRAY_SIZE(sci_channels); i++) {
        register_sci(s, i);
    }
    register_rtc(s, register_lcdc(s));
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

/* R01UH0448EJ0120 Rev.1.20, Table 1.3: ROM, RAM and E2 DataFlash. */
static void r5f51135_class_init(ObjectClass *oc, const void *data)
{
    RX113Class *rxc = RX113_MCU_CLASS(oc);

    rxc->ram_size = 32 * KiB;
    rxc->rom_flash_size = 128 * KiB;
    rxc->data_flash_size = 8 * KiB;
}

/* R01UH0448EJ0120 Rev.1.20, Table 1.3: ROM, RAM and E2 DataFlash. */
static void r5f51136_class_init(ObjectClass *oc, const void *data)
{
    RX113Class *rxc = RX113_MCU_CLASS(oc);

    rxc->ram_size = 32 * KiB;
    rxc->rom_flash_size = 256 * KiB;
    rxc->data_flash_size = 8 * KiB;
}

/* R01UH0448EJ0120 Rev.1.20, Table 1.3: ROM, RAM and E2 DataFlash. */
static void r5f51137_class_init(ObjectClass *oc, const void *data)
{
    RX113Class *rxc = RX113_MCU_CLASS(oc);

    rxc->ram_size = 64 * KiB;
    rxc->rom_flash_size = 384 * KiB;
    rxc->data_flash_size = 8 * KiB;
}

/* R01UH0448EJ0120 Rev.1.20, Table 1.3: ROM, RAM and E2 DataFlash. */
static void r5f51138_class_init(ObjectClass *oc, const void *data)
{
    RX113Class *rxc = RX113_MCU_CLASS(oc);

    rxc->ram_size = 64 * KiB;
    rxc->rom_flash_size = 512 * KiB;
    rxc->data_flash_size = 8 * KiB;
}

static const TypeInfo rx113_types[] = {
    {
        .name = TYPE_RX113_SYSTEM,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(RX113SystemState),
        .instance_init = rx113_system_init,
        .instance_finalize = rx113_system_finalize,
        .class_init = rx113_system_class_init,
    }, {
        .name = TYPE_R5F51135_MCU,
        .parent = TYPE_RX113_MCU,
        .class_init = r5f51135_class_init,
    }, {
        .name = TYPE_R5F51136_MCU,
        .parent = TYPE_RX113_MCU,
        .class_init = r5f51136_class_init,
    }, {
        .name = TYPE_R5F51137_MCU,
        .parent = TYPE_RX113_MCU,
        .class_init = r5f51137_class_init,
    }, {
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
