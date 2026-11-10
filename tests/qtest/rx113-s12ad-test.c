/*
 * RX113 S12ADb tests (R01UH0448EJ0120, section 36).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define BASE 0x89000
#define CSR BASE
#define ADDR(ch) (BASE + 0x20 + 2 * (ch))
#define IR(n) (0x87000 + (n))
#define MSTPCRA 0x80010
#define SCKCR 0x80020
#define ADC_PATH "/machine/mcu/s12ad"
#define STOPPED 0xefffffff
#define RUNNING (STOPPED & ~(1U << 17))

static QTestState *adc_init(void)
{
    QTestState *qts = qtest_init("-M rx113-r5f51138");

    qtest_writew(qts, 0x803fe, 0xa503);
    qtest_writeb(qts, 0x80036, 0);
    qtest_clock_step(qts, 34500);
    qtest_writew(qts, 0x80026, 0x100);
    qtest_writel(qts, SCKCR, 0); /* PCLKB = PCLKD = 32 MHz. */
    qtest_writel(qts, MSTPCRA, RUNNING);
    qtest_clock_step(qts, 1000);
    qtest_writeb(qts, 0x8c11f, 0);
    qtest_writeb(qts, 0x8c11f, 0x40);
    for (unsigned ch = 0; ch < 8; ch++) {
        qtest_writeb(qts, 0x8c160 + ch, 0x80);
        qtest_writeb(qts, 0x8c1b0 + ch, 0x80);
    }
    for (unsigned ch = 0; ch < 3; ch++) {
        qtest_writeb(qts, 0x8c188 + ch, 0x80);
    }
    return qts;
}

static void voltage(QTestState *qts, const char *prop, unsigned uv)
{
    qtest_qmp_assert_success(qts, "{'execute':'qom-set','arguments':{"
                            "'path':'" ADC_PATH "','property':%s,'value':%u}}",
                            prop, uv);
}

static void input(QTestState *qts, unsigned ch, unsigned uv)
{
    g_autofree char *name = g_strdup_printf("an%03u-uv", ch);

    voltage(qts, name, uv);
}

static void trigger(QTestState *qts, unsigned n)
{
    qtest_set_irq_in(qts, ADC_PATH, "trigger", n, 0);
    qtest_set_irq_in(qts, ADC_PATH, "trigger", n, 1);
}

static void start(QTestState *qts, unsigned csr)
{
    qtest_writew(qts, CSR, csr);
    qtest_writew(qts, CSR, csr | 0x8000);
}

static void test_voltage(void)
{
    static const unsigned uv[] = { 0, 805, 806, 825000, 1650000,
                                   2475000, 3299999, 3300000, 3600000 };
    static const unsigned code[] = { 0, 0, 1, 1024, 2048,
                                     3072, 4095, 4095, 4095 };
    QTestState *qts = adc_init();

    qtest_writew(qts, BASE + 4, 1);
    for (unsigned i = 0; i < G_N_ELEMENTS(uv); i++) {
        input(qts, 0, uv[i]);
        start(qts, 0x1400);
        /* tD=5 cycles, tSPL=21, tCONV=23, tED=3: 52 cycles. */
        qtest_clock_step(qts, 1624);
        g_assert_cmpuint(qtest_readb(qts, IR(102)), ==, 0);
        g_assert_cmphex(qtest_readw(qts, CSR), ==, 0x9400);
        qtest_clock_step(qts, 1);
        g_assert_cmpuint(qtest_readw(qts, ADDR(0)), ==, code[i]);
        g_assert_cmpuint(qtest_readb(qts, IR(102)), ==, 1);
        g_assert_cmpuint(qtest_readb(qts, IR(103)), ==, 0);
        g_assert_cmphex(qtest_readw(qts, CSR), ==, 0x1400);
        qtest_writeb(qts, IR(102), 0);
    }
    /* No injected input: deterministic grounded channel. */
    qtest_writew(qts, BASE + 4, 2);
    start(qts, 0x400);
    qtest_clock_step(qts, 1625);
    g_assert_cmpuint(qtest_readw(qts, ADDR(1)), ==, 0);
    g_assert_cmpuint(qtest_readb(qts, IR(102)), ==, 0);
    qtest_quit(qts);
}

