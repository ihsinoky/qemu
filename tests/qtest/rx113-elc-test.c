/*
 * RX113 ELC end-to-end tests (R01UH0448EJ0120, section 17).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qemu/sockets.h"

#define ELC 0x8b100
#define ELSR(n) (ELC + (n) + 1)
#define ELSEGR (ELC + 0x2d)
#define MSTPCRA 0x80010
#define MSTPCRB 0x80014
#define IR(n) (0x87000 + (n))
#define CMT 0x88000
#define TMR 0x88200
#define MTU 0x88600
#define ADC 0x89000
#define ELC_PATH "/machine/mcu/elc"
#define RUN_A (0xefffffff & ~((1U << 17) | (1U << 15) | \
                            (1U << 9) | (1U << 5) | (1U << 4)))
#define RUN_B (0xffffffff & ~((1U << 9) | (1U << 26)))

static void elc_configure(QTestState *qts)
{
    qtest_writew(qts, 0x803fe, 0xa503);
    qtest_writeb(qts, 0x80036, 0);
    qtest_clock_step(qts, 34500);
    qtest_writew(qts, 0x80026, 0x100);
    qtest_writel(qts, 0x80020, 0); /* PCLKB = PCLKD = 32 MHz. */
    qtest_writel(qts, MSTPCRA, RUN_A);
    qtest_writel(qts, MSTPCRB, RUN_B);
    qtest_clock_step(qts, 1000);
}

static QTestState *elc_init(void)
{
    QTestState *qts = qtest_init("-M rx113-r5f51138");

    elc_configure(qts);
    return qts;
}

static void software(QTestState *qts)
{
    qtest_writeb(qts, ELSEGR, 0x7e); /* WE before SEG. */
    qtest_writeb(qts, ELSEGR, 0x7f);
    qtest_clock_step(qts, 32);
}

static void adc_prepare(QTestState *qts)
{
    qtest_writeb(qts, 0x8c11f, 0);
    qtest_writeb(qts, 0x8c11f, 0x40);
    qtest_writeb(qts, 0x8c160, 0x80);
    qtest_qmp_assert_success(qts, "{'execute':'qom-set','arguments':{"
                            "'path':'/machine/mcu/s12ad',"
                            "'property':'an000-uv','value':1650000}}");
    qtest_writew(qts, ADC + 4, 1);
    qtest_writew(qts, ADC + 0x10, 0x0900); /* ELC trigger A. */
    qtest_writew(qts, ADC, 0x0200); /* TRGE, ADIE=0. */
}

static void adc_completed(QTestState *qts)
{
    g_assert_cmphex(qtest_readw(qts, ADC) & 0x8000, ==, 0x8000);
    qtest_clock_step(qts, 2000);
    g_assert_cmphex(qtest_readw(qts, ADC) & 0x8000, ==, 0);
    g_assert_cmpuint(qtest_readw(qts, ADC + 0x20), ==, 2048);
    g_assert_cmpuint(qtest_readb(qts, IR(102)), ==, 0);
}

static void test_registers(void)
{
    QTestState *qts = qtest_init("-M rx113-r5f51138");
    static const unsigned elsr[] = {
        1, 2, 3, 4, 7, 10, 12, 14, 15, 17, 18, 19, 20, 22, 24, 25,
    };

    g_assert_cmpuint(qtest_readb(qts, ELC), ==, 0);
    qtest_writeb(qts, ELC, 0xff); /* Module stopped: ignored. */
    qtest_writel(qts, MSTPCRB, RUN_B); /* PRC1 locked: ignored. */
    g_assert_cmpuint(qtest_readb(qts, ELC), ==, 0);
    qtest_writew(qts, 0x803fe, 0xa502);
    qtest_writel(qts, MSTPCRB, RUN_B);
    g_assert_cmphex(qtest_readb(qts, ELC), ==, 0x7f);
    for (unsigned i = 0; i < ARRAY_SIZE(elsr); i++) {
        unsigned n = elsr[i];
        unsigned id = n == 19 ? 0x5d : 0x69;

        g_assert_cmpuint(qtest_readb(qts, ELSR(n)), ==, 0);
        qtest_writeb(qts, ELSR(n), id);
        g_assert_cmphex(qtest_readb(qts, ELSR(n)), ==, id);
        qtest_writeb(qts, ELSR(n), 0x07); /* Prohibited: rejected. */
        g_assert_cmphex(qtest_readb(qts, ELSR(n)), ==, id);
    }
    qtest_writeb(qts, ELSR(18), 0x61);
    g_assert_cmphex(qtest_readb(qts, ELSR(18)), ==, 0x69);
    qtest_writeb(qts, ELSR(19), 0x69);
    g_assert_cmphex(qtest_readb(qts, ELSR(19)), ==, 0x5d);
    qtest_writeb(qts, ELSR(24), 0x6a);
    g_assert_cmphex(qtest_readb(qts, ELSR(24)), ==, 0x69);
    for (unsigned a = 0x1f; a <= 0x22; a++) {
        g_assert_cmphex(qtest_readb(qts, ELC + a), ==, 0xff);
    }
    g_assert_cmphex(qtest_readb(qts, ELSEGR), ==, 0xbe);
    g_assert_cmphex(qtest_readb(qts, ELC + 0x25), ==, 0x88);
    g_assert_cmphex(qtest_readb(qts, ELC + 0x29), ==, 0x80);
    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    g_assert_cmpuint(qtest_readb(qts, ELC), ==, 0);
    qtest_writew(qts, 0x803fe, 0xa502);
    qtest_writel(qts, MSTPCRB, RUN_B);
    g_assert_cmphex(qtest_readb(qts, ELC), ==, 0x7f);
    g_assert_cmpuint(qtest_readb(qts, ELSR(15)), ==, 0);
    qtest_quit(qts);
}

