/*
 * RX113 R12DAA tests (R01UH0448EJ0120 Rev.1.20, section 37).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define BASE 0x88040
#define DACR (BASE + 4)
#define DADPR (BASE + 5)
#define DAADSCR (BASE + 6)
#define DAVREFCR (BASE + 7)
#define MSTPCRA 0x80010
#define STOPPED 0xefffffff
#define RUNNING (STOPPED & ~(1U << 18) & ~(1U << 17))
#define DAC_PATH "/machine/mcu/dac"
#define ADC_PATH "/machine/mcu/s12ad"

static QTestState *dac_init(void)
{
    QTestState *qts = qtest_init("-M rx113-r5f51138");

    qtest_writew(qts, 0x803fe, 0xa503);
    qtest_writel(qts, MSTPCRA, RUNNING);
    qtest_clock_step(qts, 1000);
    return qts;
}

static unsigned output(QTestState *qts, unsigned ch)
{
    g_autofree char *name = g_strdup_printf("da%u-uv", ch);
    QDict *rsp = qtest_qmp(qts, "{'execute':'qom-get','arguments':{"
                          "'path':'" DAC_PATH "','property':%s}}", name);
    unsigned value;

    g_assert_true(qdict_haskey(rsp, "return"));
    value = qdict_get_int(rsp, "return");
    qobject_unref(rsp);
    return value;
}

static void voltage(QTestState *qts, const char *name, unsigned uv)
{
    qtest_qmp_assert_success(qts, "{'execute':'qom-set','arguments':{"
                            "'path':'" ADC_PATH "','property':%s,'value':%u}}",
                            name, uv);
}

static void test_registers(void)
{
    QTestState *qts = dac_init();

    g_assert_cmphex(qtest_readb(qts, DACR), ==, 0x1f);
    g_assert_cmpuint(qtest_readw(qts, BASE), ==, 0);
    g_assert_cmpuint(qtest_readw(qts, BASE + 2), ==, 0);
    for (unsigned addr = DADPR; addr <= DAVREFCR; addr++) {
        g_assert_cmpuint(qtest_readb(qts, addr), ==, 0);
    }
    qtest_writeb(qts, DACR, 0xff);
    g_assert_cmphex(qtest_readb(qts, DACR), ==, 0xdf);
    qtest_writeb(qts, DACR, 0);
    g_assert_cmphex(qtest_readb(qts, DACR), ==, 0x1f);
    qtest_writew(qts, BASE, 0xffff);
    qtest_writew(qts, BASE + 2, 0xf123);
    g_assert_cmphex(qtest_readw(qts, BASE), ==, 0xfff);
    g_assert_cmphex(qtest_readw(qts, BASE + 2), ==, 0x123);
    qtest_writeb(qts, DADPR, 0xff);
    g_assert_cmphex(qtest_readb(qts, DADPR), ==, 0x80);
    qtest_writew(qts, BASE, 0xffff);
    qtest_writew(qts, BASE + 2, 0x123f);
    g_assert_cmphex(qtest_readw(qts, BASE), ==, 0xfff0);
    g_assert_cmphex(qtest_readw(qts, BASE + 2), ==, 0x1230);
    qtest_writeb(qts, DAADSCR, 0x7f);
    g_assert_cmpuint(qtest_readb(qts, DAADSCR), ==, 0);
    /* Invalid widths and offsets cannot alter the neighboring register. */
    qtest_writeb(qts, BASE, 0);
    qtest_writel(qts, BASE, 0);
    qtest_writew(qts, BASE + 1, 0);
    qtest_writew(qts, DACR, 0);
    qtest_writeb(qts, BASE + 8, 0xff);
    g_assert_cmphex(qtest_readw(qts, BASE), ==, 0xfff0);
    g_assert_cmphex(qtest_readw(qts, BASE + 2), ==, 0x1230);
    g_assert_cmphex(qtest_readb(qts, DADPR), ==, 0x80);
    g_assert_cmpuint(qtest_readb(qts, BASE), ==, 0);
    g_assert_cmpuint(qtest_readl(qts, BASE), ==, 0);
    g_assert_cmpuint(qtest_readb(qts, BASE + 8), ==, 0);
    qtest_quit(qts);
}

