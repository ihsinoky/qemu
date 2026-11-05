/*
 * Renesas RX113 MCU skeleton
 * Based on the RX62N model, Copyright (c) 2019 Yoshinori Sato.
 * Datasheet: R01UH0448EJ0120 Rev.1.20
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_RX_RX113_H
#define HW_RX_RX113_H

#include "target/rx/cpu.h"
#include "hw/intc/rx_icu.h"
#include "hw/timer/renesas_tmr.h"
#include "hw/timer/renesas_cmt.h"
#include "hw/char/renesas_sci.h"
#include "qom/object.h"

#define TYPE_RX113_MCU "rx113-mcu"
OBJECT_DECLARE_SIMPLE_TYPE(RX113State, RX113_MCU)

#define TYPE_R5F51138_MCU "r5f51138-mcu"

struct RX113State {
    DeviceState parent_obj;

    RXCPU cpu;
    RXICUState icu;
    RTMRState tmr[2];
    RCMTState cmt[2];
    RSCIState sci[6];

    MemoryRegion *sysmem;
    MemoryRegion iram;
    MemoryRegion d_flash;
    MemoryRegion c_flash;
};

#endif /* HW_RX_RX113_H */
