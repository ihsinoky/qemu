/*
 * Renesas RX113 realtime clock (RTCA)
 * Datasheet: R01UH0448EJ0120 Rev.1.20, section 24
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_RX_RX113_RTC_H
#define HW_RX_RX113_RTC_H

#include "hw/core/sysbus.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_RX113_RTC "rx113-rtc"
OBJECT_DECLARE_SIMPLE_TYPE(RX113RTCState, RX113_RTC)

/* sysbus IRQ outputs: ALM, PRD, CUP (Table 14.3) and PRD for the LCDC */
#define RX113_RTC_IRQ_ALM       0
#define RX113_RTC_IRQ_PRD       1
#define RX113_RTC_IRQ_CUP       2
#define RX113_RTC_IRQ_PRD_LCDC  3

struct RX113RTCState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    QEMUTimer timer;
    qemu_irq alm;
    qemu_irq prd;
    qemu_irq cup;
    qemu_irq prd_lcdc;
    qemu_irq rtc_enable;
    /* Issue #50: RTCOUT pad level and output enable for the PORT mux. */
    qemu_irq rtcout;
    qemu_irq rtcout_oe;

    /* Calendar mode counters, raw BCD register values (24.2.2 - 24.2.8) */
    uint8_t sec;
    uint8_t min;
    uint8_t hour;
    uint8_t wday;
    uint8_t day;
    uint8_t mon;
    uint16_t year;
    /* Binary mode counter, BCNT3 to BCNT0 (24.2.2 - 24.2.5) */
    uint32_t bcnt;

    /* Alarm registers (24.2.9 - 24.2.16) */
    uint8_t secar;
    uint8_t minar;
    uint8_t hourar;
    uint8_t wdayar;
    uint8_t dayar;
    uint8_t monar;
    uint16_t yearar;
    uint8_t yeararen;
    uint32_t bcntar;
    uint32_t bcntaer;

    uint8_t rcr1;
    uint8_t rcr2;
    uint8_t rcr3;
    uint8_t radj;

    /* Position inside the second in 1/256 s units (R64CNT = sub >> 1) */
    uint16_t sub;
    /* Free-running 1/256 s counter modulo 512 for the periodic interrupt */
    uint16_t prd_phase;
    bool alarm_match;
    int64_t last_ns;
    /* Fractional divider tick, in 1/128 ns; negative for retardation. */
    int64_t phase_units;
    bool system_subclock;
};

#endif /* HW_RX_RX113_RTC_H */
