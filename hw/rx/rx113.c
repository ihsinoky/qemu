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
#include "hw/rx/rx113_port.h"
#include "hw/rx/rx113_iwdt.h"
#include "hw/rx/rx113_mtu.h"
#include "hw/rx/rx113_poe.h"
#include "hw/rx/rx113_s12ad.h"
#include "hw/rx/rx113_dac.h"
#include "hw/rx/rx113_dtc.h"
#include "hw/rx/rx113_elc.h"
#include "hw/rx/rx113_doc.h"
#include "hw/rx/rx113_cac.h"
#include "hw/rx/rx113_ctsu.h"
#include "hw/intc/rx_icu.h"
#include "hw/misc/unimp.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/split-irq.h"
#include "hw/core/qdev-properties.h"
#include "system/system.h"
#include "qobject/qlist.h"
#include "qom/object.h"
#include "hw/rx/rx113_usb.h"

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

/* Initial realization property; SYSTEM supplies the live clock at reset. */
#define RX113_PCLKB_HZ (32 * 1000 * 1000)
#define RX113_LOCO_HZ 4000000
#define RX113_HOCO_HZ 32000000
#define RX113_SUB_HZ 32768

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
 * PCLKB and module-stop updates use the shared peripherals' opt-in APIs.
 */
#define TYPE_RX113_SYSTEM "rx113-system"
OBJECT_DECLARE_SIMPLE_TYPE(RX113SystemState, RX113_SYSTEM)

/* Indices also define the migration array layout. */
enum {
    SYS_MSTPCRB, SYS_SCKCR, SYS_SCKCR3, SYS_PLLCR, SYS_PLLCR2,
    SYS_UPLLCR, SYS_UPLLCR2, SYS_MOSCCR, SYS_SOSCCR, SYS_LOCOCR,
    SYS_HOCOCR, SYS_MOSCWTCR, SYS_HOCOWTCR, SYS_PRCR, SYS_MOFCR,
    SYS_MSTPCRA, SYS_MSTPCRC, SYS_MSTPCRD,
    SYS_LCDSCLKCR, SYS_LCDSCLKCR2, SYS_REG_COUNT,
};

static const struct {
    hwaddr offset;
    unsigned size;
    uint32_t reset;
    uint32_t mask;
    unsigned protect;
} system_regs[SYS_REG_COUNT] = {
    /* §11.2.2-4: reserved bits read as one; PRC1 protects module stop. */
    [SYS_MSTPCRA] = { 0x10, 4, 0xefffffff, 0x1006c230, 2 },
    [SYS_MSTPCRB] = { 0x14, 4, 0xffffffff, 0xe6aa0650, 2 },
    [SYS_MSTPCRC] = { 0x18, 4, 0x7ffffffe, 0x8c180001, 2 },
    [SYS_MSTPCRD] = { 0x1c, 4, 0xffffffff, 0x8c00, 2 },
    /* §9.2.1-6: clock selections; PRC0 protects these registers. */
    [SYS_SCKCR] = { 0x20, 4, 0x33000303, 0x0f0f0f0f, 1 },
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
    /* §9.2.15-16: LCD source clock selection and stop; PRC0. */
    [SYS_LCDSCLKCR] = { 0x50, 1, 0, 0x7, 1 },
    [SYS_LCDSCLKCR2] = { 0x51, 1, 1, 0x1, 1 },
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
    RX113State *mcu;
    uint64_t main_clock_hz;
    bool rtc_enabled;
    int64_t iwdt_ready_ns;
    uint8_t ostdcr, ostdsr;
    bool ostdf_seen, main_failed, ostdi_request;
    QEMUTimer *failure_timer;
    qemu_irq ostdi;
};

/* Table 5.1 addresses and Table 14.3 ERI/RXI/TXI/TEI vector bases. */
static const struct {
    hwaddr base;
    unsigned irq;
    unsigned stop_reg;
    unsigned stop_bit;
    const char *extensions;
    unsigned extension_size;
} sci_channels[] = {
    { 0x0008a000, 214, SYS_MSTPCRB, 31, "rx113-sci0-extensions", 0x18 },
    { 0x0008a020, 218, SYS_MSTPCRB, 30, "rx113-sci1-extensions", 0x18 },
    { 0x0008a040, 186, SYS_MSTPCRB, 29, "rx113-sci2-extensions", 0x18 },
    { 0x0008a0a0, 222, SYS_MSTPCRB, 26, "rx113-sci5-extensions", 0x18 },
    { 0x0008a0c0, 226, SYS_MSTPCRB, 25, "rx113-sci6-extensions", 0x18 },
    { 0x0008b300, 238, SYS_MSTPCRB, 4, "rx113-sci12-extensions", 0x2c },
    { 0x0008a100, 230, SYS_MSTPCRC, 27, "rx113-sci8-extensions", 0x18 },
    { 0x0008a120, 234, SYS_MSTPCRC, 26, "rx113-sci9-extensions", 0x18 },
};