static void test_output(void)
{
    QTestState *qts = dac_init();
    static const unsigned codes[] = { 0, 1, 1024, 2048, 4095 };
    static const unsigned refs[] = { 0, 1, 3, 6 };
    static const unsigned spans[] = { 0, 3300000, 1430000, 2000000 };
    static const unsigned lows[] = { 0, 0, 0, 500000 };

    qtest_writew(qts, BASE, 2048);
    qtest_writeb(qts, DACR, 0x5f);
    qtest_writeb(qts, DAVREFCR, 3);
    g_assert_cmpuint(output(qts, 0), ==, 0); /* Unspecified fixture default. */
    qtest_writeb(qts, DAVREFCR, 0);
    voltage(qts, "internal-reference-uv", 1430000);
    voltage(qts, "vrefh-uv", 2500000);
    voltage(qts, "vrefl-uv", 500000);
    for (unsigned format = 0; format < 2; format++) {
        qtest_writeb(qts, DADPR, format << 7);
        for (unsigned r = 0; r < G_N_ELEMENTS(refs); r++) {
            qtest_writeb(qts, DAVREFCR, 0);
            qtest_writeb(qts, DAVREFCR, refs[r] | 0xf8);
            g_assert_cmpuint(qtest_readb(qts, DAVREFCR), ==, refs[r]);
            for (unsigned c = 0; c < G_N_ELEMENTS(codes); c++) {
                qtest_writew(qts, BASE, codes[c] << (format ? 4 : 0));
                qtest_writew(qts, BASE + 2,
                             (4095 - codes[c]) << (format ? 4 : 0));
                for (unsigned enable = 0; enable < 4; enable++) {
                    qtest_writeb(qts, DACR, enable << 6);
                    g_assert_cmpuint(output(qts, 0), ==, enable & 1 ?
                                     lows[r] + (uint64_t)spans[r] *
                                     codes[c] / 4096 : 0);
                    g_assert_cmpuint(output(qts, 1), ==, enable & 2 ?
                                     lows[r] + (uint64_t)spans[r] *
                                     (4095 - codes[c]) / 4096 : 0);
                }
            }
        }
    }
    /* Invalid REF encodings retain the prior selection, as in ELC. */
    for (unsigned r = 0; r < 8; r++) {
        if (r != 0 && r != 1 && r != 3 && r != 6) {
            qtest_writeb(qts, DAVREFCR, r);
            g_assert_cmpuint(qtest_readb(qts, DAVREFCR), ==, 6);
        }
    }
    /* Supplies are shared with the ADC, including runtime fixture changes. */
    qtest_writeb(qts, DADPR, 0);
    qtest_writew(qts, BASE, 2048);
    qtest_writeb(qts, DAVREFCR, 0);
    qtest_writeb(qts, DAVREFCR, 1);
    voltage(qts, "avcc-uv", 3000000);
    voltage(qts, "avss-uv", 1000000);
    g_assert_cmpuint(output(qts, 0), ==, 2000000);
    voltage(qts, "avcc-uv", 1000000);
    g_assert_cmpuint(output(qts, 0), ==, 0);
    voltage(qts, "avcc-uv", 500000);
    g_assert_cmpuint(output(qts, 0), ==, 0);
    qtest_quit(qts);
}

static void test_sync(void)
{
    QTestState *qts = dac_init();

    qtest_writeb(qts, DAVREFCR, 1);
    qtest_writeb(qts, DACR, 0xdf);
    qtest_writew(qts, BASE, 1024);
    qtest_writew(qts, BASE + 2, 2048);
    qtest_writeb(qts, DAADSCR, 0xff);
    g_assert_cmphex(qtest_readb(qts, DAADSCR), ==, 0x80);
    /* Idle ADC: one PCLKB cycle at reset's 500 kHz. */
    qtest_writew(qts, BASE, 2048);
    qtest_clock_step(qts, 1999);
    g_assert_cmpuint(output(qts, 0), ==, 825000);
    qtest_clock_step(qts, 1);
    g_assert_cmpuint(output(qts, 0), ==, 1650000);
    qtest_writew(qts, 0x89004, 1);
    qtest_writew(qts, 0x89000, 0x8400); /* ADIE=0, still emits scan-end. */
    qtest_clock_step(qts, 10000);
    qtest_writew(qts, BASE, 1);
    qtest_writew(qts, BASE, 3072); /* Latest value wins. */
    qtest_writew(qts, BASE + 2, 1024);
    g_assert_cmpuint(qtest_readw(qts, BASE), ==, 3072);
    qtest_clock_step(qts, 93999); /* Scan ends after 52 cycles = 104 us. */
    g_assert_cmpuint(output(qts, 0), ==, 1650000);
    g_assert_cmpuint(output(qts, 1), ==, 1650000);
    qtest_clock_step(qts, 1);
    g_assert_cmpuint(output(qts, 0), ==, 2475000);
    g_assert_cmpuint(output(qts, 1), ==, 825000);
    g_assert_cmpuint(qtest_readb(qts, 0x87066), ==, 0);
    /* Turning synchronization off exposes the current register immediately. */
    qtest_writew(qts, 0x89000, 0x8400);
    qtest_writew(qts, BASE, 2048);
    qtest_writeb(qts, DAADSCR, 0);
    g_assert_cmpuint(output(qts, 0), ==, 1650000);
    qtest_quit(qts);
}

