# time-series-bioz

Continuous, single-frequency impedance measurement on the **AD5940-BIOZ**
shield (on an **EVAL-ADICUP3029** board), over the shield's **4-wire
(F+/S+/F-/S-)** header -- true Kelvin sensing, same application layer
(`BodyImpedance.c`) as `../measure-4wire-bioz/`. Type `start <Hz>` to
stream one impedance sample every 200ms indefinitely at that frequency;
`stop` ends the run and returns to the prompt so a new frequency can be
picked. This is a time-series recorder, not a sweep -- see
`../time-series-bioz-2wire/` for the 2-wire sibling (CE0/AIN1 only, at a
faster 200Hz, with its own `zero <Hz>` baseline calibration that this
4-wire version doesn't need).

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
- `BodyImpedance.c`/`.h` is ADI's `AD5940_BIA` example application layer,
  carried over unmodified from `../measure-4wire-bioz/` -- see that
  directory's README for what makes this actually 4-wire (a separate
  AIN3/AIN2 voltage-sense DFT capture, independent of the CE0/AIN1
  excitation path).
- `registers.h`, `ad5940_port.c`, `startup.c`, `linker.ld` and `main.c` are
  this project's own from-scratch platform layer, same as `blink-led`'s:
  hand-written register structs cross-checked against Analog Devices'
  CMSIS device header (as shipped in mbed-os), not ADI's own
  `ADICUP3029Port.c` reference copied blind.

### Frequency-dependent power mode

Unlike `../measure-4wire-bioz/` (one fixed `AFEPWR_HP` for its whole
1kHz-200kHz sweep), this firmware knows its exact frequency up front at
each `start <Hz>`, so `TimeSeriesStructInit()` picks the matching power
mode instead of forcing HP unconditionally: `AFEPWR_HP`/32MHz above 80kHz,
`AFEPWR_LP`/16MHz at or below it -- `ad5940.h` documents `AFEPWR_LP` as
only valid "for signal <80kHz," which is what produced a garbage 200kHz
reading in `../measure-4wire-bioz/` before that firmware switched to
always using HP.

### Fresh hardware reset on every `start`, not just at boot

RTIA calibration reruns on every `start` (the frequency can change between
runs), and calibrating against a chip left in whatever analog state a
previous run's measurements left it in -- rather than a clean, freshly
reset state -- was producing a baseline offset on the second and later
runs, despite the first run after flashing being correct. `main()` does a
full `AD5940_HWReset()` + `AD5940PlatformCfg()` before every `start`, not
just once at boot, to avoid that.

### No MCU-side GPIO interrupt

ADI's own reference wires the AD5940's interrupt pin to an ADuCM3029 GPIO
(XINT0/IRQ0) and only calls into the app layer on that edge. This project
skips that wiring entirely: `AppBIAISR()` already checks the AD5940's own
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
`AppBIACfg.SysClkFreq` in `main.c` (the AD5940's clock, for its own
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
2. AD5940 impedance test board (or a real sensor) plugged into the
   shield's **4-wire (F+/S+/F-/S-)** header -- **not** the 2-wire header.
3. USB cable from the ADICUP3029's DAPLink port to your computer (both
   flashes the board and carries the UART over the same virtual COM port).

Measurement parameters in `main.c`'s `TimeSeriesStructInit()` (RCAL =
10kOhm, `HSTIARTIA_1K`) match ADI's own `AD5940BIAStructInit()` reference
exactly. If you swap in a different RCAL resistor value on your board,
update `cfg->RcalVal` to match, or every reported impedance will be off by
that ratio.

## Build

```
make
```

Produces `time_series_bioz.elf` and `time_series_bioz.bin`.

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
230400` works too). Expect a build banner, then a prompt for `start <Hz>`,
then one line per sample once running:

```
start 50000
sample=0 freq=50000.0Hz Z=(482.31,-118.02)ohm |Z|=496.68ohm phase=-13.75deg
sample=1 freq=50000.0Hz Z=(481.90,-117.88)ohm |Z|=496.20ohm phase=-13.74deg
...
```

Type `stop` to end the run and pick a new frequency.

## Layout

- `ad5940lib/` -- vendored ADI driver (`ad5940.c`/`.h`), unmodified.
- `BodyImpedance.c`/`.h` -- ADI's 4-wire BIA application layer, carried
  over unmodified from `../measure-4wire-bioz/`.
- `ad5940_port.c` -- the hardware-specific functions `ad5940lib` calls
  into: SPI transfers, CS/RESET GPIOs, a microsecond delay.
- `registers.h` -- hand-written ADuCM3029 register definitions (GPIO,
  watchdog, clock, SPI0, UART0, plus the Cortex-M3 core's SysTick).
- `main.c` -- MCU clock/UART bring-up, AD5940 platform config, the
  `start <Hz>`/`stop` command loop, and per-sample printing.
- `startup.c` -- vector table, reset handler, and the `_sbrk`/`_write`
  newlib retargeting.
- `linker.ld` -- flash/SRAM memory map, including the same
  `.security_options` marker `blink-led` needed to actually boot.
- `openocd/aducm3029.cfg` -- shared debug target script (see `blink-led`'s
  README for usage; same caveats, no flash driver).
