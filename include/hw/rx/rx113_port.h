/*
 * RX113 I/O ports and multi-function pin controller
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_RX_RX113_PORT_H
#define HW_RX_RX113_PORT_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_RX113_PORT "rx113-port"
OBJECT_DECLARE_SIMPLE_TYPE(RX113PortState, RX113_PORT)

#define RX113_PORT_BASE 0x8c000
#define RX113_PORT_COUNT 19
#define RX113_PORT_PINS (RX113_PORT_COUNT * 8)

struct RX113PortState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t package_pins;
    uint8_t regs[0x200];
    uint8_t external[RX113_PORT_COUNT];
    uint8_t external_mask[RX113_PORT_COUNT];
    uint8_t levels[RX113_PORT_COUNT];
    uint8_t driving[RX113_PORT_COUNT];
    qemu_irq pin_out[RX113_PORT_PINS];
    qemu_irq pin_oe[RX113_PORT_PINS];
    qemu_irq irq[8];
};

#endif /* HW_RX_RX113_PORT_H */
