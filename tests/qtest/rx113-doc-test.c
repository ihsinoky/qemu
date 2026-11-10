/*
 * RX113 DOC tests (R01UH0448EJ0120 Rev.1.20, section 40).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define DOCR 0x8b080
#define DODIR (DOCR + 2)
#define DODSR (DOCR + 4)
#define MSTPCRB 0x80014
#define RUN_B (UINT32_MAX & ~(1U << 6))
#define DCSEL 0x04
#define DOPCIE 0x10
#define DOPCF 0x20
#define DOPCFCL 0x40
#define IR57 0x87039

static QTestState *doc_init(void)
{
    QTestState *qts = qtest_init("-M rx113-r5f51138");

    qtest_writew(qts, 0x803fe, 0xa503);
    qtest_writel(qts, MSTPCRB, RUN_B);
    return qts;
}

static void test_compare(void)
{
    QTestState *qts = doc_init();

    for (unsigned dcsel = 0; dcsel <= DCSEL; dcsel += DCSEL) {
        qtest_writeb(qts, DOCR, dcsel | DOPCFCL);
        qtest_writew(qts, DODSR, 0x1234);
        /* DODSR writes and DODIR reads do not start an operation. */
        g_assert_cmpuint(qtest_readw(qts, DODIR), ==, dcsel ? 0x1234 : 0);
        g_assert_cmpuint(qtest_readb(qts, DOCR), ==, dcsel);
        qtest_writew(qts, DODIR, dcsel ? 0 : 0x1234);
        g_assert_cmpuint(qtest_readb(qts, DOCR), ==, dcsel);
        qtest_writew(qts, DODIR, dcsel ? 0x1234 : 0);
        g_assert_cmpuint(qtest_readb(qts, DOCR), ==, dcsel | DOPCF);
        qtest_writew(qts, DODIR, dcsel ? 0 : 0x1234);
        g_assert_cmpuint(qtest_readb(qts, DOCR), ==, dcsel | DOPCF);
        g_assert_cmphex(qtest_readw(qts, DODSR), ==, 0x1234);
        qtest_writeb(qts, DOCR, dcsel | DOPCFCL);
        g_assert_cmpuint(qtest_readb(qts, DOCR), ==, dcsel);
        /* Writing the read-only flag cannot set it. Reserved bits read 0. */
        qtest_writeb(qts, DOCR, dcsel | DOPCF | 0x88);
        g_assert_cmpuint(qtest_readb(qts, DOCR), ==, dcsel);
        qtest_writew(qts, DODIR, 0x1234);
    }
    qtest_quit(qts);
}

static void test_arithmetic(void)
{
    QTestState *qts = doc_init();

    /* Figures 40.3/40.4, including their wrapped results. */
    for (unsigned mode = 1; mode <= 2; mode++) {
        for (unsigned dcsel = 0; dcsel <= DCSEL; dcsel += DCSEL) {
            unsigned control = mode | dcsel;
            unsigned result = mode == 1 ? 0xfff0 : 0xf;

            qtest_writeb(qts, DOCR, control | DOPCFCL);
            qtest_writew(qts, DODSR, result);
            for (unsigned input = 4; input <= 8; input += 2) {
                result = (mode == 1 ? result + input : result - input) & 0xffff;
                qtest_writew(qts, DODIR, input);
                g_assert_cmpuint(qtest_readw(qts, DODIR), ==, input);
                g_assert_cmpuint(qtest_readw(qts, DODSR), ==, result);
                g_assert_cmpuint(qtest_readb(qts, DOCR), ==,
                                 control | (input == 8 ? DOPCF : 0));
            }
            qtest_writeb(qts, DOCR, control | DOPCFCL);
            qtest_writew(qts, DODSR, mode == 1 ? 0xfffe : 1);
            qtest_writew(qts, DODIR, 1); /* Exact boundary is not an event. */
            g_assert_cmpuint(qtest_readb(qts, DOCR), ==, control);
            qtest_writew(qts, DODIR, 1);
            g_assert_cmpuint(qtest_readw(qts, DODSR), ==,
                             mode == 1 ? 0 : 0xffff);
            g_assert_cmpuint(qtest_readb(qts, DOCR), ==, control | DOPCF);
            qtest_writew(qts, DODIR, 0); /* A later non-event retains DOPCF. */
            g_assert_cmpuint(qtest_readb(qts, DOCR), ==, control | DOPCF);
        }
    }
    qtest_quit(qts);
}

