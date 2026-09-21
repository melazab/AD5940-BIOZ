# time-series-bioz-2wire

Continuous, single-frequency impedance measurement on the **AD5940-BIOZ**
shield (on an **EVAL-ADICUP3029** board), read against ADI's **impedance
test board** or a real 2-electrode sensor plugged into the shield's 2-wire
(CE0/AIN1) header. Type `start <Hz>` over UART to stream impedance
samples continuously at that frequency. Throughput depends on frequency.
`stop` ends the run and returns to the prompt so a new frequency can be picked. `zero <Hz>`
captures an offset baseline first (see "Zero calibration" below). This is
the 2-wire sibling of `../time-series-bioz/` (which does the same thing
over the 4-wire F+/S+/F-/S- header, at its own slower 5Hz rate) -- same
UART protocol, same GUI controls, just CE0/AIN1 excitation+sense instead of
a true 4-point Kelvin connection.

## Sample rate and DFT settings

`TimeSeriesStructInit()` sets `cfg->BIOZODR = 5000.0f`. This is the
sequencer trigger rate; it does not mean 5,000 complete impedance samples
per second. Each sample requires current and voltage DFT captures, and
actual throughput depends on excitation frequency and processing time.
Source comments record approximately 327 samples/s at 10 kHz excitation and
48.8 samples/s at 1 kHz, with no checksum failures or non-monotonic sample
numbers in those checks. These are previous observations, not guaranteed
rates or a fresh validation of every configuration.

DFT length, source, and SINC filter settings come from
`AD5940_GetFreqParameters(freq_hz)`. The sequencer wait and the DFT hardware
therefore use matching settings. The previous fixed `DFTNUM_512` setup
could end integration too early when the application layer later selected
different settings.

