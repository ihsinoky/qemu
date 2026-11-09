/*
 * RX113 independent watchdog: manual R01UH0448EJ0120 sections 6/7/9/26.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bswap.h"
#include "qobject/qdict.h"

#define RR 0x88030
#define CR 0x88032
#define SR 0x88034
#define RCR 0x88036
#define CSTPR 0x88038
#define ILOCOCR 0x80035
#define PRCR 0x803fe
#define RSTSR2 0x800c0
#define UNDFF 0x4000
#define REFEF 0x8000

static char *flash_dir;
static char *flash_path;

/* A real flash image exercises reset-time option reads, not model overrides. */
static QTestState *iwdt_init_full(uint32_t ofs, bool intercept)
{
    g_autofree uint8_t *flash = g_malloc0(128 * 1024);
    g_autofree char *args = NULL;
    QTestState *qts;

    stl_le_p(flash + 128 * 1024 - 0x74, ofs);
    g_assert_true(g_file_set_contents(flash_path, (char *)flash,
                                     128 * 1024, NULL));
    args = g_strdup_printf("-M rx113-r5f51135 -bios %s", flash_path);
    qts = qtest_init(args);
    if (intercept) {
        qtest_irq_intercept_out_named(qts, "/machine/mcu/iwdt", "sysbus-irq");
    }
    return qts;
}

static QTestState *iwdt_init(uint32_t ofs)
{
    return iwdt_init_full(ofs, true);
}

static int64_t cycles(unsigned n, unsigned div)
{
    return ((int64_t)n * div * 1000000000 + 14999) / 15000;
}

static void refresh(QTestState *qts)
{
    qtest_writeb(qts, RR, 0);
    qtest_writeb(qts, RR, 0xff);
}

static void start_clock(QTestState *qts)
{
    qtest_writew(qts, PRCR, 0xa501);
    qtest_writeb(qts, ILOCOCR, 0);
    qtest_clock_step(qts, 50000);
}

static void test_registers(void)
{
    QTestState *qts = iwdt_init(0xffffffff);
    int64_t now;

    g_assert_cmphex(qtest_readb(qts, RR), ==, 0xff);
    g_assert_cmphex(qtest_readw(qts, CR), ==, 0x33f3);
    g_assert_cmphex(qtest_readw(qts, SR), ==, 0);
    g_assert_cmphex(qtest_readb(qts, RCR), ==, 0x80);
    g_assert_cmphex(qtest_readb(qts, CSTPR), ==, 0x80);
    g_assert_cmphex(qtest_readb(qts, ILOCOCR), ==, 1);
    /* ILOCOCR is PRC0-protected and cannot stop a running oscillator. */
    qtest_writeb(qts, ILOCOCR, 0);
    g_assert_cmphex(qtest_readb(qts, ILOCOCR), ==, 1);
    start_clock(qts);
    qtest_writeb(qts, ILOCOCR, 1);
    g_assert_cmphex(qtest_readb(qts, ILOCOCR), ==, 0);
    /* Required access widths; reserved bits; each control is write-once. */
    qtest_writeb(qts, CR, 0);
    g_assert_cmphex(qtest_readw(qts, CR), ==, 0x33f3);
    qtest_writew(qts, CR, 0xff0c);
    g_assert_cmphex(qtest_readw(qts, CR), ==, 0x3300);
    qtest_writew(qts, CR, 0);
    g_assert_cmphex(qtest_readw(qts, CR), ==, 0x3300);
    qtest_writeb(qts, RCR, 0x7f);
    qtest_writeb(qts, RCR, 0x80);
    g_assert_cmphex(qtest_readb(qts, RCR), ==, 0);
    qtest_writeb(qts, CSTPR, 0x7f);
    qtest_writeb(qts, CSTPR, 0x80);
    g_assert_cmphex(qtest_readb(qts, CSTPR), ==, 0);
    /* Malformed refresh sequences do not start the counter or set REFEF. */
    qtest_writeb(qts, RR, 0xff);
    qtest_writeb(qts, RR, 0);
    qtest_writeb(qts, RR, 0x54);
    qtest_writeb(qts, RR, 0xff);
    qtest_clock_step(qts, 100000000);
    g_assert_cmphex(qtest_readw(qts, SR), ==, 0);
    qtest_writeb(qts, RR, 0);
    qtest_writeb(qts, RR, 0);
    g_assert_cmphex(qtest_readb(qts, RR), ==, 0);
    qtest_readw(qts, CR);
    now = qtest_clock_step(qts, 1);
    qtest_writeb(qts, RR, 0xff);
    qtest_clock_set(qts, now + cycles(4, 1) - 1);
    g_assert_cmphex(qtest_readw(qts, SR), ==, 0);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readw(qts, SR), ==, 127);
    qtest_clock_set(qts, now + cycles(132, 1) - 1);
    g_assert_cmphex(qtest_readw(qts, SR), ==, 0);
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readw(qts, SR), ==, UNDFF);
    g_assert_true(qtest_get_irq(qts, 0));
    /* Register mode stops on error; flags are W0C and CNTVAL read-only. */
    qtest_clock_step(qts, 100000000);
    g_assert_cmphex(qtest_readw(qts, SR), ==, UNDFF);
    qtest_writew(qts, SR, 0xffff);
    g_assert_cmphex(qtest_readw(qts, SR), ==, UNDFF);
    qtest_writew(qts, SR, 0x3fff);
    g_assert_cmphex(qtest_readw(qts, SR), ==, 0);
    refresh(qts);
    qtest_clock_step(qts, cycles(4, 1));
    g_assert_cmphex(qtest_readw(qts, SR), ==, 127);
    qtest_quit(qts);
}

