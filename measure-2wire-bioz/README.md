# measure-2wire-bioz

On-demand frequency-sweep impedance measurement on the **AD5940-BIOZ**
shield (on an **EVAL-ADICUP3029** board), over the shield's 2-wire
(CE0/AIN1) header. Type `zero` (with all S1 switches closed / a known
short in place) to capture an RLIMIT/isolation-cap offset baseline, or
`start` to run one 1kHz-200kHz, 40-point sweep over whatever's currently
selected on the Z test board's S1 bank -- subtracting the `zero` baseline
if one was captured. `start`/`zero` can be run repeatedly without
reflashing, so you can flip the S1 switch and sweep again.

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
  carried over essentially as-is -- this is the tested logic that turns
  raw DFT results into calibrated impedance.
- `registers.h`, `ad5940_port.c`, `startup.c`, `linker.ld` and `main.c` are
  this project's own from-scratch platform layer, same as `blink-led`'s:
  hand-written register structs cross-checked against Analog Devices'
  CMSIS device header (as shipped in mbed-os), not ADI's own
  `ADICUP3029Port.c` reference copied blind.

### CE0/AIN1, not CE0/AIN2

The EVAL-AD5940 user guide's own EDA table pairs CE0 with AIN2. This
firmware's `BIOZStructInit()` uses **CE0/AIN1** instead -- AN-1557's own
2-wire theory section connects Z_UNKNOWN between CE0 and AIN1, and AIN2
was only ever confirmed against the EDA table's own "S+ to AIN2" note,
which is a different measurement mode, not verified for straight 2-wire
impedance.

### Zero calibration

AN-1557's "Measurement Results" section documents that a raw 2-wire
reading is the impedance under test **plus** the current-limiting
resistors (RLIMIT1/RLIMIT2, ~1kOhm each) and isolation-cap impedance --
not the unknown resistor alone. `zero` captures one sweep with a
known-zero external impedance (all S1 bank switches closed) and
`ProcessSweep()` subtracts it, per frequency point, from every later
`start` sweep. Readings printed before any `zero` has run are tagged
`(uncalibrated -- run 'zero' first)`.

### `'freq'` is disabled -- known bug

A single-frequency `freq <Hz>` command was tried (`SweepEn=bFALSE`,
`NumOfData=1`) and consistently returned wildly wrong results (millions of
ohms) on real hardware, in every variant attempted -- root cause not
found. `start`/`zero` (`SweepEn=bTRUE`, the full 40-point sweep) are the
only paths confirmed working. `main.c` refuses to run `freq` rather than
silently print wrong numbers; see `../time-series-bioz-2wire/` for a
continuous-measurement firmware that takes on this same risk deliberately
(and documents what was found there).

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

### Why there's a heap

Unlike `blink-led` (`-nodefaultlibs`, no libc at all), this program links
newlib (`-specs=nosys.specs`, no `-nodefaultlibs`) because the AD5940
library needs `sqrt`/`pow`/`log10`/`atan2` and this program's output needs
floating-point `printf`. newlib's `%f`/`%e`/`%g` conversion (`dtoa`)
mallocs small, short-lived scratch buffers internally, so `startup.c`
implements `_sbrk()` against a small fixed heap region carved out in
`linker.ld` (`HEAP_SIZE`, 4KB, sitting between `.bss` and the stack).
Every other newlib syscall stub (`_close`, `_lseek`, `_read`, `_fstat`,
`_isatty`, `_kill`, `_getpid`, `_exit`) is `-specs=nosys.specs`'s default
(always fails, harmless -- link-time warnings about this are expected and
not a problem). `_write` is retargeted to UART0 in `main.c`.

## Hardware setup

1. AD5940-BIOZ shield plugged into the ADICUP3029's headers.
2. AD5940 impedance test board (or a 2-electrode sensor) plugged into the
   shield's 2-wire (CE0/AIN1) header -- **not** the 4-wire BIA header.
3. USB cable from the ADICUP3029's DAPLink port to your computer (both
   flashes the board and carries the UART over the same virtual COM port).

Measurement parameters in `main.c`'s `BIOZStructInit()` (RCAL = 10kOhm,
`HSTIARTIA_1K`, CE0/AIN1 switch matrix) match the EVAL-AD5940 user guide's
Body Impedance validation table and AN-1557's worked example for this
~1-2kOhm range. If you swap in a different RCAL resistor value on your
board, update `cfg->RcalVal` to match, or every reported impedance will be
off by that ratio.

## Build

```
make
```

Produces `measure_2wire_bioz.elf` and `measure_2wire_bioz.bin`.

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
230400` works too). Expect a build banner, then a prompt for `zero`/
`start`. With all S1 switches closed:

```
zero
Zero calibration captured across 40 points.
```

Then set the S1 bank to whatever resistor you want and:

```
start
freq=1000.0Hz Z=(482.31,-118.02)ohm |Z|=496.68ohm phase=-13.75deg
...
```

(40 lines, one per swept frequency point). Repeat `start` (or `zero`, to
recapture the baseline) as many times as you like without reflashing.

## Layout

- `ad5940lib/` -- vendored ADI driver (`ad5940.c`/`.h`), unmodified.
- `bioz_2wire.c`/`.h` -- ADI's 2-wire BIOZ application layer, carried over
  from their `AD5940_BIOZ-2Wire` example.
- `ad5940_port.c` -- the hardware-specific functions `ad5940lib` calls
  into: SPI transfers, CS/RESET GPIOs, a microsecond delay.
- `registers.h` -- hand-written ADuCM3029 register definitions (GPIO,
  watchdog, clock, SPI0, UART0, plus the Cortex-M3 core's SysTick).
- `main.c` -- MCU clock/UART bring-up, AD5940 platform config, the
  `zero`/`start` command loop, and the zero-baseline capture/subtraction.
- `startup.c` -- vector table, reset handler, and the `_sbrk`/`_write`
  newlib retargeting.
- `linker.ld` -- flash/SRAM memory map, including the same
  `.security_options` marker `blink-led` needed to actually boot.
- `openocd/aducm3029.cfg` -- shared debug target script (see `blink-led`'s
  README for usage; same caveats, no flash driver).
