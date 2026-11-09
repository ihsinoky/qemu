/*
 * RX113 PCLKB propagation and retained module stop (manual sections 9/11).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qemu/sockets.h"

#define SCKCR 0x80020
#define SCKCR3 0x80026
#define PLLCR 0x80028
#define MSTPCRA 0x80010
#define MSTPCRB 0x80014
#define MSTPCRC 0x80018
#define PRCR 0x803fe
#define IR(n) (0x87000 + (n))

static QTestState *clock_init(void)
{
    QTestState *qts = qtest_init("-M rx113-r5f51138");

    qtest_writew(qts, PRCR, 0xa503);
    qtest_writeb(qts, 0x80036, 0); /* HOCO start */
    qtest_clock_step(qts, 34500);
    qtest_writel(qts, SCKCR, 0);
    qtest_writew(qts, SCKCR3, 0x100); /* 32 MHz HOCO, PCLKB /1 */
    return qts;
}

static uint64_t frequency(QTestState *qts, const char *child)
{
    g_autofree char *path = g_strdup_printf("/machine/mcu/%s", child);
    QDict *rsp = qtest_qmp(qts, "{'execute':'qom-get','arguments':"
                          "{'path':%s,'property':'input-freq'}}", path);
    uint64_t freq = qdict_get_int(rsp, "return");

    qobject_unref(rsp);
    return freq;
}

static void test_sources(void)
{
    QTestState *qts = clock_init();
    const char *children[] = { "cmt[0]", "cmt[1]", "tmr[0]", "tmr[1]",
                              "sci[0]", "sci[1]", "sci[2]", "sci[3]",
                              "sci[4]", "sci[5]", "sci[6]", "sci[7]" };

    /* All reset-stopped children still receive frequency changes. */
    for (unsigned i = 0; i < G_N_ELEMENTS(children); i++) {
        g_assert_cmpuint(frequency(qts, children[i]), ==, 32000000);
    }
    qtest_writew(qts, PLLCR, 0x0b01); /* 16 MHz /2 *6 = 48 MHz */
    qtest_writew(qts, SCKCR3, 0x400);
    g_assert_cmpuint(frequency(qts, "cmt[0]"), ==, 48000000);
    qtest_writel(qts, SCKCR, 0x100);
    g_assert_cmpuint(frequency(qts, "tmr[0]"), ==, 24000000);
    qtest_writew(qts, PLLCR, 0x0f02); /* /4 *8, then PCLKB /2 */
    g_assert_cmpuint(frequency(qts, "sci[0]"), ==, 16000000);
    qtest_writew(qts, 0x8002c, 0x0b01); /* UPLL cannot affect PCLKB. */
    g_assert_cmpuint(frequency(qts, "sci[5]"), ==, 16000000);
    qtest_writew(qts, SCKCR3, 0x200); /* main crystal */
    g_assert_cmpuint(frequency(qts, "sci[0]"), ==, 8000000);
    qtest_writew(qts, SCKCR3, 0); /* LOCO */
    g_assert_cmpuint(frequency(qts, "sci[0]"), ==, 2000000);
    qtest_writew(qts, SCKCR3, 0x300); /* sub-clock */
    g_assert_cmpuint(frequency(qts, "sci[0]"), ==, 16384);
    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    g_assert_cmpuint(frequency(qts, "cmt[0]"), ==, 500000);
    g_assert_cmphex(qtest_readl(qts, MSTPCRA), ==, 0xefffffff);
    qtest_quit(qts);
}