static void test_software_adc(void)
{
    QTestState *qts = elc_init();

    adc_prepare(qts);
    qtest_writeb(qts, ELSR(15), 0x69);
    software(qts); /* ELCON=0. */
    g_assert_cmpuint(qtest_readw(qts, ADC) & 0x8000, ==, 0);
    qtest_writeb(qts, ELC, 0xff);
    qtest_writeb(qts, ELSEGR, 0xbe); /* WI=1 cannot change WE. */
    g_assert_cmphex(qtest_readb(qts, ELSEGR), ==, 0xfe);
    qtest_writeb(qts, ELSEGR, 0x3e); /* Disable WE. */
    qtest_writeb(qts, ELSEGR, 0x3f); /* SEG ignored. */
    qtest_clock_step(qts, 100);
    g_assert_cmpuint(qtest_readw(qts, ADC) & 0x8000, ==, 0);
    software(qts);
    adc_completed(qts);
    qtest_writeb(qts, ELSR(15), 0);
    software(qts);
    g_assert_cmpuint(qtest_readw(qts, ADC) & 0x8000, ==, 0);
    qtest_writeb(qts, ELSR(15), 0x69);
    qtest_writel(qts, MSTPCRB, RUN_B | (1 << 9));
    qtest_writeb(qts, ELSR(15), 0); /* Stop retains selection. */
    software(qts);
    qtest_writel(qts, MSTPCRB, RUN_B);
    g_assert_cmphex(qtest_readb(qts, ELSR(15)), ==, 0x69);
    g_assert_cmpuint(qtest_readw(qts, ADC) & 0x8000, ==, 0);
    software(qts);
    adc_completed(qts);
    qtest_quit(qts);
}

static unsigned fire_timer(QTestState *qts, unsigned id)
{
    unsigned irq;

    if (id == 0x1f) {
        qtest_writew(qts, CMT + 10, 0);
        qtest_writew(qts, CMT + 8, 0); /* CMIE=0, /8. */
        qtest_writew(qts, CMT + 12, 100);
        qtest_writew(qts, CMT, 2);
        qtest_clock_step(qts, 25032);
        qtest_writew(qts, CMT, 0);
        irq = 29;
    } else if (id == 0x22 || id == 0x28) {
        unsigned base = TMR + (id == 0x28 ? 16 : 0);

        qtest_writeb(qts, base + 8, 0);
        qtest_writeb(qts, base + 4, 100);
        qtest_writeb(qts, base + 10, 9); /* /1, CMIEA=0. */
        qtest_clock_step(qts, 3157);
        qtest_writeb(qts, base + 10, 0);
        irq = id == 0x28 ? 180 : 174;
    } else {
        static const unsigned tcr[] = { 0x180, 0x200, 0, 1 };
        static const unsigned cnt[] = { 0x186, 0x206, 0x10, 0x12 };
        static const unsigned tgr[] = { 0x188, 0x208, 0x18, 0x1c };
        static const unsigned bit[] = { 1, 2, 6, 7 };
        unsigned ch = id == 8 ? 0 : id == 12 ? 1 : id == 16 ? 2 : 3;

        qtest_writew(qts, MTU + cnt[ch], 0);
        qtest_writeb(qts, MTU + tcr[ch], 1); /* /4, TIER=0. */
        qtest_writew(qts, MTU + tgr[ch], 9);
        qtest_writeb(qts, MTU + 0x80, 1 << bit[ch]);
        qtest_clock_step(qts, 1282);
        qtest_writeb(qts, MTU + 0x80, 0);
        irq = ch == 0 ? 121 : ch == 1 ? 125 : ch == 2 ? 129 : 134;
    }
    return irq;
}

static void test_timer_adc(const void *opaque)
{
    unsigned id = GPOINTER_TO_UINT(opaque);
    QTestState *qts = elc_init();
    unsigned irq;

    adc_prepare(qts);
    qtest_writeb(qts, ELSR(15), id);
    qtest_writeb(qts, ELC, 0xff);
    irq = fire_timer(qts, id);
    g_assert_cmpuint(qtest_readb(qts, IR(irq)), ==, 0);
    adc_completed(qts);
    qtest_writeb(qts, ELSR(15), 0);
    qtest_qmp_assert_success(qts, "{'execute':'qom-set','arguments':{"
                            "'path':'/machine/mcu/s12ad',"
                            "'property':'an000-uv','value':825000}}");
    fire_timer(qts, id);
    qtest_clock_step(qts, 2000);
    g_assert_cmpuint(qtest_readw(qts, ADC) & 0x8000, ==, 0);
    g_assert_cmpuint(qtest_readw(qts, ADC + 0x20), ==, 2048);
    qtest_quit(qts);
}

