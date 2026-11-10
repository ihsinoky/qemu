/*
 * Renesas RX113 data transfer controller (DTCa)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_RX_RX113_DTC_H
#define HW_RX_RX113_DTC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_RX113_DTC "rx113-dtc"
OBJECT_DECLARE_SIMPLE_TYPE(RX113DTCState, RX113_DTC)

void rx113_dtc_set_stopped(DeviceState *dev, bool stopped);

#endif /* HW_RX_RX113_DTC_H */