Samples use 16-byte binary frames at 230400 baud, 8N1. The raw UART capacity
is 23,040 bytes/s, or at most 1,440 frames/s before status messages and other
overhead; the old ASCII-line bandwidth calculation no longer applies.
See the [shared UART protocol](../README.md#uart-protocol-reference).

Do not increase `BIOZODR` blindly. The source documents wake-up timer
arithmetic underflow above roughly 10,666 Hz with its 32 kHz clock, and the
vendored `AppBIOZDataProcess()` still indexes overlapping current/voltage
pairs when processing multiple samples in a batch. Clean frame checksums
alone do not establish impedance accuracy.

## Hardware validation status

Earlier testing at 50 kHz recorded about 22 kOhm uncalibrated for a
20 kOhm resistor, consistent with an approximately 2 kOhm series baseline.
A 32 Ohm resistor was indistinguishable from a short in that setup. Those
observations predate the current gain/timing settings and do not establish
the current noise floor; recheck known loads after changing configuration.

Not yet checked: a separate sweep test on `../measure-2wire-bioz/` (`start`,
`SweepEn=bTRUE`, the same underlying `bioz_2wire.c`) showed a glitch right
around the `AppBIOZCheckFreq()` HP/LP mode switchover (roughly 50-60kHz) --
short and open circuit traces both broke down in that region. Whether that
also affects this continuous firmware at frequencies near that boundary
hasn't been confirmed; if `start <Hz>` output looks wrong specifically
somewhere in the 50-60kHz range, that switchover is the first thing to
suspect, not your wiring.

## Design

Same bare-metal, hand-rolled-registers philosophy as `../blink-led/`, with
one deliberate exception: the AD5940 itself needs a large amount of
low-level driver logic (sequencer op-codes, DFT engine setup, RTIA
calibration math) that Analog Devices ships as a library
([ad5940lib](https://github.com/analogdevicesinc/ad5940lib)) and reuses
across all their own example applications. Reimplementing that from
scratch would mean re-deriving ADI's own IP with no way to verify it
against real hardware in this environment, so:

- `ad5940lib/` is **vendored unmodified** (ADI's license permits
  redistribution for use with ADI processors/products, which this is --
  see `ad5940lib/LICENSE`).
- `bioz_2wire.c`/`.h` is ADI's `AD5940_BIOZ-2Wire` example application
  layer (from
  [ad5940-examples](https://github.com/analogdevicesinc/ad5940-examples)),
  carried over unmodified from `../measure-2wire-bioz/` -- this is the
  tested-for-sweeps logic that turns raw DFT results into calibrated
  impedance. Unlike `../time-series-bioz/`'s 4-wire `BodyImpedance.c`, this
  library switches HP/LP power mode per frequency internally
  (`AppBIOZCheckFreq()`), so `main.c` doesn't need the manual >80kHz check
  the 4-wire version does.
- `registers.h`, `ad5940_port.c`, `startup.c`, `linker.ld` and `main.c` are
  this project's own from-scratch platform layer, same as `blink-led`'s:
  hand-written register structs cross-checked against Analog Devices'
  CMSIS device header (as shipped in mbed-os), not ADI's own
  `ADICUP3029Port.c` reference copied blind.

### No MCU-side GPIO interrupt

ADI's own reference wires the AD5940's interrupt pin to an ADuCM3029 GPIO
(XINT0/IRQ0) and only calls into the app layer on that edge. This project
skips that wiring entirely: `AppBIOZISR()` already checks the AD5940's own
interrupt status register over SPI before doing any real work, so calling
it in a tight loop from `main()` is functionally equivalent to waiting for
the pin edge, just realized as polling instead.

### Two independent clocks

`MCU_ClockAndUartInit()` in `main.c` configures the **ADuCM3029's own**
system clock (26MHz HFXTAL, for the UART baud rate and SPI bus speed).
`AD5940PlatformCfg()` separately configures the **AD5940 chip's own**
internal clock over SPI (`AD5940_CLKCfg()`), sourced from its own XTAL on
the AD5940-BIOZ shield. Don't confuse `SYSTICK_CLK_FREQ_HZ` in
`ad5940_port.c` (the MCU's clock, for the delay routine) with
`AppBIOZCfg.SysClkFreq` in `main.c` (the AD5940's clock, for its own
DFT/timing calculations).

### Zero calibration

AN-1557 documents that a raw 2-wire reading includes the current-limiting
resistors (RLIMIT1/RLIMIT2, ~1kOhm each: `R43`/`R19` on
`Schematic_EVAL-AD5940BIOZ.pdf`'s ELECTRODES block) and isolation-cap
impedance (`C68`/`C1`) on top of the impedance under test -- together
roughly 2kOhm at 50kHz. `../measure-2wire-bioz/` handles this with a
`'zero'` command that captures a baseline sweep (all S1 switches closed,
i.e. a known short) and subtracts it per sweep point.

This firmware does the same thing, adapted for a single fixed frequency
instead of a 40-point sweep: `zero <Hz>` runs the same measurement path as
`start <Hz>` with a known-zero load (a short, or whatever your S1-bank
equivalent is) in place, and averages `ZERO_SAMPLES` (200) live samples into
one complex baseline. Capture duration depends on actual sample throughput
and includes initialization/calibration overhead; it is not fixed at one
second. Wait for the plain-text calibration-complete message before changing
the load.

A later `start <Hz>` at that same frequency subtracts the baseline from every
sample and sets bit 0 of the binary frame's flags. With no matching baseline,
the firmware sends raw impedance with that bit cleared; the GUI adds an
uncalibrated annotation to its decoded log. Only one baseline is retained,
and another zero replaces it. It survives `stop`/`start` and the AD5940
reset performed between runs, but is lost when the MCU is reset or powered
off.

Practical implication: `zero`/`start` must be run at the *exact* same `Hz`
value to have the baseline apply -- `zero 50000` then `start 50000.0` would
still match (both parse to the same `float`), but `zero 50000` then
`start 50001` would not, and you'd silently get uncalibrated output rather
than an error.

## Hardware setup

1. AD5940-BIOZ shield plugged into the ADICUP3029's headers.
2. Impedance test board or 2-electrode sensor plugged into the shield's
   2-wire (CE0/AIN1) header -- **not** the 4-wire BIA header.
3. USB cable from the ADICUP3029's DAPLink port to your computer (both
   flashes the board and carries the UART over the same virtual COM port).

Current parameters in [`TimeSeriesStructInit()`](main.c) are RCAL =
10 kOhm, CE0/AIN1 routing, RTIA = 5 kOhm, and `DacVoltPP = 800` mV
(with x2 excitation-buffer gain and x1 DAC gain). These differ from the
2-wire sweep firmware's settings. If you swap in a different RCAL resistor
value on your board, update `cfg->RcalVal` to
match, or every reported impedance will be off by that ratio.

## Build

```
make
```

Produces `time_series_bioz_2wire.elf` and `time_series_bioz_2wire.bin`.

## Flash

```
make flash
```

Same DAPLink drag-and-drop mechanism as `blink-led` (see its README for
details on the mass-storage flashing quirks). **After flashing, press the
board's reset button** -- DAPLink doesn't reliably reset-and-run the
target after an MSD write, so the new firmware sits there programmed but
not executing until you either hit reset or replug the USB cable.

## Read the output

The DAPLink virtual COM port carries UART0 at **230400 baud, 8N1**.
Use the [GUI quick start](../README.md#quick-start) to decode and plot samples
or record them to CSV. Select `time-series-bioz-2wire` to expose its controls.

For your own serial client, send newline-terminated commands in this order:

1. With a known short connected, send `zero 50000` and wait for the
   plain-text `Zero calibration captured...` message.
2. Replace the short with the device under test and send `start 50000`.
3. Decode the incoming **16-byte binary frames** using the
   [shared UART protocol](../README.md#uart-protocol-reference).
4. Send `stop` to end streaming and return to the command prompt.

Boot banners, prompts, and calibration status are text. Measurement samples
are binary, so a terminal such as picocom will not display readable sample
lines. The GUI reconstructs readable lines from those frames. Skipping zero
or starting at a different frequency produces raw measurements with the
baseline flag cleared. The sample counter starts at zero on each new run.

## Layout

- `ad5940lib/` -- vendored ADI driver (`ad5940.c`/`.h`), unmodified.
- `bioz_2wire.c`/`.h` -- ADI's 2-wire BIOZ application layer, carried over
  unmodified from `../measure-2wire-bioz/`.
- `ad5940_port.c` -- the hardware-specific functions `ad5940lib` calls
  into: SPI transfers, CS/RESET GPIOs, a microsecond delay.
- `registers.h` -- hand-written ADuCM3029 register definitions (GPIO,
  watchdog, clock, SPI0, UART0, plus the Cortex-M3 core's SysTick).
- `main.c` -- MCU clock/UART bring-up, AD5940 platform config, the
  `zero <Hz>`/`start <Hz>`/`stop` command loop, the zero-baseline capture
  and subtraction, and binary sample output.
- `startup.c` -- vector table, reset handler, and the `_sbrk`/`_write`
  newlib retargeting.
- `linker.ld` -- flash/SRAM memory map, including the same
  `.security_options` marker `blink-led` needed to actually boot.
- `openocd/aducm3029.cfg` -- shared debug target script (see `blink-led`'s
  README for usage; same caveats, no flash driver).