/* §9.2.1-3, §9.7.2: UPLL feeds UCLK and never PCLKB. */
static void rx113_system_update_clocks(RX113SystemState *s)
{
    uint64_t source = 0, pclkb = s->mcu->cmt[0].input_freq;
    uint64_t iclk = s->mcu->cmt[0].input_freq;
    uint64_t pclkd = 0;
    unsigned adiv = extract32(s->regs[SYS_SCKCR], 0, 4);
    unsigned div = extract32(s->regs[SYS_SCKCR], 8, 4);
    unsigned ick = extract32(s->regs[SYS_SCKCR], 16, 4);
    unsigned select = extract32(s->regs[SYS_SCKCR3], 8, 3);
    unsigned i;

    switch (select) {
    case 0:
        source = RX113_LOCO_HZ;
        break;
    case 1:
        source = RX113_HOCO_HZ;
        break;
    case 2:
        source = s->ostdsr ? RX113_LOCO_HZ : s->main_clock_hz;
        break;
    case 3:
        source = RX113_SUB_HZ;
        break;
    case 4: {
        unsigned plidiv = extract32(s->regs[SYS_PLLCR], 0, 2);
        unsigned stc = extract32(s->regs[SYS_PLLCR], 8, 6);

        if (plidiv > 2 || (stc != 0x0b && stc != 0x0f)) {
            qemu_log_mask(LOG_GUEST_ERROR, "rx113: prohibited PLL setting\n");
            goto gates;
        }
        /* Table 43.21: nominal PLL free-running frequency is 8 MHz. */
        source = s->ostdsr ? 8000000 :
                 (s->main_clock_hz >> plidiv) * (stc == 0x0b ? 6 : 8);
        break;
    }
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "rx113: prohibited clock source\n");
        goto gates;
    }
    if (div > 6) {
        qemu_log_mask(LOG_GUEST_ERROR, "rx113: prohibited PCLKB divider\n");
        goto gates;
    }
    pclkb = source >> div;
    if (ick > 6) {
        qemu_log_mask(LOG_GUEST_ERROR, "rx113: prohibited ICLK divider\n");
        goto gates;
    }
    iclk = source >> ick;
    if (adiv <= 6) {
        pclkd = source >> adiv;
    } else {
        qemu_log_mask(LOG_GUEST_ERROR, "rx113: prohibited PCLKD divider\n");
    }
