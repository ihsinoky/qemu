/*
 * RX113 FIT startup probe. Linked with unmodified Renesas R_BSP sources.
 * This application is a QEMU test, not a Renesas FIT SCI driver.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "platform.h"

/* Volatile forces a BSS read on every boot, including after QMP reset. */
volatile uint32_t main_reached;
/* Volatile makes the probe verify the runtime ROM-to-RAM data copy. */
volatile uint32_t data_canary = 0x12345678;

void main(void)
{
    const char *p = "RX113 FIT main reached\n";

    if (data_canary != 0x12345678 || main_reached != 0) {
        for (;;) {
            R_BSP_NOP();
        }
    }
    main_reached = 0x4d41494e;
    SYSTEM.PRCR.WORD = 0xa502;
    MSTP(SCI0) = 0;
    while (MSTP(SCI0)) {
        R_BSP_NOP();
    }
    SYSTEM.PRCR.WORD = 0xa500;
    SCI0.SCR.BYTE = 0;
    SCI0.SMR.BYTE = 0;
    SCI0.BRR = 8; /* Fixed 32 MHz PCLKB, approx. 115200 baud. */
    SCI0.SCR.BYTE = 0x20;
    while (*p) {
        while (!(SCI0.SSR.BYTE & 0x80)) {
            R_BSP_NOP();
        }
        SCI0.TDR = *p++;
    }
    for (;;) {
        R_BSP_NOP();
    }
}
