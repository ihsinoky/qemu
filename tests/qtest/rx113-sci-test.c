/*
 * RX113 SCI backend order and SCI8/9 wiring (manual sections 11/14/28).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/sockets.h"

#define PRCR 0x803fe
#define MSTPCRB 0x80014
#define MSTPCRC 0x80018
#define IR(n) (0x87000 + (n))

static const unsigned bases[] = {
    0x8a000, 0x8a020, 0x8a040, 0x8a0a0, 0x8a0c0, 0x8b300,
    0x8a100, 0x8a120,
};
static const unsigned vectors[] = { 214, 218, 186, 222, 226, 238, 230, 234 };

typedef struct SCITest {
    QTestState *qts;
    char *dir;
    int fd[G_N_ELEMENTS(bases)];
} SCITest;

static void sci_init(SCITest *s)
{
    g_autoptr(GString) args = g_string_new("-M rx113-r5f51138");

    s->dir = g_dir_make_tmp("rx113-sci-XXXXXX", NULL);
    g_assert_nonnull(s->dir);
    for (unsigned i = 0; i < G_N_ELEMENTS(bases); i++) {
        g_string_append_printf(args, " -chardev socket,id=s%u,path=%s/s%u,"
                               "server=on,wait=off -serial chardev:s%u",
                               i, s->dir, i, i);
    }
    s->qts = qtest_init(args->str);
    for (unsigned i = 0; i < G_N_ELEMENTS(bases); i++) {
        g_autofree char *path = g_strdup_printf("%s/s%u", s->dir, i);

        s->fd[i] = unix_connect(path, NULL);
        g_assert_cmpint(s->fd[i], >=, 0);
    }
    qtest_writew(s->qts, PRCR, 0xa503);
    qtest_writeb(s->qts, 0x80036, 0); /* HOCO start */
    qtest_clock_step(s->qts, 34500);
    qtest_writew(s->qts, 0x80026, 0x100);
    qtest_writel(s->qts, 0x80020, 0); /* PCLKB = 32 MHz */
}

static void sci_quit(SCITest *s)
{
    for (unsigned i = 0; i < G_N_ELEMENTS(bases); i++) {
        close(s->fd[i]);
    }
    qtest_quit(s->qts);
    rmdir(s->dir);
    g_free(s->dir);
}

static void host_receive(int fd, char expected)
{
    GPollFD pfd = { .fd = fd, .events = G_IO_IN };
    char byte;

    g_assert_cmpint(g_poll(&pfd, 1, 1000), ==, 1);
    g_assert_cmpint(recv(fd, &byte, 1, 0), ==, 1);
    g_assert_cmpint(byte, ==, expected);
}

static void guest_receive(QTestState *qts, unsigned base)
{
    for (unsigned i = 0; i < 1000; i++) {
        if (qtest_readb(qts, base + 4) & 0x40) {
            return;
        }
        g_usleep(1000);
    }
    g_assert_not_reached();
}

/* Check the ICU's latched requests for all eight SCI channels. */
static void expect_irq(QTestState *qts, int expected)
{
    for (unsigned ch = 0; ch < G_N_ELEMENTS(vectors); ch++) {
        for (unsigned irq = vectors[ch]; irq < vectors[ch] + 4; irq++) {
            g_assert_cmpuint(qtest_readb(qts, IR(irq)), ==,
                             expected >= 0 && irq == (unsigned)expected);
        }
    }
}

static void clear_irqs(QTestState *qts)
{
    for (unsigned ch = 0; ch < G_N_ELEMENTS(vectors); ch++) {
        for (unsigned irq = vectors[ch]; irq < vectors[ch] + 4; irq++) {
            qtest_writeb(qts, IR(irq), 0);
        }
    }
}

static void test_backend_order(void)
{
    SCITest s;

    sci_init(&s);
    qtest_writel(s.qts, MSTPCRB, 0x19ffffef);
    qtest_writel(s.qts, MSTPCRC, 0x73fffffe);
    for (unsigned i = 0; i < G_N_ELEMENTS(bases); i++) {
        char byte = 'A' + i;

        qtest_writeb(s.qts, bases[i] + 1, 8);
        qtest_writeb(s.qts, bases[i] + 2, 0x30); /* TE, RE */
        qtest_writeb(s.qts, bases[i] + 3, byte);
        host_receive(s.fd[i], byte);
        g_assert_cmpint(send(s.fd[i], &byte, 1, 0), ==, 1);
        guest_receive(s.qts, bases[i]);
        g_assert_cmpuint(qtest_readb(s.qts, bases[i] + 5), ==, byte);
    }
    sci_quit(&s);
}

