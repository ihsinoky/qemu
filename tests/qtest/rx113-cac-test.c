/*
 * RX113 CAC tests (R01UH0448EJ0120 Rev.1.20, section 10; D-009).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define CAC 0x8b000
#define CR0 CAC
#define CR1 (CAC + 1)
#define CR2 (CAC + 2)
#define ICR (CAC + 3)
#define STR (CAC + 4)
#define UPPER (CAC + 6)
#define LOWER (CAC + 8)
#define BUFFER (CAC + 10)
#define MSTPCRC 0x80018
#define STOP_C 0x7ffffffeU
#define RUN_C (STOP_C & ~(1U << 19))
#define CAC_PATH "/machine/mcu/cac"
#define PORT_PATH "/machine/mcu/port"

static QTestState *cac_init(void)
{
    QTestState *qts = qtest_init("-M rx113-r5f51138");

    qtest_writew(qts, 0x803fe, 0xa503);
    qtest_writel(qts, MSTPCRC, RUN_C);
    return qts;
}

static void input(QTestState *qts, bool level)
{
    qtest_set_irq_in(qts, CAC_PATH, "cacref", 0, level);
}

static void setup(QTestState *qts, unsigned cr1, unsigned cr2,
                  unsigned lower, unsigned upper)
{
    qtest_writeb(qts, CR0, 0);
    input(qts, false);
    qtest_writeb(qts, ICR, 0x70);
    qtest_writeb(qts, CR1, cr1);
    qtest_writeb(qts, CR2, cr2);
    qtest_writew(qts, LOWER, lower);
    qtest_writew(qts, UPPER, upper);
    qtest_writeb(qts, CR0, 1);
}

static void rising(QTestState *qts)
{
    input(qts, false);
    input(qts, true);
}

static void test_measure(void)
{
    QTestState *qts = cac_init();
    static const unsigned div[] = { 1, 4, 8, 32 };

    for (unsigned d = 0; d < 4; d++) {
        for (unsigned edges = 0; edges < 3; edges++) {
            for (unsigned result = 0; result < 3; result++) {
                unsigned lo = result == 2 ? 17 : 16;
                unsigned hi = result == 1 ? 15 : 16;

                /* LOCO target (4 MHz), bypassed external reference. */
                setup(qts, 7 | (d << 4) | (edges << 6), 0, lo, hi);
                if (edges == 1) {
                    input(qts, true); /* Invalid rising edge. */
                    qtest_clock_step(qts, 1000);
                    input(qts, false);
                } else {
                    input(qts, true);
                }
                g_assert_cmpuint(qtest_readb(qts, STR), ==, 0);
                qtest_clock_step(qts, 4000 * div[d]);
                if (edges == 0) {
                    rising(qts);
                } else if (edges == 1) {
                    input(qts, true);
                    input(qts, false);
                } else {
                    input(qts, false);
                }
                g_assert_cmpuint(qtest_readw(qts, BUFFER), ==, 16);
                g_assert_cmpuint(qtest_readb(qts, STR), ==, result ? 3 : 2);
                qtest_writeb(qts, ICR, 0x70);
                qtest_clock_step(qts, 4000 * div[d]);
                if (edges == 0) {
                    rising(qts);
                } else if (edges == 1) {
                    input(qts, true);
                    input(qts, false);
                } else {
                    input(qts, true);
                }
                g_assert_cmpuint(qtest_readw(qts, BUFFER), ==, 16);
                g_assert_cmpuint(qtest_readb(qts, STR), ==, result ? 3 : 2);
            }
        }
    }
    qtest_quit(qts);
}

