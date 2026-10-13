#!/usr/bin/env python3
"""Pack captured ECB tables into TM/llecb.bin, the file the event loads
for every fighter but Falcon (src/landinglab_sim.h, Ecb_Find/Ecb_Unpack).

    ecbpack.py <out.bin> <capture log> ...

Each log is a run of src/landinglab_capture.txt: its LLECB begin line
names the fighter (internal kind), its LLECB, LLLEN and LLLOOP lines the
table, and its LLLEANECB lines the falls' leans: each fall held at a
steady lean (the capture's hover lean T), of which the last whole loop is
kept as one blend of the grid (LeanEcb in src/landinglab_sim.h), each
frame at its animation frame (the LLFALLANIM line after it, if any). Those
lines also give the falls' lean step (fall_step: Peach's and DK's
animations move 2 frames after a frame that leant) and blend-in
(fall_blend: their falls ease in from the last pose). For the
double-jump-cancel fighters, the LLDJMV lines give their double jump's
movement frame by frame (DjMove), from the first jump of each kind.
Falcon Dive's slots (13, 14) are Falcon's own, so another fighter's are
left out, as are his leans (baked in the event). Insert the result with
LLECB=<out.bin> ./build.sh.
"""
import re
import struct
import sys

TS_COUNT = 15
TS_FALL, TS_FALLAERIAL = 4, 5
FREE = 9999  # LLLEANECB's tag for a fall that isn't held
TS_JUMPAERIALF, TS_JUMPAERIALB = 2, 3
DJC = {8: 'Ness', 9: 'Peach', 14: 'Yoshi', 16: 'Mewtwo'}  # internal kinds (Djc_Kind)
NESS = 8
TS_UPB, TS_UPBA = 13, 14
FALCON = 2
FIGHTERS = 32
DIR_BYTES = 32 + FIGHTERS * 16
LEAN_MAX = 2048  # LL_LEAN_MAX

ECB = re.compile(r'LLECB (\d+) (\d+) (\d+) ([0-9a-f]{8}) ([0-9a-f]{8}) ([0-9a-f]{8}) ([0-9a-f]{8}) ([0-9a-f]{8})\s*$')
LEN = re.compile(r'LLLEN (\d+) (-?\d+) (-?\d+)\s*$')
LOOP = re.compile(r'LLLOOP (\d+) (\d+) (\d+)\s*$')
BEGIN = re.compile(r'LLECB begin char (\d+)')
FALLANIM = re.compile(r'LLFALLANIM (\d+) (-?\d+) (\d+) ([0-9a-f]{8}) ([0-9a-f]{8}) ([0-9a-f]{8})\s*$')
LEAN = re.compile(r'LLLEANECB (\d+) (-?\d+) (\d+) (-?\d+) (\d+) (\d+) ([0-9a-f]{8}) ([0-9a-f]{8}) ([0-9a-f]{8}) ([0-9a-f]{8}) ([0-9a-f]{8}) ([0-9a-f]{8})\s*$')


def bits_float(u):
    return struct.unpack('>f', struct.pack('>I', u))[0]


def read_lean_frames(path):
    """Every LLLEANECB line: (ts, tag, frame, game side, sim side, has
    bottom, words [lean bottom top side front back], animation frame). The
    animation frame is its LLFALLANIM line's, None in a log without them."""
    out = []
    for line in open(path, errors='replace'):
        m = LEAN.search(line)
        if m:
            g = m.groups()
            out.append(tuple(int(x) for x in g[:6]) + ([int(x, 16) for x in g[6:]], None))
            continue
        m = FALLANIM.search(line)
        if m and out and out[-1][7] is None and out[-1][0] == int(m.group(1)) and out[-1][2] == int(m.group(2)):
            out[-1] = out[-1][:7] + (int(bits_float(int(m.group(4), 16)) + 0.5),)
    return out


def anim_frame(f, loop):
    """A lean frame's animation frame in its fall's loop."""
    return (f[7] if f[7] is not None else f[2]) % loop


def read_step(path, loops):
    """The falls' lean step (fall_step), fit to the LLFALLANIM frames: of
    each fall's frames in a row, how many moved the animation a frame (step
    1), and how many moved it 2 after a frame that leant and 1 otherwise
    (step 2). The one that explains more wins; 1 if the log has none."""
    pairs, fits, prev = 0, [0, 0], None
    for f in read_lean_frames(path):
        if f[7] is None or f[0] not in (TS_FALL, TS_FALLAERIAL):
            prev = None
            continue
        if prev and prev[0] == f[0] and f[2] == prev[2] + 1:
            loop = loops[f[0] == TS_FALLAERIAL]
            d = (f[7] - prev[7]) % loop
            pairs += 1
            fits[0] += d == 1 % loop
            fits[1] += d == (2 if bits_float(prev[6][0]) > 0 else 1) % loop
        prev = f
    step = 2 if fits[1] > fits[0] else 1
    if pairs:
        print('ecbpack: %s: falls step %d: of %d frame pairs, step 1 explains %d, step 2 %d' % (path, step, pairs, fits[0], fits[1]))
    return step