gates:
    {
        uint64_t uclk = source;
        unsigned select_usb = s->regs[SYS_UPLLCR];
        unsigned usb_div = select_usb & 3;
        unsigned usb_stc = (select_usb >> 8) & 0x3f;
        static const unsigned ready_bit[] = { 0, 3, 0, 0, 2 };
        bool ready = select < 5 &&
                     (select == 0 ? !(s->regs[SYS_LOCOCR] & 1) :
                      select == 3 ? !(s->regs[SYS_SOSCCR] & 1) :
                      !!(s->oscovfsr & (1U << ready_bit[select])));

        if (!ready) {
            uclk = 0;
        }
        if (select_usb & 0x10) {
            uclk = (s->oscovfsr & 0x20) && usb_div <= 1 &&
                    (usb_stc == 0x0b || usb_stc == 0x0f) ?
                    (s->main_clock_hz >> usb_div) *
                    (usb_stc == 0x0b ? 6 : 8) : 0;
        }
        rx113_usb_set_clock(s->mcu->usb, uclk,
                            s->regs[SYS_MSTPCRB] & (1U << 19));
    }
    if (s->mcu->cac) {
        static const unsigned system_source[] = { 3, 2, 0, 1 };
        uint64_t hz[6] = {
            (s->oscovfsr & 1) ?
                (s->ostdsr ? RX113_LOCO_HZ : s->main_clock_hz) : 0,
            (!(s->regs[SYS_SOSCCR] & 1) || s->rtc_enabled) ?
                RX113_SUB_HZ : 0,
            (s->oscovfsr & 8) ? RX113_HOCO_HZ : 0,
            (s->regs[SYS_LOCOCR] & 1) ? 0 : RX113_LOCO_HZ,
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) >= s->iwdt_ready_ns ?
                rx113_iwdt_clock_hz(s->mcu->iwdt) : 0,
            pclkb,
        };
        bool available = select < 4 ? hz[system_source[select]] :
                         select == 4 && (s->oscovfsr & 4);

        if (!available) {
            hz[5] = 0;
        }
        rx113_cac_set_clocks(s->mcu->cac, hz,
                             s->regs[SYS_MSTPCRC] & (1U << 19));
    }
    rx113_ctsu_set_clock(s->mcu->ctsu, pclkb,
                         s->regs[SYS_MSTPCRD] & (1U << 10));
    /* Section 14.4.7: the ICU digital filters sample on ICLK. */
    rx_icu_set_clock(DEVICE(&s->mcu->icu), iclk);
    {
        /* §9.2.15-16, §34.2.3: LCD source clock for the LCDC. */
        uint64_t lcd_hz = 0;
        unsigned lcd_source = RX113_LCD_SOURCE_MAIN;
        unsigned sel = s->regs[SYS_LCDSCLKCR] & 7;

        switch (sel) {
        case 0: /* LOCO */
            lcd_hz = (s->regs[SYS_LOCOCR] & 1) ? 0 : RX113_LOCO_HZ;
            break;
        case 1: /* HOCO */
            lcd_hz = (s->oscovfsr & 8) ? RX113_HOCO_HZ : 0;
            break;
        case 2: /* Main clock oscillator */
            lcd_hz = (s->oscovfsr & 1) ?
                     (s->ostdsr ? RX113_LOCO_HZ : s->main_clock_hz) : 0;
            break;
        case 3: /* Sub-clock oscillator */
            lcd_source = RX113_LCD_SOURCE_SUB;
            lcd_hz = (!(s->regs[SYS_SOSCCR] & 1) || s->rtc_enabled) ?
                     RX113_SUB_HZ : 0;
            break;
        case 4: /* IWDT-dedicated on-chip oscillator */
            lcd_source = RX113_LCD_SOURCE_IWDT;
            lcd_hz = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) >=
                     s->iwdt_ready_ns ?
                     rx113_iwdt_clock_hz(s->mcu->iwdt) : 0;
            break;
        default:
            qemu_log_mask(LOG_GUEST_ERROR,
                          "rx113: prohibited LCD clock source\n");
            break;
        }
        if (s->regs[SYS_LCDSCLKCR2] & 1) {
            lcd_hz = 0; /* LCDSCLKSTP stops the source clock output. */
        }
        rx113_lcdc_set_clock(s->mcu->lcdc, lcd_hz, lcd_source,
                             s->regs[SYS_MSTPCRD] & (1U << 11));
    }
    rx113_doc_set_stopped(s->mcu->doc,
                          s->regs[SYS_MSTPCRB] & (1U << 6));
    rx113_elc_set_clock(s->mcu->elc, pclkb,
                        s->regs[SYS_MSTPCRB] & (1U << 9));
    rx113_dtc_set_stopped(s->mcu->dtc,
                          s->regs[SYS_MSTPCRA] & (1U << 28));
    rx113_dac_set_clock(s->mcu->dac, pclkb,
                        s->regs[SYS_MSTPCRA] & (1U << 18));
    rx113_s12ad_set_clock(s->mcu->s12ad, pclkb, pclkd,
                          s->regs[SYS_MSTPCRA] & (1U << 17));
    rx113_poe_set_clock(s->mcu->poe, pclkb);
    rx113_mtu_set_clock(s->mcu->mtu, pclkb,
                        s->regs[SYS_MSTPCRA] & (1U << 9));
    for (i = 0; i < 2; i++) {
        renesas_cmt_set_clock(&s->mcu->cmt[i], pclkb,
                             s->regs[SYS_MSTPCRA] & (1U << (15 - i)));
        renesas_tmr_set_clock(&s->mcu->tmr[i], pclkb,
                             s->regs[SYS_MSTPCRA] & (1U << (5 - i)));
    }
    for (i = 0; i < ARRAY_SIZE(sci_channels); i++) {
        renesas_sci_set_clock(&s->mcu->sci[i], pclkb,
                             s->regs[sci_channels[i].stop_reg] &
                             (1U << sci_channels[i].stop_bit));
    }
}

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
    if (s->iwdt_ready_ns > now) {
        next = MIN(next, s->iwdt_ready_ns);
    }
    if (next != INT64_MAX) {
        timer_mod(s->osc_timer, next);
    }
    rx113_system_update_clocks(s);
}

/* Table 43.59 supplies a maximum detection time of 1 ms. */
static void rx113_system_detect_failure(void *opaque)
{
    RX113SystemState *s = opaque;

    if (!(s->ostdcr & 0x80) || !s->main_failed || s->ostdsr) {
        return;
    }
    s->ostdsr = 1;
    s->ostdf_seen = false;
    rx113_system_update_clocks(s);
    if (s->ostdcr & 1) {
        /* D-017: request at the ICU boundary; no CPU NMI delivery. */
        s->ostdi_request = true;
        qemu_set_irq(s->ostdi, 1);
        rx113_poe_oscillator_stop(s->mcu->poe);
    }
}

static void rx113_system_failure_input(void *opaque, int n, int level)
{
    RX113SystemState *s = opaque;

    s->main_failed = level;
    if (!level) {
        timer_del(s->failure_timer);
    } else if ((s->ostdcr & 0x80) && !s->ostdsr &&
               !timer_pending(s->failure_timer)) {
        timer_mod(s->failure_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000);
    }
}

static bool rx113_system_ostdi_request(Object *obj, Error **errp)
{
    return RX113_SYSTEM(obj)->ostdi_request;
}

