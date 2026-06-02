OUTPUT_ARCH(riscv)
ENTRY(_start)
SECTIONS
{
    . = 0xffffffc000000000;
    . += 0x80200000;
    _start = .;
    __kernel_start = .;

    .text : { *(.text.boot) *(.text) }
    .rodata : { *(.rodata) }
    .data : { *(.data) }
    .bss : {
        __bss_start = .;
        *(.sbss) *(.bss)
        __bss_stop = .;
    }

    . = ALIGN(16);
    __kernel_end = .;
    . = . + 0x4000;
    _end = .;
}

