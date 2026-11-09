/*
 * Helpers for the opt-in Renesas peripheral clock interfaces.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_TIMER_RENESAS_TIMER_H
#define HW_TIMER_RENESAS_TIMER_H

#include "qemu/host-utils.h"
#include "qemu/timer.h"

/* phase is in billionths of an input cycle, including the prescaler. */
static inline uint64_t renesas_timer_ticks(int64_t ns, uint64_t freq,
                                          unsigned div, uint64_t *phase)
{
    uint64_t cycles = muldiv64(ns, freq, NANOSECONDS_PER_SECOND);
    uint64_t scale = (uint64_t)div * NANOSECONDS_PER_SECOND;

    *phase += ((ns % NANOSECONDS_PER_SECOND) * freq) %
              NANOSECONDS_PER_SECOND;
    *phase += (cycles % div) * NANOSECONDS_PER_SECOND;
    cycles = cycles / div + *phase / scale;
    *phase %= scale;
    return cycles;
}

static inline int64_t renesas_timer_delay(uint64_t ticks, uint64_t freq,
                                          unsigned div, uint64_t phase)
{
    uint64_t cycles = ticks * div * NANOSECONDS_PER_SECOND - phase;

    return MAX(1, DIV_ROUND_UP(cycles, freq));
}

#endif