static void test_refresh(void)
{
    QTestState *qts = iwdt_init(0xffffffff);
    int64_t origin;

    qtest_writew(qts, CR, 0x3300); /* 128 cycles, /1, full window */
    qtest_writeb(qts, RCR, 0);
    start_clock(qts);
    origin = qtest_clock_step(qts, 1);
    refresh(qts);
    qtest_writeb(qts, CSTPR, 0); /* first refresh locks all controls */
    g_assert_cmphex(qtest_readb(qts, CSTPR), ==, 0x80);
    /* Refresh for many periods: no underflow, then stop refreshing. */
    for (unsigned n = 1; n <= 20; n++) {
        qtest_clock_set(qts, origin + cycles(n * 64, 1));
        g_assert_cmphex(qtest_readw(qts, SR) & 0xc000, ==, 0);
        refresh(qts);
    }
    qtest_clock_set(qts, origin + cycles(20 * 64 + 131, 1));
    g_assert_cmphex(qtest_readw(qts, SR) & 0xc000, ==, 0);
    qtest_clock_set(qts, origin + cycles(20 * 64 + 132, 1));
    g_assert_cmphex(qtest_readw(qts, SR), ==, UNDFF);
    qtest_quit(qts);
}

static void test_options(const void *opaque)
{
    unsigned tops = GPOINTER_TO_UINT(opaque) & 3;
    unsigned cks = GPOINTER_TO_UINT(opaque) >> 2;
    static const unsigned periods[] = { 128, 512, 1024, 2048 };
    static const unsigned divs[16] = {
        [0] = 1, [2] = 16, [3] = 32, [4] = 64, [5] = 256, [15] = 128,
    };
    uint32_t ofs = 0xffffef01 | (tops << 2) | (cks << 4);
    QTestState *qts = iwdt_init(ofs); /* automatic, NMI, full window */
    int64_t end = cycles(periods[tops], divs[cks]);

    qtest_writew(qts, CR, 0);
    qtest_writeb(qts, RCR, 0x80);
    qtest_writeb(qts, CSTPR, 0);
    g_assert_cmphex(qtest_readw(qts, CR), ==, 0x33f3);
    g_assert_cmphex(qtest_readb(qts, RCR), ==, 0x80);
    g_assert_cmphex(qtest_readb(qts, CSTPR), ==, 0x80);
    qtest_clock_set(qts, end - 1);
    g_assert_cmphex(qtest_readw(qts, SR), ==, 0);
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readw(qts, SR), ==, UNDFF);
    g_assert_true(qtest_get_irq(qts, 0));
    qtest_clock_set(qts, cycles(periods[tops] + 1, divs[cks]));
    g_assert_cmphex(qtest_readw(qts, SR), ==, UNDFF | (periods[tops] - 1));
    qtest_writew(qts, SR, 0);
    qtest_clock_set(qts, cycles(2 * periods[tops] + 1, divs[cks]));
    g_assert_cmphex(qtest_readw(qts, SR), ==, UNDFF);
    qtest_quit(qts);
}

