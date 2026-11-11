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

/* Figures 20.35/36: CST3 and the MTU3 clock drive both channels. */
static void reset_pwm_setup(QTestState *qts, unsigned flags)
{
    for (unsigned ch = 3; ch <= 4; ch++) {
        for (unsigned gr = 0; gr < 4; gr++) {
            select_pad(qts, output_pad[ch][gr], 1);
        }
    }
    qtest_writeb(qts, BASE + tcr[3], 0x21); /* /4, cycle clear */
    qtest_writeb(qts, BASE + tcr[4], 0x25); /* ignored /1024, clear A */
    qtest_writew(qts, BASE + tgr[3][0], 9);
    qtest_writew(qts, BASE + tgr[3][1], 6);
    qtest_writew(qts, BASE + tgr[4][0], 4);
    qtest_writew(qts, BASE + tgr[4][1], 2);
    qtest_writeb(qts, BASE + 0xe, 0x43); /* PSYE, active high */
    qtest_writeb(qts, BASE + 0xa, 0x3f);
    qtest_writeb(qts, BASE + mode[3], 8 | flags);
}

static void assert_reset_pwm(QTestState *qts, unsigned phases, bool cycle)
{
    static const unsigned p[] = { 0x22, 0x24, 0x30 };
    static const unsigned n[] = { 0x23, 0x25, 0x31 };

    for (unsigned i = 0; i < 3; i++) {
        bool level = phases & (1 << i);

        assert_pad(qts, p[i], true, level);
        assert_pad(qts, n[i], true, !level);
    }
    assert_pad(qts, 0x17, true, cycle);
    assert_pad(qts, 0x16, false, false); /* MTIOC3C is not a PWM output. */
}

static void test_reset_pwm_waveform(void)
{
    QTestState *qts = mtu_init();

    reset_pwm_setup(qts, 0);
    qtest_writeb(qts, BASE + tier[3], 3);
    qtest_writeb(qts, BASE + tier[4], 3);
    assert_reset_pwm(qts, 0, true);
    /* CST4 alone cannot start either counter. */
    qtest_writeb(qts, START, 0x80);
    qtest_clock_step(qts, 1000);
    g_assert_cmpuint(qtest_readw(qts, BASE + count[3]), ==, 0);
    g_assert_cmpuint(qtest_readw(qts, BASE + count[4]), ==, 0);
    qtest_writeb(qts, START, 0x40);
    for (unsigned period = 0; period < 3; period++) {
        bool cycle = !(period & 1);

        qtest_clock_step(qts, 374);
        assert_reset_pwm(qts, 0, cycle);
        g_assert_cmpuint(qtest_readb(qts, IR(135)), ==, 0);
        qtest_clock_step(qts, 1);
        assert_reset_pwm(qts, 4, cycle);
        g_assert_cmpuint(qtest_readb(qts, IR(135)), ==, 1);
        qtest_clock_step(qts, 249);
        assert_reset_pwm(qts, 4, cycle);
        qtest_clock_step(qts, 1);
        assert_reset_pwm(qts, 6, cycle);
        g_assert_cmpuint(qtest_readb(qts, IR(134)), ==, 1);
        qtest_clock_step(qts, 249);
        assert_reset_pwm(qts, 6, cycle);
        qtest_clock_step(qts, 1);
        assert_reset_pwm(qts, 7, cycle);
        g_assert_cmpuint(qtest_readb(qts, IR(130)), ==, 1);
        qtest_clock_step(qts, 374);
        assert_reset_pwm(qts, 7, cycle);
        qtest_clock_step(qts, 1);
        assert_reset_pwm(qts, 0, !cycle);
        g_assert_cmpuint(qtest_readb(qts, IR(129)), ==, 1);
        g_assert_cmpuint(qtest_readw(qts, BASE + count[3]), ==, 0);
        g_assert_cmpuint(qtest_readw(qts, BASE + count[4]), ==, 0);
        for (unsigned v = 129; v <= 138; v++) {
            qtest_writeb(qts, IR(v), 0);
        }
    }
    /* Stop returns the reset PWM outputs to their initial levels. */
    qtest_clock_step(qts, 875);
    assert_reset_pwm(qts, 7, false);
    qtest_writeb(qts, START, 0);
    assert_reset_pwm(qts, 0, true);
    qtest_clock_step(qts, 5000);
    g_assert_cmpuint(qtest_readw(qts, BASE + count[3]), ==, 7);
    g_assert_cmpuint(qtest_readw(qts, BASE + count[4]), ==, 7);
    qtest_quit(qts);
}

static void test_reset_pwm_buffers(void)
{
    QTestState *qts = mtu_init();

    reset_pwm_setup(qts, 0x30);
    qtest_writeb(qts, BASE + tier[3], 0x1f);
    qtest_writeb(qts, BASE + tier[4], 0x1f);
    /* Figure 20.118: MTU4.TMDR stays zero; MTU3 enables all four buffers. */
    qtest_writew(qts, BASE + tgr[3][2], 7);
    qtest_writew(qts, BASE + tgr[3][3], 3);
    qtest_writew(qts, BASE + tgr[4][2], 1);
    qtest_writew(qts, BASE + tgr[4][3], 5);
    qtest_writeb(qts, START, 0x40);
    qtest_clock_step(qts, 1249);
    assert_reset_pwm(qts, 7, true);
    g_assert_cmpuint(qtest_readw(qts, BASE + tgr[3][0]), ==, 9);
    g_assert_cmpuint(qtest_readw(qts, BASE + tgr[3][1]), ==, 6);
    g_assert_cmpuint(qtest_readw(qts, BASE + tgr[4][0]), ==, 4);
    g_assert_cmpuint(qtest_readw(qts, BASE + tgr[4][1]), ==, 2);
    /* Buffer matches must not issue TGIC/TGID, including on MTU4. */
    g_assert_cmpuint(qtest_readb(qts, IR(131)), ==, 0);
    g_assert_cmpuint(qtest_readb(qts, IR(132)), ==, 0);
    g_assert_cmpuint(qtest_readb(qts, IR(136)), ==, 0);
    g_assert_cmpuint(qtest_readb(qts, IR(137)), ==, 0);
    qtest_clock_step(qts, 1);
    assert_reset_pwm(qts, 0, false);
    g_assert_cmpuint(qtest_readw(qts, BASE + tgr[3][0]), ==, 7);
    g_assert_cmpuint(qtest_readw(qts, BASE + tgr[3][1]), ==, 3);
    g_assert_cmpuint(qtest_readw(qts, BASE + tgr[4][0]), ==, 1);
    g_assert_cmpuint(qtest_readw(qts, BASE + tgr[4][1]), ==, 5);
    for (unsigned period = 0; period < 3; period++) {
        bool cycle = period & 1;

        qtest_clock_step(qts, 249);
        assert_reset_pwm(qts, 0, cycle);
        qtest_clock_step(qts, 1);
        assert_reset_pwm(qts, 2, cycle);
        qtest_clock_step(qts, 250);
        assert_reset_pwm(qts, 3, cycle);
        qtest_clock_step(qts, 250);
        assert_reset_pwm(qts, 7, cycle);
        qtest_clock_step(qts, 250);
        assert_reset_pwm(qts, 0, !cycle);
    }
    qtest_quit(qts);
}

static void test_reset_pwm_polarity(void)
{
    QTestState *qts = mtu_init();

    reset_pwm_setup(qts, 0);
    /* Select per-phase polarity; N bits have no effect without dead time. */
    qtest_writeb(qts, BASE + 0xe, 0x44);
    qtest_writeb(qts, BASE + 0xf, 0x51); /* BF=01, P1=P3=1, P2=0 */
    qtest_writeb(qts, BASE + 0x36, 0x2e); /* invert all P bits */
    assert_reset_pwm(qts, 2, true);
    qtest_writeb(qts, START, 0x40);
    qtest_clock_step(qts, 1249);
    g_assert_cmphex(qtest_readb(qts, BASE + 0xf), ==, 0x51);
    assert_reset_pwm(qts, 5, true);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readb(qts, BASE + 0xf), ==, 0x6e);
    assert_reset_pwm(qts, 5, false);
    /* Output inhibition does not inhibit the timer or its waveform latch. */
    qtest_writeb(qts, BASE + 0xa, 0);
    assert_pad(qts, 0x22, false, false);
    qtest_clock_step(qts, 875);
    qtest_writeb(qts, BASE + 0xa, 0x3f);
    assert_reset_pwm(qts, 2, false);
    qtest_writeb(qts, BASE + 0xe, 4); /* PSYE off */
    assert_pad(qts, 0x17, false, false);
    qtest_quit(qts);
}

static void test_reset_pwm_coincident(void)
{
    QTestState *qts = mtu_init();

    reset_pwm_setup(qts, 0);
    for (unsigned ch = 3; ch <= 4; ch++) {
        qtest_writeb(qts, BASE + tier[ch], 0x1f);
        qtest_writew(qts, BASE + tgr[ch][0], 0xffff);
        qtest_writew(qts, BASE + tgr[ch][1], 0xffff);
        qtest_writew(qts, BASE + count[ch], 0xffff);
    }
    qtest_writeb(qts, START, 0x40);
    /* Cycle=duty toggles once; a cycle clear suppresses both overflow IRQs. */
    for (unsigned period = 0; period < 3; period++) {
        qtest_clock_step(qts, period ? 65536 * 125 : 125);
        assert_reset_pwm(qts, period & 1 ? 0 : 7, period & 1);
        g_assert_cmpuint(qtest_readw(qts, BASE + count[3]), ==, 0);
        g_assert_cmpuint(qtest_readw(qts, BASE + count[4]), ==, 0);
        g_assert_cmpuint(qtest_readb(qts, IR(133)), ==, 0);
        g_assert_cmpuint(qtest_readb(qts, IR(138)), ==, 0);
    }
    qtest_quit(qts);
}

static void test_reset_pwm_clock(void)
{
    QTestState *qts = mtu_init();

    reset_pwm_setup(qts, 0);
    qtest_writeb(qts, START, 0x40);
    qtest_clock_step(qts, 437); /* 3 counts + 62 ns, W phase active */
    assert_reset_pwm(qts, 4, true);
    /* MTU4's stored normal-mode TMDR does not permit ELC restart here. */
    qtest_set_irq_in(qts, MTU_PATH, "elc", 10, 1);
    qtest_set_irq_in(qts, MTU_PATH, "elc", 10, 0);
    g_assert_cmpuint(qtest_readw(qts, BASE + count[4]), ==, 3);
    qtest_writel(qts, MSTPCRA, 0xffffffff);
    qtest_clock_step(qts, 1000000);
    assert_reset_pwm(qts, 4, true);
    qtest_writel(qts, MSTPCRA, 0xeffffdff);
    qtest_clock_step(qts, 187);
    assert_reset_pwm(qts, 4, true);
    qtest_clock_step(qts, 1);
    assert_reset_pwm(qts, 6, true);
    qtest_writeb(qts, START, 0);
    qtest_writew(qts, BASE + count[3], 0);
    qtest_writew(qts, BASE + count[4], 0);
    qtest_writeb(qts, BASE + tcr[3], 0x26); /* MTCLKA rising, clear A */
    qtest_writeb(qts, START, 0x40);
    qtest_clock_step(qts, 1000000);
    g_assert_cmpuint(qtest_readw(qts, BASE + count[3]), ==, 0);
    g_assert_cmpuint(qtest_readw(qts, BASE + count[4]), ==, 0);
    for (unsigned edge = 0; edge < 10; edge++) {
        qtest_set_irq_in(qts, MTU_PATH, "mtclk", 0, 1);
        g_assert_cmpuint(qtest_readw(qts, BASE + count[3]), ==,
                         (edge + 1) % 10);
        g_assert_cmpuint(qtest_readw(qts, BASE + count[4]), ==,
                         (edge + 1) % 10);
        qtest_set_irq_in(qts, MTU_PATH, "mtclk", 0, 0);
    }
    assert_reset_pwm(qts, 0, false);
    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    qtest_writew(qts, 0x803fe, 0xa503);
    qtest_writel(qts, MSTPCRA, 0xeffffdff);
    assert_pad(qts, 0x22, false, false);
    g_assert_cmphex(qtest_readb(qts, BASE + mode[3]), ==, 0);
    g_assert_cmphex(qtest_readb(qts, BASE + 0xf), ==, 0);
    qtest_quit(qts);
}

static void test_reset_pwm_zero(void)
{
    static const unsigned vectors[] = { 129, 130, 134, 135 };
    QTestState *qts = mtu_init();

    reset_pwm_setup(qts, 0);
    qtest_writeb(qts, BASE + tcr[3], 0x20); /* PCLK/1, zero cycle */
    qtest_writeb(qts, BASE + tcr[4], 5); /* no local clear; clock is ignored */
    qtest_writeb(qts, BASE + tier[3], 3);
    qtest_writeb(qts, BASE + tier[4], 3);
    for (unsigned ch = 3; ch <= 4; ch++) {
        for (unsigned gr = 0; gr < 2; gr++) {
            qtest_writew(qts, BASE + tgr[ch][gr], 0);
        }
    }
    qtest_writeb(qts, START, 0x40);
    qtest_clock_step(qts, 31);
    assert_reset_pwm(qts, 0, true);
    qtest_clock_step(qts, 1);
    assert_reset_pwm(qts, 7, false);
    for (unsigned i = 0; i < G_N_ELEMENTS(vectors); i++) {
        g_assert_cmpuint(qtest_readb(qts, IR(vectors[i])), ==, 1);
        qtest_writeb(qts, IR(vectors[i]), 0);
    }
    qtest_clock_step(qts, 31);
    assert_reset_pwm(qts, 0, true);
    qtest_clock_step(qts, 125);
    assert_reset_pwm(qts, 0, true);
    for (unsigned i = 0; i < G_N_ELEMENTS(vectors); i++) {
        /* Section 20.6.26 applies to both channels of the coupled pair. */
        g_assert_cmpuint(qtest_readb(qts, IR(vectors[i])), ==, 0);
    }
    qtest_quit(qts);
}

static void test_reset_pwm_migration(void)
{
    QTestState *src = mtu_init();
    QTestState *dst;
    int64_t now;
    g_autofree char *dir = g_dir_make_tmp("rx113-mtu-reset-XXXXXX", NULL);
    g_autofree char *file = g_strdup_printf("%s/state", dir);
    g_autofree char *uri = g_strdup_printf("file:%s", file);

    g_assert_nonnull(dir);
    reset_pwm_setup(src, 0x30);
    qtest_writew(src, BASE + tgr[3][2], 7);
    qtest_writew(src, BASE + tgr[3][3], 3);
    qtest_writew(src, BASE + tgr[4][2], 1);
    qtest_writew(src, BASE + tgr[4][3], 5);
    qtest_writeb(src, START, 0x40);
    now = qtest_clock_step(src, 937); /* after all three duty matches */
    assert_reset_pwm(src, 7, true);
    qtest_qmp_assert_success(src, "{'execute':'migrate',"
                            "'arguments':{'uri':%s}}", uri);
    migration_wait(src);
    dst = qtest_init("-M rx113-r5f51138 -incoming defer");
    qtest_clock_set(dst, now);
    qtest_qmp_assert_success(dst, "{'execute':'migrate-incoming',"
                            "'arguments':{'uri':%s}}", uri);
    migration_wait(dst);
    assert_reset_pwm(dst, 7, true);
    qtest_clock_step(dst, 312);
    assert_reset_pwm(dst, 7, true);
    qtest_clock_step(dst, 1);
    assert_reset_pwm(dst, 0, false);
    g_assert_cmpuint(qtest_readw(dst, BASE + count[3]), ==, 0);
    g_assert_cmpuint(qtest_readw(dst, BASE + count[4]), ==, 0);
    g_assert_cmpuint(qtest_readw(dst, BASE + tgr[3][0]), ==, 7);
    qtest_clock_step(dst, 249);
    assert_reset_pwm(dst, 0, false);
    qtest_clock_step(dst, 1);
    assert_reset_pwm(dst, 2, false);
    qtest_quit(dst);
    qtest_quit(src);
    unlink(file);
    rmdir(dir);
}

/* Section 20.3.8: half-cycle 12, dead time 2, three distinct duties. */
static void complementary_setup(QTestState *qts, unsigned md)
{
    qtest_writeb(qts, BASE + tcr[3], 1);
    qtest_writeb(qts, BASE + tcr[4], 1);
    qtest_writew(qts, BASE + 0x16, 2);
    qtest_writew(qts, BASE + 0x22, 12);
    qtest_writew(qts, BASE + tgr[3][2], 14);
    qtest_writew(qts, BASE + tgr[3][3], 4);
    qtest_writew(qts, BASE + tgr[4][2], 6);
    qtest_writew(qts, BASE + tgr[4][3], 8);
    qtest_writeb(qts, BASE + mode[3], 0x30 | md);
}

static void measurement_pin(QTestState *qts, unsigned name, bool level)
{
    qtest_set_irq_in(qts, "/machine/mcu/port", "pin",
                     (name >> 4) * 8 + (name & 7), level);
}

