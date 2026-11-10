/*
 * Renesas RX113 data transfer controller (DTCa)
 * R01UH0448EJ0120 Rev.1.20, sections 11, 14 and 16.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/intc/rx_icu.h"
#include "hw/rx/rx113_dtc.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"

/* One timer step per descriptor, not a simulation of bus execution cycles. */
#define DTC_STEP_NS 1

struct RX113DTCState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    MemoryRegion icu_mmio;
    RXICUState *icu;
    QEMUTimer *timer;
    qemu_irq event;
    uint8_t pending[NR_IRQS];
    uint8_t level[NR_IRQS];
    uint8_t retained[NR_IRQS];
    uint8_t irq_sense[8];
    uint8_t owned[NR_IRQS];
    uint32_t vbr;
    uint8_t control;
    uint8_t short_address;
    uint8_t start;
    bool stopped;
    bool active;
    uint8_t vector;
    uint32_t info;
    uint32_t sar;
    uint32_t dar;
    uint16_t cra;
    uint16_t crb;
    uint8_t mra;
    uint8_t mrb;
    bool chained;
    bool cache_valid;
    uint8_t cache_vector;
};

/* Table 14.3: only these vectors have a DTCER register. */
static bool dtc_vector(unsigned n)
{
    switch (n) {
    case 27 ... 31:
    case 36 ... 37:
    case 45 ... 46:
    case 58 ... 61:
    case 64 ... 71:
    case 102 ... 103:
    case 106:
    case 109 ... 110:
    case 114 ... 117:
    case 121 ... 122:
    case 125 ... 126:
    case 129 ... 132:
    case 134 ... 141:
    case 174 ... 175:
    case 177 ... 178:
    case 180 ... 181:
    case 183 ... 184:
    case 187 ... 188:
    case 215 ... 216:
    case 219 ... 220:
    case 223 ... 224:
    case 227 ... 228:
    case 231 ... 232:
    case 235 ... 236:
    case 239 ... 240:
    case 247 ... 248:
        return true;
    default:
        return false;
    }
}

static int dtc_next(RX113DTCState *s)
{
    unsigned n;

    /* §14.4.4: IPR is irrelevant to DTC; smallest vector wins. */
    for (n = 0; n < NR_IRQS; n++) {
        if (s->pending[n] && s->icu->ir[n] &&
            (s->icu->ier[n / 8] & (1 << (n % 8)))) {
            return n;
        }
    }
    return -1;
}

static void dtc_cpu_request(RX113DTCState *s, unsigned n);

/* §14.4.1.1: serial interfaces retain one request while IR is set. */
static bool dtc_serial_vector(unsigned n)
{
    return n == 45 || n == 46 || n == 109 || n == 110 ||
           n == 187 || n == 188 || (n >= 215 && n <= 240 &&
           ((n - 215) % 4 < 2)) || n == 247 || n == 248;
}

static void dtc_release_retained(RX113DTCState *s)
{
    for (unsigned n = 0; n < NR_IRQS; n++) {
        if (s->retained[n] && !s->icu->ir[n]) {
            s->retained[n] = 0;
            s->icu->ir[n] = 1;
            if (s->icu->dtcer[n]) {
                s->pending[n] = 1;
            } else {
                dtc_cpu_request(s, n);
            }
        }
    }
}

static void dtc_kick(RX113DTCState *s)
{
    dtc_release_retained(s);
    /* Changing the destination to CPU also works while the DTC is stopped. */
    for (unsigned n = 0; n < NR_IRQS; n++) {
        if (s->pending[n] && !s->icu->dtcer[n] && s->icu->ir[n] &&
            (s->icu->ier[n / 8] & (1 << (n % 8)))) {
            s->pending[n] = 0;
            dtc_cpu_request(s, n);
        }
    }
    if (!timer_pending(s->timer) &&
        (s->active || (!s->stopped && s->start && dtc_next(s) >= 0))) {
        timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  DTC_STEP_NS);
    }
}

/* Deliver the retained request through the unchanged shared ICU. */
static void dtc_cpu_request(RX113DTCState *s, unsigned n)
{
    qemu_irq irq = qdev_get_gpio_in(DEVICE(s->icu), n);
    struct IRQSource *src = &s->icu->src[n];
    int level = src->sense == TRG_NEDGE ? 0 : 1;

    src->level = !level;
    qemu_set_irq(irq, level);
    src->level = s->level[n];
}

