/*
 * RX113 MTU2a register/timing tests (R01UH0448EJ0120, section 20).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define BASE 0x88600
#define IR(n) (0x87000 + (n))
#define MSTPCRA 0x80010
#define SCKCR 0x80020
#define START (BASE + 0x80)
#define SYNC (BASE + 0x81)
#define RWE (BASE + 0x84)
#define MTU_PATH "/machine/mcu/mtu"

static const unsigned tcr[] = { 0x100, 0x180, 0x200, 0, 1,
                                0x284, 0x294, 0x2a4 };
static const unsigned count[] = { 0x106, 0x186, 0x206, 0x10, 0x12,
                                  0x280, 0x290, 0x2a0 };
static const unsigned tgr[][6] = {
    { 0x108, 0x10a, 0x10c, 0x10e, 0x120, 0x122 },
    { 0x188, 0x18a }, { 0x208, 0x20a },
    { 0x18, 0x1a, 0x24, 0x26 }, { 0x1c, 0x1e, 0x28, 0x2a },
    { 0x282 }, { 0x292 }, { 0x2a2 },
};
static const unsigned tier[] = { 0x104, 0x184, 0x204, 8, 9 };
static const unsigned mode[] = { 0x101, 0x181, 0x201, 2, 3 };
static const unsigned start_bit[] = { 0, 1, 2, 6, 7, 2, 1, 0 };
static const unsigned vector[] = { 114, 121, 125, 129, 134, 139, 140, 141 };

static QTestState *mtu_init(void)
{
    QTestState *qts = qtest_init("-M rx113-r5f51138");

    qtest_writew(qts, 0x803fe, 0xa503);
    qtest_writeb(qts, 0x80036, 0);
    qtest_clock_step(qts, 34500);
    qtest_writew(qts, 0x80026, 0x100);
    qtest_writel(qts, SCKCR, 0); /* PCLKB = 32 MHz */
    qtest_writel(qts, MSTPCRA, 0xeffffdff);
    return qts;
}

static void start_channel(QTestState *qts, unsigned ch, bool run)
{
    unsigned addr = ch < 5 ? START : BASE + 0x2b4;

    qtest_writeb(qts, addr, run ? 1 << start_bit[ch] : 0);
}

static void test_compare(void)
{
    static const unsigned ngr[] = { 6, 2, 2, 4, 4, 1, 1, 1 };
    QTestState *qts = mtu_init();

    for (unsigned ch = 0; ch < 8; ch++) {
        qtest_writeb(qts, BASE + tcr[ch], 1); /* /4: 125 ns/count */
        for (unsigned gr = 0; gr < ngr[ch]; gr++) {
            unsigned v = vector[ch] + gr + (ch == 0 && gr >= 4);

            if (ch < 5) {
                qtest_writeb(qts, BASE + tier[ch], 15);
                if (ch == 0) {
                    qtest_writeb(qts, BASE + 0x124, 3);
                }
            } else {
                qtest_writeb(qts, BASE + 0x2b2, 7);
            }
            qtest_writew(qts, BASE + count[ch], 0);
            qtest_writew(qts, BASE + tgr[ch][gr], 9);
            start_channel(qts, ch, true);
            qtest_clock_step(qts, (ch < 5 ? 10 : 9) * 125 - 1);
            g_assert_cmpuint(qtest_readb(qts, IR(v)), ==, 0);
            g_assert_cmpuint(qtest_readw(qts, BASE + count[ch]), ==,
                             ch < 5 ? 9 : 8);
            qtest_clock_step(qts, 1);
            g_assert_cmpuint(qtest_readb(qts, IR(v)), ==, 1);
            g_assert_cmpuint(qtest_readw(qts, BASE + count[ch]), ==,
                             ch < 5 ? 10 : 9);
            start_channel(qts, ch, false);
            qtest_writeb(qts, IR(v), 0);
            qtest_writew(qts, BASE + tgr[ch][gr], 0xffff);
        }
    }
    qtest_quit(qts);
}

static void test_clear(void)
{
    static const unsigned ngr[] = { 4, 2, 2, 4, 4, 1, 1, 1 };
    static const unsigned clear[] = { 1, 2, 5, 6 };
    QTestState *qts = mtu_init();

    for (unsigned ch = 0; ch < 8; ch++) {
        for (unsigned gr = 0; gr < ngr[ch]; gr++) {
            qtest_writew(qts, BASE + count[ch], 0);
            qtest_writew(qts, BASE + tgr[ch][gr], 9);
            qtest_writeb(qts, BASE + tcr[ch],
                         1 | (ch < 5 ? clear[gr] << 5 : 0));
            if (ch >= 5) {
                qtest_writeb(qts, BASE + 0x2b6, 7);
            }
            start_channel(qts, ch, true);
            qtest_clock_step(qts, (ch < 5 ? 10 : 9) * 125);
            g_assert_cmpuint(qtest_readw(qts, BASE + count[ch]), ==, 0);
            qtest_clock_step(qts, 250);
            g_assert_cmpuint(qtest_readw(qts, BASE + count[ch]), ==, 2);
            start_channel(qts, ch, false);
            qtest_writew(qts, BASE + tgr[ch][gr], 0xffff);
        }
    }
    qtest_quit(qts);
}

static void test_overflow(void)
{
    QTestState *qts = mtu_init();

    for (unsigned ch = 0; ch < 5; ch++) {
        unsigned v = vector[ch] + (ch == 1 || ch == 2 ? 2 : 4);

        qtest_writeb(qts, BASE + tcr[ch], 1);
        qtest_writeb(qts, BASE + tier[ch], 0x10);
        qtest_writew(qts, BASE + count[ch], 0xfffe);
        start_channel(qts, ch, true);
        qtest_clock_step(qts, 249);
        g_assert_cmpuint(qtest_readb(qts, IR(v)), ==, 0);
        g_assert_cmphex(qtest_readw(qts, BASE + count[ch]), ==, 0xffff);
        qtest_clock_step(qts, 1);
        g_assert_cmpuint(qtest_readb(qts, IR(v)), ==, 1);
        g_assert_cmpuint(qtest_readw(qts, BASE + count[ch]), ==, 0);
        start_channel(qts, ch, false);
    }
    qtest_quit(qts);
}

