ENTRY(Reset_Handler)

MEMORY
{
    /* ADuCM3029: flash starts at 0x0; every part in the family has at
     * least 256 KB. */
    FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 256K
    /* The chip actually has ~60 KB of SRAM split across two apertures
     * (0x20000000 and 0x20040000), but 16 KB from the first one is
     * guaranteed present and is far more than this program needs. */
    SRAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 16K
}

SECTIONS
{
    .vectors :
    {
        KEEP(*(.vectors))
    } > FLASH

    /* ADI's factory kernel ROM (the thing that runs before jumping to our
     * Reset_Handler) checks this fixed flash offset before it will boot our
     * image at all -- confirmed by hardware: without it, OpenOCD shows the
     * CPU permanently parked in the kernel, never reaching flash, no matter
     * what's actually in .text. All-0xFFFFFFFF plus this magic marker is
     * ADI's own "no security features programmed" pattern (same bytes
     * their CCES-generated projects ship). */
    .security_options 0x180 :
    {
        LONG(0xFFFFFFFF)
        LONG(0xFFFFFFFF)
        LONG(0xFFFFFFFF)
        LONG(0xFFFFFFFF)
        LONG(0xA79C3203)
        LONG(0)
        LONG(0xFFFFFFFF)
        LONG(0xFFFFFFFF)
    } > FLASH

    .text :
    {
        *(.text*)
        *(.rodata*)
    } > FLASH

    /* End of flash-resident code: this is where .data's initial values
     * are stored (its "load address"), separate from where it actually
     * lives at runtime (its "virtual address", in SRAM below). */
    __etext = .;

    .data : AT (__etext)
    {
        __data_start = .;
        *(.data*)
        __data_end = .;
    } > SRAM

    .bss :
    {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        __bss_end = .;
    } > SRAM

    __stack_top = ORIGIN(SRAM) + LENGTH(SRAM);
}
