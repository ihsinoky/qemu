    .section .text.start, "ax"
    .global _start
_start:
    mov.l #__stack_top, r0
    mov.l #__bss_start, r1
    mov.l #__bss_end, r2
    mov.l #0, r3
    cmp r1, r2
    beq 2f
1:  mov.b r3, [r1]
    add #1, r1
    cmp r1, r2
    bne 1b
2:  bsr.a _main
3:  bra.b 3b