static void test_scan_order(void)
{
    QTestState *qts = adc_init();
    static const unsigned ch[] = { 0, 1, 2, 3, 4, 5, 6, 7,
                                  8, 9, 10, 11, 12, 13, 14, 15, 21 };

    qtest_writew(qts, BASE + 4, 0xffff);
    qtest_writew(qts, BASE + 6, 0x20);
    for (unsigned i = 0; i < G_N_ELEMENTS(ch); i++) {
        input(qts, ch[i], 100000 * (i + 1));
    }
    start(qts, 0x1400);
    qtest_clock_step(qts, 156); /* 5-cycle tD rounds up to 157 ns. */
    for (unsigned i = 0; i < G_N_ELEMENTS(ch); i++) {
        qtest_clock_step(qts, i == 0 ? 1375 : 1374);
        g_assert_cmpuint(qtest_readw(qts, ADDR(ch[i])), ==, 0);
        qtest_clock_step(qts, 1);
        g_assert_cmpuint(qtest_readw(qts, ADDR(ch[i])), ==,
                         (uint64_t)100000 * (i + 1) * 4096 / 3300000);
        if (i + 1 < G_N_ELEMENTS(ch)) {
            g_assert_cmpuint(qtest_readw(qts, ADDR(ch[i + 1])), ==, 0);
        }
        g_assert_cmpuint(qtest_readb(qts, IR(102)), ==, 0);
    }
    qtest_clock_step(qts, 92);
    g_assert_cmpuint(qtest_readb(qts, IR(102)), ==, 0);
    qtest_clock_step(qts, 1);
    g_assert_cmpuint(qtest_readb(qts, IR(102)), ==, 1);
    qtest_quit(qts);
}

static void test_continuous(void)
{
    QTestState *qts = adc_init();

    qtest_writew(qts, BASE + 4, 3);
    input(qts, 0, 825000);
    input(qts, 1, 1650000);
    start(qts, 0x5400);
    qtest_clock_step(qts, 2906); /* (5 + 2*44) cycles = 2906.25 ns. */
    g_assert_cmpuint(qtest_readb(qts, IR(102)), ==, 0);
    qtest_clock_step(qts, 1);
    g_assert_cmpuint(qtest_readb(qts, IR(102)), ==, 1);
    g_assert_cmpuint(qtest_readw(qts, ADDR(0)), ==, 1024);
    g_assert_cmpuint(qtest_readw(qts, ADDR(1)), ==, 2048);
    g_assert_cmphex(qtest_readw(qts, CSR), ==, 0xd400);
    qtest_writeb(qts, IR(102), 0);
    input(qts, 0, 2475000);
    qtest_clock_step(qts, 2749);
    g_assert_cmpuint(qtest_readb(qts, IR(102)), ==, 0);
    qtest_clock_step(qts, 1);
    g_assert_cmpuint(qtest_readb(qts, IR(102)), ==, 1);
    g_assert_cmpuint(qtest_readw(qts, ADDR(0)), ==, 3072);
    qtest_writew(qts, CSR, 0x5400);
    qtest_writeb(qts, IR(102), 0);
    input(qts, 0, 0);
    qtest_clock_step(qts, 1000000);
    g_assert_cmpuint(qtest_readw(qts, ADDR(0)), ==, 3072);
    g_assert_cmpuint(qtest_readb(qts, IR(102)), ==, 0);
    qtest_quit(qts);
}

