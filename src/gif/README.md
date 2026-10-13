# GIF shot scripts

Test scripts for the README and guide GIFs, one file per stage (the event's
stage is fixed per build: LLSTAGE=GRKINDEXT_BATTLE, _OLDPU (Dream Land), _PSTAD (Pokemon Stadium), _FD, _IZUMI (Fountain of
Dreams)). Build with LLSCRIPT=<file>; the
fighter is Falcon, except landinglab_gif_cast.txt (LLCHAR=file, one build
run once per fighter, as tools/harness/run_tests.py does).

Each script brackets its clip with `sync <id>`, `mark clip <id> in` and
`mark clip <id> out`. `sync` turns the whole screen magenta for two drawn
frames (and logs LLSYNC), on the same game frame as the `in` mark, so the
cutter finds the clip's start in the frame dump by color: the clip starts
on the first drawn frame after the magenta, and runs (out - in) game frames
from there, times 2 at Game Speed 1/2 (index 3) and 4 at 1/4 (index 4),
where each game frame is drawn more than once. Scripts that end at their
`in` mark (assist-*, quick-menu) set up only: the pad (pad.py) plays the
rest, and the cut ends 40 frames after the touchdown.
A script that sets options starts from `preset load Defaults`, so every
shot is the defaults plus the lines it lists.

The shot list (what each GIF is for, where it goes, length, which fighter)
is docs/media/shots.md.
