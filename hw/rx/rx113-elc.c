/*
 * RX113 event link controller (R01UH0448EJ0120, section 17)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/rx/rx113_elc.h"
#include "hw/rx/rx113_mtu.h"
#include "hw/timer/renesas_cmt.h"
#include "hw/timer/renesas_tmr.h"
#include "migration/vmstate.h"

#define ELC_REGS 0x30
#define ELC_SOURCES 0x6b
#define ELC_DESTS 26
#define ELCR 0
#define ELOPA 0x1f
#define ELOPB 0x20
#define ELOPC 0x21
#define ELOPD 0x22
#define ELSEGR 0x2d

struct RX113ELCState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    DeviceState *mtu_dev;
    RCMTState *cmt;
    RTMRState *tmr0;
    RTMRState *tmr2;
    qemu_irq link[ELC_DESTS];
    qemu_irq mtu[12];
    QEMUTimer *timer;
    uint8_t regs[ELC_REGS];
    uint8_t levels[ELC_SOURCES];
    uint32_t pending[ELC_DESTS];
    uint64_t hz;
    bool stopped;
};

static const uint8_t destinations[] = {
    1, 2, 3, 4, 7, 10, 12, 14, 15, 17, 18, 19, 20, 22, 24, 25,
};

static bool elc_source(unsigned id)
{
    return (id >= 0x08 && id <= 0x1a) || id == 0x1f ||
           (id >= 0x22 && id <= 0x24) ||
           (id >= 0x28 && id <= 0x2a) ||
           (id >= 0x3a && id <= 0x3d) ||
           (id >= 0x4e && id <= 0x51) ||
           (id >= 0x58 && id <= 0x5b) || id == 0x5d ||
           id == 0x61 || id == 0x63 || id == 0x65 || id == 0x66 ||
           id == 0x69 || id == 0x6a;
}

static bool elc_destination(unsigned offset)
{
    for (unsigned i = 0; i < ARRAY_SIZE(destinations); i++) {
        if (offset == destinations[i] + 1) {
            return true;
        }
    }
    return false;
}

static bool elc_selection(unsigned n, unsigned source)
{
    if (!source) {
        return true;
    }
    if (!elc_source(source)) {
        return false;
    }
    if (n == 18) {
        return source >= 0x63 && source <= 0x6a;
    }
    if (n == 19) {
        return source == 0x5d;
    }
    return !((n == 24 || n == 25) && source == 0x6a);
}

static void elc_subscribe(RX113ELCState *s)
{
    uint32_t mask = 0;

    if (!s->stopped && (s->regs[ELCR] & 0x80)) {
        for (unsigned i = 0; i < ARRAY_SIZE(destinations); i++) {
            unsigned source = s->regs[destinations[i] + 1];

            if (source >= 0x08 && source <= 0x1a) {
                mask |= 1U << source;
            }
        }
    }
    rx113_mtu_set_elc_events(s->mtu_dev, mask);
}

static void elc_options(RX113ELCState *s)
{
    renesas_cmt_set_elc_mode(s->cmt, 1, (s->regs[ELOPC] >> 2) & 3);
    renesas_tmr_set_elc_mode(s->tmr0, s->regs[ELOPD] & 3);
    renesas_tmr_set_elc_mode(s->tmr2, (s->regs[ELOPD] >> 4) & 3);
}

static void elc_cancel(RX113ELCState *s)
{
    timer_del(s->timer);
    memset(s->pending, 0, sizeof(s->pending));
}

static void elc_event(RX113ELCState *s, unsigned source)
{
    bool pending = false;

    if (s->stopped || !(s->regs[ELCR] & 0x80)) {
        return;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(destinations); i++) {
        unsigned n = destinations[i];

        if (s->regs[n + 1] == source) {
            if (s->pending[n] != UINT32_MAX) {
                s->pending[n]++;
            } else {
                qemu_log_mask(LOG_GUEST_ERROR, "rx113-elc: event overflow\n");
            }
            pending = true;
        }
    }
    /* Separate source and destination callbacks, including cyclic links. */
    if (pending && !timer_pending(s->timer)) {
        timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  DIV_ROUND_UP(NANOSECONDS_PER_SECOND, s->hz));
    }
}

static void elc_input(void *opaque, int n, int level)
{
    RX113ELCState *s = opaque;
    bool rising = level && !s->levels[n];

    s->levels[n] = !!level;
    if (rising && elc_source(n)) {
        elc_event(s, n);
    }
}

static void elc_deliver(void *opaque)
{
    RX113ELCState *s = opaque;
    uint32_t pending[ELC_DESTS];

    memcpy(pending, s->pending, sizeof(pending));
    memset(s->pending, 0, sizeof(s->pending));
    for (unsigned i = 0; i < ARRAY_SIZE(destinations); i++) {
        unsigned n = destinations[i];
        qemu_irq output = s->link[n];

        if (n >= 1 && n <= 4) {
            unsigned mode = n == 4 ? s->regs[ELOPB] & 3 :
                            (s->regs[ELOPA] >> (n * 2)) & 3;

            if (mode == 3) {
                continue;
            }
            output = s->mtu[(n - 1) * 3 + mode];
        }
        if (pending[n] && !qemu_irq_is_connected(output)) {
            qemu_log_mask(LOG_UNIMP,
                          "rx113-elc: ELSR%u destination unavailable\n", n);
        }
        while (pending[n]--) {
            qemu_irq_pulse(output);
        }
    }
}

void rx113_elc_set_clock(DeviceState *dev, uint64_t hz, bool stopped)
{
    RX113ELCState *s = RX113_ELC(dev);

    assert(hz && hz <= NANOSECONDS_PER_SECOND);
    s->hz = hz;
    s->stopped = stopped;
    elc_subscribe(s);
    if (stopped) {
        elc_cancel(s);
    }
}

