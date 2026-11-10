/*
 * RX113 CTSU digital interface tests (manual section 35; D-010/D-011).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define CTSU 0xa0900
#define CR0 CTSU
#define CR1 (CTSU + 1)
#define SDPRS (CTSU + 2)
#define SST (CTSU + 3)
#define MCH0 (CTSU + 4)
#define MCH1 (CTSU + 5)
#define CHAC0 (CTSU + 6)
#define CHAC1 (CTSU + 7)
#define CHTRC0 (CTSU + 0xb)
#define ST (CTSU + 0x11)
#define SSC (CTSU + 0x12)
#define SO0 (CTSU + 0x14)
#define SO1 (CTSU + 0x16)
#define SC (CTSU + 0x18)
#define RC (CTSU + 0x1a)
#define ERRS (CTSU + 0x1c)
#define TRMR 0x7fffbe
#define MSTPD 0x8001c
#define RUN_D 0xfffffbffU
#define IR(n) (0x87000 + (n))

static void configure(QTestState *qts)
{
    qtest_writew(qts, 0x803fe, 0xa503);
    qtest_writel(qts, 0x80020, 0); /* PCLKB = LOCO = 4 MHz. */
    qtest_writel(qts, MSTPD, RUN_D);
    qtest_writeb(qts, CR1, 3);
    qtest_writeb(qts, SDPRS, 0x20); /* 62 base pulses, ratio 0. */
    qtest_writeb(qts, SST, 0x10);
    qtest_writeb(qts, CHAC0, 1);
    qtest_writeb(qts, MCH0, 0);
}

static QTestState *ctsu_init(void)
{
    QTestState *qts = qtest_init("-M rx113-r5f51138 "
                              "-global rx113-ctsu.ts03-ico-hz=2000000");

    configure(qts);
    return qts;
}

static void state(QTestState *qts, unsigned expected)
{
    g_assert_cmpuint(qtest_readb(qts, ST) & 7, ==, expected);
}

static void clear_ir(QTestState *qts)
{
    for (unsigned n = 60; n <= 62; n++) {
        qtest_writeb(qts, IR(n), 0);
    }
}

static void settings(QTestState *qts, unsigned so0, unsigned so1)
{
    qtest_writew(qts, SSC, 0);
    qtest_writew(qts, SO0, so0);
    qtest_writew(qts, SO1, so1);
}

static void test_timing(void)
{
    QTestState *qts = ctsu_init();

    qtest_writeb(qts, CR0, 1);
    state(qts, 2);
    g_assert_cmpuint(qtest_readb(qts, IR(60)), ==, 1);
    qtest_clock_step(qts, 1000000); /* State 2 requires SO1. */
    state(qts, 2);
    qtest_writew(qts, SO0, 0);
    state(qts, 2);
    settings(qts, 0, 0x100); /* Base clock 1 MHz. */
    state(qts, 3);
    clear_ir(qts);
    qtest_clock_step(qts, 33999);
    state(qts, 3);
    qtest_clock_step(qts, 1); /* D-011: exactly 34 base cycles. */
    state(qts, 4);
    qtest_clock_step(qts, 76999);
    state(qts, 4);
    qtest_clock_step(qts, 1);
    state(qts, 5);
    g_assert_cmpuint(qtest_readb(qts, ST) & 0x10, ==, 0x10);
    g_assert_cmpuint(qtest_readb(qts, IR(61)), ==, 0);
    qtest_clock_step(qts, 499);
    state(qts, 5);
    qtest_clock_step(qts, 1); /* Two operating clocks. */
    state(qts, 0);
    g_assert_cmpuint(qtest_readb(qts, CR0), ==, 0);
    g_assert_cmpuint(qtest_readb(qts, IR(61)), ==, 1);
    g_assert_cmpuint(qtest_readb(qts, IR(62)), ==, 1);
    g_assert_cmpuint(qtest_readw(qts, SC), ==, 77);
    g_assert_cmpuint(qtest_readb(qts, ST) & 0x10, ==, 0x10);
    g_assert_cmpuint(qtest_readw(qts, RC), ==, 77);
    g_assert_cmpuint(qtest_readb(qts, ST), ==, 0);
    qtest_quit(qts);
}