/* 20.3.10: all six pad alternatives, levels and clock divisors. */
static void test_mtu5_measurement(void)
{
    static const unsigned pads[2][3] = {
        { 0xa4, 0xa6, 0xb0 }, { 0x11, 0x10, 0x56 },
    };
    static const unsigned divisors[] = { 1, 4, 16, 64 };
    QTestState *qts = mtu_init();

    for (unsigned route = 0; route < 2; route++) {
        for (unsigned div = 0; div < 4; div++) {
            for (unsigned io = 0x19; io <= 0x1f; io++) {
                if (io == 0x1c) {
                    continue;
                }
                qtest_writeb(qts, BASE + 0x2b4, 0);
                for (unsigned n = 5; n < 8; n++) {
                    select_pad(qts, pads[route][n - 5], 1);
                    measurement_pin(qts, pads[route][n - 5], !(io & 4));
                    qtest_writeb(qts, BASE + tcr[n], div);
                    qtest_writeb(qts, BASE + tcr[n] + 2, io);
                    qtest_writew(qts, BASE + count[n], 0xfffc);
                    qtest_writew(qts, BASE + tgr[n][0], 0xfffd);
                }
                qtest_writeb(qts, BASE + 0x2b4, 7);
                qtest_clock_step(qts, 4000);
                for (unsigned n = 5; n < 8; n++) {
                    g_assert_cmphex(qtest_readw(qts, BASE + count[n]),
                                    ==, 0xfffc);
                    measurement_pin(qts, pads[route][n - 5], !!(io & 4));
                }
                qtest_clock_step(qts, 5 * divisors[div] * 125);
                for (unsigned n = 5; n < 8; n++) {
                    g_assert_cmphex(qtest_readw(qts, BASE + count[n]),
                                    ==, (uint16_t)(0xfffc + 20));
                    g_assert_cmphex(qtest_readw(qts, BASE + tgr[n][0]),
                                    ==, 0xfffd);
                    g_assert_cmpuint(qtest_readb(qts, IR(vector[n])), ==, 0);
                    select_pad(qts, pads[route][n - 5], 0);
                }
            }
        }
    }
    qtest_quit(qts);
}

/* 20.3.11: raw crest/trough capture, clear and request enable isolation. */
static void test_mtu5_compensation(const void *opaque)
{
    unsigned setting = GPOINTER_TO_UINT(opaque);
    QTestState *qts = mtu_init();

    complementary_setup(qts, setting & 15);
    for (unsigned n = 5; n < 8; n++) {
        static const unsigned pads[] = { 0xa4, 0xa6, 0xb0 };
        static const unsigned io[] = { 0x1e, 0x19, 0x1f };

        select_pad(qts, pads[n - 5], 1);
        measurement_pin(qts, pads[n - 5], (n != 6) ^ !!(setting & 16));
        qtest_writeb(qts, BASE + tcr[n], 1);
        qtest_writeb(qts, BASE + tcr[n] + 2,
                     io[n - 5] ^ ((setting & 16) ? 4 : 0));
    }
    qtest_writeb(qts, BASE + 0x30, 0x99); /* Skip first crest and trough. */
    qtest_writeb(qts, BASE + tier[3], 1);
    qtest_writeb(qts, BASE + tier[4], 0x10);
    qtest_writeb(qts, BASE + 0x2b2, 5); /* U and W only. */
    qtest_writeb(qts, BASE + 0x2b4, 7);
    qtest_writeb(qts, START, 0xc0);
    qtest_clock_step(qts, 13 * 125 - 1);
    g_assert_cmphex(qtest_readw(qts, BASE + tgr[5][0]), ==, 0xffff);
    qtest_clock_step(qts, 1);
    g_assert_cmpuint(qtest_readw(qts, BASE + tgr[5][0]), ==, 12);
    g_assert_cmphex(qtest_readw(qts, BASE + tgr[6][0]), ==, 0xffff);
    g_assert_cmpuint(qtest_readw(qts, BASE + tgr[7][0]), ==, 12);
    g_assert_cmpuint(qtest_readb(qts, IR(129)), ==, 0);
    g_assert_cmpuint(qtest_readb(qts, IR(139)), ==, 1);
    g_assert_cmpuint(qtest_readb(qts, IR(140)), ==, 0);
    g_assert_cmpuint(qtest_readb(qts, IR(141)), ==, 1);
    qtest_writeb(qts, IR(139), 0);
    qtest_writeb(qts, IR(141), 0);
    qtest_clock_step(qts, 12 * 125);
    g_assert_cmpuint(qtest_readw(qts, BASE + tgr[5][0]), ==, 12);
    g_assert_cmpuint(qtest_readw(qts, BASE + tgr[6][0]), ==, 24);
    g_assert_cmpuint(qtest_readw(qts, BASE + tgr[7][0]), ==, 24);
    g_assert_cmpuint(qtest_readb(qts, IR(138)), ==, 0);
    g_assert_cmpuint(qtest_readb(qts, IR(139)), ==, 0);
    g_assert_cmpuint(qtest_readb(qts, IR(140)), ==, 0);
    g_assert_cmpuint(qtest_readb(qts, IR(141)), ==, 1);
    qtest_writeb(qts, BASE + 0x2b6, 7);
    qtest_clock_step(qts, 12 * 125);
    g_assert_cmpuint(qtest_readw(qts, BASE + tgr[5][0]), ==, 36);
    g_assert_cmpuint(qtest_readw(qts, BASE + count[5]), ==, 0);
    g_assert_cmpuint(qtest_readw(qts, BASE + count[6]), ==, 37);
    g_assert_cmpuint(qtest_readw(qts, BASE + count[7]), ==, 0);
    g_assert_cmpuint(qtest_readb(qts, IR(129)), ==, 1);
    qtest_clock_step(qts, 12 * 125);
    g_assert_cmpuint(qtest_readw(qts, BASE + tgr[6][0]), ==, 48);
    g_assert_cmpuint(qtest_readw(qts, BASE + count[6]), ==, 0);
    g_assert_cmpuint(qtest_readw(qts, BASE + tgr[7][0]), ==, 11);
    g_assert_cmpuint(qtest_readw(qts, BASE + count[7]), ==, 0);
    g_assert_cmpuint(qtest_readb(qts, IR(138)), ==, 1);
    qtest_quit(qts);
}

/* Independent prescalers must sample all elapsed clocks before capture. */
static void test_mtu5_capture_clocks(void)
{
    static const unsigned div[] = { 1, 4, 16, 64 };

    for (unsigned d = 0; d < 4; d++) {
        QTestState *qts = mtu_init();

        complementary_setup(qts, 15);
        select_pad(qts, 0xa4, 1);
        measurement_pin(qts, 0xa4, true);
        qtest_writeb(qts, BASE + tcr[5], d);
        qtest_writeb(qts, BASE + 0x286, 0x1f);
        qtest_writeb(qts, BASE + 0x2b4, 4);
        qtest_writeb(qts, START, 0xc0);
        qtest_clock_step(qts, 13 * 125);
        g_assert_cmpuint(qtest_readw(qts, BASE + tgr[5][0]),
                         ==, (13 * 4 - 1) / div[d]);
        qtest_clock_step(qts, 12 * 125);
        g_assert_cmpuint(qtest_readw(qts, BASE + tgr[5][0]),
                         ==, (25 * 4 - 1) / div[d]);
        qtest_quit(qts);
    }
}

/* PSEL/PMR/ASEL and package bonding qualify the measurement inputs. */
static void test_mtu5_measurement_routes(void)
{
    QTestState *qts = qtest_init("-M rx113-r5f51138 "
                                 "-global rx113-port.package-pins=64");

    qtest_writew(qts, 0x803fe, 0xa503);
    qtest_writeb(qts, 0x80036, 0);
    qtest_clock_step(qts, 34500);
    qtest_writew(qts, 0x80026, 0x100);
    qtest_writel(qts, SCKCR, 0);
    qtest_writel(qts, MSTPCRA, 0xeffffdff);

    qtest_writeb(qts, BASE + 0x286, 0x1f);
    qtest_writeb(qts, BASE + tcr[5], 1);
    qtest_writeb(qts, BASE + 0x2b4, 4);
    measurement_pin(qts, 0xa4, true);
    qtest_clock_step(qts, 125);
    g_assert_cmpuint(qtest_readw(qts, BASE + count[5]), ==, 0);
    select_pad(qts, 0xa4, 2);
    qtest_clock_step(qts, 125);
    g_assert_cmpuint(qtest_readw(qts, BASE + count[5]), ==, 0);
    select_pad(qts, 0xa4, 1);
    qtest_clock_step(qts, 125);
    g_assert_cmpuint(qtest_readw(qts, BASE + count[5]), ==, 1);
    select_pad(qts, 0xa4, 0x81);
    qtest_clock_step(qts, 125);
    g_assert_cmpuint(qtest_readw(qts, BASE + count[5]), ==, 1);
    select_pad(qts, 0xa4, 1);
    qtest_writeb(qts, PORT_BASE + 0x6a, 0);
    qtest_clock_step(qts, 125);
    g_assert_cmpuint(qtest_readw(qts, BASE + count[5]), ==, 1);
    select_pad(qts, 0xa4, 0);
    select_pad(qts, 0x11, 1); /* Unbonded on the selected 64-pin part. */
    measurement_pin(qts, 0x11, true);
    pin(qts, 0, true); /* MTCLKA is a different signal on PA4. */
    qtest_clock_step(qts, 125);
    g_assert_cmpuint(qtest_readw(qts, BASE + count[5]), ==, 1);
    select_pad(qts, 0xa4, 1);
    qtest_clock_step(qts, 62);
    qtest_writel(qts, MSTPCRA, 0xffffffff);
    qtest_clock_step(qts, 1000);
    qtest_writel(qts, MSTPCRA, 0xeffffdff);
    qtest_clock_step(qts, 62);
    g_assert_cmpuint(qtest_readw(qts, BASE + count[5]), ==, 1);
    qtest_clock_step(qts, 1);
    g_assert_cmpuint(qtest_readw(qts, BASE + count[5]), ==, 2);
    qtest_writeb(qts, BASE + 0x2b4, 0);
    qtest_clock_step(qts, 125);
    g_assert_cmpuint(qtest_readw(qts, BASE + count[5]), ==, 2);
    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    qtest_writew(qts, 0x803fe, 0xa503);
    qtest_writel(qts, MSTPCRA, 0xeffffdff);
    g_assert_cmpuint(qtest_readw(qts, BASE + count[5]), ==, 0);
    g_assert_cmphex(qtest_readw(qts, BASE + tgr[5][0]), ==, 0xffff);
    qtest_quit(qts);
}

/* Firmware calculation and a five-register transaction survive migration. */
static void test_mtu5_compensation_migration(void)
{
    QTestState *src = mtu_init(), *dst;
    int64_t now;
    unsigned measured, corrected;
    g_autofree char *dir = g_dir_make_tmp("rx113-mtu-delay-XXXXXX", NULL);
    g_autofree char *file = g_strdup_printf("%s/state", dir);
    g_autofree char *uri = g_strdup_printf("file:%s", file);

    g_assert_nonnull(dir);
    complementary_setup(src, 15);
    select_pad(src, 0xa4, 1);
    measurement_pin(src, 0xa4, true);
    qtest_writeb(src, BASE + tcr[5], 1);
    qtest_writeb(src, BASE + 0x286, 0x1e);
    qtest_writew(src, BASE + count[5], 4);
    qtest_writeb(src, BASE + 0x2b4, 4);
    qtest_writeb(src, START, 0xc0);
    qtest_clock_step(src, 2 * 125);
    measurement_pin(src, 0xa4, false); /* Two-clock inverter delay. */
    qtest_clock_step(src, 11 * 125);
    measured = qtest_readw(src, BASE + tgr[5][0]);
    g_assert_cmpuint(measured, ==, 6);
    corrected = qtest_readw(src, BASE + tgr[3][1]) + (measured - 4);
    qtest_writew(src, BASE + tgr[3][3], corrected);
    qtest_writew(src, BASE + tgr[4][2], corrected + 2);
    qtest_writew(src, BASE + tgr[4][3], corrected + 4); /* Commit in Tb1. */
    g_assert_cmpuint(qtest_readw(src, BASE + tgr[3][1]), ==, 4);
    measurement_pin(src, 0xa4, true);
    now = qtest_clock_step(src, 62);
    qtest_qmp_assert_success(src, "{'execute':'migrate',"
                            "'arguments':{'uri':%s}}", uri);
    migration_wait(src);
    dst = qtest_init("-M rx113-r5f51138 -incoming defer");
    qtest_clock_set(dst, now);
    qtest_qmp_assert_success(dst, "{'execute':'migrate-incoming',"
                            "'arguments':{'uri':%s}}", uri);
    migration_wait(dst);
    g_assert_cmpuint(qtest_readw(dst, BASE + count[5]), ==, 6);
    g_assert_cmpuint(qtest_readw(dst, BASE + tgr[5][0]), ==, 6);
    g_assert_cmpuint(qtest_readb(dst, IR(139)), ==, 0);
    qtest_clock_step(dst, 62);
    g_assert_cmpuint(qtest_readw(dst, BASE + count[5]), ==, 6);
    qtest_clock_step(dst, 1);
    g_assert_cmpuint(qtest_readw(dst, BASE + count[5]), ==, 7);
    qtest_clock_step(dst, 125); /* Old bank consumed, new bank snapshots. */
    g_assert_cmpuint(qtest_readw(dst, BASE + tgr[3][1]), ==, 4);
    qtest_clock_step(dst, 12 * 125); /* Next end-Tb2 consumes correction. */
    g_assert_cmpuint(qtest_readw(dst, BASE + tgr[3][1]), ==, corrected);
    g_assert_cmpuint(qtest_readw(dst, BASE + tgr[4][0]), ==, corrected + 2);
    g_assert_cmpuint(qtest_readw(dst, BASE + tgr[4][1]), ==, corrected + 4);
    qtest_quit(dst);
    qtest_quit(src);
    unlink(file);
    rmdir(dir);
}

static void complementary_pads(QTestState *qts)
{
    for (unsigned ch = 3; ch <= 4; ch++) {
        for (unsigned gr = 0; gr < 4; gr++) {
            select_pad(qts, output_pad[ch][gr], 1);
        }
    }
    qtest_writeb(qts, BASE + 0xe, 0x43); /* Active high, PSYE. */
    qtest_writeb(qts, BASE + 0xa, 0x3f);
}

static void assert_complementary_pads(QTestState *qts, unsigned positive,
                                      unsigned negative, bool cycle)
{
    static const unsigned p[] = { 0x22, 0x24, 0x30 };
    static const unsigned n[] = { 0x23, 0x25, 0x31 };

    for (unsigned i = 0; i < 3; i++) {
        assert_pad(qts, p[i], true, (positive >> i) & 1);
        assert_pad(qts, n[i], true, (negative >> i) & 1);
    }
    assert_pad(qts, 0x17, true, cycle);
    assert_pad(qts, 0x16, false, false);
}

/* Figure 20.41: TDDR=1 retains the carrier offset, but no pad dead time. */
static void test_complementary_no_dead_time(void)
{
    static const unsigned pads[] = { 0x22, 0x24, 0x30, 0x23, 0x25, 0x31 };

    for (unsigned md = 13; md <= 15; md++) {
        QTestState *qts = mtu_init();

        /* A write without the qualifying read cannot clear TDER. */
        qtest_writeb(qts, BASE + 0x34, 0);
        g_assert_cmpuint(qtest_readb(qts, BASE + 0x34), ==, 1);
        qtest_writeb(qts, BASE + 0x34, 0xfe);
        g_assert_cmpuint(qtest_readb(qts, BASE + 0x34), ==, 0);
        qtest_writeb(qts, BASE + 0x34, 1);
        qtest_writeb(qts, BASE + 0x34, 0); /* Prior read saw zero. */
        g_assert_cmpuint(qtest_readb(qts, BASE + 0x34), ==, 1);
        qtest_writeb(qts, BASE + 0x34, 0);
        complementary_setup(qts, md);
        qtest_writeb(qts, BASE + mode[3], 0);
        qtest_writew(qts, BASE + 0x16, 1);
        qtest_writew(qts, BASE + tgr[3][2], 13);
        qtest_writeb(qts, BASE + mode[3], 0x30 | md);
        complementary_pads(qts);
        qtest_writeb(qts, START, 0xc0);
        for (unsigned edge = 0; edge <= 48; edge++) {
            for (unsigned pol = 0; pol < 4; pol++) {
                /* Both N settings must be ignored, globally and per phase. */
                qtest_writeb(qts, BASE + 0xe, pol < 2 ? pol * 3 : 4);
                qtest_writeb(qts, BASE + 0xf, pol == 2 ? 0x11 : 0x3b);
                for (unsigned arm = 0; arm < 6; arm++) {
                    unsigned phase = arm % 3;
                    unsigned duty = 4 + phase * 2;
                    unsigned t = edge % 24;
                    bool on = edge >= 2 && t >= duty + 1 && t < 25 - duty;
                    unsigned polarity = pol < 2 ? pol :
                                        (0x11 >> (2 * phase)) & 1;

                    assert_pad(qts, pads[arm], true,
                               !polarity ^ on ^ (arm >= 3));
                }
            }
            if (edge != 48) {
                qtest_clock_step(qts, 124);
                /* No transition before the count edge. */
                g_assert_cmpuint(qtest_readw(qts, BASE + count[3]), ==,
                                 edge % 24 <= 12 ? 1 + edge % 24 :
                                 25 - edge % 24);
                qtest_clock_step(qts, 1);
            }
        }
        qtest_writeb(qts, START, 0);
        qtest_writeb(qts, BASE + 0xe, 3);
        qtest_writeb(qts, BASE + 0xe, 0x43);
        assert_complementary_pads(qts, 0, 7, true);
        qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
        qtest_writew(qts, 0x803fe, 0xa503);
        qtest_writel(qts, MSTPCRA, 0xeffffdff);
        g_assert_cmpuint(qtest_readb(qts, BASE + 0x34), ==, 1);
        qtest_quit(qts);
    }
}

