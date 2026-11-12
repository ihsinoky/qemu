/*
 * RX113 non-maskable interrupt delivery: manual R01UH0448EJ0120
 * sections 13.5.4 and 14.2.10-14.2.15 (Issue #58, D-017 follow-up).
 *
 * The register tests drive the ICU source inputs directly.  The
 * delivery tests boot a minimal program from a -bios image: it runs
 * with PSW.I at its reset value (0), so an NMI reaching the handler
 * also proves the NMI is non-maskable.  The program is hand-written
 * RX assembly (source kept in the PMO instruction file); only the
 * assembled bytes are embedded here.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bswap.h"

#define ICU 0x87000
#define NMISR (ICU + 0x580)
#define NMIER (ICU + 0x581)
#define NMICLR (ICU + 0x582)
#define NMICR (ICU + 0x583)
#define NMIFLTE (ICU + 0x590)
#define NMIFLTC (ICU + 0x594)
#define IRQCR0 (ICU + 0x500)
#define IRQFLTE0 (ICU + 0x510)
#define IRQFLTC0 (ICU + 0x514)
#define IR64 (ICU + 64)

#define RAM_BOOT 0x1000
#define RAM_MARK 0x1004
#define RAM_SNAP 0x1008
#define RAM_HITS 0x100c
#define RAM_COUNT 0x1010

/* Assembled from the assembly source in the Issue #58 instruction. */
static const uint8_t nmi_text[] = {
    0xfb, 0x5e, 0x00, 0x00, 0x01, 0xfd, 0x68, 0x5a, 0xfd, 0x68, 0x52, 0xfb,
    0x42, 0x11, 0x11, 0x11, 0x11, 0xfb, 0x6a, 0x00, 0x10, 0xe3, 0x64, 0xfb,
    0x6e, 0xfe, 0x03, 0x08, 0xfb, 0x4e, 0x03, 0xa5, 0x00, 0xd3, 0x64, 0xfb,
    0x6e, 0x20, 0x00, 0x08, 0x66, 0x04, 0xe3, 0x64, 0xfb, 0x6e, 0x26, 0x00,
    0x08, 0xfb, 0x4a, 0x00, 0x01, 0xd3, 0x64, 0xfb, 0x6e, 0x00, 0x01, 0xf8,
    0xec, 0x67, 0x66, 0x08, 0x7c, 0x07, 0x13, 0x78, 0x18, 0x7c, 0x17, 0x13,
    0x78, 0x38, 0xfb, 0x6e, 0x81, 0x75, 0x08, 0xc3, 0x68, 0xfb, 0x6e, 0x40,
    0x00, 0x08, 0x75, 0x44, 0x81, 0xc3, 0x64, 0xfb, 0x9a, 0x10, 0x10, 0xec,
    0x94, 0x62, 0x14, 0xe3, 0x94, 0x2e, 0xfa, 0x6e, 0x47, 0xfb, 0x6e, 0x80,
    0x75, 0x08, 0x58, 0x64, 0xfb, 0x6a, 0x08, 0x10, 0xe3, 0x64, 0xfb, 0x42,
    0x31, 0x49, 0x4d, 0x4e, 0xfb, 0x6a, 0x04, 0x10, 0xe3, 0x64, 0xfb, 0x6a,
    0x0c, 0x10, 0xec, 0x64, 0x62, 0x14, 0xe3, 0x64, 0xfb, 0x6e, 0x80, 0x75,
    0x08, 0x58, 0x64, 0xfb, 0x6e, 0x82, 0x75, 0x08, 0xc3, 0x64, 0x6f, 0x47,
    0x7f, 0x95, 0x04, 0x00, 0x00, 0x00,
};

static char *flash_dir;
static char *flash_path;