static void test_triggers(void)
{
    QTestState *qts = adc_init();

    qtest_writew(qts, BASE + 4, 1);
    input(qts, 0, 1650000);
    qtest_writew(qts, BASE + 0x10, 0x0900); /* ELC, source 9. */
    qtest_writew(qts, CSR, 0x1400);
    trigger(qts, 9); /* TRGE disabled. */
    qtest_clock_step(qts, 2000);
    g_assert_cmpuint(qtest_readw(qts, ADDR(0)), ==, 0);
    qtest_writew(qts, CSR, 0x1600);
    trigger(qts, 8); /* Wrong source. */
    qtest_clock_step(qts, 2000);
    g_assert_cmpuint(qtest_readw(qts, ADDR(0)), ==, 0);
    trigger(qts, 9);
    qtest_clock_step(qts, 800);
    trigger(qts, 9); /* Ignored while busy: cannot restart or queue. */
    qtest_clock_step(qts, 824);
    g_assert_cmpuint(qtest_readb(qts, IR(102)), ==, 0);
    qtest_clock_step(qts, 1);
    g_assert_cmpuint(qtest_readb(qts, IR(102)), ==, 1);
    qtest_writeb(qts, IR(102), 0);
    qtest_clock_step(qts, 2000);
    g_assert_cmpuint(qtest_readb(qts, IR(102)), ==, 0);
    /* Every MTU/ELC selector, A and B, with the proper scan-end vector. */
    qtest_writew(qts, BASE + 0x14, 2);
    input(qts, 1, 825000);
    for (unsigned n = 1; n <= 9; n++) {
        qtest_writew(qts, BASE + 0x10, (n << 8) | (n == 9 ? 1 : n + 1));
        qtest_writew(qts, CSR, 0x3640);
        trigger(qts, n);
        qtest_clock_step(qts, 1625);
        g_assert_cmpuint(qtest_readb(qts, IR(102)), ==, 1);
        g_assert_cmpuint(qtest_readb(qts, IR(103)), ==, 0);
        qtest_writeb(qts, IR(102), 0);
        trigger(qts, n == 9 ? 1 : n + 1);
        qtest_clock_step(qts, 1624);
        g_assert_cmpuint(qtest_readb(qts, IR(103)), ==, 0);
        qtest_clock_step(qts, 1);
        g_assert_cmpuint(qtest_readb(qts, IR(103)), ==, 1);
        g_assert_cmpuint(qtest_readb(qts, IR(102)), ==, 0);
        g_assert_cmpuint(qtest_readw(qts, ADDR(1)), ==, 1024);
        qtest_writeb(qts, IR(103), 0);
    }
    /* Qualified asynchronous falling edge: 2 extra PCLK cycles. */
    qtest_writew(qts, BASE + 0x10, 0);
    qtest_writew(qts, CSR, 0x1700);
    qtest_set_irq_in(qts, ADC_PATH, "trigger", 0, 1);
    qtest_set_irq_in(qts, ADC_PATH, "trigger", 0, 0);
    qtest_clock_step(qts, 1687);
    g_assert_cmpuint(qtest_readb(qts, IR(102)), ==, 0);
    qtest_clock_step(qts, 1);
    g_assert_cmpuint(qtest_readb(qts, IR(102)), ==, 1);
    qtest_quit(qts);
}

static void test_timing(void)
{
    QTestState *qts = adc_init();
    static const unsigned ch[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 15, 21 };
    static const unsigned sst[] = { 0x60, 0x73, 0x74, 0x75, 0x76, 0x77,
                                   0x78, 0x79, 0x61, 0x61, 0x80 };

    for (unsigned mode = 0; mode < 2; mode++) {
        for (unsigned i = 0; i < G_N_ELEMENTS(ch); i++) {
            qtest_writew(qts, BASE + 4, ch[i] < 16 ? 1 << ch[i] : 0);
            qtest_writew(qts, BASE + 6, ch[i] == 21 ? 0x20 : 0);
            for (unsigned d = 0; d <= 6; d++) {
                unsigned sampling = 6 + i;
                unsigned cycles = 5 + sampling + 1 + (mode ? 23 : 29) + 3;
                unsigned ns = (cycles * (1 << d) * 125 + 3) / 4;

                qtest_writel(qts, SCKCR, (d << 8) | d);
                qtest_writeb(qts, BASE + sst[i], sampling);
                start(qts, 0x1000 | (mode ? 0x400 : 0));
                qtest_clock_step(qts, ns - 1);
                g_assert_cmpuint(qtest_readb(qts, IR(102)), ==, 0);
                qtest_clock_step(qts, 1);
                g_assert_cmpuint(qtest_readb(qts, IR(102)), ==, 1);
                qtest_writeb(qts, IR(102), 0);
            }
        }
    }
    /* Distinct ADCLK/PCLK divisors: 3 PCLK + 49 ADCLK. */
    qtest_writew(qts, BASE + 4, 1);
    qtest_writew(qts, BASE + 6, 0);
    qtest_writeb(qts, BASE + 0x60, 20);
    for (unsigned p = 0; p <= 3; p++) {
        for (unsigned a = 0; a <= 3; a++) {
            unsigned ns = ((3 * (1 << p) + 49 * (1 << a)) * 125 + 3) / 4;

            qtest_writel(qts, SCKCR, (p << 8) | a);
            start(qts, 0x1400);
            qtest_clock_step(qts, ns - 1);
            g_assert_cmpuint(qtest_readb(qts, IR(102)), ==, 0);
            qtest_clock_step(qts, 1);
            g_assert_cmpuint(qtest_readb(qts, IR(102)), ==, 1);
            qtest_writeb(qts, IR(102), 0);
        }
    }
    qtest_quit(qts);
}

