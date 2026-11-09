/*
 * RX113 RTC register, counter and interrupt tests
 * (R01UH0448EJ0120 section 24, Table 14.3)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define RTC_BASE    0x8c400
#define R64CNT      (RTC_BASE + 0x00)
#define RSECCNT     (RTC_BASE + 0x02)
#define RMINCNT     (RTC_BASE + 0x04)
#define RHRCNT      (RTC_BASE + 0x06)
#define RWKCNT      (RTC_BASE + 0x08)
#define RDAYCNT     (RTC_BASE + 0x0a)
#define RMONCNT     (RTC_BASE + 0x0c)
#define RYRCNT      (RTC_BASE + 0x0e)
#define RSECAR      (RTC_BASE + 0x10)
#define RMINAR      (RTC_BASE + 0x12)
#define RHRAR       (RTC_BASE + 0x14)
#define RDAYAR      (RTC_BASE + 0x18)
#define RMONAR      (RTC_BASE + 0x1a)
#define RYRAR       (RTC_BASE + 0x1c)
#define RYRAREN     (RTC_BASE + 0x1e)
#define RCR1        (RTC_BASE + 0x22)
#define RCR2        (RTC_BASE + 0x24)

#define RCR1_AIE    0x01
#define RCR1_CIE    0x02
#define RCR1_PIE    0x04
#define RCR2_START  0x01
#define RCR2_RESET  0x02
#define RCR2_ADJ30  0x04
#define RCR2_HR24   0x40
#define RCR2_CNTMD  0x80

#define ICU_BASE    0x87000
#define ICU_IR(v)   (ICU_BASE + (v))
#define ICU_IER(v)  (ICU_BASE + 0x200 + (v) / 8)
#define VEC_CUP     63
#define VEC_ALM     92
#define VEC_PRD     93

#define LCDM1       0xa0801
#define LCDM1_BLON  0x10
#define LCDM1_LCDON 0x80

#define MS          1000000LL
#define SEC         1000000000LL

static QTestState *rtc_init(void)
{
    return qtest_init("-M rx113-r5f51138");
}

static void set_time(QTestState *qts, const uint8_t t[7], uint8_t rcr2)
{
    qtest_writeb(qts, RCR2, rcr2);
    qtest_writeb(qts, RYRCNT, t[0]);
    qtest_writeb(qts, RMONCNT, t[1]);
    qtest_writeb(qts, RDAYCNT, t[2]);
    qtest_writeb(qts, RWKCNT, t[3]);
    qtest_writeb(qts, RHRCNT, t[4]);
    qtest_writeb(qts, RMINCNT, t[5]);
    qtest_writeb(qts, RSECCNT, t[6]);
    qtest_writeb(qts, RCR2, rcr2 | RCR2_START);
}

static void check_time(QTestState *qts, const uint8_t t[7])
{
    g_assert_cmphex(qtest_readb(qts, RYRCNT), ==, t[0]);
    g_assert_cmphex(qtest_readb(qts, RMONCNT), ==, t[1]);
    g_assert_cmphex(qtest_readb(qts, RDAYCNT), ==, t[2]);
    g_assert_cmphex(qtest_readb(qts, RWKCNT), ==, t[3]);
    g_assert_cmphex(qtest_readb(qts, RHRCNT), ==, t[4]);
    g_assert_cmphex(qtest_readb(qts, RMINCNT), ==, t[5]);
    g_assert_cmphex(qtest_readb(qts, RSECCNT), ==, t[6]);
}

/* t[] = year, month, date, day of week, hour, minute, second (BCD) */
static void test_set_read_count(void)
{
    QTestState *qts = rtc_init();
    /* Wednesday 2024-02-28 23:59:58 */
    const uint8_t t0[7] = { 0x24, 0x02, 0x28, 3, 0x23, 0x59, 0x58 };
    const uint8_t t1[7] = { 0x24, 0x02, 0x28, 3, 0x23, 0x59, 0x59 };
    const uint8_t t2[7] = { 0x24, 0x02, 0x29, 4, 0x00, 0x00, 0x00 };
    const uint8_t t3[7] = { 0x24, 0x03, 0x01, 5, 0x00, 0x00, 0x00 };
    const uint8_t t4[7] = { 0x99, 0x12, 0x31, 6, 0x23, 0x59, 0x59 };
    const uint8_t t5[7] = { 0x00, 0x01, 0x01, 0, 0x00, 0x00, 0x00 };

    /* The registers read back what was written while stopped. */
    set_time(qts, t0, RCR2_HR24);
    qtest_writeb(qts, RCR2, RCR2_HR24);
    check_time(qts, t0);
    qtest_writeb(qts, RCR2, RCR2_HR24 | RCR2_START);

    qtest_clock_step(qts, 500 * MS);
    check_time(qts, t0);
    qtest_clock_step(qts, 500 * MS);
    check_time(qts, t1);
    qtest_clock_step(qts, 1 * SEC);
    check_time(qts, t2);
    qtest_clock_step(qts, 86400 * SEC);
    check_time(qts, t3);

    /* R64CNT counts 128 Hz steps (24.2.1) and is 0 on a second boundary. */
    g_assert_cmphex(qtest_readb(qts, R64CNT), ==, 0);
    qtest_clock_step(qts, 250 * MS);
    g_assert_cmphex(qtest_readb(qts, R64CNT), ==, 32);
    qtest_clock_step(qts, 750 * MS);

    /* Century wrap: 2099 rolls over to 2000 */
    set_time(qts, t4, RCR2_HR24);
    qtest_clock_step(qts, 1 * SEC);
    check_time(qts, t5);

    /* Stopping the clock freezes the counters. */
    qtest_writeb(qts, RCR2, RCR2_HR24);
    qtest_clock_step(qts, 10 * SEC);
    check_time(qts, t5);
    qtest_quit(qts);
}

