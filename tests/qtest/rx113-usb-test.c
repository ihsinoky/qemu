/*
 * RX113 USBc tests, R01UH0448EJ0120 Rev.1.20 section 27, decisions D-012/13.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include <poll.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#define USB 0xa0000
#define MSTPCRB 0x80014
#define RUN_B (UINT32_MAX & ~(1U << 19))
#define SIE_PATH "/machine/mcu/usb0"

static void wr(QTestState *qts, unsigned a, uint16_t v)
{
    qtest_writew(qts, USB + a, v);
}

static uint16_t rd(QTestState *qts, unsigned a)
{
    return qtest_readw(qts, USB + a);
}

static void completion(QTestState *qts, const char *value)
{
    qtest_qmp_assert_success(qts, "{'execute':'qom-set','arguments':{"
                            "'path':%s,'property':'sie-completion',"
                            "'value':%s}}", SIE_PATH, value);
}

static void completion_fails(QTestState *qts, const char *value)
{
    QDict *rsp = qtest_qmp(qts, "{'execute':'qom-set','arguments':{"
                          "'path':%s,'property':'sie-completion',"
                          "'value':%s}}", SIE_PATH, value);

    g_assert_true(qdict_haskey(rsp, "error"));
    qobject_unref(rsp);
}

static void property_text(QTestState *qts, const char *property,
                          const char *expected)
{
    QDict *rsp = qtest_qmp(qts, "{'execute':'qom-get','arguments':{"
                          "'path':%s,'property':%s}}", SIE_PATH, property);

    g_assert_cmpstr(qdict_get_str(rsp, "return"), ==, expected);
    qobject_unref(rsp);
}

static void tx_snapshot(QTestState *qts, const char *expected)
{
    property_text(qts, "sie-tx", expected);
}

static QTestState *usb_init(void)
{
    QTestState *qts = qtest_init("-M rx113-r5f51138");

    qtest_writew(qts, 0x803fe, 0xa503);
    /* UCLK = (16 MHz / 2) * 6; independent of the CPU clock source. */
    qtest_writeb(qts, 0x80032, 0);
    qtest_clock_step(qts, 2048000);
    qtest_writew(qts, 0x8002c, 0x0b11);
    qtest_writeb(qts, 0x8002e, 0);
    qtest_clock_step(qts, 50000);
    g_assert_cmphex(qtest_readb(qts, 0x8003c) & 0x21, ==, 0x21);
    qtest_writel(qts, MSTPCRB, RUN_B);
    wr(qts, 0, 0x401);
    return qts;
}

static void pipe_config(QTestState *qts, unsigned n, uint16_t cfg,
                         uint16_t maxp)
{
    wr(qts, 0x64, n);
    wr(qts, 0x68, cfg);
    wr(qts, 0x6c, maxp);
}

static void input(QTestState *qts, const char *property, const char *value)
{
    qtest_qmp_assert_success(qts, "{'execute':'qom-set','arguments':{"
                            "'path':%s,'property':%s,'value':%s}}",
                            SIE_PATH, property, value);
}

static void test_control_host(void)
{
    QTestState *qts = usb_init();

    wr(qts, 0, 0x441);
    wr(qts, 8, 0x10);
    wr(qts, 0x32, 0x30);
    wr(qts, 0x54, 0x0680);
    wr(qts, 0x56, 0x100);
    wr(qts, 0x5a, 8);
    wr(qts, 0x60, 0x4000);
    property_text(qts, "sie-setup", "8006000100000800");
    completion(qts, "error:0");
    completion(qts, "nak:0");
    g_assert_cmphex(rd(qts, 0x42), ==, 0);
    g_assert_cmphex(rd(qts, 0x60) & 0x4000, ==, 0x4000);
    wr(qts, 0x54, 0xffff); /* Setup parameters locked until completion. */
    g_assert_cmphex(rd(qts, 0x54), ==, 0x0680);
    completion(qts, "ack:0");
    property_text(qts, "sie-setup", "");
    g_assert_cmphex(rd(qts, 0x42), ==, 0x10);
    g_assert_cmphex(rd(qts, 0x60) & 0x4000, ==, 0);
    wr(qts, 0x42, 0);
    wr(qts, 0x60, 0x4000);
    completion(qts, "error:0");
    completion(qts, "error:0");
    completion(qts, "error:0");
    g_assert_cmphex(rd(qts, 0x42), ==, 0x20);
    g_assert_cmphex(rd(qts, 0x60) & 0x4000, ==, 0);
    wr(qts, 0x5c, 0x80);
    wr(qts, 0x60, 0x81); /* First data packet is DATA1. */
    completion(qts, "rx0:0:aa"); /* Duplicate: no FIFO/toggle/interrupt. */
    g_assert_cmphex(rd(qts, 0x22), ==, 0);
    g_assert_cmphex(rd(qts, 0x46), ==, 0);
    g_assert_cmphex(rd(qts, 0x60) & 0x40, ==, 0x40);
    completion(qts, "rx1:0:aa");
    wr(qts, 0x60, 1); /* Short packet set NAK; rearm to test duplicate. */
    completion(qts, "rx1:0:bb"); /* Duplicate accepted even with full FIFO. */
    g_assert_cmphex(qtest_readb(qts, USB + 0x14), ==, 0xaa);
    wr(qts, 0x5c, 0x10);
    wr(qts, 0x20, 0x20);
    wr(qts, 0x60, 0x81);
    wr(qts, 0x22, 0x8000);
    tx_snapshot(qts, "0:;");
    completion(qts, "ack:0");
    g_assert_cmphex(rd(qts, 0x4a) & 1, ==, 1);
    qtest_quit(qts);
}

static void test_control_function(void)
{
    QTestState *qts = usb_init();

    input(qts, "bus-event", "reset");
    g_assert_cmphex(rd(qts, 0x40) & 0x107f, ==, 0x1010);
    completion(qts, "setup:0:8006000100000800");
    g_assert_cmphex(rd(qts, 0x54), ==, 0x0680);
    g_assert_cmphex(rd(qts, 0x56), ==, 0x100);
    g_assert_cmphex(rd(qts, 0x5a), ==, 8);
    g_assert_cmphex(rd(qts, 0x40) & 0x80f, ==, 0x809);
    wr(qts, 0x60, 5); /* VALID prevents both BUF and CCPL. */
    g_assert_cmphex(rd(qts, 0x60) & 7, ==, 0);
    wr(qts, 0x40, 0xfff7);
    wr(qts, 0x20, 0x20);
    wr(qts, 0x60, 1);
    qtest_writeb(qts, USB + 0x14, 0xab);
    wr(qts, 0x22, 0x8000);
    completion(qts, "ack:0");
    wr(qts, 0x40, 0xf7ff);
    input(qts, "sie-token", "out");
    g_assert_cmphex(rd(qts, 0x40) & 0x807, ==, 0x802);
    wr(qts, 0x60, 5);
    completion(qts, "rx1:0:"); /* Status neither enters FIFO nor toggles. */
    g_assert_cmphex(rd(qts, 0x40) & 0x807, ==, 0x800);
    g_assert_cmphex(rd(qts, 0x60) & 0x40, ==, 0);

    completion(qts, "setup:0:0005000700000000"); /* Address 1792 is invalid. */
    wr(qts, 0x40, 0xfff7);
    completion_fails(qts, "ack:0");
    completion(qts, "setup:0:0005070000000000");
    /* SET_ADDRESS auto response ignores VALID, PID and CCPL. */
    completion(qts, "ack:0");
    g_assert_cmphex(rd(qts, 0x40) & 0x1077, ==, 0x1020);

    completion(qts, "setup:0:0009010000000000");
    wr(qts, 0x40, 0xfff7);
    wr(qts, 0x60, 5);
    completion(qts, "ack:0");
    g_assert_cmphex(rd(qts, 0x40) & 0x1070, ==, 0x1030);
    completion(qts, "setup:0:0005080000000000"); /* Configured: software. */
    completion_fails(qts, "ack:0");
    input(qts, "bus-event", "reset");
    for (unsigned a = 0x54; a <= 0x5a; a += 2) {
        g_assert_cmphex(rd(qts, a), ==, 0);
    }
    g_assert_cmphex(rd(qts, 0x60) & 7, ==, 0);
    g_assert_cmphex(rd(qts, 0x40) & 0x107f, ==, 0x1010);
    qtest_quit(qts);
}

static void test_control_write_errors(void)
{
    QTestState *qts = usb_init();

    completion(qts, "setup:0:2109000000000200");
    wr(qts, 0x40, 0xfff7);
    wr(qts, 0x60, 1);
    completion(qts, "rx1:0:1234");
    g_assert_cmphex(rd(qts, 0x14), ==, 0); /* MBW=byte rejects a word. */
    g_assert_cmphex(qtest_readb(qts, USB + 0x14), ==, 0x12);
    g_assert_cmphex(qtest_readb(qts, USB + 0x14), ==, 0x34);
    input(qts, "sie-token", "in");
    g_assert_cmphex(rd(qts, 0x40) & 7, ==, 4);
    wr(qts, 0x60, 5);
    completion(qts, "ack:0");
    g_assert_cmphex(rd(qts, 0x40) & 7, ==, 0);

    completion(qts, "setup:0:2109000000000200");
    wr(qts, 0x40, 0xf7f7);
    wr(qts, 0x60, 1);
    wr(qts, 0x60, 0x100); /* Even SQCLR cannot authorize first DATA0. */
    wr(qts, 0x60, 1);
    completion(qts, "rx0:0:1234"); /* First write DATA0: sequence error. */
    g_assert_cmphex(rd(qts, 0x40) & 0x807, ==, 0x806);
    g_assert_cmphex(rd(qts, 0x60) & 3, ==, 3);
    completion(qts, "setup:0:8006000100000800");
    g_assert_cmphex(rd(qts, 0x40) & 7, ==, 6); /* Error held until W0C. */
    wr(qts, 0x40, 0xf7ff);
    g_assert_cmphex(rd(qts, 0x40) & 0x807, ==, 0x801);
    wr(qts, 0x40, 0xf7f7);
    input(qts, "sie-token", "out"); /* Read without data: sequence error. */
    g_assert_cmphex(rd(qts, 0x40) & 0x807, ==, 0x806);
    qtest_quit(qts);
}

