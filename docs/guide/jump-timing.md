# Jump Timing

Jump Timing shows, while Falcon falls with his double jump left, when to jump and which aerial to press to land.

Jump Timing also tried to work from the ground once. It's in the Confessions.

<!-- GIF: Fountain of Dreams, Falcon falls past a platform and the Jump Timing cue shows the double jump and the nair for an AI, with Show turned on -->

## Options

The Jump Timing menu, in menu order.

| Option | Values | Default | What it does |
| --- | --- | --- | --- |
| Show | Off, On | Off | While Falcon falls with his double jump left, shows when to jump, with which stick, and which aerial to press when, to land as an aerial interrupt or a NIL on a platform or the floor. |
| Kind | NIL, AI, Both | AI | NIL: land with no landing lag. AI: land with an aerial interrupt. Both: whichever lets Falcon act sooner. |
| Land On | Any, Top Platform, Left Platform, Right Platform, Any Platform, Main Floor | Any | Where the jump should land. Any shows the one that lets Falcon act soonest, wherever it is. Top is the highest platform. Left and right are either side of the stage's middle. |
| Warning | 12 Frames, 16 Frames, 20 Frames, 24 Frames, 30 Frames | 20 Frames | Only show a jump at least this far ahead, so there's time to see it coming. Jumps due sooner are left out. |

## How to read it

Jump Timing starts when Falcon is falling with his double jump still unused. It shows three things: the frame to double jump, which stick direction to hold, and which aerial to press and when.

It also works from a platform drop.

The path for the jump is drawn when Paths > Ledge Route is on, and input markers show on it like on any path. See [Paths](paths.md).

On that path, a yellow gate crosses it where Falcon will be on the jump's frame, and a pink one where he presses the aerial. Press as his feet cross each gate.

## Tips

- Set Land On to one target when you are drilling a specific platform. Any picks the quickest landing wherever it is.
- Kind on Both shows whichever of NIL and AI lets Falcon act sooner.
- Jump Timing does not search for jumps from the ground in this build. To see a hop from the ground, use Jump Preview in [Paths](paths.md).
- Jump Timing is also a row in the quick menu, so you can switch it on and off between attempts. See [Presets](presets.md).

[Back to the README](../../README.md)
