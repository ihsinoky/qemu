/*
 * RX Interrupt Control Unit
 *
 * Copyright (c) 2019 Yoshinori Sato
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef HW_INTC_RX_ICU_H
#define HW_INTC_RX_ICU_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

enum TRG_MODE {
    TRG_LEVEL = 0,
    TRG_NEDGE = 1,      /* Falling */
    TRG_PEDGE = 2,      /* Raising */
    TRG_BEDGE = 3,      /* Both */
};

struct IRQSource {
    enum TRG_MODE sense;
    int level;
};

enum {
    /* Software interrupt request */
    SWI = 27,
    NR_IRQS = 256
};

struct RXICUState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    MemoryRegion memory;
    struct IRQSource src[NR_IRQS];
    uint32_t nr_irqs;
    uint8_t *map;
    uint32_t nr_sense;
    uint8_t *init_sense;

    uint8_t ir[NR_IRQS];
    uint8_t dtcer[NR_IRQS];
    uint8_t ier[NR_IRQS / 8];
    uint32_t ipr_count;
    uint8_t *ipr;
    uint8_t dmasr[4];
    uint16_t fir;
    uint8_t nmisr;
    uint8_t nmier;
    uint8_t nmiclr;
    uint8_t nmicr;
    /* RX113 NMI block (opt-in via the nmi-rx113 property) */
    bool nmi_rx113;
    uint8_t nmiflte;
    uint8_t nmifltc;
    uint8_t nmi_in_level[3];
    /* RX113 digital filters (section 14.4.7), same opt-in. */
    uint8_t irqflte;
    uint16_t irqfltc;
    uint64_t icu_hz;
    QEMUTimer *filter_timer;
    uint8_t irq_filt_raw[8];
    uint8_t irq_filt_out[8];
    uint8_t irq_filt_count[8];
    int64_t irq_filt_next[8];
    uint8_t nmi_filt_raw;
    uint8_t nmi_filt_count;
    int64_t nmi_filt_next;
    int16_t req_irq;
    qemu_irq _irq;
    qemu_irq _fir;
    qemu_irq _swi;
    qemu_irq _nmi;
};

#define TYPE_RX_ICU "rx-icu"
OBJECT_DECLARE_SIMPLE_TYPE(RXICUState, RX_ICU)

/* Deliver the ICU clock (ICLK); drives the digital filter timing. */
void rx_icu_set_clock(DeviceState *dev, uint64_t hz);

#endif /* HW_INTC_RX_ICU_H */