/* cfg: bit0 = guest enables OSTEN, bit1 = guest enables IWDTEN. */
static QTestState *nmi_init(uint32_t cfg, uint32_t ofs0)
{
    g_autofree uint8_t *flash = g_malloc0(512 * 1024);
    g_autofree char *args = NULL;
    QTestState *qts;

    memcpy(flash, nmi_text, sizeof(nmi_text));
    stl_le_p(flash + 0x100, cfg);
    for (unsigned i = 0; i < 30; i++) {
        stl_le_p(flash + 512 * 1024 - 0x80 + i * 4, 0xfff8009e);
    }
    stl_le_p(flash + 512 * 1024 - 0x80 + 30 * 4, 0xfff80067);
    stl_le_p(flash + 512 * 1024 - 0x80 + 31 * 4, 0xfff80000);
    stl_le_p(flash + 512 * 1024 - 0x74, ofs0);
    g_assert_true(g_file_set_contents(flash_path, (char *)flash,
                                      512 * 1024, NULL));
    /*
     * The qtest accelerator never executes guest code (dummy vCPU
     * threads), so the delivery tests need TCG; synchronisation is
     * by polling, not by clock steps.
     */
    args = g_strdup_printf("-M rx113-r5f51138 -bios %s -accel tcg",
                           flash_path);
    qts = qtest_init(args);
    return qts;
}

/* Poll a RAM cell until it holds the expected value (wall clock). */
static void wait_ram(QTestState *qts, uint32_t addr, uint32_t want)
{
    for (unsigned i = 0; i < 3000; i++) {
        if (qtest_readl(qts, addr) == want) {
            return;
        }
        g_usleep(1000);
    }
    g_assert_cmphex(qtest_readl(qts, addr), ==, want);
}

static void wait_nmisr(QTestState *qts, uint8_t want)
{
    for (unsigned i = 0; i < 3000; i++) {
        if (qtest_readb(qts, NMISR) == want) {
            return;
        }
        g_usleep(1000);
    }
    g_assert_cmphex(qtest_readb(qts, NMISR), ==, want);
}

static void boot_and_check(QTestState *qts)
{
    uint32_t first, second;

    wait_ram(qts, RAM_BOOT, 0x11111111);
    /* The setup sequence still has to reach the main loop. */
    for (unsigned i = 0; i < 3000 && qtest_readl(qts, RAM_COUNT) == 0;
         i++) {
        g_usleep(1000);
    }
    first = qtest_readl(qts, RAM_COUNT);
    g_assert_cmpuint(first, >, 0);
    g_usleep(10000);
    second = qtest_readl(qts, RAM_COUNT);
    g_assert_cmpuint(second, >, first);
}

static void test_registers(void)
{
    QTestState *qts = qtest_init("-M rx113-r5f51138");

    g_assert_cmphex(qtest_readb(qts, NMISR), ==, 0);
    g_assert_cmphex(qtest_readb(qts, NMIER), ==, 0);
    g_assert_cmphex(qtest_readb(qts, NMICLR), ==, 0);
    g_assert_cmphex(qtest_readb(qts, NMICR), ==, 0);
    g_assert_cmphex(qtest_readb(qts, NMIFLTE), ==, 0);
    g_assert_cmphex(qtest_readb(qts, NMIFLTC), ==, 0);
    /* NMISR is read-only; the write is ignored. */
    qtest_writeb(qts, NMISR, 0x3b);
    g_assert_cmphex(qtest_readb(qts, NMISR), ==, 0);
    /* NMIFLTE/NMIFLTC are storage-only in this model. */
    qtest_writeb(qts, NMIFLTE, 0xff);
    g_assert_cmphex(qtest_readb(qts, NMIFLTE), ==, 0x01);
    qtest_writeb(qts, NMIFLTC, 0xff);
    g_assert_cmphex(qtest_readb(qts, NMIFLTC), ==, 0x03);
    qtest_quit(qts);
}

