#!/usr/bin/env python3
"""Benchmark harness: how fast does a recompiled game run, and where does the time go?

    python3 tools/bench.py --homebrew                       # the test ROM (built from tests/hw)
    python3 tools/bench.py --homebrew --workload            # its CPU benchmark: guest speed per kernel
    python3 tools/bench.py --rom game.nds --save game.sav --frames 900 --press a:100
    python3 tools/bench.py --rom game.nds ... --build       # recompile and build first (make -jN)
    python3 tools/bench.py ... --profile 700:900            # where the time goes in frames 700-899
    python3 tools/bench.py ... --write-expect FILE          # keep the hashes of this build ...
    python3 tools/bench.py ... --expect FILE                # ... and check another build against them
    python3 tools/bench.py ... --dir A --vs B               # runs of both builds interleaved, speed-up and hashes

It runs dsn_headless (--bench 1: wall and CPU time of every frame) a few times from the same
start (a fresh copy of the save, the clock fixed with --rtc) and reports emulated frames per
second as the best of the runs, per window of frames, with the frame times that matter for
real time (the median, the 99th percentile and the worst frame against the 16.7 ms a frame
may take). The machine is shared with other work, so each frame takes the minimum over the
runs, and CPU time is shown beside wall time.

--workload starts the test ROM's CPU benchmark (tests/hw/bench_kernels.h: CRC, quicksort, recursion, matrix
multiply, copies, a jump-table VM, hardware registers; START held makes bench_9.c run it) and reports each
kernel: its checksum against a host build of the same source, and how fast the ARM9 ran it. The ARM9 is busy
the whole time, 1,120,380 guest cycles (about one instruction each) a frame, so the host time per frame is
the cost of running guest code at full load, together with the screen rendering every frame needs.

--profile A:B runs the sampling profiler in the driver (SIGPROF every millisecond of CPU time
while frames A..B-1 run, the program counter and the link register of each sample), merges the
runs, maps the addresses to functions with nm and sorts them into the parts of the machine:
the recompiled ARM9 and ARM7 code, the dispatcher, the high-level BIOS, the memory slow path
and I/O, the timers and DMA, the 2D renderer, the 3D geometry engine and rasterizer, display
capture and sound. Samples inside libc (memset, memcpy) count for the function that called
it. --sample runs macOS's `sample` on a run too, as a cross-check.

--expect checks the hashes of both screens, the memory and CPU state and the audio at fixed
frames (dsn_headless --hash): a change that must not alter the emulation leaves them equal.
The file is local: it holds hashes of a game's frames, so keep it under build/.

Only macOS is handled for --profile and --sample (nm and sample). No game data is stored here.
"""
import argparse
import bisect
import collections
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
REAL_FPS = 59.8261          # the DS: 33513982 bus cycles / (263 lines * 2130 cycles)
FRAME_MS = 1000.0 / REAL_FPS
RTC = '2026-01-01T00:00:00'  # a fixed clock, or a game that shows the time is not reproducible
DEFAULT_CFLAGS = '-O2 -g -fno-omit-frame-pointer'


def sh(cmd, **kw):
    return subprocess.run(cmd, check=True, **kw)


# ---------------------------------------------------------------- building
def build_homebrew(out):
    rom_dir = os.path.join(ROOT, 'build', 'bench', 'hwrom')
    sh([sys.executable, os.path.join(ROOT, 'tests', 'hw', 'build.py'), rom_dir], stdout=subprocess.DEVNULL)
    return os.path.join(rom_dir, 'test.nds')


def build_game(rom, out, jobs, cflags, recompile=True):
    t0 = time.time()
    if recompile or not os.path.exists(os.path.join(out, 'Makefile')):
        sh([sys.executable, '-m', 'recomp', rom, '-o', out], cwd=ROOT, stdout=subprocess.DEVNULL)
    t1 = time.time()
    sh(['make', '-C', out, f'-j{jobs}', '--no-print-directory', f'CFLAGS={cflags}'], stdout=subprocess.DEVNULL)
    t2 = time.time()
    gen = os.path.join(out, 'gen')
    csize = sum(os.path.getsize(os.path.join(gen, f)) for f in os.listdir(gen) if f.endswith('.c'))
    osize = sum(os.path.getsize(os.path.join(gen, f)) for f in os.listdir(gen) if f.endswith('.o'))
    lib = os.path.join(out, 'dsn_headless')
    return {'recompile_s': round(t1 - t0, 1), 'make_s': round(t2 - t1, 1), 'generated_c_mb': round(csize / 1e6, 1),
            'generated_o_mb': round(osize / 1e6, 1), 'binary_mb': round(os.path.getsize(lib) / 1e6, 1)}


