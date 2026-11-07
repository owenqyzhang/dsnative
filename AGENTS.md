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
  - `python3 tests/test_hw.py`: the homebrew ROM end to end (suite output against the host,
    hardware results, screens in `build/hwtest/screen.png`).
- Where DS hardware behaves differently from Unicorn (unaligned accesses force alignment on the
  ARM9, STR of the PC stores PC+12), follow GBATEK and keep the difference out of the random
  generators, with a comment saying so.
- Never add game data (ROMs, dumps, extracted assets, screenshots of commercial games) to the
  repository. Tests use the homebrew ROM built from `tests/hw/`.
- Generated code and builds go to `build/` (ignored).