static void test_restart_reset(void)
{
    QTestState *qts = ctsu_init();

    qtest_writeb(qts, CR1, 0x23); /* Operating clock = PCLKB/4. */
    qtest_writeb(qts, CR0, 1);
    clear_ir(qts);
    qtest_writeb(qts, CR0, 1); /* Repeated start does not request settings. */
    g_assert_cmpuint(qtest_readb(qts, IR(60)), ==, 0);
    settings(qts, 0, 0x100);
    qtest_clock_step(qts, 446000);
    state(qts, 0);
    qtest_readw(qts, RC);
    clear_ir(qts);
    qtest_writeb(qts, CR0, 1);
    state(qts, 0);
    qtest_clock_step(qts, 749);
    qtest_writeb(qts, CR0, 1);
    state(qts, 0);
    qtest_clock_step(qts, 1); /* Three PCLKB cycles after CTSUFN. */
    qtest_writeb(qts, CR0, 1);
    state(qts, 2);
    settings(qts, 0, 0x100);
    qtest_clock_step(qts, 150000);
    state(qts, 4);
    /* Forced stop initializes only the registers listed in section 35.4.4. */
    qtest_writeb(qts, CR0, 0x10);
    g_assert_cmpuint(qtest_readb(qts, CR1), ==, 0x23);
    g_assert_cmpuint(qtest_readw(qts, SO1), ==, 0x100);
    g_assert_cmpuint(qtest_readb(qts, MCH0), ==, 15);
    g_assert_cmpuint(qtest_readw(qts, SC), ==, 0);
    clear_ir(qts);
    qtest_clock_step(qts, 1000000);
    state(qts, 0);
    g_assert_cmpuint(qtest_readb(qts, IR(61)), ==, 0);
    g_assert_cmpuint(qtest_readb(qts, IR(62)), ==, 0);
    qtest_writeb(qts, MCH0, 0);
    qtest_writeb(qts, CR0, 1);
    settings(qts, 0, 0x100);
    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    configure(qts);
    clear_ir(qts);
    qtest_clock_step(qts, 1000000);
    state(qts, 0);
    g_assert_cmpuint(qtest_readb(qts, IR(61)), ==, 0);
    g_assert_cmpuint(qtest_readb(qts, IR(62)), ==, 0);
    qtest_quit(qts);
}

static void test_scan(void)
{
    QTestState *qts = ctsu_init();
    static const unsigned channels[] = { 0, 3, 8, 11 };

    qtest_writeb(qts, CR1, 0x43);
    qtest_writeb(qts, MCH0, 9); /* Read-only in multiscan. */
    g_assert_cmpuint(qtest_readb(qts, MCH0), ==, 0);
    qtest_writeb(qts, CHAC0, 9);
    qtest_writeb(qts, CHAC1, 9);
    qtest_writeb(qts, CR0, 1);
    for (unsigned i = 0; i < ARRAY_SIZE(channels); i++) {
        g_assert_cmpuint(qtest_readb(qts, MCH0), ==, channels[i]);
        g_assert_cmpuint(qtest_readb(qts, MCH1), ==, 15);
        state(qts, 2);
        g_assert_cmpuint(qtest_readb(qts, IR(60)), ==, 1);
        clear_ir(qts);
        settings(qts, 0, 0x100);
        qtest_clock_step(qts, 111500);
        g_assert_cmpuint(qtest_readb(qts, IR(61)), ==, 1);
        g_assert_cmpuint(qtest_readb(qts, IR(62)), ==, i == 3);
        g_assert_cmpuint(qtest_readw(qts, SC), ==,
                         channels[i] == 3 ? 154 : 77);
        g_assert_cmpuint(qtest_readw(qts, RC), ==, 77);
    }
    state(qts, 0);
    g_assert_cmpuint(qtest_readb(qts, MCH0), ==, 15);
    /* An empty target set finishes without CTSUWR/CTSURD. */
    clear_ir(qts);
    qtest_writeb(qts, CHAC0, 0);
    qtest_writeb(qts, CHAC1, 0);
    qtest_writeb(qts, CR0, 1);
    state(qts, 0);
    g_assert_cmpuint(qtest_readb(qts, IR(60)), ==, 0);
    g_assert_cmpuint(qtest_readb(qts, IR(61)), ==, 0);
    g_assert_cmpuint(qtest_readb(qts, IR(62)), ==, 1);
    qtest_quit(qts);
}