static void dtc_input(void *opaque, int n, int level)
{
    RX113DTCState *s = opaque;
    unsigned sense = s->icu->src[n].sense;
    bool issue;

    level = !!level;
    issue = sense == TRG_LEVEL ? level :
            sense == TRG_PEDGE ? level && !s->level[n] :
            sense == TRG_NEDGE ? !level && s->level[n] :
            level != s->level[n];
    s->level[n] = level;
    if (!s->icu->dtcer[n]) {
        qemu_set_irq(qdev_get_gpio_in(DEVICE(s->icu), n), level);
        return;
    }
    /* §14.4.3 requires edge detection for external IRQ DTC activation. */
    s->icu->src[n].level = level;
    if (sense == TRG_LEVEL) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "rx113-dtc: level-sensitive DTC source %d\n", n);
        return;
    }
    if (issue && s->icu->ir[n] && dtc_serial_vector(n)) {
        s->retained[n] = 1;
    }
    if (issue && !s->icu->ir[n]) {
        s->icu->ir[n] = 1;
        s->pending[n] = 1;
        dtc_kick(s);
    }
}

/* Keep DTC-owned IR flags out of the shared ICU's CPU-ack rescan. */
static void dtc_ack(void *opaque, int n, int level)
{
    RX113DTCState *s = opaque;
    uint8_t held[NR_IRQS] = { 0 };

    for (unsigned i = 0; i < NR_IRQS; i++) {
        if (s->icu->dtcer[i] &&
            (s->pending[i] || (s->active && s->vector == i))) {
            held[i] = s->icu->ir[i];
            s->icu->ir[i] = 0;
        }
    }
    qemu_set_irq(qdev_get_gpio_in_named(DEVICE(s->icu), "ack", 0), level);
    for (unsigned i = 0; i < NR_IRQS; i++) {
        s->icu->ir[i] |= held[i];
    }
    dtc_kick(s);
}

static bool dtc_read32(uint32_t addr, uint32_t *value)
{
    uint8_t bytes[4];

    if (address_space_read(&address_space_memory, addr,
                           MEMTXATTRS_UNSPECIFIED, bytes, 4) != MEMTX_OK) {
        return false;
    }
    *value = ldl_le_p(bytes);
    return true;
}

static bool dtc_write32(uint32_t addr, uint32_t value)
{
    uint8_t bytes[4];

    stl_le_p(bytes, value);
    return address_space_write(&address_space_memory, addr,
                                MEMTXATTRS_UNSPECIFIED, bytes, 4) == MEMTX_OK;
}

static uint32_t dtc_address(RX113DTCState *s, uint32_t addr)
{
    return s->short_address ? sextract32(addr, 0, 24) : addr;
}

static bool dtc_load(RX113DTCState *s)
{
    uint32_t a, b, c, d;

    s->info &= ~3U; /* §16.10.1: unaligned descriptor low bits ignored. */
    if (!dtc_read32(s->info, &a) || !dtc_read32(s->info + 4, &b) ||
        !dtc_read32(s->info + 8, &c)) {
        return false;
    }
    s->mra = a >> 24;
    if (s->short_address) {
        s->mrb = b >> 24;
        s->sar = dtc_address(s, a);
        s->dar = dtc_address(s, b);
    } else {
        if (!dtc_read32(s->info + 12, &d)) {
            return false;
        }
        s->mrb = a >> 16;
        s->sar = b;
        s->dar = c;
        c = d;
    }
    s->cra = c >> 16;
    s->crb = c;
    return true;
}

static int dtc_increment(unsigned mode, unsigned size)
{
    return mode < 2 ? 0 : mode == 2 ? size : -(int)size;
}

static bool dtc_writeback(RX113DTCState *s)
{
    unsigned off = s->short_address ? 0 : 4;
    uint32_t sar = s->sar, dar = s->dar;

    if (s->short_address) {
        sar = (sar & 0xffffff) | ((uint32_t)s->mra << 24);
        dar = (dar & 0xffffff) | ((uint32_t)s->mrb << 24);
    }
    return (!(s->mra & 8) || dtc_write32(s->info + off, sar)) &&
           (!(s->mrb & 8) || dtc_write32(s->info + off + 4, dar)) &&
           dtc_write32(s->info + off + 8, ((uint32_t)s->cra << 16) | s->crb);
}