static void test_intervals(void)
{
    QTestState *qts = cac_init();
    static const unsigned counts[] = { 0x7fff, 0xbfff, 0x3fff };

    /* Figure 10.2: in range, above the upper limit, below the lower limit. */
    setup(qts, 7, 0, 0x5555, 0xaaaa);
    input(qts, true);
    g_assert_cmpuint(qtest_readb(qts, STR), ==, 0);
    for (unsigned i = 0; i < ARRAY_SIZE(counts); i++) {
        qtest_clock_step(qts, counts[i] * 250);
        rising(qts);
        g_assert_cmpuint(qtest_readw(qts, BUFFER), ==, counts[i]);
        g_assert_cmpuint(qtest_readb(qts, STR), ==, i ? 3 : 2);
        qtest_writeb(qts, ICR, 0x70);
        g_assert_cmpuint(qtest_readb(qts, STR), ==, 0);
    }
    /* Discard the partial target cycle along with each captured count. */
    setup(qts, 7, 0, 16, 16);
    input(qts, true);
    for (unsigned i = 0; i < 8; i++) {
        qtest_clock_step(qts, 4125);
        rising(qts);
        g_assert_cmpuint(qtest_readw(qts, BUFFER), ==, 16);
        g_assert_cmpuint(qtest_readb(qts, STR), ==, 2);
        g_assert_cmpuint(qtest_readb(qts, CR0), ==, 1);
        qtest_writeb(qts, ICR, 0x70);
    }
    qtest_quit(qts);
}

static void test_active_write(void)
{
    QTestState *qts = cac_init();

    setup(qts, 7, 0, 16, 16);
    input(qts, true);
    qtest_clock_step(qts, 2000);
    qtest_writeb(qts, CR1, 0x84);
    qtest_writeb(qts, CR2, 0xff);
    qtest_writew(qts, UPPER, 0);
    qtest_writew(qts, LOWER, 0xffff);
    g_assert_cmpuint(qtest_readb(qts, CR1), ==, 7);
    g_assert_cmpuint(qtest_readb(qts, CR2), ==, 0);
    g_assert_cmpuint(qtest_readw(qts, UPPER), ==, 16);
    g_assert_cmpuint(qtest_readw(qts, LOWER), ==, 16);
    qtest_writeb(qts, CR0, 0xff); /* Repeated enable must not restart. */
    g_assert_cmpuint(qtest_readb(qts, CR0), ==, 1);
    qtest_clock_step(qts, 2000);
    rising(qts);
    g_assert_cmpuint(qtest_readb(qts, STR), ==, 2);
    g_assert_cmpuint(qtest_readw(qts, BUFFER), ==, 16);
    qtest_writeb(qts, CR0, 0);
    qtest_clock_step(qts, 5000);
    rising(qts);
    g_assert_cmpuint(qtest_readw(qts, BUFFER), ==, 16);
    qtest_writeb(qts, ICR, 0x70);
    qtest_writeb(qts, CR0, 1);
    rising(qts);
    g_assert_cmpuint(qtest_readb(qts, STR), ==, 0);
    qtest_clock_step(qts, 1000);
    rising(qts);
    g_assert_cmpuint(qtest_readw(qts, BUFFER), ==, 4);
    qtest_quit(qts);
}