static void test_bus_frame(void)
{
    QTestState *qts = usb_init();

    wr(qts, 0, 0x461); /* Host with pull-down. */
    wr(qts, 0x32, 0xd841);
    qtest_set_irq_in(qts, SIE_PATH, "line", 0, 1);
    g_assert_cmphex(rd(qts, 4) & 3, ==, 1);
    g_assert_cmphex(rd(qts, 0x42) & 0x4000, ==, 0x4000);
    qtest_clock_step(qts, 2499);
    g_assert_cmphex(rd(qts, 0x42) & 0x800, ==, 0);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(rd(qts, 0x42) & 0x800, ==, 0x800);
    wr(qts, 8, 0x40);
    property_text(qts, "bus-output",
                  "drive=se0;vbusen=0;exicen=0;pullup=0;pulldown=1");
    g_assert_cmphex(rd(qts, 8) & 7, ==, 4);
    wr(qts, 8, 0x10);
    g_assert_cmphex(rd(qts, 8) & 7, ==, 2);
    g_assert_cmphex(rd(qts, 4) & 0x40, ==, 0x40);
    qtest_clock_step(qts, 999999);
    g_assert_cmphex(rd(qts, 0x4c), ==, 0);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(rd(qts, 0x4c), ==, 1);
    g_assert_cmphex(rd(qts, 0x40) & 0x2000, ==, 0x2000);
    wr(qts, 0x40, 0xdfff);
    input(qts, "bus-event", "eof-error");
    g_assert_cmphex(rd(qts, 0x42) & 0x40, ==, 0x40);
    g_assert_cmphex(rd(qts, 8) & 0x10, ==, 0);
    qtest_clock_step(qts, 1000000);
    g_assert_cmphex(rd(qts, 0x4c), ==, 1);
    wr(qts, 8, 0x10);
    qtest_set_irq_in(qts, SIE_PATH, "line", 0, 0);
    qtest_clock_step(qts, 2500);
    g_assert_cmphex(rd(qts, 0x42) & 0x1000, ==, 0x1000);
    g_assert_cmphex(rd(qts, 8) & 0x10, ==, 0);
    qtest_set_irq_in(qts, SIE_PATH, "overcurrent", 1, 1);
    qtest_set_irq_in(qts, SIE_PATH, "id", 0, 1);
    g_assert_cmphex(rd(qts, 4) & 0xc004, ==, 0x8004);
    g_assert_cmphex(rd(qts, 0x42) & 0x8000, ==, 0x8000);
    qtest_set_irq_in(qts, SIE_PATH, "pddet", 0, 1);
    wr(qts, 0xb0, 0x90);
    g_assert_cmphex(rd(qts, 0xb0), ==, 0x290);
    g_assert_cmphex(rd(qts, 0x42) & 1, ==, 1);
    qtest_set_irq_in(qts, SIE_PATH, "vbus", 0, 1);
    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    qtest_writew(qts, 0x803fe, 0xa503);
    qtest_writel(qts, MSTPCRB, RUN_B);
    wr(qts, 0, 0x400);
    g_assert_cmphex(rd(qts, 4) & 0xc004, ==, 0x8004);
    g_assert_cmphex(rd(qts, 0x40) & 0x8080, ==, 0x80);
    g_assert_cmphex(rd(qts, 0x42), ==, 0); /* Held inputs: no new edge. */
    qtest_quit(qts);
}

static void test_dtc_fifo(void)
{
    for (unsigned config = 0; config < 16; config++) {
        bool bfre = config & 1;
        bool dclrm = config & 2;
        bool word = !(config & 8);
        unsigned width = word ? 0x400 : 0;
        unsigned f = config & 4 ? 1 : 0;
        unsigned port = f ? 0x1c : 0x18;
        unsigned sel = f ? 0x2c : 0x28;
        unsigned ctr = sel + 2;
        unsigned vec = 36 + f;
        QTestState *qts = usb_init();

        qtest_writel(qts, 0x80010, 0xcfffffff);
        qtest_writel(qts, 0x82404, 0x400);
        qtest_writeb(qts, 0x8240c, 1);
        qtest_writeb(qts, 0x87204, 1 << (vec % 8));
        qtest_writel(qts, 0x400 + vec * 4, 0x1000);
        qtest_writeb(qts, 0x87100 + vec, 1);
        /* Block: fixed FIFO source, incrementing RAM destination. */
        qtest_writel(qts, 0x1000, word ? 0x90180000 : 0x80180000);
        qtest_writel(qts, 0x1004, USB + port);
        qtest_writel(qts, 0x1008, 0x2000);
        qtest_writel(qts, 0x100c, word ? 0x02020001 : 0x04040001);
        pipe_config(qts, 1, 0x4001 | (bfre ? 0x400 : 0), 8);
        wr(qts, 0x70, 1);
        wr(qts, sel, width | 1 | (dclrm ? 0x2000 : 0));
        completion(qts, "rx:1:12345678");
        wr(qts, sel, width | 0x1001 | (dclrm ? 0x2000 : 0));
        qtest_clock_step(qts, 1);
        g_assert_cmphex(qtest_readl(qts, 0x2000), ==, 0x78563412);
        g_assert_cmphex(qtest_readb(qts, 0x87100 + vec), ==, 0);
        g_assert_cmphex(rd(qts, ctr) & 0x2000, ==,
                        bfre && !dclrm ? 0x2000 : 0);
        g_assert_cmphex(rd(qts, 0x46) & 2, ==, 2);
        wr(qts, ctr, 0x4000);
        wr(qts, sel, 0);
        wr(qts, 0x70, 0x200);
        wr(qts, 0x70, 0);
        pipe_config(qts, 1, 0x4011, 8);
        qtest_writeb(qts, 0x87100 + vec, 1);
        qtest_writel(qts, 0x1000, word ? 0x98000000 : 0x88000000);
        qtest_writel(qts, 0x1004, 0x2000);
        qtest_writel(qts, 0x1008, USB + port);
        qtest_writel(qts, 0x100c, word ? 0x02020001 : 0x04040001);
        wr(qts, 0x70, 1);
        wr(qts, sel, width | 0x1001);
        qtest_clock_step(qts, 1);
        tx_snapshot(qts, ""); /* DTC writes 4 bytes; BVAL still required. */
        wr(qts, ctr, 0x8000);
        tx_snapshot(qts, "1:12345678;");
        completion(qts, "ack:1");
        qtest_quit(qts);
    }
}

static void test_isochronous(void)
{
    QTestState *qts = usb_init();

    pipe_config(qts, 1, 0xc001, 8);
    wr(qts, 0x70, 1);
    input(qts, "sie-sof", "2047");
    g_assert_cmphex(rd(qts, 0x4c), ==, 2047);
    g_assert_cmphex(rd(qts, 0x48) & 2, ==, 2); /* Missed OUT interval. */
    wr(qts, 0x48, 0);
    completion(qts, "rx1:1:1234"); /* Iso accepts both DATA0 and DATA1. */
    g_assert_cmphex(rd(qts, 0x70) & 0x40, ==, 0);
    completion(qts, "rx0:1:5678"); /* FIFO full: overrun, discard. */
    g_assert_cmphex(rd(qts, 0x4c) & 0x8000, ==, 0x8000);
    g_assert_cmphex(rd(qts, 0x48) & 2, ==, 2);
    wr(qts, 0x20, 0x401);
    g_assert_cmphex(rd(qts, 0x14), ==, 0x3412);
    completion(qts, "error:1");
    g_assert_cmphex(rd(qts, 0x4c) & 0xc000, ==, 0xc000);
    g_assert_cmphex(rd(qts, 0x70) & 3, ==, 1); /* No bulk retry/NAK rule. */
    wr(qts, 0x4c, 0);
    g_assert_cmphex(rd(qts, 0x4c), ==, 2047);
    wr(qts, 0x40, 0xdfff);
    qtest_clock_step(qts, 1000000); /* Interpolate missing/damaged SOF. */
    g_assert_cmphex(rd(qts, 0x4c), ==, 2047);
    g_assert_cmphex(rd(qts, 0x40) & 0x2000, ==, 0x2000);

    wr(qts, 0x70, 0x200);
    wr(qts, 0x70, 0);
    pipe_config(qts, 1, 0xc211, 8); /* Function IN, two planes, IFIS. */
    wr(qts, 0x6e, 0x1000);
    wr(qts, 0x70, 1);
    qtest_writew(qts, USB + 0x14, 0xbbaa);
    wr(qts, 0x22, 0x8000);
    completion(qts, "ack:1"); /* Not eligible until next frame: underrun. */
    g_assert_cmphex(rd(qts, 0x4c) & 0x8000, ==, 0x8000);
    tx_snapshot(qts, "1:aabb;");
    input(qts, "sie-sof", "1"); /* First SOF enables prepared data. */
    tx_snapshot(qts, "1:aabb;");
    qtest_clock_step(qts, 1000000); /* Missed interval flushes oldest only. */
    tx_snapshot(qts, "");
    qtest_writew(qts, USB + 0x14, 0xddcc);
    wr(qts, 0x22, 0x8000);
    /* Temporarily inhibit flush: data becomes eligible at the next SOF. */
    wr(qts, 0x70, 0);
    input(qts, "sie-sof", "2");
    wr(qts, 0x70, 1);
    completion(qts, "ack:1");
    g_assert_cmphex(rd(qts, 0x70) & 0x40, ==, 0);
    input(qts, "bus-event", "suspend");
    qtest_clock_step(qts, 2000000);
    g_assert_cmphex(rd(qts, 0x4c) & 0x7ff, ==, 2);
    qtest_quit(qts);
}

static void test_auto_response(void)
{
    QTestState *qts = usb_init();

    pipe_config(qts, 1, 0x4011, 8);
    wr(qts, 0x46, 0);
    wr(qts, 0x70, 0x401);
    completion(qts, "ack:1");
    g_assert_cmphex(rd(qts, 0x70) & 0x40, ==, 0x40);
    completion(qts, "ack:1");
    g_assert_cmphex(rd(qts, 0x70) & 0x40, ==, 0);
    g_assert_cmphex(rd(qts, 0x46), ==, 0);
    g_assert_cmphex(rd(qts, 0x4a), ==, 0);
    wr(qts, 0x70, 0);
    pipe_config(qts, 1, 0x4001, 8);
    wr(qts, 0x70, 0x401);
    completion(qts, "nak:1");
    g_assert_cmphex(rd(qts, 0x48), ==, 2);
    qtest_quit(qts);
}

