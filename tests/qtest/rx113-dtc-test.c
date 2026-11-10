/*
 * RX113 DTCa tests (R01UH0448EJ0120, sections 11, 14 and 16).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define BASE 0x82400
#define IR(n) (0x87000 + (n))
#define DTCE(n) (0x87100 + (n))
#define IER(n) (0x87200 + (n) / 8)
#define MSTPCRA 0x80010
#define RUNNING 0xcfffffff
#define INFO 0x1000
#define SRC 0x2000
#define DST 0x3000
#define DTC_PATH "/machine/mcu/dtc"

static QTestState *dtc_init(bool short_address)
{
    QTestState *qts = qtest_init("-M rx113-r5f51138");

    qtest_writew(qts, 0x803fe, 0xa503);
    qtest_writel(qts, MSTPCRA, RUNNING);
    qtest_writel(qts, BASE + 4, 0x400);
    qtest_writeb(qts, BASE + 8, short_address);
    qtest_writeb(qts, BASE + 12, 1);
    return qts;
}

static void enable(QTestState *qts, unsigned n, unsigned info)
{
    qtest_writel(qts, qtest_readl(qts, BASE + 4) + 4 * n, info);
    qtest_writeb(qts, DTCE(n), 1);
    qtest_writeb(qts, IER(n), qtest_readb(qts, IER(n)) | (1 << (n % 8)));
}

static void descriptor(QTestState *qts, bool short_address, unsigned info,
                       unsigned mra, unsigned mrb, unsigned sar, unsigned dar,
                       unsigned cra, unsigned crb)
{
    if (short_address) {
        qtest_writel(qts, info, (mra << 24) | (sar & 0xffffff));
        qtest_writel(qts, info + 4, (mrb << 24) | (dar & 0xffffff));
        qtest_writel(qts, info + 8, (cra << 16) | crb);
    } else {
        qtest_writel(qts, info, (mra << 24) | (mrb << 16));
        qtest_writel(qts, info + 4, sar);
        qtest_writel(qts, info + 8, dar);
        qtest_writel(qts, info + 12, (cra << 16) | crb);
    }
}

static void pulse(QTestState *qts, unsigned n)
{
    qtest_set_irq_in(qts, DTC_PATH, NULL, n, 1);
    qtest_set_irq_in(qts, DTC_PATH, NULL, n, 0);
}

static int64_t swint(QTestState *qts)
{
    qtest_writeb(qts, 0x872e0, 1);
    return qtest_clock_step(qts, 1);
}

static void test_normal(const void *opaque)
{
    unsigned config = GPOINTER_TO_UINT(opaque);
    bool sh = config & 1;
    unsigned sz = (config >> 1) & 3;
    unsigned sm = (config >> 3) & 3;
    unsigned dm = (config >> 5) & 3;
    unsigned bytes = 1 << sz;
    int si = sm < 2 ? 0 : sm == 2 ? bytes : -(int)bytes;
    int di = dm < 2 ? 0 : dm == 2 ? bytes : -(int)bytes;
    QTestState *qts = dtc_init(sh);
    unsigned off = sh ? 0 : 4;

    descriptor(qts, sh, INFO, (sz << 4) | (sm << 2), dm << 2,
               SRC + 16, DST + 16, 3, 0x3456);
    for (unsigned i = 0; i < 32; i++) {
        qtest_writeb(qts, SRC + i, i + 0x40);
    }
    enable(qts, 27, INFO);
    qtest_irq_intercept_out_named(qts, "/machine/mcu/icu", "sysbus-irq");
    qtest_writeb(qts, 0x87303, 5);
    for (unsigned i = 0; i < 3; i++) {
        /* ICU output interception also disconnects SWINT, so inject here. */
        pulse(qts, 27);
        qtest_clock_step(qts, 1);
        for (unsigned j = 0; j < bytes; j++) {
            g_assert_cmpuint(qtest_readb(qts, DST + 16 + di * i + j), ==,
                             0x50 + si * i + j);
        }
        g_assert_cmpuint(qtest_readl(qts, INFO + off) & 0xffffff, ==,
                         SRC + 16 + si * (i + 1));
        g_assert_cmpuint(qtest_readl(qts, INFO + off + 4) & 0xffffff, ==,
                         DST + 16 + di * (i + 1));
        g_assert_cmphex(qtest_readl(qts, INFO + off + 8), ==,
                        ((2 - i) << 16) | 0x3456);
        g_assert_cmpuint(qtest_readb(qts, IR(27)), ==, i == 2);
        g_assert_cmpuint(qtest_readb(qts, DTCE(27)), ==, i != 2);
        g_assert_cmpuint(qtest_get_irq(qts, 0), ==, i == 2);
    }
    g_assert_cmpuint(qtest_readw(qts, BASE + 14) & 0x8000, ==, 0);
    qtest_quit(qts);
}