static void test_clock_stop(void)
{
    QTestState *qts = adc_init();

    qtest_writew(qts, BASE + 4, 1);
    input(qts, 0, 1650000);
    start(qts, 0x1400);
    qtest_clock_step(qts, 501); /* Fractional progress in sampling. */
    qtest_writel(qts, SCKCR, 1); /* ADCLK /2, PCLK unchanged. */
    qtest_writel(qts, MSTPCRA, STOPPED);
    qtest_clock_step(qts, 1000000);
    g_assert_cmpuint(qtest_readw(qts, CSR), ==, 0);
    qtest_writew(qts, CSR, 0); /* Disabled register write discarded. */
    trigger(qts, 1);
    qtest_writel(qts, MSTPCRA, RUNNING);
    /* Remaining: 34.968 ADCLK * 62.5 + 1 PCLK * 31.25 = 2216.75. */
    qtest_clock_step(qts, 2216);
    g_assert_cmpuint(qtest_readb(qts, IR(102)), ==, 0);
    qtest_clock_step(qts, 1);
    g_assert_cmpuint(qtest_readb(qts, IR(102)), ==, 1);
    g_assert_cmpuint(qtest_readw(qts, ADDR(0)), ==, 2048);
    /* Pending ICU edge remains pending across module stop. */
    qtest_writel(qts, MSTPCRA, STOPPED);
    g_assert_cmpuint(qtest_readb(qts, IR(102)), ==, 1);
    qtest_writel(qts, MSTPCRA, RUNNING);
    qtest_clock_step(qts, 1000);
    qtest_writeb(qts, IR(102), 0);
    start(qts, 0x1400);
    qtest_clock_step(qts, 501);
    /* Changing PCLK alone during an ADCLK phase leaves it unaffected. */
    qtest_writel(qts, SCKCR, 0x101);
    qtest_clock_step(qts, 2686); /* Total 3187, target 3187.5 ns. */
    g_assert_cmpuint(qtest_readb(qts, IR(102)), ==, 0);
    qtest_clock_step(qts, 1);
    g_assert_cmpuint(qtest_readb(qts, IR(102)), ==, 1);
    qtest_quit(qts);
}