/* Table 20.39 plus independent carrier timing, observed through PORT/PIDR. */
static void test_tgcr(const void *opaque)
{
    static const unsigned positive[] = { 0, 1, 2, 2, 4, 1, 4, 0 };
    static const unsigned negative[] = { 0, 4, 1, 4, 2, 2, 1, 0 };
    static const unsigned pads[] = { 0x22, 0x24, 0x30, 0x23, 0x25, 0x31 };
    static const unsigned inputs[] = { 0x04, 0x13, 0x32 };
    bool reset = GPOINTER_TO_UINT(opaque);
    QTestState *qts = mtu_init();

    g_assert_cmphex(qtest_readb(qts, BASE + 0xd), ==, 0x80);
    if (reset) {
        reset_pwm_setup(qts, 0);
    } else {
        complementary_setup(qts, 15);
        complementary_pads(qts);
    }
    for (unsigned i = 0; i < 3; i++) {
        select_pad(qts, inputs[i], 1);
    }
    qtest_writeb(qts, START, 0xc0);
    for (unsigned edge = 0; edge <= (reset ? 10 : 48);
         edge += reset ? 2 : 1) {
        for (unsigned polarity = 0; polarity < 3; polarity++) {
            qtest_writeb(qts, BASE + 0xe,
                         polarity == 2 ? 4 : polarity ? 3 : 0);
            qtest_writeb(qts, BASE + 0xf, 0x26);
            for (unsigned source = 0; source < 2; source++) {
                for (unsigned feedback = 0; feedback < 8; feedback++) {
                    for (unsigned i = 0; i < 3; i++) {
                        unsigned name = inputs[i];

                        qtest_set_irq_in(qts, "/machine/mcu/port", "pin",
                                         (name >> 4) * 8 + (name & 7),
                                         (feedback >> i) & 1);
                    }
                    for (unsigned chop = 0; chop < 4; chop++) {
                        unsigned value = 0x40 | (chop << 4) |
                                         (source ? 8 | feedback : 0);

                        qtest_writeb(qts, BASE + 0xd, value);
                        g_assert_cmphex(qtest_readb(qts, BASE + 0xd), ==,
                                        value | 0x80);
                        for (unsigned arm = 0; arm < 6; arm++) {
                            unsigned phase = arm % 3;
                            unsigned duty = reset ? 6 - phase * 2 :
                                                   4 + phase * 2;
                            unsigned t = edge % (reset ? 10 : 24);
                            bool pwm = edge >= 3 && (arm < 3 ?
                                (t >= duty + 1 && t < 25 - duty) :
                                (t >= 27 - duty || t < duty - 1));
                            bool on = ((arm < 3 ? positive[feedback] :
                                        negative[feedback]) >> phase) & 1;
                            unsigned pol = polarity == 2 ?
                                (0x26 >> (2 * phase + (arm >= 3))) & 1 :
                                polarity == 1;

                            if (reset) {
                                pwm = arm < 3 ? t >= duty + 1 : t < duty + 1;
                            }
                            if (chop & (arm < 3 ? 1 : 2)) {
                                on &= pwm;
                            }
                            if (reset && arm >= 3 && polarity == 2) {
                                pol = (0x26 >> (2 * phase)) & 1;
                            }
                            assert_pad(qts, pads[arm], true, !pol ^ on);
                        }
                    }
                }
            }
        }
        /* BDC=0 bypasses all feedback and chopping selections. */
        qtest_writeb(qts, BASE + 0xe, 3);
        qtest_writeb(qts, BASE + 0xd, 0x3f);
        for (unsigned arm = 0; arm < 6; arm++) {
            unsigned duty = reset ? 6 - (arm % 3) * 2 :
                                    4 + (arm % 3) * 2;
            unsigned t = edge % (reset ? 10 : 24);
            bool pwm = edge >= 3 && (arm < 3 ?
                (t >= duty + 1 && t < 25 - duty) :
                (t >= 27 - duty || t < duty - 1));

            if (reset) {
                pwm = arm < 3 ? t >= duty + 1 : t < duty + 1;
            }
            assert_pad(qts, pads[arm], true, pwm);
        }
        if (edge != (reset ? 10 : 48)) {
            qtest_clock_step(qts, reset ? 250 : 125);
        }
    }
    /* Protection covers TGCR reads and writes, with read-before-clear RWE. */
    qtest_writeb(qts, BASE + 0xd, 0x49);
    g_assert_cmpuint(qtest_readb(qts, RWE), ==, 1);
    qtest_writeb(qts, RWE, 0);
    qtest_writeb(qts, BASE + 0xd, 0x7f);
    g_assert_cmphex(qtest_readb(qts, BASE + 0xd), ==, 0);
    qtest_writeb(qts, RWE, 1);
    g_assert_cmphex(qtest_readb(qts, BASE + 0xd), ==, 0xc9);
    /* PSEL, PMR and ASEL qualify external feedback; MTCLK is unrelated. */
    qtest_writeb(qts, BASE + 0xe, 3);
    qtest_writeb(qts, BASE + 0xd, 0x40);
    select_pad(qts, 0x04, 0);
    select_pad(qts, 0x13, 0);
    select_pad(qts, 0x32, 0);
    for (unsigned i = 0; i < 4; i++) {
        pin(qts, i, true);
    }
    for (unsigned i = 0; i < 6; i++) {
        assert_pad(qts, pads[i], true, false);
    }
    select_pad(qts, 0x04, 1);
    assert_pad(qts, 0x22, true, true);
    assert_pad(qts, 0x31, true, true);
    qtest_writeb(qts, PORT_BASE + 0x60, 0); /* PMR disables P04 input. */
    assert_pad(qts, 0x22, true, false);
    select_pad(qts, 0xe3, 3); /* Alternative MTIOC0A, analog-capable pad. */
    qtest_set_irq_in(qts, "/machine/mcu/port", "pin", 14 * 8 + 3, 1);
    assert_pad(qts, 0x22, true, true);
    qtest_writeb(qts, PORT_BASE + 0x140 + 14 * 8 + 3, 0x83);
    assert_pad(qts, 0x22, true, false);
    /* TOER still controls pad ownership for fixed-level gating. */
    qtest_writeb(qts, BASE + 0xd, 0x49);
    qtest_writeb(qts, BASE + 0xa, 0);
    assert_pad(qts, 0x22, false, false);
    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    qtest_writew(qts, 0x803fe, 0xa503);
    qtest_writel(qts, MSTPCRA, 0xeffffdff);
    g_assert_cmphex(qtest_readb(qts, BASE + 0xd), ==, 0x80);
    qtest_quit(qts);
}

static void test_complementary_tgcr_migration(void)
{
    for (unsigned source = 0; source < 2; source++) {
        QTestState *src = mtu_init();
        QTestState *dst;
        int64_t now;
        g_autofree char *dir = g_dir_make_tmp("rx113-mtu-tgcr-XXXXXX", NULL);
        g_autofree char *file = g_strdup_printf("%s/state", dir);
        g_autofree char *uri = g_strdup_printf("file:%s", file);

        g_assert_nonnull(dir);
        complementary_setup(src, 13 + source);
        complementary_pads(src);
        select_pad(src, 0x04, 1);
        qtest_set_irq_in(src, "/machine/mcu/port", "pin", 4, 1);
        qtest_writeb(src, BASE + 0xd, source ? 0x49 : 0x40);
        qtest_writeb(src, START, 0xc0);
        now = qtest_clock_step(src, 6 * 125 + 62);
        qtest_qmp_assert_success(src, "{'execute':'migrate',"
                                "'arguments':{'uri':%s}}", uri);
        migration_wait(src);
        dst = qtest_init("-M rx113-r5f51138 -incoming defer");
        qtest_clock_set(dst, now);
        qtest_qmp_assert_success(dst, "{'execute':'migrate-incoming',"
                                "'arguments':{'uri':%s}}", uri);
        migration_wait(dst);
        g_assert_cmphex(qtest_readb(dst, BASE + 0xd), ==,
                        source ? 0xc9 : 0xc0);
        assert_complementary_pads(dst, 1, 4, true);
        qtest_clock_step(dst, 12 * 125);
        assert_complementary_pads(dst, 1, 4, false);
        /* Switch to chopping after load: carrier state was never gated. */
        qtest_writeb(dst, BASE + 0xd, source ? 0x79 : 0x70);
        assert_complementary_pads(dst, 1, 0, false);
        qtest_set_irq_in(dst, "/machine/mcu/port", "pin", 4, 0);
        if (!source) {
            assert_complementary_pads(dst, 0, 0, false);
        }
        qtest_quit(dst);
        qtest_quit(src);
        unlink(file);
        rmdir(dir);
    }
}

/* Independent steady-carrier oracle, including the initial inactive window. */
static void test_complementary_waveform(void)
{
    for (unsigned duty = 0; duty <= 14; duty++) {
        QTestState *qts = mtu_init();

        complementary_setup(qts, 15);
        qtest_writeb(qts, BASE + mode[3], 0);
        qtest_writew(qts, BASE + tgr[3][3], duty);
        qtest_writew(qts, BASE + tgr[4][2], (duty + 5) % 15);
        qtest_writew(qts, BASE + tgr[4][3], (duty + 10) % 15);
        qtest_writeb(qts, BASE + mode[3], 0x3f);
        complementary_pads(qts);
        qtest_writeb(qts, START, 0xc0);
        for (unsigned edge = 0; edge <= 96; edge++) {
            unsigned t = edge % 24;
            unsigned positive = 0, negative = 0;
            bool cycle = !edge || !(((edge - 1) / 12) & 1);

            for (unsigned i = 0; i < 3; i++) {
                unsigned d = (duty + 5 * i) % 15;
                bool p = d == 0 || (t >= d + 1 && t < 25 - d);
                bool n = d == 14 || (t >= 27 - d || t < d - 1);

                if (edge >= 3) {
                    positive |= (p && d < 12) << i;
                    negative |= (n && d > 2) << i;
                }
            }
            g_test_message("duty %u edge %u", duty, edge);
            assert_complementary_pads(qts, positive, negative, cycle);
            qtest_clock_step(qts, 124);
            assert_complementary_pads(qts, positive, negative, cycle);
            qtest_clock_step(qts, 1);
        }
        qtest_quit(qts);
    }
}

static void assert_complementary_bank(QTestState *qts, unsigned duty,
                                      unsigned half)
{
    g_assert_cmpuint(qtest_readw(qts, BASE + tgr[3][1]), ==, duty);
    g_assert_cmpuint(qtest_readw(qts, BASE + tgr[4][0]), ==, duty + 2);
    g_assert_cmpuint(qtest_readw(qts, BASE + tgr[4][1]), ==, duty + 4);
    g_assert_cmpuint(qtest_readw(qts, BASE + tgr[3][0]), ==, half + 2);
    g_assert_cmpuint(qtest_readw(qts, BASE + 0x14), ==, half);
}

static void complementary_buffers(QTestState *qts, unsigned duty,
                                   unsigned half, bool commit)
{
    qtest_writew(qts, BASE + 0x22, half);
    qtest_writew(qts, BASE + tgr[3][2], half + 2);
    qtest_writew(qts, BASE + tgr[3][3], duty);
    qtest_writew(qts, BASE + tgr[4][2], duty + 2);
    if (commit) {
        qtest_writew(qts, BASE + tgr[4][3], duty + 4);
    }
}

/* Figures 20.47/48: a new turn-off overtakes the old pending turn-on. */
static void test_complementary_arbitration(void)
{
    for (unsigned trough = 0; trough < 2; trough++) {
        for (unsigned both = 0; both < 2; both++) {
            QTestState *qts = mtu_init();
            unsigned md = both ? 15 : trough ? 14 : 13;
            unsigned old = trough ? 3 : 11;

            complementary_setup(qts, md);
            qtest_writeb(qts, BASE + mode[3], 0);
            qtest_writew(qts, BASE + tgr[3][3], old);
            qtest_writew(qts, BASE + tgr[4][2], old);
            qtest_writew(qts, BASE + tgr[4][3], old);
            qtest_writeb(qts, BASE + mode[3], 0x30 | md);
            complementary_pads(qts);
            qtest_writeb(qts, START, 0xc0);
            for (unsigned edge = 0; edge <= 60; edge++) {
                bool p = trough && ((edge >= 4 && edge < 22) || edge >= 25);
                bool n = !trough && ((edge >= 3 && edge < 10) || edge >= 13);
                bool cycle = !edge || !(((edge - 1) / 12) & 1);

                if (edge == (trough ? 17 : 5)) {
                    unsigned next = trough ? 0 : 14;

                    qtest_writew(qts, BASE + tgr[3][3], next);
                    qtest_writew(qts, BASE + tgr[4][2], next);
                    qtest_writew(qts, BASE + tgr[4][3], next);
                }
                g_test_message("mode %u edge %u", md, edge);
                assert_complementary_pads(qts, p ? 7 : 0, n ? 7 : 0, cycle);
                qtest_clock_step(qts, 124);
                assert_complementary_pads(qts, p ? 7 : 0, n ? 7 : 0, cycle);
                qtest_clock_step(qts, 1);
            }
            qtest_quit(qts);
        }
    }
}

/* D-015: the two counters reach each duty on different carrier edges. */
static void test_complementary_duty_irq(void)
{
    static const unsigned channels[] = { 3, 3, 3, 4, 4, 4, 4 };
    static const unsigned regs[] = { 1, 2, 3, 0, 1, 2, 3 };
    static const unsigned duties[] = { 4, 14, 4, 6, 8, 6, 8 };

    for (unsigned md = 13; md <= 15; md++) {
        for (unsigned buffers = 0; buffers < 4; buffers++) {
            QTestState *qts = mtu_init();

            complementary_setup(qts, md);
            qtest_writeb(qts, BASE + mode[3], md | (buffers << 4));
            qtest_writeb(qts, BASE + tier[3], 15);
            qtest_writeb(qts, BASE + tier[4], 15);
            /* Skipping affects crest/trough only, never duty or C/D. */
            qtest_writeb(qts, BASE + 0x30, 0x99);
            qtest_writeb(qts, START, 0xc0);
            for (unsigned edge = 1; edge <= 72; edge++) {
                unsigned t = (edge - 1) % 24;
                unsigned triangle = t <= 12 ? t : 24 - t;

                qtest_clock_step(qts, 124);
                for (unsigned i = 0; i < ARRAY_SIZE(channels); i++) {
                    g_assert_cmpuint(qtest_readb(qts,
                                     IR(vector[channels[i]] + regs[i])), ==, 0);
                }
                qtest_clock_step(qts, 1);
                for (unsigned i = 0; i < ARRAY_SIZE(channels); i++) {
                    unsigned ch = channels[i], gr = regs[i];
                    unsigned counter = triangle + (ch == 3 ? 2 : 0);
                    bool enabled = gr < 2 || (buffers & (1 << (gr - 2)));
                    bool match = enabled && counter == duties[i];

                    g_test_message("mode %u buffers %u edge %u ch %u gr %u",
                                   md, buffers, edge, ch, gr);
                    g_assert_cmpuint(qtest_readb(qts, IR(vector[ch] + gr)),
                                     ==, match);
                    qtest_writeb(qts, IR(vector[ch] + gr), 0);
                }
                /* Explicitly distinguish U's own counter from MTU4. */
                if (edge == 3 || edge == 23) {
                    g_assert_cmpuint(triangle + 2, ==, 4);
                } else if (edge == 5 || edge == 21) {
                    g_assert_cmpuint(triangle, ==, 4);
                    g_assert_cmpuint(triangle + 2, !=, 4);
                }
            }
            qtest_quit(qts);
        }
    }
}