static void test_nmier_set_once(void)
{
    QTestState *qts = qtest_init("-M rx113-r5f51138");

    qtest_writeb(qts, NMIER, 0x02);
    g_assert_cmphex(qtest_readb(qts, NMIER), ==, 0x02);
    qtest_writeb(qts, NMIER, 0x00); /* writing 0 never clears */
    g_assert_cmphex(qtest_readb(qts, NMIER), ==, 0x02);
    qtest_writeb(qts, NMIER, 0xff); /* only the defined bits exist */
    g_assert_cmphex(qtest_readb(qts, NMIER), ==, 0x3b);
    qtest_quit(qts);
}

static void test_nmicr_gate(void)
{
    QTestState *qts = qtest_init("-M rx113-r5f51138");

    qtest_writeb(qts, NMICR, 0x08); /* NMIMD = rising */
    g_assert_cmphex(qtest_readb(qts, NMICR), ==, 0x08);
    qtest_writeb(qts, NMIER, 0x01); /* NMIEN locks NMICR */
    qtest_writeb(qts, NMICR, 0x00);
    g_assert_cmphex(qtest_readb(qts, NMICR), ==, 0x08);
    qtest_quit(qts);
}

static void test_source_flags(void)
{
    QTestState *qts = qtest_init("-M rx113-r5f51138");

    /* OSTDI: the flag sets even with OSTEN still 0 (14.2.10 note). */
    qtest_set_irq_in(qts, "/machine/mcu/icu", "nmi-ostdi", 0, 1);
    g_assert_cmphex(qtest_readb(qts, NMISR), ==, 0x02);
    qtest_set_irq_in(qts, "/machine/mcu/icu", "nmi-ostdi", 0, 0);
    g_assert_cmphex(qtest_readb(qts, NMISR), ==, 0x02); /* latched */
    /* A clear write for a different flag leaves OSTST alone. */
    qtest_writeb(qts, NMICLR, 0x01);
    g_assert_cmphex(qtest_readb(qts, NMISR), ==, 0x02);
    qtest_writeb(qts, NMICLR, 0x02);
    g_assert_cmphex(qtest_readb(qts, NMISR), ==, 0);
    /* No re-set while the source line simply stays low. */
    g_assert_cmphex(qtest_readb(qts, NMISR), ==, 0);

    /* WUNI. */
    qtest_set_irq_in(qts, "/machine/mcu/icu", "nmi-iwdt", 0, 1);
    g_assert_cmphex(qtest_readb(qts, NMISR), ==, 0x08);
    qtest_set_irq_in(qts, "/machine/mcu/icu", "nmi-iwdt", 0, 0);
    qtest_writeb(qts, NMICLR, 0x08);
    g_assert_cmphex(qtest_readb(qts, NMISR), ==, 0);
    qtest_quit(qts);
}

static void test_pin_edges(void)
{
    QTestState *qts = qtest_init("-M rx113-r5f51138");

    /* NMIMD = 0 (falling) after reset. */
    qtest_set_irq_in(qts, "/machine/mcu/icu", "nmi-pin", 0, 1);
    g_assert_cmphex(qtest_readb(qts, NMISR), ==, 0);
    qtest_set_irq_in(qts, "/machine/mcu/icu", "nmi-pin", 0, 0);
    g_assert_cmphex(qtest_readb(qts, NMISR), ==, 0x01);
    qtest_writeb(qts, NMICLR, 0x01);
    g_assert_cmphex(qtest_readb(qts, NMISR), ==, 0);
    /* Rising mode. */
    qtest_writeb(qts, NMICR, 0x08);
    qtest_set_irq_in(qts, "/machine/mcu/icu", "nmi-pin", 0, 1);
    g_assert_cmphex(qtest_readb(qts, NMISR), ==, 0x01);
    qtest_quit(qts);
}

