/* SPDX-License-Identifier: GPL-2.0-or-later */
OUTPUT_ARCH(rx)
ENTRY(_PowerON_Reset_PC)
MEMORY {
    RAM (rw) : ORIGIN = 0, LENGTH = 64K
    ROM (rx) : ORIGIN = 0xfff80000, LENGTH = 512K
}
PHDRS {
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
    fixed PT_LOAD FLAGS(4);
}
SECTIONS {
    .text : {
        *(.text*) *(P) *(.rodata*) *(C C_1 C_2)
        . = ALIGN(4);
        _rvectors_start = .; . += 1024;
    } > ROM :text
    .data : {
        _data = .; *(.data*) *(D D_1 D_2) _edata = .;
    } > RAM AT> ROM :data
    _mdata = LOADADDR(.data);
    .bss (NOLOAD) : {
        _bss = .; *(.bss*) *(B B_1 B_2) *(COMMON) _ebss = .;
    } > RAM :NONE
    .ustack (NOLOAD) : { *(.r_bsp_ustack) } > RAM :NONE
    .istack (NOLOAD) : { *(.r_bsp_istack) } > RAM :NONE
    _ustack = 0xff00;
    _istack = 0x10000;
    .fvectors 0xffffff80 : { KEEP(*(.fvectors)) } > ROM :fixed
    /DISCARD/ : { *(.comment) *(.note*) }
}