static void test_12_hour(void)
{
    QTestState *qts = rtc_init();
    /* 11:59:59 a.m. -> 00:00:00 p.m. -> 00:00:00 a.m. of the next day */
    const uint8_t t0[7] = { 0x23, 0x12, 0x31, 0, 0x11, 0x59, 0x59 };
    const uint8_t t1[7] = { 0x23, 0x12, 0x31, 0, 0x40, 0x00, 0x00 };
    const uint8_t t2[7] = { 0x23, 0x12, 0x31, 0, 0x51, 0x59, 0x59 };
    const uint8_t t3[7] = { 0x24, 0x01, 0x01, 1, 0x00, 0x00, 0x00 };

    set_time(qts, t0, 0);
    qtest_clock_step(qts, 1 * SEC);
    check_time(qts, t1);
    set_time(qts, t2, 0);
    qtest_clock_step(qts, 1 * SEC);
    check_time(qts, t3);
    qtest_quit(qts);
}

static void test_adj30_and_reset(void)
{
    QTestState *qts = rtc_init();
    const uint8_t t0[7] = { 0x24, 0x05, 0x10, 5, 0x10, 0x20, 0x35 };
    const uint8_t t1[7] = { 0x24, 0x05, 0x10, 5, 0x10, 0x21, 0x00 };

    set_time(qts, t0, RCR2_HR24);
    qtest_writeb(qts, RCR2, RCR2_HR24 | RCR2_START | RCR2_ADJ30);
    g_assert_cmphex(qtest_readb(qts, RCR2), ==, RCR2_HR24 | RCR2_START);
    check_time(qts, t1);

    /* A software reset clears the alarm registers (24.2.9). */
    qtest_writeb(qts, RSECAR, 0x85);
    qtest_writeb(qts, RCR2, RCR2_HR24 | RCR2_START | RCR2_RESET);
    g_assert_cmphex(qtest_readb(qts, RCR2), ==, RCR2_HR24 | RCR2_START);
    g_assert_cmphex(qtest_readb(qts, RSECAR), ==, 0);
    qtest_quit(qts);
}