static void test_timer_operations(void)
{
    QTestState *qts = elc_init();
    unsigned count;

    qtest_writeb(qts, ELC, 0xff);
    qtest_writeb(qts, ELSR(1), 0x69);
    qtest_writeb(qts, ELC + 0x1f, 0xf3); /* MTU1 start. */
    software(qts);
    g_assert_cmphex(qtest_readb(qts, MTU + 0x80), ==, 2);
    qtest_clock_step(qts, 1000);
    g_assert_cmpuint(qtest_readw(qts, MTU + 0x186), >, 0);
    qtest_writeb(qts, MTU + 0x80, 0);
    qtest_writeb(qts, ELC + 0x1f, 0xf7); /* Restart does not start. */
    software(qts);
    g_assert_cmpuint(qtest_readw(qts, MTU + 0x186), ==, 0);
    g_assert_cmpuint(qtest_readb(qts, MTU + 0x80), ==, 0);
    qtest_writew(qts, MTU + 0x186, 123);
    qtest_writeb(qts, MTU + 0x182, 8); /* TGRA input capture. */
    qtest_writeb(qts, MTU + 0x180, 4); /* External clock, stable count. */
    qtest_writeb(qts, MTU + 0x80, 2);
    qtest_writeb(qts, ELC + 0x1f, 0xfb);
    software(qts);
    g_assert_cmpuint(qtest_readw(qts, MTU + 0x188), ==, 123);
    g_assert_cmpuint(qtest_readb(qts, IR(121)), ==, 0);

    qtest_writeb(qts, ELSR(7), 0x69);
    qtest_writeb(qts, ELC + 0x21, 0xfb); /* CMT1 event count. */
    qtest_writew(qts, CMT, 2);
    qtest_writew(qts, CMT + 12, 10);
    software(qts);
    software(qts);
    qtest_clock_step(qts, 1000000);
    g_assert_cmpuint(qtest_readw(qts, CMT + 10), ==, 2);
    qtest_writeb(qts, ELC + 0x21, 0xf7);
    software(qts);
    g_assert_cmpuint(qtest_readw(qts, CMT + 10), ==, 0);
    qtest_writew(qts, CMT, 0);
    qtest_writeb(qts, ELC + 0x21, 0xf3);
    software(qts);
    g_assert_cmpuint(qtest_readw(qts, CMT) & 2, ==, 2);
    qtest_writew(qts, CMT, 0);

    qtest_writeb(qts, ELSR(10), 0x69);
    qtest_writeb(qts, ELC + 0x22, 0xfe); /* TMR0 event count. */
    software(qts);
    software(qts);
    qtest_clock_step(qts, 1000000);
    g_assert_cmpuint(qtest_readb(qts, TMR + 8), ==, 2);
    qtest_writeb(qts, ELC + 0x22, 0xfd); /* Restart. */
    software(qts);
    g_assert_cmpuint(qtest_readb(qts, TMR + 8), ==, 0);
    qtest_writeb(qts, ELC + 0x22, 0xfc); /* Count start gates clock. */
    qtest_writeb(qts, TMR + 10, 9);
    qtest_clock_step(qts, 1000);
    g_assert_cmpuint(qtest_readb(qts, TMR + 8), ==, 0);
    software(qts);
    g_assert_cmpuint(qtest_readb(qts, TMR + 12), ==, 1);
    qtest_clock_step(qts, 1000);
    g_assert_cmpuint(qtest_readb(qts, TMR + 8), >, 0);
    qtest_writeb(qts, TMR + 12, 0);
    count = qtest_readb(qts, TMR + 8);
    qtest_clock_step(qts, 1000);
    g_assert_cmpuint(qtest_readb(qts, TMR + 8), ==, count);
    qtest_quit(qts);
}

static void test_dtc_adc(void)
{
    QTestState *qts = elc_init();

    adc_prepare(qts);
    qtest_writeb(qts, ELSR(15), 0x61);
    qtest_writeb(qts, ELC, 0xff);
    qtest_writel(qts, 0x82404, 0x400);
    qtest_writel(qts, 0x400 + 27 * 4, 0x1000);
    qtest_writel(qts, 0x1000, 0); /* Normal byte transfer. */
    qtest_writel(qts, 0x1004, 0x2000);
    qtest_writel(qts, 0x1008, 0x3000);
    qtest_writel(qts, 0x100c, 0x10000);
    qtest_writeb(qts, 0x2000, 0x73);
    qtest_writeb(qts, 0x8711b, 1);
    qtest_writeb(qts, 0x87203, 8);
    qtest_writeb(qts, 0x8240c, 1);
    qtest_writeb(qts, 0x872e0, 1);
    qtest_clock_step(qts, 33);
    g_assert_cmphex(qtest_readb(qts, 0x3000), ==, 0x73);
    adc_completed(qts);
    qtest_quit(qts);
}

static void test_sci_adc(const void *opaque)
{
    unsigned id = GPOINTER_TO_UINT(opaque);
    QTestState *qts = elc_init();

    adc_prepare(qts);
    qtest_writeb(qts, ELSR(15), id);
    qtest_writeb(qts, ELC, 0xff);
    qtest_writeb(qts, 0x8a0a1, 1); /* 10 us/character. */
    qtest_writeb(qts, 0x8a0a2, 0x20); /* TE, TIE=TEIE=0. */
    if (id == 0x3d) {
        qtest_writeb(qts, 0x8a0a3, 0x55);
        qtest_clock_step(qts, 10032);
    } else {
        qtest_clock_step(qts, 32);
    }
    g_assert_cmpuint(qtest_readb(qts, IR(224)), ==, 0);
    g_assert_cmpuint(qtest_readb(qts, IR(225)), ==, 0);
    adc_completed(qts);
    qtest_quit(qts);
}

static void test_icu_dtc(void)
{
    QTestState *qts = elc_init();

    qtest_writeb(qts, ELSR(18), 0x69);
    qtest_writeb(qts, ELC, 0xff);
    software(qts);
    g_assert_cmpuint(qtest_readb(qts, IR(106)), ==, 1);
    qtest_writeb(qts, IR(106), 0);
    qtest_writel(qts, 0x82404, 0x400);
    qtest_writel(qts, 0x400 + 106 * 4, 0x1000);
    qtest_writel(qts, 0x1000, 0);
    qtest_writel(qts, 0x1004, 0x2000);
    qtest_writel(qts, 0x1008, 0x3000);
    qtest_writel(qts, 0x100c, 0x10000);
    qtest_writeb(qts, 0x2000, 0x5a);
    qtest_writeb(qts, 0x87100 + 106, 1);
    qtest_writeb(qts, 0x87200 + 106 / 8, 1 << (106 % 8));
    qtest_writeb(qts, 0x8240c, 1);
    software(qts);
    qtest_clock_step(qts, 1);
    g_assert_cmphex(qtest_readb(qts, 0x3000), ==, 0x5a);
    qtest_quit(qts);
}

