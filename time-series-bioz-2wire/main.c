#include "ad5940.h"
#include "bioz_2wire.h"
#include "registers.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* STATUS: unverified. The while(1) hang below is confirmed real; that a
 * watchdog reset actually recovers from it is NOT -- the one hardware test
 * run so far (a deliberately-provoked HP-mode hang, watched for 5+ minutes)
 * showed no reboot banner and no recovery at all, despite the ~8s timeout
 * WatchdogArm() sets below. Cause not yet root-caused: candidates include a
 * WDT lock/latch this code doesn't know to clear, the DIV256-prescaler
 * assumption being wrong (making the real timeout far longer than 8s), or
 * the CTL write silently not taking effect. A register-readback diagnostic
 * (print WDT0->CTL/LOAD/STAT right after WatchdogArm()) was queued but not
 * yet run. Don't trust this to actually save you from a hang until that's
 * resolved and re-tested.
 *
 * Re-armed at the end of MCU_ClockAndUartInit() (which first disables the
 * watchdog the ADuCM3029 boots with already running) as a safety net
 * against ad5940lib's own AD5940_Initialize() -- confirmed on real
 * hardware: it does a bare `while(1);` if the AD5940's chip-ID readback
 * ever comes back as anything other than the three values it recognizes
 * (ad5940lib/ad5940.c, in the CHIPID check at the end of
 * AD5940_Initialize()). A bad SPI/hibernate-wake state left over from a
 * prior HP-mode run reproduced this: the chip-ID read came back garbage
 * (0x8055), the firmware hit that while(1), and it stayed completely
 * unresponsive -- the ADuCM3029 itself was still running (confirmed via
 * OpenOCD: halting it landed inside AD5940_CsSet, a single-instruction GPIO
 * write with no loop of its own, which is only reachable while the CPU is
 * actively executing, i.e. not a Default_Handler-style fault) -- until
 * physically power-cycled. A watchdog reset recovers from that (and any
 * other hang in vendored code this project doesn't control) automatically
 * within seconds instead of requiring physical intervention.
 *
 * Only the EN bit is ever touched (both here and in the original disable
 * below) -- PRE/MODE/IRQ are left exactly as the chip's own power-on
 * defaults left them, which are presumably already "reset (not just
 * interrupt) on timeout", since that's the evident point of a watchdog
 * that's already running before any user code gets to configure it.
 * WatchdogArm()'s LOAD value picks the actual timeout (see its own
 * comment); DIV256 -- confirmed via ADI's own mbed-os WDT driver as this
 * peripheral's POR-default prescaler -- is assumed for that math, again
 * because it's never written here.
 *
 * WatchdogKick() must be called often enough that the *longest single
 * uninterruptible step* between two kicks -- not the cumulative time of a
 * whole run -- stays under the timeout: see call sites in UartReadLine()
 * (waiting on a command), right before AD5940_HWReset() (a fresh budget
 * for the whole reset+init+RTIA-cal setup sequence, whose worst-case
 * duration isn't characterized), and once per iteration of both the
 * zero-baseline capture loop and the measurement loop. */
static void WatchdogKick(void) {
  while ((WDT0->STAT & WDT_STAT_CLRIRQ) != 0) {
  }
  WDT0->RESTART = WDT_RESTART_KEY;
}

static void WatchdogArm(void) {
  while ((WDT0->STAT & WDT_STAT_LOADING) != 0) {
  }
  /* ~8s at DIV256 / ~32kHz (65535 max at this prescale is ~512s) --
   * comfortably longer than any single operation measured on real hardware
   * so far (worst case ~46ms/sample in HP mode), short enough that a
   * genuine hang recovers in single-digit seconds rather than needing a
   * physical power cycle. */
  WDT0->LOAD = 1024;
  while ((WDT0->STAT & WDT_STAT_COUNTING) != 0) {
  }
  WDT0->CTL |= WDT_CTL_EN;
}

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

  WatchdogArm();
}

static void UartPutc(char c) {
  while ((UART0->LSR & UART_LSR_THRE) == 0) {
  }
  UART0->TX = (uint16_t)c;
}

/* Raw byte write, bypassing _write's LF->CRLF translation -- binary frame
 * bytes must go out exactly as given, not filtered as if they were text. */