static void dtc_fail(RX113DTCState *s)
{
    /* No invented hardware error register or host-memory fallback. */
    qemu_log_mask(LOG_GUEST_ERROR,
                  "rx113-dtc: invalid transfer or bus error, vector %u\n",
                  s->vector);
    s->cache_valid = false;
    s->active = false;
    s->start = 0;
}

static void dtc_step(void *opaque)
{
    RX113DTCState *s = opaque;
    unsigned mode, size, count, repeat_size;
    int sinc, dinc;
    bool boundary, terminal, chain;

    if (!s->active) {
        int n = dtc_next(s);

        if (s->stopped || !s->start || n < 0) {
            return;
        }
        s->pending[n] = 0;
        if (!s->icu->dtcer[n]) {
            dtc_cpu_request(s, n);
            dtc_kick(s);
            return;
        }
        s->vector = n;
        s->active = true;
        s->chained = false;
        if (!(s->control & 0x10) || !s->cache_valid ||
            s->cache_vector != n) {
            if (!dtc_read32(s->vbr + 4 * n, &s->info) || !dtc_load(s)) {
                dtc_fail(s);
                return;
            }
        }
        s->cache_valid = false;
    } else if (!dtc_load(s)) {
        dtc_fail(s);
        return;
    }

    mode = s->mra >> 6;
    size = (s->mra >> 4) & 3;
    if (mode == 3 || size == 3) {
        dtc_fail(s);
        return;
    }
    size = 1 << size;
    sinc = dtc_increment((s->mra >> 2) & 3, size);
    dinc = dtc_increment((s->mrb >> 2) & 3, size);
    repeat_size = s->cra >> 8;
    repeat_size = repeat_size ? repeat_size : 256;
    count = mode == 2 ? (s->cra & 0xff) : 1;
    count = count ? count : 256;
    boundary = mode == 0 ? s->cra == 1 :
               mode == 1 ? (s->cra & 0xff) == 1 : s->crb == 1;
    terminal = mode != 1 && boundary;
    chain = (s->mrb & 0x80) && (!(s->mrb & 0x40) || boundary);

    /* §16.3: the last descriptor determines IR clearing and completion. */
    if (!chain && !terminal && !(s->mrb & 0x20)) {
        s->icu->ir[s->vector] = 0;
        dtc_release_retained(s);
    }
    for (unsigned i = 0; i < count; i++) {
        uint8_t data[4];

        if (address_space_read(&address_space_memory,
                               dtc_address(s, s->sar),
                               MEMTXATTRS_UNSPECIFIED, data, size) !=
            MEMTX_OK ||
            address_space_write(&address_space_memory,
                                dtc_address(s, s->dar),
                                MEMTXATTRS_UNSPECIFIED, data, size) !=
            MEMTX_OK) {
            dtc_fail(s);
            return;
        }
        s->sar += sinc;
        s->dar += dinc;
    }
    if (mode == 0) {
        s->cra--;
    } else if (mode == 2 || (s->cra & 0xff) == 1) {
        if (s->mrb & 0x10) {
            s->sar -= sinc * repeat_size;
        } else {
            s->dar -= dinc * repeat_size;
        }
        s->cra = (s->cra & 0xff00) | (s->cra >> 8);
        if (mode == 2) {
            s->crb--;
        }
    } else {
        s->cra = (s->cra & 0xff00) | ((s->cra - 1) & 0xff);
    }
    if (!dtc_writeback(s)) {
        dtc_fail(s);
        return;
    }
    if (chain) {
        s->chained = true;
        s->info += s->short_address ? 12 : 16;
    } else {
        s->cache_valid = !s->chained && !terminal;
        s->cache_vector = s->vector;
        if (terminal) {
            s->icu->dtcer[s->vector] = 0;
        }
        if (terminal || (s->mrb & 0x20)) {
            dtc_cpu_request(s, s->vector);
        }
        s->active = false;
        qemu_irq_pulse(s->event);
    }
    dtc_kick(s);
}

void rx113_dtc_set_stopped(DeviceState *dev, bool stopped)
{
    RX113DTCState *s = RX113_DTC(dev);

    s->stopped = stopped;
    dtc_kick(s);
}

