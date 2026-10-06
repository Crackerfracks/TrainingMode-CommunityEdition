<p align="center"><img src="https://raw.githubusercontent.com/Crackerfracks/TrainingMode-CommunityEdition/master/docs/media/hero-ai.gif" width="480" alt="Falcon jumps toward a Battlefield platform; a pink path marks the aerial interrupt and the timer counts down to the nair"></p>

**Landing Lab** is a new Captain Falcon event for Training Mode - Community Edition. It predicts your landing every frame using Melee's own collision rules, and shows you what's coming before you touch down: NILs in green, aerial interrupts in pink, perfect wavelands in cyan, and the exact frame to press for each.

This is an unofficial build, not made or supported by the TM-CE team. Landing Lab's code was written with Claude, an AI coding assistant, so please send feedback here rather than to TM-CE.

## Get it

1. Download **TM-CE-LandingLab-0.6.5.zip** below. It's a small patch, so you bring your own Melee NTSC 1.02 ISO.
2. On Windows, drag your ISO onto `DRAG VANILLA MELEE HERE.bat`. On Linux or macOS, install xdelta and run `./build_linux_and_mac.sh path/to/melee.iso`.
3. Open `TM-CE-LandingLab.iso` in Dolphin, then go to Event Mode, Character-specific Tech, **Landing Lab**. Start opens the menu.

The rest of TM-CE v1.4 is untouched.

## What it shows

<table>
<tr>
<td width="50%" valign="top"><img src="https://raw.githubusercontent.com/Crackerfracks/TrainingMode-CommunityEdition/master/docs/media/nil.gif" alt="A green path and closing brackets mark a NIL onto a platform"><br><b>NILs.</b> Green means holding the stick lands you straight into standing with no landing lag. Brackets close in on the spot as the touchdown gets near.</td>
<td width="50%" valign="top"><img src="https://raw.githubusercontent.com/Crackerfracks/TrainingMode-CommunityEdition/master/docs/media/waveland.gif" alt="A cyan cue, glowing platform and converging ticks for a perfect waveland"><br><b>Wavelands.</b> Cyan marks the frames where an airdodge lands at once with full speed. The platform lights up along the slide, and ticks close in on where you'll stop.</td>
</tr>
<tr>
<td valign="top"><img src="https://raw.githubusercontent.com/Crackerfracks/TrainingMode-CommunityEdition/master/docs/media/wavedash.gif" alt="The wavedash timer counting down to the airdodge out of jumpsquat"><br><b>Wavedash timer.</b> Frame cells slide into a gate next to Falcon, so you can see the airdodge frame coming and whether you hit it.</td>
<td valign="top"><img src="https://raw.githubusercontent.com/Crackerfracks/TrainingMode-CommunityEdition/master/docs/media/controller.gif" alt="The Ring controller display with stick trail, fastfall line and buttons"><br><b>Controller display.</b> Your real stick inside its gate, the band where Melee reads zero, a trail of recent frames, and a fastfall line that lights when a flick would fastfall. It moves to stay clear of every percent.</td>
</tr>
</table>

## Ledge Practice

<table>
<tr>
<td width="50%" valign="top"><img src="https://raw.githubusercontent.com/Crackerfracks/TrainingMode-CommunityEdition/master/docs/media/ledge-route.gif" alt="Falcon on the Battlefield ledge with the route line and input chips, then the route into a nair aerial interrupt"><br><b>Routes.</b> Hang on a ledge and Landing Lab lists the fastest ways back to stage, ranked by the ledge intangibility (GALINT) you keep: drop, fastfall, double jump, then a NIL or an AI. Each input sits on the path where it's due.</td>
<td width="50%" valign="top"><img src="https://raw.githubusercontent.com/Crackerfracks/TrainingMode-CommunityEdition/master/docs/media/assist.gif" alt="Assist freezing the game on a route input until it's pressed"><br><b>Assist.</b> Quicktime practice for any route. The game freezes on each input until you hit it, then plays on at full speed. Turn the wait down as the timing sinks in.</td>
</tr>
</table>

## Frame by frame

<table>
<tr>
<td width="50%" valign="top"><img src="https://raw.githubusercontent.com/Crackerfracks/TrainingMode-CommunityEdition/master/docs/media/frame-advance.gif" alt="Frame Advance stepping through a jump with frame dots and input markers"><br><b>Frame Advance.</b> D-pad down freezes the game, and L steps one frame (hold it to step slowly). Frame dots show your speed and every marker sits on the frame it's due.</td>
<td width="50%" valign="top"><img src="https://raw.githubusercontent.com/Crackerfracks/TrainingMode-CommunityEdition/master/docs/media/menu.png" alt="The Landing Lab menu"><br><b>Your call what's on screen.</b> Every cue, path and HUD piece has its own toggle, so you can strip it down to just the thing you're drilling.</td>
</tr>
</table>

## Known issues

- Captain Falcon only for now.
- Some NILs out of a double jump show up one frame early.
- Tested in Dolphin, not yet on console.
- Once, stray drawings showed up over the version text in the corner until a restart. If it happens to you, a screenshot would help.

## Feedback

Found a cue that lied to you, or know a better ledge route? Open an issue here or message Stephen. If you can, include the stage and the frame you pressed on; Frame Advance makes that easy to count.

The full guide (every menu option and control) is in the [README](https://github.com/Crackerfracks/TrainingMode-CommunityEdition#readme).
