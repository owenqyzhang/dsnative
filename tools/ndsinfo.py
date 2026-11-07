"""Describe a DS ROM and how much of its code the recompiler finds.

    python3 tools/ndsinfo.py <game.nds> [--code] [--files]

Without --code it reads the header, the ARM9/ARM7 binaries, the Nitro module parameters,
the autoload segments, the overlays and the file system. --code also runs the recompiler's
discovery over both CPUs (static code, autoloads and every overlay) and reports what it
found, without writing any C.
"""
import argparse
import os
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
sys.path.insert(0, ROOT)
from nds import Rom, RomError      # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('rom')
    ap.add_argument('--code', action='store_true', help='run code discovery on both CPUs')
    ap.add_argument('--files', action='store_true', help='list the file system')
    args = ap.parse_args()
    try:
        rom = Rom.load(args.rom)
    except RomError as e:
        sys.exit(f'{args.rom}: {e}')
    h = rom.header
    unit = {0: 'DS', 2: 'DS + DSi (enhanced)', 3: 'DSi only'}.get(rom.unitcode, f'unit code {rom.unitcode:#x}')
    print(f'{rom.title}  {rom.gamecode}  maker {rom.maker}  ({unit}), {len(rom.data) / 1048576:.1f} MB')
    print(f'ARM9  rom {h["arm9_rom"]:#09x}  ram {h["arm9_ram"]:#010x}  entry {h["arm9_entry"]:#010x}  '
          f'{h["arm9_size"]:#x} bytes' + (f', BLZ-compressed: {len(rom.arm9):#x} decompressed' if rom.arm9_compressed else ''))
    print(f'ARM7  rom {h["arm7_rom"]:#09x}  ram {h["arm7_ram"]:#010x}  entry {h["arm7_entry"]:#010x}  {h["arm7_size"]:#x} bytes')
    p = rom.params
    if p:
        sdk = p.sdk_version
        print(f'Nitro SDK module parameters at ARM9+{p.offset:#x}: SDK {sdk >> 24}.{(sdk >> 16) & 0xFF}.{sdk & 0xFFFF:x}, '
              f'static bss {p.bss_start:#010x}-{p.bss_end:#010x}')
        for s in rom.autoloads:
            print(f'    autoload {s.addr:#010x}  {len(s.data):#x} bytes + {s.bss:#x} bss')
    else:
        print('no Nitro SDK module parameters (homebrew?)')
    for cpu, ovs in ((9, rom.overlays9), (7, rom.overlays7)):
        if not ovs:
            continue
        comp = sum(1 for o in ovs if o.compressed)
        regions = sorted({o.ram_addr for o in ovs})
        total = sum(o.ram_size for o in ovs)
        print(f'ARM{cpu} overlays: {len(ovs)} ({comp} compressed), {total / 1024:.0f} KB of code and data, '
              f'loaded at {len(regions)} distinct addresses from {regions[0]:#010x} to {regions[-1]:#010x}')
    print(f'file system: {len(rom.files)} files, {len(rom.fat)} FAT entries')
    if args.files:
        for name, fid in sorted(rom.files.items()):
            s, e = rom.fat[fid]
            print(f'    {fid:5d} {e - s:9d}  {name}')
    if args.code:
        from recomp.program import Program
        for cpu, arch, prefix in ((9, 5, 'g9'), (7, 4, 'g7')):
            t = time.time()
            segs = [(s.name, s.addr, s.data, s.kind) for s in rom.segments(cpu)]
            prog = Program(segs, arch=arch, prefix=prefix)
            prog.discover([(h[f'arm{cpu}_entry'], False)])
            st = prog.stats()
            thumb = sum(1 for k in prog.insns if k[2])
            print(f'ARM{cpu} code: {st["functions"]} functions, {st["instructions"]} instructions '
                  f'({thumb} Thumb), {st["rejected_candidates"]} pointer candidates rejected, '
                  f'{len(prog.warnings)} warnings, {time.time() - t:.1f} s')
            for w in prog.warnings[:8]:
                print(f'    {w}')


if __name__ == '__main__':
    main()