static void test_sci_receive_events(void)
{
    g_autofree char *dir = g_dir_make_tmp("rx113-elc-sci-XXXXXX", NULL);
    g_autofree char *path = g_strdup_printf("%s/sci", dir);
    g_autofree char *args = g_strdup_printf(
        "-M rx113-r5f51138 -serial null -serial null -serial null "
        "-chardev socket,id=s5,path=%s,server=on,wait=off -serial chardev:s5",
        path);
    QTestState *qts = qtest_init(args);
    int fd = unix_connect(path, NULL);
    char byte = 'a';

    g_assert_cmpint(fd, >=, 0);
    elc_configure(qts);
    qtest_writeb(qts, ELC, 0xff);
    qtest_writeb(qts, ELSR(12), 0x3b);
    qtest_writeb(qts, ELC + 0x22, 0xef); /* TMR2 event counter. */
    qtest_writeb(qts, 0x8a0a1, 1);
    qtest_writeb(qts, 0x8a0a2, 0x10); /* RE, RIE=0. */
    g_assert_cmpint(send(fd, &byte, 1, 0), ==, 1);
    for (unsigned i = 0; i < 1000; i++) {
        if (qtest_readb(qts, 0x8a0a4) & 0x40) {
            break;
        }
        g_usleep(1000);
    }
    qtest_clock_step(qts, 32);
    g_assert_cmpuint(qtest_readb(qts, TMR + 0x18), ==, 1);
    g_assert_cmpuint(qtest_readb(qts, IR(223)), ==, 0);
    /* Leave RDR full, then a second byte produces ERI without RIE. */
    qtest_writeb(qts, ELSR(12), 0x3a);
    qtest_clock_step(qts, 10000);
    g_assert_cmpint(send(fd, &byte, 1, 0), ==, 1);
    for (unsigned i = 0; i < 1000; i++) {
        if (qtest_readb(qts, 0x8a0a4) & 0x20) {
            break;
        }
        g_usleep(1000);
    }
    qtest_clock_step(qts, 32);
    g_assert_cmpuint(qtest_readb(qts, TMR + 0x18), ==, 2);
    g_assert_cmpuint(qtest_readb(qts, IR(222)), ==, 0);
    close(fd);
    qtest_quit(qts);
    rmdir(dir);
}

static void test_mtu_event(const void *opaque)
{
    unsigned id = GPOINTER_TO_UINT(opaque);
    QTestState *qts = elc_init();
    static const unsigned bases[] = { 8, 12, 16, 21 };
    static const unsigned tcr[] = { 0x180, 0x200, 0, 1 };
    static const unsigned mode[] = { 0x181, 0x201, 2, 3 };
    static const unsigned cnt[] = { 0x186, 0x206, 0x10, 0x12 };
    static const unsigned gr[][4] = {
        { 0x188, 0x18a }, { 0x208, 0x20a },
        { 0x18, 0x1a, 0x24, 0x26 }, { 0x1c, 0x1e, 0x28, 0x2a },
    };
    static const unsigned bit[] = { 1, 2, 6, 7 };
    unsigned ch = id < 12 ? 0 : id < 16 ? 1 : id < 21 ? 2 : 3;
    unsigned event = id - bases[ch];
    bool compare = event < (ch < 2 ? 2 : 4);
    bool down = ch < 2 && event == 3;
    unsigned pin = down ? ch * 2 : 0;

    qtest_writeb(qts, ELSR(12), id);
    qtest_writeb(qts, ELC + 0x22, 0xef); /* TMR2 counts events. */
    qtest_writeb(qts, ELC, 0xff);
    qtest_writeb(qts, MTU + tcr[ch], ch < 2 ? 4 : 6);
    qtest_writew(qts, MTU + cnt[ch], compare || down ? 0 : 0xffff);
    if (compare) {
        qtest_writew(qts, MTU + gr[ch][event], 0);
    }
    if (down) {
        qtest_writeb(qts, MTU + mode[ch], 5);
        qtest_set_irq_in(qts, "/machine/mcu/mtu", "mtclk", pin + 1, 1);
    }
    qtest_writeb(qts, MTU + 0x80, 1 << bit[ch]);
    qtest_set_irq_in(qts, "/machine/mcu/mtu", "mtclk", pin, 1);
    qtest_clock_step(qts, 32);
    g_assert_cmpuint(qtest_readb(qts, TMR + 0x18), ==, 1);
    for (unsigned irq = 121; irq < 139; irq++) {
        g_assert_cmpuint(qtest_readb(qts, IR(irq)), ==, 0);
    }
    qtest_quit(qts);
}

static void test_mtu_zero_period(void)
{
    QTestState *qts = elc_init();

    qtest_writeb(qts, ELSR(12), 8);
    qtest_writeb(qts, ELC + 0x22, 0xef);
    qtest_writeb(qts, ELC, 0xff);
    qtest_writew(qts, MTU + 0x188, 0);
    qtest_writeb(qts, MTU + 0x180, 0x20);
    qtest_writeb(qts, MTU + 0x80, 2);
    qtest_clock_step(qts, 126);
    g_assert_cmpuint(qtest_readb(qts, TMR + 0x18), ==, 4);
    g_assert_cmpuint(qtest_readb(qts, IR(121)), ==, 0);
    qtest_writeb(qts, ELSR(12), 0);
    qtest_clock_step(qts, 1000000000);
    g_assert_cmpuint(qtest_readb(qts, TMR + 0x18), ==, 4);
    qtest_quit(qts);
}

static void test_tmr_event(const void *opaque)
{
    unsigned id = GPOINTER_TO_UINT(opaque);
    unsigned event = id < 0x28 ? id - 0x22 : id - 0x28;
    unsigned base = TMR + (id < 0x28 ? 0 : 16);
    unsigned irq = (id < 0x28 ? 174 : 180) + event;
    QTestState *qts = elc_init();

    qtest_writeb(qts, ELSR(7), id);
    qtest_writeb(qts, ELC + 0x21, 0xfb);
    qtest_writew(qts, CMT, 2);
    qtest_writeb(qts, ELC, 0xff);
    if (event < 2) {
        qtest_writeb(qts, base + 4 + event * 2, 1);
    } else {
        qtest_writeb(qts, base + 8, 255);
    }
    qtest_writeb(qts, base + 10, 9);
    qtest_clock_step(qts, 64);
    g_assert_cmpuint(qtest_readw(qts, CMT + 10), ==, 1);
    g_assert_cmpuint(qtest_readb(qts, IR(irq)), ==, 0);
    qtest_quit(qts);
}