static void test_addition_double(void)
{
    QTestState *qts = adc_init();

    qtest_writew(qts, BASE + 4, 1);
    qtest_writew(qts, BASE + 8, 1);
    input(qts, 0, 825000);
    for (unsigned count = 0; count < 4; count++) {
        unsigned ns = ((8 + 44 * (count + 1)) * 125 + 3) / 4;

        qtest_writeb(qts, BASE + 0xc, count);
        start(qts, 0x1400);
        qtest_clock_step(qts, ns - 1);
        g_assert_cmpuint(qtest_readb(qts, IR(102)), ==, 0);
        qtest_clock_step(qts, 1);
        g_assert_cmpuint(qtest_readb(qts, IR(102)), ==, 1);
        g_assert_cmpuint(qtest_readw(qts, ADDR(0)), ==, 4096 * (count + 1));
        qtest_writeb(qts, IR(102), 0);
    }
    qtest_writew(qts, BASE + 8, 0);
    qtest_writew(qts, BASE + 0xe, 0x8020); /* left aligned, clear on read. */
    start(qts, 0x1400);
    qtest_clock_step(qts, 1625);
    g_assert_cmphex(qtest_readw(qts, ADDR(0)), ==, 0x4000);
    g_assert_cmpuint(qtest_readw(qts, ADDR(0)), ==, 0);
    qtest_writew(qts, BASE + 0xe, 0);
    qtest_writeb(qts, IR(102), 0);
    qtest_writew(qts, BASE + 0x10, 0x100);
    qtest_writew(qts, CSR, 0x1680); /* channel 0, double trigger. */
    trigger(qts, 1);
    qtest_clock_step(qts, 1625);
    g_assert_cmpuint(qtest_readw(qts, ADDR(0)), ==, 1024);
    g_assert_cmpuint(qtest_readw(qts, BASE + 0x18), ==, 0);
    g_assert_cmpuint(qtest_readb(qts, IR(102)), ==, 0);
    input(qts, 0, 1650000);
    /* Software scan between the pair does not consume the second trigger. */
    start(qts, 0x1680);
    qtest_clock_step(qts, 1625);
    g_assert_cmpuint(qtest_readw(qts, ADDR(0)), ==, 2048);
    g_assert_cmpuint(qtest_readb(qts, IR(102)), ==, 1);
    qtest_writeb(qts, IR(102), 0);
    input(qts, 0, 2475000);
    trigger(qts, 1);
    qtest_clock_step(qts, 1625);
    g_assert_cmpuint(qtest_readw(qts, ADDR(0)), ==, 2048);
    g_assert_cmpuint(qtest_readw(qts, BASE + 0x18), ==, 3072);
    g_assert_cmpuint(qtest_readb(qts, IR(102)), ==, 1);
    qtest_quit(qts);
}

static void test_reference_sample(void)
{
    QTestState *qts = adc_init();

    qtest_writew(qts, BASE + 4, 1);
    input(qts, 0, 1650000);
    voltage(qts, "vrefh-uv", 2500000);
    voltage(qts, "vrefl-uv", 500000);
    qtest_writeb(qts, 0x8c1d6, 0x80);
    qtest_writeb(qts, 0x8c1d7, 0x80);
    start(qts, 0x1400);
    /* Sample held after 5+21=26 cycles: 812.5 ns. */
    qtest_clock_step(qts, 813);
    input(qts, 0, 2500000);
    qtest_clock_step(qts, 812);
    g_assert_cmpuint(qtest_readw(qts, ADDR(0)), ==, 2355);
    start(qts, 0x1400);
    qtest_clock_step(qts, 1625);
    g_assert_cmpuint(qtest_readw(qts, ADDR(0)), ==, 4095);
    /* HVREFDIS is inactive when the external high-side reference is used. */
    qtest_writeb(qts, BASE + 0x7c, 1);
    start(qts, 0x1400);
    qtest_clock_step(qts, 1625);
    g_assert_cmpuint(qtest_readw(qts, ADDR(0)), ==, 4095);
    qtest_writeb(qts, BASE + 0x7c, 0);
    /* Internal voltage as high-side reference requires discharge sequence. */
    voltage(qts, "internal-reference-uv", 2000000);
    qtest_writeb(qts, BASE + 0x7c, 2);
    g_assert_cmpuint(qtest_readb(qts, BASE + 0x7c), ==, 0);
    qtest_writeb(qts, BASE + 0x7c, 3);
    qtest_clock_step(qts, 999);
    qtest_writeb(qts, BASE + 0x7c, 2);
    g_assert_cmpuint(qtest_readb(qts, BASE + 0x7c), ==, 3);
    qtest_clock_step(qts, 1);
    qtest_writeb(qts, BASE + 0x7c, 2);
    qtest_clock_step(qts, 5000);
    input(qts, 0, 1250000);
    start(qts, 0x1400);
    qtest_clock_step(qts, 1625);
    g_assert_cmpuint(qtest_readw(qts, ADDR(0)), ==, 2048);
    /* Extended inputs also use externally supplied voltages. */
    qtest_writeb(qts, BASE + 0x7c, 0);
    qtest_writeb(qts, 0x8c1d6, 0);
    qtest_writeb(qts, 0x8c1d7, 0);
    qtest_writew(qts, BASE + 4, 0);
    voltage(qts, "temperature-uv", 825000);
    qtest_writeb(qts, BASE + 0x70, 159); /* At least 5 us sampling. */
    qtest_writew(qts, BASE + 0x12, 0x100);
    start(qts, 0x1400);
    qtest_clock_step(qts, 5969); /* Discard the first conversion. */
    qtest_clock_step(qts, 5000);
    start(qts, 0x1400);
    qtest_clock_step(qts, 5969);
    g_assert_cmpuint(qtest_readw(qts, BASE + 0x1a), ==, 1024);
    qtest_writeb(qts, BASE + 0x71, 159);
    qtest_writew(qts, BASE + 0x12, 0x200);
    voltage(qts, "internal-reference-uv", 1650000);
    start(qts, 0x1400);
    qtest_clock_step(qts, 5969);
    qtest_clock_step(qts, 5000);
    start(qts, 0x1400);
    qtest_clock_step(qts, 5969);
    g_assert_cmpuint(qtest_readw(qts, BASE + 0x1c), ==, 2048);
    qtest_quit(qts);
}

