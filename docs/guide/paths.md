# Paths

Paths draw where you will go and mark the inputs along the way.

A path is where your [ECB](glossary.md#ecb) is headed. Crackerfracks keeps most of them **off**. They're still here for everyone else.

<!-- GIF: ../media/paths.gif (paths, Battlefield): a fighter in the air with input markers and frame dots on the path, at default settings -->

## Options

The Paths menu, in menu order.

| Option | Values | Default | What it does |
| --- | --- | --- | --- |
| Landing Path | Off, On | Off | Draws where the bottom of your ECB goes if you keep holding the stick and press nothing. It uses the landing's color when its cue is on. |
| Body Path | Off, On | Off | Also draws a dotted line through your body, which is easier to follow than the ECB bottom. |
| Ledge Route | Off, On | On | While you hang on a ledge and on the way back, draws the chosen route's path. Shown with Show Routes or Assist on in Ledge Practice. Jump Timing's path has its own toggle there. |
| Input Markers | Off, On | On | Marks the inputs along the path where each is due, and which aerials interrupt. White: stick. Yellow: jump. Pink: aerial (the C-stick directions that work light up). Cyan: airdodge. A marker brightens over the last frames before its press, and on the press it fills with its color in a white frame. |
| Window Bands | Off, On | On | Shades the stretch of the path where a press works: cyan for a [perfect waveland](glossary.md#perfect-waveland)'s airdodge, pink for an [AI](glossary.md#aerial-interrupt-ai)'s aerial. Your feet are in it on those frames. |
| Marker Size | Small, Medium, Large | Medium | How big the input markers and the aerial picker are. |
| Frame Dots | Frame Advance, Always, Off | Always | A small ring on the paths at every frame. Closer rings mean slower movement. Shown only during [Frame Advance](glossary.md#frame-advance) (always on ledge routes), always, or never. |
| Slide-off Line | Off, On | Off | When a waveland or wavedash would slide off the edge, draws where you go: full stick that way until it starts, then your real stick. |
| Jump Preview | Both, Full hop, Short hop, Off | Off | On the ground, shows where a [full hop and a short hop](glossary.md#short-hop-and-full-hop) would go if you jumped now, holding the stick where it is. |

## How to read it

- The short hop path is dashed and the body path is dotted.
- The ECB path is where the bottom of the diamond goes, not where the feet are drawn. Turn on Body Path if that is hard to follow.
- Input marker colors: white for the stick, yellow for a jump, pink for an aerial, cyan for an airdodge. On an aerial marker, the C-stick directions that work light up.
- A path drawn gray relies on an ECB frame the event's data does not cover. It learns the frame as you play, so the path fills in with use. See [Troubleshooting](troubleshooting.md).
- Closer frame dots mean you are moving slower there. Use them with Frame Advance to see which frame a landing happens on.

## Slide-off Line

With Slide-off Line on, a waveland or wavedash that would carry you off the edge of a platform or the stage draws the rest of the way: where you go holding the stick fully toward the edge until the slide starts, then where your real stick takes you.

<!-- GIF: ../media/slide-off.gif (slide-off, Stadium): a waveland that slides off a platform's edge, with the Slide-off Line on -->

## Tips

- Landing Path with Body Path off is the plainest look. Add Body Path if you lose the line.
- If frame dots clutter the screen, set Frame Dots to Frame Advance and they only appear when you step. Ledge routes always show them.
- Jump Preview and the Slide-off Line are off at the start. Turn Jump Preview on to see a full hop and a short hop from the ground.
- Paths get fainter or bolder with Intensity, and Paths in Intensity by Group moves them against it. See [Visibility](visibility.md).

[Back to the guide](README.md) · [Back to the README](../../README.md)