static void test_irq_overflow(void)
{
    QTestState *qts = cac_init();

    qtest_irq_intercept_in(qts, "/machine/mcu/icu");
    setup(qts, 7, 0, 0, 0);
    input(qts, true);
    qtest_clock_step(qts, 16384000 - 1);
    g_assert_cmpuint(qtest_readb(qts, STR), ==, 0);
    qtest_clock_step(qts, 1);
    g_assert_cmpuint(qtest_readb(qts, STR), ==, 4);
    qtest_clock_step(qts, 1000);
    rising(qts);
    g_assert_cmpuint(qtest_readw(qts, BUFFER), ==, 4);
    g_assert_cmpuint(qtest_readb(qts, STR), ==, 7);
    for (unsigned i = 0; i < 3; i++) {
        unsigned mask = 1 << i;

        qtest_writeb(qts, ICR, mask);
        g_assert_cmpuint(qtest_readb(qts, ICR), ==, mask);
        for (unsigned j = 0; j < 3; j++) {
            g_assert_cmpuint(qtest_get_irq(qts, 32 + j), ==, i == j);
            g_assert_cmpuint(qtest_readb(qts, 0x87020 + j), ==, i == j);
            qtest_writeb(qts, 0x87120 + j, 1);
            g_assert_cmpuint(qtest_readb(qts, 0x87120 + j), ==, 0);
        }
        qtest_writeb(qts, ICR, 0);
        g_assert_false(qtest_get_irq(qts, 32 + i));
    }
    qtest_writeb(qts, ICR, 7);
    for (unsigned i = 0; i < 3; i++) {
        unsigned mask = 1 << i;

        qtest_writeb(qts, ICR, 7 | (mask << 4));
        g_assert_cmpuint(qtest_readb(qts, ICR), ==, 7);
        g_assert_false(qtest_get_irq(qts, 32 + i));
        g_assert_cmpuint(qtest_readb(qts, STR), ==, 7 & ~((mask << 1) - 1));
    }
    /* Capture cleared the counter; OVFF now belongs to the new interval. */
    qtest_clock_step(qts, 16384000 - 1);
    g_assert_cmpuint(qtest_readb(qts, STR), ==, 0);
    qtest_clock_step(qts, 1);
    g_assert_cmpuint(qtest_readb(qts, STR), ==, 4);
    qtest_quit(qts);
}

static void test_filter(void)
{
    QTestState *qts = cac_init();
    static const unsigned div[] = { 1, 4, 16 };

    for (unsigned dfs = 1; dfs <= 3; dfs++) {
        unsigned period = 250 * div[dfs - 1];

        setup(qts, 0x87, dfs << 6, 0, 65535); /* Both edges. */
        qtest_clock_step(qts, 1);
        input(qts, true);
        qtest_clock_step(qts, period); /* One sample high. */
        input(qts, false);
        qtest_clock_step(qts, period * 3 - 1);
        g_assert_cmpuint(qtest_readb(qts, STR), ==, 0);
        qtest_clock_step(qts, 1);
        input(qts, true);
        qtest_clock_step(qts, period * 3 - 1); /* Third high starts count. */
        g_assert_cmpuint(qtest_readb(qts, STR), ==, 0);
        qtest_clock_step(qts, 1);
        input(qts, false);
        qtest_clock_step(qts, period * 3 - 2);
        g_assert_cmpuint(qtest_readb(qts, STR), ==, 0);
        qtest_clock_step(qts, 1); /* Third low captures. */
        g_assert_cmpuint(qtest_readb(qts, STR), ==, 2);
        g_assert_cmpuint(qtest_readw(qts, BUFFER), ==, 3 * div[dfs - 1]);
    }
    /* CFME gates counting; the pin filter retains its sampling history. */
    setup(qts, 0x87, 0x80, 0, 65535);
    qtest_writeb(qts, CR0, 0);
    qtest_clock_step(qts, 1);
    input(qts, true);
    qtest_clock_step(qts, 999); /* First high sample with CFME=0. */
    qtest_writeb(qts, CR0, 1);
    qtest_clock_step(qts, 2000); /* Third high starts counting. */
    input(qts, false);
    qtest_clock_step(qts, 2999);
    g_assert_cmpuint(qtest_readb(qts, STR), ==, 0);
    qtest_clock_step(qts, 1);
    g_assert_cmpuint(qtest_readb(qts, STR), ==, 2);
    g_assert_cmpuint(qtest_readw(qts, BUFFER), ==, 12);
    qtest_quit(qts);
}