/* Figures 20.47/48: suppressed active turn-ons still request (D-015). */
static void test_complementary_duty_pending(void)
{
    for (unsigned trough = 0; trough < 2; trough++) {
        for (unsigned both = 0; both < 2; both++) {
            QTestState *qts = mtu_init();
            unsigned md = both ? 15 : trough ? 14 : 13;
            unsigned old = trough ? 3 : 11;
            unsigned next = trough ? 0 : 14;

            complementary_setup(qts, md);
            qtest_writeb(qts, BASE + mode[3], 0);
            qtest_writew(qts, BASE + tgr[3][3], old);
            qtest_writew(qts, BASE + tgr[4][2], old);
            qtest_writew(qts, BASE + tgr[4][3], old);
            qtest_writeb(qts, BASE + mode[3], 0x30 | md);
            complementary_pads(qts);
            qtest_writeb(qts, BASE + tier[3], 2);
            qtest_writeb(qts, BASE + tier[4], 3);
            qtest_writeb(qts, START, 0xc0);
            for (unsigned edge = 1; edge <= 60; edge++) {
                unsigned active = qtest_readw(qts, BASE + tgr[3][1]);
                unsigned c3 = qtest_readw(qts, BASE + count[3]);
                unsigned c4 = qtest_readw(qts, BASE + count[4]);

                qtest_clock_step(qts, 125);
                g_assert_cmpuint(qtest_readb(qts, IR(130)), ==, c3 == active);
                g_assert_cmpuint(qtest_readb(qts, IR(134)), ==, c4 == active);
                g_assert_cmpuint(qtest_readb(qts, IR(135)), ==, c4 == active);
                if (edge == (trough ? 17 : 5)) {
                    qtest_writew(qts, BASE + tgr[3][3], next);
                    qtest_writew(qts, BASE + tgr[4][2], next);
                    qtest_writew(qts, BASE + tgr[4][3], next);
                }
                if (edge == (trough ? 24 : 13)) {
                    /* The temporary value is selected by waveform logic. */
                    g_assert_cmpuint(active, ==, old);
                    g_assert_cmpuint(qtest_readb(qts, IR(130)), ==, trough);
                    if (trough) {
                        /* Old negative turn-on is canceled by new turn-off. */
                        assert_complementary_pads(qts, 0, 0, false);
                    }
                }
                if (trough && edge == 25) {
                    /* MTU4=0 matches temporary duty, but active duty is 3. */
                    g_assert_cmpuint(active, ==, 3);
                    g_assert_cmpuint(qtest_readb(qts, IR(134)), ==, 0);
                    g_assert_cmpuint(qtest_readb(qts, IR(135)), ==, 0);
                }
                qtest_writeb(qts, IR(130), 0);
                qtest_writeb(qts, IR(134), 0);
                qtest_writeb(qts, IR(135), 0);
            }
            qtest_quit(qts);
        }
    }
}

/* Every new raw source reaches ELC even with its TIER bit clear. */
static void test_complementary_duty_elc(void)
{
    static const unsigned sources[] = { 0x11, 0x12, 0x13,
                                        0x15, 0x16, 0x17, 0x18 };
    static const unsigned channels[] = { 3, 3, 3, 4, 4, 4, 4 };
    static const unsigned regs[] = { 1, 2, 3, 0, 1, 2, 3 };
    static const unsigned duties[] = { 4, 14, 4, 6, 8, 6, 8 };

    for (unsigned i = 0; i < ARRAY_SIZE(sources); i++) {
        QTestState *qts = mtu_init();

        complementary_setup(qts, 15);
        qtest_writel(qts, 0x80014, 0xffffffff & ~(1U << 9));
        qtest_writeb(qts, 0x8b102, sources[i]); /* ELSR1 -> MTU1 start. */
        qtest_writeb(qts, 0x8b11f, 0xf3);
        qtest_writeb(qts, 0x8b100, 0x80);
        qtest_writeb(qts, START, 0xc0);
        for (unsigned edge = 1; edge <= 48; edge++) {
            unsigned t = (edge - 1) % 24;
            unsigned triangle = t <= 12 ? t : 24 - t;
            bool match = triangle + (channels[i] == 3 ? 2 : 0) == duties[i];

            qtest_clock_step(qts, edge == 1 ? 125 : 93);
            g_assert_cmpuint(qtest_readb(qts,
                             IR(vector[channels[i]] + regs[i])), ==, 0);
            qtest_clock_step(qts, 32); /* Deliver ELC on the next PCLKB. */
            g_assert_cmpuint(qtest_readb(qts, START), ==,
                             match ? 0xc2 : 0xc0);
            qtest_writeb(qts, START, 0xc0);
        }
        qtest_quit(qts);
    }
}

static void test_complementary_polarity(void)
{
    QTestState *qts = mtu_init();

    complementary_setup(qts, 15);
    complementary_pads(qts);
    qtest_writeb(qts, BASE + 0xe, 0x44); /* TOCR2 selects each arm. */
    qtest_writeb(qts, BASE + 0xf, 0x40 | 0x19); /* Transfer at crest. */
    qtest_writeb(qts, BASE + 0x36, 0x26);
    assert_complementary_pads(qts, 2, 5, true);
    qtest_writeb(qts, START, 0xc0);
    qtest_clock_step(qts, 9 * 125); /* All three positive arms active. */
    assert_complementary_pads(qts, 5, 5, true);
    qtest_clock_step(qts, 4 * 125 - 1);
    assert_complementary_pads(qts, 5, 5, true);
    g_assert_cmpuint(qtest_readb(qts, BASE + 0xf), ==, 0x59);
    qtest_clock_step(qts, 1);
    g_assert_cmpuint(qtest_readb(qts, BASE + 0xf), ==, 0x66);
    assert_complementary_pads(qts, 2, 2, false);
    qtest_writeb(qts, BASE + 0xf, 0x80 | 0x26); /* Transfer at trough. */
    qtest_writeb(qts, BASE + 0x36, 0x19);
    qtest_clock_step(qts, 12 * 125 - 1);
    g_assert_cmpuint(qtest_readb(qts, BASE + 0xf), ==, 0xa6);
    qtest_clock_step(qts, 1);
    g_assert_cmpuint(qtest_readb(qts, BASE + 0xf), ==, 0x99);
    assert_complementary_pads(qts, 2, 2, true);
    qtest_writeb(qts, BASE + 0xe, 4); /* PSYE disables only the cycle pad. */
    assert_pad(qts, 0x17, false, false);
    qtest_writeb(qts, BASE + 0xa, 0);
    assert_pad(qts, 0x22, false, false);
    qtest_writeb(qts, BASE + 0xa, 0x3f);
    assert_pad(qts, 0x22, true, false);
    qtest_quit(qts);
}

/* D-016: only the natural Tb1 boundary transfers the temporary bank. */
static void test_complementary_cce(void)
{
    QTestState *qts = mtu_init();

    complementary_setup(qts, 13);
    qtest_writeb(qts, BASE + 0xe, 3); /* PSYE=0, active high. */
    g_assert_cmpuint(qtest_readb(qts, BASE + 0x60), ==, 0);
    qtest_writeb(qts, BASE + 0x60, 0x80);
    qtest_writeb(qts, START, 0xc0);
    qtest_clock_step(qts, 6 * 125);
    complementary_buffers(qts, 8, 12, true);
    qtest_clock_step(qts, 6 * 125);

    qtest_clock_step(qts, 124);
    g_assert_cmpuint(qtest_readw(qts, BASE + count[3]), ==, 14);
    qtest_clock_step(qts, 1); /* Edge 13: counters only. */
    g_assert_cmpuint(qtest_readw(qts, BASE + count[3]), ==, 2);
    g_assert_cmpuint(qtest_readw(qts, BASE + count[4]), ==, 0);
    g_assert_cmpuint(qtest_readw(qts, BASE + 0x20), ==, 0);
    assert_complementary_bank(qts, 4, 12);
    qtest_clock_step(qts, 26 * 125);
    assert_complementary_bank(qts, 4, 12);
    /* Disable CCE while stopped; the old bank still transfers first. */
    qtest_writeb(qts, START, 0);
    qtest_writeb(qts, BASE + 0x60, 0);
    qtest_writeb(qts, START, 0xc0);
    qtest_clock_step(qts, 15 * 125 - 1);
    assert_complementary_bank(qts, 4, 12);
    qtest_clock_step(qts, 1);
    assert_complementary_bank(qts, 8, 12);
    /* A Tb1 commit still snapshots at the next eligible Ta, after clear. */
    qtest_writeb(qts, START, 0);
    qtest_readb(qts, BASE + 0x60);
    qtest_writeb(qts, BASE + 0x60, 0x80);
    qtest_writeb(qts, START, 0xc0);
    qtest_clock_step(qts, 12 * 125);
    complementary_buffers(qts, 5, 12, true);
    qtest_clock_step(qts, 125);
    assert_complementary_bank(qts, 8, 12);
    qtest_clock_step(qts, 5 * 125);
    qtest_writeb(qts, START, 0);
    qtest_writeb(qts, BASE + 0x60, 0);
    qtest_writeb(qts, START, 0xc0);
    qtest_clock_step(qts, 15 * 125);
    assert_complementary_bank(qts, 5, 12);
    qtest_quit(qts);
}

static void test_twcr_register(void)
{
    QTestState *qts = mtu_init();

    qtest_writeb(qts, BASE + 0x60, 0xff); /* No preceding read. */
    g_assert_cmpuint(qtest_readb(qts, BASE + 0x60), ==, 0);
    qtest_writeb(qts, BASE + 0x60, 1);
    g_assert_cmpuint(qtest_readb(qts, BASE + 0x60), ==, 1);
    qtest_writeb(qts, BASE + 0x60, 0x81); /* CCE was read as zero. */
    g_assert_cmpuint(qtest_readb(qts, BASE + 0x60), ==, 0x81);
    qtest_writeb(qts, BASE + 0x60, 0);
    qtest_writeb(qts, BASE + 0x60, 0x81); /* Read of ones cannot set. */
    g_assert_cmpuint(qtest_readb(qts, BASE + 0x60), ==, 0);
    qtest_writeb(qts, BASE + 0x60, 0); /* Consumes qualification. */
    qtest_writeb(qts, BASE + 0x60, 0x81);
    g_assert_cmpuint(qtest_readb(qts, BASE + 0x60), ==, 0);
    qtest_readb(qts, RWE);
    qtest_writeb(qts, RWE, 0);
    qtest_writew(qts, BASE + 0x60, 0xffff); /* Unprotected, 16-bit access. */
    g_assert_cmpuint(qtest_readw(qts, BASE + 0x60), ==, 0x81);
    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    qtest_writew(qts, 0x803fe, 0xa503);
    qtest_writel(qts, MSTPCRA, 0xeffffdff);
    g_assert_cmpuint(qtest_readb(qts, BASE + 0x60), ==, 0);
    qtest_quit(qts);
}

/* Clear from MTU0 at each carrier edge, including both Tb2 boundaries. */
static void test_complementary_sync(void)
{
    for (unsigned md = 13; md <= 15; md++) {
        for (unsigned wre = 0; wre < 2; wre++) {
            for (unsigned edge = 1; edge <= 29; edge++) {
                QTestState *qts = mtu_init();
                unsigned p = 0, n = 0;
                unsigned src = (edge - 1) % 3;

                complementary_setup(qts, md);
                complementary_pads(qts);
                qtest_writeb(qts, BASE + tcr[3], 0x61);
                qtest_writeb(qts, BASE + 0x81, 0xc0 | (1 << src));
                qtest_readb(qts, BASE + 0x60);
                qtest_writeb(qts, BASE + 0x60, wre);
                qtest_writeb(qts, START, 0xc0);
                if (edge > 1) {
                    qtest_clock_step(qts, (edge - 1) * 125);
                }
                /* Independent carrier output on the clear edge. */
                for (unsigned i = 0; i < 3; i++) {
                    unsigned duty = 4 + 2 * i;
                    unsigned t = edge % 24;

                    if (edge >= 3 && t >= duty + 1 && t < 25 - duty) {
                        p |= 1 << i;
                    }
                    if (edge >= 3 && (t >= 27 - duty || t < duty - 1)) {
                        n |= 1 << i;
                    }
                }
                qtest_writeb(qts, BASE + tcr[src], 0x21);
                qtest_writew(qts, BASE + tgr[src][0], 0);
                qtest_writeb(qts, START, 0xc0 | (1 << src));
                qtest_clock_step(qts, 124);
                qtest_clock_step(qts, 1);
                g_assert_cmpuint(qtest_readw(qts, BASE + count[3]), ==, 2);
                g_assert_cmpuint(qtest_readw(qts, BASE + count[4]), ==, 0);
                g_assert_cmpuint(qtest_readw(qts, BASE + 0x20), ==, 0);
                g_test_message("mode %u WRE %u edge %u", md, wre, edge);
                /* Natural region after the shared edge: later Tb2 only. */
                bool retain = wre && edge >= 23 && edge < 27;

                assert_complementary_pads(qts, retain ? p : 0,
                                         retain ? n : 0,
                                         !retain || edge < 13 || edge >= 25);
                if (retain) {
                    /* Repeated retained clears keep the output. */
                    qtest_clock_step(qts, 125);
                    assert_complementary_pads(qts, p, n,
                                             edge < 13 || edge >= 25);
                }
                qtest_writeb(qts, START, 0);
                qtest_writeb(qts, START, 0xc0);
                qtest_clock_step(qts, 125);
                g_assert_cmpuint(qtest_readw(qts, BASE + count[3]), ==, 3);
                qtest_quit(qts);
            }
        }
    }
}

static void test_twcr_migration(void)
{
    for (unsigned edge = 4; edge <= 22; edge += 9) {
        QTestState *src = mtu_init();
        QTestState *dst;
        g_autofree char *dir = g_dir_make_tmp("rx113-twcr-XXXXXX", NULL);
        g_autofree char *file = g_strdup_printf("%s/state", dir);
        g_autofree char *uri = g_strdup_printf("file:%s", file);
        int64_t now;

        g_assert_nonnull(dir);
        complementary_setup(src, 13);
        complementary_pads(src);
        qtest_readb(src, BASE + 0x60);
        qtest_writeb(src, BASE + 0x60, edge == 22 ? 0x81 : 1);
        qtest_writeb(src, START, 0xc0);
        if (edge == 22) {
            qtest_writeb(src, BASE + 0xe, 3);
            qtest_clock_step(src, 6 * 125);
            complementary_buffers(src, 8, 12, true);
            qtest_clock_step(src, (edge - 6) * 125);
        } else {
            qtest_clock_step(src, edge * 125);
        }
        if (edge == 13) {
            complementary_buffers(src, 8, 12, true);
        }
        qtest_readb(src, BASE + 0x60); /* CCE=0 qualification migrates. */
        now = qtest_clock_step(src, 62);
        qtest_qmp_assert_success(src, "{'execute':'migrate',"
                                "'arguments':{'uri':%s}}", uri);
        migration_wait(src);
        dst = qtest_init("-M rx113-r5f51138 -incoming defer");
        qtest_clock_set(dst, now);
        qtest_qmp_assert_success(dst, "{'execute':'migrate-incoming',"
                                "'arguments':{'uri':%s}}", uri);
        migration_wait(dst);
        if (edge == 4) {
            /* Still initial Tb2, although the initial pad window has ended. */
            qtest_writeb(dst, BASE + tcr[3], 0x61);
            qtest_writeb(dst, BASE + 0x81, 0xc1);
            qtest_writeb(dst, BASE + tcr[0], 0x20);
            qtest_writew(dst, BASE + tgr[0][0], 0);
            qtest_writeb(dst, START, 0xc1);
            qtest_clock_step(dst, 32);
            assert_complementary_pads(dst, 0, 0, true);
        }
        if (edge == 22) {
            qtest_clock_step(dst, 26 * 125);
            assert_complementary_bank(dst, 4, 12);
            qtest_writeb(dst, START, 0);
            qtest_writeb(dst, BASE + 0x60, 0);
            qtest_writeb(dst, START, 0xc0);
            qtest_clock_step(dst, 15 * 125);
            assert_complementary_bank(dst, 8, 12);
            qtest_readb(dst, BASE + 0x60);
        }
        qtest_writeb(dst, START, 0);
        qtest_writeb(dst, BASE + 0x60, 0x81);
        g_assert_cmphex(qtest_readb(dst, BASE + 0x60), ==, 0x81);
        qtest_quit(dst);
        qtest_quit(src);
        unlink(file);
        rmdir(dir);
    }
}

static void test_complementary_count(void)
{
    QTestState *qts = mtu_init();
    /* Figure 20.39: initial Tb2, Ta, crest Tb1, Ta, trough Tb2. */
    static const unsigned sub[] = {
        0, 1, 2, 1, 0, 14, 14, 14, 14, 14, 14, 13, 12,
        13, 14, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 1, 0, 14, 14,
    };

    complementary_setup(qts, 15);
    assert_complementary_bank(qts, 4, 12);
    qtest_writeb(qts, BASE + tier[3], 1);
    qtest_writeb(qts, BASE + tier[4], 0x10);
    qtest_writeb(qts, START, 0xc0);
    for (unsigned edge = 0; edge <= 74; edge++) {
        unsigned t = edge % 24;
        unsigned triangle = t <= 12 ? t : 24 - t;
        bool down = edge && (t > 12 || t == 0);
        unsigned subscript = edge < 27 ? edge : (edge - 5) % 24 + 5;

        g_assert_cmpuint(qtest_readw(qts, BASE + count[3]), ==, triangle + 2);
        g_assert_cmpuint(qtest_readw(qts, BASE + count[4]), ==, triangle);
        g_assert_cmpuint(qtest_readw(qts, BASE + 0x20), ==, sub[subscript]);
        g_assert_cmpuint(qtest_readb(qts, BASE + 0x2c) >> 7, ==, !down);
        g_assert_cmpuint(qtest_readb(qts, BASE + 0x2d) >> 7, ==, !down);
        g_assert_cmpuint(qtest_readb(qts, IR(129)), ==, edge && t == 13);
        g_assert_cmpuint(qtest_readb(qts, IR(138)), ==, edge > 24 && t == 1);
        g_assert_cmpuint(qtest_readb(qts, IR(139)), ==, 0);
        qtest_writeb(qts, IR(129), 0);
        qtest_writeb(qts, IR(138), 0);
        qtest_clock_step(qts, 124);
        g_assert_cmpuint(qtest_readw(qts, BASE + count[4]), ==, triangle);
        qtest_clock_step(qts, 1);
    }
    qtest_writeb(qts, START, 0);
    g_assert_cmpuint(qtest_readw(qts, BASE + count[3]), ==, 2);
    g_assert_cmpuint(qtest_readw(qts, BASE + count[4]), ==, 0);
    qtest_quit(qts);
}

