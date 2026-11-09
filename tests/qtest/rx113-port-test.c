/*
 * RX113 I/O ports and MPC tests (R01UH0448EJ0120 sections 18/19)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define BASE 0x8c000
#define PDR(p) (BASE + (p))
#define PODR(p) (BASE + 0x20 + (p))
#define PIDR(p) (BASE + 0x40 + (p))
#define PMR(p) (BASE + 0x60 + (p))
#define ODR(p) (BASE + 0x80 + (p) * 2)
#define PCR(p) (BASE + 0xc0 + (p))
#define PWPR (BASE + 0x11f)
#define PSRA (BASE + 0x121)
#define PFS(p, b) (BASE + 0x140 + (p) * 8 + (b))
#define PORT_PATH "/machine/mcu/port"

static unsigned port_property(QTestState *qts, const char *prop, unsigned port)
{
    QDict *rsp = qtest_qmp(qts,
        "{'execute': 'qom-get', 'arguments': "
        "{'path': '" PORT_PATH "', 'property': %s}}", prop);
    const char *str = qdict_get_str(rsp, "return");
    unsigned val;

    g_assert_cmpuint(strlen(str), ==, 38);
    g_assert_cmpint(sscanf(str + port * 2, "%2x", &val), ==, 1);
    qobject_unref(rsp);
    return val;
}

static void input(QTestState *qts, unsigned port, unsigned bit, int level)
{
    qtest_set_irq_in(qts, PORT_PATH, "pin", port * 8 + bit, level);
}

static void unlock(QTestState *qts)
{
    qtest_writeb(qts, PWPR, 0);
    qtest_writeb(qts, PWPR, 0x40);
}

static void test_registers(void)
{
    QTestState *qts = qtest_init("-M rx113-r5f51138");
    static const uint8_t pins[] = {
        0x94, 0xff, 0xff, 0x27, 0x5f, 0x7f, 0, 0, 0, 7,
        0xff, 0xff, 0xff, 0x1f, 0xff, 0xc0, 0, 0, 0xcd,
    };

    g_assert_cmphex(qtest_readb(qts, PWPR), ==, 0x80);
    for (unsigned p = 0; p < G_N_ELEMENTS(pins); p++) {
        uint8_t output = pins[p] & (p == 3 ? 0xdf : 0xff);

        g_assert_cmphex(qtest_readb(qts, PDR(p)), ==, 0);
        g_assert_cmphex(qtest_readb(qts, PODR(p)), ==, 0);
        g_assert_cmphex(qtest_readb(qts, PMR(p)), ==, 0);
        g_assert_cmphex(qtest_readb(qts, PCR(p)), ==, 0);
        qtest_writeb(qts, PODR(p), 0xff);
        g_assert_cmphex(qtest_readb(qts, PODR(p)), ==, output);
        qtest_writeb(qts, PDR(p), 0xff);
        g_assert_cmphex(port_property(qts, "levels", p), ==, output);
        g_assert_cmphex(port_property(qts, "driving", p), ==, output);
        g_assert_cmphex(qtest_readb(qts, PIDR(p)), ==, output);
        qtest_writeb(qts, PIDR(p), 0);
        g_assert_cmphex(qtest_readb(qts, PIDR(p)), ==, output);
    }
    /* No DSCR exists in chapter 18 or the address table. */
    qtest_writeb(qts, BASE + 0xe1, 0xff);
    g_assert_cmphex(qtest_readb(qts, BASE + 0xe1), ==, 0);
    qtest_qmp_assert_success(qts, "{'execute': 'system_reset'}");
    g_assert_cmphex(qtest_readb(qts, PWPR), ==, 0x80);
    g_assert_cmphex(port_property(qts, "driving", 1), ==, 0);
    qtest_quit(qts);
}

