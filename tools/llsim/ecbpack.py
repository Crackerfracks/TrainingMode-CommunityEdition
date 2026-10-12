#!/usr/bin/env python3
"""Pack captured ECB tables into TM/llecb.bin, the file the event loads
for every fighter but Falcon (src/landinglab_sim.h, Ecb_Find/Ecb_Unpack).

    ecbpack.py <out.bin> <capture log> ...

Each log is a run of src/landinglab_capture.txt: its LLECB begin line
names the fighter (internal kind), its LLECB, LLLEN and LLLOOP lines the
table, and its LLLEANECB lines the falls' leans: each fall held at a
steady lean (the capture's hover lean T), of which the last whole loop is
kept as one blend of the grid (LeanEcb in src/landinglab_sim.h).
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
TS_UPB, TS_UPBA = 13, 14
FALCON = 2
FIGHTERS = 32
DIR_BYTES = 32 + FIGHTERS * 16
LEAN_MAX = 2048  # LL_LEAN_MAX

ECB = re.compile(r'LLECB (\d+) (\d+) (\d+) ([0-9a-f]{8}) ([0-9a-f]{8}) ([0-9a-f]{8}) ([0-9a-f]{8}) ([0-9a-f]{8})\s*$')
LEN = re.compile(r'LLLEN (\d+) (-?\d+) (-?\d+)\s*$')
LOOP = re.compile(r'LLLOOP (\d+) (\d+) (\d+)\s*$')
BEGIN = re.compile(r'LLECB begin char (\d+)')
LEAN = re.compile(r'LLLEANECB (\d+) (-?\d+) (\d+) (-?\d+) (\d+) (\d+) ([0-9a-f]{8}) ([0-9a-f]{8}) ([0-9a-f]{8}) ([0-9a-f]{8}) ([0-9a-f]{8}) ([0-9a-f]{8})\s*$')


def bits_float(u):
    return struct.unpack('>f', struct.pack('>I', u))[0]


def read_lean_frames(path):
    """Every LLLEANECB line: (ts, tag, frame, game side, sim side, has
    bottom, words [lean bottom top side front back])."""
    out = []
    for line in open(path, errors='replace'):
        m = LEAN.search(line)
        if m:
            g = m.groups()
            out.append(tuple(int(x) for x in g[:6]) + ([int(x, 16) for x in g[6:]],))
    return out


def read_leans(path, loops):
    """The grid: rows (fall, back, k, words) in LeanEcb order, from the last
    whole loop of each held lean. Empty if the log has none."""
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
                if not whole:
                    print('ecbpack: %s: lean %d of state %d has no whole loop of %d, left out' % (path, tag, ts, loop))
                    continue
                level_rows.append(sorted(((fall, back, f[2] % loop, f[6]) for f in tail), key=lambda r: r[2]))
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
    return kind, rows, lens, loops, leans


def pad32(b):
    return b + b'\0' * (-len(b) % 32)


def block(rows, lens, loops, leans):
    out = struct.pack('>HH', len(rows), TS_COUNT)
    for ts in range(TS_COUNT):
        n, nxt = lens.get(ts, (0, -1))
        out += struct.pack('>hh', n, nxt)
    out += struct.pack('>3Bx', *loops)
    for (ts, frame), (flags, words) in sorted(rows.items()):
        out += struct.pack('>BBBx5I', ts, frame, flags, *words)
    if leans:
        out += struct.pack('>Hxx', len(leans))
        for fall, back, k, words in leans:
            out += struct.pack('>BBBx6I', fall, back, k, *words)
    return pad32(out)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    tables = {}
    for path in sys.argv[2:]:
        kind, rows, lens, loops, leans = read_table(path)
        if kind in tables:
            print('ecbpack: %s replaces an earlier table for kind %d' % (path, kind))
        tables[kind] = (rows, lens, loops, leans, path)
    if len(tables) > FIGHTERS:
        sys.exit('ecbpack: more than %d fighters' % FIGHTERS)
    blocks, directory, at = b'', b'', DIR_BYTES
    for kind in sorted(tables):
        rows, lens, loops, leans, path = tables[kind]
        if len(leans) > LEAN_MAX:
            sys.exit('ecbpack: %s has %d lean rows, more than the event keeps (%d)' % (path, len(leans), LEAN_MAX))
        b = block(rows, lens, loops, leans)
        directory += struct.pack('>4I', kind, at, len(b), 0)
        blocks += b
        at += len(b)
        filled = sum(1 for ts in range(TS_COUNT) if any(k[0] == ts for k in rows))
        print('kind %2d: %4d rows, %2d of %d states, falls loop %s, %4d lean rows, %s' % (kind, len(rows), filled, TS_COUNT, loops, len(leans), path))
    header = pad32(b'LLE1' + struct.pack('>I', len(tables)))
    directory += b'\0' * (FIGHTERS * 16 - len(directory))
    with open(sys.argv[1], 'wb') as f:
        f.write(header + directory + blocks)
    print('%s: %d fighters, %d bytes' % (sys.argv[1], len(tables), DIR_BYTES + len(blocks)))


if __name__ == '__main__':
    main()
