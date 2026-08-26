#ifndef REGISTERS_H
#define REGISTERS_H

#include <stdint.h>

/*
 * Minimal hand-written ADuCM3029 register definitions -- GPIO, watchdog,
 * clock (oscillator + gating), SPI0 and UART0, which is everything this
 * program touches to talk to the AD5940 over SPI and report results over
 * the DAPLink virtual COM port. Addresses, field layout and bitmasks
 * cross-checked field-by-field against Analog Devices' own CMSIS device
 * header (ADuCM302x_device.h/ADuCM302x.h, as shipped in mbed-os) and their
 * ADICUP3029Port.c/main.c reference for the AD5940_BIOZ-2Wire example, not
 * retyped from the datasheet blind.
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
             uint16_t _rsvd3[5];
    volatile const uint16_t STAT;
} WDT_TypeDef;

#define WDT0 ((WDT_TypeDef *)0x40002C00u)
#define WDT_CTL_EN (1u << 5)
/* WDT0 runs on its own (LFCLK-derived) clock domain, separate from the
 * CPU's -- a write to LOAD/CTL/RESTART takes a few WDT clock cycles to
 * actually land, and STAT's corresponding bit stays set until it does
 * (cross-checked against mbed-os's own ADuCM3029 WDT driver, which polls
 * these same bits before/after each such write). */
#define WDT_STAT_CLRIRQ   (1u << 1) /* RESTART write sync in progress */
#define WDT_STAT_LOADING  (1u << 2) /* LOAD write sync in progress */
#define WDT_STAT_COUNTING (1u << 3) /* CTL write sync in progress */
#define WDT_RESTART_KEY 0xCCCCu     /* value that actually kicks/reloads it */

/* ---- Clock oscillator control (CLKG0_OSC) ----
 * Selects/enables the high-frequency crystal that the ADICUP3029 board's
 * MCU clock tree runs from. Unrelated to the AD5940's own clock, which the
 * AD5940 library configures separately, over SPI, on the AFE itself. */

typedef struct {
             uint8_t  _rsvd0[12];
    volatile uint32_t KEY;   /* 0x0C write 0xCB14 to unlock CTL */
    volatile uint32_t CTL;   /* 0x10 oscillator enables + status */
             uint8_t  _rsvd1[8];
} CLKG_OSC_TypeDef;

#define CLKG0_OSC ((CLKG_OSC_TypeDef *)0x4004C100u)
#define CLKG_OSC_KEY_UNLOCK 0xCB14u
#define CLKG_OSC_CTL_HFOSCEN  (1u << 1)
#define CLKG_OSC_CTL_HFXTALEN (1u << 3)
#define CLKG_OSC_CTL_HFXTALOK (1u << 11)

/* ---- Clock generation/gating (CLKG0_CLK) ----
 * CTL0 selects the system clock mux source, CTL1 holds the peripheral
 * clock dividers (needed to work out the real UART bit clock), CTL5 gates
 * individual peripheral clocks (bit 4 gates GPIO). */

typedef struct {
    volatile uint32_t CTL0;   /* 0x00 clock mux select + XTAL frequency bit */
    volatile uint32_t CTL1;   /* 0x04 ACLK/PCLK/HCLK dividers */
             uint8_t  _rsvd0[4];
    volatile uint32_t CTL3;   /* 0x0C system PLL multiplier (unused here) */
             uint8_t  _rsvd1[4];
    volatile uint32_t CTL5;   /* 0x14 peripheral clock gating */
} CLKG_CLK_TypeDef;

#define CLKG0_CLK ((CLKG_CLK_TypeDef *)0x4004C300u)
#define CLKG_CLK_CTL5_GPIOCLKOFF (1u << 4)
#define CLKG_CLK_CTL0_CLKMUX_MASK  0x3u   /* 00=HFOSC 01=HFXTAL 10=SPLL 11=EXTCLK */
#define CLKG_CLK_CTL0_CLKMUX_XTAL  0x1u
#define CLKG_CLK_CTL0_HFXTAL26M    (1u << 9) /* set: board's XTAL is 26MHz, not 16MHz */
#define CLKG_CLK_CTL1_PCLKDIVCNT_SHIFT 8
#define CLKG_CLK_CTL1_PCLKDIVCNT_MASK  (0x3Fu << CLKG_CLK_CTL1_PCLKDIVCNT_SHIFT)
#define CLKG_CLK_CTL3_SPLLNSEL_MASK 0x1Fu

/* ---- SPI0 ----
 * Used as SPI master to talk to the AD5940. Every 16-bit register is
 * spaced 4 bytes apart, same padding convention as GPIO above. */

