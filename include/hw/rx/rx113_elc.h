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

/* PORT calls these only for physical updates and CPU/DTC PODR writes. */
void rx113_elc_port_input(DeviceState *dev, uint8_t old, uint8_t level);
uint8_t rx113_elc_port_write_mask(DeviceState *dev);

#endif
