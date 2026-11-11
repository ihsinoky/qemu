/*
 * RX113 port output enable 2 (POE2a)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_RX_RX113_POE_H
#define HW_RX_RX113_POE_H

#include "hw/core/sysbus.h"
#include "hw/rx/rx113_port.h"
#include "qemu/timer.h"

#define TYPE_RX113_POE "rx113-poe"
OBJECT_DECLARE_SIMPLE_TYPE(RX113POEState, RX113_POE)

struct RX113POEState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    RX113PortState *port;
    DeviceState *mtu;
    QEMUTimer *timer;
    qemu_irq irq[2];
    uint16_t regs[8], seen[8];
    uint8_t locks, previous, high, samples[5];
    uint8_t stop_cycles;
    uint32_t cycles;
    uint64_t hz, fraction;
    int64_t last_ns;
};

void rx113_poe_set_clock(DeviceState *dev, uint64_t hz);
void rx113_poe_oscillator_stop(DeviceState *dev);
void rx113_poe_port_changed(DeviceState *dev);

#endif /* HW_RX_RX113_POE_H */
