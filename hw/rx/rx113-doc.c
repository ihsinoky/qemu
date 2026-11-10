/*
 * Renesas RX113 data operation circuit
 * Hardware manual: R01UH0448EJ0120 Rev.1.20, section 40.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/rx/rx113_doc.h"
#include "migration/vmstate.h"

#define OMS 0x03
#define DCSEL 0x04
#define DOPCIE 0x10
#define DOPCF 0x20
#define DOPCFCL 0x40

struct RX113DOCState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    qemu_irq event;
    uint8_t docr;
    uint16_t dodir;
    uint16_t dodsr;
    bool stopped;
    bool has_elc;
};

static void rx113_doc_update_irq(RX113DOCState *s)
{
    qemu_set_irq(s->irq, (s->docr & (DOPCIE | DOPCF)) == (DOPCIE | DOPCF));
}

void rx113_doc_set_stopped(DeviceState *dev, bool stopped)
{
    RX113_DOC(dev)->stopped = stopped;
}

static void rx113_doc_operate(RX113DOCState *s)
{
    bool condition;
    uint32_t sum;

    switch (s->docr & OMS) {
    case 0:
        condition = (s->dodir == s->dodsr) == !!(s->docr & DCSEL);
        break;
    case 1:
        sum = (uint32_t)s->dodsr + s->dodir;
        condition = sum > UINT16_MAX;
        s->dodsr = sum;
        break;
    case 2:
        condition = s->dodsr < s->dodir;
        s->dodsr -= s->dodir;
        break;
    default:
        /* No arithmetic is defined for the prohibited OMS encoding. */
        qemu_log_mask(LOG_GUEST_ERROR, "rx113-doc: prohibited OMS setting\n");
        return;
    }
    if (condition) {
        s->docr |= DOPCF;
        rx113_doc_update_irq(s);
        /* Section 40.5.1: every condition, independent of DOPCIE/DOPCF. */
        if (s->has_elc) {
            qemu_irq_pulse(s->event);
        }
    }
}

static uint64_t rx113_doc_read(void *opaque, hwaddr addr, unsigned size)
{
    RX113DOCState *s = opaque;

    if (s->stopped) {
        return 0;
    }
    switch (addr) {
    case 0:
        if (size == 1) {
            return s->docr;
        }
        break;
    case 2:
        if (size == 2) {
            return s->dodir;
        }
        break;
    case 4:
        if (size == 2) {
            return s->dodsr;
        }
        break;
    }
    qemu_log_mask(LOG_GUEST_ERROR, "rx113-doc: invalid read at 0x%"
                  HWADDR_PRIx " size %u\n", addr, size);
    return 0;
}

static void rx113_doc_write(void *opaque, hwaddr addr, uint64_t value,
                            unsigned size)
{
    RX113DOCState *s = opaque;

    if (s->stopped) {
        return;
    }
    switch (addr) {
    case 0:
        if (size == 1) {
            s->docr = (s->docr & DOPCF) | (value & (OMS | DCSEL | DOPCIE));
            if (value & DOPCFCL) {
                s->docr &= ~DOPCF;
            }
            rx113_doc_update_irq(s);
            return;
        }
        break;
    case 2:
        if (size == 2) {
            s->dodir = value;
            rx113_doc_operate(s);
            return;
        }
        break;
    case 4:
        if (size == 2) {
            s->dodsr = value;
            return;
        }
        break;
    }
    qemu_log_mask(LOG_GUEST_ERROR, "rx113-doc: invalid write at 0x%"
                  HWADDR_PRIx " size %u\n", addr, size);
}

/* Table 5.1: DOCR is byte-accessible; DODIR/DODSR require word access. */
static const MemoryRegionOps rx113_doc_ops = {
    .read = rx113_doc_read,
    .write = rx113_doc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    /* Keep wrong-width requests whole for the register checks above. */
    .valid.unaligned = true,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
    .impl.unaligned = true,
};

static void rx113_doc_reset_hold(Object *obj, ResetType type)
{
    RX113DOCState *s = RX113_DOC(obj);

    s->docr = 0;
    s->dodir = 0;
    s->dodsr = 0;
    rx113_doc_update_irq(s);
}

static int rx113_doc_post_load(void *opaque, int version_id)
{
    rx113_doc_update_irq(opaque);
    return 0;
}

static const VMStateDescription vmstate_rx113_doc = {
    .name = TYPE_RX113_DOC,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = rx113_doc_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(docr, RX113DOCState),
        VMSTATE_UINT16(dodir, RX113DOCState),
        VMSTATE_UINT16(dodsr, RX113DOCState),
        VMSTATE_BOOL(stopped, RX113DOCState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property rx113_doc_properties[] = {
    DEFINE_PROP_BOOL("has-elc", RX113DOCState, has_elc, false),
};

static void rx113_doc_init(Object *obj)
{
    RX113DOCState *s = RX113_DOC(obj);

    s->stopped = true;
    memory_region_init_io(&s->iomem, obj, &rx113_doc_ops, s,
                          TYPE_RX113_DOC, 0x10);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    qdev_init_gpio_out_named(DEVICE(obj), &s->event, "event", 1);
}

static void rx113_doc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->user_creatable = false;
    dc->vmsd = &vmstate_rx113_doc;
    device_class_set_props(dc, rx113_doc_properties);
    rc->phases.hold = rx113_doc_reset_hold;
}

static const TypeInfo rx113_doc_type = {
    .name = TYPE_RX113_DOC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RX113DOCState),
    .instance_init = rx113_doc_init,
    .class_init = rx113_doc_class_init,
};

static void rx113_doc_register_types(void)
{
    type_register_static(&rx113_doc_type);
}

type_init(rx113_doc_register_types)