static void test_unread(void)
{
    QTestState *qts = ctsu_init();

    qtest_writeb(qts, CR1, 0x43);
    qtest_writeb(qts, CHAC0, 9);
    qtest_writeb(qts, CR0, 1);
    settings(qts, 0, 0x100);
    qtest_clock_step(qts, 111500);
    state(qts, 2);
    settings(qts, 0, 0x100);
    qtest_clock_step(qts, 1000000);
    state(qts, 3); /* Stabilization elapsed, previous result still unread. */
    g_assert_cmpuint(qtest_readw(qts, SC), ==, 77);
    state(qts, 3);
    g_assert_cmpuint(qtest_readw(qts, RC), ==, 77);
    state(qts, 4);
    qtest_clock_step(qts, 77500);
    state(qts, 0);
    g_assert_cmpuint(qtest_readw(qts, SC), ==, 154);
    g_assert_cmpuint(qtest_readw(qts, RC), ==, 77);
    qtest_quit(qts);
}

static void test_mutual(void)
{
    QTestState *qts = ctsu_init();
    static const unsigned rx[] = { 0, 0, 3, 3 };
    static const unsigned tx[] = { 5, 6, 5, 6 };

    qtest_writeb(qts, CR1, 0xc3);
    qtest_writeb(qts, CHAC0, 0x69);
    qtest_writeb(qts, CHTRC0, 0x60);
    qtest_writeb(qts, CR0, 1);
    for (unsigned pair = 0; pair < 4; pair++) {
        g_assert_cmpuint(qtest_readb(qts, MCH0), ==, rx[pair]);
        g_assert_cmpuint(qtest_readb(qts, MCH1), ==, tx[pair]);
        g_assert_cmpuint(qtest_readb(qts, IR(60)), ==, 1);
        clear_ir(qts);
        settings(qts, 0, 0x100);
        state(qts, 3);
        qtest_clock_step(qts, 111500);
        g_assert_cmpuint(qtest_readb(qts, IR(61)), ==, 1);
        g_assert_cmpuint(qtest_readb(qts, IR(62)), ==, 0);
        g_assert_cmpuint(qtest_readb(qts, ST) & 0x80, ==, 0x80);
        g_assert_cmpuint(qtest_readb(qts, IR(60)), ==, 0);
        qtest_clock_step(qts, 34000);
        state(qts, 3); /* Second edge also holds unread data. */
        g_assert_cmpuint(qtest_readw(qts, SC), ==, rx[pair] ? 154 : 77);
        g_assert_cmpuint(qtest_readw(qts, RC), ==, 77);
        state(qts, 4);
        qtest_writeb(qts, IR(61), 0);
        qtest_clock_step(qts, 77500);
        g_assert_cmpuint(qtest_readw(qts, SC), ==, rx[pair] ? 154 : 77);
        g_assert_cmpuint(qtest_readw(qts, RC), ==, 77);
        g_assert_cmpuint(qtest_readb(qts, IR(61)), ==, 1);
        g_assert_cmpuint(qtest_readb(qts, ST) & 0x80, ==, 0);
        g_assert_cmpuint(qtest_readb(qts, IR(62)), ==, pair == 3);
    }
    state(qts, 0);
    g_assert_cmpuint(qtest_readb(qts, MCH0), ==, 15);
    g_assert_cmpuint(qtest_readb(qts, MCH1), ==, 15);
    qtest_quit(qts);
}