static void test_adc_scan_end(void)
{
    QTestState *qts = elc_init();

    adc_prepare(qts);
    qtest_writeb(qts, ELSR(7), 0x58);
    qtest_writeb(qts, ELC + 0x21, 0xfb);
    qtest_writew(qts, CMT, 2);
    qtest_writeb(qts, ELC, 0xff);
    qtest_writew(qts, ADC, 0x8400); /* CPU starts fast scan, ADIE=0. */
    qtest_clock_step(qts, 1657);
    g_assert_cmpuint(qtest_readw(qts, CMT + 10), ==, 1);
    g_assert_cmpuint(qtest_readb(qts, IR(102)), ==, 0);
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
    QTestState *src = elc_init();
    QTestState *dst;
    g_autofree char *dir = g_dir_make_tmp("rx113-elc-XXXXXX", NULL);
    g_autofree char *file = g_strdup_printf("%s/state", dir);
    g_autofree char *uri = g_strdup_printf("file:%s", file);
    int64_t now;

    qtest_writeb(src, ELC, 0xff);
    qtest_writeb(src, ELSR(7), 0x69);
    qtest_writeb(src, ELSR(10), 0x69);
    qtest_writeb(src, ELC + 0x21, 0xfb);
    qtest_writeb(src, ELC + 0x22, 0xfe);
    qtest_writew(src, CMT, 2);
    software(src);
    g_assert_cmpuint(qtest_readw(src, CMT + 10), ==, 1);
    g_assert_cmpuint(qtest_readb(src, TMR + 8), ==, 1);
    qtest_writeb(src, ELSEGR, 0x7f); /* A pending link, not yet delivered. */
    now = qtest_clock_step(src, 10);
    if (stopped) {
        qtest_writel(src, MSTPCRB, RUN_B | (1 << 9));
    }
    qtest_qmp_assert_success(src, "{'execute':'migrate',"
                            "'arguments':{'uri':%s}}", uri);
    migration_wait(src);
    dst = qtest_init("-M rx113-r5f51138 -incoming defer");
    qtest_clock_set(dst, now);
    qtest_qmp_assert_success(dst, "{'execute':'migrate-incoming',"
                            "'arguments':{'uri':%s}}", uri);
    migration_wait(dst);
    qtest_clock_step(dst, 22);
    if (stopped) {
        g_assert_cmpuint(qtest_readb(dst, ELC), ==, 0);
        qtest_writel(dst, MSTPCRB, RUN_B);
    }
    g_assert_cmphex(qtest_readb(dst, ELSR(7)), ==, 0x69);
    g_assert_cmpuint(qtest_readw(dst, CMT + 10), ==, stopped ? 1 : 2);
    g_assert_cmpuint(qtest_readb(dst, TMR + 8), ==, stopped ? 1 : 2);
    software(dst);
    g_assert_cmpuint(qtest_readw(dst, CMT + 10), ==, stopped ? 2 : 3);
    g_assert_cmpuint(qtest_readb(dst, TMR + 8), ==, stopped ? 2 : 3);
    qtest_quit(dst);
    qtest_quit(src);
    unlink(file);
    rmdir(dir);
}

static void test_chained_event_counts(void)
{
    QTestState *qts = elc_init();

    adc_prepare(qts);
    qtest_writeb(qts, ELC, 0xff);
    qtest_writeb(qts, ELSR(10), 0x69);
    qtest_writeb(qts, ELSR(7), 0x22);
    qtest_writeb(qts, ELSR(15), 0x1f);
    qtest_writeb(qts, ELC + 0x22, 0xfe);
    qtest_writeb(qts, ELC + 0x21, 0xfb);
    qtest_writeb(qts, TMR + 4, 2);
    qtest_writeb(qts, TMR, 8); /* Clear at compare A, CMIEA=0. */
    qtest_writew(qts, CMT + 12, 2);
    qtest_writew(qts, CMT, 2);
    for (unsigned i = 0; i < 3; i++) {
        software(qts);
        qtest_clock_step(qts, 64);
        g_assert_cmpuint(qtest_readw(qts, ADC) & 0x8000, ==, 0);
    }
    software(qts);
    qtest_clock_step(qts, 64);
    g_assert_cmpuint(qtest_readb(qts, IR(174)), ==, 0);
    g_assert_cmpuint(qtest_readb(qts, IR(29)), ==, 0);
    adc_completed(qts);
    qtest_writeb(qts, ELSR(7), 0);
    for (unsigned i = 0; i < 4; i++) {
        software(qts);
        qtest_clock_step(qts, 64);
    }
    g_assert_cmpuint(qtest_readw(qts, ADC) & 0x8000, ==, 0);
    qtest_quit(qts);
}

#define PGR1 (ELC + 0x23)
#define PGC1 (ELC + 0x25)
#define PDBF1 (ELC + 0x27)
#define PEL0 (ELC + 0x29)
#define PEL1 (ELC + 0x2a)
#define PDRB 0x8c00b
#define PODRB 0x8c02b
#define PORT_PATH "/machine/mcu/port"

static void port_input(QTestState *qts, unsigned bit, bool level)
{
    qtest_set_irq_in(qts, PORT_PATH, "pin", 11 * 8 + bit, level);
}

static void port_counter(QTestState *qts, unsigned source)
{
    qtest_writeb(qts, ELSR(10), source);
    qtest_writeb(qts, ELC + 0x22, 0xfe); /* TMR0 event counter. */
    qtest_writeb(qts, ELC, 0xff);
}