static void test_complementary_buffers(const void *opaque)
{
    unsigned md = GPOINTER_TO_UINT(opaque);
    QTestState *qts = mtu_init();

    complementary_setup(qts, md);
    qtest_writeb(qts, START, 0xc0);
    qtest_clock_step(qts, 6 * 125); /* Ta: commit snapshots all five. */
    complementary_buffers(qts, 5, 12, true);
    qtest_clock_step(qts, 6 * 125); /* Tb1: commit must wait. */
    complementary_buffers(qts, 3, 12, true);
    assert_complementary_bank(qts, 4, 12);
    qtest_clock_step(qts, 3 * 125 - 1);
    assert_complementary_bank(qts, 4, 12);
    qtest_clock_step(qts, 1); /* End Tb1: consume the OLD temporary bank. */
    assert_complementary_bank(qts, md == 14 ? 4 : 5, 12);
    qtest_clock_step(qts, 12 * 125); /* End Tb2. */
    assert_complementary_bank(qts, md == 13 ? 5 : 3, 12);
    qtest_clock_step(qts, 12 * 125); /* Next end Tb1. */
    assert_complementary_bank(qts, 3, 12);

    /* Buffer writes without MTU4.TGRD cannot authorize any transfer. */
    complementary_buffers(qts, 9, 16, false);
    qtest_clock_step(qts, 48 * 125);
    assert_complementary_bank(qts, 3, 12);
    /* Rewriting unchanged TGRD is sufficient to commit the other four. */
    qtest_writew(qts, BASE + tgr[4][3], 7);
    qtest_clock_step(qts, 72 * 125);
    g_assert_cmpuint(qtest_readw(qts, BASE + tgr[3][1]), ==, 9);
    g_assert_cmpuint(qtest_readw(qts, BASE + tgr[4][0]), ==, 11);
    g_assert_cmpuint(qtest_readw(qts, BASE + tgr[4][1]), ==, 7);
    g_assert_cmpuint(qtest_readw(qts, BASE + tgr[3][0]), ==, 18);
    g_assert_cmpuint(qtest_readw(qts, BASE + 0x14), ==, 16);
    qtest_quit(qts);
}

static void test_complementary_cycle(void)
{
    QTestState *qts = mtu_init();

    for (unsigned half = 8; half <= 16; half += 8) {
        complementary_setup(qts, 13);
        qtest_writeb(qts, BASE + tier[3], 1);
        qtest_writeb(qts, START, 0xc0);
        qtest_clock_step(qts, 6 * 125);
        complementary_buffers(qts, 4, half, true);
        qtest_clock_step(qts, 9 * 125); /* End of crest Tb1. */
        assert_complementary_bank(qts, 4, half);
        qtest_writeb(qts, IR(129), 0);
        for (unsigned edge = 15; edge <= 24 + 4 * half; edge++) {
            unsigned t = edge <= 24 ? 24 - edge :
                         (edge - 24) % (2 * half);
            unsigned triangle = edge <= 24 || t <= half ? t : 2 * half - t;
            bool crest = edge > 24 && t == half + 1;

            /* The in-progress down-ramp keeps the OLD carrier length. */
            g_assert_cmpuint(qtest_readw(qts, BASE + count[3]), ==,
                             triangle + 2);
            g_assert_cmpuint(qtest_readw(qts, BASE + count[4]), ==, triangle);
            g_assert_cmpuint(qtest_readb(qts, IR(129)), ==, crest);
            qtest_writeb(qts, IR(129), 0);
            qtest_clock_step(qts, 125);
        }
        qtest_writeb(qts, START, 0);
    }
    qtest_quit(qts);
}

static void test_complementary_clock(void)
{
    QTestState *qts = mtu_init();

    complementary_setup(qts, 15);
    complementary_pads(qts);
    qtest_writeb(qts, START, 0xc0);
    qtest_clock_step(qts, 12 * 125 + 62); /* Inside the crest Tb1. */
    assert_complementary_pads(qts, 7, 0, true);
    qtest_writel(qts, MSTPCRA, 0xefffffff);
    qtest_clock_step(qts, 10000);
    assert_complementary_pads(qts, 7, 0, true);
    qtest_writel(qts, MSTPCRA, 0xeffffdff);
    g_assert_cmpuint(qtest_readw(qts, BASE + count[3]), ==, 14);
    g_assert_cmpuint(qtest_readw(qts, BASE + count[4]), ==, 12);
    g_assert_cmpuint(qtest_readw(qts, BASE + 0x20), ==, 12);
    assert_complementary_pads(qts, 7, 0, true);
    qtest_clock_step(qts, 62);
    g_assert_cmpuint(qtest_readw(qts, BASE + count[3]), ==, 14);
    qtest_clock_step(qts, 1);
    g_assert_cmpuint(qtest_readw(qts, BASE + count[3]), ==, 13);
    assert_complementary_pads(qts, 7, 0, false);
    qtest_writeb(qts, START, 0);
    assert_complementary_pads(qts, 0, 0, true);
    g_assert_cmpuint(qtest_readw(qts, BASE + count[3]), ==, 2);
    g_assert_cmpuint(qtest_readw(qts, BASE + count[4]), ==, 0);
    g_assert_cmpuint(qtest_readw(qts, BASE + 0x20), ==, 0);

    /* Equal external clocks still use one coupled edge. */
    qtest_writeb(qts, BASE + tcr[3], 6);
    qtest_writeb(qts, BASE + tcr[4], 6);
    qtest_writeb(qts, START, 0xc0);
    qtest_clock_step(qts, 10000);
    g_assert_cmpuint(qtest_readw(qts, BASE + count[4]), ==, 0);
    for (unsigned i = 0; i < 13; i++) {
        pin(qts, 0, true);
        pin(qts, 0, false);
    }
    g_assert_cmpuint(qtest_readw(qts, BASE + count[3]), ==, 13);
    g_assert_cmpuint(qtest_readw(qts, BASE + count[4]), ==, 11);
    g_assert_cmpuint(qtest_readw(qts, BASE + 0x20), ==, 13);
    assert_complementary_pads(qts, 7, 0, false);
    qtest_quit(qts);
}

static void test_complementary_trough_elc(void)
{
    QTestState *qts = mtu_init();

    complementary_setup(qts, 15);
    qtest_writel(qts, 0x80014, 0xffffffff & ~(1U << 9)); /* ELC */
    qtest_writeb(qts, 0x8b102, 0x1a); /* ELSR1: trough -> MTU1 start. */
    qtest_writeb(qts, 0x8b11f, 0xf3); /* ELOPA: MTU1 start. */
    qtest_writeb(qts, 0x8b100, 0x80);
    qtest_writeb(qts, BASE + tier[4], 0x10);
    qtest_writeb(qts, BASE + 0x30, 9); /* Skip the first trough IRQ. */
    qtest_writeb(qts, START, 0xc0);
    qtest_clock_step(qts, 25 * 125 - 1);
    g_assert_cmpuint(qtest_readb(qts, IR(138)), ==, 0);
    g_assert_cmpuint(qtest_readb(qts, START), ==, 0xc0);
    qtest_clock_step(qts, 1);
    g_assert_cmpuint(qtest_readb(qts, IR(138)), ==, 0);
    g_assert_cmpuint(qtest_readb(qts, BASE + 0x31), ==, 1);
    g_assert_cmpuint(qtest_readb(qts, IR(139)), ==, 0);
    qtest_clock_step(qts, 32);
    g_assert_cmpuint(qtest_readb(qts, START), ==, 0xc2);
    /* ELC generation is independent of the ICU request enable. */
    qtest_writeb(qts, BASE + tier[4], 0);
    qtest_writeb(qts, IR(138), 0);
    qtest_writeb(qts, START, 0xc0);
    qtest_clock_step(qts, 24 * 125);
    g_assert_cmpuint(qtest_readb(qts, START), ==, 0xc2);
    g_assert_cmpuint(qtest_readb(qts, IR(138)), ==, 0);
    g_assert_cmpuint(qtest_readb(qts, IR(139)), ==, 0);
    qtest_quit(qts);
}

static void test_complementary_registers(void)
{
    QTestState *qts = mtu_init();

    g_assert_cmpuint(qtest_readw(qts, BASE + 0x14), ==, 0xffff);
    g_assert_cmpuint(qtest_readw(qts, BASE + 0x16), ==, 0xffff);
    g_assert_cmpuint(qtest_readw(qts, BASE + 0x22), ==, 0xffff);
    g_assert_cmpuint(qtest_readw(qts, BASE + 0x20), ==, 0);
    qtest_writew(qts, BASE + 0x20, 1234); /* TCNTS is read-only. */
    g_assert_cmpuint(qtest_readw(qts, BASE + 0x20), ==, 0);
    complementary_setup(qts, 15);
    qtest_readb(qts, RWE);
    qtest_writeb(qts, RWE, 0);
    qtest_writew(qts, BASE + 0x14, 55);
    qtest_writew(qts, BASE + 0x16, 66);
    qtest_writew(qts, BASE + 0x22, 77); /* Buffer is not protected. */
    g_assert_cmpuint(qtest_readw(qts, BASE + 0x14), ==, 0);
    g_assert_cmpuint(qtest_readw(qts, BASE + 0x16), ==, 0);
    g_assert_cmpuint(qtest_readw(qts, BASE + 0x22), ==, 77);
    qtest_writeb(qts, RWE, 1);
    assert_complementary_bank(qts, 4, 12);
    g_assert_cmpuint(qtest_readw(qts, BASE + 0x16), ==, 2);
    qtest_writeb(qts, BASE + 0x16, 0xff); /* 16-bit access only. */
    g_assert_cmpuint(qtest_readw(qts, BASE + 0x16), ==, 2);
    qtest_writew(qts, BASE + 0x22, 12);
    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    qtest_writew(qts, 0x803fe, 0xa502);
    qtest_writel(qts, MSTPCRA, 0xeffffdff);
    g_assert_cmpuint(qtest_readw(qts, BASE + 0x14), ==, 0xffff);
    g_assert_cmpuint(qtest_readw(qts, BASE + 0x16), ==, 0xffff);
    g_assert_cmpuint(qtest_readw(qts, BASE + 0x22), ==, 0xffff);
    g_assert_cmpuint(qtest_readw(qts, BASE + 0x20), ==, 0);
    qtest_quit(qts);
}

static void test_complementary_buffer_enable(void)
{
    QTestState *qts = mtu_init();

    for (unsigned flags = 0x10; flags <= 0x20; flags += 0x10) {
        complementary_setup(qts, 13);
        qtest_writeb(qts, BASE + mode[3], flags | 13);
        qtest_writeb(qts, START, 0xc0);
        qtest_clock_step(qts, 6 * 125);
        complementary_buffers(qts, 5, 16, true);
        qtest_clock_step(qts, 9 * 125);
        /* Section 20.6.14: BFA owns cycles/A4, BFB owns B3/B4. */
        g_assert_cmpuint(qtest_readw(qts, BASE + tgr[3][0]), ==,
                         flags == 0x10 ? 18 : 14);
        g_assert_cmpuint(qtest_readw(qts, BASE + 0x14), ==,
                         flags == 0x10 ? 16 : 12);
        g_assert_cmpuint(qtest_readw(qts, BASE + tgr[4][0]), ==,
                         flags == 0x10 ? 7 : 6);
        g_assert_cmpuint(qtest_readw(qts, BASE + tgr[3][1]), ==,
                         flags == 0x20 ? 5 : 4);
        g_assert_cmpuint(qtest_readw(qts, BASE + tgr[4][1]), ==,
                         flags == 0x20 ? 9 : 8);
        qtest_writeb(qts, START, 0);
    }
    qtest_quit(qts);
}

/* Sections 20.2.26-28 and Figure 20.69: independent source counters. */
static void test_complementary_skip(void)
{
    for (unsigned cor = 0; cor <= 7; cor++) {
        QTestState *qts = mtu_init();
        unsigned cnt3 = 0, cnt4 = 0;

        complementary_setup(qts, 15);
        qtest_writeb(qts, BASE + 0x30, 0x88 | (cor << 4) | (7 - cor));
        qtest_writeb(qts, BASE + tier[3], 1);
        qtest_writeb(qts, BASE + tier[4], 0x10);
        qtest_writeb(qts, START, 0xc0);
        for (unsigned edge = 1; edge <= 24 * 18; edge++) {
            bool crest = edge % 24 == 13;
            bool trough = edge > 24 && edge % 24 == 1;
            bool irq3 = crest && cnt3 == cor;
            bool irq4 = trough && cnt4 == 7 - cor;

            qtest_clock_step(qts, 125);
            if (crest) {
                cnt3 = irq3 ? 0 : cnt3 + 1;
            }
            if (trough) {
                cnt4 = irq4 ? 0 : cnt4 + 1;
            }
            g_assert_cmpuint(qtest_readb(qts, IR(129)), ==, irq3);
            g_assert_cmpuint(qtest_readb(qts, IR(138)), ==, irq4);
            g_assert_cmpuint(qtest_readb(qts, BASE + 0x31), ==,
                             (cnt3 << 4) | cnt4);
            qtest_writeb(qts, IR(129), 0);
            qtest_writeb(qts, IR(138), 0);
        }
        qtest_writeb(qts, START, 0); /* CST stop retains skipping counts. */
        g_assert_cmpuint(qtest_readb(qts, BASE + 0x31), ==,
                         (cnt3 << 4) | cnt4);
        qtest_writeb(qts, BASE + 0x31, 0xff); /* Read-only. */
        g_assert_cmpuint(qtest_readb(qts, BASE + 0x31), ==,
                         (cnt3 << 4) | cnt4);
        qtest_writeb(qts, BASE + 0x30, 0x88); /* COR=0 clears both. */
        g_assert_cmpuint(qtest_readb(qts, BASE + 0x31), ==, 0);
        qtest_quit(qts);
    }
}

/* Figures 20.70-72: blocked writes wait without bypassing the old bank. */
static void test_complementary_skip_buffers(void)
{
    QTestState *qts = mtu_init();

    complementary_setup(qts, 13);
    g_assert_cmpuint(qtest_readb(qts, BASE + 0x30), ==, 0);
    g_assert_cmpuint(qtest_readb(qts, BASE + 0x31), ==, 0);
    g_assert_cmpuint(qtest_readb(qts, BASE + 0x32), ==, 0);
    qtest_writeb(qts, START, 0xc0);
    qtest_clock_step(qts, 6 * 125);
    complementary_buffers(qts, 5, 12, true); /* Snapshot Data1 in Ta. */
    qtest_writeb(qts, BASE + 0x32, 1);
    complementary_buffers(qts, 3, 12, true); /* Block Data2 snapshot. */
    qtest_clock_step(qts, 9 * 125);
    assert_complementary_bank(qts, 5, 12); /* Active transfer still runs. */
    qtest_clock_step(qts, 24 * 125);
    assert_complementary_bank(qts, 5, 12);
    qtest_writeb(qts, BASE + 0x32, 0); /* Re-enable: accept pending Data2. */
    qtest_clock_step(qts, 24 * 125);
    assert_complementary_bank(qts, 3, 12);
    qtest_quit(qts);

    for (unsigned md = 13; md <= 15; md++) {
        for (unsigned source = 0; source < 3; source++) {
            qts = mtu_init();
            complementary_setup(qts, md);
            qtest_writeb(qts, BASE + 0x30,
                         source == 0 ? 0x90 : source == 1 ? 9 : 0x99);
            qtest_writeb(qts, BASE + 0x32, 2);
            qtest_writeb(qts, START, 0xc0);
            qtest_clock_step(qts, 6 * 125);
            complementary_buffers(qts, 5, 12, true);
            qtest_clock_step(qts, 21 * 125); /* End first trough Tb. */
            assert_complementary_bank(qts, 4, 12);
            /* Source passes at edge 37 (crest) or 49 (trough). */
            qtest_clock_step(qts, 12 * 125); /* End second crest Tb. */
            assert_complementary_bank(qts, 4, 12);
            qtest_clock_step(qts, 12 * 125); /* End second trough Tb. */
            assert_complementary_bank(qts,
                                      md != 13 && source != 1 ? 5 : 4, 12);
            qtest_clock_step(qts, 12 * 125); /* End third crest Tb. */
            assert_complementary_bank(qts,
                                      md == 14 && source == 1 ? 4 : 5, 12);
            qtest_clock_step(qts, 12 * 125);
            assert_complementary_bank(qts, 5, 12);
            qtest_quit(qts);
        }
    }

    qts = mtu_init();
    complementary_setup(qts, 15);
    qtest_writeb(qts, BASE + 0x32, 0xfe); /* Reserved bits read zero. */
    g_assert_cmpuint(qtest_readb(qts, BASE + 0x32), ==, 2);
    qtest_writeb(qts, START, 0xc0);
    qtest_clock_step(qts, 6 * 125);
    complementary_buffers(qts, 5, 12, true);
    qtest_clock_step(qts, 96 * 125);
    assert_complementary_bank(qts, 4, 12); /* No skipping: no link window. */
    qtest_quit(qts);
}