static void test_access_reset(void)
{
    QTestState *qts = adc_init();

    g_assert_cmphex(qtest_readb(qts, BASE + 0x60), ==, 0x14);
    qtest_writeb(qts, CSR, 0xff); /* Requires word access. */
    g_assert_cmpuint(qtest_readw(qts, CSR), ==, 0);
    qtest_writew(qts, BASE + 6, 0xffff);
    g_assert_cmphex(qtest_readw(qts, BASE + 6), ==, 0x20);
    qtest_writew(qts, BASE + 6, 0);
    qtest_writew(qts, ADDR(0), 0xffff);
    g_assert_cmpuint(qtest_readw(qts, ADDR(0)), ==, 0);
    qtest_writew(qts, BASE + 4, 1);
    input(qts, 0, 1650000);
    start(qts, 0x1400);
    qtest_clock_step(qts, 500);
    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    qtest_clock_step(qts, 100000);
    g_assert_cmpuint(qtest_readb(qts, IR(102)), ==, 0);
    qtest_writel(qts, MSTPCRA, RUNNING); /* Protected write ignored. */
    g_assert_cmpuint(qtest_readb(qts, BASE + 0x60), ==, 0);
    qtest_writew(qts, 0x803fe, 0xa503);
    qtest_writel(qts, MSTPCRA, RUNNING);
    qtest_clock_step(qts, 1000);
    g_assert_cmpuint(qtest_readw(qts, BASE + 4), ==, 0);
    g_assert_cmpuint(qtest_readw(qts, ADDR(0)), ==, 0);
    g_assert_cmphex(qtest_readb(qts, BASE + 0x60), ==, 0x14);
    qtest_writew(qts, BASE + 4, 1);
    start(qts, 0x1400);
    qtest_clock_step(qts, 104000); /* Reset LOCO/8 = 500 kHz, 52 cycles. */
    g_assert_cmpuint(qtest_readw(qts, ADDR(0)), ==, 2048);
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
    QTestState *src = adc_init();
    QTestState *dst;
    int64_t now;
    g_autofree char *dir = g_dir_make_tmp("rx113-s12ad-XXXXXX", NULL);
    g_autofree char *file = g_strdup_printf("%s/state", dir);
    g_autofree char *uri = g_strdup_printf("file:%s", file);

    g_assert_nonnull(dir);
    qtest_writew(src, BASE + 4, 1);
    input(src, 0, 1650000);
    start(src, 0x1400);
    now = qtest_clock_step(src, 901); /* Sample held, conversion unfinished. */
    input(src, 0, 825000);
    qtest_writel(src, SCKCR, 1);
    if (stopped) {
        qtest_writel(src, MSTPCRA, STOPPED);
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
        qtest_writel(dst, MSTPCRA, RUNNING);
    }
    /* Remaining 724ns at old rate; only final PCLK is not slowed. */
    qtest_clock_step(dst, 1416);
    g_assert_cmpuint(qtest_readb(dst, IR(102)), ==, 0);
    qtest_clock_step(dst, 1);
    g_assert_cmpuint(qtest_readb(dst, IR(102)), ==, 1);
    g_assert_cmpuint(qtest_readw(dst, ADDR(0)), ==, 2048);
    qtest_writeb(dst, IR(102), 0);
    qtest_clock_step(dst, 1000);
    start(dst, 0x1400);
    qtest_clock_step(dst, 3157); /* (3 + 49*2)*31.25 = 3156.25 */
    g_assert_cmpuint(qtest_readw(dst, ADDR(0)), ==, 1024);
    qtest_quit(dst);
    qtest_quit(src);
    unlink(file);
    rmdir(dir);
}

