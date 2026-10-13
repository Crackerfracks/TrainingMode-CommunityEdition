# Ledge Practice

Ledge Practice lists the fastest routes from the [ledge](glossary.md#platform-floor-and-ledge) back to the stage and lets you drill them.

The ledge is where you go to be untouchable for a moment. Ledge Practice is about spending that moment well.

<!-- GIF: ../media/ledge.gif (ledge-route, Battlefield): hanging from the Battlefield ledge with the route list in the Fixed Strip and the chosen route's path drawn, at default settings -->

## The words

- A **route** is one way from the hang to the stage: let go (away or down), maybe wait, maybe [fastfall](glossary.md#fastfall), [double jump](glossary.md#double-jump) toward the stage, then land as a [NIL](glossary.md#nil) or press an aerial for an [AI](glossary.md#aerial-interrupt-ai).
- **GALINT** is the [ledge intangibility](glossary.md#ledge-intangibility) a route leaves you once you can act on stage. Routes are ranked by it.
- Three rows share the word route. **Show Routes** (this menu) lists them in the Fixed Strip. **Route** (this menu) picks one. **Ledge Route** (Paths menu) draws the picked one's path.

## Options

The Ledge Practice menu, in menu order.

| Option | Values | Default | What it does |
| --- | --- | --- | --- |
| Show Routes | Off, On | On | While you hang, shows the fastest ways from the ledge to the stage in the Fixed Strip, with the GALINT each one keeps. The chosen route is on top and the next ones are under it. |
| Route Kind | NIL, AI, Both | AI | NIL: land on the stage with no landing lag. AI: land with an aerial interrupt. Both: the two lists together. |
| Route Sort | GALINT, Easiest | GALINT | How the routes are put in order. GALINT: the most ledge intangibility first. Easiest: widest aerial window, no fastfall and least waiting first, then by GALINT. |
| Route | 1 to the number of routes found, shown as "N of M" | 1 | Picks the route directly. Left and right browse. Shows "none" when no route keeps GALINT. Presets do not store it. |
| Assist | Off, On | Off | Quicktime practice. The game freezes on each input of the route and waits for it, then plays on at full speed. A slip only counts as a miss if it costs the landing, or if its timing is checked in Sounds. |
| Assist Wait | 5 s, 3 s, 2 s, 1 s, 0.5 s, 0.25 s, 0.1 s | 2 s | How long Assist waits for each input before it steps a frame or starts over. Shorten it as the timing sinks in, down toward real time. |
| Starting Position | Ledge, Saved Position | Ledge | Where Reset puts you after an attempt: on a ledge, or where you saved with D-pad right. |
| Reset | None, Same Side, Swap, Swap on Success, Random | None | Starts over after each attempt. Swap and Random change ledges. Assist always starts over on the same side when this is None. |
| Reset Delay | Slow, Normal, Fast, Instant | Normal | How long the result stays on screen before starting over. |
| Keep Ledge Invincibility | Off, On | Off | Keeps the full ledge intangibility while hanging, so the routes do not shrink while you get ready. |
| Drop Drill | Off, On | Off | Times letting go of the ledge. A DRP lane counts down to the first frame you can. The stick has to rest on the hang's first frame, then go down or away. It is graded, with how many of your last 10 hit. |
| 15 GALINT Demo | Press A | n/a | **Captain Falcon on Battlefield only.** Plays a bair AI from a [regrab](glossary.md#regrab) on the right ledge that leaves 15 frames of ledge intangibility at the first [dash](glossary.md#dash). D-pad left plays it again. |
| Practice 15 GALINT | Press A | n/a | **Captain Falcon on Battlefield only.** Sets up the same regrab, then Assist waits on each input of the route: the drop, the jump and the bair. A miss or a landing sets it up again. Pick it again to stop. |

## How routes are found and ordered

When you grab a ledge, Landing Gear simulates your [ECB](glossary.md#ecb) through the drop timings, fastfalls, double jump frames and aerials within reach of that ledge, 45 frames past the jump. It keeps the routes that land with GALINT left and lists them in the Fixed Strip.

Every fighter hangs from the ledge in its own spot, and the search starts from yours: the spots are measured for the whole cast, and a fighter the table does not know is measured the first time it hangs. For Kirby and Jigglypuff the jump in a route is the first of their five air jumps.

Route Kind picks which lists you get: NIL routes, AI routes, or both together. Route Sort picks the order:

- GALINT puts the route that keeps the most ledge intangibility first.
- Easiest puts routes first that have the widest aerial window, no fastfall and the least waiting. Ties go by GALINT.

If no route of the chosen kind keeps GALINT, Route shows "none" and its description says so. While the search is still running, the Route description says it is searching.

Some fighters have no ledge route that keeps GALINT, with either kind: Kirby, Jigglypuff, Peach and Pichu. That is the game, not the trainer. Peach's double jump keeps rising for about 50 frames, which is longer than the search looks.

## Choosing a route

- The chosen route is on top of the Fixed Strip, and the next ones are under it.
- D-pad up while hanging shows the next route. After the last it goes back to the first. This needs more than one route and Show Routes or Assist on.
- Route in this menu picks any route directly. It reads "N of M".
- If you pressed D-pad up in Frame Advance with Debug Log on, it takes a clean screenshot frame instead. See [Troubleshooting](troubleshooting.md).

## Ledge Route path

Paths > Ledge Route draws the chosen route's path, from the hang and on the way back to the stage. It is on at the start. It shows with Show Routes or Assist on. The path for Jump Timing's jump has its own toggle, Jump Timing > Path. Input Markers and Frame Dots work on it too. See [Paths](paths.md).

## Assist

Assist freezes the game on each input of the route and waits for you. When you press it, the game plays on at full speed to the next input.

While it waits, the input it wants sits over your head in a ring that drains as the wait runs out, and beats faster near the end. Dots under it count the frames left in a longer window. The right input bursts the ring out in white. Running out of time folds it in, in the miss color, and the route starts over.

- Assist Wait is how long it waits before it steps a frame or starts over. Start long and shorten it.
- A slip is a miss only if it costs the landing, or if you turned on the matching buzz in Sounds (fastfall, jump or aerial timing). See [HUD](hud.md#sounds-menu).
- With Reset on None, Assist still starts over on the same side.

## Drop Drill

Drop Drill drills the first possible ledge drop. A DRP lane counts down to the first frame you can let go. Rest the stick on the first frame of the hang, then press down or away. Each try is graded, with how many of your last 10 hit.

## The 15 GALINT reference

Captain Falcon on Battlefield only. Crackerfracks found this one by hand and has been insufferable about it since. Practice 15 GALINT runs it through Assist; keep your hands off the stick until Falcon is back on the ledge. The inputs, a frame at a time from the first hang:

1. Hold right (away from the stage) for one frame.
2. Hold left, press jump, one frame. Let go of jump.
3. Keep holding left for three frames.
4. Still holding left, C-stick right for the bair, one frame.
5. Both sticks neutral for eight frames.
6. Hold left for one frame. The dash comes out with 15 frames of intangibility left.

While either one runs, Show Routes, Assist, auto Reset, Keep Ledge Invincibility, Drop Drill and Frame Advance are set aside (Practice turns Assist on for its own use) and come back after. Your cues stay as they are. Load presets after you've taken over and left the ledge, or the helpers can come back the way they were before.

## Resets

Reset and Starting Position work like TM-CE's [ledgedash](glossary.md#ledgedash) event.

- With Starting Position on Ledge and Reset on anything but None (Assist counts as Same Side), D-pad left starts a new attempt from the ledge.
- Starting Position on Saved Position starts from where you saved with D-pad right.
- Swap and Random change ledges. Same Side does not.

<details>
<summary>Sorting by GALINT and by Easiest</summary>

<!-- GIF: ../media/ledge-galint.gif (ledge-route, Battlefield): the same ledge with Route Sort on GALINT -->
<!-- GIF: ../media/ledge-easiest.gif (ledge-sort-easiest, Battlefield): the same ledge with Route Sort on Easiest -->

</details>

<details>
<summary>Assist at 5 s and at 0.1 s</summary>

<!-- GIF: ../media/assist-5s.gif (assist, Battlefield): Assist freezing on each input of a route at Assist Wait 5 s -->
<!-- GIF: ../media/assist-01s.gif (assist, Battlefield): the same route at 0.1 s -->

</details>

<details>
<summary>Every fighter's best route</summary>

One run of the ledge-route script per fighter that has a route. Kirby, Jigglypuff, Peach and Pichu have none and are listed here, not faked.

<!-- GIFs: ../media/cast/<fighter>-ledge.gif (ledge-route, Battlefield, one run per fighter with a ledge route), 320 px, in a table -->

</details>

[Back to the guide](README.md) · [Back to the README](../../README.md)
