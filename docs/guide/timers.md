# Timers

Timers count down the [frames](glossary.md#frame) to each input so you can press on the right one.

Six ways to count down next to you, four in a corner, one at the landing spot, three for wavelands and three for wavedashes. Pick one. Picking all of them is how 0.6.5 happened.

<!-- GIF: ../media/timers.gif (strip-conveyor, Battlefield): a double jump toward the top platform with the Conveyor look in the corner, the landing spot brackets closing, and a press on the strikeline, at default settings -->

## The words

- **Near You** is the timer that rides with your fighter. **Fixed Strip** is the one in a corner of the screen. They are two rows of the Timers menu and each has several looks.
- The Fixed Strip's looks are **Cells**, **Highway**, **Dial** and **Conveyor**. "The Cells look" always means this one; the Wavedash row has a value called Cells too, which adds a row to it.
- A **lane** is one cue's line in the Fixed Strip. There are four, always in the same place and order: JUMP (hops and double jumps), AI, NIL, and WAVE (wavelands and wavedashes). An empty lane stays drawn, faintly. A ledge drop drill or a second route borrows a fifth lane while it lasts. Cells label their rows; Highway and Dial mark each lane with its note shape (yellow tab, pink solid, green notched, cyan hollow). Conveyor puts every cue on one belt and tells them apart by the same shapes.
- The **gate** is the white line in the Fixed Strip where a press is due. (The controller display has a stick octagon, and Jump Timing's path has colored bars. Neither is called a gate on these pages.)
- A **window** is the frames a press works on. A **note** is a window as the Highway or Conveyor draws it.

## Options

The Timers menu, in menu order.

| Option | Values | Default | What it does |
| --- | --- | --- | --- |
| Near You | Off, Bubble, Halo, Pincers, ECB Fill, Lights, Old Strip | Off | A countdown that rides with you. Bubble: a ring closes on where your body will be. Halo: a bead runs into a notch. Pincers, ECB Fill and Lights close, fill or count in on you. |
| Fixed Strip | Off, Cells, Highway, Dial, Conveyor | Conveyor | A countdown in a corner. Cells slide into a gate, Dial sweeps a hand in, Highway and Conveyor (one moving belt) send notes: yellow tab JUMP, pink solid AI, green notched NIL, cyan hollow WAVE. |
| Wide Cells | Off, On | On | Gives each frame a little more room: wider cells in Cells and the Near You strips, taller notes on the Highway and Conveyor. The Dial is unchanged. |
| Landing Spot | Off, On | On | Brackets close in on the landing spot of an AI or NIL and meet it on the frame to press. |
| Waveland | Off, Ticks, Rails, Chevrons | Off | Drawn at the slide. Ticks run in from its ends and spike where they meet, rails fill in, or chevrons hop in a notch a frame. They meet on the frame to airdodge. |
| Wavedash | Off, Cells, Pips, Ring | Off | Drawn out of the [jumpsquat](glossary.md#jumpsquat). Cells: a WD lane in the Fixed Strip. Pips: a pip a frame under your feet. Ring: a ring on the floor that closes on the airdodge frame. Works with Waveland Cues off. |
| Note Travel | Fixed Speed, Catch Up | Fixed Speed | Applies to Cells and Highway. Fixed Speed: a frame is always the same distance, so a window found late shows up partway along. Catch Up: every window comes in from the far end and settles to that speed halfway in. |

## How to read it

Every look uses the same colors: pink for an AI, green for a NIL, cyan for a waveland or wavedash, yellow for a jump. A timer that goes slate means you missed the window.

- **Cells.** Each lane is a row of frame cells sliding toward the gate, one cell a frame. Solid cells are the frames to press on; hollow cells are the frames you [touch down](glossary.md#touchdown). Press when a solid cell reaches the gate. A tick under every cell and a brighter empty cell every fifth frame show the game's clock going by, so you can see the row moving even with no window on it.
- **Highway.** One upright lane per cue. Notes fall down the lane onto a strikeline, and each lane has a window one note tall above it. Press when the note is in the window.
- **Dial.** A hand sweeps round the dial and the window is a wedge on the rim. Press as the hand enters the wedge. Ticks round the rim mark the frames.
- **Conveyor.** One belt for every cue, moving a row a frame, with a heavier line every fifth frame. Notes on the same frame share a row. Shapes tell the kinds apart: a jump has a raised cap, an AI is solid, a NIL is notched, a waveland or wavedash is hollow. A plan from a press you haven't made yet hovers over the rows it will take and sets down when you press. A row of four small swatches under the belt shows the shapes in the cue colors, lit while that kind is on the belt.
- You can run several timers at once. The Fixed Strip (Conveyor) and Landing Spot are on at the start. Waveland and Wavedash are off; the Conveyor's WAVE shapes time wavelands onto platforms, and the Wavedash and Everything presets turn those two timers on.

How a window ends tells you where it was due. A hit flashes the gate white (on Highway the note burns white and a beam runs up the lane). A skip leaves the note in the gate or window in gray for a moment. A miss leaves it cracked in two. An early press on the JUMP lane says how early.

## Tips

- Start with the Fixed Strip. It stays in one place, so you can read it without looking at your fighter.
- Try Near You when you want the countdown where you are already looking. Bubble draws every live cue at its own spot. The other looks draw only the soonest cue, and when two tie, the AI comes before the NIL and the NIL before the waveland.
- While ledge routes are showing in the Fixed Strip, the Near You timer is not drawn.
- Timers get fainter or bolder with Intensity, and each can fade as you learn it. See [Visibility](visibility.md).

<details>
<summary>Fixed Strip: Cells, Highway, Dial, Conveyor</summary>

<!-- GIF: ../media/strip-cells.gif (strip-cells, Battlefield): the same AI with Fixed Strip set to Cells -->
<!-- GIF: ../media/strip-highway.gif (strip-highway, Battlefield): the same AI with Fixed Strip set to Highway -->
<!-- GIF: ../media/strip-dial.gif (strip-dial, Battlefield): the same AI with Fixed Strip set to Dial -->
<!-- GIF: ../media/strip-conveyor.gif (strip-conveyor, Battlefield): the same AI with Fixed Strip set to Conveyor -->

</details>

<details>
<summary>Near You: Bubble, Halo, Pincers, ECB Fill, Lights, Old Strip</summary>

<!-- GIF: ../media/near-bubble.gif (near-bubble, Battlefield): the same AI with Near You set to Bubble -->
<!-- GIF: ../media/near-halo.gif (near-halo, Battlefield): the same AI with Near You set to Halo -->
<!-- GIF: ../media/near-pincers.gif (near-pincers, Battlefield): the same AI with Near You set to Pincers -->
<!-- GIF: ../media/near-ecbfill.gif (near-ecbfill, Battlefield): the same AI with Near You set to ECB Fill -->
<!-- GIF: ../media/near-lights.gif (near-lights, Battlefield): the same AI with Near You set to Lights -->
<!-- GIF: ../media/near-oldstrip.gif (near-oldstrip, Battlefield): the same AI with Near You set to Old Strip -->

</details>

<details>
<summary>Waveland: Ticks, Rails, Chevrons</summary>

<!-- GIF: ../media/wl-ticks.gif (waveland, Stadium): the same perfect waveland with Waveland set to Ticks -->
<!-- GIF: ../media/wl-rails.gif (wl-rails, Stadium): the same perfect waveland with Waveland set to Rails -->
<!-- GIF: ../media/wl-chevrons.gif (wl-chevrons, Stadium): the same perfect waveland with Waveland set to Chevrons -->

</details>

<details>
<summary>Wavedash: Cells, Pips, Ring</summary>

<!-- GIF: ../media/wd-cells.gif (wavedash, first half, Final Destination): the same wavedash with Wavedash set to Cells -->
<!-- GIF: ../media/wd-pips.gif (wd-pips, Final Destination): the same wavedash with Wavedash set to Pips -->
<!-- GIF: ../media/wd-ring.gif (wd-ring, Final Destination): the same wavedash with Wavedash set to Ring -->

</details>

<details>
<summary>Note Travel: Fixed Speed and Catch Up</summary>

<!-- GIF: ../media/travel-fixed.gif (hero, Battlefield): the same AI with Note Travel set to Fixed Speed, with a window found late -->
<!-- GIF: ../media/travel-catchup.gif (travel-catchup, Battlefield): the same AI with Note Travel set to Catch Up -->

</details>

[Back to the guide](README.md) · [Back to the README](../../README.md)