static void test_pin_via_port(void)
{
    QTestState *qts = qtest_init("-M rx113-r5f51138");

    /* P35 is PORT pin index 3*8+5; the NMI input follows the pad. */
    qtest_set_irq_in(qts, "/machine/mcu/port", "pin", 29, 1);
    g_assert_cmphex(qtest_readb(qts, NMISR), ==, 0);
    qtest_set_irq_in(qts, "/machine/mcu/port", "pin", 29, 0);
    g_assert_cmphex(qtest_readb(qts, NMISR), ==, 0x01);
    qtest_quit(qts);
}

static void test_delivery_ost(void)
{
    QTestState *qts;
    uint32_t before, after;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG not available");
        return;
    }
    qts = nmi_init(1, 0xffffffff);
    boot_and_check(qts);
    qtest_set_irq_in(qts, "/machine/mcu/system", "main-clock-failed",
                     0, 1);
    wait_ram(qts, RAM_MARK, 0x4e4d4931);
    g_assert_cmphex(qtest_readl(qts, RAM_SNAP), ==, 0x02);
    g_assert_cmphex(qtest_readl(qts, RAM_HITS), ==, 1);
    /* The handler cleared OSTST itself via NMICLR. */
    g_assert_cmphex(qtest_readb(qts, NMISR), ==, 0);
    /* RTE returned to the main loop. */
    before = qtest_readl(qts, RAM_COUNT);
    g_usleep(10000);
    after = qtest_readl(qts, RAM_COUNT);
    g_assert_cmpuint(after, >, before);
    qtest_quit(qts);
}

static void test_delivery_ost_gated(void)
{
    QTestState *qts;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG not available");
        return;
    }
    qts = nmi_init(0, 0xffffffff);
    boot_and_check(qts);
    qtest_set_irq_in(qts, "/machine/mcu/system", "main-clock-failed",
                     0, 1);
    wait_nmisr(qts, 0x02);
    /* Flag latched, but with OSTEN = 0 nothing is delivered. */
    g_usleep(50000);
    g_assert_cmphex(qtest_readl(qts, RAM_MARK), ==, 0);
    /* Enabling afterwards delivers the latched request. */
    qtest_writeb(qts, NMIER, 0x02);
    wait_ram(qts, RAM_MARK, 0x4e4d4931);
    g_assert_cmphex(qtest_readl(qts, RAM_SNAP), ==, 0x02);
    g_assert_cmphex(qtest_readl(qts, RAM_HITS), ==, 1);
    qtest_quit(qts);
}

static void test_delivery_iwdt(void)
{
    /* OFS0: automatic start, NMI output, 128 cycles at ILOCO/1. */
    QTestState *qts;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG not available");
        return;
    }
    qts = nmi_init(2, 0xffffef01);
    /*
     * No "not yet" check: the underflow period (~8.53 ms of virtual
     * time, which tracks wall time under TCG) is shorter than the
     * boot check itself.  The watchdog keeps underflowing, so the
     * hit count is only required to be nonzero; the snapshot proves
     * the source.
     */
    boot_and_check(qts);
    wait_ram(qts, RAM_MARK, 0x4e4d4931);
    g_assert_cmphex(qtest_readl(qts, RAM_SNAP), ==, 0x08);
    g_assert_cmpuint(qtest_readl(qts, RAM_HITS), >=, 1);
    qtest_quit(qts);
}

/* ICLK = HOCO 32 MHz / 8 = 4 MHz: one filter cycle is 250 ns. */
static void filter_clock_init(QTestState *qts)
{
    qtest_writew(qts, 0x803fe, 0xa503); /* PRCR: unlock clock regs */
    qtest_writew(qts, 0x80026, 0x100); /* SCKCR3: HOCO selected */
    qtest_writel(qts, 0x80020, 0x00030000); /* SCKCR: ICK = /8 */
}

