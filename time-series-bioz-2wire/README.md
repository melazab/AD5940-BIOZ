# time-series-bioz-2wire

Continuous, single-frequency impedance measurement on the **AD5940-BIOZ**
shield (on an **EVAL-ADICUP3029** board), read against ADI's **impedance
test board** or a real 2-electrode sensor plugged into the shield's 2-wire
(CE0/AIN1) header. Type `start <Hz>` over UART to stream one impedance
sample every 5ms (200Hz) indefinitely at that frequency; `stop` ends the run
and returns to the prompt so a new frequency can be picked. `zero <Hz>`
captures an offset baseline first (see "Zero calibration" below). This is
the 2-wire sibling of `../time-series-bioz/` (which does the same thing
over the 4-wire F+/S+/F-/S- header, at its own slower 5Hz rate) -- same
UART protocol, same GUI controls, just CE0/AIN1 excitation+sense instead of
a true 4-point Kelvin connection.

## Sample rate: 200Hz, not faster

200Hz (`cfg->BIOZODR` in `TimeSeriesStructInit()`) is deliberately tuned to
the UART link, not the AD5940. Each printed line is ~85-90 bytes; at
230400 baud (8N1 = 23040 bytes/sec) that's ~3.7-3.9ms/line, a hard ceiling
around 250-270 lines/sec no matter how fast the AD5940 itself measures.
200Hz leaves ~25% headroom under that for line-length growth (the sample
counter gains digits over a long run) and normal jitter. Pushing `BIOZODR`
past what the UART can drain risks the AD5940's FIFO backing up between
`AppBIOZISR()` polls -- which resurfaces a real bug in `bioz_2wire.c`'s
`AppBIOZDataProcess()` (it indexes DFT result pairs as `pSrcData[i]`/
`pSrcData[i+1]` instead of `pSrcData[2*i]`/`pSrcData[2*i+1]`, so once more
than one point's worth of data is buffered, later points in that batch get
the wrong current paired with the wrong voltage).

Getting here also required shrinking `cfg->DftNum` from `bioz_2wire.c`'s
default `DFTNUM_8192` to `DFTNUM_512` -- each measurement point runs two
sequential DFTs (current, then voltage), and at 8192 points each took
~20ms in LP mode, capping the AD5940 itself around 24 samples/sec (slower
than even the original 5Hz target, with margin to spare). `DFTNUM_512` is
a real quality tradeoff, not a free speedup: it controls how many
excitation cycles get coherently averaged per DFT, so fewer points means
more per-sample noise, not "worse resolution" in the FFT sense (the
excitation frequency is programmed exactly, not searched for). At 50kHz,
512 points still span ~64 cycles -- fine. At low excitation frequencies (a
few kHz or below), 512 points span only a handful of cycles (~1 cycle at
1kHz in LP mode), so expect visibly noisier readings there than at 50kHz+;
this hasn't been tuned per-frequency, and a true 1kHz *reporting* rate
isn't achievable at all at 230400 baud regardless of `DftNum` -- the UART
line format itself would need to shrink drastically or the baud rate would
need to go up an order of magnitude, neither of which this firmware does.

## Hardware validation status

Confirmed working at 50kHz: a 20kOhm resistor across the 2-wire header
reads ~22kOhm uncalibrated, which lines up with the ~2kOhm fixed on-board
RLIMIT/coupling network (see "Zero calibration" below) sitting in series
with the resistor -- `zero <Hz>` subtracts that out. Earlier testing with a
32Ohm resistor looked identical to a dead short, which is correct (32Ohm
is well within the noise floor of a ~2kOhm baseline), not a bug -- if
you're validating against a known resistor, use something clearly bigger
than the baseline (a few kOhm or more) or the difference won't be visible
until after `zero`ing.

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
equivalent is) in place, averages `ZERO_SAMPLES` (200, ~1s at 200Hz) live
samples into one baseline `fImpCar_Type` instead of trusting a single
reading, then stores it alongside the frequency it was captured at.
`ZERO_SAMPLES` was raised from 10 to 200 alongside the DFTNUM_512 speedup
below -- each individual sample is noisier now, so averaging more of them
for the one-time baseline capture buys back some of that lost precision. A
later `start <Hz>` at that *same* frequency subtracts the baseline from
every sample before printing; a `start` at a different frequency, or
before any `zero` has run, prints the raw uncalibrated reading instead,
tagged `(uncalibrated -- run 'zero <Hz>' first)` so it's obvious from the
log which mode you're in. Only one baseline is kept at a time (not a table
indexed by frequency like the sweep version) -- rerunning `zero` at a new
frequency replaces it.

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

Measurement parameters in `main.c`'s `TimeSeriesStructInit()` (RCAL =
10kOhm, CE0/AIN1 switch matrix, RTIA = 1kOhm) match
`../measure-2wire-bioz/`'s `BIOZStructInit()` exactly. If you swap in a
different RCAL resistor value on your board, update `cfg->RcalVal` to
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

The DAPLink virtual COM port carries UART0 at **230400 baud, 8N1**:

```
picocom -b 230400 /dev/ttyACM0
```

(adjust the device node to match your system; `screen /dev/ttyACM0
230400` works too). Expect a build banner, then a prompt for `zero
<Hz>`/`start <Hz>`. With a short in place:

```
zero 50000
Zero calibration captured at 50000.0Hz: Z=(1998.41,-215.30)ohm, averaged over 200 samples.
```

Then swap in the device under test and, at the *same* frequency, one line
per sample once running:

```
start 50000
sample=0 freq=50000.0Hz Z=(20124.31,-1832.02)ohm |Z|=20207.68ohm phase=-5.20deg
sample=1 freq=50000.0Hz Z=(20089.90,-1811.88)ohm |Z|=20170.20ohm phase=-5.15deg
...
```

Skip `zero` and `start` prints the raw, uncalibrated reading instead
(device under test + RLIMIT + isolation-cap impedance), tagged
`(uncalibrated -- run 'zero <Hz>' first)`. Type `stop` to end either run.

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
  and subtraction, and per-sample printing.
- `startup.c` -- vector table, reset handler, and the `_sbrk`/`_write`
  newlib retargeting.
- `linker.ld` -- flash/SRAM memory map, including the same
  `.security_options` marker `blink-led` needed to actually boot.
- `openocd/aducm3029.cfg` -- shared debug target script (see `blink-led`'s
  README for usage; same caveats, no flash driver).
