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

On Debian/Ubuntu:

```bash
sudo apt update
sudo apt install gcc-arm-none-eabi libnewlib-arm-none-eabi make openocd python3 python3-venv python3-tk udisks2 util-linux
```

`apt` puts the command-line tools on `PATH` automatically. Verify with `arm-none-eabi-gcc --version`
and `openocd --version`. Also check `make --version`. The GUI uses
`lsblk` (util-linux) to find boards and `udisksctl` (udisks2) to mount them
when needed; command-line `make flash` uses `cp` and `sync`.

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

Install GNU Make and ensure `make --version` works in the shell used to
launch the GUI. An MSYS2 environment can provide Make and the shell tools
used by the Makefiles (`cp`, `sync`, and `rm`); do not assume Git Bash
alone includes Make. Install Python 3 with Tcl/Tk support as well, and
verify `python --version` and `python -m tkinter`.

OpenOCD isn't packaged via winget's default `msstore`/`winget` sources under a
simple name, but the [xPack OpenOCD](https://github.com/xpack-dev-tools/openocd-xpack)
build is:

```powershell
winget install --id xpack-dev-tools.openocd-xpack
```

winget adds it to `PATH` itself, but (same as the toolchain above) only
already-open shells/GUIs won't see it until restarted.

### Reset after flashing

The GUI copies the binary using Python and flushes it with `fsync` on both
Linux and Windows, then invokes OpenOCD to reset and run the selected board.
Install OpenOCD for this automatic reset. If it is missing or reset fails,
press the selected board's reset button after the copy completes. DAPLink's
mass-storage write does not reliably start the new image by itself.

Command-line `make flash` only copies and syncs the binary; reset the board
manually afterward. See [blink-led's flashing notes](blink-led/README.md#flash).
The GUI discovers DAPLink volumes using `lsblk` on Linux and by scanning
Windows drive letters for the `DAPLINK` volume label.

## Quick start

From the repository root, after installing the tools above:

```bash
python3 -m venv .venv
source .venv/bin/activate
python -m pip install -r gui/requirements.txt
python -m tkinter
python gui/main.py
```

The Tkinter check opens a small test window; close it before launching the
GUI. On Windows PowerShell, use `python -m venv .venv` and replace the
activation command with `.\.venv\Scripts\Activate.ps1`. You can also run
`.\.venv\Scripts\python.exe` directly for the remaining commands if shell
policy prevents activation.

1. Attach the shield and the appropriate 2-wire or 4-wire test connection,
   then connect the ADICUP3029's DAPLink USB port to the computer.
2. Select a firmware and its **Flash target**, then click **Build && Flash**.
   Wait for the copy and reset to finish.
3. Select that board's **Serial port** and click **Connect**. Keep the firmware
   dropdown matched to the installed image so the correct controls appear.
4. For a 2-wire measurement, connect a known short and capture **Zero** first
   (set the frequency for time-series firmware). Wait for calibration to
   finish, then replace the short with the device under test. The 4-wire
   sweep's zero is an optional offset check; 4-wire time-series has no zero.
5. Start a sweep or click **Start Continuous** at the chosen frequency.
   **Stop** ends a continuous run. Use **Start Recording** before starting
   the measurement to save its samples to CSV.

For a compiler/flashing sanity check, build `blink-led` from the command
line; it is not included in the GUI firmware dropdown:

```bash
make -C blink-led
make -C blink-led flash DAPLINK_MOUNT=/media/your-user/DAPLINK
```

Replace the mount path with your board's actual path and press reset after
flashing. Each firmware directory has its own Makefile; there is no root
Makefile.

## Firmware directories

| Directory | What it does |
|---|---|
| `blink-led/` | Bring-up/toolchain sanity check -- blinks the ADICUP3029's onboard green LED (DS3). No AD5940 involved; start here if the build/flash pipeline itself is in question. |
| `measure-2wire-bioz/` | One 2-wire (CE0/AIN1) frequency sweep per `start`; `zero` first captures a baseline (RLIMIT/isolation-cap offset) to subtract per sweep point. |
| `measure-4wire-bioz/` | One true 4-wire/Kelvin (F+/S+/F-/S-, separate excitation and sense electrode pairs) frequency sweep per `start`. |
| `time-series-bioz/` | Continuous single-frequency 4-wire measurement -- `start <Hz>` streams one impedance sample every 200ms indefinitely; `stop` ends the run. |
| `time-series-bioz-2wire/` | Continuous 2-wire (CE0/AIN1) measurement; throughput depends on excitation frequency (see defaults below). `zero <Hz>` captures a baseline at a given frequency to subtract from a later `start <Hz>` at that same frequency. |

All of them talk over UART0 at **230400 baud, 8N1** and use the same
DAPLink mass-storage flashing convention (`make flash`, or, for the four
measurement firmwares, the GUI's "Build && Flash" button) -- see `blink-led/README.md` for the flashing
quirks (reset-after-flash, etc.) that apply everywhere. What actually comes
over that UART differs by firmware -- see below if you're writing your own
tool against it instead of using `gui/`.

### Measurement defaults

| Firmware | Excitation frequency | Configured trigger rate | Configuration |
|---|---|---|---|
| `measure-2wire-bioz` | 40 linearly spaced points, 1–200 kHz | 5 Hz | [`BIOZStructInit()`](measure-2wire-bioz/main.c) |
| `measure-4wire-bioz` | 40 linearly spaced points, 1–200 kHz | 5 Hz | [`BIAStructInit()`](measure-4wire-bioz/main.c) |
| `time-series-bioz` | Set by `start <Hz>` | 5 Hz (nominally 200 ms/sample) | [`TimeSeriesStructInit()`](time-series-bioz/main.c) |
| `time-series-bioz-2wire` | Set by `start <Hz>` | 5,000 Hz; actual output is slower | [`TimeSeriesStructInit()`](time-series-bioz-2wire/main.c) |

Trigger rate is not a guarantee of sample throughput. The 2-wire time-series
source records approximately 327 samples/s at 10 kHz excitation and 48.8
samples/s at 1 kHz, with frequency-dependent DFT/filter settings. These are
previous hardware observations, not a specification for every setup. See
[its sampling notes](time-series-bioz-2wire/README.md#sample-rate-and-dft-settings).

Zero baselines are stored in MCU RAM and are lost on board reset or power
loss. Sweeps retain one baseline per sweep point. The 2-wire time-series
firmware retains only one baseline: another `zero <Hz>` replaces it, and it
is applied only when `start <Hz>` uses the same parsed frequency. For
example, `50000` and `50000.0` match; `50001` does not. Stopping a run does
not clear the baseline. This offset subtraction is separate from the
firmware's internal RTIA calibration.

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
frequency, log-x) or a time-series view (elapsed seconds since the first
sample), switching automatically based on received sweep lines or binary
sample frames. Time-series timestamps are taken by the host when frames
are decoded; they are not device acquisition timestamps. Each firmware's
specific controls (plain `start`/`zero` for the sweep firmwares, `start <Hz>`/`stop`/`zero <Hz>` for the time-series ones)
are shown/hidden based on the firmware selected.

With multiple evaluation boards connected, choose a board in **Flash target**
(device path/drive letter and unique USB serial), choose the firmware, then
click **Build && Flash**. Select the other board and repeat to flash it next.
The dropdown rescans when opened; **Refresh targets** also rescans after plugging
or unplugging boards. When multiple boards are detected, you must select one.
The GUI checks the selected board's identity before writing and passes its
serial number to OpenOCD so the same board is reset. A disconnected board
causes the operation to stop instead of switching to another board.

**Serial port** independently selects the UART connection used for measurement;
it does not select the flash target. Disconnect before choosing another UART
port and connecting to it.

### Plot history and CSV recording

**Window (s)** selects how much recent time-series data to display (10 seconds
by default). The plot retains at most 12,000 samples, so available history
depends on the actual sample rate; a larger window cannot restore samples
that have left that buffer.

After connecting, click **Start Recording**, choose a CSV path, and start the
measurement. Recording saves incoming measurement rows from that moment;
it does not export earlier plot history. Click **Stop Recording** to close
the file. The file is independent of the plot's history limit, and recording
can span multiple runs. **Stop** stops acquisition but does not itself stop
recording.

CSV columns are `kind`, `sample_num`, `freq_hz`, `time_s`, `real_ohm`,
`imag_ohm`, `mag_ohm`, `phase_deg`, and `calibrated`. Time-series rows use
`kind=sample`; `sample_num` and `time_s` restart for each new run. `calibrated`
is the frame's baseline flag (always 1 for 4-wire time-series). Sweep rows
use `kind=sweep` and leave `sample_num`, `time_s`, and `calibrated` blank.

## `docs/`

Reference material, not something to build: the EVAL-AD5940 user guide,
the AD5940 datasheet, AN-1557 (2-wire bioimpedance theory), and this
shield's own schematic (`Schematic_EVAL-AD5940BIOZ.pdf`) -- useful for
tracing connector pinouts (e.g. which physical cable lead maps to which
chip pin) or understanding the on-board RLIMIT/isolation-cap network that
sits between the cable and the AD5940 in 2-wire mode.
