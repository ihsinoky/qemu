/*
 * Renesas RX113 independent watchdog timer
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_RX_RX113_IWDT_H
#define HW_RX_RX113_IWDT_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_RX113_IWDT "rx113-iwdt"
OBJECT_DECLARE_SIMPLE_TYPE(RX113IWDTState, RX113_IWDT)

uint64_t rx113_iwdt_clock_hz(DeviceState *dev);
uint8_t rx113_iwdt_ilococr(DeviceState *dev);
void rx113_iwdt_start_clock(DeviceState *dev);
uint8_t rx113_iwdt_reset_status(DeviceState *dev);
void rx113_iwdt_clear_reset_status(DeviceState *dev, uint8_t value);

#endif