static uint64_t dtc_read(void *opaque, hwaddr addr, unsigned size)
{
    RX113DTCState *s = opaque;

    if (s->stopped && !s->active) {
        return 0;
    }
    if (addr == 0 && size == 1) {
        return s->control;
    }
    if (addr == 4 && size == 4) {
        return s->vbr;
    }
    if (addr == 8 && size == 1) {
        return s->short_address;
    }
    if (addr == 12 && size == 1) {
        return s->start;
    }
    if (addr == 14 && size == 2) {
        return (s->active ? 0x8000 : 0) | s->vector;
    }
    qemu_log_mask(LOG_GUEST_ERROR, "rx113-dtc: invalid register read\n");
    return 0;
}

static void dtc_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    RX113DTCState *s = opaque;

    if (s->stopped && !s->active) {
        return;
    }
    if (addr == 0 && size == 1) {
        s->control = (value & 0x10) | 8;
        if (!(value & 0x10)) {
            s->cache_valid = false;
        }
    } else if (addr == 4 && size == 4) {
        s->vbr = sextract32(value & 0xffffc00, 0, 28);
    } else if (addr == 8 && size == 1) {
        s->short_address = value & 1;
    } else if (addr == 12 && size == 1) {
        s->start = value & 1;
        dtc_kick(s);
    } else if (addr != 14 || size != 2) {
        qemu_log_mask(LOG_GUEST_ERROR, "rx113-dtc: invalid register write\n");
    }
}

/*
 * RX113-only ICU front end: DTCER uses the ICU's migratable backing array.
 * Other accesses retain the shared implementation, including its quirks.
 */
static uint64_t dtc_icu_read(void *opaque, hwaddr addr, unsigned size)
{
    RX113DTCState *s = opaque;
    uint64_t value = 0;

    if (addr >= 0x100 && addr < 0x200) {
        return size == 1 && dtc_vector(addr - 0x100) ?
               s->icu->dtcer[addr - 0x100] : 0;
    }
    memory_region_dispatch_read(&s->icu->memory, addr, &value,
                                size_memop(size) | MO_LE,
                                MEMTXATTRS_UNSPECIFIED);
    return value;
}

static void dtc_icu_write(void *opaque, hwaddr addr, uint64_t value,
                          unsigned size)
{
    RX113DTCState *s = opaque;

    if (addr >= 0x100 && addr < 0x200) {
        unsigned n = addr - 0x100;

        if (size == 1 && dtc_vector(n)) {
            s->icu->dtcer[n] = value & 1;
            s->owned[n] |= value & 1;
            if (s->icu->ir[n]) {
                s->pending[n] = 1;
            }
        }
    } else {
        memory_region_dispatch_write(&s->icu->memory, addr, value,
                                     size_memop(size) | MO_LE,
                                     MEMTXATTRS_UNSPECIFIED);
        if (addr < 0x100 && !s->icu->ir[addr]) {
            s->pending[addr] = 0;
        }
    }
    dtc_kick(s);
}

static const MemoryRegionOps dtc_ops = {
    .read = dtc_read,
    .write = dtc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static const MemoryRegionOps dtc_icu_ops = {
    .read = dtc_icu_read,
    .write = dtc_icu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 2 },
    .impl = { .min_access_size = 1, .max_access_size = 2 },
};

static void dtc_reset_hold(Object *obj, ResetType type)
{
    RX113DTCState *s = RX113_DTC(obj);

    timer_del(s->timer);
    for (unsigned n = 0; n < NR_IRQS; n++) {
        if (s->owned[n] || s->pending[n] || s->icu->dtcer[n] ||
            (s->active && s->vector == n)) {
            s->icu->ir[n] = 0;
            if (qatomic_read(&s->icu->req_irq) == n) {
                qatomic_set(&s->icu->req_irq, -1);
                qemu_set_irq((s->icu->fir & 0x8000) &&
                             (s->icu->fir & 255) == n ?
                             s->icu->_fir : s->icu->_irq, 0);
            }
        }
        s->icu->dtcer[n] = 0;
    }
    memset(s->pending, 0, sizeof(s->pending));
    memset(s->retained, 0, sizeof(s->retained));
    memset(s->owned, 0, sizeof(s->owned));
    s->vbr = 0;
    s->control = 8;
    s->short_address = 0;
    s->start = 0;
    s->active = false;
    s->vector = 0;
    s->cache_valid = false;
    s->chained = false;
}