static void test_input_output(void)
{
    QTestState *qts = qtest_init("-M rx113-r5f51138");

    input(qts, 1, 0, 1);
    input(qts, 1, 3, 1);
    g_assert_cmphex(qtest_readb(qts, PIDR(1)), ==, 9);
    qtest_writeb(qts, PODR(1), 0xa5);
    qtest_writeb(qts, PDR(1), 0xf0);
    g_assert_cmphex(port_property(qts, "levels", 1), ==, 0xa9);
    g_assert_cmphex(port_property(qts, "driving", 1), ==, 0xf0);
    g_assert_cmphex(qtest_readb(qts, PIDR(1)), ==, 0xa9);
    input(qts, 1, 0, 0);
    g_assert_cmphex(qtest_readb(qts, PIDR(1)), ==, 0xa8);
    /* PMR disables GPIO output, even when PDR/PODR still request high. */
    qtest_writeb(qts, PMR(1), 0x80);
    g_assert_cmphex(port_property(qts, "levels", 1), ==, 0x28);
    g_assert_cmphex(port_property(qts, "driving", 1), ==, 0x70);
    input(qts, 1, 7, 1);
    g_assert_cmphex(qtest_readb(qts, PIDR(1)), ==, 0xa8);
    /* P35 is input-only and has no PFS. */
    input(qts, 3, 5, 1);
    qtest_writeb(qts, PDR(3), 0xff);
    qtest_writeb(qts, PMR(3), 0xff);
    g_assert_cmphex(qtest_readb(qts, PIDR(3)), ==, 0x20);
    g_assert_cmphex(port_property(qts, "driving", 3), ==, 0);
    /* Board input state is not erased by an MCU reset. */
    qtest_qmp_assert_success(qts, "{'execute': 'system_reset'}");
    g_assert_cmphex(qtest_readb(qts, PIDR(1)), ==, 0x88);
    qtest_quit(qts);
}

static void test_open_drain_pullup(void)
{
    QTestState *qts = qtest_init("-M rx113-r5f51138");

    qtest_writeb(qts, PCR(1), 1);
    g_assert_cmphex(qtest_readb(qts, PIDR(1)), ==, 1);
    input(qts, 1, 0, 0);
    g_assert_cmphex(qtest_readb(qts, PIDR(1)), ==, 0);
    input(qts, 1, 0, -1);
    g_assert_cmphex(qtest_readb(qts, PIDR(1)), ==, 1);
    qtest_writeb(qts, PDR(1), 1);
    qtest_writeb(qts, ODR(1), 1); /* N-channel open drain */
    g_assert_cmphex(port_property(qts, "driving", 1), ==, 1);
    qtest_writeb(qts, PODR(1), 1);
    /* Pull-up is disabled in output mode, including open drain release. */
    g_assert_cmphex(port_property(qts, "driving", 1), ==, 0);
    g_assert_cmphex(qtest_readb(qts, PIDR(1)), ==, 0);
    input(qts, 1, 0, 1);
    g_assert_cmphex(qtest_readb(qts, PIDR(1)), ==, 1);
    qtest_writeb(qts, ODR(1), 2); /* P-channel, drives only high */
    g_assert_cmphex(port_property(qts, "driving", 1), ==, 1);
    qtest_writeb(qts, PODR(1), 0);
    g_assert_cmphex(port_property(qts, "driving", 1), ==, 0);
    /* Reserved ODR fields, P54/P55, P35, and missing pull-ups. */
    qtest_writeb(qts, ODR(1), 0xff);
    g_assert_cmphex(qtest_readb(qts, ODR(1)), ==, 0x57);
    qtest_writeb(qts, ODR(5) + 1, 0xff);
    g_assert_cmphex(qtest_readb(qts, ODR(5) + 1), ==, 0x10);
    qtest_writeb(qts, PCR(4), 0xff);
    qtest_writeb(qts, PCR(18), 0xff);
    g_assert_cmphex(qtest_readb(qts, PCR(4)), ==, 0);
    g_assert_cmphex(qtest_readb(qts, PCR(18)), ==, 0x0d);
    qtest_quit(qts);
}

