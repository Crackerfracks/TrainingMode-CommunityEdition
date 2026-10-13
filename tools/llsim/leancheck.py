#!/usr/bin/env python3
"""Check a capture's lean grid against its free drifting falls.

    leancheck.py <capture log> ...

A capture (src/landinglab_capture.txt) ends with falls drifting free; its
LLLEANECB lines tagged 9999 are those frames as the game played them. For
each, the fall's ECB is worked out as the event does (Lean_Ecb: the plain
loop frame, or its mix toward the grid's blends) and compared with the
game's, next to what the plain pose alone would give, each at its
animation frame (its LLFALLANIM line's, or the frame in the loop without
one). Prints the worst bottom and top error per fall and how many frames
were off by more than 0.01 unit; -v also lists those frames (state, frame,
animation frame, sm, side, lean, real and grid bottom and top).
"""
import sys

from ecbpack import FREE, TS_FALL, TS_FALLAERIAL, anim_frame, bits_float, read_lean_frames, read_leans, read_table


def mix(plain, levels, lean):
    """Lean_Mix: plain is (bottom, top, side, front, back), levels the rows
    of one loop frame by blend: (lean, bottom, top, side, front, back)."""
    w0, lo = 0.0, plain
    hi = None
    for i, row in enumerate(levels):
        hi = row
        if lean <= row[0] or i == len(levels) - 1:
            break
        w0, lo = row[0], row[1:]
    t = (lean - w0) / (hi[0] - w0) if hi[0] > w0 else 1.0
    t = min(t, 1.0)
    return tuple(a + (b - a) * t for a, b in zip(lo, hi[1:]))


def check(path, verbose=False):
    kind, rows, lens, loops, _, _, _ = read_table(path)
    loops = [l or 8 for l in loops]
    grid = {}
    for fall, back, k, words in read_leans(path, loops):
        grid.setdefault((fall, back, k), []).append(tuple(bits_float(w) for w in words))
    print('%s (kind %d):' % (path, kind))
    for fall, ts in enumerate((TS_FALL, TS_FALLAERIAL)):
        loop = loops[fall]
        worst = {'grid': [0, 0, 0], 'plain': [0, 0, 0]}
        n = sides = 0
        for f in read_lean_frames(path):
            if f[0] != ts or f[1] != FREE or not f[5]:
                continue
            frame, game_side, side = f[2], f[3], f[4]
            lean = bits_float(f[6][0])
            real = [bits_float(w) for w in f[6][1:]]
            anim = anim_frame(f, loop)
            key = (ts, anim)
            if key not in rows:
                continue
            plain = tuple(bits_float(w) for w in rows[key][1])
            levels = grid.get((fall, side == 2, anim))
            guess = {'plain': plain, 'grid': plain if side == 0 or lean <= 0 or not levels else mix(plain, levels, lean)}
            n += 1
            sides += side != 0
            for name, g in guess.items():
                w = worst[name]
                w[0] = max(w[0], abs(g[0] - real[0]))
                w[1] = max(w[1], abs(g[1] - real[1]))
                off = abs(g[0] - real[0]) > 0.01 or abs(g[1] - real[1]) > 0.01
                w[2] += off
                if verbose and off and name == 'grid':
                    print('    off: %d %3d anim %2d sm %d side %d lean %.4f real %.4f %.4f grid %.4f %.4f' %
                          (ts, frame, anim, game_side, side, lean, real[0], real[1], g[0], g[1]))
        if not n:
            print('  state %d: no free frames' % ts)
            continue
        print('  state %d: %d free frames (%d leaning), grid %d levels' % (ts, n, sides, len(grid.get((fall, 0, 0), []))))
        for name in ('plain', 'grid'):
            w = worst[name]
            print('    %-5s worst bottom %.4f top %.4f, %d frames off > 0.01' % (name, w[0], w[1], w[2]))


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    verbose = '-v' in sys.argv
    for path in sys.argv[1:]:
        if path != '-v':
            check(path, verbose)


if __name__ == '__main__':
    main()