static void test_binary_count(void)
{
    QTestState *qts = rtc_init();

    qtest_writeb(qts, RCR2, RCR2_CNTMD);
    qtest_writeb(qts, RSECCNT, 0xfe);
    qtest_writeb(qts, RMINCNT, 0xff);
    qtest_writeb(qts, RHRCNT, 0x00);
    qtest_writeb(qts, RWKCNT, 0x80);
    qtest_writeb(qts, RCR2, RCR2_CNTMD | RCR2_START);
    qtest_clock_step(qts, 2 * SEC);
    g_assert_cmphex(qtest_readb(qts, RSECCNT), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, RMINCNT), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, RHRCNT), ==, 0x01);
    g_assert_cmphex(qtest_readb(qts, RWKCNT), ==, 0x80);
    qtest_quit(qts);
}

static void test_register_widths(void)
{
    QTestState *qts = rtc_init();

    /* RYRCNT and RYRAR are 16-bit; the upper byte is reserved (24.2.8). */
    qtest_writew(qts, RYRCNT, 0xff42);
    g_assert_cmphex(qtest_readw(qts, RYRCNT), ==, 0x0042);
    qtest_writeb(qts, RDAYCNT, 0xff);
    g_assert_cmphex(qtest_readb(qts, RDAYCNT), ==, 0x3f);
    qtest_writeb(qts, RMONCNT, 0xff);
    g_assert_cmphex(qtest_readb(qts, RMONCNT), ==, 0x1f);
    /* R64CNT is read only */
    qtest_writeb(qts, R64CNT, 0x55);
    g_assert_cmphex(qtest_readb(qts, R64CNT), ==, 0);
    qtest_quit(qts);
}

static void enable_irq(QTestState *qts, unsigned vec)
{
    qtest_writeb(qts, ICU_IER(vec),
                 qtest_readb(qts, ICU_IER(vec)) | (1 << (vec & 7)));
    qtest_writeb(qts, ICU_IR(vec), 0);
}

static void test_periodic(void)
{
    static const struct {
        uint8_t pes;
        int64_t ns;
    } cases[] = {
        { 0x6, SEC / 256 }, { 0x7, SEC / 128 }, { 0x8, SEC / 64 },
        { 0x9, SEC / 32 }, { 0xa, SEC / 16 }, { 0xb, SEC / 8 },
        { 0xc, SEC / 4 }, { 0xd, SEC / 2 }, { 0xe, SEC }, { 0xf, 2 * SEC },
    };
    QTestState *qts = rtc_init();

    enable_irq(qts, VEC_PRD);
    qtest_writeb(qts, RCR2, RCR2_START);
    for (unsigned i = 0; i < G_N_ELEMENTS(cases); i++) {
        /* A software reset restarts the prescaler: a boundary at "now". */
        qtest_writeb(qts, RCR1, (cases[i].pes << 4) | RCR1_PIE);
        qtest_writeb(qts, RCR2, RCR2_START | RCR2_RESET);
        qtest_writeb(qts, ICU_IR(VEC_PRD), 0);

        for (unsigned n = 0; n < 3; n++) {
            qtest_clock_step(qts, cases[i].ns - 1);
            g_assert_cmpuint(qtest_readb(qts, ICU_IR(VEC_PRD)), ==, 0);
            qtest_clock_step(qts, 1);
            g_assert_cmpuint(qtest_readb(qts, ICU_IR(VEC_PRD)), ==, 1);
            qtest_writeb(qts, ICU_IR(VEC_PRD), 0);
        }
    }

    /* No interrupt with PIE clear or with a reserved PES value */
    qtest_writeb(qts, RCR1, 0xe0);
    qtest_clock_step(qts, 5 * SEC);
    g_assert_cmpuint(qtest_readb(qts, ICU_IR(VEC_PRD)), ==, 0);
    qtest_writeb(qts, RCR1, (0x5 << 4) | RCR1_PIE);
    qtest_clock_step(qts, 5 * SEC);
    g_assert_cmpuint(qtest_readb(qts, ICU_IR(VEC_PRD)), ==, 0);
    qtest_quit(qts);
}