# ---------------------------------------------------------------- running
def keys_script(spec, frames):
    """--press a:100 -> the button (the driver's letter: a b x y l r s e, U D W E) held for 5 frames every 100 frames."""
    if not spec:
        return ''
    m = re.fullmatch(r'([a-zA-Z]):(\d+)', spec)
    if not m:
        sys.exit(f'--press wants BUTTON:EVERY with the driver\'s button letter, e.g. a:100 (got {spec})')
    key, every = m.group(1), int(m.group(2))
    return ','.join(f'{f}:{key},{f + 5}:' for f in range(every, frames, every))


def run_once(exe, rom, a9, frames, keys, save_src, extra, workdir, prof=None, hashes=None, tag='run'):
    """One run of the headless driver; returns the parsed output."""
    cmd = [exe, rom, a9, '--frames', str(frames), '--bench', '1', '--stats', '--rtc', RTC]
    stale = os.path.join(os.path.dirname(workdir), 'save.sav')      # the test ROM's save test keeps one
    if os.path.exists(stale):
        os.remove(stale)
    if keys:
        cmd += ['--keys', keys]
    if save_src:
        sav = os.path.join(workdir, f'{tag}.sav')
        shutil.copyfile(save_src, sav)       # the user's save is never written
        cmd += ['--save', sav]
    if hashes:
        cmd += ['--hash', hashes]
    if prof:
        cmd += ['--prof', f'{prof[0]}:{prof[1]}:{os.path.join(workdir, tag + ".prof")}']
    cmd += extra
    out_f, err_f = os.path.join(workdir, tag + '.out'), os.path.join(workdir, tag + '.err')
    with open(out_f, 'w') as fo, open(err_f, 'w') as fe:
        p = subprocess.Popen(cmd, stdout=fo, stderr=fe, cwd=workdir)
        _, status, ru = os.wait4(p.pid, 0)
    res = {'status': os.waitstatus_to_exitcode(status), 'rusage_cpu': ru.ru_utime + ru.ru_stime,
           'maxrss_mb': ru.ru_maxrss / 1e6, 'frames': [], 'stats': {}, 'hashes': {}}
    prev_w = prev_c = 0.0
    for line in open(err_f).read().splitlines():
        m = re.match(r'bench frame (\d+) wall ([\d.]+) cpu ([\d.]+)', line)
        if m:
            w, c = float(m.group(2)), float(m.group(3))
            res['frames'].append((w - prev_w, c - prev_c))
            prev_w, prev_c = w, c
            continue
        m = re.match(r'stats ARM(\d) dispatches (\d+) slow_lookups (\d+) slow_reads (\d+) slow_writes (\d+)', line)
        if m:
            res['stats'][int(m.group(1))] = dict(zip(('dispatches', 'slow_lookups', 'slow_reads', 'slow_writes'),
                                                     map(int, m.groups()[1:])))
            continue
        m = re.match(r'frames (\d+), ARM9: (\d+) cycles', line)
        if m:
            res['emulated'] = int(m.group(1))
        if line.startswith('[ARM') or 'no recompiled code' in line:
            res.setdefault('errors', []).append(line)
    for line in open(out_f).read().splitlines():
        if line.startswith('hash '):
            f = int(line.split()[1])
            res['hashes'][f] = line
    res['out'] = out_f
    res['prof_file'] = os.path.join(workdir, tag + '.prof') if prof else None
    return res


# ---------------------------------------------------------------- the CPU workload (tests/hw/bench_*)
GUEST_CYCLES_PER_FRAME = 2 * 263 * 2130     # ARM9 cycles in a frame (two per bus cycle); one cycle is about one instruction


def host_reference():
    """The checksums the benchmark kernels must give: the same source built for the host."""
    exe = os.path.join(ROOT, 'build', 'bench', 'bench_host')
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    sh(['cc', '-O2', '-o', exe, os.path.join(ROOT, 'tests', 'hw', 'bench_host.c')])
    return dict(l.split() for l in subprocess.run([exe], capture_output=True, text=True, check=True).stdout.splitlines())