static void test_port_edges(void)
{
    for (unsigned channel = 0; channel < 3; channel++) {
        for (unsigned mode = 0; mode < 4; mode++) {
            QTestState *qts = elc_init();

            if (channel < 2) {
                qtest_writeb(qts, PEL0 + channel, 0x88 | (mode << 5));
            } else {
                qtest_writeb(qts, PGR1, 3);
                qtest_writeb(qts, PGC1, 0x88 | mode);
            }
            port_counter(qts, channel < 2 ? 0x65 + channel : 0x63);
            port_input(qts, 0, true);
            qtest_clock_step(qts, 32);
            g_assert_cmpuint(qtest_readb(qts, TMR + 8), ==, mode == 1 ? 0 : 1);
            port_input(qts, 0, false);
            qtest_clock_step(qts, 32);
            g_assert_cmpuint(qtest_readb(qts, TMR + 8), ==, mode >= 2 ? 2 : 1);
            qtest_quit(qts);
        }
    }
}

static void test_port_single(void)
{
    QTestState *qts = elc_init();

    qtest_writeb(qts, PDRB, 1);
    qtest_writeb(qts, PEL0, 0x88);
    qtest_writeb(qts, PEL1, 0xa8);
    qtest_writeb(qts, ELSR(24), 0x69);
    qtest_writeb(qts, ELSR(25), 0x69);
    qtest_writeb(qts, ELC, 0xff);
    qtest_writeb(qts, ELSEGR, 0x7e);
    qtest_writeb(qts, ELSEGR, 0x7f);
    qtest_clock_step(qts, 31);
    g_assert_cmphex(qtest_readb(qts, PODRB), ==, 0);
    qtest_clock_step(qts, 1); /* D-014: low, then high on the same pin. */
    g_assert_cmphex(qtest_readb(qts, PODRB), ==, 1);
    qtest_writeb(qts, PEL1, 0xc8);
    software(qts); /* Low then toggle. */
    g_assert_cmphex(qtest_readb(qts, PODRB), ==, 1);
    qtest_writeb(qts, PEL0, 0xc8);
    qtest_writeb(qts, PEL1, 0x88);
    software(qts); /* Toggle then low. */
    g_assert_cmphex(qtest_readb(qts, PODRB), ==, 0);
    qtest_writeb(qts, PEL1, 0xc8);
    software(qts); /* Two toggles compose to identity. */
    g_assert_cmphex(qtest_readb(qts, PODRB), ==, 0);
    qtest_writeb(qts, ELSR(25), 0);
    software(qts);
    g_assert_cmphex(qtest_readb(qts, PODRB), ==, 1);
    qtest_writeb(qts, PEL0, 0x90); /* Prohibited PSP is rejected. */
    g_assert_cmphex(qtest_readb(qts, PEL0), ==, 0xc8);
    qtest_quit(qts);
}

static void test_port_group(void)
{
    QTestState *qts = elc_init();
    static const uint8_t rotated[] = { 0x80, 0x08, 0x01, 0x80 };

    qtest_writeb(qts, PDRB, 0x99);
    qtest_writeb(qts, PODRB, 0x10);
    qtest_writeb(qts, PGR1, 0x89);
    qtest_writeb(qts, PDBF1, 0x80);
    qtest_writeb(qts, PGC1, 0xc8);
    qtest_writeb(qts, ELSR(20), 0x69);
    qtest_writeb(qts, PEL0, 0xa8); /* Group wins over single high. */
    qtest_writeb(qts, ELSR(24), 0x69);
    qtest_writeb(qts, ELC, 0xff);
    for (unsigned i = 0; i < ARRAY_SIZE(rotated); i++) {
        software(qts);
        g_assert_cmphex(qtest_readb(qts, PODRB), ==, (rotated[i] | 0x10));
    }
    qtest_writeb(qts, PDBF1, 8);
    qtest_writeb(qts, ELSR(20), 0x69); /* Reload rotation sequence. */
    software(qts);
    g_assert_cmphex(qtest_readb(qts, PODRB), ==, 0x18);
    for (unsigned mode = 0; mode < 4; mode++) {
        qtest_writeb(qts, PGC1, 0x88 | (mode << 4));
        software(qts);
        g_assert_cmphex(qtest_readb(qts, PODRB), ==,
                        0x10 | (mode == 0 || mode == 2 ? 0 :
                                mode == 1 ? 0x89 : 8));
    }
    qtest_quit(qts);
}

static void test_port_capture(void)
{
    QTestState *qts = elc_init();

    qtest_writeb(qts, PDBF1, 0x80);
    qtest_writeb(qts, PGR1, 3);
    qtest_writeb(qts, ELSR(22), 0x69);
    qtest_writeb(qts, ELC, 0xff);
    port_input(qts, 0, true);
    software(qts);
    port_input(qts, 0, false);
    port_input(qts, 1, true);
    software(qts); /* Unread capture blocks overwrite. */
    g_assert_cmphex(qtest_readb(qts, PDBF1), ==, 0x81);
    software(qts); /* CPU read released protection. */
    g_assert_cmphex(qtest_readb(qts, PDBF1), ==, 0x82);
    software(qts);
    qtest_writeb(qts, PGC1, 0x8c);
    port_input(qts, 0, true);
    software(qts); /* PGCOVE allows overwrite of unread data. */
    g_assert_cmphex(qtest_readb(qts, PDBF1), ==, 0x83);
    qtest_quit(qts);
}

static void test_port_write_protection(void)
{
    QTestState *qts = elc_init();

    qtest_writeb(qts, PGR1, 3);
    qtest_writeb(qts, PDRB, 1);
    qtest_writeb(qts, PEL0, 0x8a); /* Single PB2. */
    qtest_writeb(qts, PDRB, 5);
    qtest_writeb(qts, PODRB, 0xff); /* ELCON=0: unrestricted. */
    qtest_writeb(qts, ELC, 0xff);
    qtest_writeb(qts, PODRB, 0);
    g_assert_cmphex(qtest_readb(qts, PODRB), ==, 1);
    qtest_writeb(qts, ELSR(24), 0x69);
    qtest_writeb(qts, ELC, 0x7f);
    qtest_writeb(qts, PODRB, 0xff);
    qtest_writeb(qts, ELC, 0xff);
    qtest_writeb(qts, PODRB, 0);
    g_assert_cmphex(qtest_readb(qts, PODRB), ==, 5);
    qtest_writeb(qts, PDBF1, 0xff); /* No capture link: writable. */
    g_assert_cmphex(qtest_readb(qts, PDBF1), ==, 0xff);
    qtest_writeb(qts, ELSR(22), 0x69);
    qtest_writeb(qts, PDBF1, 0);
    g_assert_cmphex(qtest_readb(qts, PDBF1), ==, 2);
    qtest_writeb(qts, ELSR(22), 0x6a); /* DOC exception. */
    qtest_writeb(qts, PDBF1, 0);
    g_assert_cmphex(qtest_readb(qts, PDBF1), ==, 0);
    qtest_quit(qts);
}