def read_blend(path):
    """The falls' blend-in (fall_blend): how many frames a fall's pose takes
    to ease in from the last one (state.blend, LLFALLANIM's last word),
    the value seen most often; 0 if the log shows none."""
    seen = {}
    for line in open(path, errors='replace'):
        m = FALLANIM.search(line)
        if m and int(m.group(1)) in (TS_FALL, TS_FALLAERIAL):
            b = int(bits_float(int(m.group(6), 16)) + 0.5)
            if 0 < b < 64:
                seen[b] = seen.get(b, 0) + 1
    blend = max(seen, key=seen.get) if seen else 0
    if blend:
        print('ecbpack: %s: falls blend in over %d frames (%s)' % (path, blend, seen))
    return blend


DJMV = re.compile(r'LLDJMV (\d+) (\d+) (-?\d+) ([0-9a-f]{8}) ([0-9a-f]{8}) ([0-9a-f]{8})\s*$')


def read_dj(path, kind):
    """The double jump's movement, forward and backward: lists of (x, y)
    words per frame from frame 0, each from the first jump of its kind
    seen. x is the animation's part as if facing right (Ness's speed less
    his own drift speed), y the vertical speed."""
    jumps, last = {}, None
    for line in open(path, errors='replace'):
        m = DJMV.search(line)
        if not m:
            continue
        ts, frame, face = int(m.group(1)), int(m.group(2)), int(m.group(3))
        vx, vy, hvel = (bits_float(int(g, 16)) for g in m.groups()[3:])
        if frame == 0:
            if ts in jumps:
                last = None  # a later jump of the same kind: the first one stands
                continue
            jumps[ts] = []
            last = ts
        if last != ts or len(jumps[ts]) != frame:
            continue
        x = (vx - hvel if kind == NESS else vx) * face
        jumps[ts].append((float_bits(x), int(m.group(5), 16)))
    return [jumps.get(TS_JUMPAERIALF, []), jumps.get(TS_JUMPAERIALB, [])]


def float_bits(f):
    return struct.unpack('>I', struct.pack('>f', f))[0]


def read_leans(path, loops):
    """The grid: rows (fall, back, k, words) in LeanEcb order, from the last
    whole loop of each held lean, k its animation frame. Empty if the log
    has none."""
    segs, last = {}, None
    for f in read_lean_frames(path):
        ts, tag = f[0], f[1]
        if tag in (0, FREE) or ts not in (TS_FALL, TS_FALLAERIAL):
            last = None
            continue
        key = (ts, tag)
        if key != last:
            segs[key] = []  # the last run of a lean wins
            last = key
        segs[key].append(f)
    rows = []
    for fall, ts in enumerate((TS_FALL, TS_FALLAERIAL)):
        loop = loops[fall]
        for back in (0, 1):
            tags = sorted((t for (s, t) in segs if s == ts and (t < 0) == bool(back)), key=abs)
            level_rows = []
            for tag in tags:
                frames = segs[(ts, tag)]
                tail = frames[-loop:] if loop else []
                whole = (len(tail) == loop and all(b[2] == a[2] + 1 for a, b in zip(tail, tail[1:]))
                         and all(f[5] for f in tail))
                by_k = {anim_frame(f, loop): f[6] for f in tail} if whole else {}
                # a fall whose animation moves 2 frames a frame while it leans
                # (fall_step) shows only every other animation frame: each
                # missing one is the mean of the two either side
                for k in range(loop):
                    a, b = by_k.get((k - 1) % loop), by_k.get((k + 1) % loop)
                    if k not in by_k and a and b:
                        by_k[k] = [float_bits((bits_float(x) + bits_float(y)) / 2) for x, y in zip(a, b)]
                if len(by_k) < loop:
                    print('ecbpack: %s: lean %d of state %d has no whole loop of %d, left out' % (path, tag, ts, loop))
                    continue
                level_rows.append([(fall, back, k, by_k[k]) for k in range(loop)])
            for lv in level_rows:
                rows += lv
    return rows


