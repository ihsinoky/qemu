/*
 * RX ICU IPR register tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define ICU_BASE 0x87000
#define IPR_BASE (ICU_BASE + 0x300)
#define DMRSR_BASE (ICU_BASE + 0x400)

static void test_ipr_default(const void *opaque)
{
    const char *machine = opaque;
    QTestState *qts = qtest_initf("-M %s", machine);

    for (unsigned i = 0; i < 142; i++) {
        g_assert_cmphex(qtest_readb(qts, IPR_BASE + i), ==, 0);
        qtest_writeb(qts, IPR_BASE + i, 0xa5);
        g_assert_cmphex(qtest_readb(qts, IPR_BASE + i), ==, 5);
    }

    /* Preserve the legacy overlap of IPR142/143 with DMRSR0/1. */
    for (unsigned i = 0; i < 2; i++) {
        qtest_writeb(qts, DMRSR_BASE + 4 * i, 0xab);
        g_assert_cmphex(qtest_readb(qts, IPR_BASE + 142 + i), ==, 0xb);
        qtest_writeb(qts, IPR_BASE + 142 + i, 0xf6);
        g_assert_cmphex(qtest_readb(qts, DMRSR_BASE + 4 * i), ==, 6);
    }
    qtest_writeb(qts, IPR_BASE + 144, 7);
    g_assert_cmphex(qtest_readb(qts, IPR_BASE + 144), ==, 0xff);
    qtest_writeb(qts, IPR_BASE + 214, 9);
    g_assert_cmphex(qtest_readb(qts, IPR_BASE + 214), ==, 0xff);
    qtest_quit(qts);
}

static void test_ipr_extended(const void *opaque)
{
    const char *machine = opaque;
    QTestState *qts = qtest_initf("-M %s -global rx-icu.ipr-count=256",
                                   machine);

    /* Includes IPR214 (SCI0 on RX113) and both decode boundaries. */
    for (unsigned i = 0; i < 256; i++) {
        g_assert_cmphex(qtest_readb(qts, IPR_BASE + i), ==, 0);
        qtest_writeb(qts, IPR_BASE + i, i | 0xf0);
    }
    for (unsigned i = 0; i < 256; i++) {
        g_assert_cmphex(qtest_readb(qts, IPR_BASE + i), ==, i & 0xf);
    }
    qtest_writeb(qts, IPR_BASE + 214, 0xab);
    g_assert_cmphex(qtest_readb(qts, IPR_BASE + 214), ==, 0xb);
    g_assert_cmphex(qtest_readb(qts, DMRSR_BASE), ==, 0);
    qtest_writeb(qts, DMRSR_BASE, 0xa5);
    g_assert_cmphex(qtest_readb(qts, DMRSR_BASE), ==, 0xa5);
    g_assert_cmphex(qtest_readb(qts, IPR_BASE + 142), ==, 0xe);
    qtest_quit(qts);
}

static void test_ipr_small(void)
{
    QTestState *qts = qtest_init("-M gdbsim-r5f562n8 "
                               "-global rx-icu.ipr-count=1");

    qtest_writeb(qts, IPR_BASE, 0xff);
    g_assert_cmphex(qtest_readb(qts, IPR_BASE), ==, 0xf);
    qtest_writeb(qts, IPR_BASE + 1, 7);
    g_assert_cmphex(qtest_readb(qts, IPR_BASE + 1), ==, 0xff);
    qtest_writeb(qts, IPR_BASE + 142, 7);
    g_assert_cmphex(qtest_readb(qts, IPR_BASE + 142), ==, 0xff);
    g_assert_cmphex(qtest_readb(qts, DMRSR_BASE), ==, 0);
    /* Out-of-range map entries must be safe in request and ACK scanning. */
    qtest_writeb(qts, ICU_BASE + 0x200 + 215 / 8, 0x80);
    qtest_writeb(qts, ICU_BASE + 0x200 + 216 / 8, 1);
    qtest_set_irq_in(qts, "/machine/mcu/icu", NULL, 215, 1);
    qtest_set_irq_in(qts, "/machine/mcu/icu", NULL, 216, 1);
    g_assert_cmphex(qtest_readb(qts, ICU_BASE + 215), ==, 1);
    g_assert_cmphex(qtest_readb(qts, ICU_BASE + 216), ==, 1);
    qtest_set_irq_in(qts, "/machine/mcu/icu", "ack", 0, 1);
    g_assert_cmphex(qtest_readb(qts, ICU_BASE + 215), ==, 0);
    g_assert_cmphex(qtest_readb(qts, ICU_BASE + 216), ==, 1);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    const char *machines[] = { "gdbsim-r5f562n7", "gdbsim-r5f562n8" };

    g_test_init(&argc, &argv, NULL);
    for (unsigned i = 0; i < G_N_ELEMENTS(machines); i++) {
        g_autofree char *path = g_strdup_printf("/rx-icu/%s/default",
                                               machines[i]);
        g_autofree char *extended = g_strdup_printf("/rx-icu/%s/extended",
                                                   machines[i]);

        qtest_add_data_func(path, machines[i], test_ipr_default);
        qtest_add_data_func(extended, machines[i], test_ipr_extended);
    }
    qtest_add_func("/rx-icu/small", test_ipr_small);
    return g_test_run();
}
