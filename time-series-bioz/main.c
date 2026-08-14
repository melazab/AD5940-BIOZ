#include "ad5940.h"
#include "BodyImpedance.h"
#include "registers.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ADuCM3029 MCU bring-up: disable the watchdog, switch the system clock to
 * the board's 26MHz crystal (matches ADI's own ADICUP3029 reference --
 * confirmed by AD5940_Delay10us()'s SYSTICK_CLK_FREQ_HZ needing to agree
 * with whatever this leaves running), and bring up UART0 for printf().
 * This is entirely separate from the AD5940's own internal clock, which
 * AD5940PlatformCfg() below configures over SPI on the AFE itself. */
static void MCU_ClockAndUartInit(void) {
  WDT0->CTL &= (uint16_t)~WDT_CTL_EN;

  CLKG0_OSC->KEY = CLKG_OSC_KEY_UNLOCK;
  CLKG0_OSC->CTL = CLKG_OSC_CTL_HFOSCEN | CLKG_OSC_CTL_HFXTALEN;
  while ((CLKG0_OSC->CTL & CLKG_OSC_CTL_HFXTALOK) == 0) {
  }

  CLKG0_OSC->KEY = CLKG_OSC_KEY_UNLOCK;
  CLKG0_CLK->CTL0 = CLKG_CLK_CTL0_CLKMUX_XTAL | CLKG_CLK_CTL0_HFXTAL26M;
  CLKG0_CLK->CTL1 = 0; /* ACLK/PCLK/HCLK all divided by 1 */
  CLKG0_CLK->CTL5 = 0; /* gate no peripheral clocks */

  /* P0.10/P0.11 -> UART0 (mux function 1). */
  GPIO0->CFG =
      (GPIO0->CFG & ~((3u << 20) | (3u << 22))) | (1u << 20) | (1u << 22);

  /* 230400 8N1, root clock 26MHz, oversample rate 32 (matches ADI's
     * UrtCfg(): i1 = ullRtClk/(iOSR*iDiv)/iBaud - 1, then a fractional
     * correction term in FBR so the actual rate lands closer to nominal). */
  {
    const uint32_t baud = 230400u;
    const uint32_t osr = 32u;
    const unsigned long long root_clk = 26000000ull;
    uint32_t pclk_div = (CLKG0_CLK->CTL1 & CLKG_CLK_CTL1_PCLKDIVCNT_MASK) >>
                        CLKG_CLK_CTL1_PCLKDIVCNT_SHIFT;
    uint32_t div;

    if (pclk_div == 0)
      pclk_div = 1;

    UART0->LCR2 = 0x3; /* OSR = 32 */
    div = (uint32_t)((root_clk / (osr * pclk_div)) / baud) - 1;
    UART0->DIV = (uint16_t)div;
    UART0->FBR =
        (uint16_t)(0x8800u |
                   (((((2048u / (osr * pclk_div)) * root_clk) / div) / baud) -
                    2048u));

    UART0->LCR = UART_LCR_WLEN_8;
    UART0->FCR = UART_FCR_FIFOEN;
    UART0->FCR |= UART_FCR_RFCLR | UART_FCR_TFCLR;
    UART0->FCR &= (uint16_t)~(UART_FCR_RFCLR | UART_FCR_TFCLR);
  }
}

static void UartPutc(char c) {
  while ((UART0->LSR & UART_LSR_THRE) == 0) {
  }
  UART0->TX = (uint16_t)c;
}

int _write(int file, char *ptr, int len) {
  (void)file;
  for (int i = 0; i < len; i++) {
    /* Writing straight to the UART register bypasses the tty layer
         * that would normally translate LF to CR+LF for a raw terminal;
         * without this, a serial terminal moves down a line without
         * returning to column 0, and everything drifts rightward. */
    if (ptr[i] == '\n') {
      UartPutc('\r');
    }
    UartPutc(ptr[i]);
  }
  return len;
}

/* Blocks until a line (terminated by '\r' or '\n', either one -- picocom's
 * local Enter key sends '\r') is typed over UART0, echoing each character
 * back so it's visible in the terminal. Used to gate the measurement loop
 * behind a "start" command instead of it running unattended from boot. */
static void UartReadLine(char *buf, unsigned bufsize) {
  unsigned i = 0;

  for (;;) {
    while ((UART0->LSR & UART_LSR_DR) == 0) {
    }
    char c = (char)UART0->RX;

    if (c == '\r' || c == '\n') {
      putchar('\n');
      break;
    }
    if (i < bufsize - 1) {
      buf[i++] = c;
      putchar(c);
    }
  }
  buf[i] = '\0';
}