static void test_window(const void *opaque)
{
    unsigned setting = GPOINTER_TO_UINT(opaque);
    unsigned start = setting >> 2;
    unsigned end = setting & 3;
    unsigned start_count = 128 * (start + 1) / 4 - 1;
    unsigned end_count = 128 * (3 - end) / 4;
    QTestState *qts = iwdt_init(0xffffffff);
    int64_t origin, reload;

    qtest_writew(qts, CR, (start << 12) | (end << 8));
    qtest_writeb(qts, RCR, 0);
    start_clock(qts);
    origin = qtest_clock_step(qts, 1);
    refresh(qts);
    if (end_count && end_count - 1 >= start_count) {
        end_count = 0;
    }
    /* Complete refresh exactly at the inclusive start boundary. */
    qtest_clock_set(qts, origin + cycles(4 + 127 - start_count, 1));
    refresh(qts);
    reload = qtest_clock_step(qts, cycles(4, 1));
    g_assert_cmphex(qtest_readw(qts, SR), ==, 127);
    /* Refresh in the late prohibited region (when the end is specified). */
    if (end_count) {
        qtest_clock_set(qts, reload + cycles(128 - end_count, 1));
        refresh(qts);
        qtest_clock_step(qts, cycles(4, 1));
        g_assert_cmphex(qtest_readw(qts, SR), ==, REFEF);
    } else if (start != 3) {
        refresh(qts); /* too early */
        qtest_clock_step(qts, cycles(4, 1));
        g_assert_cmphex(qtest_readw(qts, SR), ==, REFEF);
    }
    qtest_quit(qts);
}

static void test_reset(const void *opaque)
{
    bool automatic = GPOINTER_TO_INT(opaque);
    QTestState *qts = iwdt_init(automatic ? 0xffffff01 : 0xffffffff);
    int64_t origin;
    QDict *event;

    if (!automatic) {
        qtest_writew(qts, CR, 0x3300);
        start_clock(qts);
        refresh(qts);
    }
    qtest_writeb(qts, 0x100, 0xa5);
    qtest_writew(qts, PRCR, 0xa502);
    qtest_writel(qts, 0x80014, 0x7fffffff);
    qtest_writeb(qts, 0x8a001, 42);
    origin = automatic ? 0 : 50000;
    qtest_clock_set(qts, origin + cycles(automatic ? 128 : 132, 1) - 1);
    g_assert_cmphex(qtest_readw(qts, SR), ==, 0);
    qtest_clock_step(qts, 1);
    event = qtest_qmp_eventwait_ref(qts, "RESET");
    qobject_unref(event);
    /* The reset request resets other MCU peripherals, not just the IWDT. */
    g_assert_cmphex(qtest_readl(qts, 0x80014), ==, 0xffffffff);
    qtest_writew(qts, PRCR, 0xa502);
    qtest_writel(qts, 0x80014, 0x7fffffff);
    g_assert_cmphex(qtest_readb(qts, 0x8a001), ==, 0xff);
    g_assert_cmphex(qtest_readb(qts, RSTSR2), ==, 1);
    g_assert_cmphex(qtest_readw(qts, SR) & 0xc000, ==, UNDFF);
    g_assert_cmphex(qtest_readw(qts, CR), ==, automatic ? 0x33f3 : 0x3300);
    qtest_writew(qts, CR, 0);
    g_assert_cmphex(qtest_readw(qts, CR), ==, automatic ? 0x33f3 : 0x3300);
    qtest_writeb(qts, RSTSR2, 0);
    g_assert_cmphex(qtest_readb(qts, RSTSR2), ==, 0);
    g_assert_cmphex(qtest_readb(qts, 0x100), ==, 0xa5); /* reset keeps SRAM */
    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    g_assert_cmphex(qtest_readw(qts, CR), ==, 0x33f3);
    g_assert_cmphex(qtest_readw(qts, SR) & 0xc000, ==, 0);
    qtest_quit(qts);
}