static void test_repeat(const void *opaque)
{
    unsigned config = GPOINTER_TO_UINT(opaque);
    bool sh = config & 1;
    bool source = config & 2;
    QTestState *qts = dtc_init(sh);
    unsigned off = sh ? 0 : 4;

    descriptor(qts, sh, INFO, 0x48, 8 | (source ? 0x10 : 0),
               SRC, DST, 0x0303, 0x1234);
    for (unsigned i = 0; i < 9; i++) {
        qtest_writeb(qts, SRC + i, 0x90 + i);
    }
    enable(qts, 27, INFO);
    qtest_writeb(qts, BASE, 0x18); /* Also exercise cached repeat state. */
    for (unsigned i = 0; i < 9; i++) {
        swint(qts);
        g_assert_cmpuint(qtest_readb(qts, DST + (source ? i : i % 3)), ==,
                         0x90 + (source ? i % 3 : i));
        g_assert_cmpuint(qtest_readl(qts, INFO + off) & 0xffffff, ==,
                         SRC + (source ? (i + 1) % 3 : i + 1));
        g_assert_cmpuint(qtest_readl(qts, INFO + off + 4) & 0xffffff, ==,
                         DST + (source ? i + 1 : (i + 1) % 3));
        g_assert_cmphex(qtest_readl(qts, INFO + off + 8), ==,
                        ((0x303 - (i + 1) % 3) << 16) | 0x1234);
        g_assert_cmpuint(qtest_readb(qts, IR(27)), ==, 0);
        g_assert_cmpuint(qtest_readb(qts, DTCE(27)), ==, 1);
    }
    qtest_quit(qts);
}

static void test_block(const void *opaque)
{
    unsigned config = GPOINTER_TO_UINT(opaque);
    bool sh = config & 1;
    bool source = config & 2;
    unsigned count = config & 4 ? 256 : 3;
    unsigned cra = count == 256 ? 0 : 0x303;
    QTestState *qts = dtc_init(sh);
    unsigned off = sh ? 0 : 4;

    descriptor(qts, sh, INFO, 0xa8, 8 | (source ? 0x10 : 0),
               SRC, DST, cra, 2);
    for (unsigned i = 0; i < count * 8; i++) {
        qtest_writeb(qts, SRC + i, i * 7 + 3);
    }
    enable(qts, 27, INFO);
    for (unsigned i = 0; i < 2; i++) {
        swint(qts);
        for (unsigned j = 0; j < count * 4; j++) {
            g_assert_cmpuint(qtest_readb(qts, DST +
                                         (source ? i * count * 4 : 0) + j), ==,
                             ((source ? j : i * count * 4 + j) * 7 + 3) & 255);
        }
        g_assert_cmpuint(qtest_readl(qts, INFO + off) & 0xffffff, ==,
                         SRC + (source ? 0 : (i + 1) * count * 4));
        g_assert_cmpuint(qtest_readl(qts, INFO + off + 4) & 0xffffff, ==,
                         DST + (source ? (i + 1) * count * 4 : 0));
        g_assert_cmphex(qtest_readl(qts, INFO + off + 8), ==,
                        (cra << 16) | (1 - i));
        g_assert_cmpuint(qtest_readb(qts, IR(27)), ==, i == 1);
    }
    qtest_quit(qts);
}

