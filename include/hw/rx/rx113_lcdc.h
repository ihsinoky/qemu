/*
 * Renesas RX113 LCD controller/driver (LCDC)
 * Datasheet: R01UH0448EJ0120 Rev.1.20, section 34
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_RX_RX113_LCDC_H
#define HW_RX_RX113_LCDC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_RX113_LCDC "rx113-lcdc"
OBJECT_DECLARE_SIMPLE_TYPE(RX113LCDCState, RX113_LCDC)

/* Table 34.6: SEG00 to SEG39, one display data register each. */
#define RX113_LCDC_NR_SEG 40

struct RX113LCDCState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint8_t lcdm0;
    uint8_t lcdm1;
    uint8_t lcdc0;
    uint8_t vlcd;
    uint8_t ram[RX113_LCDC_NR_SEG];
    bool blink_phase;
};

#endif /* HW_RX_RX113_LCDC_H */