static void test_phase_clocks(void)
{
    QTestState *qts = adc_init();
    /* Inside each clock-domain phase, including fractional ADCLK work. */
    static const unsigned elapsed[] = { 31, 101, 501, 901, 1551, 1611 };

    qtest_writew(qts, BASE + 4, 1);
    input(qts, 0, 1650000);
    for (unsigned i = 0; i < G_N_ELEMENTS(elapsed); i++) {
        qtest_writel(qts, SCKCR, 0);
        start(qts, 0x1400);
        qtest_clock_step(qts, elapsed[i]);
        qtest_writel(qts, SCKCR, 0x101); /* Both clocks /2. */
        qtest_clock_step(qts, (1625 - elapsed[i]) * 2 - 1);
        g_assert_cmpuint(qtest_readb(qts, IR(102)), ==, 0);
        qtest_clock_step(qts, 1);
        g_assert_cmpuint(qtest_readb(qts, IR(102)), ==, 1);
        qtest_writeb(qts, IR(102), 0);
    }
    for (unsigned i = 0; i < G_N_ELEMENTS(elapsed); i++) {
        qtest_writel(qts, SCKCR, 0);
        start(qts, 0x1400);
        qtest_clock_step(qts, elapsed[i]);
        qtest_writel(qts, MSTPCRA, STOPPED);
        qtest_clock_step(qts, 1000000);
        qtest_writel(qts, MSTPCRA, RUNNING);
        qtest_clock_step(qts, 1625 - elapsed[i] - 1);
        g_assert_cmpuint(qtest_readb(qts, IR(102)), ==, 0);
        qtest_clock_step(qts, 1);
        g_assert_cmpuint(qtest_readb(qts, IR(102)), ==, 1);
        qtest_writeb(qts, IR(102), 0);
        qtest_clock_step(qts, 1000); /* Analog restart before a fresh scan. */
    }
    /* Forced stop discards the unfinished conversion in every phase. */
    for (unsigned i = 0; i < G_N_ELEMENTS(elapsed); i++) {
        qtest_writew(qts, ADDR(0), 0); /* Read-only, cannot clear old data. */
        input(qts, 0, 825000);
        start(qts, 0x1400);
        qtest_clock_step(qts, elapsed[i]);
        qtest_writew(qts, CSR, 0x1400);
        qtest_clock_step(qts, 1000000);
        g_assert_cmpuint(qtest_readb(qts, IR(102)), ==, 0);
        /* Data can already have been committed in the end phases. */
        g_assert_cmpuint(qtest_readw(qts, CSR), ==, 0x1400);
    }
    qtest_quit(qts);
}

