/*
 * Minimal USB function firmware for the RX113 model: enumerates
 * as a vendor-specific bulk echo device (EP1 IN / EP2 OUT) and
 * echoes whatever arrives on EP2 back on EP1.  Polling only.
 * Register flows mirror tests/qtest/rx113-usb-test.c.
 */
#include <stdint.h>
#include <stddef.h>

#define REG8(a)  (*(volatile uint8_t *)(uintptr_t)(a))
#define REG16(a) (*(volatile uint16_t *)(uintptr_t)(a))
#define REG32(a) (*(volatile uint32_t *)(uintptr_t)(a))

#define SYS 0x00080000u
#define USB 0x000a0000u

#define SYSCFG   (USB + 0x00)
#define DVSTCTR0 (USB + 0x08)
#define CFIFO    (USB + 0x14)
#define CFIFOSEL (USB + 0x20)
#define CFIFOCTR (USB + 0x22)
#define INTSTS0  (USB + 0x40)
#define BRDYSTS  (USB + 0x46)
#define BEMPSTS  (USB + 0x4a)
#define USBREQ   (USB + 0x54)
#define USBVAL   (USB + 0x56)
#define USBINDX  (USB + 0x58)
#define USBLENG  (USB + 0x5a)
#define DCPCFG   (USB + 0x5c)
#define DCPMAXP  (USB + 0x5e)
#define DCPCTR   (USB + 0x60)
#define PIPESEL  (USB + 0x64)
#define PIPECFG  (USB + 0x68)
#define PIPEMAXP (USB + 0x6c)
#define PIPE1CTR (USB + 0x70)
#define PIPE2CTR (USB + 0x72)

static const uint8_t dev_desc[18] = {
    18, 1, 0x00, 0x02, 0, 0, 0, 64,
    0x09, 0x12, 0x11, 0x74, 0x00, 0x01, 0, 0, 0, 1
};
static const uint8_t cfg_desc[32] = {
    9, 2, 32, 0, 1, 1, 0, 0x80, 50,
    9, 4, 0, 0, 2, 0xff, 0, 0, 0,
    7, 5, 0x81, 2, 64, 0, 0,
    7, 5, 0x02, 2, 64, 0, 0
};
static const uint8_t zero2[2] = { 0, 0 };

/* Option/programming area at the top of flash; HOCO on at reset. */
__attribute__((section(".ofs"), used))
static const uint8_t ofs_area[0x7c] = {
    [0 ... 0x7b] = 0xff,
    [9] = 0xfe /* OFS1 bit 8 = 0: HOCO enabled at reset */
};
extern void start(void);
__attribute__((section(".fvectors"), used))
static void *const reset_vector[1] = { (void *)start };

static uint8_t echo_buf[256];
static int configured;

static void spin(unsigned long n)
{
    while (n--) {
        __asm__ volatile ("nop");
    }
}

static void clock_and_module_init(void)
{
    REG16(SYS + 0x3fe) = 0xa503;      /* PRCR: unlock */
    REG8(SYS + 0x32) = 0;             /* UCLK source select */
    spin(300000);
    REG16(SYS + 0x2c) = 0x0b11;       /* UCLK = 48 MHz */
    REG8(SYS + 0x2e) = 0;
    spin(300000);
    while ((REG8(SYS + 0x3c) & 0x21) != 0x21) {
        /* wait for oscillator stability flags */
    }
    REG32(SYS + 0x14) &= ~(1u << 19); /* MSTPCRB: USB0 run */
}

static uint16_t stage(void)
{
    return REG16(INTSTS0) & 7;        /* DVSQ */
}

static void clear_valid(void)
{
    REG16(INTSTS0) = 0xfff7;          /* VALID = 0 (write 0) */
}

static void dcp_write(const uint8_t *data, unsigned len)
{
    unsigned i;

    REG16(CFIFOSEL) = 0x20;           /* DCP, write direction */
    REG16(DCPCTR) = 1;                /* BUF */
    for (i = 0; i < len; i++) {
        REG8(CFIFO) = data[i];
    }
    REG16(CFIFOCTR) = 0x8000;         /* BVAL */
}

