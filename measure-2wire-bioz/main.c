#include "ad5940.h"
#include "bioz_2wire.h"
#include "registers.h"
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
 * its own AGPIOs. Adapted from ADI's AD5940Main.c reference (AD5940_BIOZ-2Wire
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

/* Measurement parameters for this specific setup: AD5940-BIOZ shield on the
 * ADICUP3029, with the AD5940 Z test board plugged into the shield's 2-wire
 * (CE0/AIN1) header (see the CE0/AIN1 switch selection below for why AIN1,
 * not the EVAL-AD5940 user guide's EDA-table AIN2). RcalVal is straight
 * from the EVAL-AD5940 user guide's Body Impedance table, which matches
 * this exact hardware combination -- not tuned by trial and error.
 * NumOfData is set to a single sweep's worth of points so each 'start'
 * does exactly one pass over whatever resistor is currently selected on
 * the Z test board's S1 bank, then returns to the prompt -- switch the S1
 * bank between runs without needing to reflash or reset. */
static void BIOZStructInit(void) {
  AppBIOZCfg_Type *cfg;
  AppBIOZGetCfg(&cfg);

  cfg->SeqStartAddr = 0;
  cfg->MaxSeqLen = 512;

  cfg->RcalVal = 10000.0f; /* 10kOhm RCAL on the EVAL-AD5940 board */
  /* 1kOhm, not 200Ohm: matches the EVAL-AD5940 user guide's own Body
   * Impedance validation table (Table 2) and AN-1557's worked example for
   * this same ~1-2kOhm impedance range -- RTIA needs to be in the same
   * ballpark as the impedance under test for good TIA output swing. */
  cfg->HstiaRtiaSel = HSTIARTIA_1K;

  /* 2-wire: CE0 drives the excitation, AIN1 senses it back through the
   * impedance test board. AN-1557's own 2-wire theory section explicitly
   * connects Z_UNKNOWN between CE0 and AIN1 -- AIN2 (the prior value here)
   * was only ever confirmed against the EDA table's "S+ to AIN2" note,
   * a different measurement mode, not actually verified for 2-wire. */
  cfg->DswitchSel = SWD_CE0;
  cfg->PswitchSel = SWP_CE0;
  cfg->NswitchSel = SWN_AIN1;
  cfg->TswitchSel = SWN_AIN1;

  /* Frequency sweep across the impedance test board's range. */
  cfg->SweepCfg.SweepEn = bTRUE;
  cfg->SweepCfg.SweepStart = 1000.0f;  /* 1kHz */
  cfg->SweepCfg.SweepStop = 200000.0f; /* 200kHz */
  cfg->SweepCfg.SweepPoints = 40;
  cfg->SweepCfg.SweepLog = bFALSE;

  cfg->BIOZODR = 5.0f; /* 5Hz output data rate */
  cfg->NumOfData = cfg->SweepCfg.SweepPoints; /* one sweep pass, then stop */
}

/* AN-1557's own "Measurement Results" section (p.9) is explicit that a raw
 * 2-wire reading is the impedance under test PLUS the contact impedance,
 * the current limiting resistors (RLIMIT1/RLIMIT2, ~1kOhm each on this
 * board), and the isolation capacitors -- not the unknown resistor alone.
 * The AD5940's own self-calibration (AppBIOZRtiaCal) only calibrates the
 * internal RTIA against RCAL; it has no idea those external fixed
 * components exist. Capturing one sweep with a known-zero external
 * impedance (all S1 bank switches closed) and subtracting it, per
 * frequency point, from every later sweep removes that fixed offset. */
#define SWEEP_POINTS_MAX 40
static fImpCar_Type zero_baseline[SWEEP_POINTS_MAX];
static int have_zero_baseline = 0;

/* capture: bTRUE stores each point into zero_baseline[] instead of
 * printing (used by the 'zero' command). bFALSE prints each point,
 * subtracting zero_baseline[] first if a baseline has been captured.
 * use_baseline_index: whether *pIndex actually lines up with the fixed
 * 1kHz-200kHz/40-point grid the baseline was captured against -- false
 * for single 'freq' readings, since an arbitrary frequency has no
 * corresponding baseline entry (index 0 there is the baseline's own
 * 1kHz point, not whatever frequency was actually requested). */