/* PWM duty is observed as the interval between duty and cycle matches. */
static void test_pwm(void)
{
    QTestState *qts = mtu_init();

    for (unsigned ch = 0; ch < 5; ch++) {
        for (unsigned md = 2; md <= (ch < 3 ? 3 : 2); md++) {
            unsigned io = ch < 3 ? tcr[ch] + 2 : 4 + (ch - 3) * 2;

            qtest_writew(qts, BASE + count[ch], 0);
            qtest_writeb(qts, BASE + mode[ch], md);
            qtest_writeb(qts, BASE + io, 0x21); /* A low, B high */
            qtest_writeb(qts, BASE + tcr[ch], 0x21); /* /4, clear A */
            qtest_writew(qts, BASE + tgr[ch][0], 99);
            qtest_writew(qts, BASE + tgr[ch][1], 24);
            qtest_writeb(qts, BASE + tier[ch], 3);
            start_channel(qts, ch, true);
            for (unsigned period = 0; period < 3; period++) {
                qtest_clock_step(qts, 3124);
                g_assert_cmpuint(qtest_readb(qts, IR(vector[ch] + 1)), ==, 0);
                qtest_clock_step(qts, 1); /* duty: 25 ticks */
                g_assert_cmpuint(qtest_readb(qts, IR(vector[ch] + 1)), ==, 1);
                qtest_clock_step(qts, 9374);
                g_assert_cmpuint(qtest_readb(qts, IR(vector[ch])), ==, 0);
                qtest_clock_step(qts, 1); /* cycle: 100 ticks */
                g_assert_cmpuint(qtest_readb(qts, IR(vector[ch])), ==, 1);
                g_assert_cmpuint(qtest_readw(qts, BASE + count[ch]), ==, 0);
                qtest_writeb(qts, IR(vector[ch]), 0);
                qtest_writeb(qts, IR(vector[ch] + 1), 0);
            }
            start_channel(qts, ch, false);
        }
    }
    qtest_quit(qts);
}

static void test_buffer_sync(void)
{
    QTestState *qts = mtu_init();

    qtest_writeb(qts, SYNC, 3);
    qtest_writew(qts, BASE + count[0], 17);
    g_assert_cmpuint(qtest_readw(qts, BASE + count[1]), ==, 17);
    qtest_writew(qts, BASE + count[1], 0);
    qtest_writeb(qts, BASE + mode[0], 0x32); /* PWM1, A/B buffers */
    qtest_writeb(qts, BASE + 0x126, 3); /* transfer on clear */
    qtest_writeb(qts, BASE + tcr[0], 0x21);
    qtest_writeb(qts, BASE + tcr[1], 0x61); /* sync clear */
    qtest_writew(qts, BASE + tgr[0][0], 99);
    qtest_writew(qts, BASE + tgr[0][1], 24);
    qtest_writew(qts, BASE + tgr[0][2], 199);
    qtest_writew(qts, BASE + tgr[0][3], 49);
    qtest_writeb(qts, BASE + tier[0], 15);
    qtest_writeb(qts, START, 3);
    qtest_clock_step(qts, 12500);
    g_assert_cmpuint(qtest_readw(qts, BASE + count[0]), ==, 0);
    g_assert_cmpuint(qtest_readw(qts, BASE + count[1]), ==, 0);
    g_assert_cmpuint(qtest_readw(qts, BASE + tgr[0][0]), ==, 199);
    g_assert_cmpuint(qtest_readw(qts, BASE + tgr[0][1]), ==, 49);
    qtest_writeb(qts, IR(114), 0);
    qtest_clock_step(qts, 24999);
    g_assert_cmpuint(qtest_readb(qts, IR(114)), ==, 0);
    qtest_clock_step(qts, 1);
    g_assert_cmpuint(qtest_readb(qts, IR(114)), ==, 1);
    g_assert_cmpuint(qtest_readb(qts, IR(116)), ==, 0); /* buffer C */
    g_assert_cmpuint(qtest_readb(qts, IR(117)), ==, 0); /* buffer D */
    qtest_quit(qts);
}

static void test_clock_stop(void)
{
    QTestState *qts = mtu_init();

    for (unsigned ch = 0; ch < 8; ch++) {
        qtest_writel(qts, SCKCR, 0);
        qtest_writeb(qts, BASE + tcr[ch], 1);
        qtest_writew(qts, BASE + count[ch], 0);
        start_channel(qts, ch, true);
        qtest_clock_step(qts, 1312); /* 10.496 ticks */
        g_assert_cmpuint(qtest_readw(qts, BASE + count[ch]), ==, 10);
        qtest_writel(qts, SCKCR, 0x100); /* half frequency */
        qtest_writel(qts, MSTPCRA, 0xefffffff);
        qtest_writew(qts, BASE + count[ch], 1234); /* blocked */
        g_assert_cmpuint(qtest_readw(qts, BASE + count[ch]), ==, 0);
        for (unsigned i = 0; i < 4; i++) {
            qtest_writel(qts, SCKCR, 0);
            qtest_writel(qts, SCKCR, 0x100);
        }
        qtest_clock_step(qts, 1000000);
        qtest_writel(qts, MSTPCRA, 0xeffffdff);
        g_assert_cmpuint(qtest_readw(qts, BASE + count[ch]), ==, 10);
        qtest_clock_step(qts, 125);
        g_assert_cmpuint(qtest_readw(qts, BASE + count[ch]), ==, 10);
        qtest_clock_step(qts, 1);
        g_assert_cmpuint(qtest_readw(qts, BASE + count[ch]), ==, 11);
        start_channel(qts, ch, false);
    }
    qtest_quit(qts);
}

static void pin(QTestState *qts, unsigned n, bool level)
{
    qtest_set_irq_in(qts, MTU_PATH, "mtclk", n, level);
}

static void test_underflow(void)
{
    QTestState *qts = mtu_init();

    for (unsigned ch = 1; ch <= 2; ch++) {
        unsigned a = (ch - 1) * 2;
        unsigned v = ch == 1 ? 124 : 128;

        qtest_writeb(qts, BASE + mode[ch], 4); /* quadrature x4 */
        qtest_writeb(qts, BASE + tier[ch], 0x30);
        qtest_writew(qts, BASE + count[ch], 0);
        start_channel(qts, ch, true);
        pin(qts, a + 1, true); /* A low, B rising: down */
        g_assert_cmpuint(qtest_readb(qts, IR(v)), ==, 1);
        g_assert_cmphex(qtest_readw(qts, BASE + count[ch]), ==, 0xffff);
        g_assert_cmphex(qtest_readb(qts, BASE + tcr[ch] + 5) & 0x80, ==, 0);
        pin(qts, a + 1, false); /* A low, B falling: up */
        g_assert_cmpuint(qtest_readb(qts, IR(v - 1)), ==, 1);
        g_assert_cmpuint(qtest_readw(qts, BASE + count[ch]), ==, 0);
        g_assert_cmphex(qtest_readb(qts, BASE + tcr[ch] + 5) & 0x80, ==, 0x80);
        start_channel(qts, ch, false);
    }
    qtest_quit(qts);
}

static void test_cascade(void)
{
    QTestState *qts = mtu_init();

    qtest_writeb(qts, BASE + tcr[1], 7); /* MTU2 overflow -> MTU1 */
    qtest_writeb(qts, BASE + tcr[2], 1);
    qtest_writew(qts, BASE + count[2], 0xffff);
    qtest_writeb(qts, START, 6);
    qtest_clock_step(qts, 125);
    g_assert_cmpuint(qtest_readw(qts, BASE + count[1]), ==, 1);
    g_assert_cmpuint(qtest_readw(qts, BASE + count[2]), ==, 0);
    qtest_writeb(qts, START, 0);
    qtest_writeb(qts, BASE + mode[2], 4);
    qtest_writew(qts, BASE + count[1], 0);
    qtest_writeb(qts, BASE + tier[1], 0x20);
    qtest_writeb(qts, START, 6);
    pin(qts, 3, true);
    g_assert_cmphex(qtest_readw(qts, BASE + count[1]), ==, 0xffff);
    g_assert_cmphex(qtest_readw(qts, BASE + count[2]), ==, 0xffff);
    g_assert_cmpuint(qtest_readb(qts, IR(124)), ==, 1);
    qtest_quit(qts);
}

