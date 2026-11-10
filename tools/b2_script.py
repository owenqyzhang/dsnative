#!/usr/bin/env python3
"""Input script generator for play-testing Pokemon Black 2 headless.

Prints a --keys argument: A presses through the title and menus (every 100 frames, as the
game's intro needs), then a seeded random walk (holds of U D W E with short releases, and a B
press now and then to close menus). The same seed and --frames give the same string, so a
session is reproducible with a fixed --rtc. Contains no game data: the walk is blind, and the
caller reads the screens to see what happened.

  python3 tools/b2_script.py --seed 3 --walk-from 1400 --frames 3000
"""
import argparse
import random


def script(seed, walk_from, frames, boot_a=True, walk_to=None, a_every=0, a_from=0):
    rng = random.Random(seed)
    events = []  # (frame, keys) in order; keys "" releases everything
    if boot_a:
        for f in range(100, min(walk_from, 1301), 100):
            events += [(f, "a"), (f + 5, "")]
    f = walk_from
    stop = walk_to if walk_to is not None else frames
    while f < stop:
        if rng.random() < 0.05:  # B closes menus and dialogue
            events += [(f, "b"), (f + 4, "")]
            f += 10
            continue
        direction = rng.choice("UDWE")
        hold = rng.randint(8, 30)
        events += [(f, direction), (f + hold, "")]
        f += hold + rng.randint(2, 6)
    # After the walk, A every a_every frames from a_from (advances battle text and menus).
    if a_every:
        for t in range(max(a_from, stop), frames, a_every):
            events += [(t, "a"), (t + 3, "")]
    return ",".join(f"{t}:{k}" for t, k in sorted(events) if t < frames)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--walk-from", type=int, default=1400)
    p.add_argument("--frames", type=int, default=3000)
    p.add_argument("--no-boot-a", action="store_true")
    p.add_argument("--walk-to", type=int, help="stop the random walk at this frame")
    p.add_argument("--a-every", type=int, default=0, help="press A every N frames after the walk")
    p.add_argument("--a-from", type=int, default=0, help="first frame of the A presses (never before the walk ends)")
    a = p.parse_args()
    print(script(a.seed, a.walk_from, a.frames, not a.no_boot_a, a.walk_to, a.a_every, a.a_from))


if __name__ == "__main__":
    main()
