/*
 * RX113 LCDC register, display RAM and segment state tests
 * (R01UH0448EJ0120 section 34)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define LCDC_BASE   0xa0800
#define LCDM0       (LCDC_BASE + 0x00)
#define LCDM1       (LCDC_BASE + 0x01)
#define LCDC0       (LCDC_BASE + 0x02)
#define VLCD        (LCDC_BASE + 0x03)
#define SEG(n)      (LCDC_BASE + 0x40 + (n))
#define NR_SEG      40

#define LCDM1_LCDSEL    0x08
#define LCDM1_BLON      0x10
#define LCDM1_SCOC      0x40
#define LCDM1_LCDON     0x80

#define LDTY_4SLICE     (3 << 2)
#define LDTY_8SLICE     (5 << 2)
#define LCDM0_LWAVE_B   0x20

/* SYSTEM registers used by the clock tests (sections 9 and 11). */
#define PRCR            0x803fe
#define MSTPCRD         0x8001c
#define LCDSCLKCR       0x80050
#define LCDSCLKCR2      0x80051
#define MSTPCRD_RUN     0xfffff7ff  /* LCDC module stop released (b11) */

/* RTC registers used by the PRD test (section 24). */
#define RTC_BASE        0x8c400
#define RCR1            (RTC_BASE + 0x22)
#define RCR2            (RTC_BASE + 0x24)
#define RCR3            (RTC_BASE + 0x26)
#define RYRCNT          (RTC_BASE + 0x0e)
#define RMONCNT         (RTC_BASE + 0x0c)
#define RDAYCNT         (RTC_BASE + 0x0a)

/*
 * Start a test machine with the LCDC module stop released; on real
 * hardware the LCDC is stopped at reset (MSTPCRD b11 = 1) and guest
 * software must release it before use, as the other RX113 tests do.
 */
static QTestState *lcdc_init(void)
{
    QTestState *qts = qtest_init("-M rx113-r5f51138");

    qtest_writew(qts, PRCR, 0xa503);
    qtest_writel(qts, MSTPCRD, MSTPCRD_RUN);
    return qts;
}

static uint64_t lcdc_prop_int(QTestState *qts, const char *prop)
{
    QDict *rsp = qtest_qmp(qts,
        "{'execute': 'qom-get', 'arguments': "
        "{'path': '/machine/mcu/lcdc', 'property': %s}}", prop);
    uint64_t val = qdict_get_int(rsp, "return");

    qobject_unref(rsp);
    return val;
}

static char *lcdc_prop(QTestState *qts, const char *prop)
{
    QDict *rsp = qtest_qmp(qts,
        "{'execute': 'qom-get', 'arguments': "
        "{'path': '/machine/mcu/lcdc', 'property': %s}}", prop);
    char *str = g_strdup(qdict_get_str(rsp, "return"));

    qobject_unref(rsp);
    return str;
}

static void check_segments(QTestState *qts, const char *prop,
                           const uint8_t *expected)
{
    g_autofree char *str = lcdc_prop(qts, prop);

    g_assert_cmpuint(strlen(str), ==, NR_SEG * 2);
    for (unsigned i = 0; i < NR_SEG; i++) {
        unsigned val;

        g_assert_cmpint(sscanf(str + i * 2, "%2x", &val), ==, 1);
        g_assert_cmphex(val, ==, expected[i]);
    }
}