static void test_access_reset(void)
{
    QTestState *qts = mtu_init();

    g_assert_cmphex(qtest_readw(qts, BASE + tgr[0][0]), ==, 0xffff);
    qtest_writeb(qts, RWE, 0); /* no read: cannot clear RWE */
    g_assert_cmpuint(qtest_readb(qts, RWE), ==, 1);
    qtest_writeb(qts, RWE, 0);
    qtest_writew(qts, BASE + count[3], 50);
    qtest_writew(qts, BASE + tgr[3][0], 100);
    g_assert_cmpuint(qtest_readw(qts, BASE + count[3]), ==, 0);
    qtest_writeb(qts, RWE, 1);
    g_assert_cmphex(qtest_readw(qts, BASE + tgr[3][0]), ==, 0xffff);
    qtest_writew(qts, BASE + count[3], 50);
    qtest_writeb(qts, BASE + count[3], 9); /* invalid byte access */
    g_assert_cmpuint(qtest_readw(qts, BASE + count[3]), ==, 50);
    qtest_writeb(qts, START, 0xff);
    g_assert_cmphex(qtest_readb(qts, START), ==, 0xc7);
    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    g_assert_cmpuint(qtest_readb(qts, START), ==, 0); /* MSTP */
    qtest_writew(qts, 0x803fe, 0xa502); /* reset restored PRC1 protection */
    qtest_writel(qts, MSTPCRA, 0xeffffdff);
    g_assert_cmpuint(qtest_readb(qts, START), ==, 0);
    g_assert_cmpuint(qtest_readw(qts, BASE + count[3]), ==, 0);
    g_assert_cmphex(qtest_readw(qts, BASE + tgr[3][0]), ==, 0xffff);
    qtest_quit(qts);
}

static void test_prescalers(void)
{
    static const unsigned div[8][8] = {
        { 1, 4, 16, 64 }, { 1, 4, 16, 64, 0, 0, 256 },
        { 1, 4, 16, 64, 0, 0, 0, 1024 },
        { 1, 4, 16, 64, 256, 1024 }, { 1, 4, 16, 64, 256, 1024 },
        { 1, 4, 16, 64 }, { 1, 4, 16, 64 }, { 1, 4, 16, 64 },
    };
    QTestState *qts = mtu_init();

    for (unsigned ch = 0; ch < 8; ch++) {
        for (unsigned sel = 0; sel < 8; sel++) {
            if (!div[ch][sel]) {
                continue;
            }
            for (unsigned both = 0; both <= (ch < 5); both++) {
                unsigned divisor = div[ch][sel];
                unsigned ns;

                if (both && divisor > 1) {
                    divisor /= 2;
                }
                ns = divisor * 125; /* four counter ticks at 32 MHz */
                qtest_writeb(qts, BASE + tcr[ch], sel | (both ? 16 : 0));
                qtest_writew(qts, BASE + count[ch], 0);
                start_channel(qts, ch, true);
                qtest_clock_step(qts, ns - 1);
                g_assert_cmpuint(qtest_readw(qts, BASE + count[ch]), ==, 3);
                qtest_clock_step(qts, 1);
                g_assert_cmpuint(qtest_readw(qts, BASE + count[ch]), ==, 4);
                start_channel(qts, ch, false);
            }
        }
    }
    qtest_quit(qts);
}

static void test_zero_period(void)
{
    QTestState *qts = mtu_init();

    qtest_writew(qts, BASE + tgr[0][0], 0);
    qtest_writeb(qts, BASE + tcr[0], 0x20); /* clear A, /1 */
    qtest_writeb(qts, BASE + tier[0], 1);
    start_channel(qts, 0, true);
    qtest_clock_step(qts, 31);
    g_assert_cmpuint(qtest_readb(qts, IR(114)), ==, 0);
    qtest_clock_step(qts, 1);
    g_assert_cmpuint(qtest_readb(qts, IR(114)), ==, 1);
    qtest_writeb(qts, IR(114), 0);
    /* Section 20.6.26: a flat request cannot cause a second rising edge. */
    qtest_clock_step(qts, 31536000000000000LL);
    g_assert_cmpuint(qtest_readb(qts, IR(114)), ==, 0);
    g_assert_cmpuint(qtest_readw(qts, BASE + count[0]), ==, 0);
    qtest_quit(qts);
}

static void test_zero_buffer(void)
{
    QTestState *qts = mtu_init();

    qtest_writeb(qts, BASE + mode[0], 0x10);
    qtest_writeb(qts, BASE + tcr[0], 0x20);
    qtest_writew(qts, BASE + tgr[0][0], 0);
    qtest_writew(qts, BASE + tgr[0][2], 9);
    qtest_writeb(qts, BASE + tier[0], 1);
    start_channel(qts, 0, true);
    qtest_clock_step(qts, 32);
    g_assert_cmpuint(qtest_readb(qts, IR(114)), ==, 1);
    g_assert_cmpuint(qtest_readw(qts, BASE + tgr[0][0]), ==, 9);
    qtest_writeb(qts, IR(114), 0);
    qtest_clock_step(qts, 312);
    /* Buffer transfer ended the flat request; a new match must have an edge. */
    g_assert_cmpuint(qtest_readb(qts, IR(114)), ==, 1);
    qtest_quit(qts);
}

static void test_match_mask_and_precedence(void)
{
    QTestState *qts = mtu_init();

    qtest_writeb(qts, BASE + tcr[0], 0x21);
    qtest_writew(qts, BASE + tgr[0][0], 9);
    start_channel(qts, 0, true);
    qtest_clock_step(qts, 1250);
    g_assert_cmpuint(qtest_readb(qts, IR(114)), ==, 0); /* disabled */
    qtest_writeb(qts, BASE + tier[0], 1);
    qtest_clock_step(qts, 1249);
    g_assert_cmpuint(qtest_readb(qts, IR(114)), ==, 0);
    qtest_clock_step(qts, 1);
    g_assert_cmpuint(qtest_readb(qts, IR(114)), ==, 1);
    start_channel(qts, 0, false);
    qtest_writew(qts, BASE + count[0], 0xffff);
    qtest_writew(qts, BASE + tgr[0][0], 0xffff);
    qtest_writeb(qts, BASE + tier[0], 0x11);
    start_channel(qts, 0, true);
    qtest_clock_step(qts, 125);
    g_assert_cmpuint(qtest_readw(qts, BASE + count[0]), ==, 0);
    g_assert_cmpuint(qtest_readb(qts, IR(118)), ==, 0); /* clear wins */
    qtest_quit(qts);
}

static void test_mtu5_stopped(void)
{
    QTestState *qts = mtu_init();

    qtest_writeb(qts, BASE + 0x2b2, 7);
    qtest_writeb(qts, BASE + 0x2b6, 7);
    for (unsigned ch = 5; ch < 8; ch++) {
        qtest_writew(qts, BASE + count[ch], 100);
        qtest_writew(qts, BASE + tgr[ch][0], 101);
        g_assert_cmpuint(qtest_readb(qts, IR(vector[ch])), ==, 1);
        g_assert_cmpuint(qtest_readw(qts, BASE + count[ch]), ==, 0);
    }
    qtest_quit(qts);
}