static void test_cmt(void)
{
    QTestState *qts = clock_init();

    for (unsigned unit = 0; unit < 2; unit++) {
        unsigned base = 0x88000 + unit * 0x10;
        unsigned ir = IR(28 + unit * 2);
        uint32_t run = 0xefffffff & ~(1U << (15 - unit));

        qtest_writel(qts, SCKCR, 0);
        qtest_writel(qts, MSTPCRA, run);
        qtest_writew(qts, base + 2, 0x40); /* /8, interrupt enabled */
        qtest_writew(qts, base + 6, 100);
        qtest_writew(qts, base, 1);
        qtest_clock_step(qts, 10125); /* 40.5 ticks */
        g_assert_cmpuint(qtest_readw(qts, base + 4), ==, 40);
        /* Halve the input clock with half a counter tick already elapsed. */
        qtest_writel(qts, SCKCR, 0x100);
        qtest_clock_step(qts, 29749);
        g_assert_cmphex(qtest_readb(qts, ir), ==, 0);
        qtest_clock_step(qts, 1);
        g_assert_cmphex(qtest_readb(qts, ir), ==, 1);
        qtest_writeb(qts, ir, 0);
        qtest_clock_step(qts, 5250); /* 10.5 ticks */
        qtest_writel(qts, MSTPCRA, 0xefffffff);
        g_assert_cmpuint(qtest_readw(qts, base + 4), ==, 0);
        qtest_writew(qts, base + 6, 0); /* blocked, retained CMCOR=100 */
        qtest_clock_step(qts, 1000000);
        g_assert_cmphex(qtest_readb(qts, ir), ==, 0);
        qtest_writel(qts, MSTPCRA, run);
        g_assert_cmpuint(qtest_readw(qts, base + 4), ==, 10);
        g_assert_cmpuint(qtest_readw(qts, base + 6), ==, 100);
        qtest_clock_step(qts, 249);
        g_assert_cmpuint(qtest_readw(qts, base + 4), ==, 10);
        qtest_clock_step(qts, 1);
        g_assert_cmpuint(qtest_readw(qts, base + 4), ==, 11);
        qtest_clock_step(qts, 44500);
        g_assert_cmphex(qtest_readb(qts, ir), ==, 1);
        qtest_writew(qts, base, 0);
    }
    qtest_quit(qts);
}

static void test_tmr(void)
{
    QTestState *qts = clock_init();

    for (unsigned unit = 0; unit < 2; unit++) {
        unsigned base = 0x88200 + unit * 0x10;
        uint32_t run = 0xefffffff & ~(1U << (5 - unit));

        qtest_writel(qts, MSTPCRA, run);
        for (unsigned ch = 0; ch < 2; ch++) {
            unsigned ir = IR(174 + unit * 6 + ch * 3);

            qtest_writel(qts, SCKCR, 0);
            qtest_writeb(qts, base + ch, 0x48); /* CMIEA, clear on A */
            qtest_writeb(qts, base + 4 + ch, 100);
            qtest_writeb(qts, base + 6 + ch, 150);
            qtest_writeb(qts, base + 10 + ch, 0x0b); /* internal /8 */
            qtest_clock_step(qts, 10125);
            g_assert_cmpuint(qtest_readb(qts, base + 8 + ch), ==, 40);
            qtest_writel(qts, SCKCR, 0x100);
            qtest_clock_step(qts, 29749);
            g_assert_cmphex(qtest_readb(qts, ir), ==, 0);
            qtest_clock_step(qts, 1);
            g_assert_cmphex(qtest_readb(qts, ir), ==, 1);
            qtest_writeb(qts, ir, 0);
            qtest_clock_step(qts, 5250);
            qtest_writel(qts, MSTPCRA, 0xefffffff);
            qtest_writeb(qts, base + 8 + ch, 200); /* blocked */
            qtest_clock_step(qts, 1000000);
            g_assert_cmphex(qtest_readb(qts, ir), ==, 0);
            g_assert_cmpuint(qtest_readb(qts, base + 8 + ch), ==, 0);
            qtest_writel(qts, MSTPCRA, run);
            g_assert_cmpuint(qtest_readb(qts, base + 8 + ch), ==, 10);
            qtest_clock_step(qts, 250);
            g_assert_cmpuint(qtest_readb(qts, base + 8 + ch), ==, 11);
            qtest_clock_step(qts, 44500);
            g_assert_cmphex(qtest_readb(qts, ir), ==, 1);
            qtest_writeb(qts, base + 10 + ch, 0);
        }
    }
    qtest_quit(qts);
}