static void test_mpc(void)
{
    QTestState *qts = qtest_init("-M rx113-r5f51138");

    qtest_writeb(qts, PFS(14, 0), 0xff);
    g_assert_cmphex(qtest_readb(qts, PFS(14, 0)), ==, 0);
    /* Cannot clear B0WI and set PFSWE at once. */
    qtest_writeb(qts, PWPR, 0x40);
    g_assert_cmphex(qtest_readb(qts, PWPR), ==, 0);
    qtest_writeb(qts, PFS(14, 0), 0xff);
    g_assert_cmphex(qtest_readb(qts, PFS(14, 0)), ==, 0);
    qtest_writeb(qts, PWPR, 0x40);
    qtest_writeb(qts, PFS(14, 0), 0xff);
    g_assert_cmphex(qtest_readb(qts, PFS(14, 0)), ==, 0xdf);
    qtest_writeb(qts, PWPR, 0xc0);
    qtest_writeb(qts, PWPR, 0x80); /* B0WI locks the existing PFSWE=1. */
    g_assert_cmphex(qtest_readb(qts, PWPR), ==, 0xc0);
    qtest_writeb(qts, PWPR, 0);
    g_assert_cmphex(qtest_readb(qts, PWPR), ==, 0x40);
    qtest_writeb(qts, PWPR, 0);
    qtest_writeb(qts, PWPR, 0x80);
    qtest_writeb(qts, PFS(14, 0), 0);
    g_assert_cmphex(qtest_readb(qts, PFS(14, 0)), ==, 0xdf);
    unlock(qts);
    /* Per-pin reserved ISEL/ASEL bits (19.2.2..19.2.15). */
    qtest_writeb(qts, PFS(0, 2), 0xff);
    qtest_writeb(qts, PFS(2, 6), 0xff);
    qtest_writeb(qts, PFS(2, 7), 0xff);
    qtest_writeb(qts, PFS(4, 0), 0xff);
    qtest_writeb(qts, PFS(10, 3), 0xff);
    qtest_writeb(qts, PFS(10, 6), 0xff);
    qtest_writeb(qts, PFS(18, 3), 0xff);
    qtest_writeb(qts, PFS(18, 6), 0xff);
    qtest_writeb(qts, PFS(3, 5), 0xff);
    g_assert_cmphex(qtest_readb(qts, PFS(0, 2)), ==, 0x1f);
    g_assert_cmphex(qtest_readb(qts, PFS(2, 6)), ==, 0x1f);
    g_assert_cmphex(qtest_readb(qts, PFS(2, 7)), ==, 0xdf);
    g_assert_cmphex(qtest_readb(qts, PFS(4, 0)), ==, 0x80);
    g_assert_cmphex(qtest_readb(qts, PFS(10, 3)), ==, 0xdf);
    g_assert_cmphex(qtest_readb(qts, PFS(10, 6)), ==, 0x5f);
    g_assert_cmphex(qtest_readb(qts, PFS(18, 3)), ==, 0x1f);
    g_assert_cmphex(qtest_readb(qts, PFS(18, 6)), ==, 0x80);
    g_assert_cmphex(qtest_readb(qts, PFS(3, 5)), ==, 0);
    input(qts, 4, 0, 1);
    /* Analog selection disables the digital input. */
    g_assert_cmphex(qtest_readb(qts, PIDR(4)), ==, 0);
    qtest_writeb(qts, PFS(4, 0), 0);
    g_assert_cmphex(qtest_readb(qts, PIDR(4)), ==, 1);
    /* Table 19.3: select TXD0 on P10, then return to GPIO output. */
    qtest_writeb(qts, PODR(1), 1);
    qtest_writeb(qts, PDR(1), 1);
    qtest_writeb(qts, PFS(1, 0), 0x0a);
    qtest_writeb(qts, PWPR, 0);
    qtest_writeb(qts, PWPR, 0x80);
    qtest_writeb(qts, PFS(1, 0), 0);
    g_assert_cmphex(qtest_readb(qts, PFS(1, 0)), ==, 0x0a);
    g_assert_cmphex(port_property(qts, "driving", 1), ==, 1);
    qtest_writeb(qts, PMR(1), 1);
    /* SCI has no pad waveform interface; selected pad is undriven. */
    g_assert_cmphex(port_property(qts, "driving", 1), ==, 0);
    qtest_writeb(qts, PMR(1), 0);
    g_assert_cmphex(port_property(qts, "driving", 1), ==, 1);
    qtest_qmp_assert_success(qts, "{'execute': 'system_reset'}");
    g_assert_cmphex(qtest_readb(qts, PFS(14, 0)), ==, 0);
    g_assert_cmphex(qtest_readb(qts, PWPR), ==, 0x80);
    qtest_quit(qts);
}

