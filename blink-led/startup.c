#include <stdint.h>

extern uint32_t __etext;
extern uint32_t __data_start;
extern uint32_t __data_end;
extern uint32_t __bss_start;
extern uint32_t __bss_end;
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
 * timers, ...). This program doesn't enable any of them, but the NVIC
 * indexes into the vector table positionally by IRQ number, so the table
 * has to be the full length even though every external entry here is an
 * unused stub. */
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

void Reset_Handler(void)
{
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
