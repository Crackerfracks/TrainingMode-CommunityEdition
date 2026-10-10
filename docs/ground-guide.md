# Ground guide (shelved until after 1.0)

Stephen shelved this on 2026-10-10 after the 0.8.3 notes. The code stays in
`src/landinglab.c` behind `#ifdef LL_GROUND_GUIDE`, so normal builds leave it
out entirely, including its two Jump Timing toggles.

## What it is

It replaces the live ground jump search, which was scrapped in 0.8.4. Stephen
picked "A + C" from the mockup (https://claude.ai/artifact/Wddk5MKnsPiiKAU5gPrwob):

- **Takeoff Bands (A).** Strips under the floor show where a hop, optionally
  with a double jump, can start and still land an AI, NIL or waveland on the
  platform above. The nearer strip is the short hop and the farther one is the
  full hop.
- **Reach Marks (C).** Each platform lights the stretch a hop from where Falcon
  stands can land on. A spot is brighter when more timings work there.

Both follow each kind's cue toggle and Intensity/Auto Fade (`Kind_K(VG_CUES, kind)`).
He rejected B, a ghost arc: he finds it ugly, and he always keeps the old hop
arc preview off.

## How the tables work

For each platform height, each hop and each ground speed, every "way on" is
simulated once. That means the hop alone, or a double jump on airborne frames
1-24 with 9 stick/hold choices. Each run takes off from x 0 in a world with
only a floor and a platform at that height.

Where each run's NIL, rising AI window frames and waveland window frames touch
the platform is counted by distance from the press, for -80 to 80 units. A
table holds `u8 n[hop][speed][kind][distance]`, with kinds 0-4 for the AI with
each aerial (`Aerial_Order`), 5 for WL and 6 for NIL. The draw sums the
aerials AI Aerial lets through.

There are two ways to get the tables:

- **Live, without a baked file.** The heights come from the stage's platforms
  at the start, and the tables are worked out over the first second or two
  under a per-frame budget.
- **Baked.** The `bake` script command works out every height from 10 to 70 in
  0.5 steps (121 tables of 11,296 bytes, about 1.4 MB) and logs them as LLGB
  lines. Then `tools/guide_bake.py dolphin.log out/` writes `llgdNN.bin`, where
  NN is the character's internal kind (Falcon is 2). Add
  `insert TM/llgdNN.bin <path>` to build.sh's gc_fst call. At run time
  `Guide_Find` reads each height's table from the disc when a platform needs
  it, into an 8-slot cache. That covers Fountain's moving platforms, but each
  read is synchronous.

Neither path has been run on hardware. The live version was built in 3a84352
but not tested. The bake was compiled, never run.

## Why it's shelved, and what to fix first

The main problem is speed coverage. Only 5 ground speeds are simulated: run,
walk or still, toward or away. Any other speed snaps to the nearest one.
Stephen expects that to look spotty: "there are SO many more ground speeds than
just still, full walk/full run, forward and away".

With baking, more speeds cost only file size. One fix is to simulate many
speeds (for example every 0.1 of initial horizontal velocity) and interpolate
between the two nearest. Another is to bin by takeoff velocity instead of by
named speed.

Other gaps:

- sloped ground (Yoshi's, the Stadium ramps)
- takeoffs from moving platforms
- fastfall timing variants
- drift other than the held or let-go stick
