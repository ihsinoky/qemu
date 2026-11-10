/*
 * Renesas RX113 data operation circuit
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_RX_RX113_DOC_H
#define HW_RX_RX113_DOC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_RX113_DOC "rx113-doc"
OBJECT_DECLARE_SIMPLE_TYPE(RX113DOCState, RX113_DOC)

void rx113_doc_set_stopped(DeviceState *dev, bool stopped);

#endif