/* Non-blocking sibling of UartReadLine, for checking whether 'stop' was
 * typed without blocking the measurement loop while waiting on it. Called
 * once per loop iteration; the buffer index persists across calls (static)
 * so a line can accumulate a few bytes at a time across many calls instead
 * of requiring one whole line to already be waiting. Returns 1 once a full
 * line is available (buf null-terminated), 0 otherwise. */
static int UartPollLine(char *buf, unsigned bufsize) {
  static unsigned i = 0;

  while ((UART0->LSR & UART_LSR_DR) != 0) {
    char c = (char)UART0->RX;
    if (c == '\r' || c == '\n') {
      putchar('\n');
      buf[i] = '\0';
      i = 0;
      return 1;
    }
    if (i < bufsize - 1) {
      buf[i++] = c;
      putchar(c);
    }
  }
  return 0;
}

/* Initialize AD5940 basic blocks like clock, FIFO, interrupt controller and
 * its own AGPIOs. Adapted from ADI's AD5940Main.c reference (AD5940_BIA
 * example) -- unchanged except GP0_INT is left unconfigured, since nothing
 * here wires the AD5940's interrupt pin to an MCU GPIO (see ad5940_port.c). */
static void AD5940PlatformCfg(void) {
  CLKCfg_Type clk_cfg;
  FIFOCfg_Type fifo_cfg;
  AGPIOCfg_Type gpio_cfg;

  AD5940_HWReset();
  AD5940_Initialize();

  clk_cfg.ADCClkDiv = ADCCLKDIV_1;
  clk_cfg.ADCCLkSrc = ADCCLKSRC_XTAL;
  clk_cfg.SysClkDiv = SYSCLKDIV_1;
  clk_cfg.SysClkSrc = SYSCLKSRC_XTAL;
  clk_cfg.HfOSC32MHzMode = bFALSE;
  clk_cfg.HFOSCEn = bFALSE;
  clk_cfg.HFXTALEn = bTRUE;
  clk_cfg.LFOSCEn = bTRUE;
  AD5940_CLKCfg(&clk_cfg);

  fifo_cfg.FIFOEn = bFALSE;
  fifo_cfg.FIFOMode = FIFOMODE_FIFO;
  fifo_cfg.FIFOSize = FIFOSIZE_4KB;
  fifo_cfg.FIFOSrc = FIFOSRC_DFT;
  fifo_cfg.FIFOThresh = 4;
  AD5940_FIFOCfg(&fifo_cfg); /* disable first, to reset the FIFO */
  fifo_cfg.FIFOEn = bTRUE;
  AD5940_FIFOCfg(&fifo_cfg);

  AD5940_INTCCfg(AFEINTC_1, AFEINTSRC_ALLINT, bTRUE);
  AD5940_INTCCfg(AFEINTC_0, AFEINTSRC_DATAFIFOTHRESH, bTRUE);
  AD5940_INTCClrFlag(AFEINTSRC_ALLINT);

  gpio_cfg.FuncSet =
      GP6_SYNC | GP5_SYNC | GP4_SYNC | GP2_TRIG | GP1_SYNC | GP0_INT;
  gpio_cfg.InputEnSet = AGPIO_Pin2;
  gpio_cfg.OutputEnSet =
      AGPIO_Pin0 | AGPIO_Pin1 | AGPIO_Pin4 | AGPIO_Pin5 | AGPIO_Pin6;
  gpio_cfg.OutVal = 0;
  gpio_cfg.PullEnSet = 0;
  AD5940_AGPIOCfg(&gpio_cfg);

  AD5940_SleepKeyCtrlS(SLPKEY_UNLOCK);
}

/* Measurement parameters for this setup: AD5940-BIOZ shield on the
 * ADICUP3029, with the AD5940 Z test board (or a real sensor) plugged into
 * the shield's 4-wire (F+/S+/F-/S-) header. RcalVal/HstiaRtiaSel match
 * ADI's AD5940BIAStructInit() reference (AD5940_BIA example) exactly.
 * Unlike measure-4wire-bioz's frequency sweep, this runs continuously
 * (NumOfData=-1) at one fixed frequency, streaming one impedance sample
 * every 1/BiaODR seconds indefinitely -- a time-series recorder, not a
 * sweep. 4-wire only for now (true Kelvin sensing: the current-carrying
 * path (CE0/AIN1, via RLIMIT1/RLIMIT2) and voltage-sense path (AIN3/AIN2)
 * are physically separate, so RLIMIT's voltage drop isn't measured, unlike
 * 2-wire); a 2-wire option can be added later by porting bioz_2wire.c's
 * equivalent struct/switch config the way measure-2wire-bioz already does. */
