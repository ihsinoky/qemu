/*
 * Renesas RX113 LCD controller/driver (LCDC)
 * Datasheet: R01UH0448EJ0120 Rev.1.20, section 34
 *
 * Models the registers (34.2), the display data registers (34.3), display
 * data area selection including blinking (34.4) and the segment output
 * state seen by the panel. Waveforms, bias and boost voltages and the clock
 * divider are retained for readback only. The LCDC defines no interrupt
 * request (Table 14.3); blinking follows the RTC periodic interrupt (34.4.2),
 * which is delivered through the "prd" GPIO input.
 *
 * The segment state is readable through QOM properties of the device:
 *   display-ram  raw display data registers, 2 hex digits per SEGnn
 *   blink-phase  true while the B-pattern is shown during blinking
 *   segments     segment state driven to the panel, 2 hex digits per SEGnn,
 *                bit n set when the SEGnn/COMn pixel is lit
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "migration/vmstate.h"
#include "hw/core/qdev.h"
#include "hw/rx/rx113_lcdc.h"
#include "qapi/error.h"
#include "qapi/visitor.h"

/* Register offsets from 0x000a0800, Table 5.1 and sections 34.2.1-34.2.4. */
#define LCDM0       0x00
#define LCDM1       0x01
#define LCDC0       0x02
#define VLCD        0x03
#define SEG00       0x40
#define LCDC_SIZE   0x100

/* 34.2.1 LCDM0 */
#define LCDM0_LDTY_SHIFT    2
#define LCDM0_LDTY_MASK     0x7
#define LCDM0_MDSET_SHIFT   6

/* 34.2.2 LCDM1: b2 and b1 are reserved and read as 0. */
#define LCDM1_LCDVLM    0x01
#define LCDM1_LCDSEL    0x08
#define LCDM1_BLON      0x10
#define LCDM1_VLCON     0x20
#define LCDM1_SCOC      0x40
#define LCDM1_LCDON     0x80
#define LCDM1_MASK      0xf9

/* 34.2.3 LCDC0: b7 and b6 are reserved and read as 0. */
#define LCDC0_MASK      0x3f

/* 34.2.4 VLCD: b7 to b5 are reserved and read as 0; reset value 04h. */
#define VLCD_MASK       0x1f
#define VLCD_RESET      0x04

/* Number of common signals driven for each LDTY[2:0] value, 34.2.1. */
static unsigned lcdc_com_count(uint8_t lcdm0)
{
    switch ((lcdm0 >> LCDM0_LDTY_SHIFT) & LCDM0_LDTY_MASK) {
    case 0:
        return 1;
    case 1:
        return 2;
    case 2:
        return 3;
    case 3:
        return 4;
    case 5:
        return 8;
    default:
        return 0; /* prohibited */
    }
}

static bool lcdc_8_slice(const RX113LCDCState *s)
{
    return ((s->lcdm0 >> LCDM0_LDTY_SHIFT) & LCDM0_LDTY_MASK) == 5;
}

/* 34.1: COM and SEG signals are driven only with SCOC = LCDON = 1. */
static bool lcdc_driving(const RX113LCDCState *s)
{
    return (s->lcdm1 & (LCDM1_SCOC | LCDM1_LCDON)) ==
           (LCDM1_SCOC | LCDM1_LCDON);
}

/* 34.4: the A-pattern (low nibble) or B-pattern (high nibble) is shown. */
static bool lcdc_pattern_b(const RX113LCDCState *s)
{
    if (s->lcdm1 & LCDM1_BLON) {
        return s->blink_phase;
    }
    return s->lcdm1 & LCDM1_LCDSEL;
}