static void test_chain(const void *opaque)
{
    unsigned cfg = GPOINTER_TO_UINT(opaque);
    bool sh = cfg & 1;
    bool conditional = cfg & 2;
    QTestState *qts = dtc_init(sh);
    unsigned stride = sh ? 12 : 16;

    descriptor(qts, sh, INFO, 8, conditional ? 0xc8 : 0x88,
               SRC, DST, 2, 0);
    descriptor(qts, sh, INFO + stride, 0x18, 8,
               SRC + 4, DST + 4, conditional ? 1 : 2, 0);
    qtest_writel(qts, SRC, 0x01020304);
    qtest_writel(qts, SRC + 4, 0xabcd1234);
    enable(qts, 27, INFO | 3); /* Descriptor alignment is masked. */
    qtest_writeb(qts, BASE, 0x18);
    swint(qts);
    g_assert_cmpuint(qtest_readb(qts, DST), ==, 4);
    g_assert_cmpuint(qtest_readw(qts, BASE + 14) & 0x8000, ==,
                     conditional ? 0 : 0x8000);
    if (!conditional) {
        g_assert_cmpuint(qtest_readb(qts, IR(27)), ==, 1);
        qtest_clock_step(qts, 1);
        g_assert_cmphex(qtest_readw(qts, DST + 4), ==, 0x1234);
    }
    swint(qts);
    g_assert_cmpuint(qtest_readw(qts, BASE + 14) & 0x8000, ==, 0x8000);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readw(qts, DST + (conditional ? 4 : 6)), ==,
                    conditional ? 0x1234 : 0xabcd);
    g_assert_cmpuint(qtest_readb(qts, DTCE(27)), ==, 0);
    g_assert_cmpuint(qtest_readb(qts, IR(27)), ==, 1);
    qtest_quit(qts);
}

static void test_gates_priority(void)
{
    QTestState *qts = dtc_init(false);

    descriptor(qts, false, INFO, 8, 8, SRC, DST, 2, 0);
    descriptor(qts, false, INFO + 16, 8, 8, SRC + 8, DST + 8, 2, 0);
    qtest_writel(qts, SRC, 0x12345678);
    qtest_writel(qts, SRC + 8, 0xabcdef01);
    enable(qts, 27, INFO);
    enable(qts, 102, INFO + 16);
    qtest_writeb(qts, BASE + 12, 0);
    qtest_writeb(qts, 0x87303, 0); /* DTC ignores IPR, including zero. */
    qtest_writeb(qts, 0x87366, 15);
    pulse(qts, 102);
    pulse(qts, 27);
    qtest_clock_step(qts, 100);
    g_assert_cmpuint(qtest_readl(qts, DST), ==, 0);
    qtest_writeb(qts, BASE + 12, 1);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readb(qts, DST), ==, 0x78);
    g_assert_cmpuint(qtest_readb(qts, DST + 8), ==, 0);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readb(qts, DST + 8), ==, 1);
    qtest_writel(qts, MSTPCRA, RUNNING | (1U << 28));
    pulse(qts, 27);
    qtest_clock_step(qts, 100);
    g_assert_cmpuint(qtest_readb(qts, DST + 1), ==, 0);
    qtest_writeb(qts, BASE + 12, 0); /* Inaccessible during MSTP. */
    qtest_writel(qts, MSTPCRA, RUNNING);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readb(qts, DST + 1), ==, 0x56);
    /* IER gates activation but retains IR, then resumes without a new edge. */
    qtest_writeb(qts, IER(102), 0);
    pulse(qts, 102);
    qtest_clock_step(qts, 100);
    g_assert_cmpuint(qtest_readb(qts, DST + 9), ==, 0);
    qtest_writeb(qts, IER(102), 0x40);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readb(qts, DST + 9), ==, 0xef);
    qtest_quit(qts);
}

