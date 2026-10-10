# Troubleshooting

If the patch fails, a path looks wrong or a cue disagrees with the game, check these first, then report it.

When a cue says you'll land an AI and you don't, one of you is wrong. This page is how to find out which.

<!-- GIF: a gray path filling in as Falcon repeats the same jump, at default settings -->

## The patch failed

The ISO you used is not a clean Melee NTSC 1.02. A clean NTSC 1.02 ISO has this MD5:

```
0e63d4223b01d9aba596259dc155a174
```

Check your ISO's MD5. If it does not match, the patch will fail. Other regions, other versions and modified ISOs do not match.

## Paths are gray

Gray paths rely on ECB frames the event has not seen yet. Falcon's jumps, falls, aerials and airdodge are built in, and the event learns the rest as you play. The path improves as you repeat the move.

If a learned frame looks wrong, Developer > Forget Learned ECBs throws away what was learned and goes back to the built-in data.

## Settings do not stick

With no memory card in slot A, your settings last until you leave the event. The Memory Card row in the Presets menu says whether a card is in and whether anything is saved. See [Presets](presets.md#memory-card).

## No rumble

Rumble buzzes only a real controller, and only with rumble turned on in Dolphin's settings. While a rumble toggle is on, the game's own rumble is off for your controller. Each toggle needs its cue on. See [Cues](cues.md#rumble).

## The quick menu does not open

- HUD > Quick Menu must be On.
- L or R must be clicked all the way. A trigger only partway in does nothing, and the D-pad does nothing either.

## The game froze

D-pad down turns Frame Advance on. It is easy to hit by accident. Press D-pad down again, or step with the Advance button (Z by default).

## Tested in Dolphin only

Landing Lab has only been tested in Dolphin. It has not been tested on console. If you try it on a console, say what happened.

## Limits

- Captain Falcon only.
- Jump Timing does not search from the ground in this build.
- Paths drawn gray need ECB frames the event has not seen.

## A screenshot without the overlay

Two ways to hide everything Landing Lab draws:

- Click L and R both all the way and press D-pad up or down. Do it again to bring it back.
- Turn on Frame Advance, set Developer > Debug Log to anything but Off, and press D-pad up. Turning Frame Advance off clears it.

## Reporting a problem

Open an issue at https://github.com/Crackerfracks/TrainingMode-CommunityEdition/issues and include:

1. The stage.
2. The frame you pressed on, counted in Frame Advance.
3. What the cue or path said, and what happened.

A cue that said you would land an AI, and you did not, is worth reporting. Frame Advance will show which of you was wrong.

## Developer menu

The Developer menu is for testing and for reporting a wrong prediction. None of its rows are stored in presets.

| Option | Values | Default | What it does |
| --- | --- | --- | --- |
| Show Accuracy | Off, On | Off | Adds a line to the panel counting how often the prediction matched the landing exactly. |
| Collision Display | Off, On | Off | Shows stage collision, Falcon's ECB, hitboxes and hurtboxes, and hides his model so it does not cover them. |
| Debug Log | Off, Landings, Frames, Everything | Off | Writes extra lines to Dolphin's log. Landings: timer presses and ledge routes. Frames: every airborne frame too. Everything: camera too, and it slows play. |
| Script | Off, plus the scripts in TM/llscript.txt if the disc has that file | Off | For testing. Plays inputs from TM/llscript.txt from a fixed spot and exact to the frame, with Debug Log on. D-pad left plays it again. A shot step freezes the game until D-pad down. |
| Forget Learned ECBs | Press A | n/a | Falcon's jumps, falls, aerials and airdodge are built in. This forgets what was learned while playing and goes back to the built-in data. |
| About | Info only | n/a | Says that gray paths rely on ECB frames the event has not seen, and that D-pad up during Frame Advance hides everything for screenshots. |

Script lists only Off unless the disc has TM/llscript.txt. When it does, the list holds the file's scripts (the first 48) and All if there is more than one.

[Back to the README](../../README.md)
