/*
 * Renesas RX113 port output enable 2
 * Hardware manual: R01UH0448EJ0120 Rev.1.20, section 21.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/rx/rx113_poe.h"
#include "hw/rx/rx113_mtu.h"
#include "migration/vmstate.h"
#include "qemu/module.h"

/* Word indices; holes at 4h and 6h are not registers. */
enum { ICSR1, OCSR1, ICSR2 = 4, SPOER, POECR2, ICSR3 };

static const uint16_t flags[8] = {
    [ICSR1] = 0xf000, [OCSR1] = 0x8000,
    [ICSR2] = 0x1000, [SPOER] = 3, [ICSR3] = 0x1000,
};
static const unsigned pair_pins[3][2] = { { 9, 11 }, { 12, 14 }, { 13, 15 } };

static uint8_t poe_pairs(RX113POEState *s)
{
    return s->regs[POECR2];
}

static void poe_update(RX113POEState *s)
{
    bool oscillator = (s->regs[ICSR3] & 0x1200) == 0x1200;
    bool group0 = oscillator || (s->regs[SPOER] & 2) ||
                  (s->regs[ICSR2] & 0x1200) == 0x1200;
    bool group34 = oscillator || (s->regs[SPOER] & 1) ||
                   (s->regs[ICSR1] & 0xf000) ||
                   (s->regs[OCSR1] & 0x8200) == 0x8200;
    uint16_t cutoff = group0 ? (s->regs[SPOER] >> 8) & 15 : 0;

    if (group34) {
        for (unsigned i = 0; i < 3; i++) {
            if (poe_pairs(s) & (0x40 >> i)) {
                cutoff |= (1 << pair_pins[i][0]) | (1 << pair_pins[i][1]);
            }
        }
    }
    rx113_port_set_poe(s->port, cutoff);
    qemu_set_irq(s->irq[0],
                 ((s->regs[ICSR1] & 0x100) && (s->regs[ICSR1] & 0xf000)) ||
                 (s->regs[OCSR1] & 0x8100) == 0x8100);
    qemu_set_irq(s->irq[1], (s->regs[ICSR2] & 0x1100) == 0x1100);
}

/* D-019: fault detection and inactive release use the same pad levels. */
static uint8_t poe_active(RX113POEState *s)
{
    uint8_t polarity = rx113_mtu_poe_polarity(s->mtu);
    uint8_t active = 0;

    for (unsigned i = 0; i < 3; i++) {
        if (!(poe_pairs(s) & (0x40 >> i))) {
            continue;
        }
        for (unsigned j = 0; j < 2; j++) {
            unsigned bit = 2 * i + j;

            if (rx113_port_mtioc_active(s->port, pair_pins[i][j],
                                        (polarity >> bit) & 1)) {
                active |= 1 << bit;
            }
        }
    }
    return active;
}

static unsigned poe_mode(RX113POEState *s, unsigned input)
{
    return input == 4 ? s->regs[ICSR2] & 3 :
                       (s->regs[ICSR1] >> (2 * input)) & 3;
}

static void poe_tick(RX113POEState *s)
{
    static const unsigned divisors[] = { 1, 8, 16, 128 };
    uint8_t inputs = rx113_port_poe_inputs(s->port);
    uint8_t active = poe_active(s);

    s->cycles++;
    if (s->stop_cycles) {
        s->stop_cycles--;
    }
    for (unsigned i = 0; i < 5; i++) {
        unsigned mode = poe_mode(s, i);
        bool high = inputs & (1 << i);
        bool detect = false;

        if (s->cycles % divisors[mode]) {
            continue;
        }
        s->high = (s->high & ~(1 << i)) | (high << i);
        if (!mode) {
            detect = !high && (s->previous & (1 << i));
        } else {
            s->samples[i] = high ? 0 : MIN(16, s->samples[i] + 1);
            detect = s->samples[i] == 16;
        }
        s->previous = (s->previous & ~(1 << i)) | (high << i);
        if (detect) {
            unsigned reg = i == 4 ? ICSR2 : ICSR1;
            uint16_t flag = 1 << (i == 4 ? 12 : 12 + i);

            s->regs[reg] |= flag;
        }
    }
    for (unsigned i = 0; i < 3; i++) {
        if (((active >> (2 * i)) & 3) == 3) {
            s->regs[OCSR1] |= 0x8000;
        }
    }
    poe_update(s);
}