static void test_internal(void)
{
    QTestState *qts = cac_init();
    static const unsigned div[] = { 32, 128, 1024, 8192 };

    for (unsigned d = 0; d < 4; d++) {
        for (unsigned edges = 0; edges < 3; edges++) {
            unsigned interval = div[d] * 250;
            unsigned half = interval / 2;
            unsigned first = edges == 1 ? interval : half;
            unsigned between = edges == 2 ? half : interval;
            unsigned count = between / 250;

            setup(qts, 6 | (edges << 6), 7 | (d << 4), count, count);
            qtest_clock_step(qts, first);
            g_assert_cmpuint(qtest_readb(qts, STR), ==, 0);
            qtest_clock_step(qts, between - 1);
            g_assert_cmpuint(qtest_readb(qts, STR), ==, 0);
            qtest_clock_step(qts, 1);
            g_assert_cmpuint(qtest_readb(qts, STR), ==, 2);
            g_assert_cmpuint(qtest_readw(qts, BUFFER), ==, count);
            for (unsigned i = 0; i < 3; i++) {
                qtest_writeb(qts, ICR, 0x70);
                qtest_clock_step(qts, between);
                g_assert_cmpuint(qtest_readw(qts, BUFFER), ==, count);
                g_assert_cmpuint(qtest_readb(qts, STR), ==, 2);
            }
        }
    }
    /* Clock reconfiguration changes the next target count immediately. */
    setup(qts, 11, 0, 0, 65535); /* PCLKB reset: LOCO / 8 = 500 kHz. */
    input(qts, true);
    qtest_clock_step(qts, 4000);
    qtest_writel(qts, 0x80020, 0); /* PCLKB = LOCO = 4 MHz. */
    qtest_clock_step(qts, 4000);
    rising(qts);
    g_assert_cmpuint(qtest_readw(qts, BUFFER), ==, 18);
    qtest_quit(qts);
}

static void test_sources(void)
{
    QTestState *qts = cac_init();
    static const unsigned hz[] = { 16000000, 32768, 32000000,
                                   4000000, 15000, 500000 };

    qtest_writeb(qts, 0x80032, 0); /* Main oscillator. */
    qtest_clock_step(qts, 2048000);
    for (unsigned source = 0; source < 6; source++) {
        unsigned expected = hz[source] / 1000;

        setup(qts, 1 | (source << 1), 0, expected, expected);
        input(qts, true);
        qtest_clock_step(qts, 1000000);
        rising(qts);
        g_assert_cmpuint(qtest_readw(qts, BUFFER), ==, expected);
        g_assert_cmpuint(qtest_readb(qts, STR), ==, 2);
    }
    /* Main/HOCO unavailable while stopped and during stabilization. */
    for (unsigned source = 0; source <= 2; source += 2) {
        unsigned stop = source == 0 ? 0x80032 : 0x80036;
        unsigned wait = source == 0 ? 2048000 : 34500;
        unsigned expected = source == 0 ? 16 : 32;

        qtest_writeb(qts, stop, 1);
        setup(qts, 1 | (source << 1), 0, 0, 65535);
        input(qts, true);
        qtest_clock_step(qts, 1000);
        qtest_writeb(qts, stop, 0);
        qtest_clock_step(qts, wait);
        rising(qts);
        g_assert_cmpuint(qtest_readw(qts, BUFFER), ==, 0);
        qtest_clock_step(qts, 1000);
        rising(qts);
        g_assert_cmpuint(qtest_readw(qts, BUFFER), ==, expected);
    }
    /* RTCEN can keep the sub oscillator running with SOSTP=1. */
    qtest_writeb(qts, 0x80033, 1);
    qtest_writeb(qts, 0x8c426, 0);
    setup(qts, 3, 0, 0, 65535);
    input(qts, true);
    qtest_clock_step(qts, 1000000);
    rising(qts);
    g_assert_cmpuint(qtest_readw(qts, BUFFER), ==, 0);
    qtest_writeb(qts, 0x8c426, 1);
    qtest_clock_step(qts, 1000000);
    rising(qts);
    g_assert_cmpuint(qtest_readw(qts, BUFFER), ==, 32);
    qtest_writeb(qts, 0x8c426, 0);
    qtest_clock_step(qts, 1000000);
    rising(qts);
    g_assert_cmpuint(qtest_readw(qts, BUFFER), ==, 0);
    qtest_quit(qts);
}

