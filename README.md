# dsnative

Nintendo DS games, recompiled ahead of time into native code, aimed at iPhone, iPad, Mac and
Apple TV.

dsnative is not an emulator in the usual sense. A DS game's ARM9 and ARM7 machine code is
translated **once, at build time**, into C. That C is compiled for the target like any other
source and linked with a runtime that stands in for the DS hardware. Nothing is generated while
the game runs, so it works where apps can't JIT (iOS, iPadOS and tvOS without a debugger
attached).

This is a proof of concept. It runs homebrew from start to finish. A commercial game (Pokémon
Black 2) now runs through boot on both CPUs and on to its first screens, but its 3D and sound
aren't implemented yet: see [Status](#status) and [the roadmap](docs/design.md#roadmap).

Nothing from any game is in this repository. You bring your own dumps.

## Why recompile instead of emulate

The DS's CPUs are 32-bit ARM (ARMv5TE and ARMv4T). Apple silicon can't run 32-bit ARM code at
all, so the Switch projects' approach (patch the game's 64-bit code and run it directly)
doesn't apply here. The code has to be translated, and doing it ahead of time keeps the
advantages of native execution:

- no JIT, so it runs on iOS and tvOS at full speed;
- game code compiled by Clang with full optimization;
- one game, one app, and room for per-game fixes and enhancements later.

## Status

| Part | State |
|---|---|
| ROM reader: header, ARM9/ARM7, Nitro SDK module parameters (to SDK 5), BLZ, autoloads on both CPUs, overlays, file system | done |
| Recompiler: ARMv5TE and ARMv4T, ARM and Thumb, to C | done; matches Unicorn's ARM946 on about 45,000 random snippets |
| Code discovery: calls, branches, jump targets read by a symbolic evaluator (GCC and CodeWarrior switches, ARM and Thumb), interworking veneers, function pointers in literal pools and data, start-up stubs, position-independent stubs found by content | done for the forms seen so far, plus a missing-code feedback loop |
| Overlays sharing an address | handled: each candidate is checked against memory at run time |
| Runtime: dispatcher, CPU modes and banked registers, interrupts, BIOS (high-level), CP15, TCM | done |
| DS hardware: memory map, VRAM banks, DMA, timers, IPC sync and FIFOs, divider, square root, cartridge reads, SPI (power, firmware, touch), keypad | done for what homebrew uses |
| 2D engines: text, affine and bitmap backgrounds, sprites, blending, master brightness, VRAM display, the 3D layer as BG0 | done (no windows, mosaic, bitmap sprites, display capture yet) |
| RTC (ARM7): serial protocol, status, date and time (BCD, 12/24-hour), alarms, frequency, adjust and free registers, reset, /INT through RCNT; starts at host time or `--rtc`, runs on emulated time | done |
| Save memory: EEPROM, FLASH and FRAM on the cartridge SPI bus, save file on disk; the chip comes from `--save-type`, a game-code table, a save file's size, else FLASH 512 KB with a note on stderr | done |
| Sound: 16 channels (PCM8, PCM16, IMA-ADPCM, PSG, noise), capture, mixer at 32768 Hz into a ring for the app, ARM7 BIOS sound calls | done; Pokémon Black 2's sound driver runs |
| 3D engine (first cut): geometry engine (command FIFO and ports, GXSTAT, matrix stacks, clipping, box/position/vector tests, geometry FIFO DMA) and an untextured software rasterizer (Z or W buffer, Gouraud, alpha, wire frames) feeding engine A's BG0; no textures, toon, fog, edge marking or anti-aliasing yet | done for the geometry side, first cut for rendering |
| Wi-Fi | not started |
| Apple app shell (Metal, controllers, touch, two screens) | not started |

## Try it

You need Python 3.10 or later with `capstone` 5, a C compiler and, for the tests,
`arm-none-eabi-gcc` and `unicorn`:

```
pip install "capstone>=5,<6" unicorn      # Homebrew's Python wants a venv for this (PEP 668)
sudo apt install gcc-arm-none-eabi         # or: brew install --cask gcc-arm-embedded

python3 tests/difftest.py                  # the recompiler against Unicorn's ARM946
python3 tests/test_discovery.py            # code discovery on small synthetic images
python3 tests/test_rom.py                  # the ROM reader on synthetic images
python3 tests/test_hw.py                   # build a homebrew ROM, recompile it, run it
```

`test_hw.py` builds a test ROM shaped like a commercial game: a BLZ-compressed ARM9 binary,
Nitro module parameters, an ITCM autoload segment and a file system. It recompiles both CPUs,
runs the result headless for 100 frames and checks the following:

