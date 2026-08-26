# AD5940-BIOZ

Bare-metal firmware and a desktop GUI for the **AD5940-BIOZ** bioimpedance
shield on an **EVAL-ADICUP3029** board (ADuCM3029, Cortex-M3). No vendor
IDE (CCES/IAR/Keil) anywhere -- startup code, linker scripts, and MCU
register access are hand-written and built with a plain
`arm-none-eabi-gcc` + `make` toolchain. Each firmware directory vendors
Analog Devices' own [ad5940lib](https://github.com/analogdevicesinc/ad5940lib)
driver and, where applicable, an ADI example application layer
(`BodyImpedance.c`/`bioz_2wire.c`) unmodified -- see the individual
directories' `README.md`/`main.c` comments for what's original versus
vendored in each case.

## Hardware

- **EVAL-ADICUP3029** motherboard (the ADuCM3029 MCU this all runs on).
- **AD5940-BIOZ** shield, plugged into the ADICUP3029's Arduino-style
  headers.
- Optional, depending on what you're testing: ADI's **AD5940 impedance
  test board** (known resistor/capacitor networks), the **custom
  snap-lead cable** (a 4-lead cable -- F+/S+/S-/F-, colored
  red/green/blue/black -- that plugs into a micro-USB-shaped jack on the
  shield, distinct from the ADICUP3029's own USB port used for
  power/programming/UART).

## Toolchain setup

The GUI's "Build && Flash" and `make` both shell out to `arm-none-eabi-gcc`,
so it needs to be on `PATH`. This is per-OS -- if you dual-boot or otherwise
run the GUI from more than one OS on the same machine, each one needs its
own install; a `PATH` fix on one side does nothing on the other.

### Linux

```bash
sudo apt update && sudo apt install gcc-arm-none-eabi openocd
```

`apt` puts both on `PATH` automatically. Verify with `arm-none-eabi-gcc --version`
and `openocd --version`. `make`/`cp`/`sync` are already there on any normal
Linux install.

### Windows