static void test_channel(const void *opaque)
{
    unsigned ch = GPOINTER_TO_UINT(opaque);
    unsigned base = bases[ch], irq = vectors[ch], bit = ch == 6 ? 27 : 26;
    uint32_t run = 0x7ffffffe & ~(1U << bit);
    SCITest s;
    QTestState *qts;

    sci_init(&s);
    qts = s.qts;
    /* Reset gate, PRC1 protection and reserved-one readback. */
    g_assert_cmphex(qtest_readl(qts, MSTPCRC), ==, 0x7ffffffe);
    g_assert_cmphex(qtest_readb(qts, base + 1), ==, 0);
    qtest_writew(qts, PRCR, 0xa501);
    qtest_writel(qts, MSTPCRC, run);
    g_assert_cmphex(qtest_readl(qts, MSTPCRC), ==, 0x7ffffffe);
    qtest_writew(qts, PRCR, 0xa503);
    qtest_writel(qts, MSTPCRC, 0);
    g_assert_cmphex(qtest_readl(qts, MSTPCRC), ==, 0x73e7fffe);
    qtest_writel(qts, MSTPCRC, run);
    g_assert_cmphex(qtest_readb(qts, base + 1), ==, 0xff);
    g_assert_cmphex(qtest_readb(qts, base + 4), ==, 0x84);
    qtest_writeb(qts, base + 1, 8);
    qtest_writeb(qts, base + 2, 0xf4); /* TE/RE, TIE/RIE/TEIE */
    expect_irq(qts, irq + 2); /* TXI on transmitter enable */
    clear_irqs(qts);
    qtest_writeb(qts, base + 3, 'X');
    host_receive(s.fd[ch], 'X');
    expect_irq(qts, irq + 2); /* TXI on TDR -> TSR */
    clear_irqs(qts);
    qtest_clock_step(qts, 36000);
    qtest_writel(qts, 0x80020, 0x100); /* PCLKB halves mid-character */
    qtest_clock_step(qts, 71999);
    expect_irq(qts, -1);
    qtest_writel(qts, MSTPCRC, 0x7ffffffe);
    qtest_writeb(qts, base + 2, 0); /* stopped MMIO must not change SCR */
    qtest_writeb(qts, base + 1, 100);
    qtest_clock_step(qts, 1000000);
    expect_irq(qts, -1);
    g_assert_cmpuint(qtest_readb(qts, base + 4), ==, 0);
    qtest_writel(qts, MSTPCRC, run);
    g_assert_cmphex(qtest_readb(qts, base + 2), ==, 0xf4);
    g_assert_cmpuint(qtest_readb(qts, base + 1), ==, 8);
    qtest_clock_step(qts, 1);
    expect_irq(qts, irq + 3); /* TEI at retained completion deadline */
    /* Disable TX interrupts before testing RX and overrun ERI. */
    qtest_writeb(qts, base + 2, 0x50);
    clear_irqs(qts);
    g_assert_cmpint(send(s.fd[ch], "A", 1, 0), ==, 1);
    guest_receive(qts, base);
    expect_irq(qts, irq + 1);
    g_assert_cmpuint(qtest_readb(qts, base + 5), ==, 'A');
    clear_irqs(qts);
    qtest_clock_step(qts, 72000); /* half the 144 us receive cooldown */
    qtest_writel(qts, MSTPCRC, 0x7ffffffe);
    g_assert_cmpint(send(s.fd[ch], "B", 1, 0), ==, 1);
    qtest_clock_step(qts, 1000000);
    expect_irq(qts, -1);
    qtest_writel(qts, MSTPCRC, run);
    qtest_clock_step(qts, 71999);
    g_assert_cmphex(qtest_readb(qts, base + 4) & 0x40, ==, 0);
    qtest_clock_step(qts, 1);
    guest_receive(qts, base);
    expect_irq(qts, irq + 1);
    clear_irqs(qts);
    /* Leave B unread and receive C: ORER must assert this channel's ERI. */
    qtest_clock_step(qts, 144000);
    g_assert_cmpint(send(s.fd[ch], "C", 1, 0), ==, 1);
    for (unsigned i = 0; i < 1000 &&
         !(qtest_readb(qts, base + 4) & 0x20); i++) {
        g_usleep(1000);
    }
    g_assert_cmphex(qtest_readb(qts, base + 4) & 0x20, ==, 0x20);
    expect_irq(qts, irq);
    g_assert_cmpuint(qtest_readb(qts, base + 5), ==, 'B');
    qtest_writeb(qts, base + 4, 0); /* clear error after SSR read */
    clear_irqs(qts);
    expect_irq(qts, -1);
    qtest_qmp_assert_success(qts, "{'execute':'system_reset'}");
    g_assert_cmphex(qtest_readl(qts, MSTPCRC), ==, 0x7ffffffe);
    g_assert_cmpuint(qtest_readb(qts, base + 1), ==, 0);
    qtest_writew(qts, PRCR, 0xa502);
    qtest_writel(qts, MSTPCRC, run);
    g_assert_cmpuint(qtest_readb(qts, base + 1), ==, 0xff);
    sci_quit(&s);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/rx113-sci/backend-order", test_backend_order);
    qtest_add_data_func("/rx113-sci/sci8", GUINT_TO_POINTER(6), test_channel);
    qtest_add_data_func("/rx113-sci/sci9", GUINT_TO_POINTER(7), test_channel);
    return g_test_run();
}