static void test_buffers(void)
{
    QTestState *qts = mtu_init();

    for (unsigned ch = 0; ch < 5; ch++) {
        if (ch == 1 || ch == 2) {
            continue;
        }
        qtest_writeb(qts, BASE + mode[ch], ch == 0 ? 0x70 : 0x30);
        qtest_writeb(qts, BASE + tcr[ch], 1);
        qtest_writew(qts, BASE + tgr[ch][0], 9);
        qtest_writew(qts, BASE + tgr[ch][1], 14);
        qtest_writew(qts, BASE + tgr[ch][2], 19);
        qtest_writew(qts, BASE + tgr[ch][3], 24);
        if (ch == 0) {
            qtest_writew(qts, BASE + tgr[ch][4], 29);
            qtest_writew(qts, BASE + tgr[ch][5], 34);
            qtest_writeb(qts, BASE + 0x124, 3);
        }
        start_channel(qts, ch, true);
        qtest_clock_step(qts, 1249);
        g_assert_cmpuint(qtest_readw(qts, BASE + tgr[ch][0]), ==, 9);
        qtest_clock_step(qts, 1);
        g_assert_cmpuint(qtest_readw(qts, BASE + tgr[ch][0]), ==, 19);
        qtest_clock_step(qts, 625);
        g_assert_cmpuint(qtest_readw(qts, BASE + tgr[ch][1]), ==, 24);
        if (ch == 0) {
            qtest_clock_step(qts, 1875); /* E match transfers F -> E */
            g_assert_cmpuint(qtest_readw(qts, BASE + tgr[ch][4]), ==, 34);
            g_assert_cmpuint(qtest_readb(qts, IR(119)), ==, 1);
            qtest_clock_step(qts, 625);
            /* F still compares while serving as a buffer (20.2.2). */
            g_assert_cmpuint(qtest_readb(qts, IR(120)), ==, 1);
        }
        start_channel(qts, ch, false);
    }
    qtest_quit(qts);
}

static void test_phase_modes(void)
{
    QTestState *qts = mtu_init();

    for (unsigned ch = 1; ch <= 2; ch++) {
        unsigned a = (ch - 1) * 2;
        unsigned b = a + 1;

        for (unsigned md = 5; md <= 7; md++) {
            pin(qts, a, false);
            pin(qts, b, false);
            qtest_writeb(qts, BASE + mode[ch], md);
            qtest_writew(qts, BASE + count[ch], 100);
            start_channel(qts, ch, true);
            if (md == 5) { /* A rising, direction B */
                pin(qts, a, true);
                g_assert_cmpuint(qtest_readw(qts, BASE + count[ch]), ==, 101);
                pin(qts, b, true); /* ignored edge */
                pin(qts, a, false); /* ignored edge */
                pin(qts, a, true);
            } else if (md == 6) { /* A rising/B low; B rising/A low */
                pin(qts, a, true);
                g_assert_cmpuint(qtest_readw(qts, BASE + count[ch]), ==, 101);
                pin(qts, a, false); /* ignored edge */
                pin(qts, b, true);
            } else { /* B edges, direction from phase difference */
                pin(qts, a, true); /* ignored edge */
                pin(qts, b, true);
                g_assert_cmpuint(qtest_readw(qts, BASE + count[ch]), ==, 101);
                pin(qts, b, false);
            }
            g_assert_cmpuint(qtest_readw(qts, BASE + count[ch]), ==, 100);
            start_channel(qts, ch, false);
        }
    }
    qtest_quit(qts);
}

static void test_external_clock(void)
{
    QTestState *qts = mtu_init();

    for (unsigned ch = 0; ch < 5; ch++) {
        unsigned sel = ch < 3 ? 4 : 6; /* MTCLKA */

        for (unsigned edge = 0; edge < 3; edge++) {
            pin(qts, 0, false);
            qtest_writeb(qts, BASE + tcr[ch], sel | (edge << 3));
            qtest_writew(qts, BASE + count[ch], 0);
            start_channel(qts, ch, true);
            pin(qts, 0, true);
            g_assert_cmpuint(qtest_readw(qts, BASE + count[ch]), ==,
                             edge == 1 ? 0 : 1);
            pin(qts, 0, false);
            g_assert_cmpuint(qtest_readw(qts, BASE + count[ch]), ==,
                             edge == 2 ? 2 : 1);
            qtest_writel(qts, MSTPCRA, 0xefffffff);
            pin(qts, 0, true);
            pin(qts, 0, false);
            qtest_writel(qts, MSTPCRA, 0xeffffdff);
            g_assert_cmpuint(qtest_readw(qts, BASE + count[ch]), ==,
                             edge == 2 ? 2 : 1);
            start_channel(qts, ch, false);
        }
    }
    qtest_quit(qts);
}

static void test_sync_precedence(void)
{
    QTestState *qts = mtu_init();

    qtest_writew(qts, BASE + count[1], 0xfffe);
    qtest_writeb(qts, SYNC, 3);
    qtest_writeb(qts, BASE + tcr[0], 0x21);
    qtest_writeb(qts, BASE + tcr[1], 0x61);
    qtest_writew(qts, BASE + tgr[0][0], 1);
    qtest_writeb(qts, BASE + tier[1], 0x11);
    qtest_writeb(qts, START, 3);
    qtest_clock_step(qts, 250);
    g_assert_cmpuint(qtest_readw(qts, BASE + count[1]), ==, 0);
    /* Compare still fires; synchronous clear suppresses only overflow. */
    g_assert_cmpuint(qtest_readb(qts, IR(121)), ==, 1);
    g_assert_cmpuint(qtest_readb(qts, IR(123)), ==, 0); /* sync clear wins */
    qtest_quit(qts);
}