static void test_irq(void)
{
    QTestState *qts = doc_init();

    qtest_irq_intercept_out_named(qts, "/machine/mcu/icu", "sysbus-irq");
    qtest_writeb(qts, 0x87339, 5); /* IPR057. */
    qtest_writeb(qts, 0x87207, 2); /* IER07.IEN1. */
    qtest_writew(qts, DODIR, 1);
    g_assert_cmphex(qtest_readb(qts, DOCR), ==, DOPCF);
    g_assert_cmpuint(qtest_readb(qts, IR57), ==, 0);
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_writeb(qts, DOCR, DOPCIE);
    g_assert_cmpuint(qtest_readb(qts, IR57), ==, 1);
    g_assert_true(qtest_get_irq(qts, 0));
    qtest_writeb(qts, IR57, 0); /* A level request cannot be cleared in IR. */
    g_assert_cmpuint(qtest_readb(qts, IR57), ==, 1);
    qtest_writeb(qts, DOCR, 0);
    g_assert_cmpuint(qtest_readb(qts, IR57), ==, 0);
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_writeb(qts, DOCR, DOPCIE | DOPCFCL);
    qtest_writew(qts, DODIR, 2);
    g_assert_true(qtest_get_irq(qts, 0));
    qtest_writeb(qts, DOCR, DOPCIE | DOPCFCL);
    g_assert_cmpuint(qtest_readb(qts, IR57), ==, 0);
    g_assert_false(qtest_get_irq(qts, 0));
    /* DOC is not a DTC source, even when firmware writes DTCER057. */
    qtest_writeb(qts, 0x87139, 1);
    g_assert_cmpuint(qtest_readb(qts, 0x87139), ==, 0);
    qtest_quit(qts);
}

static void test_access_stop_reset(void)
{
    QTestState *qts = qtest_init("-M rx113-r5f51138");

    qtest_writeb(qts, DOCR, DOPCIE | DCSEL);
    qtest_writew(qts, DODIR, 0x1234);
    qtest_writew(qts, DODSR, 0x5678);
    g_assert_cmpuint(qtest_readb(qts, DOCR), ==, 0);
    g_assert_cmpuint(qtest_readw(qts, DODIR), ==, 0);
    g_assert_cmpuint(qtest_readw(qts, DODSR), ==, 0);
    qtest_writew(qts, 0x803fe, 0xa502);
    qtest_writel(qts, MSTPCRB, RUN_B);
    g_assert_cmpuint(qtest_readb(qts, DOCR), ==, 0);
    g_assert_cmpuint(qtest_readw(qts, DODIR), ==, 0);
    g_assert_cmpuint(qtest_readw(qts, DODSR), ==, 0);
    qtest_writeb(qts, DOCR, DOPCIE);
    qtest_writew(qts, DODSR, 0x5678);
    qtest_writew(qts, DODIR, 0x1234);
    qtest_writeb(qts, DODIR, 0xff);
    qtest_writel(qts, DODIR, 0xffffffff);
    qtest_writeb(qts, DODSR + 1, 0xff);
    qtest_writew(qts, DOCR, DOPCFCL);
    g_assert_cmpuint(qtest_readb(qts, DOCR + 1), ==, 0);
    g_assert_cmpuint(qtest_readb(qts, DODIR), ==, 0);
    g_assert_cmpuint(qtest_readl(qts, DODSR), ==, 0);
    g_assert_cmphex(qtest_readb(qts, DOCR), ==, DOPCIE | DOPCF);
    qtest_writel(qts, MSTPCRB, UINT32_MAX);
    qtest_writeb(qts, DOCR, DOPCFCL);
    qtest_writew(qts, DODIR, 0xabcd);
    qtest_writew(qts, DODSR, 0xef01);
    g_assert_cmpuint(qtest_readb(qts, DOCR), ==, 0);
    g_assert_cmpuint(qtest_readw(qts, DODIR), ==, 0);
    g_assert_cmpuint(qtest_readw(qts, DODSR), ==, 0);
    qtest_writel(qts, MSTPCRB, RUN_B);
    g_assert_cmphex(qtest_readb(qts, DOCR), ==, DOPCIE | DOPCF);
    g_assert_cmphex(qtest_readw(qts, DODIR), ==, 0x1234);
    g_assert_cmphex(qtest_readw(qts, DODSR), ==, 0x5678);
    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    qtest_writew(qts, 0x803fe, 0xa502);
    qtest_writel(qts, MSTPCRB, RUN_B);
    g_assert_cmpuint(qtest_readb(qts, DOCR), ==, 0);
    g_assert_cmpuint(qtest_readw(qts, DODIR), ==, 0);
    g_assert_cmpuint(qtest_readw(qts, DODSR), ==, 0);
    g_assert_cmpuint(qtest_readb(qts, IR57), ==, 0);
    qtest_quit(qts);
}

