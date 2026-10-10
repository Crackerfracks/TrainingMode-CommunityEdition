# Glossary

These are the terms Landing Lab uses, each in a sentence or two, for Melee NTSC 1.02.

Most of Melee's vocabulary is players naming things the developers never meant to ship. Wavedashing is one of them.

<!-- GIF: Falcon double jumps toward a platform and lands on it by pressing nair while still rising, at default settings (an aerial interrupt) -->

## Aerial

An attack done in the air: neutral, forward, back, up or down air (nair, fair, bair, uair, dair).

## ECB

The Environment Collision Box: the diamond Melee uses for a character's body when it checks for floors, walls and ceilings. Its shape changes with the animation, so its bottom is not where the feet are drawn. Landing Lab simulates it to predict landings.

## NIL

No-impact landing. Falcon touches down with no landing lag at all. Landing Lab draws NIL cues in green.

## Aerial interrupt (AI)

Starting an aerial while rising, so that when the ECB updates to the aerial's animation it touches the platform and forces a landing. Falcon skips the rest of his jump and can act. Landing Lab focuses on rising AIs that save 4 or more frames, and draws AI cues in pink.

<!-- GIF: the same AI with Frame Advance on, stepping through the press and the frame the ECB touches the platform -->

## Landing lag

The frames Falcon is stuck after landing from an aerial or a fall.

## L-cancel

Pressing L, R or Z just before landing from an aerial to halve its landing lag.

## Fastfall

Tapping down on the stick while falling to fall faster. Ledge routes can include a fastfall, and Landing Lab tells you when yours was early or late.

## Jumpsquat

The frames on the ground between pressing jump and leaving the ground. Falcon's is 4 frames.

## Double jump

Falcon's second jump, used in the air. Jump Timing only runs while he still has it.

## Waveland

An airdodge into the ground that slides you along it with no lag. Landing Lab draws waveland cues in cyan.

<!-- GIF: Falcon airdodges down and sideways onto a platform and slides along it -->

## Perfect waveland

A waveland where the airdodge, angled just below sideways, lands at once and you slide with full speed.

## Wavedash

A jump straight into a waveland: the airdodge comes right out of the jumpsquat.

## GALINT

The ledge intangibility Falcon still has when he can first act on stage. Ledge routes are ranked by how much of it they keep, and Landing Lab draws it in periwinkle.

## Ledge intangibility

The time after grabbing the ledge when Falcon cannot be hit. It runs down while he hangs, so the GALINT a route keeps shrinks the longer you wait. Keep Ledge Invincibility in Ledge Practice holds it full while you get ready.

## Frame

One 60th of a second. Melee counts everything in frames, and so does Landing Lab.

## Frame Advance

Freezing the game and stepping forward one frame at a time. D-pad down turns it on and off. The Advance button (Z by default) steps one frame; hold it to step slowly.

## Platform drop and shield drop

A platform drop is passing down through a platform you are standing on. A shield drop is doing it out of shield, by pressing down on the stick at the right angle. The Shield Drop Zone in the HUD shades those angles.

## Deadzone

The band near the stick's center where Melee reads an axis as zero. The controller display draws it.

[Back to the README](../../README.md)