static uint64_t rx113_system_read(void *opaque, hwaddr addr, unsigned size)
{
    RX113SystemState *s = opaque;
    unsigned i;

    if (addr == 0x40 && size == 1) {
        return s->ostdcr;
    }
    if (addr == 0x41 && size == 1) {
        s->ostdf_seen |= s->ostdsr;
        return s->ostdsr;
    }
    if (addr == 0x35 && size == 1) {
        return rx113_iwdt_ilococr(s->mcu->iwdt);
    }
    if (addr == 0xc0 && size == 1) {
        return rx113_iwdt_reset_status(s->mcu->iwdt);
    }
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

    if ((addr == 0x40 || addr == 0x41) && size == 1) {
        if (!(s->regs[SYS_PRCR] & 1)) {
            return;
        }
        if (addr == 0x40) {
            s->ostdcr = (value & 0x81) | (s->ostdsr ? 0x80 : 0);
            if (s->ostdcr & 0x80) {
                s->regs[SYS_LOCOCR] = 0;
            }
            rx113_system_failure_input(s, 0, s->main_failed);
        } else if (!(value & 1) && s->ostdf_seen && !(s->ostdcr & 1) &&
                   (s->regs[SYS_SCKCR3] & 0x700) != 0x200 &&
                   (s->regs[SYS_SCKCR3] & 0x700) != 0x400) {
            s->ostdsr = 0;
            s->ostdf_seen = false;
            s->ostdi_request = false;
            qemu_set_irq(s->ostdi, 0);
            rx113_system_failure_input(s, 0, s->main_failed);
        }
        rx113_system_update_clocks(s);
        return;
    }
    if (addr == 0x35 && size == 1) {
        if ((s->regs[SYS_PRCR] & 1) && !(value & 1)) {
            if (!rx113_iwdt_clock_hz(s->mcu->iwdt)) {
                s->iwdt_ready_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                                   50000;
            }
            rx113_iwdt_start_clock(s->mcu->iwdt);
            rx113_system_update_oscillators(s);
        }
        return;
    }
    if (addr == 0xc0 && size == 1) {
        rx113_iwdt_clear_reset_status(s->mcu->iwdt, value);
        return;
    }
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

            if (i == SYS_LOCOCR && (s->ostdcr & 0x80)) {
                value = 0;
            }

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
    timer_del(s->failure_timer);
    s->ostdcr = s->ostdsr = 0;
    s->ostdf_seen = s->ostdi_request = false;
    qemu_set_irq(s->ostdi, 0);
    s->oscovfsr = 0;
    s->iwdt_ready_ns = 0;
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
    rx113_system_update_clocks(s);
    qemu_set_irq(s->subclock, !(s->regs[SYS_SOSCCR] & 1));
}

static void rx113_system_rtc_enable(void *opaque, int n, int level)
{
    RX113SystemState *s = opaque;

    if (s->rtc_enabled == !!level) {
        return;
    }
    s->rtc_enabled = level;
    if (s->mcu->cac && !device_is_in_reset(DEVICE(s))) {
        rx113_system_update_clocks(s);
    }
}

static void rx113_system_init(Object *obj)
{
    RX113SystemState *s = RX113_SYSTEM(obj);

    qdev_init_gpio_out_named(DEVICE(obj), &s->subclock, "subclock", 1);
    qdev_init_gpio_out_named(DEVICE(obj), &s->ostdi, "ostdi", 1);
    qdev_init_gpio_in_named(DEVICE(obj), rx113_system_failure_input,
                           "main-clock-failed", 1);
    object_property_add_bool(obj, "ostdi-request", rx113_system_ostdi_request,
                             NULL);
    s->failure_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                    rx113_system_detect_failure, s);
    qdev_init_gpio_in_named(DEVICE(obj), rx113_system_rtc_enable,
                           "rtc-enable", 1);
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
    timer_free(s->failure_timer);
}

static int rx113_system_post_load(void *opaque, int version_id)
{
    RX113SystemState *s = opaque;
    unsigned i;

    if (version_id < 8) {
        s->regs[SYS_LCDSCLKCR] = system_regs[SYS_LCDSCLKCR].reset;
        s->regs[SYS_LCDSCLKCR2] = system_regs[SYS_LCDSCLKCR2].reset;
    }
    if (version_id < 6) {
        s->regs[SYS_MSTPCRD] = system_regs[SYS_MSTPCRD].reset;
    }
    if (version_id < 4) {
        s->regs[SYS_MSTPCRC] = system_regs[SYS_MSTPCRC].reset;
    }
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
    qemu_set_irq(s->ostdi, s->ostdi_request);
    return 0;
}