static void test_cache_disel(void)
{
    QTestState *qts = dtc_init(false);

    descriptor(qts, false, INFO, 8, 0x28, SRC, DST, 3, 0);
    qtest_writel(qts, SRC, 0x12345678);
    enable(qts, 27, INFO);
    qtest_writeb(qts, BASE, 0x18);
    swint(qts);
    g_assert_cmpuint(qtest_readb(qts, IR(27)), ==, 1);
    g_assert_cmpuint(qtest_readb(qts, DTCE(27)), ==, 1);
    swint(qts); /* IR=1 coalesces a further request. */
    g_assert_cmpuint(qtest_readl(qts, INFO + 12) >> 16, ==, 2);
    qtest_writeb(qts, IR(27), 0);
    descriptor(qts, false, INFO, 8, 8, SRC + 12, DST + 12, 5, 0);
    swint(qts); /* RRS retains prior registers despite memory edits. */
    g_assert_cmphex(qtest_readb(qts, DST + 1), ==, 0x56);
    g_assert_cmpuint(qtest_readl(qts, INFO + 12) >> 16, ==, 1);
    qtest_writeb(qts, IR(27), 0);
    qtest_writeb(qts, BASE, 8);
    qtest_writeb(qts, BASE, 0x18);
    descriptor(qts, false, INFO, 8, 8, SRC + 2, DST + 12, 1, 0);
    swint(qts);
    g_assert_cmphex(qtest_readb(qts, DST + 12), ==, 0x34);
    g_assert_cmpuint(qtest_readb(qts, DTCE(27)), ==, 0);
    qtest_quit(qts);
}

static void test_registers_reset(void)
{
    QTestState *qts = dtc_init(false);

    g_assert_cmphex(qtest_readb(qts, BASE), ==, 8);
    qtest_writeb(qts, BASE, 0xff);
    g_assert_cmphex(qtest_readb(qts, BASE), ==, 0x18);
    qtest_writel(qts, BASE + 4, 0x0fffffff);
    g_assert_cmphex(qtest_readl(qts, BASE + 4), ==, 0xfffffc00);
    qtest_writel(qts, BASE + 4, 0xf7ffffff);
    g_assert_cmphex(qtest_readl(qts, BASE + 4), ==, 0x07fffc00);
    qtest_writeb(qts, BASE + 4, 0);
    g_assert_cmphex(qtest_readl(qts, BASE + 4), ==, 0x07fffc00);
    qtest_writel(qts, BASE + 4, 0);
    qtest_writeb(qts, DTCE(118), 1); /* MTU overflow cannot start DTC. */
    g_assert_cmpuint(qtest_readb(qts, DTCE(118)), ==, 0);
    descriptor(qts, false, INFO, 8, 8, SRC, DST, 0, 0);
    enable(qts, 27, INFO);
    swint(qts);
    g_assert_cmpuint(qtest_readl(qts, INFO + 12) >> 16, ==, 65535);
    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    qtest_writel(qts, MSTPCRA, RUNNING | (1U << 28));
    /* PRCR protection restored; MSTPA28 resets to zero (§11.2.2). */
    g_assert_cmphex(qtest_readb(qts, BASE), ==, 8);
    qtest_writew(qts, 0x803fe, 0xa502);
    qtest_writel(qts, MSTPCRA, RUNNING);
    g_assert_cmphex(qtest_readb(qts, BASE), ==, 8);
    g_assert_cmpuint(qtest_readl(qts, BASE + 4), ==, 0);
    g_assert_cmpuint(qtest_readb(qts, BASE + 8), ==, 0);
    g_assert_cmpuint(qtest_readb(qts, BASE + 12), ==, 0);
    g_assert_cmpuint(qtest_readw(qts, BASE + 14), ==, 0);
    g_assert_cmpuint(qtest_readb(qts, DTCE(27)), ==, 0);
    qtest_quit(qts);
}

static void test_short_flash(void)
{
    QTestState *qts = dtc_init(true);
    uint8_t rom[4];

    qtest_memread(qts, 0xfffffff0, rom, sizeof(rom));
    descriptor(qts, true, INFO, 0x28, 8, 0xfffff0, DST, 1, 0);
    enable(qts, 27, INFO);
    swint(qts);
    for (unsigned i = 0; i < 4; i++) {
        g_assert_cmpuint(qtest_readb(qts, DST + i), ==, rom[i]);
    }
    g_assert_cmphex(qtest_readl(qts, INFO), ==, 0x28fffff4);
    qtest_quit(qts);
}