static void test_analog_trim(void)
{
    QTestState *qts = ctsu_init();

    g_assert_cmpuint(qtest_readb(qts, TRMR), ==, 0);
    qtest_writeb(qts, TRMR, 0x56);
    g_assert_cmpuint(qtest_readb(qts, TRMR), ==, 0);
    qtest_writew(qts, ERRS, 0xffff);
    g_assert_cmpuint(qtest_readw(qts, ERRS), ==, 0x8f);
    qtest_writew(qts, ERRS, 2);
    qtest_writeb(qts, TRMR, 0x56);
    g_assert_cmpuint(qtest_readb(qts, TRMR), ==, 0x56);
    qtest_writew(qts, ERRS, 0);
    for (unsigned i = 0; i < 2; i++) {
        qtest_writeb(qts, CR0, 1);
        qtest_writew(qts, SSC, 0xffff);
        settings(qts, i ? 0x3ff : 0, i ? 0x61ff : 0x100);
        g_assert_cmpuint(qtest_readw(qts, SO0), ==, i ? 0x3ff : 0);
        g_assert_cmpuint(qtest_readw(qts, SO1), ==, i ? 0x61ff : 0x100);
        qtest_clock_step(qts, 111500);
        g_assert_cmpuint(qtest_readw(qts, SC), ==, 77);
        g_assert_cmpuint(qtest_readw(qts, RC), ==, 77);
    }
    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    configure(qts);
    g_assert_cmpuint(qtest_readb(qts, TRMR), ==, 0);
    qtest_quit(qts);
}

static void test_fixed_pins(void)
{
    static const unsigned pins[] = { 0x07, 0x04, 0x02, 0x123, 0x25, 0x24,
                                     0x23, 0x22, 0x21, 0x20, 0x27, 0x32 };
    QTestState *qts = ctsu_init();

    qtest_writeb(qts, 0x8c11f, 0);
    qtest_writeb(qts, 0x8c11f, 0x40);
    for (unsigned i = 0; i < ARRAY_SIZE(pins); i++) {
        unsigned port = pins[i] >> 4, bit = pins[i] & 7;
        unsigned pin = port * 8 + bit;

        qtest_writeb(qts, 0x8c140 + pin, 25);
        qtest_writeb(qts, 0x8c060 + port,
                     qtest_readb(qts, 0x8c060 + port) | (1 << bit));
    }
    qtest_writew(qts, ERRS, 4);
    for (unsigned level = 0; level < 2; level++) {
        qtest_writeb(qts, CR0, level << 3);
        for (unsigned i = 0; i < ARRAY_SIZE(pins); i++) {
            unsigned port = pins[i] >> 4, bit = pins[i] & 7;

            g_assert_cmpuint((qtest_readb(qts, 0x8c040 + port) >> bit) & 1,
                             ==, level);
        }
    }
    qtest_writew(qts, ERRS, 0);
    g_assert_cmpuint(qtest_readb(qts, 0x8c040) & 0x94, ==, 0);
    qtest_quit(qts);
}

static void test_overflow(void)
{
    QTestState *qts = ctsu_init();

    qtest_writeb(qts, CHAC0, 8);
    qtest_writeb(qts, MCH0, 3);
    qtest_writeb(qts, CR0, 1);
    settings(qts, 0xfc00, 0x1f00); /* 64 repeats, base divider 64. */
    qtest_clock_step(qts, 79496500);
    g_assert_cmpuint(qtest_readb(qts, CR0), ==, 0);
    qtest_writeb(qts, ST, 0); /* No preceding status read: cannot clear. */
    g_assert_cmpuint(qtest_readb(qts, ST) & 0x60, ==, 0x60);
    g_assert_cmpuint(qtest_readw(qts, SC), ==, 65535);
    g_assert_cmpuint(qtest_readw(qts, RC), ==, 65535);
    qtest_writeb(qts, ST, 0x40);
    g_assert_cmpuint(qtest_readb(qts, ST), ==, 0x40);
    qtest_writeb(qts, ST, 0);
    g_assert_cmpuint(qtest_readb(qts, ST), ==, 0);
    qtest_quit(qts);
}

