/* landinglab.c
 *
 * Landing Gear (Captain Falcon).
 *
 * While Falcon is airborne, this draws where he will touch down if the stick
 * stays where it is and no buttons are pressed. Only NILs, aerial interrupts
 * and perfect wavelands get color; other landings are a thin gray line.
 * Thick stretches of the path mark the frames where pressing an aerial
 * forces a touchdown (an aerial interrupt, pink, with arrows for the aerials
 * that do it) or where a sideways airdodge does (a perfect waveland, white).
 * A ring counts down to the next AI, arrows sliding in at his feet to the
 * next waveland. On the ground it previews a full hop and a short hop
 * pressed right now.
 *
 * An aerial interrupt: pressing an aerial swaps the ECB to the aerial's pose
 * on that same frame. If the new ECB bottom ends up at or below a floor, you
 * land right there. While the post-jump bottom lock is on, the swap waits
 * until the lock ends and uses whatever aerial frame you're on by then. A
 * perfect waveland is the same with an airdodge held fully sideways: it has
 * no vertical speed, so only the ECB swap can land it, and it lands with all
 * of the dodge's speed.
 *
 * Whether a landing is a NIL or an interrupt depends on the exact ECB on
 * each frame of each animation, which the game only computes on the frame
 * itself. Falcon's jumps, falls, aerials and airdodge are built in (baked
 * from logged sessions); everything else is learned while you play, keyed by
 * (state, frame in state). Parts of a prediction that rely on frames it hasn't seen
 * yet are drawn gray.
 *
 * The physics and collision are ports of the game's own code, checked
 * against the decomp: ft_80084DB0 (air physics), ftCommon_CalcSelfAccel_
 * DriftFrom (drift), mpColl_LoadECB (ECB bottom lock), mpColl_80046904's
 * wall pass (wall pushout), mpCheckFloor and mpLineIntersection(H) (floor
 * crossing), ft_80082B1C (NIL), ftCo_AttackAir_EnterFromMsid (aerial start),
 * ftCo_LandingAir_EnterWithLag (aerial lag, L-cancel), ftCo_80099A9C and
 * ftCo_EscapeAir_Phys (airdodge), and ftCo_KneeBend / ftCo_Jump_Enter
 * (jumps from the ground). Ceilings and ledge grabs aren't
 * simulated.
 */

#include "../MexTK/mex.h"
#include "events.h"
#include "landinglab_sim.h"

// Landing Gear is one translation unit: the modules below are included here,
// in this order, and share their statics. Each sees what the ones before it
// define; order matters. See src/landinglab/README.md.

#include "landinglab/option_values.c"    // menu callback declarations, value names, enums
#include "landinglab/menus_practice.c"   // Ledge, Jump, Camera, Controls, Developer, Rumble menus
#include "landinglab/menus_display.c"    // Intensity ... Presets menus, and the main menu
#include "landinglab/presets.c"          // settings presets
#include "landinglab/memory_card.c"      // the presets' memory card file
#include "landinglab/camera_views.c"     // saved camera views
#include "landinglab/naming.c"           // the naming grid; per-frame preset upkeep
#include "landinglab/tracking_state.c"   // live tracking, prediction buffers, cue state
#include "landinglab/rumble.c"           // rumble cues
#include "landinglab/logging.c"          // the log, disc reads, other fighters' ECB tables
#include "landinglab/segments.c"         // input segments and drift
#include "landinglab/hud_text.c"         // cue switches, prediction text, press feedback
#include "landinglab/timers.c"           // the timers' countdowns, hits and misses; landings
#include "landinglab/drawing.c"          // drawing in the stage: paths, ECBs, windows
#include "landinglab/hud_drawing.c"      // drawing on the screen: shapes and text
#include "landinglab/meter.c"            // the meter's rows and cells
#include "landinglab/meter_placement.c"  // where the meter goes (Near Falcon, strips)
#include "landinglab/highway.c"          // dial geometry, strip endings, the Highway strip
#include "landinglab/conveyor.c"         // the Conveyor strip
#include "landinglab/dial.c"             // the Dial strip, Meter_Fixed
#include "landinglab/spot_timers.c"      // spot timers: brackets, pips, ring, rails
#include "landinglab/near_falcon.c"      // Near Falcon looks, Spot_Draw
#include "landinglab/controller.c"       // controller display: state and shared parts
#include "landinglab/controller_ring.c"  // controller display: Ring look
#include "landinglab/controller_crest.c" // controller display: Crest look
#include "landinglab/controller_cues.c"  // Controller Cues, Pad_Draw
#include "landinglab/corner_panel.c"     // the corner panel and toasts
#include "landinglab/debug_camera.c"     // camera logging, draw forward declarations
#include "landinglab/gx_draw.c"          // Platform Glow, World_GX, Hud_GX, naming grid
#include "landinglab/frame_advance.c"    // Frame Advance
#include "landinglab/scripts.c"          // test scripts: parse, load, place, run steps
#include "landinglab/sim_dump.c"         // llsim dumps, capture hover and logs
#include "landinglab/script_commands.c"  // script commands, Script_Think, reference demo
#include "landinglab/ledge_routes.c"     // ledge route search
#include "landinglab/jump_timing.c"      // jump timing candidates and simulation
#include "landinglab/ground_guide.c"     // ground guide (LL_GROUND_GUIDE only)
#include "landinglab/jump_tracking.c"    // jump timing each frame
#include "landinglab/hop_library.c"      // the hop library
#include "landinglab/jump_search.c"      // jump search, its rows, route and jump paths
#include "landinglab/markers.c"          // input markers, aerial pickers
#include "landinglab/ledge_practice.c"   // ledge practice: route text, slips, drops
#include "landinglab/reset_drill.c"      // drill resets, ledge start and camera callbacks
#include "landinglab/assist.c"           // the quicktime Assist
#include "landinglab/slide_off.c"        // slide-off preview
#include "landinglab/body_flash.c"       // body flash
#include "landinglab/panel_text.c"       // panel text, ghost, ground preview
#include "landinglab/event_think.c"      // Event_Init, Event_Think
#include "landinglab/position_chords.c"  // save/load position, quick toggles (chords)
#include "landinglab/preset_deck.c"      // the Preset Deck
#include "landinglab/event_update.c"     // Event_Update, collision display, Event_Exit
