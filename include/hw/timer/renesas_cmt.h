/*
 * Renesas Compare-match timer Object
 *
 * Copyright (c) 2019 Yoshinori Sato
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_TIMER_RENESAS_CMT_H
#define HW_TIMER_RENESAS_CMT_H

#include "qemu/timer.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_RENESAS_CMT "renesas-cmt"
typedef struct RCMTState RCMTState;
DECLARE_INSTANCE_CHECKER(RCMTState, RCMT,
                         TYPE_RENESAS_CMT)

enum {
    CMT_CH = 2,
    CMT_NR_IRQ = 1 * CMT_CH
};

struct RCMTState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    /* Set only by renesas_cmt_set_clock(); legacy behavior is the default. */
    uint64_t phase[CMT_CH];
    uint64_t legacy_input_freq;
    bool clock_managed;
    bool module_stopped;
    uint64_t input_freq;
    MemoryRegion memory;

    uint16_t cmstr;
    uint16_t cmcr[CMT_CH];
    uint16_t cmcnt[CMT_CH];
    uint16_t cmcor[CMT_CH];
    int64_t tick[CMT_CH];
    qemu_irq cmi[CMT_CH];
    QEMUTimer timer[CMT_CH];
};

/*
 * Opt in under the BQL after realization, before guest execution starts.
 * Subsequent calls update running or stopped modules at runtime.
 * freq is a nonzero input clock in Hz, at most 1 GHz; stop retains state
 * and blocks MMIO (reads return zero). The legacy input-freq property
 * remains realization-only. Subsequent reset retains this clock policy.
 */
void renesas_cmt_set_clock(RCMTState *s, uint64_t freq, bool stopped);

#endif
