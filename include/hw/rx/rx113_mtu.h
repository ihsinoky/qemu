/*
 * Renesas RX113 multi-function timer
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_RX_RX113_MTU_H
#define HW_RX_RX113_MTU_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_RX113_MTU "rx113-mtu"
OBJECT_DECLARE_SIMPLE_TYPE(RX113MTUState, RX113_MTU)

#define RX113_MTU_IRQS 28
void rx113_mtu_set_clock(DeviceState *dev, uint64_t hz, bool stopped);

#endif