The [Arm GNU Toolchain installer](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads)
has an "Add to PATH" checkbox, but if it was skipped (or the toolchain was
installed some other way), add it manually -- adjust the version folder
name to match what's actually installed under
`C:\Program Files (x86)\GNU Arm Embedded Toolchain\`:

```powershell
$toolchainPath = "C:\Program Files (x86)\GNU Arm Embedded Toolchain\10 2021.10\bin"
$userPath = [Environment]::GetEnvironmentVariable("PATH", "User")
if ($userPath -notlike "*$toolchainPath*") {
  [Environment]::SetEnvironmentVariable("PATH", "$userPath;$toolchainPath", "User")
}
$env:PATH = "$env:PATH;$toolchainPath"  # so the current session picks it up too
arm-none-eabi-gcc --version
```

This edits the persistent **User** `PATH` (no admin rights needed) and also
patches the current session's `PATH` so it works without reopening the
terminal. Any *other* already-open terminal/GUI still needs a restart to
see the change.

`make` (and `cp`) come from whatever MinGW/MSYS/Git-Bash environment is already on `PATH` -- there's no separate install step for those here.

OpenOCD isn't packaged via winget's default `msstore`/`winget` sources under a
simple name, but the [xPack OpenOCD](https://github.com/xpack-dev-tools/openocd-xpack)
build is:

```powershell
winget install --id xpack-dev-tools.openocd-xpack
```

winget adds it to `PATH` itself, but (same as the toolchain above) only
already-open shells/GUIs won't see it until restarted.

### Why OpenOCD, on either OS

`gui/main.py` shells out to `openocd` after every flash to reset-and-run the
target over SWD. This isn't optional for the GUI: DAPLink's plain
drag-and-drop MSD write does **not** reliably reset-and-run the new image on
this board (see `blink-led/README.md`'s Flash section) -- without OpenOCD,
the old firmware keeps running until you manually press the board's reset
button or replug the USB cable. It's genuinely optional only if you're
driving `make`/`make flash` by hand and are fine pressing reset yourself.

Unlike Linux, Windows has no `lsblk`/`udisksctl` equivalent, so `gui/main.py`
finds the DAPLink drive by scanning drive letters for the `DAPLINK` volume
label instead, and copies+`fsync`s the `.bin` directly in Python rather than
shelling out to `cp`/`sync` (neither of which reliably exist on Windows).

## Firmware directories

| Directory | What it does |
|---|---|
| `blink-led/` | Bring-up/toolchain sanity check -- blinks the ADICUP3029's onboard green LED (DS3). No AD5940 involved; start here if the build/flash pipeline itself is in question. |
| `measure-2wire-bioz/` | One 2-wire (CE0/AIN1) frequency sweep per `start`; `zero` first captures a baseline (RLIMIT/isolation-cap offset) to subtract per sweep point. |
| `measure-4wire-bioz/` | One true 4-wire/Kelvin (F+/S+/F-/S-, separate excitation and sense electrode pairs) frequency sweep per `start`. |
| `time-series-bioz/` | Continuous single-frequency 4-wire measurement -- `start <Hz>` streams one impedance sample every 200ms indefinitely; `stop` ends the run. |
| `time-series-bioz-2wire/` | Same as above but 2-wire (CE0/AIN1), at 200Hz. `zero <Hz>` captures a baseline at a given frequency to subtract from a later `start <Hz>` at that same frequency. |

All of them talk over UART0 at **230400 baud, 8N1** and use the same
DAPLink mass-storage flashing convention (`make flash`, or the GUI's
"Build && Flash" button) -- see `blink-led/README.md` for the flashing
quirks (reset-after-flash, etc.) that apply everywhere. What actually comes
over that UART differs by firmware -- see below if you're writing your own
tool against it instead of using `gui/`.

## UART protocol reference

For interfacing your own application against the firmware directly (LabVIEW,
a custom Python/MATLAB script, whatever) instead of `gui/`. All four
firmwares share the same UART0 settings (**230400 baud, 8N1**, DAPLink's
virtual COM port) and command style (type a command, newline-terminated,
`\r` or `\n` either works), but differ in what per-measurement-point data
looks like on the wire.

### Sweep firmwares (`measure-2wire-bioz`, `measure-4wire-bioz`)

Plain text throughout. Commands: `zero` (capture a baseline with a known-zero
load in place), `start` (run one sweep, subtracting the zero baseline if one
was captured). One line per sweep point:

```
freq=1000.0Hz Z=(482.31,-118.02)ohm |Z|=496.68ohm phase=-13.75deg
```

Un-zeroed output is tagged `(uncalibrated -- run 'zero' first)` at the end
of the line instead.

### Time-series firmwares (`time-series-bioz`, `time-series-bioz-2wire`)

Commands: `start <Hz>` / `stop` (both), plus `zero <Hz>` (2-wire only, same
baseline-capture idea as the sweep firmwares' `zero`, at a specific
frequency). The boot banner, command prompts, and one-off status messages
("Zero calibration captured...", "Stopped.") are plain text, same as the
sweep firmwares -- but each measurement point is a **16-byte little-endian
binary frame** instead of a text line, sharing the same UART byte stream as
that text. (Why binary instead of text: printing each point as
`printf("...%.2f...")` mallocs scratch space per float internally, and on a
long continuous run this fragmented the firmware's 4KB heap badly enough to
hang it permanently after tens of thousands of samples -- see
`time-series-bioz-2wire/main.c`'s `SendSampleBinary()` comment for the full
writeup.)

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 1 | sync byte 1 | always `0xAA` |
| 1 | 1 | sync byte 2 | always `0x55` |
| 2 | 4 | `sample_num` | `uint32`, little-endian |
| 6 | 1 | `flags` | bit0 = `apply_baseline` (1 = calibrated; matches the sweep firmwares' `(uncalibrated -- run 'zero' first)` text tag). Always 1 on `time-series-bioz` (true 4-wire/Kelvin sensing has no RLIMIT offset to zero out). |
| 7 | 4 | `real` | `float32`, little-endian |
| 11 | 4 | `imag` | `float32`, little-endian |
| 15 | 1 | `checksum` | XOR of bytes `[0:15)` |

The `0xAA 0x55` sync marker is what lets a receiver tell a frame apart from
the surrounding plain text -- that byte pattern never occurs in the
firmware's printable-ASCII output, so no separate mode switch is needed;
just scan for it inline. **Frequency isn't in the frame** -- it's fixed for
the whole `start <Hz>` run, so your own code already has it from when it
sent that command. `|Z|`/phase aren't included either; compute them from
`real`/`imag` yourself (`hypot(real, imag)` / `atan2(imag, real)` in
degrees).

`gui/main.py`'s `SerialReader`/`parse_sample_frame()` is a working reference
decoder if you'd rather read code than a table.

## `gui/`

A Tkinter app (`python3 gui/main.py`, needs `pyserial`/`matplotlib` --
`pip install -r gui/requirements.txt`) that builds and flashes whichever
firmware directory you pick from a dropdown, connects over UART, and
live-plots whatever comes back -- a frequency-sweep view (|Z|/phase vs.
frequency, log-x) or a time-series view (vs. sample number), switching
automatically based on which line format the firmware is actually
printing. Each firmware's specific controls (plain `start`/`zero` for the
sweep firmwares, `start <Hz>`/`stop`/`zero <Hz>` for the time-series ones)
are shown/hidden based on the firmware selected.

## `docs/`

Reference material, not something to build: the EVAL-AD5940 user guide,
the AD5940 datasheet, AN-1557 (2-wire bioimpedance theory), and this
shield's own schematic (`Schematic_EVAL-AD5940BIOZ.pdf`) -- useful for
tracing connector pinouts (e.g. which physical cable lead maps to which
chip pin) or understanding the on-board RLIMIT/isolation-cap network that
sits between the cable and the AD5940 in 2-wire mode.

## Documentation status

Every firmware directory has its own accurate `README.md` (the
`measure-2wire-bioz/`, `measure-4wire-bioz/`, and `time-series-bioz/`
copies that used to be stale duplicates of a since-removed sibling
directory's original text have been rewritten).