static void test_port_overlap(void)
{
    QTestState *qts = elc_init();

    qtest_writeb(qts, PGR1, 3);
    qtest_writeb(qts, PEL0, 0x88);
    qtest_writeb(qts, PEL1, 0x88);
    port_counter(qts, 0x63);
    qtest_writeb(qts, ELSR(7), 0x65);
    qtest_writeb(qts, ELC + 0x21, 0xfb);
    qtest_writew(qts, CMT, 2);
    adc_prepare(qts);
    qtest_writeb(qts, ELSR(15), 0x66);
    port_input(qts, 0, true);
    qtest_clock_step(qts, 32);
    g_assert_cmpuint(qtest_readb(qts, TMR + 8), ==, 1);
    g_assert_cmpuint(qtest_readw(qts, CMT + 10), ==, 1);
    adc_completed(qts);
    qtest_quit(qts);
}

static void test_port_package(void)
{
    QTestState *qts = qtest_init(
        "-M rx113-r5f51138 -global rx113-port.package-pins=64");

    elc_configure(qts);
    qtest_writeb(qts, PDRB, 0xc0);
    qtest_writeb(qts, PEL0, 0xae);
    qtest_writeb(qts, ELSR(24), 0x69);
    qtest_writeb(qts, ELC, 0xff);
    software(qts);
    g_assert_cmphex(qtest_readb(qts, PODRB), ==, 0x40);
    qtest_writeb(qts, 0x8c121, 0xc0); /* Shared pads now select PC0/1. */
    qtest_writeb(qts, PEL0, 0x8e);
    software(qts);
    g_assert_cmphex(qtest_readb(qts, PODRB), ==, 0);
    qtest_writeb(qts, 0x8c121, 0);
    g_assert_cmphex(qtest_readb(qts, PODRB), ==, 0x40);
    qtest_quit(qts);
}

static void port_dtc_copy(QTestState *qts, unsigned src, unsigned dst)
{
    qtest_writeb(qts, IR(27), 0);
    qtest_writel(qts, 0x82404, 0x400);
    qtest_writel(qts, 0x400 + 27 * 4, 0x1000);
    qtest_writel(qts, 0x1000, 0); /* Normal byte transfer. */
    qtest_writel(qts, 0x1004, src);
    qtest_writel(qts, 0x1008, dst);
    qtest_writel(qts, 0x100c, 0x10000);
    qtest_writeb(qts, 0x8711b, 1);
    qtest_writeb(qts, 0x87203, 8);
    qtest_writeb(qts, 0x8240c, 1);
    qtest_set_irq_in(qts, "/machine/mcu/dtc", NULL, 27, 1);
    qtest_set_irq_in(qts, "/machine/mcu/dtc", NULL, 27, 0);
    qtest_clock_step(qts, 1);
}

static void test_port_dtc(void)
{
    QTestState *qts = elc_init();

    qtest_writeb(qts, PGR1, 3);
    qtest_writeb(qts, PDRB, 1);
    qtest_writeb(qts, PODRB, 1);
    qtest_writeb(qts, ELSR(22), 0x69);
    qtest_writeb(qts, ELC, 0xff);
    port_input(qts, 1, true);
    software(qts);
    qtest_writeb(qts, 0x2000, 0);
    port_dtc_copy(qts, 0x2000, PODRB);
    g_assert_cmphex(qtest_readb(qts, PODRB), ==, 1);
    port_dtc_copy(qts, 0x2000, PDBF1);
    port_input(qts, 1, false);
    software(qts);
    port_dtc_copy(qts, PDBF1, 0x3000); /* Releases unread protection. */
    g_assert_cmphex(qtest_readb(qts, 0x3000), ==, 2);
    software(qts);
    g_assert_cmphex(qtest_readb(qts, PDBF1), ==, 0);
    qtest_quit(qts);
}

static void test_port_peripheral(void)
{
    QTestState *qts = elc_init();

    qtest_writeb(qts, PDRB, 1);
    qtest_writeb(qts, PEL0, 0xa8);
    qtest_writeb(qts, ELSR(24), 0x1f); /* CMT1 compare, IRQ disabled. */
    qtest_writeb(qts, ELC, 0xff);
    fire_timer(qts, 0x1f);
    g_assert_cmphex(qtest_readb(qts, PODRB), ==, 1);
    qtest_quit(qts);
}

