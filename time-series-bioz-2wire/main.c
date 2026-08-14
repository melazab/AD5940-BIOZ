#include "ad5940.h"
#include "bioz_2wire.h"
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

/* Measurement parameters for this setup: AD5940-BIOZ shield on the
 * ADICUP3029, with the AD5940 Z test board (or a real sensor) plugged into
 * the shield's 2-wire (CE0/AIN1) header. RcalVal/HstiaRtiaSel/switch
 * selection match measure-2wire-bioz's BIOZStructInit exactly -- see that
 * file for why CE0/AIN1 (not AIN2) and HSTIARTIA_1K. Unlike
 * measure-2wire-bioz's frequency sweep, this runs continuously
 * (NumOfData=-1) at one fixed frequency, streaming one impedance sample
 * every 1/BIOZODR seconds indefinitely -- a time-series recorder, not a
 * sweep, mirroring time-series-bioz's 4-wire version. Unlike that 4-wire
 * version, PwrMod/SysClkFreq/AdcClkFreq are left untouched here: bioz_2wire.c's
 * own AppBIOZCheckFreq() (called from AppBIOZSeqCfgGen) already switches
 * HP/LP mode per frequency internally, which BodyImpedance.c (4-wire) does
 * not do -- so no manual >80kHz check is needed on this path.
 *
 * Validated on hardware at 50kHz: a 20kOhm resistor reads ~22kOhm, which is
 * exactly what's expected once you account for the on-board RLIMIT/coupling
 * network (R43+C68 on the F+ side, R19+C1 on the F- side -- see
 * Schematic_EVAL-AD5940BIOZ.pdf's ELECTRODES block) sitting in series with
 * whatever's plugged into the 2-wire header, roughly 2kOhm at 50kHz. That
 * fixed offset is what the 'zero'/apply_baseline machinery below subtracts
 * out. Not yet checked at every frequency -- in particular, a separate sweep
 * test (measure-2wire-bioz's 'start') showed a glitch right around the
 * AppBIOZCheckFreq() HP/LP mode switchover (~50-60kHz); whether that also
 * affects this continuous path at frequencies near that boundary hasn't
 * been confirmed. */
