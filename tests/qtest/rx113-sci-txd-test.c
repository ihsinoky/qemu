/*
 * RX113 SCI TXD pad waveform tests (R01UH0448EJ0120 sections 19 and 28;
 * Issue #50 stage 2). The waveform rides the shared SCI model's existing
 * transfer timing: a frame starts when the model hands the byte to its
 * backend, and one bit lasts trtime / frame_bits (4 us at PCLKB 32 MHz,
 * CKS=0, BRR=4).
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

#define SCI0        0x8a000
#define SCI1        0x8a020
#define SCI12       0x8b300

/* SCI register offsets */
#define SMR         0
#define BRR         1
#define SCR         2
#define TDR         3
#define SSR         4
#define SCMR        6

#define SCR_TE      0x20
#define SSR_TEND    0x04
#define SMR_PE      0x20
#define SMR_CHR     0x40
#define SCMR_SDIR   0x08

#define BIT_NS      4000        /* BRR=4, CKS=0, PCLKB 32 MHz */
#define MSTPB_RUN   0x19ffffef

static QTestState *txd_init(void)
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

static void assert_pad(QTestState *qts, unsigned name, bool driven, bool level)
{
    g_assert_cmpuint(pad_property(qts, "driving", name), ==, driven);
    g_assert_cmpuint(pad_property(qts, "levels", name), ==, level);
    g_assert_cmpuint((qtest_readb(qts, PORT_BASE + 0x40 + (name >> 4)) >>
                     (name & 7)) & 1, ==, level);
}

/* Sample the pad at the middle of frame bit pos (0 = start bit). */
static void assert_bit(QTestState *qts, unsigned name, int64_t base,
                       unsigned pos, bool level, int64_t *t)
{
    int64_t target = base + pos * BIT_NS + BIT_NS / 2;

    qtest_clock_step(qts, target - *t);
    *t = target;
    assert_pad(qts, name, true, level);
}

static void sci_setup(QTestState *qts, unsigned base, uint8_t smr)
{
    qtest_writeb(qts, base + SMR, smr);
    qtest_writeb(qts, base + BRR, 4);
}

/* 8N1 frame of 0x55 on SCI1 / TXD1 (P16), sampled bit by bit. */
static void test_frame_8n1(void)
{
    QTestState *qts = txd_init();
    static const bool bits[10] = {
        false,                          /* start */
        true, false, true, false, true, false, true, false, /* 0x55 LSB */
        true,                           /* stop */
    };
    int64_t t = 0;

    select_pad(qts, 0x16, 10);
    assert_pad(qts, 0x16, false, false); /* TE off: not driven */
    sci_setup(qts, SCI1, 0);
    qtest_writeb(qts, SCI1 + SCR, SCR_TE);
    assert_pad(qts, 0x16, true, true); /* idle */
    qtest_writeb(qts, SCI1 + TDR, 0x55);
    for (unsigned i = 0; i < 10; i++) {
        assert_bit(qts, 0x16, 0, i, bits[i], &t);
    }
    qtest_clock_step(qts, BIT_NS);
    assert_pad(qts, 0x16, true, true); /* idle again */
    g_assert_cmphex(qtest_readb(qts, SCI1 + SSR) & SSR_TEND, ==, SSR_TEND);
    qtest_quit(qts);
}

/* TE and module stop release both TXD pads of SCI0 (P10 and PB7). */
static void test_te_and_stop(void)
{
    QTestState *qts = txd_init();

    select_pad(qts, 0x10, 10); /* P10 = TXD0 */
    select_pad(qts, 0xb7, 23); /* PB7 = TXD0 */
    sci_setup(qts, SCI0, 0);
    qtest_writeb(qts, SCI0 + SCR, SCR_TE);
    assert_pad(qts, 0x10, true, true);
    assert_pad(qts, 0xb7, true, true);
    qtest_writeb(qts, SCI0 + SCR, 0); /* TE off */
    assert_pad(qts, 0x10, false, false);
    assert_pad(qts, 0xb7, false, false);
    qtest_writeb(qts, SCI0 + SCR, SCR_TE);
    assert_pad(qts, 0x10, true, true);
    qtest_writel(qts, MSTPCRB, MSTPB_RUN | (1U << 31)); /* stop SCI0 */
    assert_pad(qts, 0x10, false, false);
    assert_pad(qts, 0xb7, false, false);
    qtest_writel(qts, MSTPCRB, MSTPB_RUN);
    assert_pad(qts, 0x10, true, true);
    assert_pad(qts, 0xb7, true, true);
    qtest_quit(qts);
}