static void test_elc(void)
{
    QTestState *qts = doc_init();

    qtest_writeb(qts, 0x80036, 0);
    qtest_clock_step(qts, 34500);
    qtest_writew(qts, 0x80026, 0x100);
    qtest_writel(qts, 0x80020, 0); /* PCLKB = 32 MHz. */
    qtest_writel(qts, 0x80010, 0xefffffff & ~(1U << 15)); /* CMT1. */
    qtest_writel(qts, MSTPCRB, RUN_B & ~(1U << 9)); /* ELC. */
    qtest_writeb(qts, 0x8b108, 0x6a); /* ELSR7 -> CMT1. */
    qtest_writeb(qts, 0x8b121, 0xfb); /* Event count. */
    qtest_writew(qts, 0x8800c, 100);
    qtest_writew(qts, 0x88000, 2);
    qtest_writeb(qts, 0x8b100, 0xff);
    for (unsigned mode = 0; mode <= 2; mode++) {
        qtest_writeb(qts, DOCR, mode | DOPCFCL); /* DOPCIE=0. */
        qtest_writew(qts, DODSR, mode == 1 ? 0xffff : 0);
        qtest_writew(qts, DODIR, 1);
        qtest_clock_step(qts, 32);
        g_assert_cmpuint(qtest_readw(qts, 0x8800a), ==, mode * 2 + 1);
        /* A second condition emits a new event even with DOPCF already 1. */
        qtest_writew(qts, DODSR, mode == 1 ? 0xffff : 0);
        qtest_writew(qts, DODIR, 1);
        qtest_clock_step(qts, 32);
        g_assert_cmpuint(qtest_readw(qts, 0x8800a), ==, mode * 2 + 2);
        g_assert_cmpuint(qtest_readb(qts, IR57), ==, 0);
    }
    qtest_writew(qts, DODSR, 10);
    qtest_writew(qts, DODIR, 1); /* No underflow. */
    qtest_clock_step(qts, 32);
    g_assert_cmpuint(qtest_readw(qts, 0x8800a), ==, 6);
    qtest_writel(qts, MSTPCRB, UINT32_MAX & ~(1U << 9)); /* Stop DOC only. */
    qtest_writew(qts, DODIR, 10); /* Would underflow if executed. */
    qtest_clock_step(qts, 32);
    g_assert_cmpuint(qtest_readw(qts, 0x8800a), ==, 6);
    qtest_writel(qts, MSTPCRB, RUN_B & ~(1U << 9));
    g_assert_cmpuint(qtest_readw(qts, DODSR), ==, 9);
    qtest_writeb(qts, 0x8b108, 0); /* Disconnect. */
    qtest_writew(qts, DODIR, 10);
    qtest_clock_step(qts, 32);
    g_assert_cmpuint(qtest_readw(qts, 0x8800a), ==, 6);
    for (unsigned n = 24; n <= 25; n++) {
        qtest_writeb(qts, 0x8b101 + n, 0x69);
        qtest_writeb(qts, 0x8b101 + n, 0x6a); /* Prohibited selection. */
        g_assert_cmphex(qtest_readb(qts, 0x8b101 + n), ==, 0x69);
    }
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
    QTestState *src = doc_init();
    QTestState *dst;
    g_autofree char *dir = g_dir_make_tmp("rx113-doc-XXXXXX", NULL);
    g_autofree char *file = g_strdup_printf("%s/state", dir);
    g_autofree char *uri = g_strdup_printf("file:%s", file);

    qtest_writeb(src, DOCR, 1 | DOPCIE);
    qtest_writew(src, DODSR, 0xffff);
    qtest_writew(src, DODIR, 2);
    if (stopped) {
        qtest_writel(src, MSTPCRB, UINT32_MAX);
    }
    qtest_qmp_assert_success(src, "{'execute':'migrate',"
                            "'arguments':{'uri':%s}}", uri);
    migration_wait(src);
    dst = qtest_init("-M rx113-r5f51138 -incoming defer");
    qtest_qmp_assert_success(dst, "{'execute':'migrate-incoming',"
                            "'arguments':{'uri':%s}}", uri);
    migration_wait(dst);
    g_assert_cmpuint(qtest_readb(dst, DOCR), ==,
                     stopped ? 0 : 1 | DOPCIE | DOPCF);
    g_assert_cmpuint(qtest_readb(dst, IR57), ==, 1);
    qtest_writel(dst, MSTPCRB, RUN_B);
    g_assert_cmphex(qtest_readb(dst, DOCR), ==, 1 | DOPCIE | DOPCF);
    g_assert_cmpuint(qtest_readw(dst, DODIR), ==, 2);
    g_assert_cmpuint(qtest_readw(dst, DODSR), ==, 1);
    qtest_writeb(dst, DOCR, 1 | DOPCIE | DOPCFCL);
    g_assert_cmpuint(qtest_readb(dst, IR57), ==, 0);
    qtest_writew(dst, DODIR, 3);
    g_assert_cmpuint(qtest_readw(dst, DODSR), ==, 4);
    qtest_quit(dst);
    qtest_quit(src);
    unlink(file);
    rmdir(dir);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/rx113-doc/compare", test_compare);
    qtest_add_func("/rx113-doc/arithmetic", test_arithmetic);
    qtest_add_func("/rx113-doc/irq", test_irq);
    qtest_add_func("/rx113-doc/access-stop-reset", test_access_stop_reset);
    qtest_add_func("/rx113-doc/elc", test_elc);
    qtest_add_data_func("/rx113-doc/migration-running", GUINT_TO_POINTER(0),
                        test_migration);
    qtest_add_data_func("/rx113-doc/migration-stopped", GUINT_TO_POINTER(1),
                        test_migration);
    return g_test_run();
}