static void test_complementary_skip_migration(void)
{
    for (unsigned edge = 38; edge <= 40; edge += 2) {
        QTestState *src = mtu_init();
        QTestState *dst;
        int64_t now;
        g_autofree char *dir = g_dir_make_tmp("rx113-mtu-skip-XXXXXX", NULL);
        g_autofree char *file = g_strdup_printf("%s/state", dir);
        g_autofree char *uri = g_strdup_printf("file:%s", file);

        g_assert_nonnull(dir);
        complementary_setup(src, 15);
        complementary_pads(src);
        qtest_writeb(src, BASE + 0x30, 0x99);
        qtest_writeb(src, BASE + 0x32, 2);
        qtest_writeb(src, BASE + tier[3], 1);
        qtest_writeb(src, BASE + tier[4], 0x10);
        qtest_writeb(src, START, 0xc0);
        qtest_clock_step(src, 6 * 125);
        complementary_buffers(src, 5, 12, true);
        now = qtest_clock_step(src, (edge - 6) * 125 + 62);
        g_assert_cmpuint(qtest_readb(src, BASE + 0x31), ==, 1);
        qtest_writeb(src, IR(129), 0);
        qtest_qmp_assert_success(src, "{'execute':'migrate',"
                                "'arguments':{'uri':%s}}", uri);
        migration_wait(src);
        dst = qtest_init("-M rx113-r5f51138 -incoming defer");
        qtest_clock_set(dst, now);
        qtest_qmp_assert_success(dst, "{'execute':'migrate-incoming',"
                                "'arguments':{'uri':%s}}", uri);
        migration_wait(dst);
        g_assert_cmpuint(qtest_readb(dst, BASE + 0x30), ==, 0x99);
        g_assert_cmpuint(qtest_readb(dst, BASE + 0x31), ==, 1);
        g_assert_cmpuint(qtest_readb(dst, BASE + 0x32), ==, 2);
        g_assert_cmpuint(qtest_readb(dst, IR(129)), ==, 0);
        assert_complementary_bank(dst, 4, 12);
        if (edge == 40) {
            /* Inside an open Ta window: migrated readiness accepts Data2. */
            complementary_buffers(dst, 3, 12, true);
        }
        qtest_clock_step(dst, (51 - edge) * 125 - 63);
        assert_complementary_bank(dst, 4, 12);
        qtest_clock_step(dst, 1);
        assert_complementary_bank(dst, edge == 38 ? 5 : 3, 12);
        g_assert_cmpuint(qtest_readb(dst, IR(138)), ==, 1);
        g_assert_cmpuint(qtest_readb(dst, BASE + 0x31), ==, 0);
        qtest_clock_step(dst, 10 * 125);
        g_assert_cmpuint(qtest_readb(dst, IR(129)), ==, 0);
        g_assert_cmpuint(qtest_readb(dst, BASE + 0x31), ==, 0x10);
        qtest_quit(dst);
        qtest_quit(src);
        unlink(file);
        rmdir(dir);
    }
}

static void test_complementary_skip_gates(void)
{
    QTestState *qts = mtu_init();

    complementary_setup(qts, 15);
    qtest_writeb(qts, BASE + 0x30, 0x99);
    qtest_writeb(qts, START, 0xc0); /* TIER disabled: sources still count. */
    qtest_clock_step(qts, 25 * 125 + 62);
    g_assert_cmpuint(qtest_readb(qts, BASE + 0x31), ==, 0x11);
    g_assert_cmpuint(qtest_readb(qts, IR(129)), ==, 0);
    g_assert_cmpuint(qtest_readb(qts, IR(138)), ==, 0);
    qtest_writel(qts, MSTPCRA, 0xefffffff);
    qtest_clock_step(qts, 100000);
    qtest_writel(qts, MSTPCRA, 0xeffffdff);
    g_assert_cmpuint(qtest_readb(qts, BASE + 0x31), ==, 0x11);
    qtest_writeb(qts, BASE + tier[3], 1);
    qtest_writeb(qts, BASE + tier[4], 0x10);
    qtest_clock_step(qts, 12 * 125 - 63);
    g_assert_cmpuint(qtest_readb(qts, IR(129)), ==, 0);
    qtest_clock_step(qts, 1);
    g_assert_cmpuint(qtest_readb(qts, IR(129)), ==, 1);
    g_assert_cmpuint(qtest_readb(qts, BASE + 0x31), ==, 1);
    qtest_writeb(qts, BASE + 0x30, 0x91); /* Disable T4VEN only. */
    g_assert_cmpuint(qtest_readb(qts, BASE + 0x31), ==, 0);
    qtest_clock_step(qts, 24 * 125);
    g_assert_cmpuint(qtest_readb(qts, BASE + 0x31), ==, 0x10);
    qtest_writeb(qts, BASE + 0x30, 0x11); /* Disable T3AEN. */
    g_assert_cmpuint(qtest_readb(qts, BASE + 0x31), ==, 0);
    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    qtest_writew(qts, 0x803fe, 0xa502);
    qtest_writel(qts, MSTPCRA, 0xeffffdff);
    g_assert_cmpuint(qtest_readb(qts, BASE + 0x30), ==, 0);
    g_assert_cmpuint(qtest_readb(qts, BASE + 0x31), ==, 0);
    g_assert_cmpuint(qtest_readb(qts, BASE + 0x32), ==, 0);
    qtest_quit(qts);

    qts = mtu_init();
    /* Skipping settings must not affect ordinary MTU3 compare requests. */
    qtest_writeb(qts, BASE + 0x30, 0xff);
    qtest_writeb(qts, BASE + tcr[3], 0x21);
    qtest_writew(qts, BASE + tgr[3][0], 3);
    qtest_writeb(qts, BASE + tier[3], 1);
    qtest_writeb(qts, START, 0x40);
    for (unsigned i = 0; i < 3; i++) {
        qtest_clock_step(qts, 4 * 125);
        g_assert_cmpuint(qtest_readb(qts, IR(129)), ==, 1);
        g_assert_cmpuint(qtest_readb(qts, BASE + 0x31), ==, 0);
        qtest_writeb(qts, IR(129), 0);
    }
    qtest_quit(qts);
}

static void test_complementary_migration(void)
{
    QTestState *src = mtu_init();
    QTestState *dst;
    int64_t now;
    g_autofree char *dir = g_dir_make_tmp("rx113-mtu-comp-XXXXXX", NULL);
    g_autofree char *file = g_strdup_printf("%s/state", dir);
    g_autofree char *uri = g_strdup_printf("file:%s", file);

    g_assert_nonnull(dir);
    complementary_setup(src, 15);
    complementary_pads(src);
    qtest_writeb(src, START, 0xc0);
    qtest_clock_step(src, 6 * 125);
    complementary_buffers(src, 5, 12, true);
    now = qtest_clock_step(src, 6 * 125 + 62);
    complementary_buffers(src, 3, 12, true);
    qtest_qmp_assert_success(src, "{'execute':'migrate',"
                            "'arguments':{'uri':%s}}", uri);
    migration_wait(src);
    dst = qtest_init("-M rx113-r5f51138 -incoming defer");
    qtest_clock_set(dst, now);
    qtest_qmp_assert_success(dst, "{'execute':'migrate-incoming',"
                            "'arguments':{'uri':%s}}", uri);
    migration_wait(dst);
    assert_complementary_bank(dst, 4, 12);
    assert_complementary_pads(dst, 7, 0, true);
    qtest_clock_step(dst, 3 * 125 - 63);
    assert_complementary_bank(dst, 4, 12);
    qtest_clock_step(dst, 1);
    assert_complementary_bank(dst, 5, 12);
    assert_complementary_pads(dst, 7, 0, false);
    qtest_clock_step(dst, 12 * 125);
    assert_complementary_bank(dst, 3, 12);
    assert_complementary_pads(dst, 0, 6, true);
    g_assert_cmpuint(qtest_readw(dst, BASE + count[3]), ==, 5);
    g_assert_cmpuint(qtest_readw(dst, BASE + count[4]), ==, 3);
    qtest_quit(dst);
    qtest_quit(src);
    unlink(file);
    rmdir(dir);
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

#define POE 0x88900
#define PORT_PATH "/machine/mcu/port"

static void poe_input(QTestState *qts, unsigned name, bool level)
{
    qtest_set_irq_in(qts, PORT_PATH, "pin",
                     (name >> 4) * 8 + (name & 7), level);
}

static void poe_output0(QTestState *qts)
{
    select_pad(qts, 0x14, 3);
    set_io(qts, 0, 0, 5); /* Initially high, clear on compare. */
    qtest_writew(qts, BASE + tgr[0][0], 9);
    qtest_writeb(qts, POE + 0xb, 1);
    assert_pad(qts, 0x14, true, true);
}

static void test_poe_software(void)
{
    QTestState *qts = mtu_init();

    poe_output0(qts);
    /* GPIO stays driven even if its PFS names MTIOC0A. */
    qtest_writeb(qts, PORT_BASE + 0x20 + 14, 8);
    qtest_writeb(qts, PORT_BASE + 14, 8);
    qtest_writeb(qts, POE + 0xa, 2);
    assert_pad(qts, 0x14, false, false);
    assert_pad(qts, 0xe3, true, true);
    qtest_writeb(qts, POE + 0xa, 0); /* No preceding read. */
    assert_pad(qts, 0x14, false, false);
    g_assert_cmpuint(qtest_readb(qts, POE + 0xa), ==, 2);
    start_channel(qts, 0, true);
    qtest_clock_step(qts, 313);
    g_assert_cmpuint(qtest_readw(qts, BASE + count[0]), ==, 10);
    qtest_writeb(qts, POE + 0xa, 0);
    assert_pad(qts, 0x14, true, false); /* Waveform continued during cutoff. */
    qtest_writeb(qts, POE + 0xb, 15); /* Write-once selection stays 1. */
    g_assert_cmpuint(qtest_readb(qts, POE + 0xb), ==, 1);
    qtest_system_reset(qts);
    g_assert_cmpuint(qtest_readb(qts, POE + 0xb), ==, 0);
    qtest_quit(qts);
}

static void test_poe_pin(gconstpointer data)
{
    unsigned mode = GPOINTER_TO_UINT(data);
    static const unsigned div[] = { 1, 8, 16, 128 };
    QTestState *qts = mtu_init();
    unsigned cycles = mode ? div[mode] * 16 : 1;
    int64_t span = DIV_ROUND_UP(cycles * 125, 4);

    poe_output0(qts);
    poe_input(qts, 0x30, true);
    select_pad(qts, 0x30, 7);
    qtest_writew(qts, POE + 8, 0x300 | mode);
    qtest_clock_step(qts, div[mode] * 125);
    poe_input(qts, 0x30, false);
    qtest_clock_step(qts, span - 1);
    assert_pad(qts, 0x14, true, true);
    g_assert_cmpuint(qtest_readb(qts, IR(171)), ==, 0);
    qtest_clock_step(qts, 1);
    assert_pad(qts, 0x14, false, false);
    g_assert_cmphex(qtest_readw(qts, POE + 8), ==, 0x1300 | mode);
    g_assert_cmpuint(qtest_readb(qts, IR(171)), ==, 1);
    qtest_writew(qts, POE + 8, 0x300 | mode);
    if (mode) {
        assert_pad(qts, 0x14, false, false);
        poe_input(qts, 0x30, true);
        qtest_writew(qts, POE + 8, 0x300 | mode);
        assert_pad(qts, 0x14, false, false); /* High not yet sampled. */
        qtest_clock_step(qts, div[mode] * 125 / 4);
        qtest_writew(qts, POE + 8, 0x300 | mode);
    }
    assert_pad(qts, 0x14, true, true);
    g_assert_cmpuint(qtest_readb(qts, IR(171)), ==, 0);
    qtest_quit(qts);
}

static void test_poe_comparator(void)
{
    static const unsigned b[] = { 0x17, 0xb3, 0xb7, 0xc5, 0x22 };
    static const unsigned d[] = { 0x16, 0xb6, 0xc4, 0x23 };
    QTestState *qts = mtu_init();

    /* All alternate pads are GPIO outputs, high; internal outputs also high. */
    for (unsigned port = 0; port < 19; port++) {
        qtest_writeb(qts, PORT_BASE + 0x20 + port, 0xff);
        qtest_writeb(qts, PORT_BASE + port, 0xff);
    }
    set_io(qts, 3, 1, 5);
    set_io(qts, 3, 3, 5);
    qtest_writeb(qts, BASE + 0xa, 9);
    qtest_writeb(qts, POE + 0xc, 0x40);
    qtest_writew(qts, POE + 2, 0x300);
    qtest_clock_step(qts, 125);
    g_assert_cmphex(qtest_readw(qts, POE + 2), ==, 0x300);
    /* D-019's distinguishing case: internal inactive-high, GPIO P22/P23 low. */
    qtest_writeb(qts, PORT_BASE + 0x22, 0xf3);
    qtest_clock_step(qts, 31);
    g_assert_cmphex(qtest_readw(qts, POE + 2), ==, 0x300);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readw(qts, POE + 2), ==, 0x8300);
    g_assert_cmpuint(qtest_readb(qts, IR(170)), ==, 1);
    assert_pad(qts, 0x22, true, false);
    assert_pad(qts, 0x23, true, false);
    qtest_writew(qts, POE + 2, 0x300);
    g_assert_cmphex(qtest_readw(qts, POE + 2), ==, 0x8300);
    qtest_writeb(qts, PORT_BASE + 0x22, 0xfb); /* One input still active. */
    qtest_writew(qts, POE + 2, 0x300);
    g_assert_cmphex(qtest_readw(qts, POE + 2), ==, 0x8300);
    qtest_writeb(qts, PORT_BASE + 0x22, 0xff);
    qtest_writew(qts, POE + 2, 0x300);
    g_assert_cmphex(qtest_readw(qts, POE + 2), ==, 0x300);
    /* A latched comparator fault cuts off selected MTU pads only. */
    qtest_writeb(qts, PORT_BASE + 0x22, 0xf3);
    qtest_clock_step(qts, 63);
    g_assert_cmphex(qtest_readw(qts, POE + 2), ==, 0x8300);
    poe_input(qts, 0x22, false);
    poe_input(qts, 0x23, false);
    select_pad(qts, 0x22, 1);
    select_pad(qts, 0x23, 1);
    assert_pad(qts, 0x22, false, false);
    assert_pad(qts, 0x23, false, false);
    poe_input(qts, 0x22, true);
    poe_input(qts, 0x23, true);
    qtest_writew(qts, POE + 2, 0x300);
    assert_pad(qts, 0x22, true, true);
    assert_pad(qts, 0x23, true, true);
    qtest_writeb(qts, PORT_BASE + 0x62, 0);
    qtest_writeb(qts, PORT_BASE + 0x22, 0xff);
    /* Every alternate can supply its function's active level. */
    for (unsigned i = 0; i < G_N_ELEMENTS(b); i++) {
        for (unsigned j = 0; j < G_N_ELEMENTS(d); j++) {
            unsigned bp = b[i] >> 4, dp = d[j] >> 4;
            unsigned bv = qtest_readb(qts, PORT_BASE + 0x20 + bp);
            unsigned dv;

            qtest_writeb(qts, PORT_BASE + 0x20 + bp, bv & ~(1 << (b[i] & 7)));
            dv = qtest_readb(qts, PORT_BASE + 0x20 + dp);
            qtest_writeb(qts, PORT_BASE + 0x20 + dp, dv & ~(1 << (d[j] & 7)));
            qtest_clock_step(qts, 63);
            g_assert_cmphex(qtest_readw(qts, POE + 2), ==, 0x8300);
            qtest_writeb(qts, PORT_BASE + 0x20 + dp, dv);
            qtest_writeb(qts, PORT_BASE + 0x20 + bp, bv);
            qtest_writew(qts, POE + 2, 0x300);
            g_assert_cmphex(qtest_readw(qts, POE + 2), ==, 0x300);
        }
    }
    qtest_quit(qts);
}

