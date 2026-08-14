#include <stddef.h>
#include <stdint.h>

extern uint32_t __etext;
extern uint32_t __data_start;
extern uint32_t __data_end;
extern uint32_t __bss_start;
extern uint32_t __bss_end;
extern uint32_t __heap_start;
extern uint32_t __heap_end;
extern uint32_t __stack_top;

int main(void);

void Reset_Handler(void);
void Default_Handler(void);

/* Cortex-M3 core exceptions we don't otherwise use: alias them all to a
 * stub that just spins, so a stray fault halts visibly instead of jumping
 * through whatever garbage happens to be at address 0. */
void NMI_Handler(void)        __attribute__((weak, alias("Default_Handler")));
void HardFault_Handler(void)  __attribute__((weak, alias("Default_Handler")));
void MemManage_Handler(void)  __attribute__((weak, alias("Default_Handler")));
void BusFault_Handler(void)   __attribute__((weak, alias("Default_Handler")));
void UsageFault_Handler(void) __attribute__((weak, alias("Default_Handler")));
void SVC_Handler(void)        __attribute__((weak, alias("Default_Handler")));
void DebugMon_Handler(void)   __attribute__((weak, alias("Default_Handler")));
void PendSV_Handler(void)     __attribute__((weak, alias("Default_Handler")));
void SysTick_Handler(void)    __attribute__((weak, alias("Default_Handler")));

typedef void (*isr_t)(void);

/* The ADuCM3029 defines 64 external interrupt lines (RTC, GPIO, UART, DMA,
 * timers, ...). Nothing here uses interrupt-driven peripherals (SPI and
 * UART are polled, and the AD5940's own interrupt status is polled over
 * SPI rather than wired to an MCU GPIO interrupt -- see ad5940_port.c), but
 * the NVIC indexes into the vector table positionally by IRQ number, so
 * the table has to be the full length regardless. */
#define NUM_EXTERNAL_IRQS 64

__attribute__((section(".vectors"), used))
const isr_t vector_table[16 + NUM_EXTERNAL_IRQS] = {
    (isr_t)&__stack_top,            /* initial stack pointer */
    Reset_Handler,
    NMI_Handler,
    HardFault_Handler,
    MemManage_Handler,
    BusFault_Handler,
    UsageFault_Handler,
    0, 0, 0, 0,                     /* reserved */
    SVC_Handler,
    DebugMon_Handler,
    0,                               /* reserved */
    PendSV_Handler,
    SysTick_Handler,
    [16 ... 16 + NUM_EXTERNAL_IRQS - 1] = Default_Handler,
};

/* Cortex-M3 core register (ARMv7-M architecture reference manual, fixed on
 * every Cortex-M3, not vendor-specific): relocates the vector table. ADI's
 * boot ROM jumps into Reset_Handler rather than doing a full CPU reset, and
 * leaves VTOR pointing at its own vector table in ROM. Any fault taken
 * before this line gets dispatched through the boot ROM's handlers instead
 * of ours -- which, on a debugger, looks exactly like the CPU somehow
 * ending up back inside the boot kernel. */
#define SCB_VTOR (*(volatile uint32_t *)0xE000ED08u)

void Reset_Handler(void)
{
    SCB_VTOR = (uint32_t)vector_table;

    uint32_t *src = &__etext;
    uint32_t *dst = &__data_start;

    while (dst < &__data_end) {
        *dst++ = *src++;
    }

    dst = &__bss_start;
    while (dst < &__bss_end) {
        *dst++ = 0;
    }

    main();

    while (1) { }
}

void Default_Handler(void)
{
    while (1) { }
}

/* newlib syscall stub: bump allocator between __bss_end and the stack,
 * both defined in linker.ld. Needed because printf's float conversion
 * (dtoa) mallocs scratch space internally; everything else this program
 * calls (_write, _exit, _close, _lseek, _read, _fstat, _isatty) uses
 * -specs=nosys.specs's default stubs unmodified. */
void *_sbrk(ptrdiff_t incr)
{
    static uint32_t *heap_end = 0;
    uint32_t *prev_heap_end;

    if (heap_end == 0) {
        heap_end = &__heap_start;
    }
    prev_heap_end = heap_end;
    if ((uint8_t *)heap_end + incr > (uint8_t *)&__heap_end) {
        return (void *)-1;
    }
    heap_end = (uint32_t *)((uint8_t *)heap_end + incr);
    return prev_heap_end;
}