static void test_overflow_during_measurement(void)
{
    QTestState *qts = ctsu_init();

    qtest_writeb(qts, CHAC0, 8);
    qtest_writeb(qts, MCH0, 3);
    qtest_writeb(qts, CR0, 1);
    settings(qts, 0xfc00, 0x1f00);
    qtest_clock_step(qts, 544000 + 32767999);
    g_assert_cmpuint(qtest_readb(qts, ST) & 0x60, ==, 0);
    qtest_clock_step(qts, 1); /* 65536 clocks of the 2 MHz sensor ICO. */
    g_assert_cmpuint(qtest_readb(qts, ST), ==, 0x24);
    qtest_writeb(qts, ST, 0);
    qtest_clock_step(qts, 1);
    g_assert_cmpuint(qtest_readb(qts, ST), ==, 4);
    qtest_clock_step(qts, 32767999); /* Reference overflows at 65536 us. */
    g_assert_cmpuint(qtest_readb(qts, ST), ==, 0x44);
    qtest_quit(qts);
}

static void test_clock_stop(void)
{
    QTestState *qts = ctsu_init();

    qtest_writeb(qts, CR0, 1);
    settings(qts, 0, 0x100);
    qtest_clock_step(qts, 10000);
    qtest_writel(qts, MSTPD, 0xffffffff);
    qtest_writeb(qts, CR0, 0x10);
    qtest_writew(qts, SO1, 0xffff);
    g_assert_cmpuint(qtest_readw(qts, SO1), ==, 0);
    g_assert_cmpuint(qtest_readb(qts, ST), ==, 0);
    qtest_clock_step(qts, 1000000);
    qtest_writel(qts, MSTPD, RUN_D);
    g_assert_cmpuint(qtest_readw(qts, SO1), ==, 0x100);
    qtest_clock_step(qts, 23999);
    state(qts, 3);
    qtest_clock_step(qts, 1);
    state(qts, 4);
    qtest_clock_step(qts, 10000);
    qtest_writel(qts, 0x80020, 0x100); /* PCLKB halves, retain cycles. */
    qtest_clock_step(qts, 133999);
    state(qts, 4);
    qtest_clock_step(qts, 1);
    state(qts, 5);
    qtest_clock_step(qts, 1000);
    state(qts, 0);
    g_assert_cmpuint(qtest_readw(qts, SC), ==, 144);
    g_assert_cmpuint(qtest_readw(qts, RC), ==, 144);
    qtest_quit(qts);
}

static void test_elc(void)
{
    QTestState *qts = ctsu_init();

    qtest_writel(qts, 0x80014, 0xfffffdff); /* ELC enabled. */
    qtest_writeb(qts, 0x8b10f, 0x69); /* ELSR14 = software event. */
    qtest_writeb(qts, 0x8b100, 0xff);
    qtest_writeb(qts, CR0, 3);
    for (unsigned scan = 0; scan < 2; scan++) {
        state(qts, 0);
        qtest_writeb(qts, 0x8b12d, 0x7e);
        qtest_writeb(qts, 0x8b12d, 0x7f);
        qtest_clock_step(qts, 1000);
        state(qts, 2);
        settings(qts, 0, 0x100);
        qtest_clock_step(qts, 111500);
        state(qts, 0);
        g_assert_cmpuint(qtest_readw(qts, SC), ==, 77);
        g_assert_cmpuint(qtest_readw(qts, RC), ==, 77);
        g_assert_cmpuint(qtest_readb(qts, CR0), ==, 3);
        qtest_clock_step(qts, 250);
    }
    qtest_writeb(qts, CR0, 0x10);
    g_assert_cmpuint(qtest_readb(qts, MCH0), ==, 15);
    g_assert_cmpuint(qtest_readb(qts, ST), ==, 0);
    g_assert_cmpuint(qtest_readw(qts, SC), ==, 0);
    qtest_quit(qts);
}

