/*
 * RX113 TMR compare-match pad output and RTCOUT pad tests
 * (R01UH0448EJ0120 sections 19, 22 and 24; Issue #50)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define SCKCR       0x80020
#define SCKCR3      0x80026
#define MSTPCRA     0x80010
#define PORT_BASE   0x8c000
#define TMR_BASE    0x88200
#define RTC_BASE    0x8c400
#define RCR1        (RTC_BASE + 0x22)
#define RCR2        (RTC_BASE + 0x24)

#define RCR1_RTCOS  0x08
#define RCR2_START  0x01
#define RCR2_RTCOE  0x08

#define MS          1000000LL
#define SEC         1000000000LL

/* TMR register offsets within a unit; channel adds 0 or 1. */
#define TCR         0x0
#define TCSR        0x2
#define TCORA       0x4
#define TCORB       0x6
#define TCNT        0x8
#define TCCR        0xa

static QTestState *tmr_init(void)
{
    QTestState *qts = qtest_init("-M rx113-r5f51138");

    qtest_writew(qts, 0x803fe, 0xa503); /* PRCR: unlock clock/MSTP regs */
    qtest_writel(qts, SCKCR, 0);
    qtest_writew(qts, SCKCR3, 0x100); /* 32 MHz HOCO, PCLKB /1 */
    /* Run both TMR units (MSTPA5/MSTPA4), as in rx113-clock-test. */
    qtest_writel(qts, MSTPCRA, 0xefffffdf & ~(1U << 4));
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

/*
 * Figure 22.3 pulse output on TMR0/P22: clear on A, A drives high,
 * B drives low. One tick is PCLKB/8 = 250 ns; period 100, width 40.
 */
static void test_pulse(void)
{
    QTestState *qts = tmr_init();
    unsigned base = TMR_BASE;

    select_pad(qts, 0x22, 5); /* P22 = TMO0 */
    /* Output not yet enabled: the pad is not driven. */
    assert_pad(qts, 0x22, false, false);
    qtest_writeb(qts, base + TCR, 0x08); /* clear on compare A */
    qtest_writeb(qts, base + TCORA, 100);
    qtest_writeb(qts, base + TCORB, 40);
    qtest_writeb(qts, base + TCSR, 0x06); /* OSA high, OSB low */
    /* 22.3.1: driven low until the first compare match. */
    assert_pad(qts, 0x22, true, false);
    qtest_writeb(qts, base + TCCR, 0x0b); /* internal clock /8 */

    qtest_clock_step(qts, 24999); /* 99 ticks */
    assert_pad(qts, 0x22, true, false);
    qtest_clock_step(qts, 1); /* tick 100: compare A */
    assert_pad(qts, 0x22, true, true);
    qtest_clock_step(qts, 9999); /* count 39 after the clear */
    assert_pad(qts, 0x22, true, true);
    qtest_clock_step(qts, 1); /* count 40: compare B */
    assert_pad(qts, 0x22, true, false);
    qtest_clock_step(qts, 15000); /* tick 200: compare A again */
    assert_pad(qts, 0x22, true, true);
    qtest_quit(qts);
}

/* Toggle output on TMR1/P26, and no-change selects keeping the level. */
static void test_toggle(void)
{
    QTestState *qts = tmr_init();
    unsigned base = TMR_BASE, ch = 1;

    select_pad(qts, 0x26, 5); /* P26 = TMO1 */
    qtest_writeb(qts, base + TCR + ch, 0x08);
    qtest_writeb(qts, base + TCORA + ch, 50);
    qtest_writeb(qts, base + TCORB + ch, 25);
    qtest_writeb(qts, base + TCSR + ch, 0x03); /* OSA toggle, OSB none */
    assert_pad(qts, 0x26, true, false);
    qtest_writeb(qts, base + TCCR + ch, 0x0b);

    qtest_clock_step(qts, 12500); /* 50 ticks: A toggles high */
    assert_pad(qts, 0x26, true, true);
    qtest_clock_step(qts, 12500); /* B (no change), then A toggles low */
    assert_pad(qts, 0x26, true, false);
    qtest_clock_step(qts, 12500);
    assert_pad(qts, 0x26, true, true);
    qtest_quit(qts);
}

/*
 * 16-bit count mode (22.5.1(2), Issue #51 F-003): TMO0 follows the
 * 16-bit compare match, TMO1 the lower 8-bit compare match.
 */
static void test_cascade_output(void)
{
    QTestState *qts = tmr_init();
    unsigned base = TMR_BASE;

    select_pad(qts, 0x22, 5); /* P22 = TMO0 */
    select_pad(qts, 0x26, 5); /* P26 = TMO1 */
    qtest_writeb(qts, base + TCR, 0x08); /* clear on compare A (16-bit) */
    qtest_writeb(qts, base + TCORA, 0x01); /* combined TCORA = 0x0180 */
    qtest_writeb(qts, base + TCORA + 1, 0x80);
    qtest_writeb(qts, base + TCORB, 0xff);
    qtest_writeb(qts, base + TCORB + 1, 0x40);
    qtest_writeb(qts, base + TCSR, 0x03); /* TMO0: toggle on A */
    qtest_writeb(qts, base + TCSR + 1, 0x03); /* TMO1: toggle on A */
    assert_pad(qts, 0x22, true, false);
    assert_pad(qts, 0x26, true, false);
    qtest_writeb(qts, base + TCCR + 1, 0x0b); /* TMR1: internal /8 */
    qtest_writeb(qts, base + TCCR, 0x1b); /* TMR0: cascading /8 */

    qtest_clock_step(qts, 32000); /* count 128: low-byte A match */
    assert_pad(qts, 0x22, true, false);
    assert_pad(qts, 0x26, true, true);
    qtest_clock_step(qts, 32000); /* count 256: no A match */
    assert_pad(qts, 0x22, true, false);
    assert_pad(qts, 0x26, true, true);
    qtest_clock_step(qts, 32000); /* count 384: both match, counter clears */
    assert_pad(qts, 0x22, true, true);
    assert_pad(qts, 0x26, true, false);
    qtest_clock_step(qts, 32000); /* count 128 again: low-byte A match */
    assert_pad(qts, 0x22, true, true);
    assert_pad(qts, 0x26, true, true);
    qtest_quit(qts);
}

/*
 * Independent channels of one unit running concurrently: channel 0
 * rescheduling must not disturb channel 1's pending events.
 */
static void test_independent_channels(void)
{
    QTestState *qts = tmr_init();
    unsigned base = TMR_BASE;

    select_pad(qts, 0x22, 5); /* P22 = TMO0 */
    select_pad(qts, 0x26, 5); /* P26 = TMO1 */
    qtest_writeb(qts, base + TCR, 0x08); /* ch0: clear on compare A */
    qtest_writeb(qts, base + TCR + 1, 0x08); /* ch1: clear on compare A */
    qtest_writeb(qts, base + TCORA, 100);
    qtest_writeb(qts, base + TCORA + 1, 30);
    qtest_writeb(qts, base + TCORB, 0xff);
    qtest_writeb(qts, base + TCORB + 1, 0xff);
    qtest_writeb(qts, base + TCSR, 0x03); /* toggle on A */
    qtest_writeb(qts, base + TCSR + 1, 0x03);
    qtest_writeb(qts, base + TCCR, 0x0b); /* internal /8 */
    qtest_writeb(qts, base + TCCR + 1, 0x0b);

    qtest_clock_step(qts, 7500); /* 30 ticks: ch1 match */
    assert_pad(qts, 0x22, true, false);
    assert_pad(qts, 0x26, true, true);
    qtest_clock_step(qts, 7500); /* 60 ticks: ch1 match */
    assert_pad(qts, 0x26, true, false);
    qtest_clock_step(qts, 10000); /* 100 ticks: ch0 match, ch1 at 90 */
    assert_pad(qts, 0x22, true, true);
    assert_pad(qts, 0x26, true, true);
    qtest_clock_step(qts, 5000); /* 120 ticks: ch1 match survives ch0 */
    assert_pad(qts, 0x22, true, true);
    assert_pad(qts, 0x26, true, false);
    qtest_quit(qts);
}

/* Both selects zero negates the output enable; module stop drops it too. */
static void test_hiz_and_stop(void)
{
    QTestState *qts = tmr_init();
    unsigned base = TMR_BASE;

    select_pad(qts, 0x22, 5);
    qtest_writeb(qts, base + TCR, 0x08);
    qtest_writeb(qts, base + TCORA, 10);
    qtest_writeb(qts, base + TCSR, 0x02); /* OSA high */
    qtest_writeb(qts, base + TCCR, 0x0b);
    qtest_clock_step(qts, 2500); /* 10 ticks: high */
    assert_pad(qts, 0x22, true, true);

    /* Module stop: a stopped peripheral must not drive the pad. */
    qtest_writel(qts, MSTPCRA, 0xefffffff);
    assert_pad(qts, 0x22, false, false);
    qtest_clock_step(qts, 100 * MS);
    assert_pad(qts, 0x22, false, false);
    /* Resume: the retained level drives again, counting continues. */
    qtest_writel(qts, MSTPCRA, 0xefffffdf & ~(1U << 4));
    assert_pad(qts, 0x22, true, true);

    /* Disabling both output selects returns the pad to Hi-Z. */
    qtest_writeb(qts, base + TCSR, 0x00);
    assert_pad(qts, 0x22, false, false);
    qtest_quit(qts);
}

/* The second unit/channel pair reaches its own pad (TMR3 on P55). */
static void test_second_unit(void)
{
    QTestState *qts = tmr_init();
    unsigned base = TMR_BASE + 0x10, ch = 1; /* TMR3 */

    select_pad(qts, 0x55, 5); /* P55 = TMO3 */
    qtest_writeb(qts, base + TCR + ch, 0x08);
    qtest_writeb(qts, base + TCORA + ch, 10);
    qtest_writeb(qts, base + TCSR + ch, 0x02); /* OSA high */
    assert_pad(qts, 0x55, true, false);
    qtest_writeb(qts, base + TCCR + ch, 0x0b);
    qtest_clock_step(qts, 2499);
    assert_pad(qts, 0x55, true, false);
    qtest_clock_step(qts, 1);
    assert_pad(qts, 0x55, true, true);
    qtest_quit(qts);
}

/* RTCOUT reaches P32 (PSEL=7) at 1 Hz and PB0 at 64 Hz. */
static void test_rtcout(void)
{
    QTestState *qts = tmr_init();

    select_pad(qts, 0x32, 7); /* P32 = RTCOUT */
    select_pad(qts, 0xb0, 7); /* PB0 = RTCOUT */
    /* RTCOE is still off: neither pad is driven. */
    assert_pad(qts, 0x32, false, false);
    assert_pad(qts, 0xb0, false, false);
    qtest_writeb(qts, RCR2, RCR2_RTCOE);
    assert_pad(qts, 0x32, true, false);
    qtest_writeb(qts, RCR2, RCR2_RTCOE | RCR2_START);
    /* 1 Hz: low for the first half second, high for the second. */
    qtest_clock_step(qts, SEC / 2 - 1);
    assert_pad(qts, 0x32, true, false);
    qtest_clock_step(qts, 1);
    assert_pad(qts, 0x32, true, true);
    qtest_clock_step(qts, SEC / 2);
    assert_pad(qts, 0x32, true, false);

    /* Switch to 64 Hz (RTCOS is written while stopped, per 24.2.17). */
    qtest_writeb(qts, RCR2, 0);
    assert_pad(qts, 0x32, false, false);
    qtest_writeb(qts, RCR1, RCR1_RTCOS);
    qtest_writeb(qts, RCR2, RCR2_RTCOE | RCR2_START);
    assert_pad(qts, 0xb0, true, false);
    qtest_clock_step(qts, SEC / 128 - 1);
    assert_pad(qts, 0xb0, true, false);
    qtest_clock_step(qts, 1);
    assert_pad(qts, 0xb0, true, true);
    qtest_clock_step(qts, SEC / 128);
    assert_pad(qts, 0xb0, true, false);
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

/* A driven-high TMO pad and its waveform survive same-build migration. */
static void test_migration(void)
{
    QTestState *src = tmr_init();
    QTestState *dst;
    int64_t now;
    g_autofree char *dir = g_dir_make_tmp("rx113-tmr-XXXXXX", NULL);
    g_autofree char *file = g_strdup_printf("%s/state", dir);
    g_autofree char *uri = g_strdup_printf("file:%s", file);

    g_assert_nonnull(dir);
    select_pad(src, 0x22, 5);
    qtest_writeb(src, TMR_BASE + TCR, 0x08);
    qtest_writeb(src, TMR_BASE + TCORA, 100);
    qtest_writeb(src, TMR_BASE + TCORB, 40);
    qtest_writeb(src, TMR_BASE + TCSR, 0x06);
    qtest_writeb(src, TMR_BASE + TCCR, 0x0b);
    now = qtest_clock_step(src, 25000); /* compare A: high */
    assert_pad(src, 0x22, true, true);

    qtest_qmp_assert_success(src, "{'execute':'migrate',"
                            "'arguments':{'uri':%s}}", uri);
    migration_wait(src);
    dst = qtest_init("-M rx113-r5f51138 -incoming defer");
    qtest_clock_set(dst, now);
    qtest_qmp_assert_success(dst, "{'execute':'migrate-incoming',"
                            "'arguments':{'uri':%s}}", uri);
    migration_wait(dst);
    /* PORT state migrated with the selection in place. */
    assert_pad(dst, 0x22, true, true);
    qtest_clock_step(dst, 10000); /* count 40: compare B drives low */
    assert_pad(dst, 0x22, true, false);
    qtest_quit(dst);
    qtest_quit(src);
    unlink(file);
    rmdir(dir);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/rx113-tmr/pulse", test_pulse);
    qtest_add_func("/rx113-tmr/toggle", test_toggle);
    qtest_add_func("/rx113-tmr/hiz-and-stop", test_hiz_and_stop);
    qtest_add_func("/rx113-tmr/second-unit", test_second_unit);
    qtest_add_func("/rx113-tmr/rtcout", test_rtcout);
    qtest_add_func("/rx113-tmr/migration", test_migration);
    qtest_add_func("/rx113-tmr/cascade-output", test_cascade_output);
    qtest_add_func("/rx113-tmr/independent-channels",
                   test_independent_channels);
    return g_test_run();
}
