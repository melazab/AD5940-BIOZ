#include <stdint.h>
#include "registers.h"
#include "ad5940.h"

/*
 * The AD5940 library (ad5940lib/, vendored unmodified from Analog Devices)
 * calls out to these functions for everything hardware-specific: SPI
 * transfers, the CS and RESET GPIOs, and a microsecond delay. This is our
 * from-scratch equivalent of ADI's own ADICUP3029Port.c reference, redone
 * against hand-verified register definitions instead of their CMSIS
 * headers, and with two corrections: it clears the actual CS/RESET pins'
 * mux fields (pin*2 bit position) rather than the reference's mismatched
 * shift amounts, which are harmless there only because those pins already
 * default to GPIO function at reset.
 *
 * There's no MCU-side GPIO interrupt from the AD5940's own interrupt pin
 * here (unlike ADI's reference, which wires it to XINT0). main.c instead
 * polls AppBIOZISR() continuously; it internally checks the AD5940's own
 * interrupt status register over SPI before doing any real work, so
 * polling it is functionally equivalent to waiting for the pin edge, just
 * without needing the external interrupt wired up at all.
 */

/* Must match the core clock main.c actually configures (26MHz HFXTAL --
 * see MCU_ClockAndUartInit() in main.c). */
#define SYSTICK_CLK_FREQ_HZ 26000000u

#define AD5940_CS_PORT   GPIO1
#define AD5940_CS_PIN    10u  /* P1.10 */
#define AD5940_RST_PORT  GPIO2
#define AD5940_RST_PIN   6u   /* P2.6 */

void AD5940_CsClr(void) { AD5940_CS_PORT->CLR = (uint16_t)(1u << AD5940_CS_PIN); }
void AD5940_CsSet(void) { AD5940_CS_PORT->SET = (uint16_t)(1u << AD5940_CS_PIN); }
void AD5940_RstClr(void) { AD5940_RST_PORT->CLR = (uint16_t)(1u << AD5940_RST_PIN); }
void AD5940_RstSet(void) { AD5940_RST_PORT->SET = (uint16_t)(1u << AD5940_RST_PIN); }

void AD5940_Delay10us(uint32_t time)
{
    if (time == 0) return;
    /* SysTick is 24-bit: split delays that would overflow it in half. */
    if (time * 10 < SYSTICK_MAXCOUNT / (SYSTICK_CLK_FREQ_HZ / 1000000)) {
        SYSTICK->LOAD = time * 10 * (SYSTICK_CLK_FREQ_HZ / 1000000);
        SYSTICK->VAL = 0;
        SYSTICK->CTRL = SYSTICK_CTRL_CLKSOURCE | SYSTICK_CTRL_ENABLE;
        while ((SYSTICK->CTRL & SYSTICK_CTRL_COUNTFLAG) == 0) { }
        SYSTICK->CTRL = 0;
    } else {
        AD5940_Delay10us(time / 2);
        AD5940_Delay10us(time / 2 + (time & 1));
    }
}

/* Set if AD5940_ReadWriteNBytes ever has to give up waiting on the AD5940
 * (see below) -- main.c checks this right after the first chip-ID read to
 * report a clear pass/fail on the UART instead of just hanging silently,
 * which is all a genuinely unresponsive AD5940 would otherwise do. */
volatile int AD5940_SPITimedOut = 0;

/* Generous margin: a working transfer completes in well under 100us at
 * this SPI clock. This just bounds how long a *dead* bus hangs for. */
#define SPI_TIMEOUT_POLLS 1000000u

void AD5940_ReadWriteNBytes(unsigned char *pSendBuffer, unsigned char *pRecvBuff, unsigned long length)
{
    uint32_t tx_count = 0, rx_count = 0;
    uint32_t timeout = SPI_TIMEOUT_POLLS;

    SPI0->CNT = (uint16_t)length;
    while ((tx_count < length || rx_count < length) && timeout > 0) {
        uint16_t fifo_sta = SPI0->FIFO_STAT;
        if (rx_count < length && (fifo_sta & SPI_FIFO_STAT_RXBYTES_MASK)) {
            *pRecvBuff++ = (unsigned char)SPI0->RX;
            rx_count++;
        }
        if (tx_count < length && (fifo_sta & SPI_FIFO_STAT_TXBYTES_MASK) < SPI_FIFO_DEPTH_BYTES) {
            SPI0->TX = *pSendBuffer++;
            tx_count++;
        }
        timeout--;
    }
    if (timeout == 0) {
        AD5940_SPITimedOut = 1;
        return; /* leave whatever was received in *pRecvBuff; don't wait on STAT either */
    }
    while ((SPI0->STAT & SPI_STAT_XFRDONE) == 0) { } /* wait for transfer done */
}

/* Always "ready to check": main.c polls AppBIOZISR() in a tight loop rather
 * than waiting on a real MCU-side interrupt (see file comment above). */
uint32_t AD5940_GetMCUIntFlag(void) { return 1; }
uint32_t AD5940_ClrMCUIntFlag(void) { return 1; }

uint32_t AD5940_MCUResourceInit(void *pCfg)
{
    (void)pCfg;

    /* P0.0 = SCLK, P0.1 = MOSI, P0.2 = MISO: mux function 1 (SPI0), 2
     * bits/pin at bit position pin*2. */
    GPIO0->CFG = (GPIO0->CFG & ~((3u << 0) | (3u << 2) | (3u << 4))) |
                 (1u << 0) | (1u << 2) | (1u << 4);

    /* CS: plain GPIO output, idle high (not selected) before SPI is used. */
    AD5940_CS_PORT->CFG &= ~(3u << (AD5940_CS_PIN * 2));
    AD5940_CS_PORT->OEN |= (uint16_t)(1u << AD5940_CS_PIN);
    AD5940_CsSet();

    /* RESET: plain GPIO output, idle high (not in reset). */
    AD5940_RST_PORT->CFG &= ~(3u << (AD5940_RST_PIN * 2));
    AD5940_RST_PORT->OEN |= (uint16_t)(1u << AD5940_RST_PIN);
    AD5940_RstSet();

    /* Baud = PCLK/(2*(DIV+1)); DIV=0 -> PCLK/2 (13MHz off the 26MHz PCLK
     * this board's MCU clock init leaves running, well inside the AD5940's
     * 20MHz max SPI clock). SCLK idles low, data sampled on the trailing
     * edge, MSB first -- AD5940's own SPI mode. */
    SPI0->DIV = 0;
    SPI0->CTL = SPI_CTL_CSRST |  /* reset SPI state machine on a CS glitch */
                SPI_CTL_MASEN |  /* master mode */
                SPI_CTL_OEN   |  /* MISO driven normally, not open-drain */
                SPI_CTL_RXOF  |  /* overwrite (don't stall) on RX overflow */
                SPI_CTL_TIM   |  /* start transfer on write to TX */
                SPI_CTL_SPIEN;   /* enable the block */
    SPI0->CNT = 1;

    return 0;
}