static const VMStateDescription vmstate_rx113_system = {
    .name = TYPE_RX113_SYSTEM,
    .version_id = 8,
    .minimum_version_id = 1,
    .post_load = rx113_system_post_load,
    .fields = (const VMStateField[]) {
        /* Preserve earlier array layouts; append new registers by version. */
        VMSTATE_UINT32_SUB_ARRAY(regs, RX113SystemState, 0, SYS_MSTPCRA),
        VMSTATE_UINT32_V(regs[SYS_MSTPCRA], RX113SystemState, 3),
        VMSTATE_UINT32_V(regs[SYS_MSTPCRC], RX113SystemState, 4),
        VMSTATE_UINT32_V(regs[SYS_MSTPCRD], RX113SystemState, 6),
        VMSTATE_UINT32_V(regs[SYS_LCDSCLKCR], RX113SystemState, 8),
        VMSTATE_UINT32_V(regs[SYS_LCDSCLKCR2], RX113SystemState, 8),
        VMSTATE_UINT8_V(ostdcr, RX113SystemState, 7),
        VMSTATE_UINT8_V(ostdsr, RX113SystemState, 7),
        VMSTATE_BOOL_V(ostdf_seen, RX113SystemState, 7),
        VMSTATE_BOOL_V(main_failed, RX113SystemState, 7),
        VMSTATE_BOOL_V(ostdi_request, RX113SystemState, 7),
        VMSTATE_TIMER_PTR_V(failure_timer, RX113SystemState, 7),
        VMSTATE_BOOL_V(rtc_enabled, RX113SystemState, 5),
        VMSTATE_INT64_V(iwdt_ready_ns, RX113SystemState, 5),
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
    sys->mcu = s;
    sys->main_clock_hz = s->main_clock_hz;
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
    qdev_prop_set_bit(DEVICE(icu), "nmi-rx113", true);
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
    sysbus_connect_irq(icu, 3, qdev_get_gpio_in(DEVICE(&s->cpu), RX_CPU_NMI));
    sysbus_mmio_map(icu, 0, RX113_ICU_BASE);
    /* Issue #58: NMI sources (D-017 left both at the request stage). */
    qdev_connect_gpio_out_named(
        DEVICE(object_resolve_path_component(OBJECT(s), "system")),
        "ostdi", 0, qdev_get_gpio_in_named(DEVICE(icu), "nmi-ostdi", 0));
    sysbus_connect_irq(SYS_BUS_DEVICE(s->iwdt), 0,
                       qdev_get_gpio_in_named(DEVICE(icu), "nmi-iwdt", 0));
}

static void register_dtc(RX113State *s)
{
    s->dtc = qdev_new(TYPE_RX113_DTC);
    object_property_add_child(OBJECT(s), "dtc", OBJECT(s->dtc));
    object_property_set_link(OBJECT(s->dtc), "icu", OBJECT(&s->icu),
                             &error_abort);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->dtc), &error_abort);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->dtc), 0, 0x00082400);
    /* RX113-only front end, with direct delegation to the shared ICU MMIO. */
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(s->dtc), 1, RX113_ICU_BASE, 1);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->icu), 2,
                       qdev_get_gpio_in(s->dtc, SWI));
}

static void register_tmr(RX113State *s, unsigned unit)
{
    SysBusDevice *tmr;
    unsigned i, irqbase;

    object_initialize_child(OBJECT(s), "tmr[*]",
                            &s->tmr[unit], TYPE_RENESAS_TMR);
    tmr = SYS_BUS_DEVICE(&s->tmr[unit]);
    qdev_prop_set_bit(DEVICE(tmr), "has-elc", true);
    qdev_prop_set_bit(DEVICE(tmr), "has-output", true);
    qdev_prop_set_uint64(DEVICE(tmr), "input-freq", RX113_PCLKB_HZ);
    sysbus_realize(tmr, &error_abort);

    irqbase = RX113_TMR_IRQ + TMR_NR_IRQ * unit;
    for (i = 0; i < TMR_NR_IRQ; i++) {
        sysbus_connect_irq(tmr, i,
                           qdev_get_gpio_in(s->dtc, irqbase + i));
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
                           qdev_get_gpio_in(s->dtc, irqbase + i));
    }
    sysbus_mmio_map(cmt, 0, RX113_CMT_BASE + unit * 0x10);
}