static void UartWriteRaw(const uint8_t *buf, unsigned len) {
  for (unsigned i = 0; i < len; i++) {
    UartPutc((char)buf[i]);
  }
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
      WatchdogKick(); /* sitting at the prompt is normal, not a hang */
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

  /* DftNum/DftSrc/ADCSinc2Osr/ADCSinc3Osr previously hardcoded to a fixed
   * DFTNUM_512 (matching neither this struct's own other filter fields,
   * which stayed at bioz_2wire.c's ADCSINC3OSR_2 default, nor whatever
   * AppBIOZCheckFreq() -- called later, per-frequency -- actually ends up
   * configuring the DFT hardware to). That mismatch matters because
   * AppBIOZSeqMeasureGen() bakes a fixed-duration SEQ_WAIT() into the
   * sequencer's measurement command list, sized from *these* fields via
   * AD5940_ClksCalculate() (ad5940lib/ad5940.c) -- a pure open-loop timed
   * wait, no ready-flag check found. If that wait is sized for fewer
   * points/a smaller SINC3 OSR than the DFT hardware register actually
   * ends up set to (by AppBIOZCheckFreq(), which runs afterward and wins),
   * the sequencer stops the DFT (AFECTRL_DFT bFALSE) before it's actually
   * finished integrating -- i.e. every point silently runs a shorter,
   * noisier DFT than its own DftNum claims, not a deliberate speed/noise
   * tradeoff. AD5940_ClksCalculate's DATATYPE_SINC3 case scales with both
   * point count *and* SINC3 OSR, so the old mismatch (512 vs the real
   * 1024-ish, ADCSINC3OSR_2 vs the real ADCSINC3OSR_4) undersized the wait
   * by close to 4x, not 2x.
   *
   * Fix: call the exact same AD5940_GetFreqParameters(freq) that
   * AppBIOZCheckFreq() itself calls, right here, and copy all four fields
   * from its result -- a pure function of freq_hz alone, so this and
   * AppBIOZCheckFreq()'s later internal call are guaranteed to agree. */
  FreqParams_Type freq_params = AD5940_GetFreqParameters(freq_hz);
  cfg->DftNum = freq_params.DftNum;
  cfg->DftSrc = freq_params.DftSrc;
  cfg->ADCSinc2Osr = (uint8_t)freq_params.ADCSinc2Osr;
  cfg->ADCSinc3Osr = (uint8_t)freq_params.ADCSinc3Osr;

  /* 200Hz: originally sized against the old ASCII UART line format's
   * throughput ceiling (~85-90 bytes/line at 230400 baud), which no longer
   * applies now that per-sample data goes out as a 16-byte binary frame
   * (~1440/sec ceiling -- see SendSampleBinary()'s comment). Left at 200Hz
   * regardless: it's the sequencer's *trigger* period (the Wakeup Timer),
   * not a guarantee -- the real limit is how long the DFT/settling above
   * actually take (comfortably over 5ms at any frequency this firmware
   * targets, per the DftNum fix above), so BIOZODR asking for faster than
   * that doesn't get you a faster rate, and whether asking for *much*
   * faster than achievable causes its own problems (missed/overlapping
   * triggers) hasn't been checked. Still relevant regardless of the exact
   * value: going faster than the AD5940 can actually drain risks its FIFO
   * backing up between AppBIOZISR() polls, which resurfaces a real
   * indexing bug in bioz_2wire.c's AppBIOZDataProcess() (pairs up the
   * wrong current/voltage DFT results once more than one point's worth of
   * data is buffered). */
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

/* Binary sample frame -- see gui/main.py's parse_sample_frame() for the
 * matching decoder. Replaces the old printf("...%.2f...", ...)-per-sample
 * text format: at BIOZODR=200Hz, a long continuous run means tens of
 * thousands of printf float conversions (dtoa mallocs scratch space
 * internally -- see startup.c's _sbrk() comment), against a 4KB heap that
 * has no way to give memory back to _sbrk(). newlib's malloc/free do reuse
 * freed blocks via their own freelist, but dtoa's scratch size varies with
 * each float's digit count, so a small heap doing tens of thousands of
 * differently-sized alloc/free cycles can eventually fragment past the
 * point where some later request can be satisfied. Confirmed on hardware:
 * two separate runs went permanently silent (no crash message, no reboot
 * banner -- every Cortex-M3 fault handler here is aliased to
 * Default_Handler's `while(1){}`) at two different sample counts (17265,
 * then 30586) -- ruling out a fixed/deterministic bug and pointing at
 * fragmentation timing that depends on the actual noise in each run's float
 * values. Sending raw fields as bytes instead means nothing in the
 * per-sample path calls malloc.
 *
 * Layout (16 bytes, little-endian to match both the ADuCM3029 and an x86
 * receiver -- no byte-swap needed):
 *   [0:2)   sync = 0xAA, 0x55 (never appears in this firmware's plain-text
 *           banner/prompt output, so a receiver can always tell frame from
 *           text without a separate out-of-band mode switch)
 *   [2:6)   sample_num, uint32
 *   [6]     flags: bit0 = apply_baseline (matches the old " (uncalibrated
 *           -- run 'zero <Hz>' first)" text suffix)
 *   [7:11)  real, float32
 *   [11:15) imag, float32
 *   [15]    checksum: XOR of bytes [0:15)
 * Frequency isn't included -- it's fixed for the whole 'start <Hz>' run, so
 * the receiver already has it from when it sent that command. */
static void SendSampleBinary(uint32_t sample_num, float real, float imag,
                              int apply_baseline) {
  uint8_t frame[16];

  frame[0] = 0xAA;
  frame[1] = 0x55;
  memcpy(&frame[2], &sample_num, sizeof(sample_num));
  frame[6] = (uint8_t)(apply_baseline ? 1 : 0);
  memcpy(&frame[7], &real, sizeof(real));
  memcpy(&frame[11], &imag, sizeof(imag));

  uint8_t chk = 0;
  for (unsigned i = 0; i < 15; i++) {
    chk ^= frame[i];
  }
  frame[15] = chk;

  UartWriteRaw(frame, sizeof(frame));
}

static void PrintSample(uint32_t *pData, uint32_t DataCount, uint32_t *pSampleNum,
                         int apply_baseline) {
  fImpCar_Type *pImp = (fImpCar_Type *)pData;

  for (uint32_t i = 0; i < DataCount; i++, (*pSampleNum)++) {
    fImpCar_Type z = pImp[i];
    if (apply_baseline) {
      z.Real -= zero_baseline.Real;
      z.Image -= zero_baseline.Image;
    }
    SendSampleBinary(*pSampleNum, z.Real, z.Image, apply_baseline);
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
    WatchdogKick(); /* fresh budget going into reset+init+RTIA-cal below */
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
        WatchdogKick();
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
        WatchdogKick();
        count = sizeof(app_buff) / sizeof(app_buff[0]);
        AppBIOZISR(app_buff, &count);
        if (count > 0) {
          PrintSample(app_buff, count, &sample_num, apply_baseline);
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
