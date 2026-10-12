#!/bin/sh
# Builds tools/llsim/llsim with the PC's compiler. Floats stay floats, and
# multiply-adds fuse as devkitPPC fuses them into fmadds: unfused, 14 of
# Falcon's 110 Battlefield checks drifted from the game's hashes; fused,
# they match. Needs a CPU with FMA (x86 since Haswell).
# sim.c only sees MexTK's headers, main.c only the C library's.
cd "$(dirname "$0")" || exit 1
CC=${CC:-gcc}
FLAGS="-O2 -std=gnu11 -mfma -ffp-contract=fast -fno-fast-math -fwrapv -fno-strict-aliasing -fno-builtin"
$CC $FLAGS -w -c sim.c -o sim.o &&
    $CC $FLAGS -Wall -c main.c -o main.o &&
    $CC -o llsim sim.o main.o -lm &&
    rm -f sim.o main.o