static void migration_wait(QTestState *qts)
{
    for (unsigned i = 0; i < 1000; i++) {
        QDict *rsp = qtest_qmp(qts, "{'execute':'query-migrate'}");
        QDict *result = qdict_get_qdict(rsp, "return");
        const char *status = qdict_get_try_str(result, "status");
        bool done = status && !strcmp(status, "completed");

        if (status && !strcmp(status, "failed")) {
            g_error("Migration failed: %s",
                    qdict_get_try_str(result, "error-desc"));
        }
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
    bool stopped = GPOINTER_TO_INT(opaque);
    QTestState *src = mtu_init();
    QTestState *dst;
    int64_t now;
    g_autofree char *dir = g_dir_make_tmp("rx113-mtu-XXXXXX", NULL);
    g_autofree char *file = g_strdup_printf("%s/state", dir);
    g_autofree char *uri = g_strdup_printf("file:%s", file);

    g_assert_nonnull(dir);
    for (unsigned ch = 0; ch < 8; ch++) {
        qtest_writeb(src, BASE + tcr[ch], ch < 5 ? 0x21 : 1);
        qtest_writew(src, BASE + tgr[ch][0], 99);
        if (ch < 5) {
            qtest_writeb(src, BASE + tier[ch], 1);
        }
    }
    qtest_writeb(src, BASE + 0x2b2, 7);
    qtest_writeb(src, BASE + 0x2b6, 7);
    qtest_writeb(src, START, 0xc7);
    qtest_writeb(src, BASE + 0x2b4, 7);
    now = qtest_clock_step(src, 5062); /* 40.496 ticks */
    qtest_writel(src, SCKCR, 0x100);
    if (stopped) {
        qtest_writel(src, MSTPCRA, 0xefffffff);
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
        qtest_writel(dst, MSTPCRA, 0xeffffdff);
    }
    for (unsigned ch = 0; ch < 8; ch++) {
        g_assert_cmpuint(qtest_readw(dst, BASE + count[ch]), ==, 40);
    }
    qtest_clock_step(dst, 14625); /* 99 ticks reached on MTU5 */
    for (unsigned ch = 0; ch < 8; ch++) {
        g_assert_cmpuint(qtest_readb(dst, IR(vector[ch])), ==, 0);
    }
    qtest_clock_step(dst, 1);
    for (unsigned ch = 5; ch < 8; ch++) {
        g_assert_cmpuint(qtest_readb(dst, IR(vector[ch])), ==, 1);
        g_assert_cmpuint(qtest_readw(dst, BASE + count[ch]), ==, 0);
    }
    qtest_clock_step(dst, 250);
    for (unsigned ch = 0; ch < 5; ch++) {
        g_assert_cmpuint(qtest_readb(dst, IR(vector[ch])), ==, 1);
        g_assert_cmpuint(qtest_readw(dst, BASE + count[ch]), ==, 0);
    }
    qtest_quit(dst);
    qtest_quit(src);
    unlink(file);
    rmdir(dir);
}

static void test_migrate_continuous(void)
{
    QTestState *src = mtu_init();
    QTestState *dst;
    int64_t now;
    g_autofree char *dir = g_dir_make_tmp("rx113-mtu-XXXXXX", NULL);
    g_autofree char *file = g_strdup_printf("%s/state", dir);
    g_autofree char *uri = g_strdup_printf("file:%s", file);

    g_assert_nonnull(dir);
    qtest_writew(src, BASE + tgr[0][0], 0);
    qtest_writeb(src, BASE + tcr[0], 0x20);
    qtest_writeb(src, BASE + tier[0], 1);
    start_channel(src, 0, true);
    now = qtest_clock_step(src, 125);
    g_assert_cmpuint(qtest_readb(src, IR(114)), ==, 1);
    qtest_writeb(src, IR(114), 0);
    qtest_qmp_assert_success(src, "{'execute':'migrate',"
                            "'arguments':{'uri':%s}}", uri);
    migration_wait(src);
    dst = qtest_init("-M rx113-r5f51138 -incoming defer");
    qtest_clock_set(dst, now);
    qtest_qmp_assert_success(dst, "{'execute':'migrate-incoming',"
                            "'arguments':{'uri':%s}}", uri);
    migration_wait(dst);
    qtest_clock_step(dst, 1000);
    g_assert_cmpuint(qtest_readb(dst, IR(114)), ==, 0); /* no phantom edge */
    start_channel(dst, 0, false);
    start_channel(dst, 0, true);
    qtest_clock_step(dst, 32);
    g_assert_cmpuint(qtest_readb(dst, IR(114)), ==, 1); /* fresh start */
    qtest_quit(dst);
    qtest_quit(src);
    unlink(file);
    rmdir(dir);
}

#define PORT_BASE 0x8c000

static const unsigned tior[] = { 0x102, 0x182, 0x202, 4, 6 };
/* One pad per actual MTIOC signal; all are bonded in the 100-pin part. */
static const unsigned output_pad[][4] = {
    { 0x14, 0x15, 0x32, 0x02 }, { 0x20, 0x21 }, { 0x26, 0x27 },
    { 0x17, 0x22, 0x16, 0x23 }, { 0x24, 0x30, 0x25, 0x31 },
};

static unsigned pad_property(QTestState *qts, const char *property,
                             unsigned name)
{
    QDict *rsp = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'/machine/mcu/port',"
        "'property':%s}}", property);
    const char *str = qdict_get_str(rsp, "return");
    unsigned value;

    g_assert_cmpint(sscanf(str + (name >> 4) * 2, "%2x", &value), ==, 1);
    qobject_unref(rsp);
    return (value >> (name & 7)) & 1;
}

static void select_pad(QTestState *qts, unsigned name, unsigned psel)
{
    unsigned port = name >> 4, bit = name & 7;
    unsigned pmr = PORT_BASE + 0x60 + port;
    unsigned old = qtest_readb(qts, pmr);

    qtest_writeb(qts, pmr, old & ~(1 << bit));
    qtest_writeb(qts, PORT_BASE + 0x11f, 0);
    qtest_writeb(qts, PORT_BASE + 0x11f, 0x40);
    qtest_writeb(qts, PORT_BASE + 0x140 + port * 8 + bit, psel);
    qtest_writeb(qts, pmr, old | (1 << bit));
}

static void assert_pad(QTestState *qts, unsigned name, bool driven, bool level)
{
    g_assert_cmpuint(pad_property(qts, "driving", name), ==, driven);
    g_assert_cmpuint(pad_property(qts, "levels", name), ==, level);
    g_assert_cmpuint((qtest_readb(qts, PORT_BASE + 0x40 + (name >> 4)) >>
                     (name & 7)) & 1, ==, level);
}

static void set_io(QTestState *qts, unsigned ch, unsigned gr, unsigned io)
{
    unsigned addr = BASE + tior[ch] + gr / 2;
    unsigned shift = (gr & 1) * 4;
    unsigned old = qtest_readb(qts, addr);

    qtest_writeb(qts, addr, (old & ~(15 << shift)) | (io << shift));
}

/* Tables 20.12-27: every output's initial, set, clear and toggle levels. */
static void test_pad_normal(void)
{
    static const unsigned ngr[] = { 4, 2, 2, 4, 4 };
    QTestState *qts = mtu_init();

    qtest_writeb(qts, BASE + 0xa, 0xff);
    for (unsigned ch = 0; ch < 5; ch++) {
        qtest_writeb(qts, BASE + tcr[ch], 0x21);
        for (unsigned gr = 0; gr < ngr[ch]; gr++) {
            unsigned pad = output_pad[ch][gr];
            unsigned psel = pad == 0x14 ? 3 : 1;

            select_pad(qts, pad, psel);
            for (unsigned io = 0; io < 8; io++) {
                bool initial = io & 4;
                bool enabled = io & 3;
                bool result = (io & 3) == 3 ? !initial : (io & 3) == 2;

                start_channel(qts, ch, false);
                qtest_writew(qts, BASE + count[ch], 0);
                qtest_writew(qts, BASE + tgr[ch][0], 19);
                qtest_writew(qts, BASE + tgr[ch][gr], 9);
                set_io(qts, ch, gr, io);
                assert_pad(qts, pad, enabled, enabled && initial);
                start_channel(qts, ch, true);
                qtest_clock_step(qts, 1249);
                assert_pad(qts, pad, enabled, enabled && initial);
                qtest_clock_step(qts, 1);
                assert_pad(qts, pad, enabled, enabled && result);
                /* Stopping retains the output; rewriting TIOR initializes. */
                start_channel(qts, ch, false);
                qtest_clock_step(qts, 5000);
                assert_pad(qts, pad, enabled, enabled && result);
                set_io(qts, ch, gr, io);
                assert_pad(qts, pad, enabled, enabled && initial);
            }
            set_io(qts, ch, gr, 0);
            qtest_writew(qts, BASE + tgr[ch][gr], 0xffff);
        }
    }
    qtest_quit(qts);
}