static void test_reset(void)
{
    QTestState *qts = lcdc_init();
    uint8_t zero[NR_SEG] = { 0 };

    g_assert_cmphex(qtest_readb(qts, LCDM0), ==, 0);
    g_assert_cmphex(qtest_readb(qts, LCDM1), ==, 0);
    g_assert_cmphex(qtest_readb(qts, LCDC0), ==, 0);
    g_assert_cmphex(qtest_readb(qts, VLCD), ==, 0x04);
    for (unsigned i = 0; i < NR_SEG; i++) {
        g_assert_cmphex(qtest_readb(qts, SEG(i)), ==, 0);
    }
    check_segments(qts, "display-ram", zero);
    check_segments(qts, "segments", zero);

    /* Reset restores the values after a guest has changed them. */
    qtest_writeb(qts, LCDM0, 0x5f);
    qtest_writeb(qts, VLCD, 0x13);
    qtest_writeb(qts, SEG(3), 0xff);
    qtest_qmp_assert_success(qts, "{'execute': 'system_reset'}");
    /* The reset also restores the module stop; release it again. */
    qtest_writew(qts, PRCR, 0xa503);
    qtest_writel(qts, MSTPCRD, MSTPCRD_RUN);
    g_assert_cmphex(qtest_readb(qts, LCDM0), ==, 0);
    g_assert_cmphex(qtest_readb(qts, VLCD), ==, 0x04);
    g_assert_cmphex(qtest_readb(qts, SEG(3)), ==, 0);
    qtest_quit(qts);
}

static void test_registers(void)
{
    QTestState *qts = lcdc_init();

    qtest_writeb(qts, LCDM0, 0xa5);
    g_assert_cmphex(qtest_readb(qts, LCDM0), ==, 0xa5);
    /* LCDM1 b2 and b1 are reserved and read as 0 (34.2.2). */
    qtest_writeb(qts, LCDM1, 0xff);
    g_assert_cmphex(qtest_readb(qts, LCDM1), ==, 0xf9);
    qtest_writeb(qts, LCDM1, 0x06);
    g_assert_cmphex(qtest_readb(qts, LCDM1), ==, 0);
    /* LCDC0 b7, b6 and VLCD b7-b5 are reserved and read as 0. */
    qtest_writeb(qts, LCDC0, 0xff);
    g_assert_cmphex(qtest_readb(qts, LCDC0), ==, 0x3f);
    qtest_writeb(qts, VLCD, 0xff);
    g_assert_cmphex(qtest_readb(qts, VLCD), ==, 0x1f);
    /* Word accesses decompose into the byte registers. */
    qtest_writew(qts, LCDM0, 0x0c0d);
    g_assert_cmphex(qtest_readb(qts, LCDM0), ==, 0x0d);
    g_assert_cmphex(qtest_readb(qts, LCDM1), ==, 0x08);
    /* Reserved offsets read as zero and ignore writes. */
    qtest_writeb(qts, LCDC_BASE + 0x10, 0xff);
    g_assert_cmphex(qtest_readb(qts, LCDC_BASE + 0x10), ==, 0);
    qtest_writeb(qts, SEG(NR_SEG), 0xff);
    g_assert_cmphex(qtest_readb(qts, SEG(NR_SEG)), ==, 0);
    qtest_quit(qts);
}

static void test_display_ram(void)
{
    QTestState *qts = lcdc_init();
    uint8_t expected[NR_SEG];

    for (unsigned i = 0; i < NR_SEG; i++) {
        expected[i] = (i * 37 + 0x5a) & 0xff;
        qtest_writeb(qts, SEG(i), expected[i]);
    }
    for (unsigned i = 0; i < NR_SEG; i++) {
        g_assert_cmphex(qtest_readb(qts, SEG(i)), ==, expected[i]);
    }
    check_segments(qts, "display-ram", expected);
    qtest_quit(qts);
}