static void test_bit_masks(void)
{
    static const struct {
        unsigned address;
        uint16_t mask;
        uint16_t fixed;
    } fields[] = {
        { 0x30, 0xff00, 0 }, { 0x32, 0xd871, 0 },
        { 0x36, 0x03ff, 0 }, { 0x38, 0x03ff, 0 },
        { 0x3a, 0x03ff, 0 }, { 0x3c, 0x0140, 0 },
        { 0x54, 0xffff, 0 }, { 0x56, 0xffff, 0 },
        { 0x58, 0xffff, 0 }, { 0x5a, 0xffff, 0 },
        { 0x5c, 0x0090, 0 }, { 0x5e, 0xf07f, 0 },
        { 0xb0, 0x00bf, 0 }, { 0xcc, 0x0001, 2 },
        { 0xd0, 0x00c0, 0 }, { 0xd2, 0x00c0, 0 },
        { 0xd4, 0x00c0, 0 }, { 0xd6, 0x00c0, 0 },
        { 0xd8, 0x00c0, 0 }, { 0xda, 0x00c0, 0 },
    };
    QTestState *qts = usb_init();

    for (unsigned bit = 0; bit < 16; bit++) {
        uint16_t value = 0x400 | (1U << bit);

        wr(qts, 0, value);
        g_assert_cmphex(rd(qts, 0), ==, value & 0x579);
    }
    qtest_set_irq_in(qts, SIE_PATH, "line", 0, 1);
    wr(qts, 0, 0x500); /* CNEN monitors the digital line without USBE. */
    g_assert_cmphex(rd(qts, 4) & 3, ==, 1);
    wr(qts, 0, 0x400);
    g_assert_cmphex(rd(qts, 4) & 3, ==, 0);
    qtest_set_irq_in(qts, SIE_PATH, "line", 0, 0);
    wr(qts, 0, 0x441);
    for (unsigned i = 0; i < G_N_ELEMENTS(fields); i++) {
        for (unsigned bit = 0; bit < 16; bit++) {
            uint16_t value = 1U << bit;

            wr(qts, fields[i].address, value);
            g_assert_cmphex(rd(qts, fields[i].address), ==,
                            (value & fields[i].mask) | fields[i].fixed);
            wr(qts, fields[i].address, 0);
        }
    }
    for (unsigned f = 0; f < 3; f++) {
        unsigned sel = f ? 0x24 + 4 * f : 0x20;

        for (unsigned bit = 0; bit < 16; bit++) {
            uint16_t value = 1U << bit;

            wr(qts, sel, value);
            g_assert_cmphex(rd(qts, sel), ==,
                            value & (f ? 0xb50f : 0x852f));
        }
        wr(qts, sel, 0);
    }
    for (unsigned n = 1; n < 10; n++) {
        unsigned control = 0x6e + 2 * n;

        wr(qts, 0x64, n);
        for (unsigned bit = 0; bit < 16; bit++) {
            uint16_t value = 1U << bit;

            wr(qts, 0x68, value);
            g_assert_cmphex(rd(qts, 0x68), ==,
                            value & (n <= 5 ? 0xc69f : 0xc41f));
            wr(qts, 0x6c, value);
            g_assert_cmphex(rd(qts, 0x6c), ==,
                            value & (n <= 2 ? 0xf1ff :
                                     n <= 5 ? 0xf078 : 0xf07f));
            wr(qts, 0x6e, value);
            g_assert_cmphex(rd(qts, 0x6e), ==, value & 0x1007);
            wr(qts, control, value);
            g_assert_cmphex(rd(qts, control) & ~0xc040, ==,
                            value & (n <= 5 ? 0x603 : 0x203));
            wr(qts, control, 0x100); /* Clear sequence and actions. */
        }
        wr(qts, control, 0);
    }
    qtest_quit(qts);
}

static void test_wakeup(void)
{
    QTestState *qts = usb_init();

    qtest_set_irq_in(qts, SIE_PATH, "line", 0, 1);
    input(qts, "bus-event", "reset");
    input(qts, "bus-event", "suspend");
    wr(qts, 0x30, 0x4000);
    wr(qts, 8, 0x100);
    qtest_clock_step(qts, 1999999);
    g_assert_cmphex(rd(qts, 8) & 0x100, ==, 0x100);
    qtest_clock_step(qts, 1);
    property_text(qts, "bus-output",
                  "drive=k;vbusen=0;exicen=0;pullup=0;pulldown=0");
    qtest_clock_step(qts, 9999999);
    g_assert_cmphex(rd(qts, 8) & 0x100, ==, 0x100);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(rd(qts, 8) & 0x100, ==, 0);
    property_text(qts, "bus-output",
                  "drive=idle;vbusen=0;exicen=0;pullup=0;pulldown=0");
    wr(qts, 0, 1); /* RESM input continues with the register clock stopped. */
    qtest_set_irq_in(qts, SIE_PATH, "line", 0, 0);
    g_assert_cmphex(qtest_readb(qts, 0x8705a), ==, 1);
    wr(qts, 0, 0x401);
    g_assert_cmphex(rd(qts, 0x40) & 0x4040, ==, 0x4000);
    wr(qts, 0x40, 0xbfff);
    g_assert_cmphex(qtest_readb(qts, 0x8705a), ==, 0);
    qtest_quit(qts);
}

static void test_periodic_host(void)
{
    QTestState *qts = usb_init();

    wr(qts, 0, 0x441);
    wr(qts, 8, 0x10);
    pipe_config(qts, 6, 0x8006, 8);
    wr(qts, 0x6e, 1); /* Every second frame. */
    wr(qts, 0x7a, 1);
    completion_fails(qts, "rx:6:aa");
    qtest_clock_step(qts, 1000000);
    completion_fails(qts, "rx:6:aa");
    qtest_clock_step(qts, 1000000);
    completion(qts, "rx:6:aa");
    wr(qts, 0x20, 6);
    g_assert_cmphex(qtest_readb(qts, USB + 0x14), ==, 0xaa);
    completion_fails(qts, "rx:6:bb"); /* One periodic transaction per frame. */
    qtest_clock_step(qts, 2000000);
    completion(qts, "rx:6:bb");
    g_assert_cmphex(qtest_readb(qts, USB + 0x14), ==, 0xbb);
    pipe_config(qts, 1, 0xc001, 8);
    wr(qts, 0x70, 1);
    qtest_clock_step(qts, 1000000);
    completion(qts, "rx:1:12");
    completion_fails(qts, "rx:1:34");
    qtest_clock_step(qts, 1000000);
    completion(qts, "rx:1:34"); /* FIFO full: overrun retains first bytes. */
    g_assert_cmphex(rd(qts, 0x4c) & 0x8000, ==, 0x8000);
    wr(qts, 0x20, 1);
    g_assert_cmphex(qtest_readb(qts, USB + 0x14), ==, 0x12);
    qtest_quit(qts);
}

static void test_queued_zlp(void)
{
    for (unsigned dclrm = 0; dclrm < 2; dclrm++) {
        QTestState *qts = usb_init();

        pipe_config(qts, 1, 0x4601, 8);
        wr(qts, 0x70, 1);
        wr(qts, 0x28, 0x401 | (dclrm ? 0x2000 : 0));
        completion(qts, "rx:1:0102030405060708");
        completion(qts, "rx:1:");
        g_assert_cmphex(rd(qts, 0x46) & 2, ==, 0);
        for (unsigned i = 0; i < 4; i++) {
            qtest_readw(qts, USB + 0x18);
        }
        g_assert_cmphex(rd(qts, 0x46) & 2, ==, 2);
        g_assert_cmphex(rd(qts, 0x2a), ==, dclrm ? 0 : 0x2000);
        qtest_quit(qts);
    }
}

static void test_inventory(void)
{
    /* Independent Table 5.1 inventory, including all five TRE/TRN pairs. */
    static const uint8_t addresses[] = {
        0x00, 0x04, 0x08, 0x14, 0x18, 0x1c, 0x20, 0x28, 0x2c,
        0x22, 0x2a, 0x2e, 0x30, 0x32, 0x36, 0x38, 0x3a, 0x3c,
        0x40, 0x42, 0x46, 0x48, 0x4a, 0x4c, 0x54, 0x56, 0x58,
        0x5a, 0x5c, 0x5e, 0x60, 0x64, 0x68, 0x6c, 0x6e,
        0x70, 0x72, 0x74, 0x76, 0x78, 0x7a, 0x7c, 0x7e, 0x80,
        0x90, 0x92, 0x94, 0x96, 0x98, 0x9a, 0x9c, 0x9e, 0xa0,
        0xa2, 0xb0, 0xcc, 0xd0, 0xd2, 0xd4, 0xd6, 0xd8, 0xda,
    };
    QTestState *qts = qtest_init("-M rx113-r5f51138");

    g_assert_cmpuint(G_N_ELEMENTS(addresses), ==, 62);
    qtest_writew(qts, 0x803fe, 0xa503);
    qtest_writel(qts, MSTPCRB, RUN_B);
    for (unsigned i = 0; i < G_N_ELEMENTS(addresses); i++) {
        g_assert_cmphex(rd(qts, addresses[i]), ==, 0); /* SCKE = 0. */
    }
    wr(qts, 0, 0x400);
    for (unsigned i = 0; i < G_N_ELEMENTS(addresses); i++) {
        unsigned a = addresses[i];
        uint16_t reset = a == 0 ? 0x400 : a == 0x5e || a == 0x60 ? 0x40 :
                         a == 0xcc ? 2 : 0;

        g_assert_cmphex(rd(qts, a), ==, reset);
    }
    /* PIPEMAXP has a per-pipe reset of 64; no selection reads zero. */
    for (unsigned n = 1; n < 10; n++) {
        wr(qts, 0x64, n);
        g_assert_cmphex(rd(qts, 0x68), ==, 0);
        g_assert_cmphex(rd(qts, 0x6c), ==, 64);
        g_assert_cmphex(rd(qts, 0x6e), ==, 0);
    }
    /* D-012: these addresses are holes, not registers from another IP. */
    wr(qts, 0x02, 0xffff);
    wr(qts, 0x0c, 0xffff);
    wr(qts, 0x50, 0xffff);
    g_assert_cmphex(rd(qts, 0x02), ==, 0);
    g_assert_cmphex(rd(qts, 0x0c), ==, 0);
    g_assert_cmphex(rd(qts, 0x50), ==, 0);
    qtest_quit(qts);
}