/* 7-bit data with even parity, then MSB-first order, on SCI12 / P14. */
static void test_formats(void)
{
    QTestState *qts = txd_init();
    static const bool bits[10] = {
        false,                          /* start */
        true, true, false, false, false, false, false, /* 0x03, 7 bits */
        false,                          /* even parity: two 1s -> 0 */
        true,                           /* stop */
    };
    int64_t t = 0;

    select_pad(qts, 0x14, 12); /* P14 = TXD12 */
    sci_setup(qts, SCI12, SMR_CHR | SMR_PE);
    qtest_writeb(qts, SCI12 + SCR, SCR_TE);
    qtest_writeb(qts, SCI12 + TDR, 0x03);
    for (unsigned i = 0; i < 10; i++) {
        assert_bit(qts, 0x14, 0, i, bits[i], &t);
    }
    qtest_clock_step(qts, BIT_NS);
    t += BIT_NS;
    g_assert_cmphex(qtest_readb(qts, SCI12 + SSR) & SSR_TEND, ==, SSR_TEND);

    /* MSB first (SCMR.SDIR): 0x01 sends its single 1 as the last bit. */
    qtest_writeb(qts, SCI12 + SCR, 0);
    qtest_writeb(qts, SCI12 + SCMR, SCMR_SDIR);
    sci_setup(qts, SCI12, 0);
    qtest_writeb(qts, SCI12 + SCR, SCR_TE);
    qtest_writeb(qts, SCI12 + TDR, 0x01);
    {
        int64_t base = t; /* the SDIR frame starts now */

        assert_bit(qts, 0x14, base, 1, false, &t); /* first data: bit7 = 0 */
        assert_bit(qts, 0x14, base, 8, true, &t);  /* last data: bit0 = 1 */
        assert_bit(qts, 0x14, base, 9, true, &t);  /* stop */
    }
    qtest_quit(qts);
}

/* A queued byte starts its frame at the previous frame's end. */
static void test_back_to_back(void)
{
    QTestState *qts = txd_init();
    int64_t t = 0;

    select_pad(qts, 0x16, 10);
    sci_setup(qts, SCI1, 0);
    qtest_writeb(qts, SCI1 + SCR, SCR_TE);
    qtest_writeb(qts, SCI1 + TDR, 0xff);
    assert_bit(qts, 0x16, 0, 0, false, &t); /* start of frame 1 */
    qtest_writeb(qts, SCI1 + TDR, 0x00); /* queued while frame 1 runs */
    assert_bit(qts, 0x16, 0, 9, true, &t);  /* stop of frame 1 */
    assert_bit(qts, 0x16, 0, 10, false, &t); /* start of frame 2, no gap */
    assert_bit(qts, 0x16, 0, 11, false, &t); /* data bit 0 of 0x00 */
    assert_bit(qts, 0x16, 0, 19, true, &t);  /* stop of frame 2 */
    qtest_clock_step(qts, BIT_NS);
    assert_pad(qts, 0x16, true, true);
    g_assert_cmphex(qtest_readb(qts, SCI1 + SSR) & SSR_TEND, ==, SSR_TEND);
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
            g_error("Migration failed");
        }
        qobject_unref(rsp);
        if (done) {
            return;
        }
        g_usleep(10000);
    }
    g_assert_not_reached();
}

/* A frame in progress survives migration and completes on the target. */
static void test_migration(void)
{
    QTestState *src = txd_init();
    QTestState *dst;
    int64_t now;
    g_autofree char *dir = g_dir_make_tmp("rx113-scitxd-XXXXXX", NULL);
    g_autofree char *file = g_strdup_printf("%s/state", dir);
    g_autofree char *uri = g_strdup_printf("file:%s", file);

    g_assert_nonnull(dir);
    select_pad(src, 0x16, 10);
    sci_setup(src, SCI1, 0);
    qtest_writeb(src, SCI1 + SCR, SCR_TE);
    qtest_writeb(src, SCI1 + TDR, 0x55);
    now = qtest_clock_step(src, 14000); /* data bit 2 of 0x55: high */
    assert_pad(src, 0x16, true, true);

    qtest_qmp_assert_success(src, "{'execute':'migrate',"
                            "'arguments':{'uri':%s}}", uri);
    migration_wait(src);
    dst = qtest_init("-M rx113-r5f51138 -incoming defer");
    qtest_clock_set(dst, now);
    qtest_qmp_assert_success(dst, "{'execute':'migrate-incoming',"
                            "'arguments':{'uri':%s}}", uri);
    migration_wait(dst);
    assert_pad(dst, 0x16, true, true); /* same bit, still driven */
    qtest_clock_step(dst, 4000); /* data bit 3 of 0x55: low */
    assert_pad(dst, 0x16, true, false);
    qtest_clock_step(dst, 26000); /* frame completes */
    assert_pad(dst, 0x16, true, true);
    g_assert_cmphex(qtest_readb(dst, SCI1 + SSR) & SSR_TEND, ==, SSR_TEND);
    qtest_quit(dst);
    qtest_quit(src);
    rmdir(dir);
}