static void test_segment_state(void)
{
    QTestState *qts = lcdc_init();
    uint8_t expected[NR_SEG] = { 0 };
    g_autofree char *str = NULL;

    /* 4-time slice, 1/3 bias, waveform A, external resistance division. */
    qtest_writeb(qts, LCDM0, LDTY_4SLICE | 1);
    qtest_writeb(qts, SEG(0), 0x21);    /* A: COM0, B: COM1 */
    qtest_writeb(qts, SEG(5), 0xf5);    /* A: COM0, COM2; B: all */
    qtest_writeb(qts, SEG(39), 0x08);   /* A: COM3 */

    /* Nothing is lit until the display is enabled. */
    check_segments(qts, "segments", expected);
    g_assert_false(qtest_qom_get_bool(qts, "/machine/mcu/lcdc", "driving"));
    qtest_writeb(qts, LCDM1, LCDM1_SCOC);
    check_segments(qts, "segments", expected);
    qtest_writeb(qts, LCDM1, LCDM1_LCDON);
    check_segments(qts, "segments", expected);

    /* SCOC = LCDON = 1 shows the A-pattern. */
    qtest_writeb(qts, LCDM1, LCDM1_SCOC | LCDM1_LCDON);
    g_assert_true(qtest_qom_get_bool(qts, "/machine/mcu/lcdc", "driving"));
    expected[0] = 0x1;
    expected[5] = 0x5;
    expected[39] = 0x8;
    check_segments(qts, "segments", expected);
    str = lcdc_prop(qts, "pattern");
    g_assert_cmpstr(str, ==, "A");

    /* LCDSEL = 1 shows the B-pattern. */
    qtest_writeb(qts, LCDM1, LCDM1_SCOC | LCDM1_LCDON | LCDM1_LCDSEL);
    expected[0] = 0x2;
    expected[5] = 0xf;
    expected[39] = 0;
    check_segments(qts, "segments", expected);

    /* Static display drives COM0 only. */
    qtest_writeb(qts, LCDM0, 0);
    expected[0] = 0;
    expected[5] = 0x1;
    check_segments(qts, "segments", expected);
    qtest_quit(qts);
}

static void test_8_slice(void)
{
    QTestState *qts = lcdc_init();
    uint8_t expected[NR_SEG] = { 0 };

    qtest_writeb(qts, LCDM0, LDTY_8SLICE | 2);
    qtest_writeb(qts, SEG(11), 0x81);
    qtest_writeb(qts, SEG(20), 0xf0);
    qtest_writeb(qts, LCDM1, LCDM1_SCOC | LCDM1_LCDON);
    expected[11] = 0x81;
    expected[20] = 0xf0;
    check_segments(qts, "segments", expected);
    qtest_quit(qts);
}

static void test_blink(void)
{
    QTestState *qts = lcdc_init();
    uint8_t expected[NR_SEG] = { 0 };

    qtest_writeb(qts, LCDM0, LDTY_4SLICE | 1);
    qtest_writeb(qts, SEG(2), 0x61);    /* A: COM0, B: COM1 and COM2 */
    qtest_writeb(qts, LCDM1, LCDM1_SCOC | LCDM1_LCDON | LCDM1_BLON);

    /* Blinking starts from the A-pattern and flips on each PRD edge. */
    expected[2] = 0x1;
    check_segments(qts, "segments", expected);
    qtest_set_irq_in(qts, "/machine/mcu/lcdc", "prd", 0, 1);
    qtest_set_irq_in(qts, "/machine/mcu/lcdc", "prd", 0, 0);
    expected[2] = 0x6;
    check_segments(qts, "segments", expected);
    g_assert_true(qtest_qom_get_bool(qts, "/machine/mcu/lcdc",
                                     "blink-phase"));
    qtest_set_irq_in(qts, "/machine/mcu/lcdc", "prd", 0, 1);
    qtest_set_irq_in(qts, "/machine/mcu/lcdc", "prd", 0, 0);
    expected[2] = 0x1;
    check_segments(qts, "segments", expected);

    /* PRD has no effect without BLON. */
    qtest_writeb(qts, LCDM1, LCDM1_SCOC | LCDM1_LCDON);
    qtest_set_irq_in(qts, "/machine/mcu/lcdc", "prd", 0, 1);
    check_segments(qts, "segments", expected);
    qtest_quit(qts);
}

