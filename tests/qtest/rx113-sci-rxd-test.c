/*
 * RX113 SCI RXD pad reception tests (R01UH0448EJ0120 sections 19 and
 * 28; Issue #65 stage 2). The PORT forwards the resolved RXD and SCK
 * pad levels to the SCI, which samples them with the model's bit
 * timing (one bit = one model slot: 4 us at PCLKB 32 MHz, CKS=0,
 * BRR=4). External levels are driven through the PORT "pin" inputs.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define SCKCR       0x80020
#define SCKCR3      0x80026
#define MSTPCRB     0x80014
#define PORT_BASE   0x8c000
#define PORT_PATH   "/machine/mcu/port"

#define SCI0        0x8a000

/* SCI register offsets */
#define SMR         0
#define BRR         1
#define SCR         2
#define TDR         3
#define SSR         4
#define RDR         5
#define SCMR        6

#define SCR_TE      0x20
#define SCR_RE      0x10
#define SCR_CKE_EXT 0x02
#define SSR_TEND    0x04
#define SSR_PER     0x08
#define SSR_FER     0x10
#define SSR_ORER    0x20
#define SSR_RDRF    0x40
#define SMR_CM      0x80
#define SMR_PE      0x20

#define BIT_NS      4000        /* BRR=4, CKS=0, PCLKB 32 MHz */
#define MSTPB_RUN   0x19ffffef

/* Port 2 pins: P20 = TXD0, P21 = RXD0, P22 = SCK0 (all PSEL=10). */
#define PIN_TXD0    16
#define PIN_RXD0    17
#define PIN_SCK0    18
#define NAME_TXD0   0x20
#define NAME_RXD0   0x21
#define NAME_SCK0   0x22

static QTestState *rxd_init(void)
{
    QTestState *qts = qtest_init("-M rx113-r5f51138");

    qtest_writew(qts, 0x803fe, 0xa503); /* PRCR: unlock clock/MSTP regs */
    qtest_writel(qts, SCKCR, 0);
    qtest_writew(qts, SCKCR3, 0x100); /* 32 MHz HOCO, PCLKB /1 */
    qtest_writel(qts, MSTPCRB, MSTPB_RUN);
    return qts;
}

static unsigned pad_property(QTestState *qts, const char *property,
                             unsigned name)
{
    QDict *rsp = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':'/machine/mcu/port',"
        "'property':%s}}", property);
    const char *str = qdict_get_str(rsp, "return");
    unsigned value;

    g_assert_cmpint(sscanf(str + (name >> 4) * 2, "%2x", &value), ==, 1);
    qobject_unref(rsp);
    return (value >> (name & 7)) & 1;
}

static void select_pad(QTestState *qts, unsigned name, unsigned psel)
{
    unsigned port = name >> 4, bit = name & 7;
    unsigned pmr = PORT_BASE + 0x60 + port;
    unsigned old = qtest_readb(qts, pmr);

    qtest_writeb(qts, pmr, old & ~(1 << bit));
    qtest_writeb(qts, PORT_BASE + 0x11f, 0);
    qtest_writeb(qts, PORT_BASE + 0x11f, 0x40);
    qtest_writeb(qts, PORT_BASE + 0x140 + port * 8 + bit, psel);
    qtest_writeb(qts, pmr, old | (1 << bit));
}

static void drive(QTestState *qts, unsigned pin, unsigned level)
{
    qtest_set_irq_in(qts, PORT_PATH, "pin", pin, level);
}

static void sci_setup(QTestState *qts, uint8_t smr)
{
    qtest_writeb(qts, SCI0 + SMR, smr);
    qtest_writeb(qts, SCI0 + BRR, 4);
}

/*
 * Send one asynchronous frame on RXD0. The start bit is driven at the
 * current virtual time; each following bit is set at the start of its
 * bit window and sampled at its centre by the model.
 */
static void send_async(QTestState *qts, const bool *bits, unsigned nbits,
                       bool stop_level)
{
    drive(qts, PIN_RXD0, 0); /* start bit */
    for (unsigned k = 0; k < nbits; k++) {
        qtest_clock_step(qts, BIT_NS);
        drive(qts, PIN_RXD0, bits[k]);
    }
    qtest_clock_step(qts, BIT_NS);
    drive(qts, PIN_RXD0, stop_level);
    qtest_clock_step(qts, BIT_NS); /* stop bit sampled at its centre */
    drive(qts, PIN_RXD0, 1);
}

static void bits_lsb(bool *bits, uint8_t byte, unsigned nbits)
{
    for (unsigned k = 0; k < nbits; k++) {
        bits[k] = (byte >> k) & 1;
    }
}

