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
    DeviceState *elc;
    DeviceState *poe;
    uint32_t package_pins;
    uint8_t regs[0x200];
    uint8_t external[RX113_PORT_COUNT];
    uint8_t external_mask[RX113_PORT_COUNT];
    uint8_t levels[RX113_PORT_COUNT];
    uint8_t driving[RX113_PORT_COUNT];
    qemu_irq pin_out[RX113_PORT_PINS];
    qemu_irq pin_oe[RX113_PORT_PINS];
    qemu_irq irq[8];
    qemu_irq cacref;
    qemu_irq mtu_feedback[3];
    qemu_irq mtic5[3];
    bool ctsu_fixed, ctsu_level;
    uint16_t mtioc, mtioc_oe, poe_cutoff;
};

void rx113_port_set_poe(RX113PortState *s, uint16_t mask);
uint8_t rx113_port_poe_inputs(RX113PortState *s);
bool rx113_port_mtioc_active(RX113PortState *s, unsigned signal, bool level);

void rx113_port_set_ctsu(RX113PortState *s, bool fixed, bool level);

/* Qualified PORTB GPIO bits, direction, pin levels and output latch. */
uint8_t rx113_port_b_mask(RX113PortState *s);
uint8_t rx113_port_b_direction(RX113PortState *s);
uint8_t rx113_port_b_levels(RX113PortState *s);
uint8_t rx113_port_b_latch(RX113PortState *s);
void rx113_port_b_write(RX113PortState *s, uint8_t mask, uint8_t value);

#endif /* HW_RX_RX113_PORT_H */