static void test_clock_registers(void)
{
    QTestState *qts = qtest_init("-M rx113-r5f51138");

    /* Reset values (9.2.15, 9.2.16): source = LOCO, output stopped. */
    g_assert_cmphex(qtest_readb(qts, LCDSCLKCR), ==, 0);
    g_assert_cmphex(qtest_readb(qts, LCDSCLKCR2), ==, 1);
    /* Both registers are PRC0-protected. */
    qtest_writeb(qts, LCDSCLKCR, 3);
    qtest_writeb(qts, LCDSCLKCR2, 0);
    g_assert_cmphex(qtest_readb(qts, LCDSCLKCR), ==, 0);
    g_assert_cmphex(qtest_readb(qts, LCDSCLKCR2), ==, 1);
    /* Unlocked: reserved bits read as 0. */
    qtest_writew(qts, PRCR, 0xa501);
    qtest_writeb(qts, LCDSCLKCR, 0xff);
    g_assert_cmphex(qtest_readb(qts, LCDSCLKCR), ==, 0x07);
    qtest_writeb(qts, LCDSCLKCR2, 0xfe);
    g_assert_cmphex(qtest_readb(qts, LCDSCLKCR2), ==, 0x00);
    qtest_writeb(qts, LCDSCLKCR2, 0xff);
    g_assert_cmphex(qtest_readb(qts, LCDSCLKCR2), ==, 0x01);
    qtest_quit(qts);
}

/* Configure the manual's worked example: sub-clock, LCDCL = 256 Hz. */
static void lcdc_setup_subclock(QTestState *qts)
{
    qtest_writeb(qts, LCDSCLKCR, 3);    /* sub-clock oscillator */
    qtest_writeb(qts, LCDSCLKCR2, 0);   /* source clock output on */
    qtest_writeb(qts, LCDC0, 0x06);     /* 32768 / 128 = 256 Hz */
    qtest_writeb(qts, LCDM0, LDTY_4SLICE | 1);
}

static void test_frame_frequency(void)
{
    QTestState *qts = lcdc_init();

    qtest_writeb(qts, LCDM0, LDTY_4SLICE | 1);
    /* LCDSCLKSTP = 1 at reset stops the source clock: no frame rate. */
    g_assert_cmpuint(lcdc_prop_int(qts, "frame-frequency"), ==, 0);
    /* Figure 34.14's example: 256 Hz LCD clock, 4 slices -> 64 Hz. */
    lcdc_setup_subclock(qts);
    g_assert_cmpuint(lcdc_prop_int(qts, "frame-frequency"), ==, 64);
    /* LOCO (4 MHz) uses the main-group table: code 15h divides by 4096. */
    qtest_writeb(qts, LCDSCLKCR, 0);
    qtest_writeb(qts, LCDC0, 0x15);
    g_assert_cmpuint(lcdc_prop_int(qts, "frame-frequency"), ==, 244);
    /* Code 06h is not in the main-group table: prohibited, no clock. */
    qtest_writeb(qts, LCDC0, 0x06);
    g_assert_cmpuint(lcdc_prop_int(qts, "frame-frequency"), ==, 0);
    qtest_quit(qts);
}

static void test_scan(void)
{
    QTestState *qts = lcdc_init();
    const char *lcdc = "/machine/mcu/lcdc";
    /* Half an LCDCL period at 256 Hz. */
    const int64_t half = 1953125;

    lcdc_setup_subclock(qts);
    qtest_writeb(qts, LCDM1, LCDM1_SCOC | LCDM1_LCDON);
    g_assert_cmpuint(lcdc_prop_int(qts, "scan-slice"), ==, 0);
    g_assert_false(qtest_qom_get_bool(qts, lcdc, "drive-phase"));
    /* Waveform A inverts the drive phase every half period. */
    qtest_clock_step(qts, half);
    g_assert_cmpuint(lcdc_prop_int(qts, "scan-slice"), ==, 0);
    g_assert_true(qtest_qom_get_bool(qts, lcdc, "drive-phase"));
    qtest_clock_step(qts, half);
    g_assert_cmpuint(lcdc_prop_int(qts, "scan-slice"), ==, 1);
    g_assert_false(qtest_qom_get_bool(qts, lcdc, "drive-phase"));
    /* The remaining three slices complete the frame. */
    qtest_clock_step(qts, half * 6);
    g_assert_cmpuint(lcdc_prop_int(qts, "scan-slice"), ==, 0);
    g_assert_false(qtest_qom_get_bool(qts, lcdc, "drive-phase"));

    /* Waveform B inverts the phase once per LCDCL period instead. */
    qtest_writeb(qts, LCDM0, LDTY_4SLICE | 1 | LCDM0_LWAVE_B);
    g_assert_false(qtest_qom_get_bool(qts, lcdc, "drive-phase"));
    qtest_clock_step(qts, half);
    g_assert_false(qtest_qom_get_bool(qts, lcdc, "drive-phase"));
    qtest_clock_step(qts, half);
    g_assert_true(qtest_qom_get_bool(qts, lcdc, "drive-phase"));
    qtest_quit(qts);
}

