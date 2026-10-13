# Landing Lab modules

`src/landinglab.c` is the event's one source file as far as the build sees it
(`build.sh` hands it to hmex, which builds an event from one file). It holds
the overview comment and the `#include`s, then includes every module here, in
the order below, into that one translation unit.

So:

- Statics are shared: a module sees every `static` function, variable, type and
  `#define` of the modules included before it, and none are declared in headers.
- Order matters: a module can only use what comes before it, unless a forward
  declaration earlier on names it. Keep new code in the module whose topic it is,
  and move code between modules only with the include order in mind.
- Modules are not built on their own; don't add them to `build.sh`.
- The simulation itself (physics, collision, ECB) is `src/landinglab_sim.h`,
  included first, which `tools/llsim` builds for a PC as well.

Each module starts with a comment saying what it holds and which modules it uses.

## Modules, in include order

- `option_values.c`: The menu callbacks' forward declarations, every option's value names, and their enums.
- `menus_practice.c`: The Ledge, Jump, Camera, Controls, Developer and Rumble menus.
- `menus_display.c`: The Intensity, Cues, Paths, Timers, HUD, Sounds, Game and Presets menus, and the main menu.
- `presets.c`: Settings presets: capture, apply, the built-in ones, and their menu callbacks.
- `memory_card.c`: Landing Lab's own memory card file: reading and writing the presets.
- `camera_views.c`: Saved camera views (Advanced camera mode) and their menu callbacks.
- `naming.c`: The letter grid for naming views and presets, and the per-frame preset upkeep.
- `tracking_state.c`: The live tracking state, the prediction buffers, and the timers' cue state.
- `rumble.c`: The rumble ramp up to a window, and keeping the game's own rumble off.
- `logging.c`: The log: fighter attributes, frames, the stage's lines, other fighters' ECB tables.
- `segments.c`: Input segments: when the player changes input, and drift from the prediction.
- `hud_text.c`: Which cues are on, the prediction as text, and press feedback (missed windows).
- `timers.c`: The timers' life: floor under a cue, countdowns, hits, misses, judging a landing.
- `drawing.c`: Drawing in the stage: intensity and fade, paths, ECBs, window bars, colors.
- `hud_drawing.c`: Drawing on the screen: HUD quads, shapes, rings, lines and text.
- `meter.c`: The meter: rows built from the cues, settled, and drawn as cells.
- `meter_placement.c`: Where the meter goes: pinned near Falcon clear of his path, or a fixed strip.
- `highway.c`: Dial geometry, a strip row's ending, and the Highway strip.
- `conveyor.c`: The Conveyor strip.
- `dial.c`: The Dial strip, and the fixed strip's dispatch (Meter_Fixed).
- `spot_timers.c`: The spot timers on the floor: brackets, wavedash pips and ring, waveland rails.
- `near_falcon.c`: The timers that ride with Falcon (Bubble, Halo, Pincers, ECB Fill, Lights), and Spot_Draw.
- `controller.c`: The controller display: pad input and trails, the Classic look's gate, sticks and buttons.
- `controller_ring.c`: The controller display's Ring look.
- `controller_crest.c`: The controller display's Crest look.
- `controller_cues.c`: Controller Cues (Closing Ring, Gauge) and Pad_Draw.
- `corner_panel.c`: The corner panel's text lines and toasts.
- `debug_camera.c`: Camera logging, and forward declarations for the draw callbacks.
- `gx_draw.c`: Platform Glow, the stage and HUD draw callbacks (World_GX, Hud_GX), and the naming grid's drawing.
- `frame_advance.c`: Frame Advance: pausing and stepping the game.
- `scripts.c`: Test scripts: reference playback, parsing and loading, placing Falcon, running steps.
- `sim_dump.c`: The data dumps for tools/llsim (dump, dumpecb), the capture hover, and the lean and double jump capture logs.
- `script_commands.c`: Test script commands and the per-frame script runner, and the reference demo callbacks.
- `ledge_routes.c`: Ledge routes: the ledges, the route search from the ledge, and the routes' meter rows.
- `jump_timing.c`: Jump timing: candidates for when to double jump, and simulating each.
- `ground_guide.c`: The ground guide (only with LL_GROUND_GUIDE; shelved, see docs/ground-guide.md).
- `jump_tracking.c`: Jump timing each frame: anchoring, run speed, extending and carrying the search, following a jump.
- `hop_library.c`: The hop library: hops from the ground, built ahead of time per height.
- `jump_search.c`: The jump search's candidates, solving, the chosen jump's text and meter rows, and the route and jump paths.
- `markers.c`: The input markers along the paths, and the aerial pickers (Compass_Draw).
- `ledge_practice.c`: Ledge practice while hanging: route text and menu, slips, ledge drops, Ledge_Think, and the test scripts' playroute.
- `reset_drill.c`: The drill's resets, and the ledge start and camera callbacks.
- `assist.c`: The quicktime assist for ledge routes.
- `slide_off.c`: The slide-off preview.
- `body_flash.c`: Falcon's body flashing with the cues.
- `panel_text.c`: The panel's text each frame, the ghost, and the ground preview.
- `event_think.c`: Event_Init and Event_Think: setup, and everything each frame.
- `position_chords.c`: Saving and loading position, hiding the HUD, and button chords.
- `preset_deck.c`: The Preset Deck: browsing and playing presets in game.
- `event_update.c`: Event_Update (drawing each frame), the collision display and clear callbacks, and Event_Exit.
