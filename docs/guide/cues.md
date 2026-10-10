# Cues

Cues mark the frames where a press gives you a landing: a NIL, an aerial interrupt or a waveland.

Each cue is a promise: press on this frame and you land. When one breaks that promise, [Troubleshooting](troubleshooting.md) explains how to report it.

<!-- GIF: Battlefield, double jump toward the top platform with the pink AI cue counting down to the nair, at default settings -->

## Options

The Cues menu, in menu order.

| Option | Values | Default | What it does |
| --- | --- | --- | --- |
| NIL Cues | Off, On | Off | Green cue. Marks where holding the stick lands you with no landing lag (NIL). |
| AI Cues | Off, On | On | Pink cue. Marks where pressing an aerial lands you (aerial interrupt). The path shows where, with arrows for the aerials that work there. |
| Waveland Cues | Off, Platforms, All Floors | All Floors | Cyan cue. Marks when an airdodge just below sideways lands at once with full speed (a perfect waveland), and the wavedash out of a jump. Platforms leaves out wavelands onto the main floor. |
| AI Filter | Useful, All | Useful | Useful shows only aerial interrupts that land while Falcon is still rising and save 4 frames or more without aerial lag. All shows every one. |
| AI Aerial | Any, Nair, Fair, Bair, Uair, Dair | Any | Counts down to one aerial's window only. Any uses the aerial that works on the most frames. With the stick held in, A is a fair, not a nair, and its window can be shorter. |
| Body Flash | Off, On | On | Flashes Falcon in the cue's color on the first frame he can act after a hit. Tints him periwinkle while he keeps ledge intangibility. |
| Platform Glow | Off, On | Off | Lights up the floor a waveland or wavedash slides along. It fills in toward the landing spot as the window nears and flashes on each of its frames. |
| Intensity | 1 Faint, 2 Soft, 3 Standard, 4 Bold, 5 Boldest | 3 Standard | How strongly the cues, paths and timers show. Standard is the usual look. See [Visibility](visibility.md). |
| Intensity by Group | Submenu | n/a | Sets cues, paths, timers, ledge info and the controller fainter or bolder than Intensity, and holds Auto Fade. See [Visibility](visibility.md). |
| Rumble | Submenu | n/a | Buzzes the controller up to a cue's window. See below. |

## Cue colors

| Color | Means |
| --- | --- |
| Green | NIL |
| Pink | Aerial interrupt |
| Cyan | Waveland and wavedash |
| Periwinkle | GALINT |
| Slate | Missed |

In a timer, solid cells are the frames to press on, hollow cells are the frames you touch down, and the white gate marks the frame to press now. See [Timers](timers.md).

## How to read it

Each cue is a timer plus a mark on the path. The timer counts down to its window. The path shows where Falcon lands, and with AI Cues on it adds arrows for the aerials that work there.

## Tips

- AI Filter is on Useful so you only see AIs worth practicing. Set it to All if you want every one, including AIs that save less than 4 frames or land on the way down.
- Pick an aerial in AI Aerial when you want to drill one move.
- The landing spot brackets that meet at the cue are a Timers option (Landing Spot). See [Timers](timers.md).
- Chimes and buzzes for hits and misses are in the Sounds menu. See [HUD](hud.md#sounds-menu).

<details>
<summary>Body Flash and Platform Glow, off and on</summary>

<!-- GIF: the same waveland with Body Flash and Platform Glow off, then Body Flash on, then both on -->

</details>

## Rumble

Rumble buzzes your controller faster and faster up to a window and stops dead as it opens. Each toggle needs its cue turned on.

| Option | Values | Default | What it does |
| --- | --- | --- | --- |
| AI Rumble | Off, On | Off | Buzzes faster and faster up to an AI window and stops dead as it opens. Needs AI Cues on. |
| NIL Rumble | Off, On | Off | The same for NIL windows. Needs NIL Cues on. |
| Waveland Rumble | Off, On | Off | The same for perfect waveland and wavedash windows. Needs Waveland Cues on. |
| Game Rumble | Info only | n/a | While any of these is on, the game's own rumble is off for your controller, so only the cues buzz. |

Dolphin only passes rumble to a real controller with rumble turned on in its settings.

[Back to the README](../../README.md)