static uint8_t lcdc_segment(const RX113LCDCState *s, unsigned seg)
{
    unsigned coms = lcdc_com_count(s->lcdm0);
    uint8_t data = s->ram[seg];

    if (!lcdc_driving(s) || coms == 0) {
        return 0;
    }
    /* 34.3 Table 34.7: 8-time-slice uses all eight bits as COM0-COM7. */
    if (!lcdc_8_slice(s)) {
        data = lcdc_pattern_b(s) ? data >> 4 : data & 0xf;
    }
    return data & (0xff >> (8 - coms));
}

static void lcdc_log_rewrite(const RX113LCDCState *s, const char *what)
{
    if (s->lcdm1 & LCDM1_SCOC) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: %s must not be rewritten while LCDM1.SCOC = 1\n",
                      TYPE_RX113_LCDC, what);
    }
}

static uint64_t lcdc_read(void *opaque, hwaddr addr, unsigned size)
{
    RX113LCDCState *s = opaque;

    switch (addr) {
    case LCDM0:
        return s->lcdm0;
    case LCDM1:
        return s->lcdm1;
    case LCDC0:
        return s->lcdc0;
    case VLCD:
        return s->vlcd;
    }
    if (addr >= SEG00 && addr < SEG00 + RX113_LCDC_NR_SEG) {
        return s->ram[addr - SEG00];
    }
    qemu_log_mask(LOG_GUEST_ERROR, "%s: read of reserved offset 0x%"
                  HWADDR_PRIx "\n", TYPE_RX113_LCDC, addr);
    return 0;
}

static void lcdc_write(void *opaque, hwaddr addr, uint64_t value,
                       unsigned size)
{
    RX113LCDCState *s = opaque;
    uint8_t val = value;

    switch (addr) {
    case LCDM0:
        lcdc_log_rewrite(s, "LCDM0");
        if (lcdc_com_count(val) == 0 || (val >> LCDM0_MDSET_SHIFT) == 3 ||
            (val & 3) == 3) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: prohibited LCDM0 setting 0x%02x\n",
                          TYPE_RX113_LCDC, val);
        }
        s->lcdm0 = val;
        return;
    case LCDM1:
        if ((val ^ s->lcdm1) & (LCDM1_VLCON | LCDM1_LCDVLM)) {
            lcdc_log_rewrite(s, "LCDM1.VLCON/LCDVLM");
        }
        /* 34.4.2 and Figure 34.3: blinking starts from the A-pattern. */
        if ((val & LCDM1_BLON) && !(s->lcdm1 & LCDM1_BLON)) {
            s->blink_phase = false;
        }
        s->lcdm1 = val & LCDM1_MASK;
        return;
    case LCDC0:
        lcdc_log_rewrite(s, "LCDC0");
        s->lcdc0 = val & LCDC0_MASK;
        return;
    case VLCD:
        if (s->lcdm1 & LCDM1_VLCON) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: VLCD must not be rewritten while "
                          "LCDM1.VLCON = 1\n", TYPE_RX113_LCDC);
        }
        s->vlcd = val & VLCD_MASK;
        return;
    }
    if (addr >= SEG00 && addr < SEG00 + RX113_LCDC_NR_SEG) {
        s->ram[addr - SEG00] = val;
        return;
    }
    qemu_log_mask(LOG_GUEST_ERROR, "%s: write to reserved offset 0x%"
                  HWADDR_PRIx "\n", TYPE_RX113_LCDC, addr);
}

static const MemoryRegionOps lcdc_ops = {
    .read = lcdc_read,
    .write = lcdc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
};

/* RTC periodic interrupt (PRD): switch between A- and B-pattern, 34.4.2. */
static void lcdc_prd_set(void *opaque, int n, int level)
{
    RX113LCDCState *s = opaque;

    if (level && (s->lcdm1 & LCDM1_BLON)) {
        s->blink_phase = !s->blink_phase;
    }
}

static char *lcdc_hex(const uint8_t *data, unsigned len)
{
    GString *str = g_string_sized_new(len * 2);

    for (unsigned i = 0; i < len; i++) {
        g_string_append_printf(str, "%02x", data[i]);
    }
    return g_string_free(str, false);
}

