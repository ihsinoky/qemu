/*
 * Renesas RX113 USBc register and SIE boundary core
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_RX_RX113_USB_H
#define HW_RX_RX113_USB_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_RX113_USB "rx113-usb"
OBJECT_DECLARE_SIMPLE_TYPE(RX113USBState, RX113_USB)

void rx113_usb_set_clock(DeviceState *dev, uint64_t uclk, bool stopped);

#endif
