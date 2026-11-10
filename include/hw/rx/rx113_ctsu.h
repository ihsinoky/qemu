/*
 * Renesas RX113 capacitive touch sensing unit
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_RX_RX113_CTSU_H
#define HW_RX_RX113_CTSU_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_RX113_CTSU "rx113-ctsu"
OBJECT_DECLARE_SIMPLE_TYPE(RX113CTSUState, RX113_CTSU)

void rx113_ctsu_set_clock(DeviceState *dev, uint64_t hz, bool stopped);

#endif