static void test_peripherals(void)
{
    QTestState *qts = dtc_init(false);

    /* HOCO/PCLKB = 32 MHz; release ADC, MTU and SCI0 module stops. */
    qtest_writeb(qts, 0x80036, 0);
    qtest_clock_step(qts, 34500);
    qtest_writew(qts, 0x80026, 0x100);
    qtest_writel(qts, 0x80020, 0);
    qtest_writel(qts, MSTPCRA, RUNNING & ~((1 << 17) | (1 << 9)));
    qtest_writel(qts, 0x80014, 0x7fffffff);
    qtest_clock_step(qts, 1000);
    descriptor(qts, false, INFO, 0x10, 8, 0x89020, DST, 1, 0);
    enable(qts, 102, INFO);
    qtest_qmp_assert_success(qts, "{'execute':'qom-set','arguments':{"
                            "'path':'/machine/mcu/s12ad',"
                            "'property':'an000-uv','value':1650000}}");
    qtest_writew(qts, 0x89004, 1);
    qtest_writew(qts, 0x89000, 0x9400);
    qtest_clock_step(qts, 1626);
    g_assert_cmphex(qtest_readw(qts, DST), ==, 2048);
    g_assert_cmpuint(qtest_readb(qts, DTCE(102)), ==, 0);

    descriptor(qts, false, INFO + 16, 0, 8, SRC, DST + 4, 1, 0);
    qtest_writeb(qts, SRC, 0x73);
    enable(qts, 114, INFO + 16);
    qtest_writeb(qts, 0x88700, 1); /* MTU0 /4 */
    qtest_writeb(qts, 0x88704, 1);
    qtest_writew(qts, 0x88708, 9);
    qtest_writeb(qts, 0x88680, 1);
    qtest_clock_step(qts, 1251);
    g_assert_cmphex(qtest_readb(qts, DST + 4), ==, 0x73);
    qtest_writeb(qts, 0x88680, 0);

    descriptor(qts, false, INFO + 32, 0, 8, SRC, DST + 8, 1, 0);
    enable(qts, 216, INFO + 32);
    qtest_writeb(qts, 0x8a002, 0xa0); /* SCI0 TE+TIE produces TXI0. */
    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readb(qts, DST + 8), ==, 0x73);
    g_assert_cmpuint(qtest_readb(qts, DTCE(216)), ==, 0);
    qtest_quit(qts);
}

static void test_retention_writeback(void)
{
    QTestState *qts = dtc_init(false);

    /* Two SCI requests held while stopped; one is retained, not lost. */
    descriptor(qts, false, INFO, 8, 8, SRC, DST, 3, 0);
    qtest_writel(qts, SRC, 0x12345678);
    enable(qts, 216, INFO);
    qtest_writeb(qts, BASE + 12, 0);
    pulse(qts, 216);
    pulse(qts, 216);
    qtest_writeb(qts, BASE + 12, 1);
    qtest_clock_step(qts, 2);
    g_assert_cmphex(qtest_readw(qts, DST), ==, 0x5678);
    g_assert_cmpuint(qtest_readl(qts, INFO + 12) >> 16, ==, 1);

    /* A fixed SAR/DAR must not overwrite descriptor edits made by DMA. */
    descriptor(qts, false, INFO + 16, 0x20, 0, SRC + 8, INFO + 20, 2, 0);
    qtest_writel(qts, SRC + 8, SRC + 12);
    enable(qts, 27, INFO + 16);
    swint(qts);
    g_assert_cmphex(qtest_readl(qts, INFO + 20), ==, SRC + 12);
    qtest_quit(qts);
}

static void test_zero_counts(void)
{
    QTestState *qts = dtc_init(true);

    /* Repeat size zero means 256, including decrement/rewind at the end. */
    descriptor(qts, true, INFO, 0x4c, 0x18, SRC + 255, DST, 0, 0x1234);
    for (unsigned i = 0; i < 256; i++) {
        qtest_writeb(qts, SRC + i, i);
    }
    enable(qts, 27, INFO);
    for (unsigned i = 0; i < 256; i++) {
        swint(qts);
        g_assert_cmpuint(qtest_readb(qts, DST + i), ==, 255 - i);
    }
    g_assert_cmphex(qtest_readl(qts, INFO), ==, 0x4c000000 | (SRC + 255));
    g_assert_cmphex(qtest_readl(qts, INFO + 8), ==, 0x1234);
    g_assert_cmpuint(qtest_readb(qts, IR(27)), ==, 0);
    /* Block count zero means 65536, not an already-completed transfer. */
    descriptor(qts, true, INFO, 0x88, 8, SRC, DST, 0x101, 0);
    swint(qts);
    g_assert_cmphex(qtest_readl(qts, INFO + 8), ==, 0x0101ffff);
    g_assert_cmpuint(qtest_readb(qts, DTCE(27)), ==, 1);
    qtest_quit(qts);
}