static void test_tmr_cascade(void)
{
    QTestState *qts = clock_init();

    qtest_writel(qts, MSTPCRA, 0xefffffdf);
    qtest_writeb(qts, 0x88200, 0x48);
    qtest_writeb(qts, 0x88204, 1);
    qtest_writeb(qts, 0x88205, 0);
    qtest_writeb(qts, 0x88206, 2);
    qtest_writeb(qts, 0x88207, 0);
    qtest_writeb(qts, 0x8820a, 0x18); /* TMR0 counts TMR1 overflows */
    qtest_writeb(qts, 0x8820b, 0x0b); /* TMR1 input: PCLKB /8 */
    qtest_clock_step(qts, 32125); /* 128.5 ticks of the 16-bit counter */
    g_assert_cmpuint(qtest_readw(qts, 0x88208), ==, 128);
    qtest_writel(qts, SCKCR, 0x100);
    qtest_writel(qts, MSTPCRA, 0xefffffff);
    qtest_clock_step(qts, 1000000);
    qtest_writel(qts, MSTPCRA, 0xefffffdf);
    qtest_clock_step(qts, 63749);
    g_assert_cmpuint(qtest_readb(qts, IR(174)), ==, 0);
    qtest_clock_step(qts, 1);
    g_assert_cmpuint(qtest_readb(qts, IR(174)), ==, 1);
    g_assert_cmpuint(qtest_readw(qts, 0x88208), ==, 0);
    qtest_quit(qts);
}

static void test_sci(void)
{
    static const unsigned base[] = {
        0x8a000, 0x8a020, 0x8a040, 0x8a0a0, 0x8a0c0, 0x8b300,
        0x8a100, 0x8a120,
    };
    static const unsigned bit[] = { 31, 30, 29, 26, 25, 4, 27, 26 };
    QTestState *qts = clock_init();

    for (unsigned ch = 0; ch < G_N_ELEMENTS(base); ch++) {
        unsigned mstp = ch < 6 ? MSTPCRB : MSTPCRC;
        uint32_t reset = ch < 6 ? 0xffffffff : 0x7ffffffe;
        uint32_t run = reset & ~(1U << bit[ch]);

        qtest_writel(qts, SCKCR, 0);
        qtest_writel(qts, mstp, run);
        qtest_writeb(qts, base[ch] + 1, 8);
        qtest_writeb(qts, base[ch] + 2, 0x20);
        qtest_writeb(qts, base[ch] + 3, 'A');
        qtest_clock_step(qts, 36000);
        qtest_writel(qts, SCKCR, 0x100);
        qtest_clock_step(qts, 35999);
        g_assert_cmphex(qtest_readb(qts, base[ch] + 4) & 4, ==, 0);
        qtest_writel(qts, mstp, reset);
        qtest_writeb(qts, base[ch] + 1, 100); /* blocked */
        /* Repeated changes while stopped must not consume fractional work. */
        for (unsigned i = 0; i < 16; i++) {
            qtest_writel(qts, SCKCR, 0);
            qtest_writel(qts, SCKCR, 0x100);
        }
        qtest_clock_step(qts, 1000000);
        g_assert_cmpuint(qtest_readb(qts, base[ch] + 4), ==, 0);
        qtest_writel(qts, mstp, run);
        g_assert_cmpuint(qtest_readb(qts, base[ch] + 1), ==, 8);
        qtest_clock_step(qts, 36000);
        g_assert_cmphex(qtest_readb(qts, base[ch] + 4) & 4, ==, 0);
        qtest_clock_step(qts, 1);
        g_assert_cmphex(qtest_readb(qts, base[ch] + 4) & 4, ==, 4);
        /* The cached duration for the next character must also be updated. */
        qtest_writeb(qts, base[ch] + 3, 'B');
        qtest_clock_step(qts, 143999);
        g_assert_cmphex(qtest_readb(qts, base[ch] + 4) & 4, ==, 0);
        qtest_clock_step(qts, 1);
        g_assert_cmphex(qtest_readb(qts, base[ch] + 4) & 4, ==, 4);
    }
    qtest_quit(qts);
}

/* Poll host I/O without advancing virtual time. */
static void receive_ready(QTestState *qts)
{
    for (unsigned i = 0; i < 100; i++) {
        if (qtest_readb(qts, 0x8a004) & 0x40) {
            return;
        }
        g_usleep(1000);
    }
    g_assert_not_reached();
}