/* Asynchronous reception, with a false start rejected first. */
static void test_rx_async(void)
{
    QTestState *qts = rxd_init();
    bool bits[8];

    select_pad(qts, NAME_RXD0, 10);
    sci_setup(qts, 0);
    drive(qts, PIN_RXD0, 1);
    qtest_writeb(qts, SCI0 + SCR, SCR_RE);

    /* A low pulse shorter than half a bit is not a start bit. */
    drive(qts, PIN_RXD0, 0);
    qtest_clock_step(qts, 1000);
    drive(qts, PIN_RXD0, 1);
    qtest_clock_step(qts, BIT_NS * 12);
    g_assert_cmphex(qtest_readb(qts, SCI0 + SSR) & SSR_RDRF, ==, 0);

    bits_lsb(bits, 0xa5, 8);
    send_async(qts, bits, 8, true);
    g_assert_cmphex(qtest_readb(qts, SCI0 + SSR) & SSR_RDRF, ==, SSR_RDRF);
    g_assert_cmphex(qtest_readb(qts, SCI0 + RDR), ==, 0xa5);
    g_assert_cmphex(qtest_readb(qts, SCI0 + SSR) & SSR_RDRF, ==, 0);
    qtest_quit(qts);
}

/* Framing and parity errors latch with the byte; overrun drops it. */
static void test_rx_async_errors(void)
{
    QTestState *qts = rxd_init();
    bool bits[9];

    select_pad(qts, NAME_RXD0, 10);
    sci_setup(qts, 0);
    drive(qts, PIN_RXD0, 1);
    qtest_writeb(qts, SCI0 + SCR, SCR_RE);

    /* Stop bit held low: FER, byte still delivered. */
    bits_lsb(bits, 0x3c, 8);
    send_async(qts, bits, 8, false);
    g_assert_cmphex(qtest_readb(qts, SCI0 + SSR) & SSR_FER, ==, SSR_FER);
    g_assert_cmphex(qtest_readb(qts, SCI0 + RDR), ==, 0x3c);
    qtest_readb(qts, SCI0 + SSR);
    qtest_writeb(qts, SCI0 + SSR, 0); /* clear the error flags */

    /* Even parity, parity bit forced wrong: PER. */
    qtest_writeb(qts, SCI0 + SCR, 0);
    sci_setup(qts, SMR_PE);
    qtest_writeb(qts, SCI0 + SCR, SCR_RE);
    bits_lsb(bits, 0x00, 8);
    bits[8] = true; /* 0x00 has even parity 0 */
    send_async(qts, bits, 9, true);
    g_assert_cmphex(qtest_readb(qts, SCI0 + SSR) & SSR_PER, ==, SSR_PER);
    g_assert_cmphex(qtest_readb(qts, SCI0 + RDR), ==, 0x00);
    qtest_readb(qts, SCI0 + SSR);
    qtest_writeb(qts, SCI0 + SSR, 0);

    /* A second frame completing while RDRF is set overruns. */
    qtest_writeb(qts, SCI0 + SCR, 0);
    sci_setup(qts, 0);
    qtest_writeb(qts, SCI0 + SCR, SCR_RE);
    bits_lsb(bits, 0x11, 8);
    send_async(qts, bits, 8, true);
    bits_lsb(bits, 0x22, 8);
    send_async(qts, bits, 8, true);
    g_assert_cmphex(qtest_readb(qts, SCI0 + SSR) & SSR_ORER, ==, SSR_ORER);
    g_assert_cmphex(qtest_readb(qts, SCI0 + RDR), ==, 0x11);
    qtest_quit(qts);
}

/* Wait until the SCK0 pad shows the given level (bounded). */
static void wait_sck(QTestState *qts, unsigned level)
{
    for (unsigned i = 0; i < 200; i++) {
        if (pad_property(qts, "levels", NAME_SCK0) == level) {
            return;
        }
        qtest_clock_step(qts, 200);
    }
    g_assert_cmpuint(pad_property(qts, "levels", NAME_SCK0), ==, level);
}

/*
 * Reception-only clock generation (section 28.5.1): setting RE starts
 * the SCK output; RXD bits set during each low phase are sampled on
 * the rising edges.
 */
static void test_rx_sync_internal(void)
{
    QTestState *qts = rxd_init();
    bool bits[8];

    select_pad(qts, NAME_RXD0, 10);
    select_pad(qts, NAME_SCK0, 10);
    sci_setup(qts, SMR_CM);
    drive(qts, PIN_RXD0, 1);
    g_assert_cmpuint(pad_property(qts, "driving", NAME_SCK0), ==, 0);
    qtest_writeb(qts, SCI0 + SCR, SCR_RE);
    g_assert_cmpuint(pad_property(qts, "driving", NAME_SCK0), ==, 1);

    bits_lsb(bits, 0x5a, 8);
    drive(qts, PIN_RXD0, bits[0]); /* SCK already fell when RE was set */
    for (unsigned k = 1; k < 8; k++) {
        wait_sck(qts, 1); /* rising edge: previous bit sampled */
        wait_sck(qts, 0); /* next falling edge opens the bit window */
        drive(qts, PIN_RXD0, bits[k]);
    }
    wait_sck(qts, 1); /* bit 7 sampled */
    g_assert_cmphex(qtest_readb(qts, SCI0 + SSR) & SSR_RDRF, ==, SSR_RDRF);
    g_assert_cmphex(qtest_readb(qts, SCI0 + RDR), ==, 0x5a);

    /* Clearing RE stops the clock at the high level. */
    qtest_writeb(qts, SCI0 + SCR, 0);
    g_assert_cmpuint(pad_property(qts, "driving", NAME_SCK0), ==, 0);
    qtest_quit(qts);
}

