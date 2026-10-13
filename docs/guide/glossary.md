# Glossary

The Melee words Landing Gear uses, each in plain words, for Melee NTSC 1.02. Every page of the guide links here the first time it uses one.

Most of Melee's vocabulary is players naming things the developers never meant to ship. The developers found out from the players.

<!-- GIF: ../media/glossary-ai.gif (ai-plain, Battlefield): a double jump toward a platform that lands by pressing nair while still rising, at default settings -->

## Frame

One 60th of a second. Melee counts everything in frames, and so does Landing Gear. A 4-frame window is a fifteenth of a second.

## Frame Advance

Freezing the game and stepping forward one frame at a time. D-pad down turns it on and off. The Advance Button (Z unless you change it in Speed) steps one frame; hold it to step slowly.

## Platform, floor and ledge

The floor is the stage's main surface. Platforms float above it: you can stand on them, jump up through them and drop down through them. The ledges are the floor's two ends, which you can hang from. Battlefield has three platforms and two ledges.

## Aerial

An attack done in the air: neutral, forward, back, up or down air (nair, fair, bair, uair, dair). A with the stick, or the C-stick in a direction.

## ECB

The Environment Collision Box: the invisible diamond Melee uses for your body when it checks for floors, walls and ceilings. Its shape changes with the animation and updates once a frame, so its bottom point is not where your feet are drawn. Landing Gear simulates it to predict where you land.

<!-- GIF: ../media/glossary-frames.gif (ai-frames, Battlefield): the same AI in Frame Advance, one held frame per shot, the ECB updating to the aerial and touching the platform -->

## Window

The frames a press works on. Most windows in Landing Gear are a few frames long; some are one. Every cue counts down to a window.

## Cue

A countdown to a window, plus a mark on the path where that window is. Landing Gear has three kinds: NIL (green), aerial interrupt (pink), waveland and wavedash (cyan).

## Touchdown

The frame your ECB meets the floor. The timers draw a touchdown as a hollow cell and the press that gets you there as a solid one.

## Landing lag

The frames you're stuck after touching down. A plain landing from a jump or a fall is 4 frames. An aerial has its own landing lag, and it is longer.

## Jumpsquat

The frames on the ground between pressing jump and leaving it. Every fighter has its own number: Fox's is 3, Falcon's is 4, Bowser's is 8.

## Short hop and full hop

Let go of the jump button before jumpsquat ends and you get a short hop, a lower jump. Hold it through and you get a full hop. Landing Gear writes them SH and FH.

## Double jump

Your jump in the air. Most fighters have one. Kirby and Jigglypuff have five; Landing Gear plans with their first and leaves the rest to them. Ledge routes and Jump Timing both start from a double jump you haven't used yet.

## Fastfall

Tapping down on the stick once you've started falling (it does nothing while you rise) to fall faster. Ledge routes can include one, and Landing Gear tells you when yours was early or late.

## NIL

No-impact landing: touching down with no landing lag at all, so you can act the moment you're on the floor. It comes from meeting the floor on the right frame of a jump or a fall, with no aerial out; which frame that is depends on where the ECB is, and Landing Gear works it out. NIL cues are green.

## Aerial interrupt (AI)

Starting an aerial with your ECB bottom placed so that, when the ECB updates to the aerial's current frame, it touches the ground and forces a landing there and then. You can be rising or falling; the ground can be a platform, the floor or a ledge. A rising AI ends your jump early and puts you on a platform frames before you would have got there, ready to act. AI cues are pink. The default AI Filter (Useful) shows only AIs that save 4 frames or more, which is nearly every rising one and almost no falling ones.

<!-- GIF: ../media/glossary-ai.gif (ai-plain, Battlefield): the same AI, plain -->

## Autocancel

Landing from an aerial on frames where the move has no landing lag of its own, so you get a plain landing's 4 frames instead. Each aerial has its own autocancel frames. It is not an aerial interrupt, whatever the first draft of this project said.

## L-cancel

Pressing L, R or Z in the 7 frames before landing from an aerial to halve its landing lag.

<!-- GIF: ../media/glossary-lcancel.gif (lcancel, Battlefield) -->

## Airdodge

Pressing L or R in the air: a short dodge in the stick's direction. In the air you can't be hit for most of it, then you fall helpless. Into the ground, it becomes a waveland.

## Waveland

An airdodge into the ground that slides you along it. The slide is the airdodge's landing lag: you can't act until it ends, but you cover ground while you wait. Waveland cues are cyan.

<!-- GIF: ../media/glossary-waveland.gif (slide-off, Stadium): an airdodge down and sideways onto a platform, sliding along it -->

## Perfect waveland

A waveland where the airdodge, angled just below sideways, lands on its first frame and slides at full speed.

## Wavedash

A waveland straight out of a jump: the airdodge comes right out of the jumpsquat, so you slide along the floor without ever really leaving it. In the game since 2001, intended or not.

<!-- GIF: ../media/glossary-wavedash.gif (wd-slow, Final Destination) -->

## Ledgedash

Dropping from the ledge, double jumping back up and wavelanding onto the stage, so you arrive on your feet with ledge intangibility to spare. TM-CE has its own event for it; Landing Gear borrows its reset rules.

## Intangibility

Frames where attacks pass through you. Ledge intangibility and the middle of an airdodge are the two Landing Gear cares about.

## Ledge intangibility

After you grab a ledge, you can't be hit for a while. The count starts at the grab and runs down while you hang, so the longer you wait, the less you keep. Melee players say intangibility; the Ledge Practice row that holds it full is called Keep Ledge Invincibility, and means the same thing here.

## GALINT

The ledge intangibility you still have once you're back on the stage and can act. Ledge routes are ranked by how much of it they keep. Landing Gear draws it in periwinkle.

## Regrab

Letting go of the ledge and grabbing it again.

## Dash

The first frames of a run, from tapping the stick sideways on the ground. The 15 GALINT reference counts its intangibility at the first dash.

## Platform drop and shield drop

A platform drop is passing down through a platform you are standing on. A shield drop is doing it out of shield, by pressing down on the stick at the right angle. The Shield Drop Zone in the HUD shades those angles.

## Deadzone

The band near the stick's center where Melee reads an axis as zero. The controller display draws it.

[Back to the guide](README.md) · [Back to the README](../../README.md)