static void test_iwdt_start(void)
{
    g_autofree char *dir = g_dir_make_tmp("cac-rom-XXXXXX", NULL);
    g_autofree char *file = g_strdup_printf("%s/rom", dir);
    g_autofree char *rom = g_malloc0(512 * 1024);
    QTestState *qts;

    /* OFS0.IWDTSTRT=1 selects manual start; no watchdog side effects. */
    rom[512 * 1024 - 0x74] = 2;
    g_assert_true(g_file_set_contents(file, rom, 512 * 1024, NULL));
    qts = qtest_initf("-M rx113-r5f51138 -bios %s", file);
    qtest_writew(qts, 0x803fe, 0xa503);
    qtest_writel(qts, MSTPCRC, RUN_C);
    setup(qts, 9, 0, 0, 65535);
    input(qts, true);
    qtest_clock_step(qts, 1000000);
    rising(qts);
    g_assert_cmpuint(qtest_readw(qts, BUFFER), ==, 0);
    qtest_writeb(qts, 0x80035, 0);
    qtest_clock_step(qts, 49999);
    rising(qts);
    g_assert_cmpuint(qtest_readw(qts, BUFFER), ==, 0);
    qtest_clock_step(qts, 1);
    rising(qts);
    g_assert_cmpuint(qtest_readw(qts, BUFFER), ==, 0);
    qtest_clock_step(qts, 1000000);
    rising(qts);
    g_assert_cmpuint(qtest_readw(qts, BUFFER), ==, 15);
    qtest_writeb(qts, 0x80035, 1); /* The oscillator cannot be stopped. */
    qtest_clock_step(qts, 1000000);
    rising(qts);
    g_assert_cmpuint(qtest_readw(qts, BUFFER), ==, 15);
    qtest_quit(qts);
    unlink(file);
    rmdir(dir);
}

static void test_access_stop_reset(void)
{
    QTestState *qts = qtest_init("-M rx113-r5f51138");

    qtest_writeb(qts, CR0, 1);
    qtest_writew(qts, UPPER, 123);
    g_assert_cmpuint(qtest_readb(qts, CR0), ==, 0);
    qtest_writew(qts, 0x803fe, 0xa503);
    qtest_writel(qts, MSTPCRC, RUN_C);
    for (unsigned a = 0; a < 12; a++) {
        g_assert_cmpuint(qtest_readb(qts, CAC + a), ==, 0);
    }
    setup(qts, 7, 0, 0, 65535);
    qtest_writel(qts, CR0, 0xffffffff);
    qtest_writew(qts, CR1, 0xffff);
    qtest_writeb(qts, UPPER, 0);
    qtest_writeb(qts, UPPER + 1, 0);
    qtest_writew(qts, BUFFER, 0xffff);
    qtest_writeb(qts, STR, 7);
    g_assert_cmpuint(qtest_readb(qts, CR1), ==, 7);
    g_assert_cmpuint(qtest_readw(qts, UPPER), ==, 65535);
    g_assert_cmpuint(qtest_readw(qts, BUFFER), ==, 0);
    g_assert_cmpuint(qtest_readb(qts, STR), ==, 0);
    input(qts, true);
    qtest_clock_step(qts, 2000);
    qtest_writel(qts, MSTPCRC, STOP_C);
    qtest_writeb(qts, CR0, 0);
    qtest_writew(qts, UPPER, 0);
    qtest_clock_step(qts, 100000);
    g_assert_cmpuint(qtest_readb(qts, CR0), ==, 0);
    g_assert_cmpuint(qtest_readw(qts, UPPER), ==, 0);
    qtest_writel(qts, MSTPCRC, RUN_C);
    g_assert_cmpuint(qtest_readb(qts, CR0), ==, 1);
    g_assert_cmpuint(qtest_readw(qts, UPPER), ==, 65535);
    qtest_clock_step(qts, 2000);
    rising(qts);
    g_assert_cmpuint(qtest_readw(qts, BUFFER), ==, 16);
    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    qtest_writew(qts, 0x803fe, 0xa503);
    qtest_writel(qts, MSTPCRC, RUN_C);
    g_assert_cmpuint(qtest_readb(qts, CR0), ==, 0);
    g_assert_cmpuint(qtest_readw(qts, BUFFER), ==, 0);
    g_assert_cmpuint(qtest_readb(qts, STR), ==, 0);
    qtest_quit(qts);
}