static void test_poe_oscillator(gconstpointer data)
{
    bool strict_once = GPOINTER_TO_UINT(data);
    QTestState *qts = mtu_init();
    QDict *rsp;

    poe_output0(qts);
    qtest_writew(qts, POE + 0xe, 0x200);
    qtest_writeb(qts, 0x80040, 0x81);
    qtest_set_irq_in(qts, "/machine/mcu/system", "main-clock-failed", 0, 1);
    qtest_clock_step(qts, 999999);
    g_assert_cmpuint(qtest_readb(qts, 0x80041), ==, 0);
    qtest_clock_step(qts, 1);
    g_assert_cmpuint(qtest_readb(qts, 0x80041), ==, 1);
    g_assert_cmphex(qtest_readw(qts, POE + 0xe), ==, 0x1200);
    assert_pad(qts, 0x14, false, false);
    g_assert_cmpuint(qtest_readb(qts, IR(170)), ==, 0);
    rsp = qtest_qmp(qts, "{'execute':'qom-get','arguments':"
                    "{'path':'/machine/mcu/system','property':'ostdi-request'}}");
    g_assert_true(qdict_get_bool(rsp, "return"));
    qobject_unref(rsp);
    qtest_writew(qts, POE + 0xe, 0x200);
    g_assert_cmphex(qtest_readw(qts, POE + 0xe), ==, 0x1200);
    qtest_clock_step(qts, 312);
    qtest_writew(qts, POE + 0xe, 0x200);
    g_assert_cmphex(qtest_readw(qts, POE + 0xe), ==, 0x1200);
    qtest_clock_step(qts, 1);
    if (strict_once) {
        /* D-020: an OSTSTE clear is ignored; OSTSTF remains the release. */
        qtest_writew(qts, POE + 0xe, 0x1000);
        g_assert_cmphex(qtest_readw(qts, POE + 0xe), ==, 0x1200);
        assert_pad(qts, 0x14, false, false);
        qtest_writew(qts, POE + 0xe, 0x200);
        g_assert_cmphex(qtest_readw(qts, POE + 0xe), ==, 0x200);
        assert_pad(qts, 0x14, true, true);
        /*
         * OSTSTE stayed armed: after the documented recovery (clock
         * restored, OSTDF cleared), a second stop trips the output
         * again without any new ICSR3 write.
         */
        qtest_set_irq_in(qts, "/machine/mcu/system", "main-clock-failed",
                         0, 0);
        qtest_writeb(qts, 0x80040, 0);
        qtest_writeb(qts, 0x80041, 0);
        g_assert_cmpuint(qtest_readb(qts, 0x80041), ==, 0);
        qtest_writeb(qts, 0x80040, 0x81);
        qtest_set_irq_in(qts, "/machine/mcu/system", "main-clock-failed",
                         0, 1);
        qtest_clock_step(qts, 1000001);
        g_assert_cmpuint(qtest_readb(qts, 0x80041), ==, 1);
        g_assert_cmphex(qtest_readw(qts, POE + 0xe), ==, 0x1200);
        assert_pad(qts, 0x14, false, false);
        qtest_clock_step(qts, 1000); /* let the stop-cycle window pass */
        qtest_writew(qts, POE + 0xe, 0x200);
        g_assert_cmphex(qtest_readw(qts, POE + 0xe), ==, 0x200);
        assert_pad(qts, 0x14, true, true);
    } else {
        qtest_writew(qts, POE + 0xe, 0x200);
        g_assert_cmphex(qtest_readw(qts, POE + 0xe), ==, 0x200);
        assert_pad(qts, 0x14, true, true);
    }
    qtest_set_irq_in(qts, "/machine/mcu/system", "main-clock-failed", 0, 0);
    qtest_writeb(qts, 0x80040, 0);
    /* OSTDF locks OSTDE. */
    g_assert_cmpuint(qtest_readb(qts, 0x80040), ==, 0x80);
    qtest_writeb(qts, 0x80041, 0);
    g_assert_cmpuint(qtest_readb(qts, 0x80041), ==, 0);
    qtest_system_reset(qts);
    if (strict_once) {
        /* A first write of 0 also consumes the one modification. */
        qtest_writew(qts, POE + 0xe, 0x1000);
        g_assert_cmphex(qtest_readw(qts, POE + 0xe), ==, 0);
        qtest_writew(qts, POE + 0xe, 0x200);
        g_assert_cmphex(qtest_readw(qts, POE + 0xe), ==, 0);
        qtest_system_reset(qts);
    }
    qtest_writew(qts, POE + 0xe, 0x200);
    g_assert_cmphex(qtest_readw(qts, POE + 0xe), ==, 0x200);
    qtest_quit(qts);
}

static void test_poe_routes(void)
{
    static const struct { unsigned pin, input; } routes[] = {
        { 0xc4, 0 }, { 0xa3, 0 }, { 0x11, 0 },
        { 0xb5, 1 }, { 0x10, 1 },
        { 0xa6, 2 }, { 0x04, 2 }, { 0x56, 2 },
        { 0xb3, 3 }, { 0xe0, 3 }, { 0x02, 3 }, { 0xd4, 3 },
        { 0x17, 4 }, { 0x30, 4 }, { 0xe3, 4 }, { 0xd3, 4 },
    };
    QTestState *qts = mtu_init();

    poe_output0(qts);
    select_pad(qts, 0x22, 1);
    set_io(qts, 3, 1, 5);
    qtest_writeb(qts, BASE + 0xa, 1);
    qtest_writeb(qts, POE + 0xc, 0x40);
    qtest_writew(qts, POE, 0x100);
    qtest_writew(qts, POE + 8, 0x300);
    for (unsigned i = 0; i < G_N_ELEMENTS(routes); i++) {
        unsigned pin = routes[i].pin, input = routes[i].input;
        unsigned addr = POE + (input == 4 ? 8 : 0);
        unsigned flag = 1 << (input == 4 ? 12 : 12 + input);
        unsigned enable = input == 4 ? 0x300 : 0x100;
        unsigned irq = input == 4 ? 171 : 170;
        unsigned pad = input == 4 ? 0x14 : 0x22;

        poe_input(qts, pin, true);
        select_pad(qts, pin, 7);
        qtest_clock_step(qts, 125);
        poe_input(qts, pin, false);
        qtest_clock_step(qts, 125);
        g_assert_cmphex(qtest_readw(qts, addr), ==, enable | flag);
        assert_pad(qts, pad, false, false);
        g_assert_cmpuint(qtest_readb(qts, IR(irq)), ==, 1);
        qtest_writew(qts, addr, enable);
        assert_pad(qts, pad, true, true);
        g_assert_cmpuint(qtest_readb(qts, IR(irq)), ==, 0);
        /* PFS qualification: a low GPIO cannot request POE. */
        unsigned pmr = PORT_BASE + 0x60 + (pin >> 4);

        qtest_writeb(qts, pmr, qtest_readb(qts, pmr) & ~(1 << (pin & 7)));
        qtest_clock_step(qts, 125);
        g_assert_cmphex(qtest_readw(qts, addr), ==, enable);
    }
    qtest_quit(qts);
}

static void test_poe_groups(void)
{
    static const unsigned positive[] = { 0x22, 0x24, 0x30 };
    static const unsigned negative[] = { 0x23, 0x25, 0x31 };

    for (unsigned group = 0; group < 3; group++) {
        QTestState *qts = mtu_init();

        for (unsigned ch = 0; ch < 5; ch++) {
            if (ch == 1 || ch == 2) {
                continue;
            }
            for (unsigned gr = 0; gr < 4; gr++) {
                select_pad(qts, output_pad[ch][gr],
                           ch == 0 && gr == 0 ? 3 : 1);
                set_io(qts, ch, gr, 5);
            }
        }
        qtest_writeb(qts, BASE + 0xa, 0x3f);
        qtest_writeb(qts, POE + 0xb, 15);
        qtest_writeb(qts, POE + 0xc, 0x40 >> group);
        qtest_writeb(qts, POE + 0xa, 3);
        for (unsigned i = 0; i < 4; i++) {
            assert_pad(qts, output_pad[0][i], false, false);
        }
        for (unsigned i = 0; i < 3; i++) {
            assert_pad(qts, positive[i], i != group, i != group);
            assert_pad(qts, negative[i], i != group, i != group);
        }
        qtest_readb(qts, POE + 0xa);
        qtest_writeb(qts, POE + 0xa, 0);
        for (unsigned i = 0; i < 3; i++) {
            assert_pad(qts, positive[i], true, true);
            assert_pad(qts, negative[i], true, true);
        }
        qtest_quit(qts);
    }
}

static void test_poe_clock_glitch(void)
{
    QTestState *qts = mtu_init();

    poe_output0(qts);
    poe_input(qts, 0x30, true);
    select_pad(qts, 0x30, 7);
    /* Independent byte-wide write-once fields. */
    qtest_writeb(qts, POE + 8, 1);
    qtest_writeb(qts, POE + 9, 3);
    g_assert_cmphex(qtest_readw(qts, POE + 8), ==, 0x301);
    qtest_clock_step(qts, 250);
    poe_input(qts, 0x30, false);
    qtest_clock_step(qts, 15 * 250);
    assert_pad(qts, 0x14, true, true);
    poe_input(qts, 0x30, true);
    qtest_clock_step(qts, 250); /* A sampled high resets the low streak. */
    poe_input(qts, 0x30, false);
    qtest_clock_step(qts, 15 * 250);
    assert_pad(qts, 0x14, true, true);
    /* Halve PCLKB; MTU module stop does not stop POE sampling. */
    qtest_writel(qts, SCKCR, 0x100);
    qtest_writel(qts, MSTPCRA, 0xffffffff);
    qtest_clock_step(qts, 499);
    assert_pad(qts, 0x14, true, true);
    qtest_clock_step(qts, 1);
    assert_pad(qts, 0x14, false, false);
    g_assert_cmphex(qtest_readw(qts, POE + 8), ==, 0x1301);
    qtest_quit(qts);
}

static void test_poe_migration(gconstpointer data)
{
    bool oscillator = GPOINTER_TO_UINT(data);
    QTestState *src = mtu_init(), *dst;
    g_autofree char *dir = g_dir_make_tmp("rx113-poe-XXXXXX", NULL);
    g_autofree char *file = g_strdup_printf("%s/state", dir);
    g_autofree char *uri = g_strdup_printf("file:%s", file);
    int64_t now;

    g_assert_nonnull(dir);
    poe_output0(src);
    if (oscillator) {
        qtest_writew(src, POE + 0xe, 0x200);
        qtest_writeb(src, 0x80040, 0x81);
        qtest_set_irq_in(src, "/machine/mcu/system", "main-clock-failed", 0, 1);
        now = qtest_clock_step(src, 500000);
    } else {
        poe_input(src, 0x30, false);
        select_pad(src, 0x30, 7);
        qtest_writew(src, POE + 8, 0x301);
        now = qtest_clock_step(src, 8 * 250 + 15);
        qtest_writeb(src, POE + 0xa, 2);
        qtest_readb(src, POE + 0xa); /* Save read-before-clear qualification. */
    }
    qtest_qmp_assert_success(src, "{'execute':'migrate',"
                            "'arguments':{'uri':%s}}", uri);
    migration_wait(src);
    dst = qtest_init("-M rx113-r5f51138 -incoming defer");
    qtest_clock_set(dst, now);
    qtest_qmp_assert_success(dst, "{'execute':'migrate-incoming',"
                            "'arguments':{'uri':%s}}", uri);
    migration_wait(dst);
    if (oscillator) {
        qtest_clock_step(dst, 499999);
        assert_pad(dst, 0x14, true, true);
        qtest_clock_step(dst, 1);
        assert_pad(dst, 0x14, false, false);
        g_assert_cmphex(qtest_readw(dst, POE + 0xe), ==, 0x1200);
        qtest_clock_step(dst, 1000);
        /* D-020: the write-once latch migrated; the clear is ignored. */
        qtest_writew(dst, POE + 0xe, 0x1000);
        g_assert_cmphex(qtest_readw(dst, POE + 0xe), ==, 0x1200);
        assert_pad(dst, 0x14, false, false);
        /* OSTSTF clear remains the release, on the destination too. */
        qtest_writew(dst, POE + 0xe, 0x200);
        g_assert_cmphex(qtest_readw(dst, POE + 0xe), ==, 0x200);
        assert_pad(dst, 0x14, true, true);
    } else {
        assert_pad(dst, 0x14, false, false);
        qtest_writeb(dst, POE + 0xa, 0);
        assert_pad(dst, 0x14, true, true);
        qtest_clock_step(dst, 8 * 250 - 16);
        assert_pad(dst, 0x14, true, true);
        qtest_clock_step(dst, 1);
        assert_pad(dst, 0x14, false, false);
        g_assert_cmphex(qtest_readw(dst, POE + 8), ==, 0x1301);
        poe_input(dst, 0x30, true);
        qtest_clock_step(dst, 250);
        qtest_writew(dst, POE + 8, 0x301);
        assert_pad(dst, 0x14, true, true);
    }
    qtest_quit(dst);
    qtest_quit(src);
    unlink(file);
    rmdir(dir);
}

static void test_poe_package64(void)
{
    QTestState *qts = qtest_init("-M rx113-r5f51138 "
                               "-global rx113-port.package-pins=64");

    for (unsigned port = 0; port < 19; port++) {
        qtest_writeb(qts, PORT_BASE + 0x20 + port, 0xff);
        qtest_writeb(qts, PORT_BASE + port, 0xff);
    }
    qtest_writeb(qts, POE + 0xc, 0x40);
    qtest_writew(qts, POE + 2, 0x300);
    /* P22/P23 and the P11 POE0 route are unbonded in this package. */
    poe_input(qts, 0x22, false);
    poe_input(qts, 0x23, false);
    poe_input(qts, 0x11, false);
    select_pad(qts, 0x11, 7);
    qtest_clock_step(qts, 10000);
    g_assert_cmphex(qtest_readw(qts, POE + 2), ==, 0x300);
    g_assert_cmphex(qtest_readw(qts, POE), ==, 0);
    qtest_writeb(qts, PORT_BASE + 0x21, 0x3f); /* Bonded P16/P17. */
    qtest_clock_step(qts, 10000);
    g_assert_cmphex(qtest_readw(qts, POE + 2), ==, 0x8300);
    qtest_quit(qts);
}

static void test_poe_system(void)
{
    QTestState *qts = mtu_init();

    qtest_writeb(qts, 0x800a2, 0);
    qtest_writeb(qts, 0x80032, 0);
    qtest_clock_step(qts, 500);
    qtest_writew(qts, 0x80026, 0x200); /* 16 MHz main oscillator. */
    qtest_writew(qts, 0x803fe, 0xa500);
    qtest_writeb(qts, 0x80040, 0x81);
    g_assert_cmpuint(qtest_readb(qts, 0x80040), ==, 0);
    qtest_writew(qts, 0x803fe, 0xa503);
    /* Detection without POE/NMI notification. */
    qtest_writeb(qts, 0x80040, 0x80);
    qtest_writeb(qts, 0x80034, 1);
    g_assert_cmpuint(qtest_readb(qts, 0x80034), ==, 0);
    qtest_set_irq_in(qts, "/machine/mcu/system", "main-clock-failed", 0, 1);
    qtest_clock_step(qts, 1000000);
    g_assert_cmpuint(qtest_readb(qts, 0x80041), ==, 1);
    g_assert_cmpuint(qtest_readw(qts, POE + 0xe), ==, 0);
    qtest_writew(qts, BASE + count[0], 0);
    start_channel(qts, 0, true);
    qtest_clock_step(qts, 1000);
    g_assert_cmpuint(qtest_readw(qts, BASE + count[0]), ==, 4);
    qtest_writeb(qts, 0x80041, 0); /* Cannot clear while CKSEL is main. */
    g_assert_cmpuint(qtest_readb(qts, 0x80041), ==, 1);
    qtest_writew(qts, 0x80026, 0x100);
    qtest_set_irq_in(qts, "/machine/mcu/system", "main-clock-failed", 0, 0);
    qtest_writeb(qts, 0x80041, 0);
    g_assert_cmpuint(qtest_readb(qts, 0x80041), ==, 0);
    qtest_writeb(qts, 0x80040, 0x81);
    qtest_set_irq_in(qts, "/machine/mcu/system", "main-clock-failed", 0, 1);
    qtest_clock_step(qts, 500000);
    qtest_set_irq_in(qts, "/machine/mcu/system", "main-clock-failed", 0, 0);
    qtest_clock_step(qts, 1000000);
    g_assert_cmpuint(qtest_readb(qts, 0x80041), ==, 0);
    qtest_quit(qts);
}

