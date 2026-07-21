#ifndef REGISTERS_H
#define REGISTERS_H

#include <stdint.h>

/*
 * Minimal hand-written ADuCM3029 register definitions -- just the GPIO,
 * watchdog and clock-gating registers this example touches. Addresses and
 * field positions cross-checked against Analog Devices' own CMSIS device
 * header (ADuCM302x.h, as shipped in mbed-os) and their EVAL-ADICUP3029
 * example firmware, not retyped from the datasheet blind.
 */

/* ---- GPIO (ports 0, 1, 2; identical layout every 0x40 bytes) ---- */

typedef struct {
    volatile uint32_t CFG;   /* 0x00 pin mux, 2 bits/pin, 00 = plain GPIO */
    volatile uint16_t OEN;   /* 0x04 output enable, 1 bit/pin */
             uint16_t _rsvd0;
    volatile uint16_t PE;    /* 0x08 pull-up/down enable */
             uint16_t _rsvd1;
    volatile uint16_t IEN;   /* 0x0C input path enable */
             uint16_t _rsvd2;
    volatile const uint16_t IN;  /* 0x10 registered pin input level */
             uint16_t _rsvd3;
    volatile uint16_t OUT;   /* 0x14 output data latch */
             uint16_t _rsvd4;
    volatile uint16_t SET;   /* 0x18 write-1-to-set bits in OUT */
             uint16_t _rsvd5;
    volatile uint16_t CLR;   /* 0x1C write-1-to-clear bits in OUT */
             uint16_t _rsvd6;
    volatile uint16_t TGL;   /* 0x20 write-1-to-toggle bits in OUT */
    /* POL/IENA/IENB/INT/DS follow; unused here, omitted */
} GPIO_TypeDef;

#define GPIO0 ((GPIO_TypeDef *)0x40020000u)
#define GPIO1 ((GPIO_TypeDef *)0x40020040u)
#define GPIO2 ((GPIO_TypeDef *)0x40020080u)

/* ---- Watchdog timer (WDT0) ----
 * ADuCM302x parts boot with the watchdog already running, so every ADI
 * example -- and this one -- disables it early. */

typedef struct {
    volatile uint16_t LOAD;
             uint16_t _rsvd0;
    volatile const uint16_t CCNT;
             uint16_t _rsvd1;
    volatile uint16_t CTL;
             uint16_t _rsvd2;
    volatile uint16_t RESTART;
} WDT_TypeDef;

#define WDT0 ((WDT_TypeDef *)0x40002C00u)
#define WDT_CTL_EN (1u << 5)

/* ---- Clock gating (CLKG0_CLK) ----
 * CTL5 gates individual peripheral clocks; bit 4 gates GPIO. */

typedef struct {
             uint32_t _rsvd[5];
    volatile uint32_t CTL5;  /* offset 0x14 */
} CLKG_CLK_TypeDef;

#define CLKG0_CLK ((CLKG_CLK_TypeDef *)0x4004C300u)
#define CLKG_CLK_CTL5_GPIOCLKOFF (1u << 4)

#endif
