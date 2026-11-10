/*
 * RX113 12-bit A/D converter (S12ADb)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_RX_RX113_S12AD_H
#define HW_RX_RX113_S12AD_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_RX113_S12AD "rx113-s12ad"
OBJECT_DECLARE_SIMPLE_TYPE(RX113S12ADState, RX113_S12AD)

bool rx113_s12ad_active(DeviceState *dev);

void rx113_s12ad_set_clock(DeviceState *dev, uint64_t pclkb, uint64_t pclkd,
                           bool stopped);

#endif /* HW_RX_RX113_S12AD_H */