static void test_windows_access(void)
{
    QTestState *qts = usb_init();

    for (unsigned n = 1; n < 10; n++) {
        pipe_config(qts, n, (n <= 5 ? 0x4000 : 0x8000) | n, n & 1 ? 32 : 64);
        wr(qts, 0x6e, n & 7);
    }
    for (unsigned n = 1; n < 10; n++) {
        wr(qts, 0x64, n);
        g_assert_cmphex(rd(qts, 0x68), ==, (n <= 5 ? 0x4000 : 0x8000) | n);
        g_assert_cmphex(rd(qts, 0x6c), ==, n & 1 ? 32 : 64);
        g_assert_cmphex(rd(qts, 0x6e), ==, n & 7);
    }
    wr(qts, 0x64, 0);
    wr(qts, 0x68, 0xffff);
    wr(qts, 0x6c, 0xffff);
    g_assert_cmphex(rd(qts, 0x68), ==, 0);
    g_assert_cmphex(rd(qts, 0x6c), ==, 0);
    wr(qts, 0xcc, 0xffff);
    g_assert_cmphex(rd(qts, 0xcc), ==, 3);
    wr(qts, 0xd0, 0xffff);
    g_assert_cmphex(rd(qts, 0xd0), ==, 0xc0);
    wr(qts, 0x30, 0xa500);
    qtest_writeb(qts, USB + 0x30, 0xff);
    qtest_writel(qts, USB + 0x30, UINT32_MAX);
    qtest_writew(qts, USB + 0x31, 0xffff);
    g_assert_cmphex(rd(qts, 0x30), ==, 0xa500);
    g_assert_cmphex(qtest_readb(qts, USB + 0x30), ==, 0);
    g_assert_cmphex(qtest_readl(qts, USB + 0x30), ==, 0);
    wr(qts, 0x54, 0xbeef); /* Setup registers are read-only in function mode. */
    g_assert_cmphex(rd(qts, 0x54), ==, 0);
    wr(qts, 0, 0x441);
    wr(qts, 0x54, 0xbeef);
    g_assert_cmphex(rd(qts, 0x54), ==, 0xbeef);
    wr(qts, 0, 0x440); /* USBE disable retains host setup registers. */
    g_assert_cmphex(rd(qts, 0x54), ==, 0xbeef);
    wr(qts, 0, 0x401);
    wr(qts, 0x46, 0xffff); /* Software cannot create a completion. */
    wr(qts, 0x48, 0xffff);
    wr(qts, 0x4a, 0xffff);
    g_assert_cmphex(rd(qts, 0x46), ==, 0);
    g_assert_cmphex(rd(qts, 0x48), ==, 0);
    g_assert_cmphex(rd(qts, 0x4a), ==, 0);
    qtest_quit(qts);
}

static void test_fifo(void)
{
    QTestState *qts = usb_init();

    /* TX packet is visible at the SIE only after BVAL, never looped back. */
    pipe_config(qts, 1, 0x4011, 64);
    wr(qts, 0x20, 0x401);
    wr(qts, 0x70, 1);
    g_assert_cmphex(rd(qts, 0x22), ==, 0x2000);
    qtest_writew(qts, USB + 0x14, 0x2211);
    qtest_writeb(qts, USB + 0x14, 0x33);
    tx_snapshot(qts, "");
    g_assert_cmphex(rd(qts, 0x14), ==, 0);
    wr(qts, 0x22, 0x8000);
    tx_snapshot(qts, "1:112233;");
    g_assert_cmphex(rd(qts, 0x22) & 0x2000, ==, 0);
    g_assert_cmphex(rd(qts, 0x4a), ==, 0);
    completion(qts, "ack:1");
    tx_snapshot(qts, "");
    g_assert_cmphex(rd(qts, 0x4a), ==, 2);
    wr(qts, 0x4a, 0xfffd);
    /* BCLR clears bytes but cannot assert BEMP. BVAL+BCLR sends a ZLP. */
    qtest_writeb(qts, USB + 0x14, 0xff);
    wr(qts, 0x22, 0x4000);
    g_assert_cmphex(rd(qts, 0x4a), ==, 0);
    qtest_writeb(qts, USB + 0x14, 0xff);
    wr(qts, 0x22, 0xc000);
    tx_snapshot(qts, "1:;");
    completion(qts, "ack:1");
    wr(qts, 0x4a, 0xfffd);
    wr(qts, 0x20, 0);
    wr(qts, 0x70, 0x200);
    wr(qts, 0x70, 0);
    g_assert_cmphex(rd(qts, 0x4a), ==, 0);

    /* RX data comes only from the SIE fixture; exercise rewind and RCNT. */
    pipe_config(qts, 2, 0x4002, 64);
    wr(qts, 0x72, 1);
    wr(qts, 0x20, 0x8402);
    g_assert_cmphex(rd(qts, 0x22), ==, 0);
    g_assert_cmphex(qtest_readb(qts, USB + 0x14), ==, 0);
    g_assert_cmphex(rd(qts, 0x48), ==, 0); /* Empty read is not a NAK. */
    completion(qts, "rx:2:112233");
    g_assert_cmphex(rd(qts, 0x22), ==, 0x2003);
    g_assert_cmphex(rd(qts, 0x14), ==, 0x2211);
    g_assert_cmphex(rd(qts, 0x22), ==, 0x2001);
    wr(qts, 0x20, 0xc402); /* REW self clears. */
    g_assert_cmphex(rd(qts, 0x20), ==, 0x8402);
    g_assert_cmphex(rd(qts, 0x14), ==, 0x2211);
    g_assert_cmphex(qtest_readb(qts, USB + 0x14), ==, 0x33);
    g_assert_cmphex(rd(qts, 0x22), ==, 0);
    completion(qts, "rx:2:");
    g_assert_cmphex(rd(qts, 0x22), ==, 0x2000);
    wr(qts, 0x22, 0x4000);
    g_assert_cmphex(rd(qts, 0x22), ==, 0);
    /* FIFO big endian swaps a word, not the order of byte accesses. */
    wr(qts, 0x20, 0x502);
    completion(qts, "rx:2:123456");
    g_assert_cmphex(rd(qts, 0x14), ==, 0x1234);
    g_assert_cmphex(qtest_readb(qts, USB + 0x14), ==, 0x56);
    qtest_quit(qts);
}

static void test_fifo_ports(void)
{
    static const unsigned ports[] = { 0x14, 0x18, 0x1c };
    static const unsigned selects[] = { 0x20, 0x28, 0x2c };
    static const unsigned controls[] = { 0x22, 0x2a, 0x2e };
    QTestState *qts = usb_init();

    for (unsigned f = 0; f < 3; f++) {
        for (unsigned word = 0; word < 2; word++) {
            pipe_config(qts, 3, 0x4013, 64);
            wr(qts, 0x74, 1);
            wr(qts, selects[f], 3 | (word ? 0x400 : 0));
            if (word) {
                qtest_writew(qts, USB + ports[f], 0x3412);
            } else {
                /* A wrong-width access must not consume buffer space. */
                qtest_writew(qts, USB + ports[f], 0xffff);
                qtest_writeb(qts, USB + ports[f], 0x12);
                qtest_writeb(qts, USB + ports[f], 0x34);
            }
            qtest_writeb(qts, USB + ports[f], 0x56);
            wr(qts, controls[f], 0x8000);
            tx_snapshot(qts, "3:123456;");
            completion(qts, "ack:3");
            wr(qts, selects[f], 0);
            wr(qts, 0x74, 0x200);
            wr(qts, 0x74, 0);
            pipe_config(qts, 3, 0x4003, 64);
            wr(qts, selects[f], 3 | (word ? 0x400 : 0));
            wr(qts, 0x74, 1);
            completion(qts, "rx:3:123456");
            if (word) {
                g_assert_cmphex(qtest_readw(qts, USB + ports[f]), ==, 0x3412);
            } else {
                g_assert_cmphex(qtest_readw(qts, USB + ports[f]), ==, 0);
                g_assert_cmphex(qtest_readb(qts, USB + ports[f]), ==, 0x12);
                g_assert_cmphex(qtest_readb(qts, USB + ports[f]), ==, 0x34);
            }
            g_assert_cmphex(qtest_readb(qts, USB + ports[f]), ==, 0x56);
            wr(qts, selects[f], 0);
            wr(qts, 0x74, 0x200);
            wr(qts, 0x74, 0);
        }
    }
    completion_fails(qts, "rx:0:1122"); /* PID remains NAK after reset. */
    completion_fails(qts, "rx:3:xyz");
    completion_fails(qts, "ack:99");
    qtest_quit(qts);
}

