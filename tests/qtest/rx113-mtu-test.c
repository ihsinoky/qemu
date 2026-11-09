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

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
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
