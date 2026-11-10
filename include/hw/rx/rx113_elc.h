/*
 * RX113 event link controller
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_RX_RX113_ELC_H
#define HW_RX_RX113_ELC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_RX113_ELC "rx113-elc"
OBJECT_DECLARE_SIMPLE_TYPE(RX113ELCState, RX113_ELC)

void rx113_elc_set_clock(DeviceState *dev, uint64_t hz, bool stopped);

#endif