static void test_receive(void)
{
    int fd;
    QTestState *qts = qtest_init_with_serial("-M rx113-r5f51138", &fd);

    qtest_writew(qts, PRCR, 0xa503);
    qtest_writeb(qts, 0x80036, 0);
    qtest_clock_step(qts, 34500);
    qtest_writew(qts, SCKCR3, 0x100);
    qtest_writel(qts, SCKCR, 0);
    qtest_writel(qts, MSTPCRB, 0x7fffffff);
    qtest_writeb(qts, 0x8a001, 8);
    qtest_writeb(qts, 0x8a002, 0x50); /* receive, RXI enabled */
    g_assert_cmpint(send(fd, "A", 1, 0), ==, 1);
    receive_ready(qts);
    g_assert_cmpuint(qtest_readb(qts, 0x8a005), ==, 'A');
    qtest_writeb(qts, IR(215), 0);
    qtest_clock_step(qts, 36000);
    qtest_writel(qts, SCKCR, 0x100); /* remaining cooldown doubles to 72 us */
    qtest_writel(qts, MSTPCRB, 0xffffffff);
    g_assert_cmpint(send(fd, "B", 1, 0), ==, 1);
    qtest_clock_step(qts, 1000000);
    g_assert_cmpuint(qtest_readb(qts, IR(215)), ==, 0);
    qtest_writel(qts, MSTPCRB, 0x7fffffff);
    qtest_clock_step(qts, 71999);
    g_assert_cmphex(qtest_readb(qts, 0x8a004) & 0x40, ==, 0);
    qtest_clock_step(qts, 1);
    receive_ready(qts);
    g_assert_cmpuint(qtest_readb(qts, 0x8a005), ==, 'B');
    g_assert_cmpuint(qtest_readb(qts, IR(215)), ==, 1);
    close(fd);
    qtest_quit(qts);
}

static void wait_migration(QTestState *qts)
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
    QTestState *src = clock_init();
    QTestState *dst;
    int64_t now;
    g_autofree char *dir = g_dir_make_tmp("rx113-clock-XXXXXX", NULL);
    g_autofree char *file = g_strdup_printf("%s/state", dir);
    g_autofree char *uri = g_strdup_printf("file:%s", file);

    g_assert_nonnull(dir);
    qtest_writel(src, MSTPCRA, 0xefff7fdf); /* CMT0/1 and TMR0/1 unit */
    qtest_writel(src, MSTPCRB, 0x7fffffff);
    qtest_writew(src, 0x88006, 100);
    qtest_writew(src, 0x88002, 0x40);
    qtest_writew(src, 0x88000, 1);
    qtest_writeb(src, 0x88204, 100);
    qtest_writeb(src, 0x88200, 0x48);
    qtest_writeb(src, 0x8820a, 0x0b);
    qtest_writeb(src, 0x8a001, 8);
    qtest_writeb(src, 0x8a002, 0x20);
    qtest_writeb(src, 0x8a003, 'A');
    qtest_writel(src, MSTPCRC, 0x73fffffe);
    for (unsigned base = 0x8a100; base <= 0x8a120; base += 0x20) {
        qtest_writeb(src, base + 1, 8);
        qtest_writeb(src, base + 2, 0x20);
        qtest_writeb(src, base + 3, 'A');
    }
    now = qtest_clock_step(src, 10125);
    qtest_writel(src, SCKCR, 0x100);
    if (stopped) {
        qtest_writel(src, MSTPCRA, 0xefffffff);
        qtest_writel(src, MSTPCRB, 0xffffffff);
        qtest_writel(src, MSTPCRC, 0x7ffffffe);
    }
    qtest_qmp_assert_success(src, "{'execute':'migrate',"
                            "'arguments':{'uri':%s}}", uri);
    wait_migration(src);
    dst = qtest_init("-M rx113-r5f51138 -incoming defer");
    /* The qtest accelerator's private clock is not part of migration state. */
    qtest_clock_set(dst, now);
    qtest_qmp_assert_success(dst, "{'execute':'migrate-incoming',"
                            "'arguments':{'uri':%s}}", uri);
    wait_migration(dst);
    g_assert_cmpuint(frequency(dst, "cmt[0]"), ==, 16000000);
    g_assert_cmphex(qtest_readl(dst, MSTPCRC), ==,
                    stopped ? 0x7ffffffe : 0x73fffffe);
    g_assert_cmpuint(frequency(dst, "sci[6]"), ==, 16000000);
    g_assert_cmpuint(frequency(dst, "sci[7]"), ==, 16000000);
    if (stopped) {
        qtest_clock_step(dst, 1000000);
        g_assert_cmpuint(qtest_readb(dst, IR(28)), ==, 0);
        g_assert_cmpuint(qtest_readb(dst, IR(174)), ==, 0);
        g_assert_cmpuint(qtest_readb(dst, 0x8a004), ==, 0);
        qtest_writel(dst, MSTPCRA, 0xefff7fdf);
        for (unsigned base = 0x8a100; base <= 0x8a120; base += 0x20) {
            g_assert_cmpuint(qtest_readb(dst, base + 4), ==, 0);
        }
        qtest_writel(dst, MSTPCRB, 0x7fffffff);
        qtest_writel(dst, MSTPCRC, 0x73fffffe);
    }
    g_assert_cmpuint(qtest_readw(dst, 0x88004), ==, 40);
    g_assert_cmpuint(qtest_readb(dst, 0x88208), ==, 40);
    qtest_clock_step(dst, 29749);
    g_assert_cmpuint(qtest_readb(dst, IR(28)), ==, 0);
    g_assert_cmpuint(qtest_readb(dst, IR(174)), ==, 0);
    qtest_clock_step(dst, 1);
    g_assert_cmpuint(qtest_readb(dst, IR(28)), ==, 1);
    g_assert_cmpuint(qtest_readb(dst, IR(174)), ==, 1);
    for (unsigned base = 0x8a100; base <= 0x8a120; base += 0x20) {
        g_assert_cmpuint(qtest_readb(dst, base + 1), ==, 8);
    }
    qtest_clock_step(dst, 93999);
    for (unsigned base = 0x8a100; base <= 0x8a120; base += 0x20) {
        g_assert_cmphex(qtest_readb(dst, base + 4) & 4, ==, 0);
    }
    g_assert_cmphex(qtest_readb(dst, 0x8a004) & 4, ==, 0);
    qtest_clock_step(dst, 1);
    g_assert_cmphex(qtest_readb(dst, 0x8a004) & 4, ==, 4);
    for (unsigned base = 0x8a100; base <= 0x8a120; base += 0x20) {
        g_assert_cmphex(qtest_readb(dst, base + 4) & 4, ==, 4);
    }
    qtest_quit(dst);
    qtest_quit(src);
    unlink(file);
    rmdir(dir);
}

