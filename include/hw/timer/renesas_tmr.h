/*
 * Renesas 8bit timer Object
 *
 * Copyright (c) 2018 Yoshinori Sato
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_TIMER_RENESAS_TMR_H
#define HW_TIMER_RENESAS_TMR_H

#include "qemu/timer.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_RENESAS_TMR "renesas-tmr"
typedef struct RTMRState RTMRState;
DECLARE_INSTANCE_CHECKER(RTMRState, RTMR,
                         TYPE_RENESAS_TMR)

enum timer_event {
    cmia = 0,
    cmib = 1,
    ovi = 2,
    none = 3,
    TMR_NR_EVENTS = 4
};

enum {
    TMR_CH = 2,
    TMR_NR_IRQ = 3 * TMR_CH
};

struct RTMRState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    /* Set only by renesas_tmr_set_clock(); legacy behavior is the default. */
    int64_t clock_tick[TMR_CH];
    uint8_t clock_event[TMR_CH];
    uint64_t phase[TMR_CH];
    uint64_t legacy_input_freq;
    bool clock_managed;
    bool module_stopped;
    uint64_t input_freq;
    MemoryRegion memory;

    int64_t tick;
    uint8_t tcnt[TMR_CH];
    uint8_t tcora[TMR_CH];
    uint8_t tcorb[TMR_CH];
    uint8_t tcr[TMR_CH];
    uint8_t tccr[TMR_CH];
    uint8_t tcor[TMR_CH];
    uint8_t tcsr[TMR_CH];
    int64_t div_round[TMR_CH];
    uint8_t next[TMR_CH];
    qemu_irq cmia[TMR_CH];
    qemu_irq cmib[TMR_CH];
    qemu_irq ovi[TMR_CH];
    QEMUTimer timer[TMR_CH];
};

/*
 * Opt in under the BQL after realization, before guest execution starts.
 * Subsequent calls update running or stopped modules at runtime.
 * freq is a nonzero input clock in Hz, at most 1 GHz; stop retains state
 * and blocks MMIO (reads return zero). The legacy input-freq property
 * remains realization-only. Subsequent reset retains this clock policy.
 */
void renesas_tmr_set_clock(RTMRState *s, uint64_t freq, bool stopped);

#endif