static bool poe_running(RX113POEState *s)
{
    return s->regs[ICSR1] || s->regs[ICSR2] || poe_pairs(s) ||
           s->stop_cycles || rx113_port_poe_inputs(s->port) != 0x1f ||
           s->previous != 0x1f;
}

static void poe_schedule(RX113POEState *s)
{
    if (s->hz && poe_running(s)) {
        uint64_t wait = DIV_ROUND_UP(1000000000ULL - s->fraction, s->hz);

        timer_mod(s->timer, s->last_ns + wait);
    } else {
        timer_del(s->timer);
    }
}

static void poe_sync(RX113POEState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (s->hz && timer_pending(s->timer)) {
        uint64_t elapsed = (now - s->last_ns) * s->hz + s->fraction;
        uint64_t ticks = elapsed / 1000000000;

        s->fraction = elapsed % 1000000000;
        while (ticks--) {
            poe_tick(s);
        }
    }
    s->last_ns = now;
}

static void poe_timer(void *opaque)
{
    RX113POEState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t elapsed = (now - s->last_ns) * s->hz + s->fraction;

    s->fraction = elapsed % 1000000000;
    s->last_ns = now;
    for (uint64_t ticks = elapsed / 1000000000; ticks; ticks--) {
        poe_tick(s);
    }
    poe_schedule(s);
}

void rx113_poe_port_changed(DeviceState *dev)
{
    RX113POEState *s = RX113_POE(dev);

    if (!timer_pending(s->timer)) {
        s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        poe_schedule(s);
    }
}

void rx113_poe_set_clock(DeviceState *dev, uint64_t hz)
{
    RX113POEState *s = RX113_POE(dev);

    poe_sync(s);
    s->hz = hz;
    poe_schedule(s);
}

void rx113_poe_oscillator_stop(DeviceState *dev)
{
    RX113POEState *s = RX113_POE(dev);

    poe_sync(s);
    s->regs[ICSR3] |= 0x1000;
    /* Negate on a PCLK edge after at least ten complete clock periods. */
    s->stop_cycles = 10 + (s->fraction != 0);
    poe_update(s);
    poe_schedule(s);
}

static uint64_t poe_read(void *opaque, hwaddr addr, unsigned size)
{
    RX113POEState *s = opaque;
    unsigned reg = addr / 2, shift = (addr & 1) * 8;
    uint16_t mask = size == 2 ? 0xffff : 0xff << shift;

    poe_sync(s);
    s->seen[reg] |= s->regs[reg] & flags[reg] & mask;
    poe_schedule(s);
    return (s->regs[reg] & mask) >> shift;
}

static void poe_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    RX113POEState *s = opaque;
    unsigned reg = addr / 2, shift = (addr & 1) * 8;
    uint16_t mask = size == 2 ? 0xffff : 0xff << shift;
    uint16_t writable = 0, once = 0, clear;
    uint16_t val = value << shift;

    poe_sync(s);
    switch (reg) {
    case ICSR1:
        once = 0xff;
        writable = 0x100;
        break;
    case OCSR1:
        once = 0x200;
        writable = 0x100;
        break;
    case ICSR2:
        once = 0x203;
        writable = 0x100;
        break;
    case SPOER:
        once = 0xf00;
        writable = 3;
        break;
    case POECR2:
        once = 0x70;
        break;
    case ICSR3:
        /* D-020 (supersedes D-018): OSTSTE is strictly write-once. */
        once = 0x200;
        break;
    }
    /* The two ICSR2 fields can be programmed through separate byte writes. */
    if (reg == ICSR2) {
        if ((mask & 3) && !(s->locks & (1 << 2))) {
            writable |= 3;
            s->locks |= 1 << 2;
        }
        if ((mask & 0x200) && !(s->locks & (1 << 3))) {
            writable |= 0x200;
            s->locks |= 1 << 3;
        }
    } else {
        unsigned lock = reg == SPOER ? 4 : reg == POECR2 ? 5 : reg;

        if ((once & mask) && !(s->locks & (1 << lock))) {
            writable |= once;
            s->locks |= 1 << lock;
        }
    }
    clear = s->seen[reg] & flags[reg] & mask & ~val;
    if (reg == ICSR1 || reg == ICSR2) {
        for (unsigned i = 0; i < 5; i++) {
            unsigned r = i == 4 ? ICSR2 : ICSR1;
            if (r == reg && poe_mode(s, i) && !(s->high & (1 << i))) {
                clear &= ~(1 << (i == 4 ? 12 : 12 + i));
            }
        }
    } else if (reg == OCSR1 && poe_active(s)) {
        clear = 0;
    } else if (reg == ICSR3 && s->stop_cycles) {
        clear = 0;
    }
    s->regs[reg] &= ~clear;
    s->seen[reg] &= ~clear;
    if (reg == SPOER) {
        /* Setting needs no read; clearing does. */
        writable &= ~3 | val;
    }
    writable &= mask;
    s->regs[reg] = (s->regs[reg] & ~writable) | (val & writable);
    poe_update(s);
    poe_schedule(s);
}