static void test_registers(void)
{
    QTestState *qts = qtest_init("-M rx113-r5f51138");

    qtest_writel(qts, MSTPD, RUN_D); /* PRC1 protection. */
    g_assert_cmpuint(qtest_readl(qts, MSTPD), ==, 0xffffffff);
    qtest_writeb(qts, CR1, 0xff);
    qtest_writew(qts, 0x803fe, 0xa503);
    qtest_writel(qts, MSTPD, 0);
    g_assert_cmpuint(qtest_readl(qts, MSTPD), ==, 0xffff73ff);
    for (unsigned a = 0; a < 0x12; a++) {
        g_assert_cmpuint(qtest_readb(qts, CTSU + a), ==,
                         a == 4 || a == 5 ? 15 : 0);
    }
    for (unsigned a = 0x12; a <= 0x1c; a += 2) {
        g_assert_cmpuint(qtest_readw(qts, CTSU + a), ==, 0);
    }
    qtest_writew(qts, CR0, 0xffff);
    qtest_writeb(qts, SO1, 0xff);
    qtest_writew(qts, SO1 + 1, 0xffff);
    qtest_writel(qts, SO0, 0xffffffff);
    qtest_writew(qts, SC, 0xffff);
    g_assert_cmpuint(qtest_readb(qts, CR0), ==, 0);
    g_assert_cmpuint(qtest_readw(qts, SO0), ==, 0);
    g_assert_cmpuint(qtest_readw(qts, SO1), ==, 0);
    g_assert_cmpuint(qtest_readw(qts, SC), ==, 0);
    qtest_writeb(qts, SDPRS, 0xff);
    qtest_writeb(qts, CHAC1, 0xff);
    qtest_writeb(qts, CTSU + 0xc, 0xff);
    qtest_writeb(qts, CTSU + 0x10, 0xff);
    qtest_writew(qts, SSC, 0xffff);
    qtest_writew(qts, SO1, 0xffff);
    g_assert_cmpuint(qtest_readb(qts, SDPRS), ==, 0x7f);
    g_assert_cmpuint(qtest_readb(qts, CHAC1), ==, 15);
    g_assert_cmpuint(qtest_readb(qts, CTSU + 0xc), ==, 15);
    g_assert_cmpuint(qtest_readb(qts, CTSU + 0x10), ==, 0x33);
    g_assert_cmpuint(qtest_readw(qts, SSC), ==, 0x0f00);
    g_assert_cmpuint(qtest_readw(qts, SO1), ==, 0x7fff);
    qtest_quit(qts);
}

static void test_dtc(void)
{
    QTestState *qts = ctsu_init();

    qtest_writel(qts, 0x80010, 0xcfffffff);
    qtest_writel(qts, 0x82404, 0x400);
    qtest_writeb(qts, 0x8240c, 1);
    qtest_writeb(qts, 0x87207, 0x30);
    for (unsigned n = 60; n <= 61; n++) {
        unsigned info = n == 60 ? 0x1000 : 0x1010;

        qtest_writel(qts, 0x400 + 4 * n, info);
        qtest_writeb(qts, 0x87100 + n, 1);
        /* Word block transfers; repeat destination for WR, source for RD. */
        qtest_writel(qts, info, n == 60 ? 0x98080000 : 0x98180000);
        qtest_writel(qts, info + 4, n == 60 ? 0x2000 : SC);
        qtest_writel(qts, info + 8, n == 60 ? SSC : 0x3000);
        qtest_writel(qts, info + 12, n == 60 ? 0x03030002 : 0x02020002);
    }
    for (unsigned i = 0; i < 2; i++) {
        qtest_writew(qts, 0x2000 + i * 6, 0);
        qtest_writew(qts, 0x2002 + i * 6, 0);
        qtest_writew(qts, 0x2004 + i * 6, 0x100);
    }
    qtest_writeb(qts, CR1, 0x43);
    qtest_writeb(qts, CHAC0, 9);
    qtest_writeb(qts, CR0, 1);
    qtest_clock_step(qts, 1); /* DTC writes SSC/SO0/SO1 without CPU service. */
    state(qts, 3);
    qtest_clock_step(qts, 300000);
    state(qts, 0);
    g_assert_cmpuint(qtest_readw(qts, 0x3000), ==, 77);
    g_assert_cmpuint(qtest_readw(qts, 0x3002), ==, 77);
    g_assert_cmpuint(qtest_readw(qts, 0x3004), ==, 154);
    g_assert_cmpuint(qtest_readw(qts, 0x3006), ==, 77);
    g_assert_cmpuint(qtest_readb(qts, ST) & 0x10, ==, 0);
    g_assert_cmpuint(qtest_readb(qts, 0x8713c), ==, 0);
    g_assert_cmpuint(qtest_readb(qts, 0x8713d), ==, 0);
    g_assert_cmpuint(qtest_readb(qts, IR(62)), ==, 1);
    qtest_quit(qts);
}