static void test_poe_compare_pairs(void)
{
    static const unsigned pads[3][2] = {
        { 0x22, 0x23 }, { 0x24, 0x25 }, { 0x30, 0x31 },
    };
    QTestState *qts = mtu_init();

    for (unsigned port = 0; port < 19; port++) {
        qtest_writeb(qts, PORT_BASE + 0x20 + port, 0xff);
        qtest_writeb(qts, PORT_BASE + port, 0xff);
    }
    qtest_writeb(qts, POE + 0xc, 0x70);
    qtest_writew(qts, POE + 2, 0x300);
    for (unsigned pair = 0; pair < 3; pair++) {
        unsigned port = pads[pair][0] >> 4;
        unsigned a = 1 << (pads[pair][0] & 7);
        unsigned b = 1 << (pads[pair][1] & 7);

        qtest_writeb(qts, PORT_BASE + 0x20 + port, 0xff & ~a);
        qtest_clock_step(qts, 125);
        g_assert_cmphex(qtest_readw(qts, POE + 2), ==, 0x300);
        qtest_writeb(qts, PORT_BASE + 0x20 + port, 0xff & ~a & ~b);
        qtest_clock_step(qts, 125);
        g_assert_cmphex(qtest_readw(qts, POE + 2), ==, 0x8300);
        qtest_writeb(qts, PORT_BASE + 0x20 + port, 0xff);
        qtest_writew(qts, POE + 2, 0x300);
        g_assert_cmphex(qtest_readw(qts, POE + 2), ==, 0x300);
    }
    /* ASEL must not hide a fault from the PFS-independent comparator. */
    poe_input(qts, 0xe2, true);
    select_pad(qts, 0xe2, 0x80);
    qtest_writeb(qts, PORT_BASE + 0x22, 0xdf); /* MTIOC4C active. */
    qtest_clock_step(qts, 125);
    g_assert_cmphex(qtest_readw(qts, POE + 2), ==, 0x300);
    poe_input(qts, 0xe2, false); /* MTIOC4A alternate analog pad active. */
    qtest_clock_step(qts, 125);
    g_assert_cmphex(qtest_readw(qts, POE + 2), ==, 0x8300);
    qtest_writeb(qts, PORT_BASE + 0x22, 0xff);
    poe_input(qts, 0xe2, true);
    qtest_writew(qts, POE + 2, 0x300);
    /* Global active-high polarity: all GPIO pads low are inactive. */
    qtest_writeb(qts, PORT_BASE + 0x6e, 0);
    qtest_writeb(qts, PORT_BASE + 0x140 + 14 * 8 + 2, 0);
    for (unsigned port = 0; port < 19; port++) {
        qtest_writeb(qts, PORT_BASE + 0x20 + port, 0);
    }
    qtest_writeb(qts, BASE + 0xe, 3);
    qtest_clock_step(qts, 125);
    g_assert_cmphex(qtest_readw(qts, POE + 2), ==, 0x300);
    qtest_writeb(qts, PORT_BASE + 0x22, 12);
    qtest_clock_step(qts, 125);
    g_assert_cmphex(qtest_readw(qts, POE + 2), ==, 0x8300);
    qtest_writeb(qts, PORT_BASE + 0x22, 0);
    qtest_writew(qts, POE + 2, 0x300);
    g_assert_cmphex(qtest_readw(qts, POE + 2), ==, 0x300);
    qtest_quit(qts);
    /* TOCS selects each pair's own TOCR2 polarity, not TOCR1 OLSP/N. */
    for (unsigned pair = 0; pair < 3; pair++) {
        qts = mtu_init();
        for (unsigned port = 0; port < 19; port++) {
            qtest_writeb(qts, PORT_BASE + port, 0xff);
        }
        qtest_writeb(qts, BASE + 0xe, 4);
        qtest_writeb(qts, BASE + 0xf, 3 << (pair * 2));
        qtest_writeb(qts, POE + 0xc, 0x40 >> pair);
        qtest_writew(qts, POE + 2, 0x300);
        qtest_clock_step(qts, 125);
        g_assert_cmphex(qtest_readw(qts, POE + 2), ==, 0x300);
        unsigned port = pads[pair][0] >> 4;
        unsigned mask = (1 << (pads[pair][0] & 7)) |
                        (1 << (pads[pair][1] & 7));

        qtest_writeb(qts, PORT_BASE + 0x20 + port, mask);
        qtest_clock_step(qts, 125);
        g_assert_cmphex(qtest_readw(qts, POE + 2), ==, 0x8300);
        qtest_quit(qts);
    }
}

static void test_poe_registers(void)
{
    static const struct { unsigned addr, size, value; } regs[] = {
        { 0x88900, 2, 0x1ff }, { 0x88902, 2, 0x300 },
        { 0x88908, 2, 0x303 }, { 0x8890a, 1, 3 },
        { 0x8890b, 1, 15 }, { 0x8890c, 1, 0x70 },
        { 0x8890e, 2, 0x200 },
    };
    QTestState *qts = mtu_init();

    for (unsigned i = 0; i < G_N_ELEMENTS(regs); i++) {
        if (regs[i].size == 1) {
            qtest_writeb(qts, regs[i].addr, 0xff);
            g_assert_cmphex(qtest_readb(qts, regs[i].addr), ==, regs[i].value);
        } else {
            qtest_writew(qts, regs[i].addr, 0xffff);
            g_assert_cmphex(qtest_readw(qts, regs[i].addr), ==, regs[i].value);
        }
    }
    qtest_writew(qts, POE + 4, 0xffff);
    qtest_writew(qts, POE + 6, 0xffff);
    qtest_writeb(qts, POE + 0xd, 0xff);
    g_assert_cmpuint(qtest_readw(qts, POE + 4), ==, 0);
    g_assert_cmpuint(qtest_readw(qts, POE + 6), ==, 0);
    g_assert_cmpuint(qtest_readb(qts, POE + 0xd), ==, 0);
    qtest_writew(qts, POE, 0);
    qtest_writew(qts, POE + 2, 0);
    qtest_writew(qts, POE + 8, 0);
    qtest_writeb(qts, POE + 0xb, 0);
    qtest_writeb(qts, POE + 0xc, 0);
    g_assert_cmphex(qtest_readw(qts, POE), ==, 0xff);
    g_assert_cmphex(qtest_readw(qts, POE + 2), ==, 0x200);
    g_assert_cmphex(qtest_readw(qts, POE + 8), ==, 0x203);
    g_assert_cmphex(qtest_readb(qts, POE + 0xb), ==, 15);
    g_assert_cmphex(qtest_readb(qts, POE + 0xc), ==, 0x70);
    qtest_system_reset(qts);
    for (unsigned i = 0; i < 16; i++) {
        g_assert_cmpuint(qtest_readb(qts, POE + i), ==, 0);
    }
    qtest_quit(qts);
}

/*
 * Input capture from MTIOC pad edges (Issue #68). P04 is MTIOC0A,
 * P13 is MTIOC0B and P20 is MTIOC1A, all at PSEL=1; the PORT pin
 * inputs are port * 8 + bit. TCR=1 gives 125 ns per count.
 */
#define TIOR0H (BASE + 0x102)
#define TIOR1 (BASE + 0x182)
#define TIOR2 (BASE + 0x202)
#define TICCR (BASE + 0x190)
#define PIN_MTIOC0A 4
#define PIN_MTIOC0B 11
#define PIN_MTIOC1A 16

static void drive_pin(QTestState *qts, unsigned pin, unsigned level)
{
    qtest_set_irq_in(qts, "/machine/mcu/port", "pin", pin, level);
}

static void test_capture_edges(void)
{
    QTestState *qts = mtu_init();

    qtest_writeb(qts, BASE + tcr[0], 1); /* PCLKB/4 */
    qtest_writeb(qts, BASE + tier[0], 1); /* TGIEA */
    select_pad(qts, 0x04, 1); /* P04 = MTIOC0A */
    drive_pin(qts, PIN_MTIOC0A, 0);
    qtest_writeb(qts, TIOR0H, 0x08); /* TGRA: rising-edge capture */
    start_channel(qts, 0, true);

    qtest_clock_step(qts, 125 * 40);
    drive_pin(qts, PIN_MTIOC0A, 1);
    g_assert_cmpuint(qtest_readw(qts, BASE + tgr[0][0]), ==, 40);
    g_assert_cmpuint(qtest_readb(qts, IR(vector[0])), ==, 1);
    qtest_writeb(qts, IR(vector[0]), 0);

    /* A falling edge does not capture in rising-edge mode. */
    qtest_clock_step(qts, 125 * 10);
    drive_pin(qts, PIN_MTIOC0A, 0);
    g_assert_cmpuint(qtest_readw(qts, BASE + tgr[0][0]), ==, 40);
    drive_pin(qts, PIN_MTIOC0A, 1);
    g_assert_cmpuint(qtest_readw(qts, BASE + tgr[0][0]), ==, 50);

    /* Falling-edge mode: only the fall captures. */
    qtest_writeb(qts, TIOR0H, 0x09);
    qtest_clock_step(qts, 125 * 10);
    drive_pin(qts, PIN_MTIOC0A, 0);
    g_assert_cmpuint(qtest_readw(qts, BASE + tgr[0][0]), ==, 60);
    drive_pin(qts, PIN_MTIOC0A, 1);
    g_assert_cmpuint(qtest_readw(qts, BASE + tgr[0][0]), ==, 60);

    /* Both-edges mode; a stopped channel captures nothing. */
    qtest_writeb(qts, TIOR0H, 0x0a);
    qtest_clock_step(qts, 125 * 10);
    drive_pin(qts, PIN_MTIOC0A, 0);
    g_assert_cmpuint(qtest_readw(qts, BASE + tgr[0][0]), ==, 70);
    start_channel(qts, 0, false);
    drive_pin(qts, PIN_MTIOC0A, 1);
    qtest_clock_step(qts, 125 * 10);
    drive_pin(qts, PIN_MTIOC0A, 0);
    g_assert_cmpuint(qtest_readw(qts, BASE + tgr[0][0]), ==, 70);
    qtest_quit(qts);
}

/* Figure 20.11 shape: TGRB capture designated as the clear source. */
static void test_capture_clear(void)
{
    QTestState *qts = mtu_init();

    qtest_writeb(qts, BASE + tcr[0], 1 | (2 << 5)); /* /4, clear by B */
    select_pad(qts, 0x04, 1);
    select_pad(qts, 0x13, 1); /* P13 = MTIOC0B */
    drive_pin(qts, PIN_MTIOC0A, 0);
    drive_pin(qts, PIN_MTIOC0B, 1);
    qtest_writeb(qts, TIOR0H, 0x98); /* A rising capture, B falling */
    start_channel(qts, 0, true);

    qtest_clock_step(qts, 125 * 24);
    drive_pin(qts, PIN_MTIOC0A, 1);
    g_assert_cmpuint(qtest_readw(qts, BASE + tgr[0][0]), ==, 24);
    qtest_clock_step(qts, 125 * 8);
    drive_pin(qts, PIN_MTIOC0B, 0);
    g_assert_cmpuint(qtest_readw(qts, BASE + tgr[0][1]), ==, 32);
    g_assert_cmpuint(qtest_readw(qts, BASE + count[0]), ==, 0);
    qtest_clock_step(qts, 125 * 4);
    g_assert_cmpuint(qtest_readw(qts, BASE + count[0]), ==, 4);
    qtest_quit(qts);
}

/* TICCR cross-capture: MTIOC1A edges also capture into MTU2.TGRA. */
static void test_capture_ticcr(void)
{
    QTestState *qts = mtu_init();

    qtest_writeb(qts, BASE + tcr[1], 1);
    qtest_writeb(qts, BASE + tcr[2], 1);
    qtest_writeb(qts, TIOR1, 0x08);
    qtest_writeb(qts, TIOR2, 0x08);
    select_pad(qts, 0x20, 1); /* P20 = MTIOC1A */
    drive_pin(qts, PIN_MTIOC1A, 0);
    qtest_writeb(qts, START, (1 << start_bit[1]) | (1 << start_bit[2]));

    qtest_clock_step(qts, 125 * 20);
    drive_pin(qts, PIN_MTIOC1A, 1);
    g_assert_cmpuint(qtest_readw(qts, BASE + tgr[1][0]), ==, 20);
    /* Untouched: TGR reads the model's 0xffff idle value. */
    g_assert_cmpuint(qtest_readw(qts, BASE + tgr[2][0]), ==, 0xffff);

    qtest_writeb(qts, TICCR, 1); /* I1AE */
    g_assert_cmpuint(qtest_readb(qts, TICCR), ==, 1);
    drive_pin(qts, PIN_MTIOC1A, 0);
    qtest_clock_step(qts, 125 * 10);
    drive_pin(qts, PIN_MTIOC1A, 1);
    g_assert_cmpuint(qtest_readw(qts, BASE + tgr[1][0]), ==, 30);
    g_assert_cmpuint(qtest_readw(qts, BASE + tgr[2][0]), ==, 30);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/rx113-mtu/mtu5-capture-clocks",
                   test_mtu5_capture_clocks);
    qtest_add_func("/rx113-mtu/mtu5-measurement-routes",
                   test_mtu5_measurement_routes);
    qtest_add_func("/rx113-mtu/mtu5-compensation-migration",
                   test_mtu5_compensation_migration);
    qtest_add_func("/rx113-mtu/mtu5-measurement", test_mtu5_measurement);
    for (unsigned md = 13; md <= 15; md++) {
        for (unsigned low = 0; low < 2; low++) {
            g_autofree char *name = g_strdup_printf(
                "/rx113-mtu/mtu5-compensation-%u-%u", md, low);

            qtest_add_data_func(name, GUINT_TO_POINTER(md | (low << 4)),
                                test_mtu5_compensation);
        }
    }
    qtest_add_func("/rx113-mtu/complementary-tgcr-migration",
                   test_complementary_tgcr_migration);
    qtest_add_func("/rx113-mtu/complementary-no-dead-time",
                   test_complementary_no_dead_time);
    qtest_add_data_func("/rx113-mtu/reset-pwm-tgcr",
                        GUINT_TO_POINTER(1), test_tgcr);
    qtest_add_func("/rx113-mtu/twcr-migration", test_twcr_migration);
    qtest_add_func("/rx113-mtu/twcr-register", test_twcr_register);
    qtest_add_func("/rx113-mtu/complementary-cce", test_complementary_cce);
    qtest_add_func("/rx113-mtu/complementary-sync", test_complementary_sync);
    qtest_add_data_func("/rx113-mtu/complementary-tgcr",
                        GUINT_TO_POINTER(0), test_tgcr);
    qtest_add_func("/rx113-mtu/complementary-arbitration",
                   test_complementary_arbitration);
    qtest_add_func("/rx113-mtu/complementary-duty-irq",
                   test_complementary_duty_irq);
    qtest_add_func("/rx113-mtu/complementary-duty-pending",
                   test_complementary_duty_pending);
    qtest_add_func("/rx113-mtu/complementary-duty-elc",
                   test_complementary_duty_elc);
    qtest_add_func("/rx113-mtu/complementary-polarity",
                   test_complementary_polarity);
    qtest_add_func("/rx113-mtu/complementary-waveform",
                   test_complementary_waveform);
    qtest_add_func("/rx113-mtu/complementary-count", test_complementary_count);
    for (unsigned md = 13; md <= 15; md++) {
        g_autofree char *name = g_strdup_printf(
            "/rx113-mtu/complementary-buffers-%u", md);

        qtest_add_data_func(name, GUINT_TO_POINTER(md),
                            test_complementary_buffers);
    }
    qtest_add_func("/rx113-mtu/complementary-registers",
                   test_complementary_registers);
    qtest_add_func("/rx113-mtu/complementary-buffer-enable",
                   test_complementary_buffer_enable);
    qtest_add_func("/rx113-mtu/complementary-cycle", test_complementary_cycle);
    qtest_add_func("/rx113-mtu/complementary-clock", test_complementary_clock);
    qtest_add_func("/rx113-mtu/complementary-trough-elc",
                   test_complementary_trough_elc);
    qtest_add_func("/rx113-mtu/complementary-skip-migration",
                   test_complementary_skip_migration);
    qtest_add_func("/rx113-mtu/complementary-skip-gates",
                   test_complementary_skip_gates);
    qtest_add_func("/rx113-mtu/complementary-skip", test_complementary_skip);
    qtest_add_func("/rx113-mtu/complementary-skip-buffers",
                   test_complementary_skip_buffers);
    qtest_add_func("/rx113-mtu/complementary-migration",
                   test_complementary_migration);
    qtest_add_func("/rx113-mtu/reset-pwm-waveform", test_reset_pwm_waveform);
    qtest_add_func("/rx113-mtu/reset-pwm-buffers", test_reset_pwm_buffers);
    qtest_add_func("/rx113-mtu/reset-pwm-polarity", test_reset_pwm_polarity);
    qtest_add_func("/rx113-mtu/reset-pwm-coincident",
                   test_reset_pwm_coincident);
    qtest_add_func("/rx113-mtu/reset-pwm-clock", test_reset_pwm_clock);
    qtest_add_func("/rx113-mtu/reset-pwm-zero", test_reset_pwm_zero);
    qtest_add_func("/rx113-mtu/reset-pwm-migration", test_reset_pwm_migration);
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
    qtest_add_func("/rx113-mtu/poe-software", test_poe_software);
    qtest_add_func("/rx113-mtu/poe-comparator", test_poe_comparator);
    for (unsigned i = 0; i < 4; i++) {
        g_autofree char *name = g_strdup_printf("/rx113-mtu/poe-pin-%u", i);

        qtest_add_data_func(name, GUINT_TO_POINTER(i), test_poe_pin);
    }
    qtest_add_data_func("/rx113-mtu/poe-ostste-once", GUINT_TO_POINTER(1),
                        test_poe_oscillator);
    qtest_add_data_func("/rx113-mtu/poe-oststf-release", GUINT_TO_POINTER(0),
                        test_poe_oscillator);
    qtest_add_func("/rx113-mtu/poe-routes", test_poe_routes);
    qtest_add_func("/rx113-mtu/poe-groups", test_poe_groups);
    qtest_add_func("/rx113-mtu/poe-clock-glitch", test_poe_clock_glitch);
    qtest_add_data_func("/rx113-mtu/poe-migration-sampling",
                        GUINT_TO_POINTER(0), test_poe_migration);
    qtest_add_data_func("/rx113-mtu/poe-migration-oscillator",
                        GUINT_TO_POINTER(1), test_poe_migration);
    qtest_add_func("/rx113-mtu/capture-edges", test_capture_edges);
    qtest_add_func("/rx113-mtu/capture-clear", test_capture_clear);
    qtest_add_func("/rx113-mtu/capture-ticcr", test_capture_ticcr);
    qtest_add_func("/rx113-mtu/poe-package64", test_poe_package64);
    qtest_add_func("/rx113-mtu/poe-system", test_poe_system);
    qtest_add_func("/rx113-mtu/poe-compare-pairs", test_poe_compare_pairs);
    qtest_add_func("/rx113-mtu/poe-registers", test_poe_registers);
    return g_test_run();
}