/* Simultaneous transmission and reception on the generated clock. */
static void test_rx_sync_full_duplex(void)
{
    QTestState *qts = rxd_init();
    bool bits[8];

    select_pad(qts, NAME_TXD0, 10);
    select_pad(qts, NAME_RXD0, 10);
    select_pad(qts, NAME_SCK0, 10);
    sci_setup(qts, SMR_CM);
    drive(qts, PIN_RXD0, 1);
    qtest_writeb(qts, SCI0 + SCR, SCR_TE | SCR_RE);
    qtest_writeb(qts, SCI0 + TDR, 0xa5);

    bits_lsb(bits, 0x5a, 8);
    drive(qts, PIN_RXD0, bits[0]);
    for (unsigned k = 1; k < 8; k++) {
        wait_sck(qts, 1);
        wait_sck(qts, 0);
        drive(qts, PIN_RXD0, bits[k]);
    }
    wait_sck(qts, 1);
    g_assert_cmphex(qtest_readb(qts, SCI0 + SSR) & SSR_RDRF, ==, SSR_RDRF);
    g_assert_cmphex(qtest_readb(qts, SCI0 + RDR), ==, 0x5a);
    /* TEND follows the model's transfer timer (trtime = 9 slots). */
    qtest_clock_step(qts, BIT_NS * 3);
    g_assert_cmphex(qtest_readb(qts, SCI0 + SSR) & SSR_TEND, ==, SSR_TEND);
    qtest_quit(qts);
}

/*
 * External clock: the test drives SCK0 as an input. Reception samples
 * RXD on rising edges; the transmitter presents the next bit on each
 * falling edge.
 */
static void test_rx_sync_external(void)
{
    QTestState *qts = rxd_init();
    bool bits[8];
    /* 0x3c LSB first: 0 0 1 1 1 1 0 0 */
    static const bool tx_bits[8] = {
        false, false, true, true, true, true, false, false
    };

    select_pad(qts, NAME_TXD0, 10);
    select_pad(qts, NAME_RXD0, 10);
    select_pad(qts, NAME_SCK0, 10);
    sci_setup(qts, SMR_CM);
    drive(qts, PIN_RXD0, 1);
    drive(qts, PIN_SCK0, 1);
    qtest_writeb(qts, SCI0 + SCR, SCR_TE | SCR_RE | SCR_CKE_EXT);
    g_assert_cmpuint(pad_property(qts, "driving", NAME_SCK0), ==, 0);

    bits_lsb(bits, 0x96, 8);
    for (unsigned k = 0; k < 8; k++) {
        drive(qts, PIN_RXD0, bits[k]);
        drive(qts, PIN_SCK0, 0);
        drive(qts, PIN_SCK0, 1); /* rising edge samples the bit */
    }
    g_assert_cmphex(qtest_readb(qts, SCI0 + SSR) & SSR_RDRF, ==, SSR_RDRF);
    g_assert_cmphex(qtest_readb(qts, SCI0 + RDR), ==, 0x96);

    qtest_writeb(qts, SCI0 + TDR, 0x3c);
    g_assert_cmpuint(pad_property(qts, "levels", NAME_TXD0), ==, tx_bits[0]);
    for (unsigned k = 1; k < 8; k++) {
        drive(qts, PIN_SCK0, 0); /* falling edge presents bit k */
        g_assert_cmpuint(pad_property(qts, "levels", NAME_TXD0),
                         ==, tx_bits[k]);
        drive(qts, PIN_SCK0, 1);
    }
    drive(qts, PIN_SCK0, 0); /* eighth fall: frame complete, bit held */
    g_assert_cmpuint(pad_property(qts, "levels", NAME_TXD0), ==, tx_bits[7]);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/rx113-sci-rxd/rx-async", test_rx_async);
    qtest_add_func("/rx113-sci-rxd/rx-async-errors", test_rx_async_errors);
    qtest_add_func("/rx113-sci-rxd/rx-sync-internal", test_rx_sync_internal);
    qtest_add_func("/rx113-sci-rxd/rx-sync-full-duplex",
                   test_rx_sync_full_duplex);
    qtest_add_func("/rx113-sci-rxd/rx-sync-external", test_rx_sync_external);
    return g_test_run();
}