static void test_double_bfre_counter(void)
{
    QTestState *qts = usb_init();

    pipe_config(qts, 1, 0x4211, 8); /* Two TX planes, normal BRDY. */
    wr(qts, 0x70, 1);
    wr(qts, 0x20, 1);
    for (unsigned i = 0; i < 8; i++) {
        qtest_writeb(qts, USB + 0x14, i);
    }
    tx_snapshot(qts, "1:0001020304050607;");
    qtest_writeb(qts, USB + 0x14, 0xaa);
    wr(qts, 0x22, 0x8000);
    completion(qts, "ack:1");
    g_assert_cmphex(rd(qts, 0x4a), ==, 0);
    tx_snapshot(qts, "1:aa;");
    completion(qts, "ack:1");
    g_assert_cmphex(rd(qts, 0x4a), ==, 2);

    pipe_config(qts, 2, 0x4682, 8); /* BFRE, double buffer, SHTNAK. */
    wr(qts, 0x96, 2);
    wr(qts, 0x94, 0x300); /* Clear count, enable. */
    wr(qts, 0x72, 1);
    wr(qts, 0x20, 2);
    completion(qts, "rx:2:0102030405060708");
    g_assert_cmphex(rd(qts, 0x96), ==, 1);
    wr(qts, 0x94, 0x300); /* TRCLR is blocked while counting/data remains. */
    g_assert_cmphex(rd(qts, 0x96), ==, 1);
    completion(qts, "rx:2:090a0b0c0d0e0f10");
    g_assert_cmphex(rd(qts, 0x96), ==, 0);
    g_assert_cmphex(rd(qts, 0x72) & 3, ==, 0);
    g_assert_cmphex(rd(qts, 0x46) & 4, ==, 0);
    for (unsigned i = 1; i <= 16; i++) {
        g_assert_cmphex(qtest_readb(qts, USB + 0x14), ==, i);
    }
    g_assert_cmphex(rd(qts, 0x46) & 4, ==, 4);
    g_assert_cmphex(rd(qts, 0x22), ==, 0x2008); /* BFRE retains plane. */
    wr(qts, 0x22, 0x4000);
    g_assert_cmphex(rd(qts, 0x22), ==, 0);
    qtest_quit(qts);
}

static void test_interrupts(void)
{
    QTestState *qts = usb_init();

    pipe_config(qts, 1, 0x4011, 64); /* DIR change asserts BRDY. */
    g_assert_cmphex(rd(qts, 0x46), ==, 2);
    g_assert_cmphex(rd(qts, 0x40) & 0x100, ==, 0);
    wr(qts, 0x36, 2);
    g_assert_cmphex(rd(qts, 0x40) & 0x100, ==, 0x100);
    g_assert_cmphex(qtest_readb(qts, 0x87026), ==, 0);
    wr(qts, 0x30, 0x100);
    g_assert_cmphex(qtest_readb(qts, 0x87026), ==, 1); /* Vector 38. */
    g_assert_cmphex(qtest_readb(qts, 0x8705a), ==, 0); /* No USBR. */
    wr(qts, 0x40, 0); /* Summary flags are read-only. */
    g_assert_cmphex(rd(qts, 0x40) & 0x100, ==, 0x100);
    wr(qts, 0x46, 0xfffd);
    g_assert_cmphex(rd(qts, 0x40) & 0x100, ==, 0);
    wr(qts, 0x3c, 0x40); /* BRDYM links BRDY to readiness. */
    g_assert_cmphex(rd(qts, 0x46) & 2, ==, 2);
    wr(qts, 0x46, 0);
    g_assert_cmphex(rd(qts, 0x46) & 2, ==, 2);
    wr(qts, 0x28, 0x1001); /* D0FIFO ready request, vector 36. */
    g_assert_cmphex(qtest_readb(qts, 0x87024), ==, 1);
    qtest_writeb(qts, 0x87124, 1);
    qtest_writeb(qts, 0x87126, 1);
    qtest_writeb(qts, 0x8715a, 1);
    g_assert_cmphex(qtest_readb(qts, 0x87124), ==, 1);
    g_assert_cmphex(qtest_readb(qts, 0x87126), ==, 0);
    g_assert_cmphex(qtest_readb(qts, 0x8715a), ==, 0);
    qtest_quit(qts);
}

static void test_vbus(void)
{
    QTestState *qts = usb_init();

    wr(qts, 0x30, 0x8000);
    qtest_set_irq_in(qts, SIE_PATH, "vbus", 0, 1);
    g_assert_cmphex(rd(qts, 0x40) & 0x8080, ==, 0x8080);
    g_assert_cmphex(qtest_readb(qts, 0x87026), ==, 1); /* USBI edge. */
    g_assert_cmphex(qtest_readb(qts, 0x8705a), ==, 1); /* USBR level. */
    qtest_writeb(qts, 0x8705a, 0);
    g_assert_cmphex(qtest_readb(qts, 0x8705a), ==, 1);
    wr(qts, 0x40, 0x7fff);
    g_assert_cmphex(rd(qts, 0x40) & 0x8080, ==, 0x80);
    g_assert_cmphex(qtest_readb(qts, 0x8705a), ==, 0);
    wr(qts, 0, 1); /* External VBUS detection continues with SCKE = 0. */
    qtest_set_irq_in(qts, SIE_PATH, "vbus", 0, 0);
    g_assert_cmphex(qtest_readb(qts, 0x8705a), ==, 1);
    wr(qts, 0x40, 0x7fff); /* Cannot clear with the register clock stopped. */
    g_assert_cmphex(qtest_readb(qts, 0x8705a), ==, 1);
    wr(qts, 0, 0x401);
    g_assert_cmphex(rd(qts, 0x40) & 0x8080, ==, 0x8000);
    wr(qts, 0x40, 0x7fff);
    g_assert_cmphex(qtest_readb(qts, 0x8705a), ==, 0);
    qtest_quit(qts);
}

static void test_nrdy_and_stop(void)
{
    QTestState *qts = usb_init();

    pipe_config(qts, 1, 0x4011, 64);
    wr(qts, 0x70, 1);
    wr(qts, 0x38, 2);
    completion(qts, "nak:1"); /* Function IN token with no packet. */
    g_assert_cmphex(rd(qts, 0x48), ==, 2);
    g_assert_cmphex(rd(qts, 0x40) & 0x200, ==, 0x200);
    wr(qts, 0x48, 0xfffd);
    wr(qts, 0, 0x441); /* Host controller. */
    wr(qts, 0x08, 0x10);
    completion(qts, "nak:1"); /* Host NAK does not assert NRDY. */
    g_assert_cmphex(rd(qts, 0x48), ==, 0);
    completion(qts, "error:1");
    completion(qts, "error:1");
    g_assert_cmphex(rd(qts, 0x48), ==, 0);
    completion(qts, "error:1");
    g_assert_cmphex(rd(qts, 0x48), ==, 2);
    g_assert_cmphex(rd(qts, 0x70) & 3, ==, 0);
    wr(qts, 0x70, 1);
    wr(qts, 0x48, 0xfffd);
    completion(qts, "stall:1");
    g_assert_cmphex(rd(qts, 0x48), ==, 2);
    g_assert_cmphex(rd(qts, 0x70) & 3, ==, 3);
    wr(qts, 0x70, 2);
    wr(qts, 0x70, 0);
    wr(qts, 0x20, 1);
    qtest_writeb(qts, USB + 0x14, 0x12);
    wr(qts, 0x22, 0x8000);
    wr(qts, 0x70, 1);
    qtest_writel(qts, MSTPCRB, UINT32_MAX);
    wr(qts, 0x22, 0x4000);
    wr(qts, 0x64, 2);
    wr(qts, 0x68, 0);
    completion_fails(qts, "ack:1");
    g_assert_cmphex(rd(qts, 0), ==, 0);
    g_assert_cmphex(rd(qts, 0x68), ==, 0);
    qtest_writel(qts, MSTPCRB, RUN_B);
    g_assert_cmphex(rd(qts, 0x68), ==, 0x4011);
    tx_snapshot(qts, "1:12;");
    completion(qts, "ack:1");
    wr(qts, 0, 0x41); /* SCKE = 0 blocks MMIO and SIE. */
    g_assert_cmphex(rd(qts, 0x68), ==, 0);
    completion_fails(qts, "ack:1");
    wr(qts, 0, 0x441);
    qtest_writeb(qts, 0x8002e, 1); /* UPLL stop blocks SIE. */
    completion_fails(qts, "nak:1");
    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    qtest_writew(qts, 0x803fe, 0xa503);
    qtest_writel(qts, MSTPCRB, RUN_B);
    wr(qts, 0, 0x400);
    wr(qts, 0x64, 1);
    g_assert_cmphex(rd(qts, 0x68), ==, 0);
    g_assert_cmphex(rd(qts, 0x4a), ==, 0);
    tx_snapshot(qts, "");
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

static void test_timer_migration(void)
{
    QTestState *src = usb_init();
    QTestState *dst;
    int64_t now;
    g_autofree char *dir = g_dir_make_tmp("rx113-usb-timer-XXXXXX", NULL);
    g_autofree char *file = g_strdup_printf("%s/state", dir);
    g_autofree char *uri = g_strdup_printf("file:%s", file);

    wr(src, 0, 0x441);
    wr(src, 8, 0x10);
    wr(src, 0x32, 0x30);
    wr(src, 0x54, 0x0680);
    wr(src, 0x60, 0x4000);
    completion(src, "error:0");
    completion(src, "error:0");
    qtest_set_irq_in(src, SIE_PATH, "line", 0, 1);
    now = qtest_clock_step(src, 1000);
    qtest_qmp_assert_success(src, "{'execute':'migrate',"
                            "'arguments':{'uri':%s}}", uri);
    migration_wait(src);
    dst = qtest_init("-M rx113-r5f51138 -incoming defer");
    qtest_clock_set(dst, now);
    qtest_qmp_assert_success(dst, "{'execute':'migrate-incoming',"
                            "'arguments':{'uri':%s}}", uri);
    migration_wait(dst);
    g_assert_cmphex(qtest_readb(dst, 0x87026), ==, 0);
    qtest_clock_step(dst, 1499);
    g_assert_cmphex(rd(dst, 0x42) & 0x800, ==, 0);
    qtest_clock_step(dst, 1);
    g_assert_cmphex(rd(dst, 0x42) & 0x800, ==, 0x800);
    qtest_clock_step(dst, 997500);
    g_assert_cmphex(rd(dst, 0x4c), ==, 1);
    completion(dst, "error:0");
    g_assert_cmphex(rd(dst, 0x42) & 0x30, ==, 0x20);
    g_assert_cmphex(qtest_readb(dst, 0x87026), ==, 1);
    qtest_writeb(dst, 0x87026, 0);
    wr(dst, 0x42, 0xffff);
    g_assert_cmphex(qtest_readb(dst, 0x87026), ==, 0);
    qtest_quit(dst);
    qtest_quit(src);
    unlink(file);
    rmdir(dir);
}

static void test_migration(void)
{
    QTestState *src = usb_init();
    QTestState *dst;
    g_autofree char *dir = g_dir_make_tmp("rx113-usb-XXXXXX", NULL);
    g_autofree char *file = g_strdup_printf("%s/state", dir);
    g_autofree char *uri = g_strdup_printf("file:%s", file);

    pipe_config(src, 1, 0x4211, 64);
    wr(src, 0x70, 1);
    wr(src, 0x20, 1);
    qtest_writeb(src, USB + 0x14, 0xab);
    wr(src, 0x22, 0x8000);
    qtest_writeb(src, USB + 0x14, 0xcd); /* Partial second plane. */
    pipe_config(src, 2, 0x4002, 64);
    wr(src, 0x72, 1);
    wr(src, 0x2c, 2);
    completion(src, "rx:2:112233");
    g_assert_cmphex(qtest_readb(src, USB + 0x1c), ==, 0x11);
    completion(src, "setup:0:0005070000000000");
    qtest_set_irq_in(src, SIE_PATH, "vbus", 0, 1);
    qtest_set_irq_in(src, SIE_PATH, "overcurrent", 0, 1);
    wr(src, 0x40, 0x7fff);
    wr(src, 0x42, 0);
    qtest_writeb(src, 0x87026, 0);
    qtest_writel(src, MSTPCRB, UINT32_MAX);
    qtest_qmp_assert_success(src, "{'execute':'migrate',"
                            "'arguments':{'uri':%s}}", uri);
    migration_wait(src);
    dst = qtest_init("-M rx113-r5f51138 -incoming defer");
    qtest_qmp_assert_success(dst, "{'execute':'migrate-incoming',"
                            "'arguments':{'uri':%s}}", uri);
    migration_wait(dst);
    g_assert_cmphex(rd(dst, 0), ==, 0);
    qtest_writel(dst, MSTPCRB, RUN_B);
    g_assert_cmphex(rd(dst, 0x40) & 0x8080, ==, 0x80);
    g_assert_cmphex(rd(dst, 4) & 0xc000, ==, 0x4000);
    g_assert_cmphex(qtest_readb(dst, 0x87026), ==, 0);
    tx_snapshot(dst, "0:;1:ab;");
    completion(dst, "ack:0");
    g_assert_cmphex(rd(dst, 0x40) & 0x70, ==, 0x20);
    completion(dst, "ack:1");
    /* A partial CPU write inhibits BEMP after the other plane completes. */
    g_assert_cmphex(rd(dst, 0x4a) & 2, ==, 0);
    wr(dst, 0x22, 0x8000);
    tx_snapshot(dst, "1:cd;");
    completion(dst, "ack:1");
    g_assert_cmphex(rd(dst, 0x4a) & 2, ==, 2);
    g_assert_cmphex(qtest_readb(dst, USB + 0x1c), ==, 0x22);
    g_assert_cmphex(qtest_readb(dst, USB + 0x1c), ==, 0x33);
    qtest_quit(dst);
    qtest_quit(src);
    unlink(file);
    rmdir(dir);
}

/*
 * Production adapter tests (Issue #72): a real usb-storage device
 * is attached to the adapter's bus; no fixture completions are
 * used. The guest-side register flow mirrors the fixture host
 * tests, with clock-stepped polling in place of completions.
 */
static QTestState *adapter_init(const char *args)
{
    QTestState *qts = qtest_init(args);

    qtest_writew(qts, 0x803fe, 0xa503);
    qtest_writeb(qts, 0x80032, 0);
    qtest_clock_step(qts, 2048000);
    qtest_writew(qts, 0x8002c, 0x0b11);
    qtest_writeb(qts, 0x8002e, 0);
    qtest_clock_step(qts, 50000);
    g_assert_cmphex(qtest_readb(qts, 0x8003c) & 0x21, ==, 0x21);
    qtest_writel(qts, MSTPCRB, RUN_B);
    wr(qts, 0, 0x401);
    return qts;
}

static char *adapter_image(void)
{
    char *name = g_strdup("rx113-usb-img-XXXXXX");
    int fd = g_mkstemp(name);

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 1 << 20), ==, 0);
    close(fd);
    return name;
}