static void test_stop(void)
{
    QTestState *qts = lcdc_init();
    const int64_t half = 1953125;

    lcdc_setup_subclock(qts);
    qtest_writeb(qts, LCDM1, LCDM1_SCOC | LCDM1_LCDON);
    qtest_clock_step(qts, half * 2);
    g_assert_cmpuint(lcdc_prop_int(qts, "scan-slice"), ==, 1);

    /* LCDSCLKSTP stops the source clock output: the scan freezes. */
    qtest_writeb(qts, LCDSCLKCR2, 1);
    g_assert_cmpuint(lcdc_prop_int(qts, "frame-frequency"), ==, 0);
    qtest_clock_step(qts, half * 8);
    g_assert_cmpuint(lcdc_prop_int(qts, "scan-slice"), ==, 1);
    qtest_writeb(qts, LCDSCLKCR2, 0);
    qtest_clock_step(qts, half * 2);
    g_assert_cmpuint(lcdc_prop_int(qts, "scan-slice"), ==, 2);

    /* Module stop: reads return 0, writes are dropped, state is held. */
    qtest_writel(qts, MSTPCRD, 0xffffffff);
    g_assert_cmphex(qtest_readb(qts, LCDM0), ==, 0);
    qtest_writeb(qts, LCDM0, 0);
    qtest_clock_step(qts, half * 4);
    qtest_writel(qts, MSTPCRD, MSTPCRD_RUN);
    g_assert_cmphex(qtest_readb(qts, LCDM0), ==, LDTY_4SLICE | 1);
    g_assert_cmpuint(lcdc_prop_int(qts, "scan-slice"), ==, 2);
    qtest_quit(qts);
}

static void test_prd_from_rtc(void)
{
    QTestState *qts = lcdc_init();
    uint8_t expected[NR_SEG] = { 0 };
    bool flipped = false;

    qtest_writeb(qts, LCDM0, LDTY_4SLICE | 1);
    qtest_writeb(qts, SEG(2), 0x61);    /* A: COM0, B: COM1 and COM2 */
    qtest_writeb(qts, LCDM1, LCDM1_SCOC | LCDM1_LCDON | LCDM1_BLON);
    expected[2] = 0x1;
    check_segments(qts, "segments", expected);

    /*
     * Start the real RTC with the fastest periodic rate (PES = 0110b,
     * 1/256 s); its PRD output is wired to the LCDC blink input.
     */
    qtest_writeb(qts, RCR2, 0x40);
    qtest_writeb(qts, RYRCNT, 0x24);
    qtest_writeb(qts, RMONCNT, 0x01);
    qtest_writeb(qts, RDAYCNT, 0x01);
    qtest_writeb(qts, RCR1, 0x64);      /* PES = 0110b, PIE = 1 */
    qtest_writeb(qts, RCR3, 0x01);      /* RTCEN */
    qtest_writeb(qts, RCR2, 0x41);      /* START */
    for (unsigned i = 0; i < 20 && !flipped; i++) {
        qtest_clock_step(qts, 1000000);
        flipped = qtest_qom_get_bool(qts, "/machine/mcu/lcdc",
                                      "blink-phase");
    }
    g_assert_true(flipped);
    expected[2] = 0x6;
    check_segments(qts, "segments", expected);
    qtest_quit(qts);
}

/* Issue #63: COM/SEG pad levels observed through the PORT model. */
#define PORT_BASE   0x8c000
#define PMR(p)      (PORT_BASE + 0x60 + (p))
#define PWPR        (PORT_BASE + 0x11f)
#define PFS(p, b)   (PORT_BASE + 0x140 + (p) * 8 + (b))
#define PORT_1      1
#define PORT_B      11
#define PORT_C      12