static void test_filter_registers(void)
{
    QTestState *qts = qtest_init("-M rx113-r5f51138");

    g_assert_cmphex(qtest_readb(qts, IRQFLTE0), ==, 0);
    g_assert_cmphex(qtest_readw(qts, IRQFLTC0), ==, 0);
    qtest_writeb(qts, IRQFLTE0, 0xa5);
    g_assert_cmphex(qtest_readb(qts, IRQFLTE0), ==, 0xa5);
    qtest_writew(qts, IRQFLTC0, 0x1234);
    g_assert_cmphex(qtest_readw(qts, IRQFLTC0), ==, 0x1234);
    g_assert_cmphex(qtest_readb(qts, IRQFLTC0), ==, 0x34);
    g_assert_cmphex(qtest_readb(qts, IRQFLTC0 + 1), ==, 0x12);
    qtest_writeb(qts, IRQFLTC0 + 1, 0xab);
    g_assert_cmphex(qtest_readw(qts, IRQFLTC0), ==, 0xab34);
    /* NMIFLTC sits at 0x594 per the manual, not at 0x591. */
    qtest_writeb(qts, NMIFLTC, 2);
    g_assert_cmphex(qtest_readb(qts, NMIFLTC), ==, 2);
    g_assert_cmphex(qtest_readb(qts, ICU + 0x591), ==, 0xff);
    qtest_quit(qts);
}

static void test_filter_nmi(void)
{
    QTestState *qts = qtest_init("-M rx113-r5f51138");

    filter_clock_init(qts);
    qtest_set_irq_in(qts, "/machine/mcu/icu", "nmi-pin", 0, 1);
    qtest_writeb(qts, NMIER, 0x01); /* NMIEN */
    qtest_writeb(qts, NMIFLTC, 0); /* sampling clock = ICLK */
    qtest_writeb(qts, NMIFLTE, 1); /* filter on */
    /* A 400 ns low pulse is sampled once: removed. */
    qtest_set_irq_in(qts, "/machine/mcu/icu", "nmi-pin", 0, 0);
    qtest_clock_step(qts, 400);
    qtest_set_irq_in(qts, "/machine/mcu/icu", "nmi-pin", 0, 1);
    qtest_clock_step(qts, 1000);
    g_assert_cmphex(qtest_readb(qts, NMISR) & 1, ==, 0);
    /* A low level held past three samples reaches the detector. */
    qtest_set_irq_in(qts, "/machine/mcu/icu", "nmi-pin", 0, 0);
    qtest_clock_step(qts, 800);
    g_assert_cmphex(qtest_readb(qts, NMISR) & 1, ==, 1);
    qtest_set_irq_in(qts, "/machine/mcu/icu", "nmi-pin", 0, 1);
    qtest_writeb(qts, NMICLR, 0x01);
    g_assert_cmphex(qtest_readb(qts, NMISR), ==, 0);
    qtest_quit(qts);
}

static void test_filter_nmi_divider(void)
{
    QTestState *qts = qtest_init("-M rx113-r5f51138");

    filter_clock_init(qts);
    qtest_set_irq_in(qts, "/machine/mcu/icu", "nmi-pin", 0, 1);
    qtest_writeb(qts, NMIER, 0x01);
    qtest_writeb(qts, NMIFLTC, 3); /* sampling clock = ICLK/64 = 16 us */
    qtest_writeb(qts, NMIFLTE, 1);
    /* 10 us is less than one sampling cycle now: removed. */
    qtest_set_irq_in(qts, "/machine/mcu/icu", "nmi-pin", 0, 0);
    qtest_clock_step(qts, 10000);
    qtest_set_irq_in(qts, "/machine/mcu/icu", "nmi-pin", 0, 1);
    qtest_clock_step(qts, 1000);
    g_assert_cmphex(qtest_readb(qts, NMISR) & 1, ==, 0);
    /* 60 us covers three samples (48 us): the edge lands. */
    qtest_set_irq_in(qts, "/machine/mcu/icu", "nmi-pin", 0, 0);
    qtest_clock_step(qts, 60000);
    g_assert_cmphex(qtest_readb(qts, NMISR) & 1, ==, 1);
    qtest_quit(qts);
}