def workload_report(runs, pf):
    """Per kernel: checksum against the host, frames it ran for, and the host time those frames took."""
    ref = host_reference()
    got, spans = {}, {}
    for l in open(runs[0]['out']).read().splitlines():
        m = re.match(r'\[debug\] (bench_\w+) ([0-9a-f]{8})$', l)
        if m:
            got[m.group(1)] = m.group(2)
        m = re.match(r'\[debug\] benchspan (\w+) ([0-9a-f]{8}) ([0-9a-f]{8})$', l)
        if m:
            spans[m.group(1)] = (int(m.group(2), 16), int(m.group(3), 16))
    ok = True
    print(f'   {"kernel":<10} {"frames":>6} {"host ms/frame":>14} {"Mcycles/s":>10} {"x real":>7}   checksum')
    total_t = total_f = 0
    res = {}
    for name in ref:
        k = name[len('bench_'):]
        status = 'ok' if got.get(name) == ref[name] else f'WRONG (host {ref[name]}, got {got.get(name)})'
        ok &= status == 'ok'
        if k not in spans:
            print(f'   {k:<10} did not finish: {status}')
            continue
        a, b = spans[k]
        # frame index = VBlank count (the test ROM enables the interrupt in its first frame)
        t = sum(x[0] for x in pf[a:b])
        f = b - a
        total_t += t
        total_f += f
        res[k] = {'frames': f, 'ms_per_frame': 1000 * t / f, 'mcycles_s': f * GUEST_CYCLES_PER_FRAME / t / 1e6}
        print(f'   {k:<10} {f:>6} {1000 * t / f:>14.3f} {res[k]["mcycles_s"]:>10.0f} {res[k]["mcycles_s"] * 1e6 / (GUEST_CYCLES_PER_FRAME * REAL_FPS):>7.1f}   {status}')
    if total_f:
        print(f'   {"all":<10} {total_f:>6} {1000 * total_t / total_f:>14.3f} {total_f * GUEST_CYCLES_PER_FRAME / total_t / 1e6:>10.0f}')
    return ok, res


# ---------------------------------------------------------------- reporting
def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(len(v) * p / 100))]


def window_stats(per_frame, a, b):
    """per_frame: list of (wall, cpu) minima over the runs. Returns fps and frame time figures for [a, b)."""
    w = [x[0] for x in per_frame[a:b]]
    c = [x[1] for x in per_frame[a:b]]
    if not w:
        return None
    n = len(w)
    return {'frames': n, 'wall_fps': n / sum(w), 'cpu_fps': n / sum(c) if sum(c) else 0,
            'median_ms': statistics.median(w) * 1000, 'p99_ms': pct(w, 99) * 1000, 'max_ms': max(w) * 1000,
            'x_real': n / sum(w) / REAL_FPS}


def best_frames(runs):
    n = min(len(r['frames']) for r in runs)
    return [(min(r['frames'][i][0] for r in runs), min(r['frames'][i][1] for r in runs)) for i in range(n)]


def print_report(name, runs, windows, build_info=None):
    pf = best_frames(runs)
    n = len(pf)
    print(f'== {name}: {n} frames, best of {len(runs)} run(s); real time is {REAL_FPS} fps ({FRAME_MS:.2f} ms a frame)')
    if build_info:
        print('   build: ' + ', '.join(f'{k} {v}' for k, v in build_info.items()))
    allw = window_stats(pf, 0, n)
    print(f'   all: {allw["wall_fps"]:.0f} fps wall ({allw["x_real"]:.1f}x real time), {allw["cpu_fps"]:.0f} fps CPU; '
          f'frame median {allw["median_ms"]:.2f} ms, p99 {allw["p99_ms"]:.2f} ms, worst {allw["max_ms"]:.2f} ms')
    print(f'   {"frames":>11} {"wall fps":>9} {"x real":>7} {"cpu fps":>8} {"median":>8} {"p99":>8} {"worst":>8}   (ms per frame)')
    for a, b in windows:
        s = window_stats(pf, a, min(b, n))
        if s:
            print(f'   {a:>5}-{min(b, n):<5} {s["wall_fps"]:>9.0f} {s["x_real"]:>7.1f} {s["cpu_fps"]:>8.0f} '
                  f'{s["median_ms"]:>8.2f} {s["p99_ms"]:>8.2f} {s["max_ms"]:>8.2f}')
    worst = sorted(range(n), key=lambda i: -pf[i][0])[:6]
    print('   heaviest frames: ' + ', '.join(f'#{i} {pf[i][0] * 1000:.2f} ms' for i in sorted(worst)) +
          f'  (4x real time allows {FRAME_MS / 4:.2f} ms a frame; {sum(1 for x in pf if x[0] * 1000 > FRAME_MS / 4)} frames are over)')
    walls = [sum(x[0] for x in r['frames']) for r in runs]
    print(f'   run to run (all frames): wall {min(walls):.3f} s best, {max(walls):.3f} s worst')
    # dispatcher and slow-path counters (identical in every run)
    st = runs[0]['stats']
    if st:
        for k in (9, 7):
            d = st[k]
            hit = 100.0 * (1 - d['slow_lookups'] / d['dispatches']) if d['dispatches'] else 0
            print(f'   ARM{k}: {d["dispatches"] / n:,.0f} dispatches/frame, cache hit rate {hit:.2f}%, '
                  f'slow reads {d["slow_reads"] / n:,.0f}/frame, slow writes {d["slow_writes"] / n:,.0f}/frame')
    if runs[0].get('errors'):
        print('   NOTE: the emulation stopped: ' + runs[0]['errors'][0])