static void test_alarm_and_carry(void)
{
    QTestState *qts = rtc_init();
    const uint8_t t0[7] = { 0x24, 0x06, 0x15, 6, 0x08, 0x30, 0x58 };

    enable_irq(qts, VEC_ALM);
    enable_irq(qts, VEC_CUP);
    set_time(qts, t0, RCR2_HR24);

    /* 08:31:01 on any day: compares hour, minute and second */
    qtest_writeb(qts, RSECAR, 0x80 | 0x01);
    qtest_writeb(qts, RMINAR, 0x80 | 0x31);
    qtest_writeb(qts, RHRAR, 0x80 | 0x08);
    qtest_writeb(qts, RCR1, RCR1_AIE | RCR1_CIE);
    qtest_writeb(qts, ICU_IR(VEC_ALM), 0);
    qtest_writeb(qts, ICU_IR(VEC_CUP), 0);

    qtest_clock_step(qts, 2 * SEC);   /* 08:31:00 */
    g_assert_cmpuint(qtest_readb(qts, ICU_IR(VEC_ALM)), ==, 0);
    g_assert_cmpuint(qtest_readb(qts, ICU_IR(VEC_CUP)), ==, 1);
    qtest_clock_step(qts, 1 * SEC);   /* 08:31:01 */
    g_assert_cmpuint(qtest_readb(qts, ICU_IR(VEC_ALM)), ==, 1);

    /* The year alarm is only compared when RYRAREN.ENB is set. */
    qtest_writeb(qts, ICU_IR(VEC_ALM), 0);
    qtest_writeb(qts, RYRAR, 0x25);
    qtest_writeb(qts, RYRAREN, 0x80);
    qtest_writeb(qts, RSECAR, 0x80 | 0x01);
    qtest_clock_step(qts, 86400 * SEC);
    g_assert_cmpuint(qtest_readb(qts, ICU_IR(VEC_ALM)), ==, 0);
    qtest_quit(qts);
}

/* Issue 14: the RTC PRD toggles the LCDC blink phase (34.4.2). */
static void test_lcdc_blink(void)
{
    QTestState *qts = rtc_init();
    const char *lcdc = "/machine/mcu/lcdc";

    qtest_writeb(qts, LCDM1, LCDM1_LCDON | LCDM1_BLON);
    qtest_writeb(qts, RCR2, RCR2_START);
    g_assert_false(qtest_qom_get_bool(qts, lcdc, "blink-phase"));

    /* No PRD, no blinking */
    qtest_clock_step(qts, 2 * SEC);
    g_assert_false(qtest_qom_get_bool(qts, lcdc, "blink-phase"));

    qtest_writeb(qts, RCR1, (0xd << 4) | RCR1_PIE);  /* 1/2 s */
    qtest_clock_step(qts, SEC / 2 - 1);
    qtest_clock_step(qts, 1);
    g_assert_true(qtest_qom_get_bool(qts, lcdc, "blink-phase"));
    qtest_clock_step(qts, SEC / 2);
    g_assert_false(qtest_qom_get_bool(qts, lcdc, "blink-phase"));
    qtest_clock_step(qts, SEC / 2);
    g_assert_true(qtest_qom_get_bool(qts, lcdc, "blink-phase"));
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/rx113-rtc/set-read-count", test_set_read_count);
    qtest_add_func("/rx113-rtc/12-hour", test_12_hour);
    qtest_add_func("/rx113-rtc/adj30-reset", test_adj30_and_reset);
    qtest_add_func("/rx113-rtc/binary-count", test_binary_count);
    qtest_add_func("/rx113-rtc/register-widths", test_register_widths);
    qtest_add_func("/rx113-rtc/periodic", test_periodic);
    qtest_add_func("/rx113-rtc/alarm-carry", test_alarm_and_carry);
    qtest_add_func("/rx113-rtc/lcdc-blink", test_lcdc_blink);
    return g_test_run();
}