static void ProcessSweep(uint32_t *pData, uint32_t DataCount, uint32_t *pIndex,
                          int capture, int use_baseline_index) {
  fImpCar_Type *pImp = (fImpCar_Type *)pData;
  float freq;
  int apply_baseline = have_zero_baseline && use_baseline_index;

  AppBIOZCtrl(BIOZCTRL_GETFREQ, &freq);
  for (uint32_t i = 0; i < DataCount && *pIndex < SWEEP_POINTS_MAX; i++, (*pIndex)++) {
    if (capture) {
      zero_baseline[*pIndex] = pImp[i];
      continue;
    }

    fImpCar_Type z = pImp[i];
    if (apply_baseline) {
      z.Real -= zero_baseline[*pIndex].Real;
      z.Image -= zero_baseline[*pIndex].Image;
    }
    float mag = AD5940_ComplexMag(&z);
    float phase_deg = AD5940_ComplexPhase(&z) * 180.0f / MATH_PI;
    printf("freq=%.1fHz Z=(%.2f,%.2f)ohm |Z|=%.2fohm phase=%.2fdeg%s\n", freq,
           z.Real, z.Image, mag, phase_deg,
           apply_baseline ? "" : " (uncalibrated -- run 'zero' first)");
  }
}

static uint32_t app_buff[512];

/* Runs one full init+measure+stop cycle. capture/is_single/single_freq
 * configure it exactly like a 'zero'/'start'/'freq' command would; silent
 * suppresses all per-point output (used only for the boot warm-up below).
 * Returns the number of points actually measured. */
static uint32_t RunMeasurement(AppBIOZCfg_Type *cfg, int capture, int is_single,
                                float single_freq, int silent) {
  uint32_t count;

  /* BIOZStructInit() sets the full 1kHz-200kHz/40-point sweep; a single
   * 'freq' reading overrides just the sweep-related fields on top of
   * that, so 'start'/'zero' always restore the full sweep first. This has
   * to re-run AppBIOZInit() (not just tweak cfg) because the sweep vs.
   * single-frequency choice determines what sequence commands and RTIA
   * calibration table AppBIOZInit() bakes in -- ReDoRtiaCal and
   * bParaChanged force it to actually regenerate both instead of silently
   * reusing whatever the previous command configured. */
  BIOZStructInit();
  if (is_single) {
    cfg->SweepCfg.SweepEn = bFALSE;
    cfg->SinFreq = single_freq;
    cfg->NumOfData = 1;
  }
  cfg->ReDoRtiaCal = bTRUE;
  cfg->bParaChanged = bTRUE;
  AppBIOZInit(app_buff, sizeof(app_buff) / sizeof(app_buff[0]));

  uint32_t sweep_points = (uint32_t)cfg->NumOfData;
  AppBIOZCtrl(BIOZCTRL_START, 0);

  uint32_t received = 0;
  uint32_t index = 0;
  while (received < sweep_points) {
    count = sizeof(app_buff) / sizeof(app_buff[0]);
    AppBIOZISR(app_buff, &count);
    if (count > 0) {
      if (!silent) {
        ProcessSweep(app_buff, count, &index, capture, !is_single);
      }
      received += count;
    }
  }
  AppBIOZCtrl(BIOZCTRL_STOPNOW, 0);

  if (capture && !silent) {
    have_zero_baseline = 1;
    printf("Zero calibration captured across %lu points.\n",
           (unsigned long)index);
  }
  return received;
}

int main(void) {
  MCU_ClockAndUartInit();
  printf("measure-2wire-bioz build %s %s\n", __DATE__, __TIME__);

  /* Configures SPI0 (pin mux, CTL/DIV) and the CS/RESET GPIO output
     * drivers. Without this, SPI0 is left at its power-on-reset state
     * (disabled) and every transfer times out regardless of wiring. */
  AD5940_MCUResourceInit(0);

  AD5940_HWReset();
  AD5940PlatformCfg();

  AppBIOZCfg_Type *cfg;
  AppBIOZGetCfg(&cfg);

  /* Harmless leftover from investigating the 'freq' bug below: doesn't
   * fix it (see the NumOfData=1 comment), but doesn't hurt either. */
  RunMeasurement(cfg, 0, 0, 0.0f, 1);

  char line[32];
  for (;;) {
    printf("Close all S1 switches (0Ohm) and type 'zero' to calibrate out "
           "RLIMIT/isolation/contact offset, or set a switch and type "
           "'start' for a full sweep.\n");
    UartReadLine(line, sizeof(line));

    int capture = 0;
    if (strcmp(line, "zero") == 0) {
      capture = 1;
    } else if (strcmp(line, "start") == 0) {
      capture = 0;
    } else if (strncmp(line, "freq ", 5) == 0) {
      /* KNOWN BROKEN: single-point (NumOfData=1) measurements return
       * wildly wrong results (millions of ohms) in every variant tried --
       * SweepEn=bFALSE, and a SweepEn=bTRUE 2-point sweep truncated to 1
       * point via NumOfData, both with and without a prior warm-up cycle.
       * Root cause not found; 'start'/'zero' (NumOfData=40) are unaffected
       * and thoroughly tested. Refusing to run this rather than silently
       * printing wrong numbers. */
      printf("'freq' is disabled -- single-point measurements return "
             "wrong results (unresolved bug). Use 'start' instead.\n");
      continue;
    } else {
      continue;
    }

    RunMeasurement(cfg, capture, 0, 0.0f, 0);
  }
}