# ---------------------------------------------------------------- profiling
def nm_text_symbols(exe):
    """Sorted (address, name) of the functions in exe, and the __text range, for macOS."""
    out = subprocess.run(['nm', '-n', exe], capture_output=True, text=True, check=True).stdout
    syms = []
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3 and p[1] in 'tT':
            syms.append((int(p[0], 16), p[2][1:] if p[2].startswith('_') else p[2]))
    syms.sort()
    otool = subprocess.run(['otool', '-l', exe], capture_output=True, text=True, check=True).stdout
    m = re.search(r'sectname __text\n\s+segname __TEXT\n\s+addr (0x[0-9a-f]+)\n\s+size (0x[0-9a-f]+)', otool)
    lo, size = int(m.group(1), 16), int(m.group(2), 16)
    return syms, (lo, lo + size)


def object_functions(outdir):
    """Which runtime object (rt_gpu2d.o, ...) each function name lives in."""
    where = {}
    for f in sorted(os.listdir(outdir)):
        if f.startswith('rt_') and f.endswith('.o'):
            for line in subprocess.run(['nm', os.path.join(outdir, f)], capture_output=True, text=True).stdout.splitlines():
                p = line.split()
                if len(p) == 3 and p[1] in 'tT':
                    where[p[2][1:] if p[2].startswith('_') else p[2]] = f
    return where


TWO_D_HELPERS = {'vram_engine_ptr'}
MACHINE_PARTS = [       # rt_nds.o, by function name
    (r'read_slow|write_slow|dsr_read_slow|dsr_write_slow', 'memory slow path (read/write_slow)'),
    (r'io_read|io_write|sub_io|fifo|div_|sqrt_|spi_|card_|ipc', 'I/O registers'),
    (r'timer', 'timers'),
    (r'dma', 'DMA'),
]
CAT_ORDER = ['ARM9 recompiled code', 'ARM7 recompiled code', 'dispatcher (run loop, lookup)', 'BIOS (high level)',
             'memory slow path (read/write_slow)', 'I/O registers', 'timers', 'DMA', 'machine, other',
             '2D line renderer', '3D geometry engine', '3D rasterizer', 'display capture', 'sound mixing',
             'save, RTC, Wi-Fi, firmware', 'driver (hash, audio drain)', 'libc / kernel, caller unknown', 'unresolved']


def classify(name, where):
    if name.startswith('g9_'):
        return 'ARM9 recompiled code'
    if name.startswith('g7_'):
        return 'ARM7 recompiled code'
    obj = where.get(name)
    if name in TWO_D_HELPERS:      # in nds.c, but only the 2D renderer calls them (per pixel)
        return '2D line renderer'
    if obj == 'rt_cpu.o':
        if name in ('dsr_run', 'lookup', 'entry_matches', 'lookup_pic', 'enter_irq', 'irq_pending', 'in_bios'):
            return 'dispatcher (run loop, lookup)'
        return 'BIOS (high level)'
    if obj == 'rt_nds.o':
        for rx, cat in MACHINE_PARTS:
            if re.search(rx, name):
                return cat
        return 'machine, other'
    return {'rt_gpu2d.o': '2D line renderer', 'rt_gpu3d.o': '3D geometry engine', 'rt_gpu3d_render.o': '3D rasterizer',
            'rt_capture.o': 'display capture', 'rt_sound.o': 'sound mixing', 'rt_rtc.o': 'save, RTC, Wi-Fi, firmware',
            'rt_save.o': 'save, RTC, Wi-Fi, firmware', 'rt_wifi.o': 'save, RTC, Wi-Fi, firmware',
            'rt_firmware.o': 'save, RTC, Wi-Fi, firmware', 'rt_dsi.o': 'save, RTC, Wi-Fi, firmware',
            'rt_main_headless.o': 'driver (hash, audio drain)'}.get(obj, 'unresolved')