- a CPU workload (CRC, sorting, recursion, jump tables, function pointers, 64-bit division,
  soft-float), compared line by line with the same code compiled for the host;
- the hardware divider and square root against software, and BIOS calls;
- cartridge reads through the card protocol;
- IPC messages to the ARM7, which sleeps in the BIOS between messages;
- VBlank and timer interrupts at the right rates;
- scripted d-pad input moving a sprite.

Both screens are written to `build/hwtest/screen.png`:

<img src="docs/images/hwtest.png" width="256" alt="The test ROM's two screens: a Mandelbrot set drawn into VRAM on top, a scrolled tiled background with a sprite below">

### With your own game

```
python3 tools/ndsinfo.py game.nds --code     # what the ROM holds and how much code is found
python3 -m recomp game.nds -o build/game --build
cd build/game && ./dsn_headless ../../game.nds arm9.bin --frames 600 --png out.png --stats
```

`ndsinfo` works on any DS ROM today. The recompiler and build run, and a commercial game runs
until the first hardware the runtime doesn't implement yet (3D and sound). When it reaches code
that discovery missed, the runtime appends the address to `missing.txt`. Pass that file back with
`--extra missing.txt` for the next build.

Headless options: `--keys 100:a,105:` presses A at frame 100 and releases it at 105 (buttons
`a b x y l r s e` for A, B, X, Y, L, R, Start and Select; `U D W E` for up, down, left and
right), `--touch F:X:Y`, `--save PATH` keeps the save memory in a file (with `--save-type TYPE`
when the game isn't in the built-in table of save chips: see the note the runtime prints), and
`--rtc YYYY-MM-DDTHH:MM:SS` fixes the clock for a reproducible run.

## How it works

1. `tools/nds.py` reads the ROM. It decompresses the ARM9 static image and the overlays, and
   finds the autoload segments (ITCM, DTCM) at the addresses the game copies them to.
2. `recomp/program.py` discovers the code. It walks it from the entry points, following
   calls, branches, jump tables and Thumb/ARM veneers, and takes as candidates any literal-pool
   or data word that points at plausible code. Then it groups the code into C functions.
3. `recomp/isa.py` translates each instruction into C with exact flag, shifter, interworking
   and memory semantics, including the ARMv4/ARMv5 differences.
4. The runtime (`runtime/`) dispatches by guest address. A recompiled function runs until
   control leaves it (a call, a return, an indirect jump, a BIOS call that waits, or the end
   of its time slice) and returns the next guest address. No guest stack frame lives on the
   host stack, so guest threads, interrupts and context switches need no special handling.
5. `runtime/nds.c` and `runtime/gpu2d.c` are the DS. Both CPUs run in slices of 256 bus
   cycles, scanline by scanline.

More in [docs/design.md](docs/design.md): the dispatch model, overlays, timing, testing, and
the road to commercial games, an Apple app and the 3DS.

## Layout

| Path | What |
|---|---|
| `tools/nds.py`, `tools/ndsinfo.py` | ROM reader and inspector |
| `recomp/` | the recompiler (`python3 -m recomp`) |
| `runtime/` | the C runtime: CPU and dispatcher (`cpu.c`), the DS (`nds.c`), 2D (`gpu2d.c`), headless driver |
| `tests/difftest.py` | differential test against Unicorn |
| `tests/test_discovery.py` | code discovery cases the test ROM only hits with some layouts |
| `tests/test_rom.py` | the ROM reader: module parameters and autoload lists of each SDK layout |
| `tests/test_hw.py`, `tests/hw/` | the homebrew end-to-end test |

## Acknowledgements

- [GBATEK](https://problemkaputt.de/gbatek.htm) by Martin Korth: the DS hardware reference
  this runtime follows.
- [melonDS](https://melonds.kuribo64.net/): its direct boot is the reference for what the
  firmware leaves in memory. No melonDS code is used (it is GPLv3).
- [Unicorn](https://www.unicorn-engine.org/) / QEMU: the ARM946 model the recompiler is tested
  against.
- [Capstone](https://www.capstone-engine.org/): disassembly comments in the generated C.
- [nxnative](https://github.com/seedds/nxnative) and N64Recomp: the idea of native,
  ahead-of-time ports with a game profile and a runtime.

Not affiliated with Nintendo. Nintendo DS is a trademark of Nintendo.

## License

MIT, see [LICENSE](LICENSE).