static void test_irq_mux(void)
{
    QTestState *qts = qtest_init("-M rx113-r5f51138");

    /* Existing ICU supports raw pin edges: select rising edge for IRQ2. */
    qtest_writeb(qts, 0x87502, 8);
    input(qts, 13, 2, 1);
    g_assert_cmphex(qtest_readb(qts, 0x87042), ==, 0);
    input(qts, 13, 2, 0);
    unlock(qts);
    qtest_writeb(qts, PFS(13, 2), 0x40);
    input(qts, 13, 2, 1);
    g_assert_cmphex(qtest_readb(qts, 0x87042), ==, 1);
    qtest_writeb(qts, 0x87042, 0);
    qtest_writeb(qts, PFS(13, 2), 0);
    input(qts, 13, 2, 0);
    input(qts, 13, 2, 1);
    g_assert_cmphex(qtest_readb(qts, 0x87042), ==, 0);
    qtest_quit(qts);
}

static void test_package64(void)
{
    QTestState *qts = qtest_init("-M rx113-r5f51138 "
                               "-global rx113-port.package-pins=64");

    unlock(qts);
    qtest_writeb(qts, PODR(0), 0xff);
    qtest_writeb(qts, PFS(0, 2), 0xff);
    g_assert_cmphex(qtest_readb(qts, PODR(0)), ==, 0);
    g_assert_cmphex(qtest_readb(qts, PFS(0, 2)), ==, 0);
    qtest_writeb(qts, PODR(12), 3);
    g_assert_cmphex(qtest_readb(qts, PODR(12)), ==, 0);
    /* Switch PB6's pad to PC0 GPIO, while peripheral selection stays PB6. */
    qtest_writeb(qts, PSRA, 0x40);
    qtest_writeb(qts, PODR(12), 1);
    qtest_writeb(qts, PDR(12), 1);
    g_assert_cmphex(port_property(qts, "driving", 11), ==, 0x40);
    g_assert_cmphex(port_property(qts, "levels", 11), ==, 0x40);
    g_assert_cmphex(qtest_readb(qts, PIDR(12)), ==, 1);
    g_assert_cmphex(qtest_readb(qts, PIDR(11)), ==, 0);
    qtest_writeb(qts, PDR(12), 0);
    input(qts, 11, 6, 1);
    g_assert_cmphex(qtest_readb(qts, PIDR(12)), ==, 1);
    g_assert_cmphex(qtest_readb(qts, PIDR(11)), ==, 0);
    qtest_writeb(qts, PODR(11), 0xc0);
    g_assert_cmphex(qtest_readb(qts, PODR(11)), ==, 0x80);
    qtest_writeb(qts, PMR(11), 0x40);
    g_assert_cmphex(qtest_readb(qts, PMR(11)), ==, 0x40);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/rx113-port/registers", test_registers);
    qtest_add_func("/rx113-port/input-output", test_input_output);
    qtest_add_func("/rx113-port/open-drain-pullup", test_open_drain_pullup);
    qtest_add_func("/rx113-port/mpc", test_mpc);
    qtest_add_func("/rx113-port/irq-mux", test_irq_mux);
    qtest_add_func("/rx113-port/package64", test_package64);
    return g_test_run();
}
