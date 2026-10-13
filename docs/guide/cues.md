# Cues

Cues mark the frames where a press gives you a landing: a [NIL](glossary.md#nil), an [aerial interrupt](glossary.md#aerial-interrupt-ai) or a [waveland](glossary.md#waveland). Each cue is a countdown to a [window](glossary.md#window) plus a mark on the path where that window is.

Each cue is a promise: press on this frame and you land. When one breaks that promise, [Troubleshooting](troubleshooting.md) explains how to report it. Frame Advance settles who was wrong, and it has not always been you.

<!-- GIF: ../media/cues-ai.gif (ai-plain, Battlefield): a double jump toward the top platform with the pink AI cue counting down to the nair, at default settings -->

## Options

The Cues menu, in menu order.

| Option | Values | Default | What it does |
| --- | --- | --- | --- |
| NIL Cues | Off, On | Off | Green cue. Marks where holding the stick lands you with no landing lag (a NIL). |
| AI Cues | Off, On | On | Pink cue. Marks where pressing an aerial lands you (an aerial interrupt). The path shows where, with arrows for the aerials that work there. |
| Waveland Cues | Off, Platforms, All Floors | All Floors | Cyan cue. Marks when an airdodge just below sideways lands at once with full speed (a [perfect waveland](glossary.md#perfect-waveland)), and the [wavedash](glossary.md#wavedash) out of a jump. Platforms leaves out wavelands onto the main floor. |
| AI Filter | Useful, All | Useful | Useful shows only aerial interrupts that land while you're still rising, save 4 frames or more, and land without the aerial's landing lag. All shows every one, falling ones included. |
| AI Aerial | Any, Nair, Fair, Bair, Uair, Dair | Any | Counts down to one aerial's window only. Any uses the aerial that works on the most frames. With the stick held in, A is a fair, not a nair, and its window can be shorter. |
| Body Flash | Off, On | On | Flashes you in the cue's color on the first frame you can act after a hit. Tints you periwinkle while you keep [ledge intangibility](glossary.md#ledge-intangibility). |
| Platform Glow | Off, On | Off | Lights up the floor a waveland or wavedash slides along. It fills in toward the landing spot as the window nears and flashes on each of its frames. |
| Intensity | 1 Faint, 2 Soft, 3 Standard, 4 Bold, 5 Boldest | 3 Standard | How strongly the cues, paths and timers show. Standard is the usual look. See [Visibility](visibility.md). |
| Intensity by Group | Submenu | n/a | Sets cues, paths, timers, ledge info and the controller fainter or bolder than Intensity, and holds Auto Fade. See [Visibility](visibility.md). |
| Rumble | Submenu | n/a | Buzzes the controller up to a cue's window. See below. |

## Cue colors

This is the legend the main menu's Help row shows in game.

| Color | Means |
| --- | --- |
| Green | NIL |
| Pink | Aerial interrupt |
| Cyan | Waveland and wavedash |
| Periwinkle | GALINT |
| Slate | Missed |

In the Fixed Strip's Cells look, solid cells are the frames to press on, hollow cells are the frames you [touch down](glossary.md#touchdown), and the white gate marks the frame to press now. The other looks are explained in [Timers](timers.md).

## How to read it

Each cue is a timer plus a mark on the path. The timer counts down to its window. The path shows where you land, and with AI Cues on it adds arrows for the aerials that work there.

## Spot cues

The landing spot itself gets cues of its own, all in the Timers menu: a pair of brackets that close in on it and meet on the frame to press (Landing Spot), and the ticks, rails or chevrons that meet at a waveland's slide (Waveland). Platform Glow, in this menu, lights the floor under the slide.

<details>
<summary>The landing spot on the path, in the timer, and with the glow</summary>

<!-- GIF: ../media/spot-path.gif (spot-path, Dream Land) -->
<!-- GIF: ../media/spot-timer.gif (spot-timer, Dream Land) -->
<!-- GIF: ../media/spot-glow.gif (spot-glow, Dream Land) -->

</details>

## Tips

- AI Filter is on Useful so you only see AIs worth practicing. Falling AIs are real aerial interrupts; they just save about a frame, so Useful hides them along with anything else under 4 frames. Set it to All if you want every one.
- Pick an aerial in AI Aerial when you want to drill one move.
- Chimes and buzzes for hits and misses are in the Sounds menu. See [HUD](hud.md#sounds-menu).

<details>
<summary>Body Flash and Platform Glow, off and on</summary>

<!-- GIF: ../media/wl-plain.gif (wl-plain, Stadium): the same waveland with Body Flash and Platform Glow off -->
<!-- GIF: ../media/wl-flash.gif (wl-flash, Stadium): Body Flash on -->
<!-- GIF: ../media/wl-flash-glow.gif (wl-flash-glow, Stadium): both on -->

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

[Back to the guide](README.md) · [Back to the README](../../README.md)
