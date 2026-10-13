# Troubleshooting

If the patch fails, a path looks wrong or a cue disagrees with the game, check these first, then report it.

When a cue says you'll land an AI and you don't, one of you is wrong. This page is how to find out which.

<!-- GIF: ../media/paths.gif (paths, Battlefield): a gray path filling in as the same jump is repeated, at default settings -->

## The patch failed

The ISO you used is not a clean Melee NTSC 1.02. A clean NTSC 1.02 ISO has this MD5:

```
0e63d4223b01d9aba596259dc155a174
```

Check your ISO's MD5. If it does not match, the patch will fail. Other regions, other versions and modified ISOs do not match.

## Paths are gray

A gray path relies on an [ECB](glossary.md#ecb) frame the event's data does not cover. Every fighter's jumps, falls, aerials and airdodge are built in; a move outside those (a special, say) is learned as you play, and the path improves as you repeat it.

If a learned frame looks wrong, Developer > Forget Learned ECBs throws away what was learned and goes back to the built-in data.

Falcon's data is built into the event itself. Every other fighter's is read from a file on the disc, TM/llecb.bin. A build made without that file says so when it is built, and in it every fighter but Falcon starts gray and learns in play. The release patch is meant to carry the file; if every fighter but Falcon is gray from the start, say so in an issue.

## Settings do not stick

With no memory card in slot A, your settings last until you leave the event. The Memory Card row in the Presets menu says whether a card is in and whether anything is saved. See [Presets](presets.md#memory-card).

## No rumble

Rumble buzzes only a real controller, and only with rumble turned on in Dolphin's settings. While a rumble toggle is on, the game's own rumble is off for your controller. Each toggle needs its cue on. See [Cues](cues.md#rumble).

## The quick menu does not open

- HUD > Quick Menu must be On.
- L or R must be clicked all the way. A trigger only partway in does nothing, and the D-pad does nothing either.

## The game froze

D-pad down turns [Frame Advance](glossary.md#frame-advance) on. It is easy to hit by accident. Press D-pad down again, or step with the Advance Button (Z by default).

## Tested in Dolphin only

Landing Gear has only been tested in Dolphin. It has not been tested on console. If you try it on a console, say what happened.

## Limits

- Every slot on the character select works. Zelda and Sheik are both in, and Sheik gets her own data when you transform. Ice Climbers means Popo; Nana is ignored.
- Kirby and Jigglypuff have five air jumps. Ledge routes and Jump Timing plan with the first one only.
- Kirby, Jigglypuff, Peach and Pichu have no ledge route that keeps GALINT. That is the game, not a bug: Peach's double jump keeps rising for about 50 frames, longer than the search looks. Route reads "none" for them.
- Peach's float is not simulated.
- The 15 GALINT Demo and Practice 15 GALINT are Captain Falcon on Battlefield only. Anyone else gets a toast saying so.
- The first time you stand on a new floor height, Jump Timing's hops take a few seconds to work out.
- Jump Timing does not predict walking off an edge, only dashing, running or sliding off one.
- A gray path needs an ECB frame the built-in data does not cover. See above.
- Tested in Dolphin, not on console.

## A screenshot without the overlay

Two ways to hide everything Landing Gear draws:

- Click L and R both all the way and press D-pad up or down. Do it again to bring it back.
- Turn on Frame Advance, set Developer > Debug Log to anything but Off, and press D-pad up. Turning Frame Advance off clears it.

## Reporting a problem

Open an issue at https://github.com/Crackerfracks/TrainingMode-CommunityEdition/issues and include:

1. The stage and the fighter.
2. The frame you pressed on, counted in Frame Advance.
3. What the cue or path said, and what happened.

A cue that said you would land an AI, and you did not, is worth reporting. Frame Advance will show which of you was wrong.

This is a fork. The TM-CE team did not write it and does not support it, so send reports here and not to the TM-CE Discord or repository.

## Developer menu

The Developer menu is for testing and for reporting a wrong prediction. None of its rows are stored in presets.

| Option | Values | Default | What it does |
| --- | --- | --- | --- |
| Show Accuracy | Off, On | Off | Adds a line to the panel counting how often the prediction matched the landing exactly. |
| Collision Display | Off, On | Off | Shows stage collision, your ECB, hitboxes and hurtboxes, and hides your model so it does not cover them. |
| Debug Log | Off, Landings, Frames, Everything | Off | Writes extra lines to Dolphin's log. Landings: timer presses and ledge routes. Frames: every airborne frame too. Everything: camera too, and it slows play. |
| Script | Off, plus the scripts in TM/llscript.txt if the disc has that file | Off | For testing. Plays inputs from TM/llscript.txt from a fixed spot and exact to the frame, with Debug Log on. D-pad left plays it again. A shot step freezes the game until D-pad down. |
| Forget Learned ECBs | Press A | n/a | Every fighter's jumps, falls, aerials and airdodge are built in. This forgets what was learned while playing and goes back to the built-in data. |
| About | Info only | n/a | Says that gray paths rely on ECB frames the event has not seen, and that D-pad up during Frame Advance hides everything for screenshots. |

Script lists only Off unless the disc has TM/llscript.txt. When it does, the list holds the file's scripts (the first 48) and All if there is more than one.

[Back to the guide](README.md) · [Back to the README](../../README.md)
