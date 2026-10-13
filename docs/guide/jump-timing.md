# Jump Timing

Jump Timing shows, while you fall with your [double jump](glossary.md#double-jump) left, when to jump and which aerial to press to land. On the ground it also shows what a hop leads to, standing still or on the move.

The first version searched live from the ground and was scrapped in 0.8.4. Its planned replacement, a guide painted on the floor, knew five ground speeds, and Melee has the rest, so it sits behind an `#ifdef` until after 1.0. This one cheats: every hop is worked out once per floor height, then slid sideways to wherever you happen to be.

<!-- GIF: ../media/jump-timing.gif (jump-timing, Fountain of Dreams): a fall past a platform with the Jump Timing cue showing the double jump and the nair for an AI, with Show turned on -->

## Options

The Jump Timing menu, in menu order.

| Option | Values | Default | What it does |
| --- | --- | --- | --- |
| Show | Off, On | Off | While you fall with your double jump left, shows when to jump, with which stick, and which aerial to press when, to land as an [aerial interrupt](glossary.md#aerial-interrupt-ai) or a [NIL](glossary.md#nil) on a platform or the floor. |
| Kind | NIL, AI, Both | AI | NIL: land with no landing lag. AI: land with an aerial interrupt. Both: whichever lets you act sooner. |
| Land On | Any, Top Platform, Left Platform, Right Platform, Any Platform, Main Floor | Any | Where the jump should land. Any shows the one that lets you act soonest, wherever it is. Top is the highest platform. Left and right are either side of the stage's middle. |
| Warning | 12 Frames, 16 Frames, 20 Frames, 24 Frames, 30 Frames | 20 Frames | Only show a jump at least this far ahead, so there's time to see it coming. Jumps due sooner are left out. |
| Path | Off, On | Off | Draws the shown jump's path in its color, with a yellow bar across it where you are on the jump's frame and a pink one on the aerial's. |
| Plan Text | Off, On | On | Spells the shown jump out in the info panel, as in "DJ up in 6f, nair +11-13: AI". The first press counts frames from now; each "+" counts from the press before it, so that nair comes 11 to 13 frames after the double jump. |

## How to read it

Jump Timing starts when you are falling with your double jump still unused (for Kirby and Jigglypuff, the first of their air jumps). It shows three things: the frame to double jump, which stick direction to hold, and which aerial to press and when.

It also works from a [platform drop](glossary.md#platform-drop-and-shield-drop), which it treats as a fall.

The path for the jump is drawn when Jump Timing > Path is on (it starts off). Input markers show where each press is due whether the path is drawn or not. See [Paths](paths.md).

In the Fixed Strip, the jump gets a **JUMP** [lane](timers.md#the-words) that counts down to the double jump. It ends like the cue lanes do: double jump on its frame and it bursts and flashes, early or late and it folds away in the miss color, with "early 2" or "late" beside it. While Show is on, the JUMP lane stays in the strip even when it is empty.

With Path on, a yellow bar crosses the path where you will be on the jump's frame, and a pink one where you press the aerial. Press as your feet cross each bar.

## From the ground

- Standing still, Jump Timing shows what a hop now leads to, parked in the JUMP lane until you take off. The info panel spells it out, as in "SH now, DJ up +30, nair +11".
- Walking, dashing or running, it assumes you keep going as you are. A **HOP** lane counts down to the frame to [short hop or full hop](glossary.md#short-hop-and-full-hop), with SH or FH beside the count, and the JUMP lane under it counts to the double jump.
- Dashing, running or sliding toward a platform's edge also shows the double jumps after you go off it, so the note is already moving in the lane before you leave the floor. Walking off an edge is not predicted: you teeter instead.
- Hop routes whose double jump comes on the way down are listed, but after any route that double jumps on the way up.
- The first time you stand on a new floor height, the hops take a few seconds to work out. After that they come up almost at once.

<!-- GIF: ../media/jump-timing-run.gif (jump-timing-run, Fountain of Dreams): running toward a platform's edge with the HOP and JUMP lanes counting down -->

## Tips

- Set Land On to one target when you are drilling a specific platform. Any picks the quickest landing wherever it is.
- Kind on Both shows whichever of NIL and AI lets you act sooner.
- Jump Timing is also a row in the quick menu, so you can switch it on and off between attempts. See [Presets](presets.md).

[Back to the guide](README.md) · [Back to the README](../../README.md)