static void TimeSeriesStructInit(float freq_hz) {
  AppBIOZCfg_Type *cfg;
  AppBIOZGetCfg(&cfg);

  cfg->SeqStartAddr = 0;
  cfg->MaxSeqLen = 512;

  cfg->RcalVal = 10000.0f; /* 10kOhm RCAL on the EVAL-AD5940 board */

  /* Raised from HSTIARTIA_1K (measure-2wire-bioz's value) to HSTIARTIA_5K.
   * V_HSTIA_out ~= I_loop * RTIA, and I_loop = V_excitation / Z_loop -- so
   * RTIA sets how much of the ADC's input range a given loop impedance
   * actually uses. 1K was undersized for this setup's real loop impedance
   * (the ~2kOhm fixed RLIMIT/coupling network alone, per the 'zero'
   * baseline, before adding any skin/tissue impedance on top), leaving ADC
   * range unused. 5K is a closer match to the observed few-kOhm loop.
   * Like the DacVoltPP bump above, this only improves signal-vs-electronic
   * noise -- it does nothing for contact-impedance artifact noise. */
  cfg->HstiaRtiaSel = HSTIARTIA_5K;

  /* bioz_2wire.c's default DacVoltPP (600mV) raised to 800mV -- the
   * documented max (bioz_2wire.h: "Maximum value is 800mVpp"). ExcitBufGain
   * (EXCITBUFGAIN_2, x2) and HsDacGain (HSDACGAIN_1, x1) are already at
   * their own max multipliers, left untouched -- 800mV is the actual
   * ceiling here (final excitation = DacVoltPP*HsDacGain*ExcitBufGain =
   * 800*1*2 = 1600mVpp, up from 1200mVpp), only a ~33% amplitude increase.
   * This raises signal amplitude relative to the AD5940's own electronic/
   * ADC noise floor; it does *not* address contact-impedance artifact
   * noise from the 2-wire electrodes themselves, which is a separate,
   * likely larger noise source for a skin-contact measurement. */
  cfg->DacVoltPP = 800.0f;

  /* 2-wire: CE0 drives the excitation, AIN1 senses it back through the
   * impedance under test (see measure-2wire-bioz/main.c's BIOZStructInit
   * for why CE0/AIN1, not AIN2). */
  cfg->DswitchSel = SWD_CE0;
  cfg->PswitchSel = SWP_CE0;
  cfg->NswitchSel = SWN_AIN1;
  cfg->TswitchSel = SWN_AIN1;

  cfg->SweepCfg.SweepEn = bFALSE;
  cfg->SinFreq = freq_hz;

  /* DFTNUM_512 (down from bioz_2wire.c's default DFTNUM_8192): each
   * measurement point runs two sequential DFTs (current, then voltage --
   * see bioz_2wire.c's AppBIOZSeqCfgGen), each taking DftNum/SincRate
   * seconds. At DFTNUM_8192 that's ~20ms/DFT in LP mode (SINC3 output rate
   * 400kHz = ADCRATE_800KHZ/ADCSINC3OSR_2), ~41ms/point total -- capping
   * the AD5940 itself at ~24 samples/sec, deliberately slower than the 5Hz
   * this used to run at. DFTNUM_512 (16x fewer points) brings that down to
   * ~1.28ms/DFT, ~3ms/point including switch-settling waits -- comfortably
   * under UART's own ceiling below.
   * Tradeoff: DFTNUM sets how many excitation cycles get coherently
   * averaged per DFT, i.e. it trades noise/SNR for speed, not "resolution"
   * in the FFT-bin sense (the excitation frequency is programmed exactly,
   * not searched for). At 50kHz, 512 SINC3-rate samples still span ~64
   * cycles -- plenty. At low excitation frequencies (a few kHz or below),
   * 512 samples span only a handful of cycles (at 1kHz, roughly 1 cycle in
   * LP mode), so expect noticeably noisier readings there than at 50kHz+;
   * this hasn't been tuned per-frequency. */
  cfg->DftNum = DFTNUM_512;

  /* 200Hz: fast as the UART link can sustain, not as fast as the AD5940
   * can run. Each printed line is ~85-90 bytes; at 230400 baud (8N1, so
   * 23040 bytes/sec) that's ~3.7-3.9ms/line -- a real ceiling around
   * 250-270 lines/sec regardless of measurement speed. 200Hz (5ms/sample)
   * leaves ~25% headroom under that ceiling for line-length growth (sample
   * numbers gain digits over a long continuous run) and normal jitter.
   * Going faster than the UART can drain risks the AD5940's FIFO backing
   * up between AppBIOZISR() polls, which resurfaces a real indexing bug in
   * bioz_2wire.c's AppBIOZDataProcess() (pairs up the wrong current/voltage
   * DFT results once more than one point's worth of data is buffered) --
   * this rate was chosen specifically to stay clear of that, not just for
   * raw speed. */
  cfg->BIOZODR = 200.0f;
  cfg->NumOfData = -1; /* run until 'stop' -- no sweep, no natural end */

  /* Forces AppBIOZInit() to actually redo RTIA calibration and regenerate
   * the sequence for the new frequency, rather than silently reusing
   * whatever a previous 'start' configured -- needed now that 'start' can
   * run more than once per boot (see the stop/restart loop in main()). */
  cfg->ReDoRtiaCal = bTRUE;
  cfg->bParaChanged = bTRUE;
}

/* 'zero <Hz>' captures a baseline (see main()) by averaging this many live
 * samples instead of trusting a single one -- smooths out ordinary
 * sample-to-sample DFT noise in the continuous stream. Raised from 10 to
 * 200 (1s at BIOZODR=200Hz, versus 2s at the old 5Hz) alongside the
 * DFTNUM_512 change above: each individual sample is noisier now (less
 * coherent averaging per DFT), so averaging more of them here buys back
 * some of that lost precision for the baseline specifically -- it's a
 * one-time capture, so there's no reason to keep it as fast as 'start'. */
#define ZERO_SAMPLES 200

/* AN-1557's own "Measurement Results" section documents that a raw 2-wire
 * reading is the impedance under test PLUS the fixed on-board RLIMIT/
 * isolation-cap network (see TimeSeriesStructInit()'s comment above) --
 * not the unknown impedance alone. zero_baseline holds one such raw reading
 * (captured with a known-zero load, e.g. a short) to subtract from every
 * later 'start' sample at the same frequency, the same technique
 * measure-2wire-bioz's 'zero' command uses per sweep point. Only one
 * baseline is kept (not a table like the sweep version) since this
 * firmware only ever runs at a single frequency at a time; a baseline
 * captured at one frequency doesn't apply to a 'start' at a different one
 * -- the isolation caps' impedance is frequency-dependent -- so
 * apply_baseline (set in main()) only turns on when the two match. */
static fImpCar_Type zero_baseline;
static float zero_baseline_freq = 0.0f;
static int have_zero_baseline = 0;