static void test_elc(void)
{
    QTestState *qts = dac_init();

    qtest_writel(qts, 0x80014, UINT32_MAX & ~(1U << 9) & ~(1U << 6));
    qtest_writeb(qts, 0x8b112, 0x6a); /* ELSR17 = DOC event. */
    qtest_writeb(qts, 0x8b100, 0x80);
    qtest_writeb(qts, DAVREFCR, 1);
    qtest_writew(qts, BASE, 2048);
    qtest_writew(qts, BASE + 2, 1024);
    qtest_writew(qts, 0x8b082, 1); /* DOC mismatch, DOPCIE=0. */
    qtest_clock_step(qts, 2000);
    g_assert_cmphex(qtest_readb(qts, DACR), ==, 0x5f);
    g_assert_cmpuint(output(qts, 0), ==, 1650000);
    g_assert_cmpuint(output(qts, 1), ==, 0);
    /* Disabling the event link leaves DAOE0 set. */
    qtest_writeb(qts, 0x8b112, 0);
    g_assert_cmphex(qtest_readb(qts, DACR), ==, 0x5f);
    qtest_writeb(qts, DACR, 0x1f);
    qtest_writew(qts, 0x8b082, 1);
    qtest_clock_step(qts, 2000);
    g_assert_cmphex(qtest_readb(qts, DACR), ==, 0x1f);
    qtest_writeb(qts, 0x8b112, 0x6a);
    /* Prohibited ELC + DAADST combination is diagnosed and ignored. */
    qtest_writeb(qts, DAADSCR, 0x80);
    qtest_writew(qts, 0x8b082, 1);
    qtest_clock_step(qts, 2000);
    g_assert_cmphex(qtest_readb(qts, DACR), ==, 0x1f);
    qtest_writeb(qts, DAADSCR, 0);
    qtest_writew(qts, 0x8b082, 1);
    qtest_clock_step(qts, 2000);
    g_assert_cmphex(qtest_readb(qts, DACR), ==, 0x5f);
    qtest_quit(qts);
}

static void test_stop_reset(void)
{
    QTestState *qts = dac_init();

    qtest_writeb(qts, DAVREFCR, 1);
    qtest_writeb(qts, DACR, 0xdf);
    qtest_writew(qts, BASE, 1024);
    qtest_writew(qts, BASE + 2, 2048);
    qtest_writel(qts, MSTPCRA, STOPPED);
    for (unsigned addr = DACR; addr <= DAVREFCR; addr++) {
        g_assert_cmpuint(qtest_readb(qts, addr), ==, 0);
        qtest_writeb(qts, addr, 0);
    }
    for (unsigned ch = 0; ch < 2; ch++) {
        g_assert_cmpuint(qtest_readw(qts, BASE + 2 * ch), ==, 0);
        qtest_writew(qts, BASE + 2 * ch, 4095);
    }
    qtest_set_irq_in(qts, DAC_PATH, "elc", 0, 1);
    qtest_set_irq_in(qts, DAC_PATH, "sync", 0, 1);
    qtest_clock_step(qts, 1000000);
    g_assert_cmpuint(output(qts, 0), ==, 825000);
    g_assert_cmpuint(output(qts, 1), ==, 1650000);
    qtest_writel(qts, MSTPCRA, RUNNING);
    g_assert_cmphex(qtest_readb(qts, DACR), ==, 0xdf);
    g_assert_cmpuint(qtest_readb(qts, DAVREFCR), ==, 1);
    g_assert_cmpuint(qtest_readw(qts, BASE), ==, 1024);
    g_assert_cmpuint(qtest_readw(qts, BASE + 2), ==, 2048);
    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    g_assert_cmpuint(output(qts, 0), ==, 0);
    g_assert_cmpuint(output(qts, 1), ==, 0);
    qtest_writew(qts, 0x803fe, 0xa503);
    qtest_writel(qts, MSTPCRA, RUNNING);
    g_assert_cmphex(qtest_readb(qts, DACR), ==, 0x1f);
    g_assert_cmpuint(qtest_readw(qts, BASE), ==, 0);
    g_assert_cmpuint(qtest_readw(qts, BASE + 2), ==, 0);
    g_assert_cmpuint(qtest_readb(qts, DAVREFCR), ==, 0);
    qtest_quit(qts);
}