static void test_chain_stop(void)
{
    QTestState *qts = dtc_init(false);

    descriptor(qts, false, INFO, 8, 0x88, SRC, DST, 1, 0);
    descriptor(qts, false, INFO + 16, 8, 8, SRC + 1, DST + 1, 1, 0);
    qtest_writew(qts, SRC, 0xabcd);
    enable(qts, 27, INFO);
    swint(qts);
    g_assert_cmphex(qtest_readw(qts, BASE + 14), ==, 0x801b);
    qtest_writeb(qts, BASE + 12, 0);
    qtest_writel(qts, MSTPCRA, RUNNING | (1U << 28));
    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readw(qts, DST), ==, 0xabcd);
    g_assert_cmpuint(qtest_readb(qts, DTCE(27)), ==, 0);
    qtest_writel(qts, MSTPCRA, RUNNING);
    g_assert_cmpuint(qtest_readw(qts, BASE + 14) & 0x8000, ==, 0);
    g_assert_cmpuint(qtest_readb(qts, BASE + 12), ==, 0);
    qtest_quit(qts);
}

static void test_external_and_destination(void)
{
    QTestState *qts = dtc_init(false);

    /* External falling edge, as required by §14.4.3. */
    qtest_writeb(qts, 0x87500, 4);
    descriptor(qts, false, INFO, 8, 8, SRC, DST, 2, 0);
    qtest_writew(qts, SRC, 0xabcd);
    enable(qts, 64, INFO);
    pulse(qts, 64);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readb(qts, DST), ==, 0xcd);
    qtest_writeb(qts, BASE + 12, 0);
    pulse(qts, 64);
    qtest_clock_step(qts, 1);
    g_assert_cmpuint(qtest_readb(qts, IR(64)), ==, 1);
    qtest_irq_intercept_out_named(qts, "/machine/mcu/icu", "sysbus-irq");
    qtest_writeb(qts, 0x87340, 5);
    qtest_writeb(qts, DTCE(64), 0); /* CPU destination while DTCST=0. */
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_cmpuint(qtest_readb(qts, DST + 1), ==, 0);
    qtest_quit(qts);
}