/*
 * Clock-synchronous frame on SCI0: TXD0 on P20, SCK0 on P22 (both
 * PSEL=10). One bit lasts BIT_NS; SCK is low for the first half of each
 * bit (TXD changes on the falling edge) and high for the second half,
 * idles high, and TXD holds the last bit after the frame (section 28.5).
 */
static void sync_frame(QTestState *qts, const bool bits[8])
{
    /* At the frame start SCK has fallen and bit 0 is presented. */
    assert_pad(qts, 0x22, true, false);
    assert_pad(qts, 0x20, true, bits[0]);
    for (unsigned k = 0; k < 8; k++) {
        qtest_clock_step(qts, BIT_NS / 2);
        assert_pad(qts, 0x22, true, true); /* rising edge mid-bit */
        assert_pad(qts, 0x20, true, bits[k]);
        qtest_clock_step(qts, BIT_NS / 2);
        if (k < 7) {
            assert_pad(qts, 0x22, true, false); /* next falling edge */
            assert_pad(qts, 0x20, true, bits[k + 1]);
        }
    }
    /* After the frame SCK stays high and TXD holds the last bit. */
    assert_pad(qts, 0x22, true, true);
    assert_pad(qts, 0x20, true, bits[7]);
    qtest_clock_step(qts, BIT_NS);
    assert_pad(qts, 0x22, true, true);
    assert_pad(qts, 0x20, true, bits[7]);
}

static void test_sync_tx(void)
{
    QTestState *qts = txd_init();
    /* 0xA5 LSB first: 1 0 1 0 0 1 0 1 */
    static const bool lsb[8] = {
        true, false, true, false, false, true, false, true
    };
    /* 0xB1 MSB first: 1 0 1 1 0 0 0 1 */
    static const bool msb[8] = {
        true, false, true, true, false, false, false, true
    };

    select_pad(qts, 0x20, 10); /* P20 = TXD0 */
    select_pad(qts, 0x22, 10); /* P22 = SCK0 */
    assert_pad(qts, 0x22, false, false); /* TE off: not driven */
    sci_setup(qts, SCI0, 0x80); /* SMR.CM: clock synchronous mode */
    qtest_writeb(qts, SCI0 + SCR, SCR_TE);
    assert_pad(qts, 0x22, true, true); /* SCK idles high */
    qtest_writeb(qts, SCI0 + TDR, 0xa5);
    sync_frame(qts, lsb);
    g_assert_cmphex(qtest_readb(qts, SCI0 + SSR) & SSR_TEND, ==, SSR_TEND);

    qtest_writeb(qts, SCI0 + SCR, 0);
    qtest_writeb(qts, SCI0 + SCMR, SCMR_SDIR); /* MSB first */
    qtest_writeb(qts, SCI0 + SCR, SCR_TE);
    qtest_writeb(qts, SCI0 + TDR, 0xb1);
    sync_frame(qts, msb);
    qtest_quit(qts);
}

/* An external clock (SCR.CKE bit 1) leaves SCK undriven in this stage. */
static void test_sync_external_clock(void)
{
    QTestState *qts = txd_init();

    select_pad(qts, 0x20, 10);
    select_pad(qts, 0x22, 10);
    sci_setup(qts, SCI0, 0x80);
    qtest_writeb(qts, SCI0 + SCR, SCR_TE | 0x02); /* CKE: external */
    assert_pad(qts, 0x22, false, false);
    qtest_writeb(qts, SCI0 + TDR, 0xa5);
    qtest_clock_step(qts, BIT_NS * 10);
    assert_pad(qts, 0x22, false, false);
    assert_pad(qts, 0x20, true, true); /* TXD stays at its idle level */
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/rx113-sci-txd/frame-8n1", test_frame_8n1);
    qtest_add_func("/rx113-sci-txd/te-and-stop", test_te_and_stop);
    qtest_add_func("/rx113-sci-txd/formats", test_formats);
    qtest_add_func("/rx113-sci-txd/back-to-back", test_back_to_back);
    qtest_add_func("/rx113-sci-txd/migration", test_migration);
    qtest_add_func("/rx113-sci-txd/sync-tx", test_sync_tx);
    qtest_add_func("/rx113-sci-txd/sync-external-clock",
                   test_sync_external_clock);
    return g_test_run();
}
