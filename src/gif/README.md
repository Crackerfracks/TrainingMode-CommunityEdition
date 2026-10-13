# GIF shot scripts

Test scripts for the README and guide GIFs, one file per stage (the event's
stage is fixed per build: LLSTAGE=GRKINDEXT_BATTLE, _STORY (Yoshi's Story),
_OLDPU (Dream Land), _PSTAD (Pokemon Stadium), _FD, _IZUMI (Fountain of
Dreams)). Build with LLSCRIPT=<file>; the
fighter is Falcon unless the shot list says the cast (LLCHAR=file, one run
per fighter, as tools/harness/run_tests.py does).

Each script brackets its clip with `mark clip <id> in` and `mark clip <id>
out`: the log's LLMARK lines give the game frames to cut the frame dump at.
A script that sets options starts from `preset load Defaults`, so every
shot is the defaults plus the lines it lists.

The shot list (what each GIF is for, where it goes, length, which fighter)
is docs/media/shots.md.
