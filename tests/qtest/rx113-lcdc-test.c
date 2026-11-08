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
    QTestState *qts = qtest_init("-M rx113-r5f51138");
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
    g_assert_cmphex(qtest_readb(qts, LCDM0), ==, 0);
    g_assert_cmphex(qtest_readb(qts, VLCD), ==, 0x04);
    g_assert_cmphex(qtest_readb(qts, SEG(3)), ==, 0);
    qtest_quit(qts);
}

static void test_registers(void)
{
    QTestState *qts = qtest_init("-M rx113-r5f51138");

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
    QTestState *qts = qtest_init("-M rx113-r5f51138");
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
    QTestState *qts = qtest_init("-M rx113-r5f51138");
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
    QTestState *qts = qtest_init("-M rx113-r5f51138");
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
    QTestState *qts = qtest_init("-M rx113-r5f51138");
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

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/rx113-lcdc/reset", test_reset);
    qtest_add_func("/rx113-lcdc/registers", test_registers);
    qtest_add_func("/rx113-lcdc/display-ram", test_display_ram);
    qtest_add_func("/rx113-lcdc/segment-state", test_segment_state);
    qtest_add_func("/rx113-lcdc/8-slice", test_8_slice);
    qtest_add_func("/rx113-lcdc/blink", test_blink);
    return g_test_run();
}
