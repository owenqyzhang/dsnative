# Notes for agents

- dsnative recompiles DS games (ARMv5TE ARM9 + ARMv4T ARM7) to C ahead of time and runs them
  on a C runtime that stands in for the DS. [README.md](README.md) is the overview,
  [docs/design.md](docs/design.md) the internals and the roadmap.
- Before a change to `recomp/isa.py`, `recomp/program.py` or `runtime/`, run the tests and keep
  them passing:
  - `python3 tests/difftest.py --cases 8192 --seed N` for a few seeds: instruction semantics
    against Unicorn's ARM946. A semantic change needs a generator in it if none covers it.
  - `python3 tests/test_discovery.py`: code discovery on synthetic images, for cases that
    depend on how a toolchain lays out the test ROM. A discovery fix needs a case in it.
  - `python3 tests/test_rom.py` after a change to `tools/nds.py`: module parameters and
    autoload lists in each SDK's layout.
  - `python3 tests/test_hw.py`: the homebrew ROM end to end (suite output against the host,
    hardware results, screens in `build/hwtest/screen.png`).
  - `python3 tests/test_units.py`: C unit tests of the runtime (`tests/unit/test_*.c`);
    `DSN_SANITIZE=1` builds them under ASan and UBSan.
    A change to the 3D rasterizer's per-pixel path or the 2D renderer needs `python3 tools/bench.py --vs DIR`
    against the previous build (the hashes must be equal; `tests/unit/test_threads.c` checks the
    thread counts).
  - `python3 tests/test_interp.py` after a change to `runtime/interp.c` or the miss path in `cpu.c`;
    `difftest.py` also runs every snippet interpreted and mixed, so interpreter semantics must follow
    `recomp/isa.py`.
  - `python3 tests/test_app.py` after a change to `app/`, `tools/build_app.py` or the runtime API
    the app uses (`nds.h`, `nds_audio_take`, the options): the Mac app end to end on the test ROM
    and `swift test`. The iOS and tvOS builds are checked by hand with `tools/build_app.py --platform
    ios-sim|tvos-sim --run` (see `app/README.md`).
- Hardware subsystems live in files of their own: `runtime/rtc.c`, `save.c`, `sound.c`,
  `gpu3d.c`, each with a header holding its state (a member of `Nds`). `nds.c` gives them the
  first look at every I/O access (`<x>_io_read`/`<x>_io_write`), calls `<x>_init`, the
  per-line hooks and `<x>_shutdown`, and passes them headless options (`<x>_option`). Their
  homebrew tests go in `tests/hw/<x>_9.c` / `<x>_7.c` (built automatically; ARM9 hooks run
  after `DONE`, ARM7 ones answer `arm7_command`), with the expected lines and any headless
  options in `tests/hw/expect_<x>.txt`.
- Where DS hardware behaves differently from Unicorn (unaligned accesses force alignment on the
  ARM9, STR of the PC stores PC+12), follow GBATEK and keep the difference out of the random
  generators, with a comment saying so.
- Play-testing a commercial game headless: `tools/b2_script.py` and `tools/random_keys.py` make
  seeded `--keys` scripts (Pokémon Black 2's menus and a random walk), `--touch F:X:Y` drives the
  bottom screen, `--png` writes the last frame (run again with another `--frames` for others),
  `--dump F` prints the 2D/3D registers and the polygon list, `--hash` compares two builds. Work on
  a copy of a save, never the original.
- Never add game data (ROMs, dumps, extracted assets, screenshots of commercial games) to the
  repository. Tests use the homebrew ROM built from `tests/hw/`.
- Generated code and builds go to `build/` (ignored).
