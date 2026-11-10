"""Seeded random button script for --keys, to play-test a game headless.

    python3 tools/random_keys.py --seed N [--frames 8000] [--start 900] [--gap 60] [--hold 30]

Prints a --keys string (F:KEYS,... with a release at F:) that is the same for the same seed.
Keys are Black 2's letters: a b x y l r s e and directions U D W E. Before --start the script
only taps A every --tap frames so the title and menus are reached; afterwards it presses one or
two random buttons, each held for a random number of frames up to --hold, with gaps up to --gap.
Nothing about the game is assumed, so the output is usable with any DS game.
"""
import argparse
import random


def script(seed, frames, start, tap, gap, hold):
    rng = random.Random(seed)
    events = []
    f = tap
    while f < min(start, frames):
        events.append((f, 'a'))
        events.append((f + 3, ''))
        f += tap
    f = max(start, tap)
    buttons = list('abxylrseUDWE')
    while f < frames:
        f += rng.randint(1, gap)
        if f >= frames:
            break
        n = 1 if rng.random() < 0.8 else 2
        keys = ''.join(sorted(set(rng.choice(buttons) for _ in range(n))))
        events.append((f, keys))
        f += rng.randint(1, hold)
        events.append((min(f, frames - 1), ''))
    # Later events override earlier ones at the same frame, so keep the order stable.
    events.sort(key=lambda e: e[0])
    out = []
    for frame, keys in events:
        out.append(f'{frame}:{keys}')
    return ','.join(out)


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--seed', type=int, required=True)
    p.add_argument('--frames', type=int, default=8000)
    p.add_argument('--start', type=int, default=900, help='frame where random presses start')
    p.add_argument('--tap', type=int, default=100, help='A tap interval before --start')
    p.add_argument('--gap', type=int, default=60, help='largest idle gap between presses')
    p.add_argument('--hold', type=int, default=30, help='longest hold of one press')
    a = p.parse_args()
    print(script(a.seed, a.frames, a.start, a.tap, a.gap, a.hold))


if __name__ == '__main__':
    main()