static void control_complete(void)
{
    while (stage() == 0) {
        /* setup stage: host has not latched yet */
    }
    /* If a data stage is in flight, wait for the status stage. */
    while (stage() == 1 || stage() == 3) {
    }
    if (stage() == 2 || stage() == 4 || stage() == 5) {
        REG16(DCPCTR) = 5;            /* CCPL */
    }
    while (stage() != 0) {
    }
}

static void handle_setup(void)
{
    uint16_t req = REG16(USBREQ);
    uint16_t val = REG16(USBVAL);
    uint16_t len = REG16(USBLENG);
    uint8_t breq = (uint8_t)(req >> 8);

    if (req == 0x0680 && val == 0x0100) {          /* GET_DESCRIPTOR dev */
        clear_valid();
        if (len > 18) {
            len = 18;
        }
        dcp_write(dev_desc, len);
        control_complete();
    } else if (req == 0x0680 && val == 0x0200) {   /* GET_DESCRIPTOR cfg */
        clear_valid();
        if (len > 32) {
            len = 32;
        }
        dcp_write(cfg_desc, len);
        control_complete();
    } else if (breq == 5) {                        /* SET_ADDRESS: auto */
        clear_valid();
    } else if (breq == 9) {                        /* SET_CONFIGURATION */
        clear_valid();
        control_complete();
        configured = (val & 0xff) != 0;
        if (configured) {
            REG16(PIPESEL) = 1;
            REG16(PIPECFG) = 0x4011;  /* bulk, SIE sends, EP1 */
            REG16(PIPEMAXP) = 64;
            REG16(PIPESEL) = 2;
            REG16(PIPECFG) = 0x4002;  /* bulk, SIE receives, EP2 */
            REG16(PIPEMAXP) = 64;
            REG16(PIPESEL) = 0;
            REG16(PIPE2CTR) = 1;      /* arm OUT pipe */
        }
    } else if (req == 0x0880) {                    /* GET_CONFIGURATION */
        static const uint8_t one = 1, zero = 0;

        clear_valid();
        dcp_write(configured ? &one : &zero, 1);
        control_complete();
    } else if (req == 0x0080) {                    /* GET_STATUS */
        clear_valid();
        if (len > 2) {
            len = 2;
        }
        dcp_write(zero2, len);
        control_complete();
    } else {
        /* Stall unsupported requests. */
        clear_valid();
        REG16(DCPCTR) = 2;            /* PID = STALL */
    }
}

static void poll_bulk_echo(void)
{
    unsigned n, i;
    uint16_t ctr;

    if (!(REG16(BRDYSTS) & (1u << 2))) {
        return;
    }
    REG16(CFIFOSEL) = 0x8402;         /* pipe 2, read + RCNT */
    while (!(REG16(CFIFOCTR) & 0x2000)) {
        /* FRDY */
    }
    ctr = REG16(CFIFOCTR);
    n = ctr & 0x1ff;
    if (n > sizeof(echo_buf)) {
        n = sizeof(echo_buf);
    }
    for (i = 0; i < n; i++) {
        echo_buf[i] = REG8(CFIFO);
    }
    REG16(CFIFOCTR) = 0x4000;         /* BCLR */

    REG16(CFIFOSEL) = 0x401;          /* pipe 1, write */
    REG16(PIPE1CTR) = 1;              /* BUF */
    for (i = 0; i < n; i++) {
        REG8(CFIFO) = echo_buf[i];
    }
    REG16(CFIFOCTR) = 0x8000;         /* BVAL */
    while (!(REG16(BEMPSTS) & (1u << 1))) {
    }
    REG16(BEMPSTS) = 0xfffd;
}

int main(void)
{
    clock_and_module_init();

    REG16(SYSCFG) = 0x401;            /* USBE | SCKE, function */
    REG16(DCPCFG) = 0;
    REG16(DCPMAXP) = 0x40;            /* MPS 64 */
    REG16(SYSCFG) = 0x409;            /* + DPRPU */

    for (;;) {
        if (configured && stage() == 1) {
            /* a bus reset dropped us back to Powered */
            configured = 0;
        }
        if (REG16(INTSTS0) & 0x0008) { /* VALID */
            handle_setup();
        }
        if (configured) {
            poll_bulk_echo();
        }
    }
    return 0;
}