static const MemoryRegionOps poe_ops = {
    .read = poe_read,
    .write = poe_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 2,
    .impl.min_access_size = 1,
    .impl.max_access_size = 2,
};

static void poe_reset_hold(Object *obj, ResetType type)
{
    RX113POEState *s = RX113_POE(obj);

    timer_del(s->timer);
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->seen, 0, sizeof(s->seen));
    memset(s->samples, 0, sizeof(s->samples));
    s->locks = s->stop_cycles = s->cycles = 0;
    s->previous = s->high = 0x1f;
    s->fraction = 0;
    s->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    poe_update(s);
}

static int poe_post_load(void *opaque, int version_id)
{
    RX113POEState *s = opaque;

    poe_update(s);
    return 0;
}

static const VMStateDescription vmstate_poe = {
    .name = TYPE_RX113_POE,
    .version_id = 2,
    .minimum_version_id = 1,
    .post_load = poe_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16_ARRAY(regs, RX113POEState, 8),
        VMSTATE_UINT16_ARRAY(seen, RX113POEState, 8),
        VMSTATE_UINT8(locks, RX113POEState),
        VMSTATE_UINT8(previous, RX113POEState),
        VMSTATE_UINT8(high, RX113POEState),
        VMSTATE_UINT8_ARRAY(samples, RX113POEState, 5),
        VMSTATE_UINT8(stop_cycles, RX113POEState),
        VMSTATE_UINT32(cycles, RX113POEState),
        VMSTATE_UINT64(hz, RX113POEState),
        VMSTATE_UINT64(fraction, RX113POEState),
        VMSTATE_INT64(last_ns, RX113POEState),
        VMSTATE_TIMER_PTR(timer, RX113POEState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property poe_properties[] = {
    DEFINE_PROP_LINK("port", RX113POEState, port, TYPE_RX113_PORT,
                     RX113PortState *),
    DEFINE_PROP_LINK("mtu", RX113POEState, mtu, TYPE_RX113_MTU, DeviceState *),
};

static void poe_init(Object *obj)
{
    RX113POEState *s = RX113_POE(obj);

    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, poe_timer, s);
    memory_region_init_io(&s->iomem, obj, &poe_ops, s, TYPE_RX113_POE, 0x10);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    for (unsigned i = 0; i < 2; i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq[i]);
    }
}

static void poe_finalize(Object *obj)
{
    timer_free(RX113_POE(obj)->timer);
}

static void poe_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_poe;
    dc->user_creatable = false;
    device_class_set_props(dc, poe_properties);
    RESETTABLE_CLASS(klass)->phases.hold = poe_reset_hold;
}

static const TypeInfo poe_info = {
    .name = TYPE_RX113_POE,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RX113POEState),
    .instance_init = poe_init,
    .instance_finalize = poe_finalize,
    .class_init = poe_class_init,
};

static void poe_register_types(void)
{
    type_register_static(&poe_info);
}

type_init(poe_register_types)
