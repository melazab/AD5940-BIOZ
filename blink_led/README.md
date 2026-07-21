# blink_led

A from-scratch, bare-metal blink example for the **ADuCM3029** (Cortex-M3) on
an **EVAL-ADICUP3029** board, driving the onboard LED **DS3** (green).

No vendor IDE (CCES/IAR/Keil) is used anywhere in the build. The startup
code, linker script, and register access are hand-written and built with a
plain `arm-none-eabi-gcc` + `make` toolchain.

## Dependencies

- `arm-none-eabi-gcc` / `arm-none-eabi-objcopy` / `arm-none-eabi-size` --
  the cross toolchain used to build and produce the flashable `.bin`.
- `make`.
- `bear` (optional) -- generates `compile_commands.json` from an actual
  build so clangd (e.g. Mason-installed in nvim) resolves the cross-compile
  flags and target headers correctly instead of guessing. Regenerate with
  `make clean && bear -- make` any time flags or files change; nothing else
  needs to reference this file.
- `openocd` (optional, `apt install openocd`) -- used here purely for
  debugging/inspection (halt, read memory/registers, single-step) over the
  board's CMSIS-DAP interface. It does **not** flash: this OpenOCD build has
  no flash driver for the ADuCM3029's flash controller (only for the
  unrelated ADuCM360), so programming is still done via DAPLink's
  drag-and-drop MSD interface below.

## Build

```
make
```

Produces `blink_led.elf` (with debug info) and `blink_led.bin` (raw image
for flashing).

## Flash

The board's DAPLink interface chip shows up as a USB mass-storage drive
(`/media/$USER/DAPLINK` here). Copying a `.bin` onto it flashes the target:

```
make flash
```

This does `cp blink_led.bin $(DAPLINK_MOUNT)/` followed by `sync` --
the `sync` matters. A plain `cp` can return before the data actually leaves
the page cache over USB, in which case DAPLink never sees a complete file
and silently keeps running whatever was flashed before (this cost real
debugging time -- see "Flash security block" below for how it was caught).
Override the mount point if needed: `make flash DAPLINK_MOUNT=/media/you/DAPLINK`.

A real flash cycle briefly unmounts and remounts the drive; if it doesn't,
the write likely didn't take.

## Debug / verify with OpenOCD

```
cd openocd
openocd -f aducm3029.cfg -c "init" -c "reset halt" -c "reg pc" -c "mdw 0x0 8"
```

Useful for confirming the chip is actually executing your code (check `pc`
is inside `.text`, not stuck elsewhere) and that flash at `0x0` reads back
the expected vector table (`0x20004000` initial SP, then `Reset_Handler`'s
address). `aducm3029.cfg` connects over SWD via the board's CMSIS-DAP
interface; no flash bank is declared since there's no driver for it.

## Layout

- `startup.c` -- vector table and reset handler. Copies `.data` from flash
  to SRAM, zeros `.bss`, calls `main()` directly. No libc startup, no
  syscall stubs (`-nostartfiles -nodefaultlibs -lgcc` in the Makefile).
- `linker.ld` -- flash/SRAM memory map and section placement.
- `registers.h` -- only the GPIO, watchdog, and clock-gating registers this
  program touches, hand-written and cross-checked against Analog Devices'
  own CMSIS device header (as shipped in mbed-os) and their EVAL-ADICUP3029
  example firmware, not guessed from the datasheet blind.
- `main.c` -- disables the watchdog, ensures the GPIO clock is enabled,
  configures P2.00 as an output, toggles it in a loop.
- `openocd/aducm3029.cfg` -- minimal SWD/CMSIS-DAP target script for
  debugging (see above).

## Hardware notes learned the hard way

- **LED pin.** DS3 (green) is on **P2.00** (GPIO32 in ADI's flat numbering:
  GPIO0-15=Port0, 16-31=Port1, 32-47=Port2). This was read directly off the
  ADICUP3029 schematic (sheet 2, "ADUCM3029 CONNECTIONS": pin 36 of the
  ADuCM3029 is GPIO32, net `3029_LED1`, which drives DS3 through R23). An
  earlier guess based on mbed's `EV-COG-AD3029LZ` target (`P2.02`/`P2.10`)
  was wrong -- that target's LED wiring doesn't match how this baseboard
  wires its own DS3/DS4. DS4 (blue) is the other one, on **P1.15**
  (GPIO31).
- **Watchdog and GPIO clock gating.** The ADuCM3029 boots with the
  watchdog already running, and the GPIO peripheral clock can be gated
  off. `main()` unconditionally clears both (`WDT0->CTL &= ~WDT_CTL_EN`,
  `CLKG0_CLK->CTL5 &= ~CLKG_CLK_CTL5_GPIOCLKOFF`) -- harmless if they
  weren't actually the issue, since clearing an already-clear bit is a
  no-op.
- **Flash security block at 0x180.** This was the actual root cause of the
  LED never blinking, and took the longest to find. The ADuCM3029 has a
  factory kernel/boot ROM that runs before jumping to user flash, and it
  validates the image first -- specifically, it checks a fixed structure
  at flash offset `0x180` (8 words: four `0xFFFFFFFF`, a magic marker
  `0xA79C3203`, a "last CRC page" value, then two more `0xFFFFFFFF`). Without
  it, OpenOCD showed the CPU permanently parked in the kernel at a fixed
  address (`0x00040102`, just past the 256 KB flash region) no matter what
  was actually in `.text` -- confirmed by watching `pc` stay identical
  across repeated `resume`/`halt` cycles over several seconds, i.e. never
  moving, not just "caught early." Adding the block (see `.security_options`
  in `linker.ld`) fixed it immediately: `pc` landed inside `.text` and the
  GPIO output register was observed toggling in step with the blink loop.
  mbed's own ADuCM3029 target ships a *differently laid out* version of
  this same structure and boots fine, which is what led to first assuming
  it was optional -- it isn't; it just doesn't have to be byte-identical
  to ADI's CCES-generated layout, only present at the right offset.
