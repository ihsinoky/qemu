/*
 * Renesas Serial Communication Interface
 *
 * Copyright (c) 2018 Yoshinori Sato
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_CHAR_RENESAS_SCI_H
#define HW_CHAR_RENESAS_SCI_H

#include "chardev/char-fe.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_RENESAS_SCI "renesas-sci"
typedef struct RSCIState RSCIState;
DECLARE_INSTANCE_CHECKER(RSCIState, RSCI,
                         TYPE_RENESAS_SCI)

enum {
    ERI = 0,
    RXI = 1,
    TXI = 2,
    TEI = 3,
    SCI_NR_IRQ = 4
};

struct RSCIState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    MemoryRegion memory;
    QEMUTimer timer;
    CharFrontend chr;
    qemu_irq irq[SCI_NR_IRQ];
    qemu_irq event[SCI_NR_IRQ];

    uint8_t smr;
    uint8_t brr;
    uint8_t scr;
    uint8_t tdr;
    uint8_t ssr;
    uint8_t rdr;
    uint8_t scmr;
    uint8_t semr;

    uint8_t read_ssr;
    int64_t trtime;
    int64_t rx_next;
    /* Set only by renesas_sci_set_clock(); legacy behavior is the default. */
    QEMUTimer rx_timer;
    /* Remaining work in billionths of an input cycle, not wall time. */
    int64_t clock_tick;
    int64_t tx_remaining;
    int64_t rx_remaining;
    bool tx_pending;
    uint64_t legacy_input_freq;
    bool clock_managed;
    bool module_stopped;
    uint64_t input_freq;
};

/*
 * Opt in under the BQL after realization, before guest execution starts.
 * Subsequent calls update running or stopped modules at runtime.
 * freq is a nonzero input clock in Hz, at most 1 GHz; stop retains state
 * and blocks MMIO (reads return zero). The legacy input-freq property
 * remains realization-only. Subsequent reset retains this clock policy.
 */
void renesas_sci_set_clock(RSCIState *s, uint64_t freq, bool stopped);

#endif