/* Poll a register mask while stepping whole 1 ms frames. */
static void adapter_wait(QTestState *qts, unsigned a, uint16_t mask,
                         uint16_t want)
{
    for (unsigned i = 0; i < 500; i++) {
        if ((rd(qts, a) & mask) == want) {
            return;
        }
        qtest_clock_step(qts, 1000000);
    }
    g_assert_cmphex(rd(qts, a) & mask, ==, want);
}

static void adapter_host_start(QTestState *qts)
{
    wr(qts, 0, 0x461); /* Host mode with pull-downs. */
    adapter_wait(qts, 0x42, 0x800, 0x800); /* ATTCH from the real device. */
    wr(qts, 8, 0x40); /* Bus reset: the adapter resets the device. */
    qtest_clock_step(qts, 1000000);
    wr(qts, 8, 0x10); /* UACT: SOF starts. */
    qtest_clock_step(qts, 2000000);
}

static void adapter_setup(QTestState *qts, uint16_t req, uint16_t val,
                          uint16_t len)
{
    wr(qts, 0x54, req);
    wr(qts, 0x56, val);
    wr(qts, 0x58, 0);
    wr(qts, 0x5a, len);
    wr(qts, 0x60, 0x4000); /* SUREQ */
    adapter_wait(qts, 0x60, 0x4000, 0); /* SIGN */
    g_assert_cmphex(rd(qts, 0x42) & 0x30, ==, 0x10);
    wr(qts, 0x42, 0xffef);
}

/* Read the data stage of a control IN transfer from the DCP FIFO. */
static void adapter_control_in_data(QTestState *qts, uint8_t *out,
                                    unsigned len)
{
    wr(qts, 0x5c, 0x80); /* DCPCFG: receiving, NAK on the short packet. */
    wr(qts, 0x46, 0xfffe); /* Clear BRDY before arming. */
    wr(qts, 0x60, 0x81); /* PID=BUF */
    wr(qts, 0x20, 0x8400); /* CFIFOSEL: pipe 0, RCNT, word access. */
    adapter_wait(qts, 0x22, 0x2000, 0x2000); /* FRDY */
    g_assert_cmpuint(rd(qts, 0x22) & 0x1ff, ==, len);
    for (unsigned i = 0; i < len; i++) {
        out[i] = qtest_readb(qts, USB + 0x14);
    }
    wr(qts, 0x22, 0x4000); /* BCLR */
}

/* The OUT status stage of a control IN transfer (a ZLP). */
static void adapter_control_in_status(QTestState *qts)
{
    wr(qts, 0x5c, 0x10); /* DCPCFG: transmitting. */
    wr(qts, 0x20, 0x20);
    wr(qts, 0x4a, 0xfffe); /* Clear BEMP before arming. */
    wr(qts, 0x60, 0x81); /* PID=BUF */
    wr(qts, 0x22, 0x8000); /* BVAL: commit the zero-length packet. */
    adapter_wait(qts, 0x4a, 1, 1); /* BEMP: the ZLP was sent. */
    wr(qts, 0x4a, 0xfffe);
}

/* The IN status stage of a no-data control OUT transfer (a ZLP). */
static void adapter_control_out_status(QTestState *qts)
{
    wr(qts, 0x5c, 0x80);
    wr(qts, 0x60, 0x81); /* PID=BUF */
    /* The status ZLP ends the transfer: the DCP falls back to NAK. */
    adapter_wait(qts, 0x60, 3, 0);
    wr(qts, 0x20, 0x8400);
    g_assert_cmphex(rd(qts, 0x22) & 0x1ff, ==, 0);
    wr(qts, 0x22, 0x4000); /* BCLR */
}

static void adapter_get_descriptor(QTestState *qts, uint8_t *desc,
                                   uint16_t vendor)
{
    adapter_setup(qts, 0x0680, 0x100, 18);
    adapter_control_in_data(qts, desc, 18);
    adapter_control_in_status(qts);
    g_assert_cmpuint(desc[0], ==, 18);
    g_assert_cmpuint(desc[1], ==, 1);
    g_assert_cmphex(desc[8] | (desc[9] << 8), ==, vendor);
    g_assert_cmphex(desc[10] | (desc[11] << 8), ==, 0x0001);
}

/*
 * port=1 pins the device to the root port; without it QEMU
 * chains an automatic hub onto a one-port bus.
 */
static QTestState *adapter_storage_init(const char *image)
{
    g_autofree char *args = g_strdup_printf(
        "-M rx113-r5f51138 -drive if=none,id=msd0,file=%s,format=raw "
        "-device usb-storage,drive=msd0,bus=usb-bus.0,port=1", image);

    return adapter_init(args);
}

static void test_adapter_enumerate(void)
{
    g_autofree char *image = adapter_image();
    QTestState *qts = adapter_storage_init(image);
    uint8_t desc[18];

    adapter_host_start(qts);
    adapter_get_descriptor(qts, desc, 0x46f4);
    /* SET_ADDRESS 5, then reach the device at its new address. */
    adapter_setup(qts, 0x0500, 5, 0);
    adapter_control_out_status(qts);
    wr(qts, 0xda, 0x80); /* DEVADD5: full speed. */
    wr(qts, 0x5e, 0x5040); /* DCPMAXP: DEVSEL=5, MPS=64. */
    adapter_get_descriptor(qts, desc, 0x46f4);
    /* SET_CONFIGURATION 1. */
    adapter_setup(qts, 0x0900, 1, 0);
    adapter_control_out_status(qts);
    qtest_quit(qts);
    unlink(image);
}