static void register_sci(RX113State *s, unsigned unit)
{
    SysBusDevice *sci;
    unsigned i;

    object_initialize_child(OBJECT(s), "sci[*]",
                            &s->sci[unit], TYPE_RENESAS_SCI);
    sci = SYS_BUS_DEVICE(&s->sci[unit]);
    /* Keep the original six backends; append SCI8 and SCI9. */
    qdev_prop_set_chr(DEVICE(sci), "chardev", serial_hd(unit));
    qdev_prop_set_uint64(DEVICE(sci), "input-freq", RX113_PCLKB_HZ);
    qdev_prop_set_bit(DEVICE(sci), "has-output", true);
    qdev_prop_set_bit(DEVICE(sci), "has-input", true);
    sysbus_realize(sci, &error_abort);

    for (i = 0; i < SCI_NR_IRQ; i++) {
        sysbus_connect_irq(sci, i,
                          qdev_get_gpio_in(s->dtc,
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
    s->lcdc = dev;
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
    qdev_connect_gpio_out_named(dev, "rtc-enable", 0,
                               qdev_get_gpio_in_named(sys, "rtc-enable", 0));
    sysbus_realize_and_unref(sbd, &error_abort);
    sysbus_mmio_map(sbd, 0, RX113_RTC_BASE);
    sysbus_connect_irq(sbd, RX113_RTC_IRQ_ALM,
                       qdev_get_gpio_in(s->dtc, RTC_ALM));
    sysbus_connect_irq(sbd, RX113_RTC_IRQ_PRD,
                       qdev_get_gpio_in(s->dtc, RTC_PRD));
    sysbus_connect_irq(sbd, RX113_RTC_IRQ_CUP,
                       qdev_get_gpio_in(s->dtc, RTC_CUP));
    sysbus_connect_irq(sbd, RX113_RTC_IRQ_PRD_LCDC,
                       qdev_get_gpio_in_named(lcdc, "prd", 0));
}

/* Sections 18/19: PORT and MPC, IRQ input selectors (Table 19.1). */
static DeviceState *register_port(RX113State *s)
{
    DeviceState *dev = qdev_new(TYPE_RX113_PORT);

    object_property_add_child(OBJECT(s), "port", OBJECT(dev));
    if (s->lcdc) {
        object_property_set_link(OBJECT(dev), "lcdc", OBJECT(s->lcdc),
                                 &error_abort);
    }
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_abort);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, RX113_PORT_BASE);
    for (unsigned i = 0; i < 8; i++) {
        qdev_connect_gpio_out_named(dev, "irq", i,
                                   qdev_get_gpio_in(s->dtc, 64 + i));
    }
    qdev_connect_gpio_out_named(dev, "cacref", 0,
                               qdev_get_gpio_in_named(s->cac, "cacref", 0));
    qdev_connect_gpio_out_named(dev, "nmi", 0,
                                qdev_get_gpio_in_named(DEVICE(&s->icu),
                                                       "nmi-pin", 0));
    for (unsigned i = 0; i < 3; i++) {
        qdev_connect_gpio_out_named(dev, "mtu-feedback", i,
                                   qdev_get_gpio_in_named(s->mtu,
                                                         "feedback", i));
    }
    for (unsigned i = 0; i < 3; i++) {
        qdev_connect_gpio_out_named(dev, "mtic5", i,
                                   qdev_get_gpio_in_named(s->mtu, "mtic5", i));
    }
    for (unsigned i = 0; i < RX113_MTIOC_PINS; i++) {
        qdev_connect_gpio_out_named(s->mtu, "mtioc", i,
                                   qdev_get_gpio_in_named(dev, "mtioc", i));
        qdev_connect_gpio_out_named(s->mtu, "mtioc-oe", i,
                                   qdev_get_gpio_in_named(dev, "mtioc",
                                                       i + RX113_MTIOC_PINS));
        qdev_connect_gpio_out_named(dev, "mtioc-in", i,
                                    qdev_get_gpio_in_named(s->mtu,
                                                           "mtioc-in", i));
    }
    /* Issue #50: TMR compare outputs (unit * 2 + channel) and RTCOUT. */
    for (unsigned unit = 0; unit < 2; unit++) {
        for (unsigned ch = 0; ch < 2; ch++) {
            unsigned sig = unit * 2 + ch;

            qdev_connect_gpio_out_named(DEVICE(&s->tmr[unit]), "tmo", ch,
                                        qdev_get_gpio_in_named(dev, "tmo",
                                                               sig));
            qdev_connect_gpio_out_named(DEVICE(&s->tmr[unit]), "tmo-oe", ch,
                                        qdev_get_gpio_in_named(dev, "tmo",
                                                               sig + 4));
        }
    }
    {
        DeviceState *rtc = DEVICE(object_resolve_path_component(OBJECT(s),
                                                                "rtc"));

        qdev_connect_gpio_out_named(rtc, "rtcout", 0,
                                    qdev_get_gpio_in_named(dev, "rtcout", 0));
        qdev_connect_gpio_out_named(rtc, "rtcout-oe", 0,
                                    qdev_get_gpio_in_named(dev, "rtcout", 1));
    }
    {
        /* Unit order is SCI0, 1, 2, 5, 6, 12, 8, 9 (sci_channels). */
        static const uint8_t txd_signal_for_unit[] = { 0, 1, 2, 3, 4, 7, 5, 6 };

        for (unsigned unit = 0; unit < ARRAY_SIZE(txd_signal_for_unit);
             unit++) {
            unsigned signal = txd_signal_for_unit[unit];

            qdev_connect_gpio_out_named(DEVICE(&s->sci[unit]), "txd", 0,
                                        qdev_get_gpio_in_named(dev, "txd",
                                                               signal));
            qdev_connect_gpio_out_named(DEVICE(&s->sci[unit]), "txd-oe", 0,
                                        qdev_get_gpio_in_named(dev, "txd",
                                                               signal + 8));
            qdev_connect_gpio_out_named(DEVICE(&s->sci[unit]), "sck", 0,
                                        qdev_get_gpio_in_named(dev, "sck",
                                                               signal));
            qdev_connect_gpio_out_named(DEVICE(&s->sci[unit]), "sck-oe", 0,
                                        qdev_get_gpio_in_named(dev, "sck",
                                                               signal + 8));
            qdev_connect_gpio_out_named(dev, "rxd", signal,
                                        qdev_get_gpio_in_named(
                                            DEVICE(&s->sci[unit]), "rxd", 0));
            qdev_connect_gpio_out_named(dev, "sck-sense", signal,
                                        qdev_get_gpio_in_named(
                                            DEVICE(&s->sci[unit]), "sck-in",
                                            0));
        }
    }
    return dev;
}

static void register_s12ad(RX113State *s, DeviceState *port)
{
    DeviceState *dev = qdev_new(TYPE_RX113_S12AD);

    s->s12ad = dev;
    object_property_add_child(OBJECT(s), "s12ad", OBJECT(dev));
    object_property_set_link(OBJECT(dev), "port", OBJECT(port), &error_abort);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_abort);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, 0x00089000);
    /* Table 14.3: S12ADI0 and GBADI are edge sources 102 and 103. */
    for (unsigned i = 0; i < 2; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(dev), i,
                           qdev_get_gpio_in(s->dtc, 102 + i));
    }
}

