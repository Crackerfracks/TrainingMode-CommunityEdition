#!/bin/sh
# Builds tools/llsim/llsim with the PC's compiler. Floats stay floats and
# no multiply-adds are fused, to round as close to the GameCube as a PC can.
# sim.c only sees MexTK's headers, main.c only the C library's.
cd "$(dirname "$0")" || exit 1
CC=${CC:-gcc}
FLAGS="-O2 -std=gnu11 -ffp-contract=off -fno-fast-math -fwrapv -fno-strict-aliasing -fno-builtin"
$CC $FLAGS -w -c sim.c -o sim.o &&
    $CC $FLAGS -Wall -c main.c -o main.o &&
    $CC -o llsim sim.o main.o -lm &&
    rm -f sim.o main.o