static unsigned port_prop(QTestState *qts, const char *prop, unsigned port)
{
    QDict *rsp = qtest_qmp(qts,
        "{'execute': 'qom-get', 'arguments': "
        "{'path': '/machine/mcu/port', 'property': %s}}", prop);
    const char *str = qdict_get_str(rsp, "return");
    unsigned val;

    g_assert_cmpuint(strlen(str), ==, 38);
    g_assert_cmpint(sscanf(str + port * 2, "%2x", &val), ==, 1);
    qobject_unref(rsp);
    return val;
}

/* Select the LCDC pin function (PSEL = 24, 19.3.4) on one pad. */
static void pad_select_lcd(QTestState *qts, unsigned port, unsigned bit)
{
    qtest_writeb(qts, PWPR, 0);
    qtest_writeb(qts, PWPR, 0x40);
    qtest_writeb(qts, PFS(port, bit), 24);
    qtest_writeb(qts, PWPR, 0x80);
    qtest_writeb(qts, PMR(port),
                 qtest_readb(qts, PMR(port)) | (1 << bit));
}

static void test_pads_scan(void)
{
    QTestState *qts = lcdc_init();
    const int64_t half = 1953125;   /* half of the 256 Hz LCDCL */

    lcdc_setup_subclock(qts);
    qtest_writeb(qts, SEG(0), 0x01);    /* SEG00 lit on slice 0 only */
    pad_select_lcd(qts, PORT_C, 5);     /* COM0 */
    pad_select_lcd(qts, PORT_C, 4);     /* COM1 */
    pad_select_lcd(qts, PORT_C, 2);     /* COM3 */
    pad_select_lcd(qts, PORT_1, 3);     /* SEG00 */
    pad_select_lcd(qts, PORT_B, 7);     /* SEG11 in 4-slice mode */
    qtest_writeb(qts, LCDM1, LCDM1_SCOC | LCDM1_LCDON);

    /* Slice 0, first half: COM0 driven high, SEG00 low (lit). */
    g_assert_cmphex(port_prop(qts, "driving", PORT_C), ==, 0x20);
    g_assert_cmphex(port_prop(qts, "levels", PORT_C), ==, 0x20);
    g_assert_cmphex(port_prop(qts, "driving", PORT_1), ==, 0x08);
    g_assert_cmphex(port_prop(qts, "levels", PORT_1), ==, 0x00);
    /* SEG11 is unlit: it follows the selected common's level. */
    g_assert_cmphex(port_prop(qts, "driving", PORT_B), ==, 0x80);
    g_assert_cmphex(port_prop(qts, "levels", PORT_B), ==, 0x80);

    /* Second half: the drive phase inverts (waveform A). */
    qtest_clock_step(qts, half);
    g_assert_cmphex(port_prop(qts, "levels", PORT_C), ==, 0x00);
    g_assert_cmphex(port_prop(qts, "levels", PORT_1), ==, 0x08);
    g_assert_cmphex(port_prop(qts, "levels", PORT_B), ==, 0x00);

    /* Slice 1: COM1 takes over; SEG00 is dark on this slice. */
    qtest_clock_step(qts, half);
    g_assert_cmphex(port_prop(qts, "driving", PORT_C), ==, 0x10);
    g_assert_cmphex(port_prop(qts, "levels", PORT_C), ==, 0x10);
    g_assert_cmphex(port_prop(qts, "levels", PORT_1), ==, 0x08);
    qtest_quit(qts);
}