static void test_mixed_addition(void)
{
    QTestState *qts = adc_init();

    qtest_writew(qts, BASE + 4, 3);
    qtest_writew(qts, BASE + 6, 0x20);
    qtest_writew(qts, BASE + 8, 1);
    qtest_writew(qts, BASE + 0xa, 0x20);
    qtest_writeb(qts, BASE + 0xc, 1);
    input(qts, 0, 825000);
    input(qts, 1, 1650000);
    input(qts, 21, 2475000);
    start(qts, 0x1400);
    qtest_clock_step(qts, 1532); /* First addition conversion committed. */
    g_assert_cmpuint(qtest_readw(qts, ADDR(0)), ==, 4096);
    input(qts, 0, 1650000);
    qtest_clock_step(qts, 1375);
    g_assert_cmpuint(qtest_readw(qts, ADDR(0)), ==, (1024 + 2048) << 2);
    g_assert_cmpuint(qtest_readw(qts, ADDR(1)), ==, 0);
    qtest_clock_step(qts, 1375);
    g_assert_cmpuint(qtest_readw(qts, ADDR(1)), ==, 2048);
    qtest_clock_step(qts, 1375);
    g_assert_cmpuint(qtest_readw(qts, ADDR(21)), ==, 3072 << 2);
    qtest_clock_step(qts, 1375);
    g_assert_cmpuint(qtest_readw(qts, ADDR(21)), ==, 6144 << 2);
    qtest_clock_step(qts, 92);
    g_assert_cmpuint(qtest_readb(qts, IR(102)), ==, 0);
    qtest_clock_step(qts, 1);
    g_assert_cmpuint(qtest_readb(qts, IR(102)), ==, 1);
    qtest_quit(qts);
}

static void test_group_double(void)
{
    QTestState *qts = adc_init();

    qtest_writew(qts, BASE + 0x14, 1 << 1);
    qtest_writew(qts, BASE + 0x10, 0x0102);
    input(qts, 0, 825000);
    input(qts, 1, 1650000);
    qtest_writew(qts, CSR, 0x36c0); /* Group, double, both IRQs enabled. */
    trigger(qts, 1);
    qtest_clock_step(qts, 1625);
    g_assert_cmpuint(qtest_readb(qts, IR(102)), ==, 0);
    g_assert_cmpuint(qtest_readw(qts, ADDR(0)), ==, 1024);
    trigger(qts, 2); /* Group B must not consume the group A double state. */
    qtest_clock_step(qts, 1625);
    g_assert_cmpuint(qtest_readb(qts, IR(103)), ==, 1);
    g_assert_cmpuint(qtest_readw(qts, ADDR(1)), ==, 2048);
    g_assert_cmpuint(qtest_readw(qts, BASE + 0x18), ==, 0);
    qtest_writeb(qts, IR(103), 0);
    input(qts, 0, 2475000);
    trigger(qts, 1);
    qtest_clock_step(qts, 1624);
    g_assert_cmpuint(qtest_readb(qts, IR(102)), ==, 0);
    qtest_clock_step(qts, 1);
    g_assert_cmpuint(qtest_readb(qts, IR(102)), ==, 1);
    g_assert_cmpuint(qtest_readb(qts, IR(103)), ==, 0);
    g_assert_cmpuint(qtest_readw(qts, ADDR(0)), ==, 1024);
    g_assert_cmpuint(qtest_readw(qts, BASE + 0x18), ==, 3072);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/rx113-s12ad/voltage", test_voltage);
    qtest_add_func("/rx113-s12ad/scan-order", test_scan_order);
    qtest_add_func("/rx113-s12ad/continuous", test_continuous);
    qtest_add_func("/rx113-s12ad/triggers", test_triggers);
    qtest_add_func("/rx113-s12ad/timing", test_timing);
    qtest_add_func("/rx113-s12ad/clock-stop", test_clock_stop);
    qtest_add_func("/rx113-s12ad/addition-double", test_addition_double);
    qtest_add_func("/rx113-s12ad/reference-sample", test_reference_sample);
    qtest_add_func("/rx113-s12ad/access-reset", test_access_reset);
    qtest_add_func("/rx113-s12ad/phase-clocks", test_phase_clocks);
    qtest_add_func("/rx113-s12ad/mixed-addition", test_mixed_addition);
    qtest_add_func("/rx113-s12ad/group-double", test_group_double);
    qtest_add_data_func("/rx113-s12ad/migration-running",
                        GINT_TO_POINTER(0), test_migration);
    qtest_add_data_func("/rx113-s12ad/migration-stopped",
                        GINT_TO_POINTER(1), test_migration);
    return g_test_run();
}
