# measure-4wire-bioz

On-demand frequency-sweep impedance measurement on the **AD5940-BIOZ**
shield (on an **EVAL-ADICUP3029** board), over the shield's **4-wire
(F+/S+/F-/S-)** header -- true Kelvin sensing, not the CE0/AIN1-only
2-wire measurement `../measure-2wire-bioz/` does. Type `zero` (all S1
switches closed) as a sanity check, or `start` to run one 1kHz-200kHz,
40-point sweep over whatever's currently selected on the Z test board's S1
bank. Same `start`/`zero`-per-command workflow as `../measure-2wire-bioz/`,
just over the 4-wire header and its own application layer
(`BodyImpedance.c`, not `bioz_2wire.c`).

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
- `BodyImpedance.c`/`.h` is ADI's `AD5940_BIA` example application layer
  (from [ad5940-examples](https://github.com/analogdevicesinc/ad5940-examples)),
  carried over essentially as-is -- this is the tested logic that turns
  raw DFT results into calibrated impedance, and the one place that
  actually configures the 4-wire switch/mux sequence (see below).
- `registers.h`, `ad5940_port.c`, `startup.c`, `linker.ld` and `main.c` are
  this project's own from-scratch platform layer, same as `blink-led`'s:
  hand-written register structs cross-checked against Analog Devices'
  CMSIS device header (as shipped in mbed-os), not ADI's own
  `ADICUP3029Port.c` reference copied blind.

### What makes this actually 4-wire

`BodyImpedance.c`'s measurement sequence does two separate ADC/DFT
captures per point: one on `ADCMUXP_HSTIA_P`/`ADCMUXN_HSTIA_N` (the
current flowing out CE0, back in AIN1, through RLIMIT1/RLIMIT2), and a
second, independent one on `ADCMUXP_AIN3`/`ADCMUXN_AIN2` -- the actual
voltage sense pair, wired to physically separate electrodes from the
excitation path. Since the sense leads carry negligible current,
RLIMIT1/RLIMIT2's voltage drop doesn't show up in the sense measurement --
that's the Kelvin-sensing property, not something `main.c` configures.
Contrast `../measure-2wire-bioz/`, where the "voltage" measurement is
`ADCMUXP_VCE0` -- the excitation node itself, which does include the
RLIMIT drop, hence that firmware's need for `zero` calibration.

### Fixed power mode across the whole sweep

`ad5940.h` documents `AFEPWR_LP` as only valid "for signal <80kHz," but
`BodyImpedance.c` (unlike `bioz_2wire.c`, which switches HP/LP per point
via `AppBIOZCheckFreq()`) uses one power mode for an entire sweep. Since
this sweep runs to 200kHz, `BIAStructInit()` forces `AFEPWR_HP`/32MHz for
the whole 1kHz-200kHz range -- `AFEPWR_LP`'s compiled-in default produced
a garbage reading at the 200kHz end before this was found. HP mode remains
valid at the low end too; it just costs more power, which doesn't matter
for a bench tool.

### Zero calibration is a sanity check, not a subtraction you need

`zero` still captures a baseline (all S1 switches closed) the same way
`../measure-2wire-bioz/` does, for symmetry and as a diagnostic -- but
true 4-wire sensing shouldn't need it: if the setup is working, the
captured baseline should come out close to zero, and `start` results
shouldn't move much whether or not `zero` was run first. A real,
non-negligible baseline here would mean contact/lead impedance on the
sense electrodes themselves isn't fully zeroed out, or points at a wiring
problem -- not the RLIMIT effect 2-wire has by construction.

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
2. AD5940 impedance test board plugged into the shield's **4-wire
   (F+/S+/F-/S-)** header -- **not** the 2-wire header.
3. USB cable from the ADICUP3029's DAPLink port to your computer (both
   flashes the board and carries the UART over the same virtual COM port).

Measurement parameters in `main.c`'s `BIAStructInit()` (RCAL = 10kOhm,
`HSTIARTIA_1K`) match ADI's own `AD5940BIAStructInit()` reference exactly;
the 1kHz-200kHz/40-point sweep range is this project's own choice (a
single pass over the Z test board's S1 bank), not ADI's compiled-in
default (continuous single-frequency 50kHz). If you swap in a different
RCAL resistor value on your board, update `cfg->RcalVal` to match, or
every reported impedance will be off by that ratio.

## Build

```
make
```

Produces `measure_4wire_bioz.elf` and `measure_4wire_bioz.bin`.

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

(40 lines, one per swept frequency point). Repeat `start`/`zero` as many
times as you like without reflashing.

## Layout

- `ad5940lib/` -- vendored ADI driver (`ad5940.c`/`.h`), unmodified.
- `BodyImpedance.c`/`.h` -- ADI's 4-wire BIA application layer, carried
  over from their `AD5940_BIA` example.
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
