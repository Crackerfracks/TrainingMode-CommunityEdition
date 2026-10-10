#!/usr/bin/env python3
"""Turns a Dolphin log from the ground guide bake (the script command "bake",
with landinglab.c built with -DLL_GROUND_GUIDE) into TM/llgdNN.bin.

usage: guide_bake.py dolphin.log out_dir

The log has one LLGBH header line, then LLGB <height> <offset> <hex> lines
for each table's rows that aren't all 0, then "LLGB end". The file is a
32-byte header (magic "LLGD", version, character kind, number of heights,
lowest height, step, stride, dims; big-endian like the GameCube) and the
tables, each padded to the stride.
"""
import os
import re
import struct
import sys

VERSION = 1


def main():
    log, out = sys.argv[1], sys.argv[2]
    head = None
    tables = {}
    ended = False
    with open(log, errors="replace") as f:
        for line in f:
            m = re.search(r"LLGBH (\d+) (\d+) ([\d.]+) ([\d.]+) (\d+) (\d+)", line)
            if m:
                head = [int(m[1]), int(m[2]), float(m[3]), float(m[4]), int(m[5]), int(m[6])]
                tables = {}
                ended = False
                continue
            if re.search(r"LLGB end", line):
                ended = True
                continue
            m = re.search(r"LLGB (\d+) (\d+) ([0-9a-f]+)", line)
            if m and head:
                t = tables.setdefault(int(m[1]), bytearray(head[4]))
                data = bytes.fromhex(m[3])
                off = int(m[2])
                t[off:off + len(data)] = data
    if not head:
        sys.exit("no LLGBH line: was the bake run?")
    if not ended:
        sys.exit("no LLGB end line: the bake didn't finish")
    kind, num, lo, step, stride, dims = head
    path = os.path.join(out, "llgd%02d.bin" % kind)
    with open(path, "wb") as f:
        f.write(struct.pack(">4I2f2I", 0x4C4C4744, VERSION, kind, num, lo, step, stride, dims))
        for i in range(num):
            f.write(bytes(tables.get(i, bytearray(stride))))
    print("%s: %d heights from %.1f, %d with something in them" % (path, num, lo, len(tables)))


if __name__ == "__main__":
    main()