static void test_legacy(void)
{
    const char *machines[] = { "gdbsim-r5f562n7", "gdbsim-r5f562n8" };

    for (unsigned i = 0; i < G_N_ELEMENTS(machines); i++) {
        g_autofree char *args = g_strdup_printf("-M %s", machines[i]);
        QTestState *qts = qtest_init(args);
        QDict *rsp;

        /* Legacy CMT period and readback are deliberately unchanged. */
        qtest_writew(qts, 0x88002, 0x40);
        qtest_writew(qts, 0x88006, 100);
        qtest_writew(qts, 0x88000, 1);
        qtest_clock_step(qts, 10000);
        g_assert_cmpuint(qtest_readw(qts, 0x88004), ==, 0);
        qtest_clock_step(qts, 6663);
        g_assert_cmphex(qtest_readb(qts, IR(28)), ==, 0);
        qtest_clock_step(qts, 1);
        g_assert_cmphex(qtest_readb(qts, IR(28)), ==, 1);
        rsp = qtest_qmp(qts, "{'execute':'qom-set','arguments':"
                        "{'path':'/machine/mcu/cmt[0]',"
                        "'property':'input-freq','value':16000000}}");
        g_assert_true(qdict_haskey(rsp, "error"));
        qobject_unref(rsp);
        qtest_quit(qts);
    }
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/rx113-clock/sources", test_sources);
    qtest_add_func("/rx113-clock/cmt", test_cmt);
    qtest_add_func("/rx113-clock/tmr", test_tmr);
    qtest_add_func("/rx113-clock/tmr-cascade", test_tmr_cascade);
    qtest_add_func("/rx113-clock/sci", test_sci);
    qtest_add_func("/rx113-clock/receive", test_receive);
    qtest_add_data_func("/rx113-clock/migration-running",
                        GINT_TO_POINTER(0), test_migration);
    qtest_add_data_func("/rx113-clock/migration-stopped",
                        GINT_TO_POINTER(1), test_migration);
    qtest_add_func("/rx113-clock/legacy", test_legacy);
    return g_test_run();
}