static void test_adapter_storage(void)
{
    static const uint8_t cbw[31] = {
        0x55, 0x53, 0x42, 0x43, 0x01, 0x00, 0x00, 0x00,
        0x24, 0x00, 0x00, 0x00, 0x80, 0x00, 0x06, 0x12,
        0x00, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    g_autofree char *image = adapter_image();
    QTestState *qts = adapter_storage_init(image);
    uint8_t desc[18];
    uint8_t data[36];

    adapter_host_start(qts);
    adapter_get_descriptor(qts, desc, 0x46f4);
    adapter_setup(qts, 0x0500, 5, 0);
    adapter_control_out_status(qts);
    wr(qts, 0xda, 0x80);
    wr(qts, 0x5e, 0x5040);
    adapter_setup(qts, 0x0900, 1, 0);
    adapter_control_out_status(qts);

    /* Bulk pipes at address 5: pipe 1 IN ep1, pipe 2 OUT ep2. */
    pipe_config(qts, 1, 0x4001, 0x5040);
    pipe_config(qts, 2, 0x4012, 0x5040);

    /* CBW (INQUIRY) out on pipe 2. */
    wr(qts, 0x20, 0x402); /* CFIFOSEL: pipe 2, word access. */
    wr(qts, 0x72, 1); /* PIPE2CTR: PID=BUF */
    for (unsigned i = 0; i < 30; i += 2) {
        qtest_writew(qts, USB + 0x14, cbw[i] | (cbw[i + 1] << 8));
    }
    qtest_writeb(qts, USB + 0x14, cbw[30]);
    wr(qts, 0x22, 0x8000); /* BVAL */
    adapter_wait(qts, 0x4a, 4, 4); /* BEMP pipe 2 */
    wr(qts, 0x4a, 0xfffb);

    /*
     * Arm the IN pipe only now: the BOT device STALLs an IN
     * token while it still expects a CBW.
     */
    wr(qts, 0x70, 1); /* PIPE1CTR: PID=BUF */

    /* INQUIRY data in on pipe 1. */
    wr(qts, 0x20, 0x8401);
    adapter_wait(qts, 0x22, 0x2000, 0x2000);
    g_assert_cmpuint(rd(qts, 0x22) & 0x1ff, ==, 36);
    for (unsigned i = 0; i < 36; i++) {
        data[i] = qtest_readb(qts, USB + 0x14);
    }
    wr(qts, 0x22, 0x4000); /* BCLR */
    g_assert_cmpuint(data[0], ==, 0); /* Direct-access device. */
    g_assert_true(memcmp(data + 8, "QEMU    ", 8) == 0);

    /* CSW in on pipe 1: signature and success status. */
    adapter_wait(qts, 0x22, 0x2000, 0x2000);
    g_assert_cmpuint(rd(qts, 0x22) & 0x1ff, ==, 13);
    for (unsigned i = 0; i < 13; i++) {
        data[i] = qtest_readb(qts, USB + 0x14);
    }
    wr(qts, 0x22, 0x4000);
    g_assert_cmphex(data[0] | (data[1] << 8) | (data[2] << 16) |
                    ((uint32_t)data[3] << 24), ==, 0x53425355);
    g_assert_cmpuint(data[12], ==, 0);
    qtest_quit(qts);
    unlink(image);
}

static void test_adapter_nak(void)
{
    QTestState *qts = adapter_init(
        "-M rx113-r5f51138 -device usb-kbd,bus=usb-bus.0,port=1");
    uint8_t desc[18];

    adapter_host_start(qts);
    adapter_get_descriptor(qts, desc, 0x0627);
    adapter_setup(qts, 0x0900, 1, 0); /* SET_CONFIGURATION 1 */
    adapter_control_out_status(qts);

    /* Interrupt IN pipe 6, endpoint 1, every second frame. */
    pipe_config(qts, 6, 0x8001, 8);
    wr(qts, 0x6e, 1); /* PIPEPERI: IITV=1 */
    wr(qts, 0x7a, 1); /* PIPE6CTR: PID=BUF */
    /*
     * No key is pressed: the device NAKs every poll. A host NAK
     * is a silent retry (section 27.3.3.2): no status bit, no
     * error escalation, the pipe stays armed and the polling
     * simply continues. (This build has no sendkey command, so
     * a real key report cannot be injected; liveness is proven
     * by a control transfer afterwards instead.)
     */
    for (unsigned i = 0; i < 5; i++) {
        qtest_clock_step(qts, 1000000);
    }
    g_assert_cmphex(rd(qts, 0x46) & 0x40, ==, 0); /* No BRDY. */
    g_assert_cmphex(rd(qts, 0x48) & 0x40, ==, 0); /* No NRDY. */
    g_assert_cmphex(rd(qts, 0x42) & 0x20, ==, 0); /* No SERR. */
    g_assert_cmphex(rd(qts, 0x7a) & 3, ==, 1); /* The pipe stays armed. */
    adapter_get_descriptor(qts, desc, 0x0627); /* The link is alive. */
    qtest_quit(qts);
}

/*
 * USB/IP function adapter (Issue #74): a minimal client speaks the
 * protocol over a socket while the test plays the firmware through
 * MMIO, exactly like the fixture tests play the host.
 */
typedef struct UsbipClient {
    int fd;
    uint8_t buf[16384];
    size_t len;
} UsbipClient;

static QTestState *usbip_init(uint16_t *port_out)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sin = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    socklen_t slen = sizeof(sin);
    uint16_t port;
    QTestState *qts;
    g_autofree char *args = NULL;
    g_autofree char *expect = NULL;

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(bind(fd, (struct sockaddr *)&sin, sizeof(sin)), ==, 0);
    g_assert_cmpint(getsockname(fd, (struct sockaddr *)&sin, &slen), ==, 0);
    port = ntohs(sin.sin_port);
    close(fd);
    args = g_strdup_printf("-M rx113-r5f51138 -global "
                           "rx113-usb.usbip-listen=127.0.0.1:%u", port);
    qts = qtest_init(args);
    qtest_writew(qts, 0x803fe, 0xa503);
    qtest_writeb(qts, 0x80032, 0);
    qtest_clock_step(qts, 2048000);
    qtest_writew(qts, 0x8002c, 0x0b11);
    qtest_writeb(qts, 0x8002e, 0);
    qtest_clock_step(qts, 50000);
    g_assert_cmphex(qtest_readb(qts, 0x8003c) & 0x21, ==, 0x21);
    qtest_writel(qts, MSTPCRB, RUN_B);
    wr(qts, 0, 0x401);
    expect = g_strdup_printf("%u", port);
    property_text(qts, "usbip-port", expect);
    *port_out = port;
    return qts;
}

static void usbip_connect(UsbipClient *c, uint16_t port)
{
    struct sockaddr_in sin = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };

    c->fd = socket(AF_INET, SOCK_STREAM, 0);
    g_assert_cmpint(c->fd, >=, 0);
    g_assert_cmpint(connect(c->fd, (struct sockaddr *)&sin, sizeof(sin)),
                    ==, 0);
    c->len = 0;
}

static void usbip_send(UsbipClient *c, const uint8_t *data, size_t len)
{
    while (len) {
        ssize_t n = send(c->fd, data, len, 0);

        g_assert_cmpint(n, >, 0);
        data += n;
        len -= n;
    }
}

static void usbip_put32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}

static uint32_t usbip_get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

/* Read until at least need bytes are buffered, stepping frames. */
static bool usbip_fill(QTestState *qts, UsbipClient *c, size_t need,
                       unsigned budget)
{
    while (c->len < need) {
        struct pollfd pfd = { .fd = c->fd, .events = POLLIN };

        if (!budget--) {
            return false;
        }
        if (poll(&pfd, 1, 2) > 0 && (pfd.revents & POLLIN)) {
            ssize_t n = recv(c->fd, c->buf + c->len,
                             sizeof(c->buf) - c->len, 0);

            g_assert_cmpint(n, >, 0);
            c->len += n;
        }
        qtest_clock_step(qts, 1000000);
    }
    return true;
}

static void usbip_consume(UsbipClient *c, size_t n)
{
    memmove(c->buf, c->buf + n, c->len - n);
    c->len -= n;
}

static void usbip_devlist(QTestState *qts, UsbipClient *c)
{
    uint8_t req[8] = { 0x01, 0x11, 0x80, 0x05, 0, 0, 0, 0 };

    usbip_send(c, req, sizeof(req));
    g_assert_true(usbip_fill(qts, c, 8 + 4 + 312, 20000));
    g_assert_cmphex(c->buf[2] << 8 | c->buf[3], ==, 0x0005);
    g_assert_cmphex(usbip_get32(c->buf + 8), ==, 1);
    g_assert_cmpstr((const char *)c->buf + 8 + 4 + 256, ==, "1-1");
    usbip_consume(c, 8 + 4 + 312);
}

static void usbip_import(QTestState *qts, UsbipClient *c)
{
    uint8_t req[40] = { 0x01, 0x11, 0x80, 0x03 };

    memcpy(req + 8, "1-1", 3);
    usbip_send(c, req, sizeof(req));
    /* OP_REP_IMPORT carries the full 312-byte device record. */
    g_assert_true(usbip_fill(qts, c, 8 + 312, 20000));
    g_assert_cmphex(c->buf[2] << 8 | c->buf[3], ==, 0x0003);
    g_assert_cmphex(usbip_get32(c->buf + 4), ==, 0);
    g_assert_cmpstr((const char *)c->buf + 8 + 256, ==, "1-1");
    usbip_consume(c, 8 + 312);
    adapter_wait(qts, 0x40, 0x80, 0x80); /* VBSTS follows the import. */
}

static void usbip_pullup(QTestState *qts)
{
    wr(qts, 0x5e, 0x40); /* DCPMAXP: 64-byte control pipe. */
    wr(qts, 0, 0x409); /* DPRPU: the bridge answers with a bus reset. */
    adapter_wait(qts, 0x40, 0x70, 0x10); /* Device state: Powered. */
}

static void usbip_submit(UsbipClient *c, uint32_t seq, uint32_t ep,
                         bool in, uint32_t len, const uint8_t *setup,
                         const uint8_t *out)
{
    uint8_t hdr[48] = { 0 };

    usbip_put32(hdr + 0, 1);
    usbip_put32(hdr + 4, seq);
    usbip_put32(hdr + 8, 0x10001);
    usbip_put32(hdr + 12, in ? 1 : 0);
    usbip_put32(hdr + 16, ep);
    usbip_put32(hdr + 24, len);
    usbip_put32(hdr + 32, 0xffffffff);
    if (setup) {
        memcpy(hdr + 40, setup, 8);
    }
    usbip_send(c, hdr, sizeof(hdr));
    if (!in && out && len) {
        usbip_send(c, out, len);
    }
}