/* 20.3.5: paired matches drive A/C, never B/D; equal TGRs retain level. */
static void test_pad_pwm1(void)
{
    static const unsigned ngr[] = { 4, 2, 2, 4, 4 };
    QTestState *qts = mtu_init();

    qtest_writeb(qts, BASE + 0xa, 0xff);
    for (unsigned ch = 0; ch < 5; ch++) {
        for (unsigned gr = 0; gr < ngr[ch]; gr += 2) {
            unsigned pad = output_pad[ch][gr];
            unsigned psel = pad == 0x14 ? 3 : 1;

            qtest_writeb(qts, BASE + mode[ch], 2);
            qtest_writeb(qts, BASE + tcr[ch], gr ? 0xa1 : 0x21);
            qtest_writew(qts, BASE + count[ch], 0);
            qtest_writew(qts, BASE + tgr[ch][gr], 99);
            qtest_writew(qts, BASE + tgr[ch][gr + 1], 24);
            select_pad(qts, pad, psel);
            select_pad(qts, output_pad[ch][gr + 1], 1);
            qtest_writeb(qts, BASE + tior[ch] + gr / 2, 0x21);
            assert_pad(qts, pad, true, false);
            assert_pad(qts, output_pad[ch][gr + 1], false, false);
            start_channel(qts, ch, true);
            for (unsigned cycle = 0; cycle < 3; cycle++) {
                qtest_clock_step(qts, 3124);
                assert_pad(qts, pad, true, false);
                qtest_clock_step(qts, 1);
                assert_pad(qts, pad, true, true);
                qtest_clock_step(qts, 9374);
                assert_pad(qts, pad, true, true);
                qtest_clock_step(qts, 1);
                assert_pad(qts, pad, true, false);
            }
            /* Unreachable duty match gives flat low (0%). */
            qtest_writew(qts, BASE + tgr[ch][gr + 1], 199);
            qtest_clock_step(qts, 25000);
            assert_pad(qts, pad, true, false);
            /* Equal matches retain low, then retain high after a duty match. */
            qtest_writew(qts, BASE + tgr[ch][gr + 1], 99);
            qtest_clock_step(qts, 25000);
            assert_pad(qts, pad, true, false);
            qtest_writew(qts, BASE + tgr[ch][gr + 1], 24);
            qtest_clock_step(qts, 3125);
            assert_pad(qts, pad, true, true);
            qtest_writew(qts, BASE + tgr[ch][gr + 1], 99);
            qtest_clock_step(qts, 21875);
            assert_pad(qts, pad, true, true); /* 100% */
            start_channel(qts, ch, false);
            /* Back to normal: A stays high until TIOR is written. */
            qtest_writeb(qts, BASE + mode[ch], 0);
            assert_pad(qts, pad, true, true);
            set_io(qts, ch, gr, 1);
            assert_pad(qts, pad, true, false);
            qtest_writeb(qts, BASE + tior[ch] + gr / 2, 0);
            qtest_writew(qts, BASE + tgr[ch][gr], 0xffff);
            qtest_writew(qts, BASE + tgr[ch][gr + 1], 0xffff);
        }
    }
    qtest_quit(qts);
}

static void test_pad_pwm2(void)
{
    QTestState *qts = mtu_init();

    for (unsigned ch = 0; ch < 3; ch++) {
        unsigned pad = output_pad[ch][0];

        select_pad(qts, pad, pad == 0x14 ? 3 : 1);
        qtest_writeb(qts, BASE + mode[ch], 3);
        qtest_writeb(qts, BASE + tcr[ch], 0x41); /* cycle B */
        qtest_writeb(qts, BASE + tior[ch], 0x12);
        qtest_writew(qts, BASE + tgr[ch][0], 24);
        qtest_writew(qts, BASE + tgr[ch][1], 99);
        start_channel(qts, ch, true);
        for (unsigned cycle = 0; cycle < 3; cycle++) {
            qtest_clock_step(qts, 3124);
            assert_pad(qts, pad, true, false);
            qtest_clock_step(qts, 1);
            assert_pad(qts, pad, true, true);
            qtest_clock_step(qts, 9375);
            assert_pad(qts, pad, true, false);
        }
        qtest_writew(qts, BASE + tgr[ch][0], 199);
        qtest_clock_step(qts, 25000);
        assert_pad(qts, pad, true, false);
        qtest_writew(qts, BASE + tgr[ch][0], 99);
        qtest_clock_step(qts, 12500);
        assert_pad(qts, pad, true, false);
        qtest_writew(qts, BASE + tgr[ch][0], 24);
        qtest_clock_step(qts, 3125);
        assert_pad(qts, pad, true, true);
        qtest_writew(qts, BASE + tgr[ch][0], 99);
        qtest_clock_step(qts, 21875);
        assert_pad(qts, pad, true, true);
        /* CPU zero writes restore initial level in PWM2. */
        qtest_writew(qts, BASE + count[ch], 0);
        assert_pad(qts, pad, true, false);
        start_channel(qts, ch, false);
    }
    qtest_quit(qts);
}