static void test_status_and_fault(void)
{
    QTestState *qts = dtc_init(false);

    descriptor(qts, false, INFO, 0x10, 0, BASE + 14, DST, 1, 0);
    enable(qts, 27, INFO);
    swint(qts);
    g_assert_cmphex(qtest_readw(qts, DST), ==, 0x801b);
    qtest_writeb(qts, IR(27), 0);
    descriptor(qts, false, INFO, 0xc0, 8, SRC, DST + 8, 1, 0);
    enable(qts, 27, INFO);
    swint(qts); /* Prohibited mode logs and stops without moving data. */
    g_assert_cmpuint(qtest_readb(qts, BASE + 12), ==, 0);
    g_assert_cmpuint(qtest_readb(qts, DST + 8), ==, 0);
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
    unsigned scenario = GPOINTER_TO_UINT(opaque);
    bool active = scenario != 0;
    QTestState *src = dtc_init(true);
    QTestState *dst;
    g_autofree char *dir = g_dir_make_tmp("rx113-dtc-XXXXXX", NULL);
    g_autofree char *file = g_strdup_printf("%s/state", dir);
    g_autofree char *uri = g_strdup_printf("file:%s", file);
    int64_t now;

    descriptor(src, true, INFO, 8, 0x88, SRC, DST, 1, 0);
    descriptor(src, true, INFO + 12, 8, 8, SRC + 1, DST + 1, 1, 0);
    qtest_writew(src, SRC, 0xabcd);
    if (scenario == 2) {
        descriptor(src, true, INFO, 8, 8, SRC, DST, 3, 0);
        qtest_writeb(src, BASE, 0x18);
    }
    enable(src, 27, INFO);
    if (!active) {
        qtest_writeb(src, BASE + 12, 0);
    }
    now = swint(src);
    qtest_qmp_assert_success(src, "{'execute':'migrate',"
                            "'arguments':{'uri':%s}}", uri);
    migration_wait(src);
    dst = qtest_init("-M rx113-r5f51138 -incoming defer");
    qtest_clock_set(dst, now);
    qtest_qmp_assert_success(dst, "{'execute':'migrate-incoming',"
                            "'arguments':{'uri':%s}}", uri);
    migration_wait(dst);
    if (!active) {
        qtest_writeb(dst, BASE + 12, 1);
    }
    if (scenario == 2) {
        /* Cached registers, not this edited descriptor, must be restored. */
        descriptor(dst, true, INFO, 8, 8, SRC + 8, DST + 8, 1, 0);
        swint(dst);
    }
    qtest_clock_step(dst, 2);
    g_assert_cmphex(qtest_readw(dst, DST), ==, 0xabcd);
    g_assert_cmpuint(qtest_readb(dst, DTCE(27)), ==, scenario == 2);
    g_assert_cmpuint(qtest_readb(dst, IR(27)), ==, scenario != 2);
    qtest_quit(dst);
    qtest_quit(src);
    unlink(file);
    rmdir(dir);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    for (unsigned sh = 0; sh < 2; sh++) {
        for (unsigned sz = 0; sz < 3; sz++) {
            for (unsigned sm = 0; sm < 4; sm++) {
                for (unsigned dm = 0; dm < 4; dm++) {
                    unsigned cfg = sh | (sz << 1) | (sm << 3) | (dm << 5);
                    g_autofree char *name = g_strdup_printf(
                        "/rx113-dtc/normal/%u-%u-%u-%u", sh, sz, sm, dm);

                    qtest_add_data_func(name, GUINT_TO_POINTER(cfg),
                                        test_normal);
                }
            }
        }
    }
    for (unsigned cfg = 0; cfg < 4; cfg++) {
        g_autofree char *r = g_strdup_printf("/rx113-dtc/repeat/%u", cfg);
        g_autofree char *c = g_strdup_printf("/rx113-dtc/chain/%u", cfg);

        qtest_add_data_func(r, GUINT_TO_POINTER(cfg), test_repeat);
        qtest_add_data_func(c, GUINT_TO_POINTER(cfg), test_chain);
    }
    for (unsigned cfg = 0; cfg < 8; cfg++) {
        g_autofree char *b = g_strdup_printf("/rx113-dtc/block/%u", cfg);

        qtest_add_data_func(b, GUINT_TO_POINTER(cfg), test_block);
    }
    qtest_add_func("/rx113-dtc/gates-priority", test_gates_priority);
    qtest_add_func("/rx113-dtc/cache-disel", test_cache_disel);
    qtest_add_func("/rx113-dtc/registers-reset", test_registers_reset);
    qtest_add_func("/rx113-dtc/short-flash", test_short_flash);
    qtest_add_func("/rx113-dtc/peripherals", test_peripherals);
    qtest_add_func("/rx113-dtc/retention-writeback", test_retention_writeback);
    qtest_add_func("/rx113-dtc/chain-stop", test_chain_stop);
    qtest_add_func("/rx113-dtc/zero-counts", test_zero_counts);
    qtest_add_func("/rx113-dtc/external-destination",
                   test_external_and_destination);
    qtest_add_func("/rx113-dtc/status-fault", test_status_and_fault);
    qtest_add_data_func("/rx113-dtc/migration-active", GINT_TO_POINTER(1),
                        test_migration);
    qtest_add_data_func("/rx113-dtc/migration-pending", GINT_TO_POINTER(0),
                        test_migration);
    qtest_add_data_func("/rx113-dtc/migration-cached", GUINT_TO_POINTER(2),
                        test_migration);
    return g_test_run();
}
