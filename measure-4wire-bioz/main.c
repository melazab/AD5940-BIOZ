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
 * ADICUP3029, with the AD5940 Z test board plugged into the shield's 4-wire
 * (F+/S+/F-/S-) header. Adapted from ADI's AD5940BIAStructInit() reference
 * (AD5940_BIA example) -- RcalVal/HstiaRtiaSel match it exactly; the sweep
 * range/NumOfData are our own choice (single pass over whatever's on the Z
 * test board's S1 bank, matching the measure-2wire-bioz workflow), not
 * ADI's default (continuous single-frequency 50kHz). Unlike 2-wire, the
 * current-carrying path (CE0/AIN1, via RLIMIT1/RLIMIT2) and the
 * voltage-sense path (AIN3/AIN2, wired directly in BodyImpedance.c's
 * AppBIASeqMeasureGen) are physically separate -- the sense leads carry
 * negligible current, so RLIMIT1/RLIMIT2's voltage drop doesn't get
 * measured. That's true Kelvin sensing, not something this struct
 * configures; see BodyImpedance.c for the actual switch/mux sequence. */
static void BIAStructInit(void) {
  AppBIACfg_Type *cfg;
  AppBIAGetCfg(&cfg);

  cfg->SeqStartAddr = 0;
  cfg->MaxSeqLen = 512;

  cfg->RcalVal = 10000.0f; /* 10kOhm RCAL on the EVAL-AD5940 board */
  cfg->HstiaRtiaSel = HSTIARTIA_1K; /* matches ADI's AD5940BIAStructInit */

  /* ad5940.h documents AFEPWR_LP as only valid "for signal <80kHz" -- the
   * struct's compiled-in default (AFEPWR_LP @ 16MHz) is what caused the
   * garbage 200kHz reading (and the RTIA-cal debug print's same outlier at
   * 200kHz): our sweep runs to 200kHz, well past LP's documented ceiling.
   * BodyImpedance.c uses one fixed power mode for the whole sweep (unlike
   * bioz_2wire.c, which adaptively switches HP/LP per point), so this has
   * to cover the whole 1kHz-200kHz range -- HP mode remains valid at the
   * low end too, it just costs more power, which doesn't matter here. */
  cfg->PwrMod = AFEPWR_HP;
  cfg->SysClkFreq = 32000000.0f;
  cfg->AdcClkFreq = 32000000.0f;

  /* Frequency sweep across the impedance test board's range. */
  cfg->SweepCfg.SweepEn = bTRUE;
  cfg->SweepCfg.SweepStart = 1000.0f;  /* 1kHz */
  cfg->SweepCfg.SweepStop = 200000.0f; /* 200kHz */
  cfg->SweepCfg.SweepPoints = 40;
  cfg->SweepCfg.SweepLog = bFALSE;

  cfg->BiaODR = 5.0f; /* 5Hz output data rate */
  cfg->NumOfData = cfg->SweepCfg.SweepPoints; /* one sweep pass, then stop */
}

/* True 4-wire (Kelvin) sensing shouldn't need the RLIMIT/isolation-cap
 * offset correction measure-2wire-bioz needs -- the voltage sense leads
 * (AIN3/AIN2) carry negligible current, so RLIMIT1/RLIMIT2's drop is
 * excluded by construction, not subtracted in software. Still keeping a
 * 'zero' capture/subtract path here (same technique, just on the
 * fImpPol_Type polar output instead of 2-wire's fImpCar_Type cartesian
 * one) as a sanity check -- if 4-wire is working, the captured baseline
 * should come out close to zero, and 'start' results shouldn't move much
 * whether or not 'zero' was run first. A real, non-negligible baseline
 * here would mean S2/S4 (contact/lead impedance) aren't fully zeroed, or
 * a wiring issue, not the RLIMIT effect 2-wire has. */
#define SWEEP_POINTS_MAX 40
typedef struct {
  float Real;
  float Image;
} Cartesian;
static Cartesian zero_baseline[SWEEP_POINTS_MAX];
static int have_zero_baseline = 0;

/* capture: bTRUE stores each point into zero_baseline[] instead of
 * printing (used by the 'zero' command). bFALSE prints each point,
 * subtracting zero_baseline[] first if a baseline has been captured. */
static void ProcessSweep(uint32_t *pData, uint32_t DataCount, uint32_t *pIndex,
                          int capture) {
  fImpPol_Type *pImp = (fImpPol_Type *)pData;
  float freq;

  AppBIACtrl(BIACTRL_GETFREQ, &freq);
  for (uint32_t i = 0; i < DataCount && *pIndex < SWEEP_POINTS_MAX; i++, (*pIndex)++) {
    /* AD5940_ComplexPhase() (2-wire) uses atan2(Image, Real) with no sign
     * flip, so this is the matching polar->cartesian conversion. */
    Cartesian z;
    z.Real = pImp[i].Magnitude * cosf(pImp[i].Phase);
    z.Image = pImp[i].Magnitude * sinf(pImp[i].Phase);

    if (capture) {
      zero_baseline[*pIndex] = z;
      continue;
    }

    if (have_zero_baseline) {
      z.Real -= zero_baseline[*pIndex].Real;
      z.Image -= zero_baseline[*pIndex].Image;
    }
    float mag = sqrtf(z.Real * z.Real + z.Image * z.Image);
    float phase_deg = atan2f(z.Image, z.Real) * 180.0f / MATH_PI;
    printf("freq=%.1fHz Z=(%.2f,%.2f)ohm |Z|=%.2fohm phase=%.2fdeg%s\n", freq,
           z.Real, z.Image, mag, phase_deg,
           have_zero_baseline ? "" : " (uncalibrated -- run 'zero' first)");
  }
}

int main(void) {
  static uint32_t app_buff[512];
  uint32_t count;

  MCU_ClockAndUartInit();
  printf("measure-4wire-bioz build %s %s\n", __DATE__, __TIME__);

  /* Configures SPI0 (pin mux, CTL/DIV) and the CS/RESET GPIO output
     * drivers. Without this, SPI0 is left at its power-on-reset state
     * (disabled) and every transfer times out regardless of wiring. */
  AD5940_MCUResourceInit(0);

  AD5940_HWReset();
  AD5940PlatformCfg();
  BIAStructInit();

  /* Runs the RTIA self-calibration once (against the onboard RCAL
   * resistor) and prepares the sequencer. Not repeated per 'start' --
   * only the external Z_UNKNOWN sweep needs to rerun when the Z test
   * board's S1 bank switch changes, not the internal RTIA calibration. */
  AppBIAInit(app_buff, sizeof(app_buff) / sizeof(app_buff[0]));

  char line[16];
  uint32_t sweep_points = 0;
  AppBIACfg_Type *cfg;
  AppBIAGetCfg(&cfg);
  sweep_points = (uint32_t)cfg->NumOfData;

  for (;;) {
    printf("Close all S1 switches (0Ohm) and type 'zero' to sanity-check the "
           "offset (should be near-zero for true 4-wire), or set a switch "
           "and type 'start' for one sweep pass over it.\n");
    UartReadLine(line, sizeof(line));
    int capture;
    if (strcmp(line, "zero") == 0) {
      capture = 1;
    } else if (strcmp(line, "start") == 0) {
      capture = 0;
    } else {
      continue;
    }

    AppBIACtrl(BIACTRL_START, 0);

    uint32_t received = 0;
    uint32_t index = 0;
    while (received < sweep_points) {
      count = sizeof(app_buff) / sizeof(app_buff[0]);
      AppBIAISR(app_buff, &count);
      if (count > 0) {
        ProcessSweep(app_buff, count, &index, capture);
        received += count;
      }
    }
    AppBIACtrl(BIACTRL_STOPNOW, 0);

    if (capture) {
      have_zero_baseline = 1;
      printf("Zero calibration captured across %lu points.\n",
             (unsigned long)index);
    }
  }
}
