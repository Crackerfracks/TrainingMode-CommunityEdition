#!/usr/bin/env python3
"""Pack captured ECB tables into TM/llecb.bin, the file the event loads
for every fighter but Falcon (src/landinglab_sim.h, Ecb_Find/Ecb_Unpack).

    ecbpack.py <out.bin> <capture log> ...

Each log is a run of src/landinglab_capture.txt: its LLECB begin line
names the fighter (internal kind), its LLECB, LLLEN and LLLOOP lines the
table.
Falcon Dive's slots (13, 14) are Falcon's own, so another fighter's are
left out. Insert the result with LLECB=<out.bin> ./build.sh.
"""
import re
import struct
import sys

TS_COUNT = 15
TS_UPB, TS_UPBA = 13, 14
FALCON = 2
FIGHTERS = 32
DIR_BYTES = 32 + FIGHTERS * 16

ECB = re.compile(r'LLECB (\d+) (\d+) (\d+) ([0-9a-f]{8}) ([0-9a-f]{8}) ([0-9a-f]{8}) ([0-9a-f]{8}) ([0-9a-f]{8})\s*$')
LEN = re.compile(r'LLLEN (\d+) (-?\d+) (-?\d+)\s*$')
LOOP = re.compile(r'LLLOOP (\d+) (\d+) (\d+)\s*$')
BEGIN = re.compile(r'LLECB begin char (\d+)')


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
    return kind, rows, lens, loops


def pad32(b):
    return b + b'\0' * (-len(b) % 32)


def block(rows, lens, loops):
    out = struct.pack('>HH', len(rows), TS_COUNT)
    for ts in range(TS_COUNT):
        n, nxt = lens.get(ts, (0, -1))
        out += struct.pack('>hh', n, nxt)
    out += struct.pack('>3Bx', *loops)
    for (ts, frame), (flags, words) in sorted(rows.items()):
        out += struct.pack('>BBBx5I', ts, frame, flags, *words)
    return pad32(out)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    tables = {}
    for path in sys.argv[2:]:
        kind, rows, lens, loops = read_table(path)
        if kind in tables:
            print('ecbpack: %s replaces an earlier table for kind %d' % (path, kind))
        tables[kind] = (rows, lens, loops, path)
    if len(tables) > FIGHTERS:
        sys.exit('ecbpack: more than %d fighters' % FIGHTERS)
    blocks, directory, at = b'', b'', DIR_BYTES
    for kind in sorted(tables):
        rows, lens, loops, path = tables[kind]
        b = block(rows, lens, loops)
        directory += struct.pack('>4I', kind, at, len(b), 0)
        blocks += b
        at += len(b)
        filled = sum(1 for ts in range(TS_COUNT) if any(k[0] == ts for k in rows))
        print('kind %2d: %4d rows, %2d of %d states, falls loop %s, %s' % (kind, len(rows), filled, TS_COUNT, loops, path))
    header = pad32(b'LLE1' + struct.pack('>I', len(tables)))
    directory += b'\0' * (FIGHTERS * 16 - len(directory))
    with open(sys.argv[1], 'wb') as f:
        f.write(header + directory + blocks)
    print('%s: %d fighters, %d bytes' % (sys.argv[1], len(tables), DIR_BYTES + len(blocks)))


if __name__ == '__main__':
    main()
