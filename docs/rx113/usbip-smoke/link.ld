ENTRY(_start)
PHDRS {
    flash PT_LOAD FLAGS(5);
    opts  PT_LOAD FLAGS(4);
    none  PT_NULL;
}
MEMORY {
    FLASH (rx)  : ORIGIN = 0xFFF80000, LENGTH = 512K
    RAM   (rwx) : ORIGIN = 0x00000000, LENGTH = 64K
}
SECTIONS {
    .text 0xFFF80000 : {
        *(.text.start)
        *(.text*)
        *(.rodata*)
        *(P) *(C)
    } > FLASH :flash
    .bss : {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        *(B*) *(D_1)
        __bss_end = .;
    } > RAM :none
    .ofs 0xFFFFFF80 : {
        KEEP(*(.ofs))
    } > FLASH :opts
    .fvectors 0xFFFFFFFC : {
        KEEP(*(.fvectors))
    } > FLASH :opts
    __stack_top = 0x00010000;
}