static void TimeSeriesStructInit(float freq_hz) {
  AppBIACfg_Type *cfg;
  AppBIAGetCfg(&cfg);

  cfg->SeqStartAddr = 0;
  cfg->MaxSeqLen = 512;

  cfg->RcalVal = 10000.0f; /* 10kOhm RCAL on the EVAL-AD5940 board */
  cfg->HstiaRtiaSel = HSTIARTIA_1K; /* matches ADI's AD5940BIAStructInit */

  /* ad5940.h documents AFEPWR_LP as only valid "for signal <80kHz" (see
   * measure-4wire-bioz's BIAStructInit for how this was found -- the same
   * default LP/16MHz config produced a garbage reading at 200kHz there).
   * Here the frequency is fixed and known up front, so pick the mode that
   * actually matches it instead of forcing HP unconditionally. */
  if (freq_hz > 80000.0f) {
    cfg->PwrMod = AFEPWR_HP;
    cfg->SysClkFreq = 32000000.0f;
    cfg->AdcClkFreq = 32000000.0f;
  } else {
    cfg->PwrMod = AFEPWR_LP;
    cfg->SysClkFreq = 16000000.0f;
    cfg->AdcClkFreq = 16000000.0f;
  }

  cfg->SweepCfg.SweepEn = bFALSE;
  cfg->SinFreq = freq_hz;

  cfg->BiaODR = 5.0f; /* 5Hz: one sample every 200ms */
  cfg->NumOfData = -1; /* run until 'stop' -- no sweep, no natural end */

  /* Forces AppBIAInit() to actually redo RTIA calibration and regenerate
   * the sequence for the new frequency, rather than silently reusing
   * whatever a previous 'start' configured -- needed now that 'start' can
   * run more than once per boot (see the stop/restart loop in main()). */
  cfg->ReDoRtiaCal = bTRUE;
  cfg->bParaChanged = bTRUE;
}

static void PrintSample(uint32_t *pData, uint32_t DataCount, uint32_t *pSampleNum) {
  fImpPol_Type *pImp = (fImpPol_Type *)pData;
  float freq;

  AppBIACtrl(BIACTRL_GETFREQ, &freq);
  for (uint32_t i = 0; i < DataCount; i++, (*pSampleNum)++) {
    float mag = pImp[i].Magnitude;
    float phase_deg = pImp[i].Phase * 180.0f / MATH_PI;
    float real = mag * cosf(pImp[i].Phase);
    float image = mag * sinf(pImp[i].Phase);
    printf("sample=%lu freq=%.1fHz Z=(%.2f,%.2f)ohm |Z|=%.2fohm phase=%.2fdeg\n",
           (unsigned long)*pSampleNum, freq, real, image, mag, phase_deg);
  }
}

int main(void) {
  static uint32_t app_buff[512];
  uint32_t count;

  MCU_ClockAndUartInit();
  printf("time-series-bioz build %s %s\n", __DATE__, __TIME__);

  /* Configures SPI0 (pin mux, CTL/DIV) and the CS/RESET GPIO output
     * drivers. Without this, SPI0 is left at its power-on-reset state
     * (disabled) and every transfer times out regardless of wiring. */
  AD5940_MCUResourceInit(0);

  char line[16];
  for (;;) {
    float freq_hz = 0.0f;
    for (;;) {
      printf("Type 'start <Hz>' to begin continuous 4-wire measurement at "
             "that frequency (e.g. 'start 50000'). Type 'stop' once "
             "running to end it and pick a new frequency.\n");
      UartReadLine(line, sizeof(line));
      if (sscanf(line, "start %f", &freq_hz) == 1 && freq_hz > 0.0f) {
        break;
      }
    }

    /* Full hardware reset before every run (not just at boot): RTIA
     * calibration below runs on every 'start' since the frequency can
     * change, and calibrating against a chip left in whatever analog state
     * the previous run's measurements left it in (rather than a clean,
     * freshly-reset state) was producing a baseline offset on the second
     * and later runs despite the first run after flashing being correct. */
    AD5940_HWReset();
    AD5940PlatformCfg();

    TimeSeriesStructInit(freq_hz);
    AppBIAInit(app_buff, sizeof(app_buff) / sizeof(app_buff[0]));
    AppBIACtrl(BIACTRL_START, 0);

    uint32_t sample_num = 0; /* starts back at 0 on every fresh 'start' */
    for (;;) {
      count = sizeof(app_buff) / sizeof(app_buff[0]);
      AppBIAISR(app_buff, &count);
      if (count > 0) {
        PrintSample(app_buff, count, &sample_num);
      }
      if (UartPollLine(line, sizeof(line)) && strcmp(line, "stop") == 0) {
        AppBIACtrl(BIACTRL_STOPNOW, 0);
        printf("Stopped.\n");
        break;
      }
    }
  }
}