def profile_report(exe, outdir, prof_files, top=14):
    syms, (tlo, thi) = nm_text_symbols(exe)
    addrs = [s[0] for s in syms]
    where = object_functions(outdir)
    nm_main = next(a for a, n in syms if n == 'main')
    cats, funcs = collections.Counter(), collections.Counter()
    total = 0
    for pf in prof_files:
        lines = open(pf).read().splitlines()
        slide = int(lines[0].split()[1], 16) - nm_main
        for line in lines[1:]:
            w = line.split()
            addrs8 = [int(x, 16) - slide for x in w[:8]]
            libc_name = w[8] if len(w) > 8 else None
            pc, callers = addrs8[0], addrs8[1:]

            def sym(a):
                if not (tlo <= a < thi):
                    return None
                i = bisect.bisect_right(addrs, a) - 1
                return syms[i][1] if i >= 0 else None
            name = sym(pc)
            if name is not None:
                cat = classify(name, where)
            else:                   # inside libc or the kernel (memset, memcpy): the caller is where the time was spent
                caller = next((c for c in map(sym, callers) if c), None)
                name = f'{libc_name or "?"} (libc, from {caller})' if caller else f'{libc_name or "?"} (libc)'
                cat = classify(caller, where) if caller else 'libc / kernel, caller unknown'
            cats[cat] += 1
            funcs[name or '?'] += 1
            total += 1
    if not total:
        print('   no profile samples')
        return {}
    print(f'   profile: {total} samples (1 ms of CPU each) from {len(prof_files)} run(s)')
    for c in CAT_ORDER:
        if cats[c]:
            print(f'     {100.0 * cats[c] / total:5.1f}%  {c}')
    print('     hottest functions:')
    for n, v in funcs.most_common(top):
        print(f'       {100.0 * v / total:5.1f}%  {n}')
    return {c: round(100.0 * cats[c] / total, 2) for c in CAT_ORDER if cats[c]}


