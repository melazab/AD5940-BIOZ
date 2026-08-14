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
quirks (reset-after-flash, etc.) that apply everywhere.

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
