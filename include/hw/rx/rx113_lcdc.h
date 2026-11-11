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

/*
 * LCD source clock kinds (9.2.15 LCDSCLKCR.LCDSCLKSEL); the LCDC0
 * divider table differs per kind (34.2.3). LOCO, HOCO and the main
 * clock oscillator share one table.
 */
enum RX113LCDCSource {
    RX113_LCD_SOURCE_MAIN,
    RX113_LCD_SOURCE_SUB,
    RX113_LCD_SOURCE_IWDT,
};

struct RX113LCDCState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint8_t lcdm0;
    uint8_t lcdm1;
    uint8_t lcdc0;
    uint8_t vlcd;
    uint8_t ram[RX113_LCDC_NR_SEG];
    bool blink_phase;

    /* Clock delivery from the SYSTEM model (rx113.c). */
    uint64_t src_hz;
    unsigned src_kind;
    bool stopped;
    /* Frame scan position: half-LCDCL steps at anchor_ns. */
    uint64_t step;
    int64_t anchor_ns;
};

/*
 * Deliver the LCD source clock selected in the SYSTEM model. hz is
 * the source frequency (0 when the source is stopped or LCDSCLKSTP
 * stops the output); source selects the LCDC0 divider table; stopped
 * is the MSTPCRD module-stop state of the LCDC.
 */
void rx113_lcdc_set_clock(DeviceState *dev, uint64_t hz, unsigned source,
                          bool stopped);

/*
 * Current COM/SEG pad drive for the PORT model (Issue #63). The
 * masks follow the pin functions of Tables 19.3 to 19.16 at
 * PSEL = 24: com bit n is COMn, seg bit n is SEGnn. A set drive bit
 * means the LCDC actively drives the pin at the matching level bit.
 * A module-stopped LCDC drives nothing. The levels are the digital
 * abstraction of Figures 34.15 to 34.17 documented in the issue
 * report: the selected common is driven opposite to a lit segment,
 * non-selected commons are not driven (their deselect level is a
 * bias voltage with no digital equivalent).
 */
void rx113_lcdc_pad_state(DeviceState *dev, uint8_t *com_level,
                          uint8_t *com_drive, uint64_t *seg_level,
                          uint64_t *seg_drive);

#endif /* HW_RX_RX113_LCDC_H */
