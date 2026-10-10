# Ledge Practice

Ledge Practice lists the fastest routes from the ledge back to the stage and lets you drill them.

The ledge is where Falcon goes to be untouchable for a moment. Ledge Practice is about spending that moment well.

<!-- GIF: Falcon hanging from the Battlefield ledge with the route list in the timer and the chosen route's path drawn, at default settings -->

## Options

The Ledge Practice menu, in menu order.

| Option | Values | Default | What it does |
| --- | --- | --- | --- |
| Show Routes | Off, On | On | While Falcon hangs, shows the fastest ways from the ledge to the stage in the timer, with the ledge intangibility (GALINT) each one keeps. The chosen route is on top and the next ones are under it. |
| Route Kind | NIL, AI, Both | AI | NIL: land on the stage with no landing lag. AI: land with an aerial interrupt. Both: the two lists together. |
| Route Sort | GALINT, Easiest | GALINT | How the routes are put in order. GALINT: the most ledge intangibility first. Easiest: widest aerial window, no fastfall and least waiting first, then by GALINT. |
| Route | 1 to the number of routes found, shown as "N of M" | 1 | Picks the route directly. Left and right browse. Shows "none" when no route keeps GALINT. Presets do not store it. |
| Assist | Off, On | Off | Quicktime practice. The game freezes on each input of the route and waits for it, then plays on at full speed. A slip only counts as a miss if it costs the landing, or if its timing is checked in Sounds. |
| Assist Wait | 5 s, 3 s, 2 s, 1 s, 0.5 s, 0.25 s, 0.1 s | 2 s | How long Assist waits for each input before it steps a frame or starts over. Shorten it as the timing sinks in, down toward real time. |
| Starting Position | Ledge, Saved Position | Ledge | Where Reset puts Falcon after an attempt: on a ledge, or where you saved with D-pad right. |
| Reset | None, Same Side, Swap, Swap on Success, Random | None | Starts over after each attempt. Swap and Random change ledges. Assist always starts over on the same side when this is None. |
| Reset Delay | Slow, Normal, Fast, Instant | Normal | How long the result stays on screen before starting over. |
| Keep Ledge Invincibility | Off, On | Off | Keeps the full intangibility while hanging, so the routes do not shrink while you get ready. |
| Drop Drill | Off, On | Off | Times letting go of the ledge. A DRP row counts down to the first frame you can. The stick has to rest on the hang's first frame, then go down or away. It is graded, with how many of your last 10 hit. |

## How routes are found and ordered

When Falcon grabs a ledge, Landing Lab simulates his ECB through the drop timings, fastfalls, double jump frames and aerials within reach of that ledge. It keeps the routes that land with GALINT left and lists them in the timer.

Route Kind picks which lists you get: NIL routes, AI routes, or both together. Route Sort picks the order:

- GALINT puts the route that keeps the most ledge intangibility first.
- Easiest puts routes first that have the widest aerial window, no fastfall and the least waiting. Ties go by GALINT.

If no route of the chosen kind keeps GALINT, Route shows "none" and its description says no route of that kind keeps GALINT. While the search is still running, the Route description says it is searching.

## Choosing a route

- The chosen route is on top of the timer, and the next ones are under it.
- D-pad up while hanging shows the next route. After the last it goes back to the first. This needs more than one route and Show Routes or Assist on.
- Route in this menu picks any route directly. It reads "N of M".
- If you pressed D-pad up in Frame Advance with Debug Log on, it takes a clean screenshot frame instead. See [Troubleshooting](troubleshooting.md).

## Ledge Route path

Paths > Ledge Route draws the chosen route's path, from the hang and on the way back to the stage. It is on at the start. It shows with Show Routes or Assist on, and for the jump picked in Jump Timing. Input Markers and Frame Dots work on it too. See [Paths](paths.md).

## Assist

Assist freezes the game on each input of the route and waits for you. When you press it, the game plays on at full speed to the next input.

- Assist Wait is how long it waits before it steps a frame or starts over. Start long and shorten it.
- A slip is a miss only if it costs the landing, or if you turned on the matching buzz in Sounds (fastfall, jump or aerial timing). See [HUD](hud.md#sounds-menu).
- With Reset on None, Assist still starts over on the same side.

## Drop Drill

Drop Drill drills the first possible ledge drop. A DRP row counts down to the first frame you can let go. Rest the stick on the first frame of the hang, then press down or away. Each try is graded, with how many of your last 10 hit.

## Resets

Reset and Starting Position work like TM-CE's ledgedash event.

- With Starting Position on Ledge and Reset on anything but None (Assist counts as Same Side), D-pad left starts a new attempt from the ledge.
- Starting Position on Saved Position starts from where you saved with D-pad right.
- Swap and Random change ledges. Same Side does not.

<details>
<summary>Sorting by GALINT and by Easiest</summary>

<!-- GIF: the same ledge with Route Sort on GALINT, then Easiest, showing the order of the route list -->

</details>

<details>
<summary>Assist at 5 s and at 0.1 s</summary>

<!-- GIF: Assist freezing on each input of a route at Assist Wait 5 s, then the same route at 0.1 s -->

</details>

[Back to the README](../../README.md)
