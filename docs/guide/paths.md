# Paths

Paths draw where Falcon will go and mark the inputs along the way.

Paths show where Falcon's ECB is headed. Crackerfracks keeps most of them **off**. They're still here for everyone else.

<!-- GIF: Falcon in the air with input markers and frame dots on the path, at default settings -->

## Options

The Paths menu, in menu order.

| Option | Values | Default | What it does |
| --- | --- | --- | --- |
| Landing Path | Off, On | Off | Draws where the bottom of Falcon's ECB goes if you keep holding the stick and press nothing. It uses the landing's color when its cue is on. |
| Body Path | Off, On | Off | Also draws a dotted line through Falcon's body, which is easier to follow than the ECB bottom. |
| Ledge Route | Off, On | On | While Falcon hangs on a ledge and on the way back, draws the chosen route's path. Shown with Show Routes or Assist on in Ledge Practice, and for the jump chosen in Jump Timing. |
| Input Markers | Off, On | On | Marks the inputs along the path where each is due, and which aerials interrupt. White: stick. Yellow: jump. Pink: aerial (the C-stick directions that work light up). Cyan: airdodge. |
| Marker Size | Small, Medium, Large | Medium | How big the input markers and the aerial picker are. |
| Frame Dots | Frame Advance, Always, Off | Always | A small ring on the paths at every frame. Closer rings mean slower movement. Shown only during Frame Advance (always on ledge routes), always, or never. |
| Slide-off Line | Off, On | Off | When a waveland or wavedash would slide off the edge, draws where Falcon goes: full stick that way until it starts, then your real stick. |
| Jump Preview | Both, Full hop, Short hop, Off | Off | On the ground, shows where a full hop and a short hop would go if you jumped now, holding the stick where it is. |

## How to read it

- The short hop path is dashed and the body path is dotted.
- The ECB path is where the bottom of the diamond goes, not where the feet are drawn. Turn on Body Path if that is hard to follow.
- Input marker colors: white for the stick, yellow for a jump, pink for an aerial, cyan for an airdodge. On an aerial marker, the C-stick directions that work light up.
- A path drawn gray relies on ECB frames the event has not seen yet. It learns them as you play, so the path fills in with use. See [Troubleshooting](troubleshooting.md).
- Closer frame dots mean Falcon is moving slower there. Use them with Frame Advance to see which frame a landing happens on.

## Tips

- Landing Path with Body Path off is the plainest look. Add Body Path if you lose the line.
- If frame dots clutter the screen, set Frame Dots to Frame Advance and they only appear when you step. Ledge routes always show them.
- Jump Preview and the Slide-off Line are off at the start. Turn Jump Preview on to see a full hop and a short hop from the ground.
- Paths get fainter or bolder with Intensity, and Paths in Intensity by Group moves them against it. See [Visibility](visibility.md).

[Back to the README](../../README.md)