static void test_port(void)
{
    static const unsigned pins[] = { 0x15, 0x27, 0xa0, 0xc7 };

    for (unsigned package = 64; package <= 100; package += 36) {
        g_autofree char *args = g_strdup_printf(
            "-M rx113-r5f51138 -global rx113-port.package-pins=%u", package);
        QTestState *qts = qtest_init(args);

        qtest_writew(qts, 0x803fe, 0xa503);
        qtest_writel(qts, MSTPCRC, RUN_C);
        qtest_writeb(qts, 0x8c11f, 0);
        qtest_writeb(qts, 0x8c11f, 0x40);
        for (unsigned i = 0; i < ARRAY_SIZE(pins); i++) {
            unsigned port = pins[i] >> 4, bit = pins[i] & 7;
            unsigned pin = port * 8 + bit;

            qtest_writeb(qts, CR0, 0);
            qtest_writeb(qts, CR1, 7);
            qtest_writeb(qts, ICR, 0x70);
            qtest_writew(qts, UPPER, 65535);
            qtest_writeb(qts, 0x8c140 + pin, 7);
            qtest_writeb(qts, 0x8c060 + port, 0);
            qtest_set_irq_in(qts, PORT_PATH, "pin", pin, 1);
            qtest_writeb(qts, CR0, 1);
            qtest_clock_step(qts, 1000);
            qtest_set_irq_in(qts, PORT_PATH, "pin", pin, 0);
            g_assert_cmpuint(qtest_readb(qts, STR), ==, 0);
            qtest_writeb(qts, 0x8c060 + port, 1 << bit);
            qtest_set_irq_in(qts, PORT_PATH, "pin", pin, 1);
            qtest_clock_step(qts, 1000);
            qtest_set_irq_in(qts, PORT_PATH, "pin", pin, 0);
            qtest_set_irq_in(qts, PORT_PATH, "pin", pin, 1);
            g_assert_cmpuint(qtest_readw(qts, BUFFER), ==, 4);
            g_assert_cmpuint(qtest_readb(qts, STR), ==, 2);
            qtest_writeb(qts, CR0, 0);
            qtest_writeb(qts, ICR, 0x70);
            /* PMR alone is insufficient: check wrong PSEL and P27 ASEL. */
            qtest_writeb(qts, 0x8c140 + pin, pins[i] == 0x27 ? 0x87 : 0);
            qtest_writeb(qts, CR0, 1);
            qtest_set_irq_in(qts, PORT_PATH, "pin", pin, 0);
            qtest_set_irq_in(qts, PORT_PATH, "pin", pin, 1);
            qtest_clock_step(qts, 1000);
            qtest_set_irq_in(qts, PORT_PATH, "pin", pin, 0);
            qtest_set_irq_in(qts, PORT_PATH, "pin", pin, 1);
            g_assert_cmpuint(qtest_readb(qts, STR), ==, 0);
            qtest_writeb(qts, CR0, 0);
            qtest_writeb(qts, 0x8c060 + port, 0);
            qtest_set_irq_in(qts, PORT_PATH, "pin", pin, 0);
        }
        qtest_quit(qts);
    }
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
    bool stopped = GPOINTER_TO_UINT(opaque);
    QTestState *src = cac_init();
    QTestState *dst;
    int64_t now;
    g_autofree char *dir = g_dir_make_tmp("cac-XXXXXX", NULL);
    g_autofree char *file = g_strdup_printf("%s/state", dir);
    g_autofree char *uri = g_strdup_printf("file:%s", file);

    setup(src, 7, 0, 0, 65535);
    input(src, true);
    /* Preserve half of a target-clock cycle. */
    now = qtest_clock_step(src, 2125);
    if (stopped) {
        qtest_writel(src, MSTPCRC, STOP_C);
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
        qtest_clock_step(dst, 100000);
        g_assert_cmpuint(qtest_readb(dst, CR0), ==, 0);
        qtest_writel(dst, MSTPCRC, RUN_C);
    }
    g_assert_cmpuint(qtest_readb(dst, CR0), ==, 1);
    qtest_clock_step(dst, 1875);
    rising(dst);
    g_assert_cmpuint(qtest_readw(dst, BUFFER), ==, 16);
    g_assert_cmpuint(qtest_readb(dst, STR), ==, 2);
    qtest_quit(dst);
    qtest_quit(src);
    unlink(file);
    rmdir(dir);
}