static void test_clock(void)
{
    QTestState *qts = iwdt_init(0xffffffff);

    qtest_writew(qts, CR, 0x3300);
    qtest_writeb(qts, RCR, 0);
    refresh(qts);
    qtest_clock_step(qts, 100000000);
    g_assert_cmphex(qtest_readw(qts, SR), ==, 0); /* no dedicated clock */
    qtest_writew(qts, PRCR, 0xa503);
    qtest_writeb(qts, ILOCOCR, 0);
    qtest_clock_step(qts, 50000 + cycles(4, 1) - 1);
    g_assert_cmphex(qtest_readw(qts, SR), ==, 0);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readw(qts, SR), ==, 127);
    /* PCLKB source/divider and MSTP cannot gate/change the IWDT clock. */
    qtest_writel(qts, 0x80010, 0xffffffff);
    qtest_writel(qts, 0x80020, 0x66000606);
    qtest_writew(qts, 0x80026, 0);
    qtest_clock_step(qts, cycles(128, 1));
    g_assert_cmphex(qtest_readw(qts, SR), ==, UNDFF);
    qtest_quit(qts);
}

static void test_refresh_deadline(void)
{
    QTestState *qts = iwdt_init(0xffffffff);
    int64_t origin;

    qtest_writew(qts, CR, 0x3300);
    qtest_writeb(qts, RCR, 0);
    start_clock(qts);
    origin = qtest_clock_step(qts, 1);
    refresh(qts);
    qtest_clock_set(qts, origin + cycles(128, 1));
    g_assert_cmphex(qtest_readw(qts, SR), ==, 3);
    refresh(qts); /* four count edges: count 3 is guaranteed safe */
    qtest_clock_set(qts, origin + cycles(132, 1));
    g_assert_cmphex(qtest_readw(qts, SR), ==, 127);
    qtest_clock_set(qts, origin + cycles(257, 1));
    g_assert_cmphex(qtest_readw(qts, SR), ==, 2);
    refresh(qts); /* underflow beats synchronization at count 2 */
    qtest_clock_set(qts, origin + cycles(260, 1));
    g_assert_cmphex(qtest_readw(qts, SR), ==, UNDFF);
    qtest_quit(qts);

    /* FF before the start is an error even if completion is in the window. */
    qts = iwdt_init(0xffffeb01); /* auto, 75% start, no end, WUNI */
    qtest_clock_set(qts, cycles(31, 1));
    g_assert_cmphex(qtest_readw(qts, SR), ==, 96);
    refresh(qts);
    qtest_clock_set(qts, cycles(35, 1));
    g_assert_cmphex(qtest_readw(qts, SR), ==, REFEF);
    qtest_quit(qts);
}

static void test_unconnected(void)
{
    QTestState *qts = iwdt_init_full(0xffffaf01, false);
    int64_t year = 365LL * 24 * 60 * 60 * 1000000000;
    uint64_t ticks = 365ULL * 24 * 60 * 60 * 15000;
    unsigned phase = (ticks - 128) % 129;
    unsigned count = phase ? 128 - phase : 0;

    qtest_clock_set(qts, cycles(129, 1));
    g_assert_cmphex(qtest_readw(qts, SR), ==, UNDFF | 127);
    qtest_writew(qts, SR, 0);
    qtest_clock_set(qts, cycles(229, 1));
    g_assert_cmphex(qtest_readw(qts, SR), ==, 27);
    qtest_clock_set(qts, cycles(257, 1));
    g_assert_cmphex(qtest_readw(qts, SR), ==, UNDFF);
    /* Long calendar steps must not iterate unobservable NMI requests. */
    qtest_clock_set(qts, year);
    g_assert_cmphex(qtest_readw(qts, SR), ==, UNDFF | count);
    qtest_writew(qts, SR, 0);
    refresh(qts);
    qtest_clock_set(qts, year + cycles(4, 1));
    g_assert_cmphex(qtest_readw(qts, SR), ==, 127);
    qtest_quit(qts);
}

static void wait_migration(QTestState *qts)
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