static void test_pad_output_control(void)
{
    QTestState *qts = mtu_init();
    static const unsigned pads[] = { 0x22, 0x24, 0x30, 0x23, 0x25, 0x31 };

    g_assert_cmphex(qtest_readb(qts, BASE + 0xa), ==, 0xc0);
    qtest_writeb(qts, BASE + 0xa, 0xff);
    qtest_writeb(qts, BASE + 4, 0x66);
    qtest_writeb(qts, BASE + 5, 0x66);
    qtest_writeb(qts, BASE + 6, 0x66);
    qtest_writeb(qts, BASE + 7, 0x66);
    for (unsigned i = 0; i < G_N_ELEMENTS(pads); i++) {
        select_pad(qts, pads[i], 1);
    }
    for (unsigned i = 0; i < G_N_ELEMENTS(pads); i++) {
        qtest_writeb(qts, BASE + 0xa, 1 << i);
        for (unsigned j = 0; j < G_N_ELEMENTS(pads); j++) {
            assert_pad(qts, pads[j], i == j, i == j);
        }
    }
    /* TRWER guards TOER and TOCR1/2, but not TOLBR (20.2.16). */
    qtest_writeb(qts, BASE + 0xe, 0x47);
    qtest_writeb(qts, BASE + 0xf, 0xff);
    qtest_writeb(qts, BASE + 0x36, 0xff);
    qtest_readb(qts, RWE);
    qtest_writeb(qts, RWE, 0);
    qtest_writeb(qts, BASE + 0xa, 0);
    qtest_writeb(qts, BASE + 0xe, 0);
    qtest_writeb(qts, BASE + 0xf, 0);
    qtest_writeb(qts, BASE + 0x36, 0x15);
    g_assert_cmphex(qtest_readb(qts, BASE + 0xe), ==, 0);
    g_assert_cmphex(qtest_readb(qts, BASE + 0x36), ==, 0x15);
    qtest_writeb(qts, RWE, 1);
    g_assert_cmphex(qtest_readb(qts, BASE + 0xa), ==, 0xe0);
    g_assert_cmphex(qtest_readb(qts, BASE + 0xe), ==, 0x47);
    g_assert_cmphex(qtest_readb(qts, BASE + 0xf), ==, 0xff);
    qtest_writeb(qts, BASE + 0xe, 0xf);
    qtest_writeb(qts, BASE + 0xe, 0x40);
    g_assert_cmphex(qtest_readb(qts, BASE + 0xe), ==, 0x4f);
    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    qtest_writew(qts, 0x803fe, 0xa503);
    qtest_writel(qts, MSTPCRA, 0xeffffdff);
    g_assert_cmphex(qtest_readb(qts, BASE + 0xa), ==, 0xc0);
    g_assert_cmphex(qtest_readb(qts, BASE + 0xe), ==, 0);
    g_assert_cmphex(qtest_readb(qts, BASE + 0xf), ==, 0);
    g_assert_cmphex(qtest_readb(qts, BASE + 0x36), ==, 0);
    for (unsigned i = 0; i < G_N_ELEMENTS(pads); i++) {
        assert_pad(qts, pads[i], false, false);
    }
    qtest_quit(qts);
}

static void test_pad_sync_and_buffers(void)
{
    QTestState *qts = mtu_init();

    /* Five PWM2 duty pads share MTU1 B's synchronous cycle (Fig. 20.27). */
    qtest_writeb(qts, SYNC, 3);
    qtest_writeb(qts, BASE + tcr[0], 0x61);
    qtest_writeb(qts, BASE + tcr[1], 0x41);
    qtest_writeb(qts, BASE + mode[0], 3);
    qtest_writeb(qts, BASE + mode[1], 3);
    qtest_writeb(qts, BASE + tior[0], 0x22);
    qtest_writeb(qts, BASE + tior[0] + 1, 0x22);
    qtest_writeb(qts, BASE + tior[1], 0x12);
    for (unsigned gr = 0; gr < 4; gr++) {
        unsigned pad = output_pad[0][gr];

        select_pad(qts, pad, pad == 0x14 ? 3 : 1);
        qtest_writew(qts, BASE + tgr[0][gr], gr == 3 ? 99 : 24);
    }
    select_pad(qts, 0x20, 1);
    qtest_writew(qts, BASE + tgr[1][0], 24);
    qtest_writew(qts, BASE + tgr[1][1], 99);
    qtest_writeb(qts, START, 3);
    qtest_clock_step(qts, 3125);
    for (unsigned gr = 0; gr < 4; gr++) {
        assert_pad(qts, output_pad[0][gr], true, gr != 3);
    }
    assert_pad(qts, 0x20, true, true);
    qtest_clock_step(qts, 9375);
    for (unsigned gr = 0; gr < 4; gr++) {
        assert_pad(qts, output_pad[0][gr], true, false);
    }
    assert_pad(qts, 0x20, true, false);
    qtest_writeb(qts, START, 0);
    qtest_writeb(qts, SYNC, 0);
    /* Buffered PWM1: new period/duty apply after the old cycle clears. */
    qtest_writeb(qts, BASE + mode[0], 0x32);
    qtest_writeb(qts, BASE + tcr[0], 0x21);
    qtest_writeb(qts, BASE + tior[0], 0x21);
    qtest_writeb(qts, BASE + 0x126, 3);
    qtest_writew(qts, BASE + tgr[0][0], 99);
    qtest_writew(qts, BASE + tgr[0][1], 24);
    qtest_writew(qts, BASE + tgr[0][2], 199);
    qtest_writew(qts, BASE + tgr[0][3], 49);
    assert_pad(qts, output_pad[0][2], false, false);
    start_channel(qts, 0, true);
    qtest_clock_step(qts, 3125);
    assert_pad(qts, 0x14, true, true);
    qtest_clock_step(qts, 9375);
    assert_pad(qts, 0x14, true, false);
    qtest_clock_step(qts, 6249);
    assert_pad(qts, 0x14, true, false);
    qtest_clock_step(qts, 1);
    assert_pad(qts, 0x14, true, true);
    /* Module stop retains both the current pad and the fractional clock. */
    qtest_writel(qts, MSTPCRA, 0xefffffff);
    qtest_clock_step(qts, 100000);
    assert_pad(qts, 0x14, true, true);
    qtest_writel(qts, MSTPCRA, 0xeffffdff);
    qtest_clock_step(qts, 18750);
    assert_pad(qts, 0x14, true, false);
    qtest_quit(qts);
}

static void test_pad_migration(void)
{
    QTestState *src = mtu_init();
    QTestState *dst;
    int64_t now;
    g_autofree char *dir = g_dir_make_tmp("rx113-mtu-pad-XXXXXX", NULL);
    g_autofree char *file = g_strdup_printf("%s/state", dir);
    g_autofree char *uri = g_strdup_printf("file:%s", file);

    g_assert_nonnull(dir);
    select_pad(src, 0xb3, 1);
    qtest_writeb(src, BASE + tcr[0], 0x21);
    qtest_writeb(src, BASE + mode[0], 2);
    qtest_writeb(src, BASE + tior[0], 0x21);
    qtest_writew(src, BASE + tgr[0][0], 99);
    qtest_writew(src, BASE + tgr[0][1], 24);
    start_channel(src, 0, true);
    now = qtest_clock_step(src, 5062);
    assert_pad(src, 0xb3, true, true);
    qtest_qmp_assert_success(src, "{'execute':'migrate',"
                            "'arguments':{'uri':%s}}", uri);
    migration_wait(src);
    dst = qtest_init("-M rx113-r5f51138 -incoming defer");
    qtest_clock_set(dst, now);
    qtest_qmp_assert_success(dst, "{'execute':'migrate-incoming',"
                            "'arguments':{'uri':%s}}", uri);
    migration_wait(dst);
    assert_pad(dst, 0xb3, true, true);
    qtest_clock_step(dst, 7437);
    assert_pad(dst, 0xb3, true, true);
    qtest_clock_step(dst, 1);
    assert_pad(dst, 0xb3, true, false);
    qtest_quit(dst);
    qtest_quit(src);
    unlink(file);
    rmdir(dir);
}

