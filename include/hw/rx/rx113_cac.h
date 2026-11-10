/*
 * Renesas RX113 clock frequency accuracy measurement circuit
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_RX_RX113_CAC_H
#define HW_RX_RX113_CAC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_RX113_CAC "rx113-cac"
OBJECT_DECLARE_SIMPLE_TYPE(RX113CACState, RX113_CAC)

/* FMCS/RSCS order: main, sub, HOCO, LOCO, IWDTCLK, PCLKB. */
void rx113_cac_set_clocks(DeviceState *dev, const uint64_t hz[6], bool stopped);

#endif