typedef struct {
    volatile uint16_t STAT;       /* 0x00 status (XFRDONE etc) */
             uint16_t _rsvd0;
    volatile const uint16_t RX;   /* 0x04 receive FIFO */
             uint16_t _rsvd1;
    volatile uint16_t TX;         /* 0x08 transmit FIFO */
             uint16_t _rsvd2;
    volatile uint16_t DIV;        /* 0x0C baud rate = PCLK/(2*(DIV+1)) */
             uint16_t _rsvd3;
    volatile uint16_t CTL;        /* 0x10 configuration */
             uint16_t _rsvd4;
             uint16_t _rsvd5a;    /* 0x14 IEN, not used (no SPI interrupts) */
             uint16_t _rsvd5b;
    volatile uint16_t CNT;        /* 0x18 transfer byte count */
             uint16_t _rsvd6;
             uint16_t _rsvd7a;    /* 0x1C DMA enable, not used */
             uint16_t _rsvd7b;
    volatile const uint16_t FIFO_STAT; /* 0x20 TX/RX FIFO byte counts */
} SPI_TypeDef;

#define SPI0 ((SPI_TypeDef *)0x40004000u)

#define SPI_STAT_XFRDONE (1u << 1)

#define SPI_CTL_CSRST  (1u << 14) /* reset SPI state machine on CS glitch */
#define SPI_CTL_OEN    (1u << 9)  /* MISO driven normally (not open-drain) */
#define SPI_CTL_RXOF   (1u << 8)  /* overwrite (don't stall) on RX overflow */
#define SPI_CTL_TIM    (1u << 6)  /* start transfer on write to TX */
#define SPI_CTL_MASEN  (1u << 1)  /* master mode */
#define SPI_CTL_SPIEN  (1u << 0)  /* enable SPI block */

/* FIFO_STAT: bits[3:0] = TX FIFO byte count, bits[11:8] = RX FIFO byte count */
#define SPI_FIFO_STAT_TXBYTES_MASK 0x000Fu
#define SPI_FIFO_STAT_RXBYTES_MASK 0x0F00u
#define SPI_FIFO_DEPTH_BYTES 8u

/* ---- UART0 ----
 * Retargets printf() to the DAPLink virtual COM port and reads back the
 * "start" command line. TX and RX share the same address (0x00): the
 * hardware decodes which register based on whether the bus access is a
 * write or a read, so both names are given the same offset here rather
 * than modeled as a union -- either works, this just says what each
 * access direction means. Everything else the AD5940 examples don't need
 * (IEN/IIR/MCR/MSR/SCR) is left as reserved padding to keep later fields
 * at the right offset. */

typedef struct {
    union {
        volatile uint16_t TX;       /* 0x00 transmit holding register (write) */
        volatile const uint16_t RX; /* 0x00 receive buffer register (read) */
    };
             uint16_t _rsvd0;
             uint16_t _rsvd1a; uint16_t _rsvd1b; /* 0x04 IEN, unused (no UART interrupts) */
             uint16_t _rsvd2a; uint16_t _rsvd2b; /* 0x08 IIR, unused */
    volatile uint16_t LCR;   /* 0x0C line control (word length, parity) */
             uint16_t _rsvd3;
             uint16_t _rsvd4a; uint16_t _rsvd4b; /* 0x10 MCR, unused */
    volatile const uint16_t LSR; /* 0x14 line status (THRE = TX ready) */
             uint16_t _rsvd5;
             uint16_t _rsvd6a; uint16_t _rsvd6b; /* 0x18 MSR, unused */
             uint16_t _rsvd7a; uint16_t _rsvd7b; /* 0x1C SCR, unused */
    volatile uint16_t FCR;   /* 0x20 FIFO control */
             uint16_t _rsvd8;
    volatile uint16_t FBR;   /* 0x24 fractional baud rate */
             uint16_t _rsvd9;
    volatile uint16_t DIV;   /* 0x28 integer baud rate divisor */
             uint16_t _rsvd10;
    volatile uint16_t LCR2;  /* 0x2C oversample rate select */
} UART_TypeDef;

#define UART0 ((UART_TypeDef *)0x40005000u)

#define UART_LCR_WLEN_8 0x3u
#define UART_LSR_DR   (1u << 0) /* receive buffer has data */
#define UART_LSR_THRE (1u << 5) /* transmit holding register empty */
#define UART_FCR_FIFOEN (1u << 0)
#define UART_FCR_RFCLR  (1u << 1)
#define UART_FCR_TFCLR  (1u << 2)

/* ---- SysTick ----
 * Not ADuCM3029-specific: part of the Cortex-M3 core itself (ARMv7-M
 * architecture reference manual), same address/layout on every Cortex-M3
 * part. Used here purely as a busy-wait timebase for AD5940_Delay10us(). */

typedef struct {
    volatile uint32_t CTRL; /* 0x00 ENABLE=bit0 TICKINT=bit1 CLKSOURCE=bit2 COUNTFLAG=bit16 */
    volatile uint32_t LOAD; /* 0x04 reload value (24-bit) */
    volatile uint32_t VAL;  /* 0x08 current value, write-any-clears */
} SysTick_TypeDef;

#define SYSTICK ((SysTick_TypeDef *)0xE000E010u)
#define SYSTICK_CTRL_ENABLE    (1u << 0)
#define SYSTICK_CTRL_CLKSOURCE (1u << 2)
#define SYSTICK_CTRL_COUNTFLAG (1u << 16)
#define SYSTICK_MAXCOUNT ((1u << 24) - 1)

#endif