static void test_pad_zero_period(void)
{
    QTestState *qts = mtu_init();

    select_pad(qts, 0x14, 3);
    qtest_writeb(qts, BASE + tcr[0], 0x20); /* PCLK/1, zero period */
    qtest_writew(qts, BASE + tgr[0][0], 0);
    qtest_writeb(qts, BASE + tior[0], 5); /* initial high, clear on match */
    start_channel(qts, 0, true);
    qtest_clock_step(qts, 31);
    assert_pad(qts, 0x14, true, true);
    qtest_clock_step(qts, 1);
    assert_pad(qts, 0x14, true, false);
    qtest_clock_step(qts, 1000000000);
    assert_pad(qts, 0x14, true, false);
    start_channel(qts, 0, false);
    qtest_writeb(qts, BASE + tcr[0], 0x20);
    qtest_writeb(qts, BASE + tior[0], 3);
    start_channel(qts, 0, true);
    /* Four PCLK/1 matches per 125 ns; odd and even toggles are observable. */
    qtest_clock_step(qts, 32);
    assert_pad(qts, 0x14, true, true);
    qtest_clock_step(qts, 31);
    assert_pad(qts, 0x14, true, false);
    qtest_clock_step(qts, 31);
    assert_pad(qts, 0x14, true, true);
    qtest_clock_step(qts, 31);
    assert_pad(qts, 0x14, true, false);
    qtest_quit(qts);
}

static void test_pad_mux(void)
{
    static const struct {
        unsigned pad, psel, ch, gr;
    } routes[] = {
        { 0x04, 1, 0, 0 }, { 0x13, 1, 0, 1 },
        { 0xb0, 2, 0, 2 }, { 0xa3, 1, 0, 3 },
        { 0x56, 3, 1, 0 }, { 0xa3, 3, 1, 1 },
        { 0xe0, 2, 2, 0 }, { 0xa4, 3, 2, 1 },
        { 0xe4, 3, 3, 0 }, { 0xb3, 3, 3, 1 },
        { 0x123, 1, 3, 2 }, { 0x16, 2, 3, 3 },
        { 0xb3, 2, 4, 0 }, { 0xd1, 1, 4, 1 },
        { 0xe1, 1, 4, 2 }, { 0xd2, 1, 4, 3 },
    };
    QTestState *qts = mtu_init();

    qtest_writeb(qts, BASE + 0xa, 0xff);
    for (unsigned i = 0; i < G_N_ELEMENTS(routes); i++) {
        unsigned pad = routes[i].pad;
        unsigned port = pad >> 4, bit = pad & 7;

        set_io(qts, routes[i].ch, routes[i].gr, 6);
        select_pad(qts, pad, routes[i].psel);
        assert_pad(qts, pad, true, true);
        /* GPIO latch/direction cannot change a selected MTIOC output. */
        qtest_writeb(qts, PORT_BASE + 0x20 + port, 0);
        qtest_writeb(qts, PORT_BASE + port, 0);
        assert_pad(qts, pad, true, true);
        qtest_writeb(qts, PORT_BASE + 0x60 + port, 0);
        assert_pad(qts, pad, false, false);
        set_io(qts, routes[i].ch, routes[i].gr, 0);
        /* An invalid PSEL never selects the MTU signal. */
        select_pad(qts, pad, 0);
        assert_pad(qts, pad, false, false);
        qtest_writeb(qts, PORT_BASE + 0x60 + port, 0);
        qtest_writeb(qts, PORT_BASE + port, 1 << bit);
        qtest_writeb(qts, PORT_BASE + 0x20 + port, 1 << bit);
        assert_pad(qts, pad, true, true);
        qtest_writeb(qts, PORT_BASE + port, 0);
        qtest_writeb(qts, PORT_BASE + 0x20 + port, 0);
    }
    qtest_quit(qts);
}

static void test_pad_package64(void)
{
    QTestState *qts = qtest_init("-M rx113-r5f51138 "
                               "-global rx113-port.package-pins=64");

    qtest_writew(qts, 0x803fe, 0xa503);
    qtest_writel(qts, MSTPCRA, 0xeffffdff);
    qtest_writeb(qts, BASE + 0xa, 0xff);
    set_io(qts, 3, 1, 6);
    select_pad(qts, 0xb7, 1);
    assert_pad(qts, 0xb7, true, true);
    /* PSRA switches GPIO names; the peripheral selection remains PB7. */
    qtest_writeb(qts, PORT_BASE + 0x121, 0x80);
    qtest_writeb(qts, PORT_BASE + 12, 2);
    g_assert_cmpuint(pad_property(qts, "driving", 0xb7), ==, 1);
    g_assert_cmpuint(pad_property(qts, "levels", 0xb7), ==, 1);
    g_assert_cmphex(qtest_readb(qts, PORT_BASE + 0x40 + 12) & 2, ==, 2);
    g_assert_cmphex(qtest_readb(qts, PORT_BASE + 0x40 + 11) & 0x80, ==, 0);
    /* Unbonded P04 cannot become an MTIOC0A output in the 64-pin package. */
    set_io(qts, 0, 0, 6);
    select_pad(qts, 0x04, 1);
    assert_pad(qts, 0x04, false, false);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/rx113-mtu/pad-package64", test_pad_package64);
    qtest_add_func("/rx113-mtu/pad-mux", test_pad_mux);
    qtest_add_func("/rx113-mtu/pad-zero-period", test_pad_zero_period);
    qtest_add_func("/rx113-mtu/pad-migration", test_pad_migration);
    qtest_add_func("/rx113-mtu/pad-sync-buffers", test_pad_sync_and_buffers);
    qtest_add_func("/rx113-mtu/pad-normal", test_pad_normal);
    qtest_add_func("/rx113-mtu/pad-pwm1", test_pad_pwm1);
    qtest_add_func("/rx113-mtu/pad-pwm2", test_pad_pwm2);
    qtest_add_func("/rx113-mtu/pad-output-control", test_pad_output_control);
    qtest_add_func("/rx113-mtu/compare", test_compare);
    qtest_add_func("/rx113-mtu/clear", test_clear);
    qtest_add_func("/rx113-mtu/overflow", test_overflow);
    qtest_add_func("/rx113-mtu/pwm", test_pwm);
    qtest_add_func("/rx113-mtu/buffer-sync", test_buffer_sync);
    qtest_add_func("/rx113-mtu/clock-stop", test_clock_stop);
    qtest_add_func("/rx113-mtu/underflow", test_underflow);
    qtest_add_func("/rx113-mtu/cascade", test_cascade);
    qtest_add_func("/rx113-mtu/access-reset", test_access_reset);
    qtest_add_func("/rx113-mtu/prescalers", test_prescalers);
    qtest_add_func("/rx113-mtu/zero-period", test_zero_period);
    qtest_add_func("/rx113-mtu/zero-buffer", test_zero_buffer);
    qtest_add_func("/rx113-mtu/mask-precedence",
                   test_match_mask_and_precedence);
    qtest_add_func("/rx113-mtu/mtu5-stopped", test_mtu5_stopped);
    qtest_add_func("/rx113-mtu/buffers", test_buffers);
    qtest_add_func("/rx113-mtu/phase-modes", test_phase_modes);
    qtest_add_func("/rx113-mtu/external-clock", test_external_clock);
    qtest_add_func("/rx113-mtu/sync-precedence", test_sync_precedence);
    qtest_add_func("/rx113-mtu/migration-continuous", test_migrate_continuous);
    qtest_add_data_func("/rx113-mtu/migration-running",
                        GINT_TO_POINTER(0), test_migration);
    qtest_add_data_func("/rx113-mtu/migration-stopped",
                        GINT_TO_POINTER(1), test_migration);
    return g_test_run();
}