static void migration_wait(QTestState *qts)
{
    for (unsigned i = 0; i < 1000; i++) {
        QDict *rsp = qtest_qmp(qts, "{'execute':'query-migrate'}");
        QDict *result = qdict_get_qdict(rsp, "return");
        const char *status = qdict_get_try_str(result, "status");
        bool done = status && !strcmp(status, "completed");

        g_assert_cmpstr(status, !=, "failed");
        qobject_unref(rsp);
        if (done) {
            return;
        }
        g_usleep(10000);
    }
    g_assert_not_reached();
}

static void test_migration(const void *opaque)
{
    unsigned phase = GPOINTER_TO_UINT(opaque);
    bool stopped = phase == 6;
    unsigned elapsed = phase == 2 ? 0 : phase == 3 ? 10000 :
                       phase == 5 ? 111250 : 44000;
    QTestState *src = ctsu_init();
    QTestState *dst;
    int64_t now;
    g_autofree char *dir = g_dir_make_tmp("ctsu-XXXXXX", NULL);
    g_autofree char *file = g_strdup_printf("%s/state", dir);
    g_autofree char *uri = g_strdup_printf("file:%s", file);

    qtest_writeb(src, CR0, 1);
    if (phase != 2) {
        settings(src, 0, 0x100);
    }
    now = qtest_clock_step(src, MAX(elapsed, 1));
    if (stopped) {
        qtest_writel(src, MSTPD, 0xffffffff);
    }
    qtest_qmp_assert_success(src, "{'execute':'migrate',"
                            "'arguments':{'uri':%s}}", uri);
    migration_wait(src);
    dst = qtest_init("-M rx113-r5f51138 -incoming defer");
    qtest_clock_set(dst, now);
    qtest_qmp_assert_success(dst, "{'execute':'migrate-incoming',"
                            "'arguments':{'uri':%s}}", uri);
    migration_wait(dst);
    if (stopped) {
        qtest_clock_step(dst, 1000000);
        g_assert_cmpuint(qtest_readb(dst, ST), ==, 0);
        qtest_writel(dst, MSTPD, RUN_D);
    }
    state(dst, stopped ? 4 : phase);
    if (phase == 2) {
        settings(dst, 0, 0x100);
    }
    qtest_clock_step(dst, 111500 - elapsed - 1);
    state(dst, 5);
    qtest_clock_step(dst, 1);
    state(dst, 0);
    g_assert_cmpuint(qtest_readw(dst, SC), ==, 77);
    g_assert_cmpuint(qtest_readw(dst, RC), ==, 77);
    qtest_quit(dst);
    qtest_quit(src);
    unlink(file);
    rmdir(dir);
}

