# SalsaDS

A Nintendo DS emulator for the PlayStation 3 (homebrew, HEN/CFW), designed
around the Cell processor: the PPE runs the two ARM CPUs, the SPEs do the
vector work (2D composition, 3D rasterization, sound mixing, filters) and the
RSX presents the two screens. It is a sibling of
[Salsa64](https://github.com/ricowp81/Salsa64) and reuses its build
infrastructure and its two MIT libraries, `lib/rsxkit` and `lib/spukit`.

**Status: experimental preview.** SalsaDS is early software. It does **not**
reach full speed in most games yet, a lot of optimization and many
improvements are still missing, and you should expect graphical and audio
glitches. It has been tested on a real PS3 with HEN; it has not been tested
on CFW. Games seen running there: New Super Mario Bros., Mario Kart DS and
Super Mario 64 DS (no speed is promised for any of them). Source code lives
in this repository and the `.pkg` packages are published in GitHub Releases.

## Status by phase

| Phase | What | State |
|---|---|---|
| 0 | Feasibility tests on a real PS3 (byte-swapped memory access, flags, SPE 3D rasterizer, SPE 2D, two-screen output, ARM to PowerPC JIT) | done, numbers in `docs/fase0.md` |
| 1 | Core with interpreters: ARM9 (ARMv5TE) and ARM7 (ARMv4T), memory map, IRQ, timers, DMA, IPC, cartridge, SPI with generated firmware, HLE BIOS and direct boot | done |
| 2 | 2D engines A and B and sound on SPEs (16 channels), compositor with per-line register log | done; runs on the PS3 |
| 3 | 3D: geometry engine and SPE rasterizer | working, with known inaccuracies; still being refined |
| 4 | ARM/Thumb to PPC64 recompiler (JIT) for the ARM9 and the ARM7 | working in the main package; performance work in progress |
| 5 | HD internal resolution, filters, save states, profiling | not started |

The day-to-day log, with what was measured and what is pending, is
`docs/estado.md` (Spanish). The project guide is `docs/proyecto.md`.

## Basic usage

1. Install the `.pkg` from the latest release on a PS3 with HEN (or CFW,
   untested). The package is called "SalsaDS".
2. Copy dumps of games you own (your own backups, never downloads) to the PS3
   into `/dev_hdd0/salsads/`, with any file name ending in `.nds`. The
   cartridge encryption is not emulated, so each dump has to be a decrypted
   `.nds` image.
3. Start the program. It shows a list of the `.nds` files in that folder
   (d-pad or left stick to move, L1/R1 to jump a page, X to open a game, O to
   leave SalsaDS). If there is no `.nds` file it offers a built-in load test
   instead. The last game you opened is remembered.
4. Inside a game, hold SELECT + START for 1 second to return to the list.
   Saves (the cartridge save chip) are kept per game in
   `/dev_hdd0/salsads/saves/<game code>.sav`; the log is
   `/dev_hdd0/salsads/nucleo.log`. The save chip type (EEPROM, FRAM or
   Flash, and its size) is chosen automatically per game (built-in table,
   then detection from the game's own SPI commands) and logged; to force it,
   add a line like `IRD=flash512k` to `/dev_hdd0/salsads/guardado.cfg`
   (see `docs/guardado.md`, in Spanish).

Other options, all plain files in `/dev_hdd0/salsads/`:

- `controles.cfg`: remaps the DualShock 3 buttons and the touch pointer. By
  default the sticks, the d-pad, L2 (screen layout) and R2 (touch) work as
  described in `src/frontend/dspad.h`.
- `rom-paged`: an empty file; reads the ROM in pages instead of loading it
  whole (done automatically for ROMs larger than 64 MB).
- `audio.cfg`: a line `original = si` makes the audio output quantize to 10 bits like the original console.

## Building

Everything builds inside Docker (colima or Docker Desktop). The toolchain
image is derived from the Salsa64 one (ps3toolchain, PSL1GHT and the NVIDIA Cg
Toolkit, which only exists for x86-64; on Apple Silicon Docker must be able to
run `linux/amd64` containers).

```sh
# 1. Base image: build it from the Salsa64 repository (about an hour), or set SALSADS_BASE
# 2. SalsaDS toolchain image (seconds if the base exists)
tools/docker-image.sh
# 3. Build the PS3 programs
tools/build.sh
# 4. Host unit tests, little-endian and big-endian (qemu-ppc64)
tools/docker-image-ppc64.sh
tools/build.sh test-host
```

Test ROMs (`tests/roms/src`, own code) are built with
[BlocksDS](https://github.com/blocksds/sdk) through `tools/build-roms.sh`; the
built ones live in `tests/roms/homebrew/` (see `AVISOS.md` there for the
licenses of what BlocksDS links into them). `tools/cpurun.sh` runs a ROM on the
interpreters natively on a Mac. Running on a real PS3 uses
`tools/deploy-ps3.sh` (FTP, address in the git-ignored `tools/ps3.conf`) and
the UDP log receiver `tools/udplog.py`; RPCS3 can be used for quick checks but
is not valid for performance numbers.

## Legal notice

SalsaDS is not affiliated with, endorsed or sponsored by Nintendo or Sony.
"Nintendo DS" and "PlayStation" are trademarks of their respective owners.

This repository does not contain, and will not accept, commercial ROMs, DS or
DSi BIOS images, firmware dumps, encryption key tables, or any other material
copyrighted by Nintendo or Sony, nor any part of Sony's official PS3 SDK. The
emulator boots games directly (HLE of the BIOS calls) and **generates** the
firmware user data itself. Use it only with dumps of games you own. The only
ROMs in the repository are homebrew test programs written for this project
(`tests/roms/src`).

## License

Copyright (C) 2026 ricowp81.

SalsaDS is free software under the **GNU General Public License, version 2 or
(at your option) any later version** ([LICENSE](LICENSE)). `lib/rsxkit/` and
`lib/spukit/` are under the **MIT license** (their own `LICENSE` files). Third
party sources and references are listed in
[docs/THIRD_PARTY.md](docs/THIRD_PARTY.md); no code from melonDS or NooDS
(GPL-3.0) is used.

## Support

If you enjoy SalsaDS, you can buy me a coffee at
[buymeacoffee.com/ricowp81](https://buymeacoffee.com/ricowp81). Thanks!

## AI disclosure

SalsaDS is an AI-assisted project. The emulator itself is tested on real PS3 hardware.