static void test_filter_irq(void)
{
    QTestState *qts = qtest_init("-M rx113-r5f51138");

    filter_clock_init(qts);
    qtest_writeb(qts, IRQCR0, 0x04); /* IRQ0: falling edge */
    qtest_set_irq_in(qts, "/machine/mcu/icu", NULL, 64, 1);
    qtest_writew(qts, IRQFLTC0, 0); /* sampling clock = ICLK */
    qtest_writeb(qts, IRQFLTE0, 0x01); /* filter on for IRQ0 */
    /* A 400 ns low pulse is removed. */
    qtest_set_irq_in(qts, "/machine/mcu/icu", NULL, 64, 0);
    qtest_clock_step(qts, 400);
    qtest_set_irq_in(qts, "/machine/mcu/icu", NULL, 64, 1);
    qtest_clock_step(qts, 1000);
    g_assert_cmphex(qtest_readb(qts, IR64), ==, 0);
    /* A held low level lands the falling edge after three samples. */
    qtest_set_irq_in(qts, "/machine/mcu/icu", NULL, 64, 0);
    qtest_clock_step(qts, 800);
    g_assert_cmphex(qtest_readb(qts, IR64), ==, 1);
    qtest_set_irq_in(qts, "/machine/mcu/icu", NULL, 64, 1);
    qtest_clock_step(qts, 800);
    qtest_writeb(qts, IR64, 0);
    g_assert_cmphex(qtest_readb(qts, IR64), ==, 0);
    /* Level mode: IR follows the filtered level, with the delay. */
    qtest_writeb(qts, IRQCR0, 0x00);
    qtest_set_irq_in(qts, "/machine/mcu/icu", NULL, 64, 0);
    qtest_clock_step(qts, 800);
    g_assert_cmphex(qtest_readb(qts, IR64), ==, 0);
    qtest_set_irq_in(qts, "/machine/mcu/icu", NULL, 64, 1);
    qtest_clock_step(qts, 400);
    g_assert_cmphex(qtest_readb(qts, IR64), ==, 0); /* one sample only */
    qtest_clock_step(qts, 400);
    g_assert_cmphex(qtest_readb(qts, IR64), ==, 1); /* flipped by now */
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);
    flash_dir = g_dir_make_tmp("rx113-nmi-XXXXXX", NULL);
    g_assert_nonnull(flash_dir);
    flash_path = g_strdup_printf("%s/flash.bin", flash_dir);

    qtest_add_func("/rx113-nmi/registers", test_registers);
    qtest_add_func("/rx113-nmi/nmier-set-once", test_nmier_set_once);
    qtest_add_func("/rx113-nmi/nmicr-gate", test_nmicr_gate);
    qtest_add_func("/rx113-nmi/source-flags", test_source_flags);
    qtest_add_func("/rx113-nmi/pin-edges", test_pin_edges);
    qtest_add_func("/rx113-nmi/pin-via-port", test_pin_via_port);
    qtest_add_func("/rx113-nmi/delivery-ost", test_delivery_ost);
    qtest_add_func("/rx113-nmi/delivery-ost-gated", test_delivery_ost_gated);
    qtest_add_func("/rx113-nmi/delivery-iwdt", test_delivery_iwdt);
    qtest_add_func("/rx113-nmi/filter-registers", test_filter_registers);
    qtest_add_func("/rx113-nmi/filter-nmi", test_filter_nmi);
    qtest_add_func("/rx113-nmi/filter-nmi-divider", test_filter_nmi_divider);
    qtest_add_func("/rx113-nmi/filter-irq", test_filter_irq);

    ret = g_test_run();
    unlink(flash_path);
    rmdir(flash_dir);
    g_free(flash_path);
    g_free(flash_dir);
    return ret;
}
