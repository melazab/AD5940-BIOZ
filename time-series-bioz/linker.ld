ENTRY(Reset_Handler)

MEMORY
{
    /* ADuCM3029: flash starts at 0x0; every part in the family has at
     * least 256 KB. */
    FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 256K
    /* Total SRAM is 64KB, split between code and data SRAM apertures
     * (0x20000000 and 0x20040000) by PMG_TST_SRAM_CTL.INSTREN, which this
     * program never touches. 32KB was tried here first on the assumption
     * that the reset default leaves that much contiguous at 0x20000000;
     * that was wrong -- confirmed by hardware (SWD bus fault, STKERR,
     * trying to access 0x20007fdc) that memory above roughly 16KB isn't
     * actually backed there under the reset default. 16KB is what
     * blink-led already proved present and working; stick to that
     * confirmed-safe value instead of an unverified datasheet inference. */
    SRAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 16K
}

/* Reserve this much SRAM, right below the stack, for _sbrk()'s heap --
 * see startup.c. Only printf's internal float-to-string conversion
 * (dtoa) allocates here, in small, short-lived chunks. */
HEAP_SIZE = 4K;

SECTIONS
{
    .vectors :
    {
        KEEP(*(.vectors))
    } > FLASH

    /* ADI's factory kernel ROM (the thing that runs before jumping to our
     * Reset_Handler) checks these fixed flash fields before it will boot
     * our image -- see EE-381 "Using the ADuCM302x Processor Boot Kernel",
     * Table 2:
     *   0x180-0x18F  read-protection key hash (all-FF = not requested)
     *   0x190        CRC of that key hash (gates SWD/read-protection state
     *                only, not booting -- left as ADI's own placeholder
     *                value, same as their CCES-generated projects ship)
     *   0x194        N: flash pages 0..N are CRC-protected before boot
     *   0x198        in-circuit write-protect key (all-FF = not requested)
     *   0x19C        per-block flash write-protect (all-FF = none)
     * When N pages are protected, the kernel expects a real CRC32 (poly
     * 0x4C11DB7) of pages 0..N stored at (N*0x800)+0x7FC -- *unless* that
     * exact word reads 0xFFFFFFFF, which explicitly disables the check.
     * N=127 (max for this part) puts that word at 0x3FFFC, comfortably
     * past this program's actual end, so growing the program can never
     * collide with it (see the LONG(0xFFFFFFFF) at that address below). */
    .security_options 0x180 :
    {
        LONG(0xFFFFFFFF)
        LONG(0xFFFFFFFF)
        LONG(0xFFFFFFFF)
        LONG(0xFFFFFFFF)
        LONG(0xA79C3203)
        LONG(127)
        LONG(0xFFFFFFFF)
        LONG(0xFFFFFFFF)
    } > FLASH

    .text :
    {
        *(.text*)
        *(.rodata*)
    } > FLASH

    /* GCC emits an ARM EH unwind index even without exceptions enabled;
     * without an explicit home for it here, it lands at the same load
     * address as .data below and the linker complains about the overlap. */
    .ARM.exidx :
    {
        *(.ARM.exidx*)
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

    /* Heap is a fixed-size region directly above .bss; the stack takes
     * everything above that, growing down from the true top of SRAM. */
    __heap_start = __bss_end;
    __heap_end = __heap_start + HEAP_SIZE;
    __stack_top = ORIGIN(SRAM) + LENGTH(SRAM);

    /* Disables the user-code CRC check for N=127 in .security_options
     * above. Placed last and at a fixed absolute address so it can never
     * overlap real .text regardless of how the rest of the program grows;
     * the ASSERT catches it at link time instead of silently bricking the
     * boot again if this program ever grows past 254KB. */
    ASSERT(__etext <= 0x0003FFFC, "program has grown into the boot-kernel CRC-disable word at 0x3FFFC -- see linker.ld")
    .user_code_crc_disable 0x0003FFFC :
    {
        LONG(0xFFFFFFFF)
    } > FLASH
}