/* Table 17.3 source numbers are independent of the ICU vector numbers. */
static void elc_source_connect(DeviceState *src, const char *name, unsigned pin,
                               DeviceState *elc, unsigned id)
{
    qdev_connect_gpio_out_named(src, name, pin,
                               qdev_get_gpio_in_named(elc, "event", id));
}

static void register_elc(RX113State *s)
{
    DeviceState *dev = qdev_new(TYPE_RX113_ELC);
    DeviceState *splitter = qdev_new(TYPE_SPLIT_IRQ);
    Object *port = object_resolve_path_component(OBJECT(s), "port");

    s->elc = dev;
    object_property_add_child(OBJECT(s), "elc", OBJECT(dev));
    object_property_set_link(OBJECT(dev), "mtu", OBJECT(s->mtu),
                             &error_abort);
    object_property_set_link(OBJECT(dev), "cmt", OBJECT(&s->cmt[0]),
                             &error_abort);
    object_property_set_link(OBJECT(dev), "tmr0", OBJECT(&s->tmr[0]),
                             &error_abort);
    object_property_set_link(OBJECT(dev), "tmr2", OBJECT(&s->tmr[1]),
                             &error_abort);
    object_property_set_link(OBJECT(dev), "port", port, &error_abort);
    RX113_PORT(port)->elc = dev;
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_abort);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, 0x0008b100);
    for (unsigned id = 0x08; id <= 0x1a; id++) {
        elc_source_connect(s->mtu, "event", id, dev, id);
    }
    elc_source_connect(DEVICE(&s->cmt[0]), "event", 1, dev, 0x1f);
    for (unsigned unit = 0; unit < 2; unit++) {
        for (unsigned i = 0; i < 3; i++) {
            elc_source_connect(DEVICE(&s->tmr[unit]), "event", i, dev,
                               (unit ? 0x28 : 0x22) + i);
        }
    }
    for (unsigned i = 0; i < SCI_NR_IRQ; i++) {
        elc_source_connect(DEVICE(&s->sci[3]), "event", i, dev, 0x3a + i);
    }
    object_property_add_child(OBJECT(s), "adc-scan-end", OBJECT(splitter));
    qdev_prop_set_uint16(splitter, "num-lines", 2);
    qdev_realize_and_unref(splitter, NULL, &error_abort);
    qdev_connect_gpio_out(splitter, 0,
                         qdev_get_gpio_in_named(dev, "event", 0x58));
    qdev_connect_gpio_out(splitter, 1,
                         qdev_get_gpio_in_named(s->dac, "sync", 0));
    qdev_connect_gpio_out_named(s->s12ad, "scan-end", 0,
                               qdev_get_gpio_in(splitter, 0));
    elc_source_connect(s->dtc, "transfer-end", 0, dev, 0x61);
    elc_source_connect(s->doc, "event", 0, dev, 0x6a);
    for (unsigned i = 0; i < 12; i++) {
        qdev_connect_gpio_out_named(dev, "mtu", i,
                                   qdev_get_gpio_in_named(s->mtu, "elc", i));
    }
    qdev_connect_gpio_out_named(dev, "link", 7,
                               qdev_get_gpio_in_named(DEVICE(&s->cmt[0]),
                                                      "elc", 1));
    for (unsigned i = 0; i < 2; i++) {
        qdev_connect_gpio_out_named(dev, "link", i ? 12 : 10,
                                   qdev_get_gpio_in_named(DEVICE(&s->tmr[i]),
                                                          "elc", 0));
    }
    qdev_connect_gpio_out_named(dev, "link", 14,
                               qdev_get_gpio_in_named(s->ctsu, "start", 0));
    qdev_connect_gpio_out_named(dev, "link", 15,
                               qdev_get_gpio_in_named(s->s12ad, "trigger", 9));
    qdev_connect_gpio_out_named(dev, "link", 17,
                               qdev_get_gpio_in_named(s->dac, "elc", 0));
    qdev_connect_gpio_out_named(dev, "link", 18,
                               qdev_get_gpio_in(s->dtc, 106));
    qdev_connect_gpio_out_named(dev, "link", 19,
                               qdev_get_gpio_in(s->dtc, 80));
}

/* Section 27, Table 14.3: USBI/USBR and both DTC FIFO requests. */
static void register_usb(RX113State *s)
{
    static const unsigned vectors[] = { 38, 90, 36, 37 };

    s->usb = qdev_new(TYPE_RX113_USB);
    object_property_add_child(OBJECT(s), "usb0", OBJECT(s->usb));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->usb), &error_abort);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->usb), 0, 0x000a0000);
    for (unsigned i = 0; i < ARRAY_SIZE(vectors); i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(s->usb), i,
                           qdev_get_gpio_in(s->dtc, vectors[i]));
    }
}