static void dtc_realize(DeviceState *dev, Error **errp)
{
    RX113DTCState *s = RX113_DTC(dev);

    if (!s->icu) {
        error_setg(errp, "rx113-dtc requires an ICU link");
    }
}

static void dtc_init(Object *obj)
{
    RX113DTCState *s = RX113_DTC(obj);

    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, dtc_step, s);
    memory_region_init_io(&s->iomem, obj, &dtc_ops, s, TYPE_RX113_DTC, 0x10);
    memory_region_init_io(&s->icu_mmio, obj, &dtc_icu_ops, s,
                          "rx113-dtc-icu", 0x600);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->icu_mmio);
    qdev_init_gpio_in(DEVICE(obj), dtc_input, NR_IRQS);
    qdev_init_gpio_in_named(DEVICE(obj), dtc_ack, "ack", 1);
    qdev_init_gpio_out_named(DEVICE(obj), &s->event, "transfer-end", 1);
}

static void dtc_finalize(Object *obj)
{
    timer_free(RX113_DTC(obj)->timer);
}

static int dtc_pre_save(void *opaque)
{
    RX113DTCState *s = opaque;

    for (unsigned i = 0; i < 8; i++) {
        s->irq_sense[i] = s->icu->src[64 + i].sense;
    }
    return 0;
}

static int dtc_post_load(void *opaque, int version_id)
{
    RX113DTCState *s = opaque;

    for (unsigned i = 0; i < NR_IRQS; i++) {
        if (s->icu->dtcer[i]) {
            s->icu->src[i].level = s->level[i];
            if (i >= 64 && i < 72) {
                s->icu->src[i].sense = s->irq_sense[i - 64] & 3;
            }
        }
    }
    return 0;
}

static const VMStateDescription vmstate_dtc = {
    .name = TYPE_RX113_DTC,
    .pre_save = dtc_pre_save,
    .post_load = dtc_post_load,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(pending, RX113DTCState, NR_IRQS),
        VMSTATE_UINT8_ARRAY(level, RX113DTCState, NR_IRQS),
        VMSTATE_UINT8_ARRAY(retained, RX113DTCState, NR_IRQS),
        VMSTATE_UINT8_ARRAY(irq_sense, RX113DTCState, 8),
        VMSTATE_UINT8_ARRAY(owned, RX113DTCState, NR_IRQS),
        VMSTATE_UINT32(vbr, RX113DTCState),
        VMSTATE_UINT8(control, RX113DTCState),
        VMSTATE_UINT8(short_address, RX113DTCState),
        VMSTATE_UINT8(start, RX113DTCState),
        VMSTATE_BOOL(stopped, RX113DTCState),
        VMSTATE_BOOL(active, RX113DTCState),
        VMSTATE_UINT8(vector, RX113DTCState),
        VMSTATE_UINT32(info, RX113DTCState),
        VMSTATE_UINT32(sar, RX113DTCState),
        VMSTATE_UINT32(dar, RX113DTCState),
        VMSTATE_UINT16(cra, RX113DTCState),
        VMSTATE_UINT16(crb, RX113DTCState),
        VMSTATE_UINT8(mra, RX113DTCState),
        VMSTATE_UINT8(mrb, RX113DTCState),
        VMSTATE_BOOL(chained, RX113DTCState),
        VMSTATE_BOOL(cache_valid, RX113DTCState),
        VMSTATE_UINT8(cache_vector, RX113DTCState),
        VMSTATE_TIMER_PTR(timer, RX113DTCState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property dtc_properties[] = {
    DEFINE_PROP_LINK("icu", RX113DTCState, icu, TYPE_RX_ICU, RXICUState *),
};

static void dtc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = dtc_realize;
    dc->vmsd = &vmstate_dtc;
    rc->phases.hold = dtc_reset_hold;
    device_class_set_props(dc, dtc_properties);
}

static const TypeInfo dtc_info = {
    .name = TYPE_RX113_DTC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RX113DTCState),
    .instance_init = dtc_init,
    .instance_finalize = dtc_finalize,
    .class_init = dtc_class_init,
};

static void dtc_register_types(void)
{
    type_register_static(&dtc_info);
}

type_init(dtc_register_types)