static void test_sync_clock_stop(void)
{
    QTestState *qts = dac_init();

    qtest_writeb(qts, DAVREFCR, 1);
    qtest_writeb(qts, DACR, 0x5f);
    qtest_writeb(qts, DAADSCR, 0x80);
    qtest_writew(qts, BASE, 2048);
    qtest_clock_step(qts, 501);
    qtest_writel(qts, MSTPCRA, RUNNING | (1U << 18));
    qtest_clock_step(qts, 1000000);
    g_assert_cmpuint(output(qts, 0), ==, 0);
    qtest_writel(qts, 0x80020, 0x200); /* PCLKB twice reset's rate. */
    qtest_writel(qts, MSTPCRA, RUNNING);
    qtest_clock_step(qts, 749);
    g_assert_cmpuint(output(qts, 0), ==, 0);
    qtest_clock_step(qts, 1);
    g_assert_cmpuint(output(qts, 0), ==, 1650000);
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
    bool stopped = GPOINTER_TO_UINT(opaque);
    QTestState *src = dac_init();
    QTestState *dst;
    int64_t now;
    g_autofree char *dir = g_dir_make_tmp("rx113-dac-XXXXXX", NULL);
    g_autofree char *file = g_strdup_printf("%s/state", dir);
    g_autofree char *uri = g_strdup_printf("file:%s", file);

    g_assert_nonnull(dir);
    voltage(src, "avcc-uv", 3000000);
    qtest_writeb(src, DAVREFCR, 1);
    qtest_writeb(src, DACR, 0xdf);
    qtest_writew(src, BASE, 1024);
    qtest_writew(src, BASE + 2, 2048);
    qtest_writeb(src, DAADSCR, 0x80);
    qtest_writew(src, BASE, 3072);
    now = qtest_clock_step(src, 501);
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
    g_assert_cmpuint(output(dst, 0), ==, 750000);
    g_assert_cmpuint(output(dst, 1), ==, 1500000);
    if (stopped) {
        qtest_clock_step(dst, 1000000);
        g_assert_cmpuint(output(dst, 0), ==, 750000);
        qtest_writel(dst, MSTPCRA, RUNNING);
    }
    g_assert_cmpuint(qtest_readw(dst, BASE), ==, 3072);
    qtest_clock_step(dst, 1498);
    g_assert_cmpuint(output(dst, 0), ==, 750000);
    qtest_clock_step(dst, 1);
    g_assert_cmpuint(output(dst, 0), ==, 2250000);
    qtest_quit(dst);
    qtest_quit(src);
    unlink(file);
    rmdir(dir);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/rx113-dac/registers", test_registers);
    qtest_add_func("/rx113-dac/output", test_output);
    qtest_add_func("/rx113-dac/sync", test_sync);
    qtest_add_func("/rx113-dac/elc", test_elc);
    qtest_add_func("/rx113-dac/stop-reset", test_stop_reset);
    qtest_add_func("/rx113-dac/sync-clock-stop", test_sync_clock_stop);
    qtest_add_data_func("/rx113-dac/migration-running", GUINT_TO_POINTER(0),
                        test_migration);
    qtest_add_data_func("/rx113-dac/migration-stopped", GUINT_TO_POINTER(1),
                        test_migration);
    return g_test_run();
}