static void rx113_realize(DeviceState *dev, Error **errp)
{
    RX113State *s = RX113_MCU(dev);
    RX113Class *rxc = RX113_MCU_GET_CLASS(dev);
    DeviceState *port;

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

    if (s->main_clock_hz < 64 || s->main_clock_hz > 20000000) {
        error_setg(errp, "RX113 main-clock-hz must be between 64 and 20000000");
        return;
    }

    /* Initialize CPU */
    object_initialize_child(OBJECT(s), "cpu", &s->cpu, TYPE_RX113_CPU);
    qdev_realize(DEVICE(&s->cpu), NULL, &error_abort);

    s->iwdt = qdev_new(TYPE_RX113_IWDT);
    object_property_add_child(OBJECT(s), "iwdt", OBJECT(s->iwdt));
    object_property_set_link(OBJECT(s->iwdt), "flash", OBJECT(&s->c_flash),
                             &error_abort);
    object_property_set_link(OBJECT(s->iwdt), "cpu", OBJECT(&s->cpu),
                             &error_abort);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->iwdt), &error_abort);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->iwdt), 0, 0x00088030);

    register_system(s);
    register_icu(s);
    register_dtc(s);
    s->cpu.env.ack = qdev_get_gpio_in_named(s->dtc, "ack", 0);
    s->mtu = qdev_new(TYPE_RX113_MTU);
    object_property_add_child(OBJECT(s), "mtu", OBJECT(s->mtu));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->mtu), &error_abort);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->mtu), 0, 0x00088600);
    /* Table 14.3: all 28 MTU interrupts are consecutive edge sources. */
    for (unsigned i = 0; i < RX113_MTU_IRQS; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(s->mtu), i,
                           qdev_get_gpio_in(s->dtc, 114 + i));
    }
    register_tmr(s, 0);
    register_tmr(s, 1);
    register_cmt(s, 0);
    register_cmt(s, 1);
    for (unsigned i = 0; i < ARRAY_SIZE(sci_channels); i++) {
        register_sci(s, i);
    }
    register_rtc(s, register_lcdc(s));
    s->cac = qdev_new(TYPE_RX113_CAC);
    object_property_add_child(OBJECT(s), "cac", OBJECT(s->cac));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->cac), &error_abort);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->cac), 0, 0x0008b000);
    /* Table 14.3: level sources 32..34, without DTC activation. */
    for (unsigned i = 0; i < 3; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(s->cac), i,
                           qdev_get_gpio_in(DEVICE(&s->icu), 32 + i));
    }
    port = register_port(s);

    s->poe = qdev_new(TYPE_RX113_POE);
    object_property_add_child(OBJECT(s), "poe", OBJECT(s->poe));
    object_property_set_link(OBJECT(s->poe), "port", OBJECT(port),
                             &error_abort);
    object_property_set_link(OBJECT(s->poe), "mtu", OBJECT(s->mtu),
                             &error_abort);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->poe), &error_abort);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->poe), 0, 0x00088900);
    RX113_PORT(port)->poe = s->poe;
    for (unsigned i = 0; i < 2; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(s->poe), i,
                           qdev_get_gpio_in(DEVICE(&s->icu), 170 + i));
    }
    register_s12ad(s, port);
    s->dac = qdev_new(TYPE_RX113_DAC);
    object_property_add_child(OBJECT(s), "dac", OBJECT(s->dac));
    object_property_set_link(OBJECT(s->dac), "adc", OBJECT(s->s12ad),
                             &error_abort);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->dac), &error_abort);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->dac), 0, 0x00088040);
    s->doc = qdev_new(TYPE_RX113_DOC);
    object_property_add_child(OBJECT(s), "doc", OBJECT(s->doc));
    qdev_prop_set_bit(s->doc, "has-elc", true);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->doc), &error_abort);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->doc), 0, 0x0008b080);
    /* Table 14.3: level source 57, with no DTC activation. */
    sysbus_connect_irq(SYS_BUS_DEVICE(s->doc), 0,
                       qdev_get_gpio_in(DEVICE(&s->icu), 57));
    s->ctsu = qdev_new(TYPE_RX113_CTSU);
    object_property_add_child(OBJECT(s), "ctsu", OBJECT(s->ctsu));
    object_property_set_link(OBJECT(s->ctsu), "port",
                             object_resolve_path_component(OBJECT(s), "port"),
                             &error_abort);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->ctsu), &error_abort);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->ctsu), 0, 0x000a0900);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->ctsu), 1, 0x007fffbe);
    for (unsigned i = 0; i < 3; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(s->ctsu), i,
                           qdev_get_gpio_in(i < 2 ? s->dtc : DEVICE(&s->icu),
                                            60 + i));
    }
    register_elc(s);
    register_usb(s);
}

static const Property rx113_properties[] = {
    /* Explicit board crystal assumption, matching the FIT generic RX113 BSP. */
    DEFINE_PROP_UINT64("main-clock-hz", RX113State, main_clock_hz, 16000000),
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