static char *lcdc_get_display_ram(Object *obj, Error **errp)
{
    RX113LCDCState *s = RX113_LCDC(obj);

    return lcdc_hex(s->ram, RX113_LCDC_NR_SEG);
}

static char *lcdc_get_segments(Object *obj, Error **errp)
{
    RX113LCDCState *s = RX113_LCDC(obj);
    uint8_t seg[RX113_LCDC_NR_SEG];

    for (unsigned i = 0; i < RX113_LCDC_NR_SEG; i++) {
        seg[i] = lcdc_segment(s, i);
    }
    return lcdc_hex(seg, RX113_LCDC_NR_SEG);
}

static bool lcdc_get_driving(Object *obj, Error **errp)
{
    return lcdc_driving(RX113_LCDC(obj));
}

static char *lcdc_get_pattern(Object *obj, Error **errp)
{
    return g_strdup(lcdc_pattern_b(RX113_LCDC(obj)) &&
                    !lcdc_8_slice(RX113_LCDC(obj)) ? "B" : "A");
}

static void lcdc_get_com_count(Object *obj, Visitor *v, const char *name,
                               void *opaque, Error **errp)
{
    uint8_t count = lcdc_com_count(RX113_LCDC(obj)->lcdm0);

    visit_type_uint8(v, name, &count, errp);
}

static bool lcdc_get_blink_phase(Object *obj, Error **errp)
{
    return RX113_LCDC(obj)->blink_phase;
}

static void lcdc_reset_hold(Object *obj, ResetType type)
{
    RX113LCDCState *s = RX113_LCDC(obj);

    /* 34.2 and 34.3: all registers and display data clear, VLCD = 04h. */
    s->lcdm0 = 0;
    s->lcdm1 = 0;
    s->lcdc0 = 0;
    s->vlcd = VLCD_RESET;
    memset(s->ram, 0, sizeof(s->ram));
    s->blink_phase = false;
}

static void lcdc_init(Object *obj)
{
    RX113LCDCState *s = RX113_LCDC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &lcdc_ops, s, TYPE_RX113_LCDC,
                          LCDC_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    qdev_init_gpio_in_named(DEVICE(obj), lcdc_prd_set, "prd", 1);

    object_property_add_str(obj, "display-ram", lcdc_get_display_ram, NULL);
    object_property_add_str(obj, "segments", lcdc_get_segments, NULL);
    object_property_add_str(obj, "pattern", lcdc_get_pattern, NULL);
    object_property_add_bool(obj, "driving", lcdc_get_driving, NULL);
    object_property_add(obj, "time-slices", "uint8", lcdc_get_com_count,
                        NULL, NULL, NULL);
    object_property_add_bool(obj, "blink-phase", lcdc_get_blink_phase,
                             NULL);
}

static const VMStateDescription vmstate_rx113_lcdc = {
    .name = TYPE_RX113_LCDC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(lcdm0, RX113LCDCState),
        VMSTATE_UINT8(lcdm1, RX113LCDCState),
        VMSTATE_UINT8(lcdc0, RX113LCDCState),
        VMSTATE_UINT8(vlcd, RX113LCDCState),
        VMSTATE_UINT8_ARRAY(ram, RX113LCDCState, RX113_LCDC_NR_SEG),
        VMSTATE_BOOL(blink_phase, RX113LCDCState),
        VMSTATE_END_OF_LIST()
    },
};

static void lcdc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->vmsd = &vmstate_rx113_lcdc;
    dc->user_creatable = false;
    rc->phases.hold = lcdc_reset_hold;
}

static const TypeInfo rx113_lcdc_info = {
    .name = TYPE_RX113_LCDC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RX113LCDCState),
    .instance_init = lcdc_init,
    .class_init = lcdc_class_init,
};

static void rx113_lcdc_register_types(void)
{
    type_register_static(&rx113_lcdc_info);
}

type_init(rx113_lcdc_register_types)
