# Jump Timing

Jump Timing shows, while Falcon falls with his double jump left, when to jump and which aerial to press to land. On the ground it also shows what a hop leads to, standing still or on the move.

Jump Timing also tried to work from the ground once and gave up. It's in the Confessions. It's back now, and this time it's cheating: every hop is worked out once per platform height, then just slid sideways to wherever you are.

<!-- GIF: Fountain of Dreams, Falcon falls past a platform and the Jump Timing cue shows the double jump and the nair for an AI, with Show turned on -->

## Options

The Jump Timing menu, in menu order.

| Option | Values | Default | What it does |
| --- | --- | --- | --- |
| Show | Off, On | Off | While Falcon falls with his double jump left, shows when to jump, with which stick, and which aerial to press when, to land as an aerial interrupt or a NIL on a platform or the floor. |
| Kind | NIL, AI, Both | AI | NIL: land with no landing lag. AI: land with an aerial interrupt. Both: whichever lets Falcon act sooner. |
| Land On | Any, Top Platform, Left Platform, Right Platform, Any Platform, Main Floor | Any | Where the jump should land. Any shows the one that lets Falcon act soonest, wherever it is. Top is the highest platform. Left and right are either side of the stage's middle. |
| Warning | 12 Frames, 16 Frames, 20 Frames, 24 Frames, 30 Frames | 20 Frames | Only show a jump at least this far ahead, so there's time to see it coming. Jumps due sooner are left out. |
| Path | Off, On | Off | Draws the shown jump's path in its color, with a yellow bar where Falcon is on the jump's frame and a pink one on the aerial's. |
| Plan Text | Off, On | On | Spells the shown jump out in the info panel, as in "DJ up in 6f, nair f7-9: AI". |

## How to read it

Jump Timing starts when Falcon is falling with his double jump still unused. It shows three things: the frame to double jump, which stick direction to hold, and which aerial to press and when.

It also works from a platform drop.

The path for the jump is drawn when Jump Timing > Path is on (it starts off). Input markers show where each press is due whether the path is drawn or not. See [Paths](paths.md).

The jump's lane in the timer ends like the cue lanes do: double jump on its frame and it bursts and flashes, early or late and it folds away in the miss color, with "early 2" or "late" beside it.

On that path, a yellow gate crosses it where Falcon will be on the jump's frame, and a pink one where he presses the aerial. Press as his feet cross each gate.

## Tips

- Set Land On to one target when you are drilling a specific platform. Any picks the quickest landing wherever it is.
- Kind on Both shows whichever of NIL and AI lets Falcon act sooner.
- On the ground, standing still shows what a hop now leads to, parked in the JUMP lane until you take off.
- Walking, dashing or running, it assumes you keep going as you are. A HOP lane counts down to the frame to short hop (SH) or full hop (FH), and the JUMP lane under it counts to the double jump.
- Running or sliding toward a platform's edge also shows the double jumps after you go off it, so the note is already moving in the lane before you leave the floor.
- The first time you stand on a new floor height, the hops take a few seconds to work out. After that they come up almost at once.
- Jump Timing is also a row in the quick menu, so you can switch it on and off between attempts. See [Presets](presets.md).

[Back to the README](../../README.md)