static void PrintSample(uint32_t *pData, uint32_t DataCount, uint32_t *pSampleNum,
                         float freq, int apply_baseline) {
  fImpCar_Type *pImp = (fImpCar_Type *)pData;

  for (uint32_t i = 0; i < DataCount; i++, (*pSampleNum)++) {
    fImpCar_Type z = pImp[i];
    if (apply_baseline) {
      z.Real -= zero_baseline.Real;
      z.Image -= zero_baseline.Image;
    }
    float mag = AD5940_ComplexMag(&z);
    float phase_deg = AD5940_ComplexPhase(&z) * 180.0f / MATH_PI;
    printf("sample=%lu freq=%.1fHz Z=(%.2f,%.2f)ohm |Z|=%.2fohm phase=%.2fdeg%s\n",
           (unsigned long)*pSampleNum, freq, z.Real, z.Image, mag, phase_deg,
           apply_baseline ? "" : " (uncalibrated -- run 'zero <Hz>' first)");
  }
}

int main(void) {
  static uint32_t app_buff[512];
  uint32_t count;

  MCU_ClockAndUartInit();
  printf("time-series-bioz-2wire build %s %s\n", __DATE__, __TIME__);

  /* Configures SPI0 (pin mux, CTL/DIV) and the CS/RESET GPIO output
     * drivers. Without this, SPI0 is left at its power-on-reset state
     * (disabled) and every transfer times out regardless of wiring. */
  AD5940_MCUResourceInit(0);

  char line[16];
  for (;;) {
    float freq_hz = 0.0f;
    int capture = 0; /* 1 for 'zero <Hz>', 0 for 'start <Hz>' */
    for (;;) {
      printf("Type 'zero <Hz>' with a known-zero load (e.g. a short) in "
             "place to capture the RLIMIT/isolation-cap offset at that "
             "frequency, or 'start <Hz>' to begin continuous measurement "
             "(subtracting the zero baseline if one was captured at the "
             "same frequency). Type 'stop' once running to end it and pick "
             "again.\n");
      UartReadLine(line, sizeof(line));
      if (sscanf(line, "start %f", &freq_hz) == 1 && freq_hz > 0.0f) {
        capture = 0;
        break;
      }
      if (sscanf(line, "zero %f", &freq_hz) == 1 && freq_hz > 0.0f) {
        capture = 1;
        break;
      }
    }

    /* Full hardware reset before every run (not just at boot): RTIA
     * calibration below runs on every 'start'/'zero' since the frequency
     * can change, and calibrating against a chip left in whatever analog
     * state the previous run's measurements left it in (rather than a
     * clean, freshly-reset state) was producing a baseline offset on the
     * second and later runs despite the first run after flashing being
     * correct (see time-series-bioz's identical fix for the 4-wire case). */
    AD5940_HWReset();
    AD5940PlatformCfg();

    TimeSeriesStructInit(freq_hz);
    AppBIOZInit(app_buff, sizeof(app_buff) / sizeof(app_buff[0]));
    AppBIOZCtrl(BIOZCTRL_START, 0);

    float freq_reported;
    AppBIOZCtrl(BIOZCTRL_GETFREQ, &freq_reported);

    /* Exact float equality is fine here: both freq_hz values came from
     * sscanf("%f", ...) on whatever the user typed, so a 'zero 50000'
     * followed by a 'start 50000' parse identically -- there's no
     * accumulated floating-point drift to worry about between them. */
    int apply_baseline = have_zero_baseline && (freq_hz == zero_baseline_freq);

    if (capture) {
      float sum_real = 0.0f, sum_image = 0.0f;
      uint32_t got = 0;
      while (got < ZERO_SAMPLES) {
        count = sizeof(app_buff) / sizeof(app_buff[0]);
        AppBIOZISR(app_buff, &count);
        fImpCar_Type *pImp = (fImpCar_Type *)app_buff;
        for (uint32_t i = 0; i < count && got < ZERO_SAMPLES; i++, got++) {
          sum_real += pImp[i].Real;
          sum_image += pImp[i].Image;
        }
      }
      AppBIOZCtrl(BIOZCTRL_STOPNOW, 0);

      zero_baseline.Real = sum_real / ZERO_SAMPLES;
      zero_baseline.Image = sum_image / ZERO_SAMPLES;
      zero_baseline_freq = freq_hz;
      have_zero_baseline = 1;
      printf("Zero calibration captured at %.1fHz: Z=(%.2f,%.2f)ohm, "
             "averaged over %u samples.\n",
             freq_reported, zero_baseline.Real, zero_baseline.Image,
             (unsigned)ZERO_SAMPLES);
    } else {
      uint32_t sample_num = 0; /* starts back at 0 on every fresh 'start' */
      for (;;) {
        count = sizeof(app_buff) / sizeof(app_buff[0]);
        AppBIOZISR(app_buff, &count);
        if (count > 0) {
          PrintSample(app_buff, count, &sample_num, freq_reported, apply_baseline);
        }
        if (UartPollLine(line, sizeof(line)) && strcmp(line, "stop") == 0) {
          AppBIOZCtrl(BIOZCTRL_STOPNOW, 0);
          printf("Stopped.\n");
          break;
        }
      }
    }
  }
}