def read_table(path):
    kind, rows, lens, loops = None, {}, {}, (0, 0, 0)
    for line in open(path, errors='replace'):
        m = BEGIN.search(line)
        if m:
            # the last table in the log wins
            kind, rows, lens, loops = int(m.group(1)), {}, {}, (0, 0, 0)
            continue
        if kind is None:
            continue
        m = ECB.search(line)
        if m:
            ts, frame, flags = (int(g) for g in m.groups()[:3])
            rows[(ts, frame)] = (flags, [int(g, 16) for g in m.groups()[3:]])
            continue
        m = LEN.search(line)
        if m:
            ts, n, nxt = (int(g) for g in m.groups())
            lens[ts] = (n, nxt)
            continue
        m = LOOP.search(line)
        if m:
            loops = tuple(int(g) for g in m.groups())
    if kind is None or not rows:
        sys.exit('ecbpack: %s has no LLECB table' % path)
    if kind != FALCON:
        rows = {k: v for k, v in rows.items() if k[0] not in (TS_UPB, TS_UPBA)}
        for ts in (TS_UPB, TS_UPBA):
            lens[ts] = (0, -1)
    if kind != FALCON and loops == (0, 0, 0):
        print('ecbpack: %s has no LLLOOP line: its falls loop as Falcon\'s (8)' % path)
    leans = read_leans(path, [l or 8 for l in loops]) if kind != FALCON else []
    dj = read_dj(path, kind) if kind in DJC else [[], []]
    step = read_step(path, [l or 8 for l in loops])
    blend = read_blend(path)
    return kind, rows, lens, loops, leans, dj, step, blend


def pad32(b):
    return b + b'\0' * (-len(b) % 32)


def block(rows, lens, loops, leans, dj, step, blend):
    out = struct.pack('>HH', len(rows), TS_COUNT)
    for ts in range(TS_COUNT):
        n, nxt = lens.get(ts, (0, -1))
        out += struct.pack('>hh', n, nxt)
    # the old pad byte: the step in its low 2 bits (1 as 0), the blend above
    out += struct.pack('>4B', *loops, (step if step > 1 else 0) | blend << 2)
    for (ts, frame), (flags, words) in sorted(rows.items()):
        out += struct.pack('>BBBx5I', ts, frame, flags, *words)
    if leans:
        data = struct.pack('>Hxx', len(leans))
        for fall, back, k, words in leans:
            data += struct.pack('>BBBx6I', fall, back, k, *words)
        out += section(b'LEAN', data)
    if dj[0] or dj[1]:
        data = struct.pack('>HH', len(dj[0]), len(dj[1]))
        for x, y in dj[0] + dj[1]:
            data += struct.pack('>II', x, y)
        out += section(b'DJMV', data)
    return pad32(out)


def section(tag, data):
    """A tagged section after the rows (Ecb_Unpack): tag, size, data."""
    return tag + struct.pack('>I', len(data)) + data


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    tables = {}
    for path in sys.argv[2:]:
        kind, rows, lens, loops, leans, dj, step, blend = read_table(path)
        if kind in tables:
            print('ecbpack: %s replaces an earlier table for kind %d' % (path, kind))
        if kind in DJC and not dj[0]:
            print('ecbpack: %s has no LLDJMV double jump: %s\'s double jump falls back to the usual physics' % (path, DJC[kind]))
        tables[kind] = (rows, lens, loops, leans, dj, step, blend, path)
    if len(tables) > FIGHTERS:
        sys.exit('ecbpack: more than %d fighters' % FIGHTERS)
    blocks, directory, at = b'', b'', DIR_BYTES
    for kind in sorted(tables):
        rows, lens, loops, leans, dj, step, blend, path = tables[kind]
        if len(leans) > LEAN_MAX:
            sys.exit('ecbpack: %s has %d lean rows, more than the event keeps (%d)' % (path, len(leans), LEAN_MAX))
        b = block(rows, lens, loops, leans, dj, step, blend)
        directory += struct.pack('>4I', kind, at, len(b), 0)
        blocks += b
        at += len(b)
        filled = sum(1 for ts in range(TS_COUNT) if any(k[0] == ts for k in rows))
        dj_note = ', double jump %d/%d frames' % (len(dj[0]), len(dj[1])) if kind in DJC else ''
        print('kind %2d: %4d rows, %2d of %d states, falls loop %s step %d blend %d, %4d lean rows%s, %s' % (kind, len(rows), filled, TS_COUNT, loops, step, blend, len(leans), dj_note, path))
    header = pad32(b'LLE1' + struct.pack('>I', len(tables)))
    directory += b'\0' * (FIGHTERS * 16 - len(directory))
    with open(sys.argv[1], 'wb') as f:
        f.write(header + directory + blocks)
    print('%s: %d fighters, %d bytes' % (sys.argv[1], len(tables), DIR_BYTES + len(blocks)))


if __name__ == '__main__':
    main()
