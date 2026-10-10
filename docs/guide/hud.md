# HUD

The HUD menu sets how your controller and the info panel show on screen, and how the quick menu behaves.

The controller display shows what the game actually read from your stick. That is often not what you meant.

<!-- GIF: the Ring controller display beside the percent, with the stick trail, deadzone band and fastfall line, at default settings -->

## Options

The HUD menu, in menu order.

| Option | Values | Default | What it does |
| --- | --- | --- | --- |
| Controller | By Percent, Bottom Left, Bottom Right, Off | By Percent | Shows your real stick, the band where Melee reads an axis as zero, a trail of the last frames, and the fastfall line (lit when a flick would fastfall). By Percent puts it beside your percent display. |
| Controller Look | Ring, Crest, Classic | Ring | Ring: the whole controller in a small ellipse, with L and R as its halves, filling into their nubs. Crest: a shield with L and R as its wings. Classic: the wider block with trigger bars. |
| Controller Size | Small, Medium, Large | Small | How big the Ring look is. Small takes the same room as TM-CE's own controller display. |
| Buttons and Triggers | Off, On | On | Shows the buttons, triggers and C-stick too, in the controller's own colors. |
| Controller Cues | Off, Closing Ring, Gauge | Off | Shows the button an AI or waveland window wants, timed on the controller: a ring closing in on it, or a gauge going round it. It is never filled, so it cannot pass for a press. Needs Buttons and Triggers. |
| Shield Drop Zone | Off, On | On | Shades the stick angles that drop through a platform out of shield, in the stick's gate. Shown while Falcon is on a platform. |
| Info Panel | Off, On | On | Two lines in a top corner: what is coming up, and how your last attempt went. |
| Panel Side | Auto, Right, Left | Auto | Where the info panel goes. Auto keeps it on the side of the screen Falcon is not on. |
| Quick Menu | Off, On | On | L or R clicked all the way with D-pad up or down freezes the game and opens a deck of your presets. Left and right flip through them. Down changes the chosen one's settings. |
| Quick Menu Idle | 3 s, 5 s, 8 s, Never | 5 s | The quick menu stays up when you let go of the trigger. It fades out and closes after this long with no D-pad or trigger press. B closes it at once. |
| Quick Menu Play | 1 s, 2 s, 3 s, Never | 2 s | Moving the stick plays out of the quick menu. The game goes on and the menu fades out over this long. The D-pad or a trigger brings it back. |

## Controller display

The controller display draws your real stick, the deadzone band, a trail of the last frames and the fastfall line. The fastfall line lights when a flick would fastfall.

Controller Look changes how the controller is drawn. Controller Size only changes the Ring look. The display has its own intensity level in Intensity by Group, separate from the main Intensity. See [Visibility](visibility.md).

<details>
<summary>Controller looks: Ring, Crest, Classic</summary>

<!-- GIF: the Ring look at Small, Medium and Large -->
<!-- GIF: the Crest look -->
<!-- GIF: the Classic look -->

</details>

### Controller cues

Controller Cues shows the button an AI or waveland window wants, on the controller itself. Closing Ring is a ring that closes in on the button. Gauge is a gauge that goes round it. Neither ever fills, so neither can look like you pressed the button. They need Buttons and Triggers on.

<details>
<summary>Controller cues: Closing Ring and Gauge</summary>

<!-- GIF: an AI with Controller Cues on Closing Ring, the ring closing on A -->
<!-- GIF: the same AI with Controller Cues on Gauge -->

</details>

### Shield drop zone

While Falcon is on a platform, Shield Drop Zone shades the stick angles in the gate that drop you through the platform out of shield.

<!-- GIF: Falcon in shield on a platform with the shield drop zone shaded in the gate -->

## Info panel

The info panel is two lines in a top corner. One says what is coming up. The other says how your last attempt went. Panel Side Auto puts it on the side of the screen Falcon is not on.

## Quick menu timing

Quick Menu Idle and Quick Menu Play only matter if Quick Menu is on.

- Idle is how long the quick menu stays up with no D-pad or trigger press. It stays up when you let go of the trigger. Never keeps it open until you press B.
- Play is how long the menu takes to fade out after you move the stick, while the game goes on. Never keeps it up and the stick does not play out of it.

All of the quick menu's inputs are in [Presets](presets.md#quick-menu).

## Sounds menu

The Sounds menu sets the chime on a hit and which slips buzz. It is in the main Landing Lab menu, after HUD.

| Option | Values | Default | What it does |
| --- | --- | --- | --- |
| Chime on a Hit | Off, On | On | Chimes when you land a NIL, an AI or a perfect waveland, for the cues that are on. |
| Buzz: Missed Window | Off, On | On | Buzzes when you press for an AI or waveland window and miss it, early or late. |
| Buzz: Skipped Window | Off, On | Off | Buzzes when an AI or waveland window passes with no press at all. Off, you can watch windows go by to get a feel for the timing. |
| Chime: Ledge Route | Off, On | On | Chimes when a ledge route lands with GALINT left, instead of the cue chime. |
| Buzz: Lost Ledge Route | Off, On | On | Buzzes when a ledge route ends without its NIL or AI: landing lag, or missing the stage. |
| Buzz: Fastfall Timing | Off, On | Off | Buzzes and starts over when a ledge route's fastfall is early or late, even if it still lands. Off: only the panel notes it. |
| Buzz: Jump Timing | Off, On | Off | Buzzes and starts over when a ledge route's double jump is early or late, even if it still lands. Off: only the panel notes it. |
| Buzz: Aerial Timing | Off, On | Off | Buzzes and starts over when a ledge route's aerial is outside its window, even if it still lands. Off: only the panel notes it. |

The Missed and Skipped buzzes are about cues. The last five rows are about [ledge routes](ledge.md).

Controller rumble is a separate setting. See [Cues](cues.md#rumble).

[Back to the README](../../README.md)