static void test_pads_waveform_b(void)
{
    QTestState *qts = lcdc_init();
    const int64_t half = 1953125;

    lcdc_setup_subclock(qts);
    qtest_writeb(qts, LCDM0, LDTY_4SLICE | LCDM0_LWAVE_B | 1);
    qtest_writeb(qts, SEG(0), 0x01);
    pad_select_lcd(qts, PORT_C, 5);     /* COM0 */
    pad_select_lcd(qts, PORT_1, 3);     /* SEG00 */
    qtest_writeb(qts, LCDM1, LCDM1_SCOC | LCDM1_LCDON);

    g_assert_cmphex(port_prop(qts, "levels", PORT_C), ==, 0x20);
    qtest_clock_step(qts, half);
    /* Waveform B holds the phase for the whole slice. */
    g_assert_cmphex(port_prop(qts, "driving", PORT_C), ==, 0x20);
    g_assert_cmphex(port_prop(qts, "levels", PORT_C), ==, 0x20);
    g_assert_cmphex(port_prop(qts, "levels", PORT_1), ==, 0x00);
    qtest_quit(qts);
}

static void test_pads_ground_and_stop(void)
{
    QTestState *qts = lcdc_init();

    lcdc_setup_subclock(qts);
    pad_select_lcd(qts, PORT_C, 5);     /* COM0 */
    pad_select_lcd(qts, PORT_1, 3);     /* SEG00 */

    /* SCOC = 0: ground level is driven on the LCD pins (34.2.2). */
    g_assert_cmphex(port_prop(qts, "driving", PORT_C), ==, 0x20);
    g_assert_cmphex(port_prop(qts, "levels", PORT_C), ==, 0x00);
    g_assert_cmphex(port_prop(qts, "driving", PORT_1), ==, 0x08);
    g_assert_cmphex(port_prop(qts, "levels", PORT_1), ==, 0x00);

    /* Module stop: the LCDC drives nothing. */
    qtest_writel(qts, MSTPCRD, 0xffffffff);
    g_assert_cmphex(port_prop(qts, "driving", PORT_C), ==, 0x00);
    g_assert_cmphex(port_prop(qts, "driving", PORT_1), ==, 0x00);
    qtest_quit(qts);
}

static void test_pads_8_slice(void)
{
    QTestState *qts = lcdc_init();
    const int64_t half = 1953125;

    qtest_writeb(qts, LCDSCLKCR, 3);
    qtest_writeb(qts, LCDSCLKCR2, 0);
    qtest_writeb(qts, LCDC0, 0x06);
    qtest_writeb(qts, LCDM0, LDTY_8SLICE | 1);
    pad_select_lcd(qts, PORT_B, 7);     /* COM4 in 8-slice mode */
    qtest_writeb(qts, LCDM1, LCDM1_SCOC | LCDM1_LCDON);

    /* PB7 is SEG11 outside 8-slice mode; here it idles until slice 4. */
    g_assert_cmphex(port_prop(qts, "driving", PORT_B), ==, 0x00);
    qtest_clock_step(qts, half * 8);
    g_assert_cmphex(port_prop(qts, "driving", PORT_B), ==, 0x80);
    g_assert_cmphex(port_prop(qts, "levels", PORT_B), ==, 0x80);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/rx113-lcdc/reset", test_reset);
    qtest_add_func("/rx113-lcdc/registers", test_registers);
    qtest_add_func("/rx113-lcdc/display-ram", test_display_ram);
    qtest_add_func("/rx113-lcdc/segment-state", test_segment_state);
    qtest_add_func("/rx113-lcdc/8-slice", test_8_slice);
    qtest_add_func("/rx113-lcdc/blink", test_blink);
    qtest_add_func("/rx113-lcdc/clock-registers", test_clock_registers);
    qtest_add_func("/rx113-lcdc/frame-frequency", test_frame_frequency);
    qtest_add_func("/rx113-lcdc/scan", test_scan);
    qtest_add_func("/rx113-lcdc/stop", test_stop);
    qtest_add_func("/rx113-lcdc/prd-from-rtc", test_prd_from_rtc);
    qtest_add_func("/rx113-lcdc/pads-scan", test_pads_scan);
    qtest_add_func("/rx113-lcdc/pads-waveform-b", test_pads_waveform_b);
    qtest_add_func("/rx113-lcdc/pads-ground-stop", test_pads_ground_and_stop);
    qtest_add_func("/rx113-lcdc/pads-8-slice", test_pads_8_slice);
    return g_test_run();
}