def sample_report(exe, args_cmd, outdir, delay):
    """macOS `sample` on a running driver, as a cross-check of --profile: the process starts, and `sample`
    attaches `delay` seconds later (so the late, heavy frames are what it sees) and samples at 1 ms until the
    process ends. Prints where the samples fell by function and by library."""
    path = os.path.join(outdir, 'sample.txt')
    p = subprocess.Popen(args_cmd, cwd=outdir, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(delay)
    subprocess.run(['sample', str(p.pid), '3', '1', '-file', path], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    p.wait()
    text = open(path).read() if os.path.exists(path) else ''
    part = text.split('Sort by top of stack, same collapsed')[-1].split('Binary Images:')[0]
    rows = [(int(m.group(3)), m.group(1).strip(), m.group(2)) for m in
            (re.match(r'\s+(\S.*?)\s+\(in (\S+)\)\s+(\d+)\s*$', l) for l in part.splitlines()) if m]
    total = sum(r[0] for r in rows)
    if not total:
        print('   sample(1): no samples (the run ended before sample attached: use a longer run or a smaller --sample delay)')
        return
    print(f'   sample(1): {total} samples after {delay} s; by library: ' +
          ', '.join(f'{m} {100.0 * sum(r[0] for r in rows if r[2] == m) / total:.1f}%' for m in sorted({r[2] for r in rows})))
    for n, name, mod in rows[:12]:
        print(f'       {100.0 * n / total:5.1f}%  {name} ({mod})')


# ---------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser(description='Benchmark a recompiled game: frames per second and where the time goes.',
                                 formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument('--homebrew', action='store_true', help="the test ROM built from tests/hw (100 frames unless --frames)")
    g.add_argument('--rom', help='a game, for local use (no game data is ever committed)')
    ap.add_argument('--vs', metavar='DIR', help='another build directory: its runs alternate with --dir\'s, and the two are compared '
                    '(speed-up per window; its hashes must equal --dir\'s)')
    ap.add_argument('--dir', help='build directory (default build/bench/homebrew or build/bench/<rom name>)')
    ap.add_argument('--build', action='store_true', help='recompile and run make first')
    ap.add_argument('--no-recompile', action='store_true', help='with --build: only run make (the generated C is unchanged)')
    ap.add_argument('--jobs', type=int, default=4, help='make -j (the machine is shared: keep it modest)')
    ap.add_argument('--cflags', default=DEFAULT_CFLAGS)
    ap.add_argument('--workload', action='store_true', help='with --homebrew: run the CPU benchmark kernels (START held)')
    ap.add_argument('--frames', type=int)
    ap.add_argument('--save', help='a save file to copy for every run (the original is not written)')
    ap.add_argument('--press', help="BUTTON:EVERY, e.g. a:100 presses A (the driver's letter) for 5 frames every 100 frames")
    ap.add_argument('--keys', help="the driver's --keys script, instead of --press")
    ap.add_argument('--runs', type=int, default=5)
    ap.add_argument('--window', type=int, default=100, help='frames per row of the table')
    ap.add_argument('--profile', metavar='A:B', help='sample where the time goes in frames A to B-1')
    ap.add_argument('--sample', nargs='?', const=0.5, type=float, metavar='DELAY', help="also run macOS's sample on a run, attached DELAY seconds (default 0.5) after it starts")
    ap.add_argument('--hash', default='300,600,720,800,900', help='frames to hash (default: %(default)s) or every:K')
    ap.add_argument('--no-hash', action='store_true')
    ap.add_argument('--expect', help='compare the hashes with this file')
    ap.add_argument('--write-expect', help='write the hashes to this file')
    ap.add_argument('--json', help='write the results here')
    ap.add_argument('extra', nargs='*', help='more options for dsn_headless, after --')
    args = ap.parse_args()

    if args.homebrew:
        name, rom_path = 'homebrew test ROM', build_homebrew(None)
        frames = args.frames or (800 if args.workload else 100)
        out = args.dir or os.path.join(ROOT, 'build', 'bench', 'homebrew')
        if args.keys is not None:
            keys = args.keys
        else:
            keys = '5:s' if args.workload else '35:E,45:ED,55:'
        save = None
        hashes = '100,300,500,800' if args.workload else '25,50,75,100'
    else:
        rom_path = os.path.abspath(args.rom)
        name = os.path.splitext(os.path.basename(rom_path))[0]
        frames = args.frames or 900
        out = args.dir or os.path.join(ROOT, 'build', 'bench', re.sub(r'\W+', '_', name).lower())
        keys = args.keys if args.keys is not None else keys_script(args.press, frames)
        save = os.path.abspath(args.save) if args.save else None
        hashes = args.hash
    out = os.path.abspath(out)
    info = None
    if args.build or not os.path.exists(os.path.join(out, 'dsn_headless')):
        info = build_game(rom_path, out, args.jobs, args.cflags, recompile=not args.no_recompile)
        print('built: ' + ', '.join(f'{k} {v}' for k, v in info.items()), file=sys.stderr)
    exe = os.path.join(out, 'dsn_headless')
    a9 = os.path.join(out, 'arm9.bin')
    work = os.path.join(out, 'bench')
    os.makedirs(work, exist_ok=True)
    extra = args.extra
    if args.homebrew and not args.workload:        # the test suite's own options (rtc, save, wav, frames)
        for path in sorted(os.listdir(os.path.join(ROOT, 'tests', 'hw'))):
            if path.startswith('expect_') and path.endswith('.txt'):
                for line in open(os.path.join(ROOT, 'tests', 'hw', path)):
                    if line.startswith('args:'):
                        extra += line[5:].split()

    runs, runs_b = [], []
    exe_b = os.path.join(os.path.abspath(args.vs), 'dsn_headless') if args.vs else None
    for i in range(args.runs):
        for lst, ex, tag in ((runs, exe, 'run'), (runs_b, exe_b, 'runb')):
            if ex is None:
                continue
            r = run_once(ex, rom_path, a9, frames, keys, save, extra, work,
                         hashes=None if args.no_hash else hashes, tag=f'{tag}{i}')
            if r['status'] not in (0, 1) or not r['frames']:
                sys.exit(f'run {i} of {ex} failed (status {r["status"]}); see {r["out"]}')
            lst.append(r)
    # the emulation must be reproducible, or the hashes mean nothing
    if not args.no_hash:
        for r in runs[1:]:
            if r['hashes'] != runs[0]['hashes']:
                print('WARNING: the runs gave different hashes: the emulation is not deterministic', file=sys.stderr)
                break
    n = min(len(r['frames']) for r in runs)
    wins = [(a, min(a + args.window, n)) for a in range(0, n, args.window)]
    print_report(name, runs, wins, info)
    if runs_b:
        pa, pb = best_frames(runs), best_frames(runs_b)
        print(f'== {args.vs} (B) against {out} (A), runs interleaved; ms per frame, best frames of {args.runs} runs')
        print(f'   {"frames":>11} {"A ms":>8} {"B ms":>8} {"B faster":>9}   {"A p99":>7} {"B p99":>7}')
        for a, b in wins + [(0, n)]:
            sa, sb = window_stats(pa, a, b), window_stats(pb, a, b)
            ta, tb = sum(x[0] for x in pa[a:b]) / (b - a) * 1000, sum(x[0] for x in pb[a:b]) / (b - a) * 1000
            label = 'all' if (a, b) == (0, n) else f'{a}-{b}'
            print(f'   {label:>11} {ta:>8.3f} {tb:>8.3f} {ta / tb:>8.2f}x   {sa["p99_ms"]:>7.2f} {sb["p99_ms"]:>7.2f}')
        if not args.no_hash:
            same = runs[0]['hashes'] == runs_b[0]['hashes']
            print('   hashes: ' + ('B is bit-identical to A at ' + ','.join(str(f) for f in sorted(runs[0]['hashes'])) if same
                                   else 'B DIFFERS from A: ' + ', '.join(str(f) for f in sorted(runs[0]['hashes'])
                                                                      if runs[0]['hashes'][f] != runs_b[0]['hashes'].get(f))))
            if not same:
                return 1

    result = {'name': name, 'frames': n, 'runs': args.runs, 'build': info}
    pf = best_frames(runs)
    if args.workload:
        ok, result['kernels'] = workload_report(runs, pf)
        if not ok:
            result['hash_ok'] = False
    result['windows'] = {f'{a}-{b}': window_stats(pf, a, b) for a, b in wins}
    if args.profile:
        a, b = map(int, args.profile.split(':'))
        files = []
        for i in range(args.runs):
            r = run_once(exe, rom_path, a9, frames, keys, save, extra, work, prof=(a, b), tag=f'prof{i}')
            files.append(r['prof_file'])
        print(f'   frames {a}-{b}:')
        result['profile'] = profile_report(exe, out, files)
    if args.sample:
        cmd = [exe, rom_path, a9, '--frames', str(frames), '--rtc', RTC] + (['--keys', keys] if keys else [])
        if save:
            sav = os.path.join(work, 'sample.sav')
            shutil.copyfile(save, sav)
            cmd += ['--save', sav]
        sample_report(exe, cmd + extra, work, args.sample)
    if not args.no_hash and runs[0]['hashes']:
        hl = [runs[0]['hashes'][f] for f in sorted(runs[0]['hashes'])]
        if args.write_expect:
            open(args.write_expect, 'w').write('\n'.join(hl) + '\n')
            print(f'   wrote {len(hl)} hash lines to {args.write_expect}')
        if args.expect:
            want = {int(l.split()[1]): l for l in open(args.expect).read().splitlines() if l.startswith('hash ')}
            bad = [f for f in sorted(want) if runs[0]['hashes'].get(f) != want[f]]
            if bad:
                print(f'   HASH MISMATCH at frames {bad}')
                for f in bad[:3]:
                    print(f'     want {want[f]}\n     got  {runs[0]["hashes"].get(f)}')
                result['hash_ok'] = False
            else:
                print(f'   hashes match {args.expect} ({len(want)} frames: screens, memory, registers, audio)')
                result['hash_ok'] = True
    if args.json:
        json.dump(result, open(args.json, 'w'), indent=1)
    return 0 if result.get('hash_ok', True) else 1


if __name__ == '__main__':
    sys.exit(main())
