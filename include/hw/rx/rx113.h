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

#define TYPE_R5F51135_MCU "r5f51135-mcu"
#define TYPE_R5F51136_MCU "r5f51136-mcu"
#define TYPE_R5F51137_MCU "r5f51137-mcu"
#define TYPE_R5F51138_MCU "r5f51138-mcu"

struct RX113State {
    DeviceState parent_obj;

    RXCPU cpu;
    RXICUState icu;
    DeviceState *iwdt;
    DeviceState *mtu;
    DeviceState *poe;
    DeviceState *s12ad;
    DeviceState *dac;
    DeviceState *dtc;
    DeviceState *elc;
    DeviceState *doc;
    DeviceState *cac;
    DeviceState *ctsu;
    DeviceState *usb;
    RTMRState tmr[2];
    RCMTState cmt[2];
    RSCIState sci[8];

    uint64_t main_clock_hz;
    MemoryRegion *sysmem;
    MemoryRegion iram;
    MemoryRegion d_flash;
    MemoryRegion c_flash;
};

#endif /* HW_RX_RX113_H */
