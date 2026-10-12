/* linker.ld — flat-ish ELF image, loaded at 0x10000 by stage2.
 * `objcopy -O binary` turns the result into kernel.bin (offset 0 in
 * the flat file == virtual 0x10000). */
ENTRY(entry)

SECTIONS
{
    . = 0x10000;

    .text : {
        *(.text)
        *(.text.*)
    }

    .rodata : {
        *(.rodata)
        *(.rodata.*)
    }

    .data : {
        *(.data)
        *(.data.*)
    }

    .bss : {
        __bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        __bss_end = .;
    }

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
        *(.debug*)
        *(.drectve)
    }
}