static void test_migration(void)
{
    QTestState *src = iwdt_init(0xffffffff);
    QTestState *dst;
    g_autofree char *file = g_strdup_printf("%s/state", flash_dir);
    g_autofree char *uri = g_strdup_printf("file:%s", file);
    g_autofree char *args = g_strdup_printf(
        "-M rx113-r5f51135 -bios %s -incoming defer", flash_path);
    int64_t origin, now;

    qtest_writew(src, CR, 0x3320); /* /16, 128 cycles */
    qtest_writeb(src, RCR, 0);
    start_clock(src);
    origin = qtest_clock_step(src, 1);
    refresh(src);
    /* Migrate during the four-cycle refresh synchronization, mid-tick. */
    now = qtest_clock_set(src, origin + cycles(2, 16) + 123);
    g_assert_cmphex(qtest_readw(src, SR), ==, 0);
    qtest_qmp_assert_success(src, "{'execute':'migrate',"
                            "'arguments':{'uri':%s}}", uri);
    wait_migration(src);
    dst = qtest_init(args);
    qtest_irq_intercept_out_named(dst, "/machine/mcu/iwdt", "sysbus-irq");
    qtest_clock_set(dst, now);
    qtest_qmp_assert_success(dst, "{'execute':'migrate-incoming',"
                            "'arguments':{'uri':%s}}", uri);
    wait_migration(dst);
    qtest_clock_set(dst, origin + cycles(4, 16) - 1);
    g_assert_cmphex(qtest_readw(dst, SR), ==, 0);
    qtest_clock_step(dst, 1);
    g_assert_cmphex(qtest_readw(dst, SR), ==, 127);
    qtest_writew(dst, CR, 0); /* migration retains write protection */
    g_assert_cmphex(qtest_readw(dst, CR), ==, 0x3320);
    qtest_clock_set(dst, origin + cycles(132, 16) - 1);
    g_assert_false(qtest_get_irq(dst, 0));
    qtest_clock_step(dst, 1);
    g_assert_cmphex(qtest_readw(dst, SR), ==, UNDFF);
    g_assert_true(qtest_get_irq(dst, 0));
    qtest_quit(dst);
    qtest_quit(src);
    unlink(file);
}

static void test_auto_refresh_error(void)
{
    /* 128 cycles, /1, 25% start, end >= start disables the end. */
    QTestState *qts = iwdt_init(0xffffe001);

    refresh(qts); /* too early */
    qtest_clock_set(qts, cycles(4, 1) - 1);
    g_assert_cmphex(qtest_readw(qts, SR) & 0xc000, ==, 0);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readw(qts, SR), ==, REFEF);
    g_assert_true(qtest_get_irq(qts, 0));
    qtest_clock_set(qts, cycles(5, 1));
    g_assert_cmphex(qtest_readw(qts, SR), ==, REFEF | 127);
    g_assert_false(qtest_get_irq(qts, 0));
    /* The next source replaces the uncleared previous flag. */
    qtest_clock_set(qts, cycles(133, 1));
    g_assert_cmphex(qtest_readw(qts, SR), ==, UNDFF);
    g_assert_true(qtest_get_irq(qts, 0));
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    static const unsigned cks[] = { 0, 2, 3, 4, 5, 15 };
    int result;

    g_test_init(&argc, &argv, NULL);
    flash_dir = g_dir_make_tmp("rx113-iwdt-XXXXXX", NULL);
    g_assert_nonnull(flash_dir);
    flash_path = g_strdup_printf("%s/flash.bin", flash_dir);
    qtest_add_func("/rx113-iwdt/registers", test_registers);
    qtest_add_func("/rx113-iwdt/refresh", test_refresh);
    qtest_add_func("/rx113-iwdt/refresh-deadline", test_refresh_deadline);
    qtest_add_func("/rx113-iwdt/clock", test_clock);
    qtest_add_func("/rx113-iwdt/unconnected", test_unconnected);
    qtest_add_func("/rx113-iwdt/migration", test_migration);
    qtest_add_func("/rx113-iwdt/auto-refresh-error", test_auto_refresh_error);
    qtest_add_data_func("/rx113-iwdt/reset-register", GINT_TO_POINTER(0),
                        test_reset);
    qtest_add_data_func("/rx113-iwdt/reset-auto", GINT_TO_POINTER(1),
                        test_reset);
    for (unsigned i = 0; i < G_N_ELEMENTS(cks); i++) {
        for (unsigned tops = 0; tops < 4; tops++) {
            g_autofree char *name = g_strdup_printf(
                "/rx113-iwdt/options/%u/%u", cks[i], tops);
            qtest_add_data_func(name, GUINT_TO_POINTER((cks[i] << 2) | tops),
                                test_options);
        }
    }
    for (unsigned setting = 0; setting < 16; setting++) {
        g_autofree char *name = g_strdup_printf("/rx113-iwdt/window/%u",
                                               setting);
        qtest_add_data_func(name, GUINT_TO_POINTER(setting), test_window);
    }
    result = g_test_run();
    unlink(flash_path);
    rmdir(flash_dir);
    g_free(flash_path);
    g_free(flash_dir);
    return result;
}