static uint64_t elc_read(void *opaque, hwaddr addr, unsigned size)
{
    RX113ELCState *s = opaque;

    return s->stopped ? 0 : s->regs[addr];
}

static void elc_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    RX113ELCState *s = opaque;

    if (s->stopped) {
        return;
    }
    if (elc_destination(addr)) {
        if (!elc_selection(addr - 1, val)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "rx113-elc: prohibited ELSR%u value 0x%02x\n",
                          (unsigned)addr - 1, (unsigned)val);
            return;
        }
        s->regs[addr] = val;
        s->pending[addr - 1] = 0;
        elc_subscribe(s);
        return;
    }
    switch (addr) {
    case ELCR:
        s->regs[addr] = val | 0x7f;
        elc_subscribe(s);
        if (!(val & 0x80)) {
            elc_cancel(s);
        }
        break;
    case ELOPA:
        s->regs[addr] = val | 3;
        break;
    case ELOPB:
        s->regs[addr] = val | 0xfc;
        break;
    case ELOPC:
        s->regs[addr] = val | 0xf3;
        elc_options(s);
        break;
    case ELOPD:
        s->regs[addr] = val | 0xcc;
        elc_options(s);
        break;
    case ELSEGR:
        if (!(val & 0x80)) {
            bool generate = (s->regs[addr] & 0x40) && (val & 1);

            s->regs[addr] = (val & 0x40) | 0xbe;
            if (generate) {
                elc_event(s, 0x69);
            }
        }
        break;
    case 0x23: /* PGR1 */
    case 0x27: /* PDBF1 */
        s->regs[addr] = val;
        goto port_unimplemented;
    case 0x25: /* PGC1 */
        s->regs[addr] = val | 0x88;
        goto port_unimplemented;
    case 0x29: /* PEL0 */
    case 0x2a: /* PEL1 */
        s->regs[addr] = val | 0x80;
port_unimplemented:
        qemu_log_mask(LOG_UNIMP, "rx113-elc: PORTB linkage unavailable\n");
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "rx113-elc: reserved register 0x%02x\n",
                      (unsigned)addr);
        break;
    }
}

static const MemoryRegionOps elc_ops = {
    .read = elc_read,
    .write = elc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
};

static void elc_reset_hold(Object *obj, ResetType type)
{
    RX113ELCState *s = RX113_ELC(obj);

    elc_cancel(s);
    memset(s->regs, 0, sizeof(s->regs));
    s->regs[ELCR] = 0x7f;
    memset(s->regs + ELOPA, 0xff, 4);
    s->regs[0x25] = 0x88;
    s->regs[0x29] = s->regs[0x2a] = 0x80;
    s->regs[ELSEGR] = 0xbe;
}

static int elc_post_load(void *opaque, int version_id)
{
    RX113ELCState *s = opaque;

    if (!s->hz || s->hz > NANOSECONDS_PER_SECOND) {
        return -EINVAL;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(destinations); i++) {
        unsigned n = destinations[i];

        if (!elc_selection(n, s->regs[n + 1])) {
            return -EINVAL;
        }
    }
    /* Timer modes migrate with the timers; do not synchronize them here. */
    return 0;
}

static const VMStateDescription vmstate_elc = {
    .name = TYPE_RX113_ELC,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = elc_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, RX113ELCState, ELC_REGS),
        VMSTATE_UINT8_ARRAY(levels, RX113ELCState, ELC_SOURCES),
        VMSTATE_UINT32_ARRAY(pending, RX113ELCState, ELC_DESTS),
        VMSTATE_UINT64(hz, RX113ELCState),
        VMSTATE_BOOL(stopped, RX113ELCState),
        VMSTATE_TIMER_PTR(timer, RX113ELCState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property elc_properties[] = {
    DEFINE_PROP_LINK("mtu", RX113ELCState, mtu_dev, TYPE_RX113_MTU,
                     DeviceState *),
    DEFINE_PROP_LINK("cmt", RX113ELCState, cmt, TYPE_RENESAS_CMT, RCMTState *),
    DEFINE_PROP_LINK("tmr0", RX113ELCState, tmr0, TYPE_RENESAS_TMR,
                     RTMRState *),
    DEFINE_PROP_LINK("tmr2", RX113ELCState, tmr2, TYPE_RENESAS_TMR,
                     RTMRState *),
};

static void elc_init(Object *obj)
{
    RX113ELCState *s = RX113_ELC(obj);

    s->hz = 500000;
    s->stopped = true;
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, elc_deliver, s);
    memory_region_init_io(&s->iomem, obj, &elc_ops, s, TYPE_RX113_ELC,
                          ELC_REGS);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    qdev_init_gpio_in_named(DEVICE(obj), elc_input, "event", ELC_SOURCES);
    qdev_init_gpio_out_named(DEVICE(obj), s->link, "link", ELC_DESTS);
    qdev_init_gpio_out_named(DEVICE(obj), s->mtu, "mtu", 12);
}

static void elc_finalize(Object *obj)
{
    timer_free(RX113_ELC(obj)->timer);
}

static void elc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->user_creatable = false;
    dc->vmsd = &vmstate_elc;
    device_class_set_props(dc, elc_properties);
    rc->phases.hold = elc_reset_hold;
}

static const TypeInfo elc_info = {
    .name = TYPE_RX113_ELC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RX113ELCState),
    .instance_init = elc_init,
    .instance_finalize = elc_finalize,
    .class_init = elc_class_init,
};

static void elc_register_types(void)
{
    type_register_static(&elc_info);
}

type_init(elc_register_types)
