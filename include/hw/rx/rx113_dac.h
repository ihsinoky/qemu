/*
 * RX113 12-bit D/A converter (R12DAA)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_RX_RX113_DAC_H
#define HW_RX_RX113_DAC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_RX113_DAC "rx113-dac"
OBJECT_DECLARE_SIMPLE_TYPE(RX113DACState, RX113_DAC)

void rx113_dac_set_clock(DeviceState *dev, uint64_t pclkb, bool stopped);

#endif /* HW_RX_RX113_DAC_H */