static void usbip_expect_ret(QTestState *qts, UsbipClient *c, uint32_t seq,
                             int32_t want_status, uint8_t *payload,
                             uint32_t *actual_out)
{
    uint32_t actual;

    g_assert_true(usbip_fill(qts, c, 48, 20000));
    g_assert_cmphex(usbip_get32(c->buf + 0), ==, 3);
    g_assert_cmphex(usbip_get32(c->buf + 4), ==, seq);
    g_assert_cmpint((int32_t)usbip_get32(c->buf + 20), ==, want_status);
    actual = usbip_get32(c->buf + 24);
    if (actual_out) {
        *actual_out = actual;
    }
    if (actual) {
        g_assert_true(usbip_fill(qts, c, 48 + actual, 20000));
        if (payload) {
            memcpy(payload, c->buf + 48, actual);
        }
    }
    usbip_consume(c, 48 + actual);
}

/* Drive one control transfer's firmware side to the status stage. */
static void usbip_fw_status(QTestState *qts)
{
    adapter_wait(qts, 0x40, 0x8, 0x8); /* VALID */
    wr(qts, 0x40, 0xfff7); /* release the setup */
}

static void test_usbip_enumerate(void)
{
    uint16_t port;
    QTestState *qts = usbip_init(&port);
    UsbipClient c;
    static const uint8_t get_desc[8] = {
        0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x08, 0x00
    };
    static const uint8_t desc[8] = {
        0x12, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x40
    };
    static const uint8_t set_conf[8] = {
        0x00, 0x09, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    uint8_t payload[64];
    uint32_t actual = 0;

    usbip_connect(&c, port);
    usbip_devlist(qts, &c);
    usbip_import(qts, &c);
    usbip_pullup(qts);

    usbip_submit(&c, 1, 0, true, 8, get_desc, NULL);
    usbip_fw_status(qts);
    wr(qts, 0x20, 0x20);
    wr(qts, 0x60, 1);
    for (unsigned i = 0; i < 8; i++) {
        qtest_writeb(qts, USB + 0x14, desc[i]);
    }
    wr(qts, 0x22, 0x8000); /* BVAL */
    adapter_wait(qts, 0x40, 0x7, 0x2); /* Control read status stage. */
    wr(qts, 0x60, 5); /* CCPL */
    usbip_expect_ret(qts, &c, 1, 0, payload, &actual);
    g_assert_cmpuint(actual, ==, 8);
    g_assert_true(memcmp(payload, desc, 8) == 0);

    usbip_submit(&c, 2, 0, false, 0, set_conf, NULL);
    usbip_fw_status(qts);
    wr(qts, 0x60, 5); /* CCPL */
    usbip_expect_ret(qts, &c, 2, 0, NULL, &actual);
    g_assert_cmpuint(actual, ==, 0);
    g_assert_cmphex(rd(qts, 0x40) & 0x70, ==, 0x30); /* Configured. */
    close(c.fd);
    qtest_quit(qts);
}

static void test_usbip_bulk(void)
{
    uint16_t port;
    QTestState *qts = usbip_init(&port);
    UsbipClient c;
    static const char out_text[] = "USBIP-OUT!";
    static const char in_text[] = "USBIP-IN!!";
    uint8_t payload[64];
    uint32_t actual = 0;

    usbip_connect(&c, port);
    usbip_import(qts, &c);
    usbip_pullup(qts);
    pipe_config(qts, 1, 0x4011, 64); /* Bulk, SIE sends, EP1. */
    pipe_config(qts, 2, 0x4002, 64); /* Bulk, SIE receives, EP2. */

    wr(qts, 0x72, 1); /* Arm pipe 2 (BUF). */
    usbip_submit(&c, 10, 2, false, 10, NULL,
                 (const uint8_t *)out_text);
    usbip_expect_ret(qts, &c, 10, 0, NULL, &actual);
    g_assert_cmpuint(actual, ==, 0);
    wr(qts, 0x20, 0x8402); /* CFIFO read, RCNT, pipe 2. */
    adapter_wait(qts, 0x22, 0x2000, 0x2000); /* FRDY */
    for (unsigned i = 0; i < 10; i++) {
        g_assert_cmphex(qtest_readb(qts, USB + 0x14), ==, out_text[i]);
    }
    wr(qts, 0x22, 0x4000); /* BCLR */

    wr(qts, 0x20, 0x401);
    wr(qts, 0x70, 1);
    for (unsigned i = 0; i < 10; i++) {
        qtest_writeb(qts, USB + 0x14, in_text[i]);
    }
    wr(qts, 0x22, 0x8000); /* BVAL */
    usbip_submit(&c, 11, 1, true, 10, NULL, NULL);
    usbip_expect_ret(qts, &c, 11, 0, payload, &actual);
    g_assert_cmpuint(actual, ==, 10);
    g_assert_true(memcmp(payload, in_text, 10) == 0);
    close(c.fd);
    qtest_quit(qts);
}

static void test_usbip_interrupt(void)
{
    uint16_t port;
    QTestState *qts = usbip_init(&port);
    UsbipClient c;
    static const uint8_t report[4] = { 0x01, 0x02, 0x03, 0x04 };
    uint8_t payload[64];
    uint32_t actual = 0;

    usbip_connect(&c, port);
    usbip_import(qts, &c);
    usbip_pullup(qts);
    pipe_config(qts, 6, 0x8011, 8); /* Interrupt, SIE sends, EP1. */

    wr(qts, 0x20, 0x406);
    wr(qts, 0x7a, 1);
    for (unsigned i = 0; i < 4; i++) {
        qtest_writeb(qts, USB + 0x14, report[i]);
    }
    wr(qts, 0x22, 0x8000); /* BVAL */
    usbip_submit(&c, 12, 1, true, 4, NULL, NULL);
    usbip_expect_ret(qts, &c, 12, 0, payload, &actual);
    g_assert_cmpuint(actual, ==, 4);
    g_assert_true(memcmp(payload, report, 4) == 0);
    close(c.fd);
    qtest_quit(qts);
}

static void test_usbip_unlink(void)
{
    uint16_t port;
    QTestState *qts = usbip_init(&port);
    UsbipClient c;
    uint8_t unlink[48] = { 0 };
    uint8_t payload[64];
    uint32_t actual = 0;

    usbip_connect(&c, port);
    usbip_import(qts, &c);
    usbip_pullup(qts);
    pipe_config(qts, 1, 0x4011, 64);

    usbip_submit(&c, 20, 1, true, 4, NULL, NULL);
    for (unsigned i = 0; i < 5; i++) {
        qtest_clock_step(qts, 1000000);
    }
    g_assert_false(usbip_fill(qts, &c, 1, 3)); /* Nothing completes. */

    usbip_put32(unlink + 0, 2);
    usbip_put32(unlink + 4, 21);
    usbip_put32(unlink + 8, 0x10001);
    usbip_put32(unlink + 20, 20);
    usbip_send(&c, unlink, sizeof(unlink));
    g_assert_true(usbip_fill(qts, &c, 48, 20000));
    g_assert_cmphex(usbip_get32(c.buf + 0), ==, 4); /* RET_UNLINK */
    g_assert_cmphex(usbip_get32(c.buf + 4), ==, 21);
    g_assert_cmpint((int32_t)usbip_get32(c.buf + 20), ==, 0);
    usbip_consume(&c, 48);
    for (unsigned i = 0; i < 5; i++) {
        qtest_clock_step(qts, 1000000);
    }
    g_assert_false(usbip_fill(qts, &c, 1, 3)); /* No RET_SUBMIT follows. */

    wr(qts, 0x20, 0x401);
    wr(qts, 0x70, 1);
    qtest_writeb(qts, USB + 0x14, 0xaa);
    qtest_writeb(qts, USB + 0x14, 0xbb);
    wr(qts, 0x22, 0x8000);
    usbip_submit(&c, 22, 1, true, 2, NULL, NULL);
    usbip_expect_ret(qts, &c, 22, 0, payload, &actual);
    g_assert_cmpuint(actual, ==, 2);
    g_assert_cmphex(payload[0], ==, 0xaa);
    g_assert_cmphex(payload[1], ==, 0xbb);
    close(c.fd);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/rx113-usb/adapter-enumerate", test_adapter_enumerate);
    qtest_add_func("/rx113-usb/adapter-storage", test_adapter_storage);
    qtest_add_func("/rx113-usb/adapter-nak", test_adapter_nak);
    qtest_add_func("/rx113-usb/usbip-enumerate", test_usbip_enumerate);
    qtest_add_func("/rx113-usb/usbip-bulk", test_usbip_bulk);
    qtest_add_func("/rx113-usb/usbip-interrupt", test_usbip_interrupt);
    qtest_add_func("/rx113-usb/usbip-unlink", test_usbip_unlink);
    qtest_add_func("/rx113-usb/queued-zlp", test_queued_zlp);
    qtest_add_func("/rx113-usb/wakeup", test_wakeup);
    qtest_add_func("/rx113-usb/periodic-host", test_periodic_host);
    qtest_add_func("/rx113-usb/bit-masks", test_bit_masks);
    qtest_add_func("/rx113-usb/isochronous", test_isochronous);
    qtest_add_func("/rx113-usb/auto-response", test_auto_response);
    qtest_add_func("/rx113-usb/bus-frame", test_bus_frame);
    qtest_add_func("/rx113-usb/dtc-fifo", test_dtc_fifo);
    qtest_add_func("/rx113-usb/control-host", test_control_host);
    qtest_add_func("/rx113-usb/control-function", test_control_function);
    qtest_add_func("/rx113-usb/control-write-errors",
                   test_control_write_errors);
    qtest_add_func("/rx113-usb/inventory", test_inventory);
    qtest_add_func("/rx113-usb/windows-access", test_windows_access);
    qtest_add_func("/rx113-usb/fifo", test_fifo);
    qtest_add_func("/rx113-usb/fifo-ports", test_fifo_ports);
    qtest_add_func("/rx113-usb/double-bfre-counter", test_double_bfre_counter);
    qtest_add_func("/rx113-usb/interrupts", test_interrupts);
    qtest_add_func("/rx113-usb/vbus", test_vbus);
    qtest_add_func("/rx113-usb/nrdy-stop-reset", test_nrdy_and_stop);
    qtest_add_func("/rx113-usb/timer-migration", test_timer_migration);
    qtest_add_func("/rx113-usb/migration", test_migration);
    return g_test_run();
}