static void test_frequencies(void)
{
    QTestState *qts = qtest_init("-M rx113-r5f51138 "
                               "-global rx113-ctsu.ref-ico-hz=2000000");
    QDict *rsp;

    configure(qts);
    rsp = qtest_qmp(qts, "{'execute':'qom-get','arguments':{"
                   "'path':'/machine/mcu/ctsu','property':'ts00-ico-hz'}}");
    g_assert_cmpuint(qdict_get_int(rsp, "return"), ==, 2000000);
    qobject_unref(rsp);
    for (unsigned i = 0; i < 2; i++) {
        qtest_writeb(qts, CR0, 1);
        settings(qts, 0, 0x100);
        qtest_clock_step(qts, 111500);
        g_assert_cmpuint(qtest_readw(qts, SC), ==, i ? 0 : 154);
        g_assert_cmpuint(qtest_readw(qts, RC), ==, 154);
        qtest_qmp_assert_success(qts, "{'execute':'qom-set','arguments':{"
                                "'path':'/machine/mcu/ctsu',"
                                "'property':'ts00-ico-hz','value':0}}");
    }
    qtest_quit(qts);
}

static void test_timing_parameters(void)
{
    static const unsigned base[] = { 510, 126, 62 };
    QTestState *qts = ctsu_init();

    for (unsigned op = 0; op < 3; op++) {
        for (unsigned pr = 0; pr < 3; pr++) {
            unsigned div = 4U << op;
            unsigned ratio = pr + 1;
            unsigned repeat = pr + 1;
            unsigned quarters = 4 * base[pr] * (ratio + 1) + base[pr] - 2;
            unsigned measure = quarters * div * 250 / 4 * repeat;
            unsigned stabilize = 17 * div * 250; /* SST=8 proportional. */
            unsigned end = 500U << op;

            qtest_writeb(qts, CR0, 0x10);
            qtest_writeb(qts, MCH0, 0);
            qtest_writeb(qts, CR1, 3 | (op << 4));
            qtest_writeb(qts, SST, 8);
            qtest_writeb(qts, SDPRS, (pr << 4) | ratio);
            qtest_writeb(qts, CR0, 1);
            settings(qts, (repeat - 1) << 10, 0x100);
            qtest_clock_step(qts, stabilize - 1);
            state(qts, 3);
            qtest_clock_step(qts, 1);
            state(qts, 4);
            qtest_clock_step(qts, measure - 1);
            state(qts, 4);
            qtest_clock_step(qts, 1);
            state(qts, 5);
            qtest_clock_step(qts, end);
            state(qts, 0);
            g_assert_cmpuint(qtest_readw(qts, SC), ==, measure / 1000);
            g_assert_cmpuint(qtest_readw(qts, RC), ==, measure / 1000);
        }
    }
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/rx113-ctsu/registers", test_registers);
    qtest_add_func("/rx113-ctsu/timing", test_timing);
    qtest_add_func("/rx113-ctsu/restart-reset", test_restart_reset);
    qtest_add_func("/rx113-ctsu/scan", test_scan);
    qtest_add_func("/rx113-ctsu/unread", test_unread);
    qtest_add_func("/rx113-ctsu/mutual", test_mutual);
    qtest_add_func("/rx113-ctsu/analog-trim", test_analog_trim);
    qtest_add_func("/rx113-ctsu/fixed-pins", test_fixed_pins);
    qtest_add_func("/rx113-ctsu/overflow", test_overflow);
    qtest_add_func("/rx113-ctsu/overflow-during-measurement",
                   test_overflow_during_measurement);
    qtest_add_func("/rx113-ctsu/clock-stop", test_clock_stop);
    qtest_add_func("/rx113-ctsu/elc", test_elc);
    qtest_add_func("/rx113-ctsu/frequencies", test_frequencies);
    qtest_add_func("/rx113-ctsu/dtc", test_dtc);
    qtest_add_func("/rx113-ctsu/timing-parameters", test_timing_parameters);
    for (unsigned phase = 2; phase <= 6; phase++) {
        g_autofree char *name =
            g_strdup_printf("/rx113-ctsu/migration-%u", phase);

        qtest_add_data_func(name, GUINT_TO_POINTER(phase), test_migration);
    }
    return g_test_run();
}