static void test_port_migration(const void *opaque)
{
    bool stopped = GPOINTER_TO_UINT(opaque);
    QTestState *src = elc_init();
    QTestState *dst;
    g_autofree char *dir = g_dir_make_tmp("rx113-elc-port-XXXXXX", NULL);
    g_autofree char *file = g_strdup_printf("%s/state", dir);
    g_autofree char *uri = g_strdup_printf("file:%s", file);
    int64_t now;

    qtest_writeb(src, PDRB, 0x89);
    qtest_writeb(src, PGR1, 0x8f);
    qtest_writeb(src, PDBF1, 0x80);
    qtest_writeb(src, PGC1, 0xc8);
    qtest_writeb(src, ELSR(20), 0x69);
    qtest_writeb(src, ELSR(22), 0x69);
    qtest_writeb(src, ELC, 0xff);
    port_input(src, 1, true);
    software(src); /* First transfer, and unread capture of PB1. */
    g_assert_cmphex(qtest_readb(src, PODRB), ==, 0x80);
    port_input(src, 1, false);
    port_input(src, 2, true);
    port_counter(src, 0x63);
    qtest_writeb(src, ELSEGR, 0x7f); /* Queue next rotation/capture. */
    now = qtest_clock_step(src, 10);
    if (stopped) {
        qtest_writel(src, MSTPCRB, RUN_B | (1 << 9));
        qtest_writeb(src, PDBF1, 0); /* Stopped MMIO has no effect. */
        g_assert_cmphex(qtest_readb(src, PDBF1), ==, 0);
    }
    qtest_qmp_assert_success(src, "{'execute':'migrate',"
                            "'arguments':{'uri':%s}}", uri);
    migration_wait(src);
    dst = qtest_init("-M rx113-r5f51138 -incoming defer");
    qtest_clock_set(dst, now);
    qtest_qmp_assert_success(dst, "{'execute':'migrate-incoming',"
                            "'arguments':{'uri':%s}}", uri);
    migration_wait(dst);
    qtest_clock_step(dst, 22);
    if (stopped) {
        qtest_writel(dst, MSTPCRB, RUN_B);
    }
    g_assert_cmphex(qtest_readb(dst, PODRB), ==, stopped ? 0x80 : 8);
    qtest_clock_step(dst, 32);
    g_assert_cmpuint(qtest_readb(dst, TMR + 8), ==, 0);
    software(dst); /* Retained rotation phase and unread protection. */
    g_assert_cmphex(qtest_readb(dst, PODRB), ==, stopped ? 8 : 1);
    g_assert_cmphex(qtest_readb(dst, PDBF1), ==, 0x82);
    software(dst); /* Reading PDBF released capture protection. */
    g_assert_cmphex(qtest_readb(dst, PDBF1), ==, 0x84);
    qtest_qmp_assert_success(dst, "{'execute':'system_reset'}");
    elc_configure(dst);
    g_assert_cmphex(qtest_readb(dst, PDBF1), ==, 0);
    g_assert_cmphex(qtest_readb(dst, PGR1), ==, 0);
    qtest_writeb(dst, PDRB, 0x89);
    qtest_writeb(dst, PGR1, 0x89);
    qtest_writeb(dst, PDBF1, 0x80);
    qtest_writeb(dst, PGC1, 0xc8);
    qtest_writeb(dst, ELSR(20), 0x69);
    qtest_writeb(dst, ELC, 0xff);
    software(dst);
    g_assert_cmphex(qtest_readb(dst, PODRB), ==, 0x80);
    qtest_quit(dst);
    qtest_quit(src);
    unlink(file);
    rmdir(dir);
}

int main(int argc, char **argv)
{
    static const unsigned timers[] = { 8, 12, 16, 21, 0x1f, 0x22, 0x28 };

    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/rx113-elc/port-edges", test_port_edges);
    qtest_add_func("/rx113-elc/port-peripheral", test_port_peripheral);
    qtest_add_func("/rx113-elc/port-dtc", test_port_dtc);
    qtest_add_data_func("/rx113-elc/port-migration", GUINT_TO_POINTER(0),
                        test_port_migration);
    qtest_add_data_func("/rx113-elc/port-migration-stopped",
                        GUINT_TO_POINTER(1),
                        test_port_migration);
    qtest_add_func("/rx113-elc/port-single", test_port_single);
    qtest_add_func("/rx113-elc/port-group", test_port_group);
    qtest_add_func("/rx113-elc/port-capture", test_port_capture);
    qtest_add_func("/rx113-elc/port-write-protection",
                   test_port_write_protection);
    qtest_add_func("/rx113-elc/port-overlap", test_port_overlap);
    qtest_add_func("/rx113-elc/port-package", test_port_package);
    for (unsigned id = 8; id < 0x1a; id++) {
        g_autofree char *name = g_strdup_printf(
            "/rx113-elc/mtu-event/%02x", id);

        qtest_add_data_func(name, GUINT_TO_POINTER(id), test_mtu_event);
    }
    qtest_add_func("/rx113-elc/mtu-zero-period", test_mtu_zero_period);
    for (unsigned i = 0; i < 6; i++) {
        unsigned id = i < 3 ? 0x22 + i : 0x28 + i - 3;
        g_autofree char *name = g_strdup_printf(
            "/rx113-elc/tmr-event/%02x", id);

        qtest_add_data_func(name, GUINT_TO_POINTER(id), test_tmr_event);
    }
    qtest_add_func("/rx113-elc/adc-scan-end", test_adc_scan_end);
    qtest_add_func("/rx113-elc/registers", test_registers);
    qtest_add_func("/rx113-elc/software-adc", test_software_adc);
    qtest_add_func("/rx113-elc/timer-operations", test_timer_operations);
    qtest_add_func("/rx113-elc/dtc-adc", test_dtc_adc);
    qtest_add_func("/rx113-elc/icu-dtc", test_icu_dtc);
    qtest_add_func("/rx113-elc/sci-receive-events", test_sci_receive_events);
    qtest_add_func("/rx113-elc/chained-event-counts",
                   test_chained_event_counts);
    qtest_add_data_func("/rx113-elc/migration-pending", GUINT_TO_POINTER(0),
                        test_migration);
    qtest_add_data_func("/rx113-elc/migration-stopped", GUINT_TO_POINTER(1),
                        test_migration);
    for (unsigned i = 0; i < ARRAY_SIZE(timers); i++) {
        g_autofree char *name = g_strdup_printf("/rx113-elc/timer-adc/%02x",
                                               timers[i]);

        qtest_add_data_func(name, GUINT_TO_POINTER(timers[i]), test_timer_adc);
    }
    qtest_add_data_func("/rx113-elc/sci-txi-adc", GUINT_TO_POINTER(0x3c),
                        test_sci_adc);
    qtest_add_data_func("/rx113-elc/sci-tei-adc", GUINT_TO_POINTER(0x3d),
                        test_sci_adc);
    return g_test_run();
}