static void test_migration_timed(const void *opaque)
{
    bool internal = GPOINTER_TO_UINT(opaque);
    QTestState *src = cac_init();
    QTestState *dst;
    int64_t now;
    g_autofree char *dir = g_dir_make_tmp("cac-timed-XXXXXX", NULL);
    g_autofree char *file = g_strdup_printf("%s/state", dir);
    g_autofree char *uri = g_strdup_printf("file:%s", file);

    setup(src, internal ? 6 : 0x87, internal ? 7 : 0xc0, 0, 65535);
    if (!internal) {
        qtest_clock_step(src, 1);
        input(src, true);
    }
    now = qtest_clock_set(src, 4500);
    qtest_qmp_assert_success(src, "{'execute':'migrate',"
                            "'arguments':{'uri':%s}}", uri);
    migration_wait(src);
    dst = qtest_init("-M rx113-r5f51138 -incoming defer");
    qtest_clock_set(dst, now);
    qtest_qmp_assert_success(dst, "{'execute':'migrate-incoming',"
                            "'arguments':{'uri':%s}}", uri);
    migration_wait(dst);
    /* Restore the test's external driver after PORT restores its outputs. */
    input(dst, true);
    qtest_clock_step(dst, 7499);
    g_assert_cmpuint(qtest_readb(dst, STR), ==, 0);
    qtest_clock_step(dst, 1);
    if (!internal) {
        g_assert_cmpuint(qtest_readb(dst, STR), ==, 0); /* Third high sample. */
        input(dst, false);
        qtest_clock_step(dst, 11999);
        g_assert_cmpuint(qtest_readb(dst, STR), ==, 0);
        qtest_clock_step(dst, 1);
    }
    g_assert_cmpuint(qtest_readb(dst, STR), ==, 2);
    g_assert_cmpuint(qtest_readw(dst, BUFFER), ==, internal ? 32 : 48);
    qtest_quit(dst);
    qtest_quit(src);
    unlink(file);
    rmdir(dir);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/rx113-cac/measure", test_measure);
    qtest_add_func("/rx113-cac/intervals", test_intervals);
    qtest_add_func("/rx113-cac/active-write", test_active_write);
    qtest_add_func("/rx113-cac/irq-overflow", test_irq_overflow);
    qtest_add_func("/rx113-cac/filter", test_filter);
    qtest_add_func("/rx113-cac/internal", test_internal);
    qtest_add_func("/rx113-cac/sources", test_sources);
    qtest_add_func("/rx113-cac/iwdt-start", test_iwdt_start);
    qtest_add_func("/rx113-cac/access-stop-reset", test_access_stop_reset);
    qtest_add_func("/rx113-cac/port", test_port);
    qtest_add_data_func("/rx113-cac/migration-running", GUINT_TO_POINTER(0),
                        test_migration);
    qtest_add_data_func("/rx113-cac/migration-stopped", GUINT_TO_POINTER(1),
                        test_migration);
    qtest_add_data_func("/rx113-cac/migration-filter", GUINT_TO_POINTER(0),
                        test_migration_timed);
    qtest_add_data_func("/rx113-cac/migration-reference", GUINT_TO_POINTER(1),
                        test_migration_timed);
    return g_test_run();
}
