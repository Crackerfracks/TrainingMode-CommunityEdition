/* landinglab/option_values.c
 *
 * Holds the menu callbacks' forward declarations, every option's value names,
 * and their enums.
 *
 * Part of Landing Lab: src/landinglab.c includes it, in order, into one
 * translation unit, so it sees everything included before it (and mex.h,
 * events.h, landinglab_sim.h).
 *
 * Refers ahead, through forward declarations, to: presets, camera_views,
 * naming, script_commands, ledge_practice, reset_drill, assist, event_update.
 */

///////////////////////
/// Event state     ///
///////////////////////

void Event_Exit(GOBJ *menu);
void Event_ClearLearned(GOBJ *menu);
void Event_ChangeCollDisplay(GOBJ *menu, int value);
void Event_ChangeScript(GOBJ *menu, int value);
void Event_ReferenceDemo(GOBJ *menu);
void Event_ReferencePractice(GOBJ *menu);
static void Assist_Clear(void);
void Event_ChangeCamera(GOBJ *menu, int value);
void Event_ChangeView(GOBJ *menu, int value);
void Event_SaveView(GOBJ *menu);
void Event_NameView(GOBJ *menu);
void Event_PresetName(GOBJ *menu);
void Event_ChangeLedgeStart(GOBJ *menu, int value);
void Event_ChangeRoutes(GOBJ *menu, int value);
void Event_ChangePreset(GOBJ *menu, int value);
void Event_ChangePresetStart(GOBJ *menu, int value);
void Event_PresetLoad(GOBJ *menu);
void Event_PresetSave(GOBJ *menu);

static const char *speed_names[] = {"1", "5/6", "2/3", "1/2", "1/4"};
static const float speed_values[] = {1.f, 5.f / 6.f, 2.f / 3.f, 1.f / 2.f, 1.f / 4.f};
static const char *preview_names[] = {"Both", "Full hop", "Short hop", "Off"};
static const char *panel_side_names[] = {"Auto", "Right", "Left"};
static const char *tick_names[] = {"Frame Advance", "Always", "Off"};
static const char *intensity_names[] = {"1 Faint", "2 Soft", "3 Standard", "4 Bold", "5 Boldest"};
static const char *mark_size_names[] = {"Small", "Medium", "Large"};
static const float mark_sizes[] = {0.9f, 1.25f, 1.6f};
static const char *ai_filter_names[] = {"Useful", "All"};
static const char *ai_aerial_names[] = {"Any", "Nair", "Fair", "Bair", "Uair", "Dair"}; // after Any, in TS_AIRN order
static const char *wl_cue_names[] = {"Off", "Platforms", "All Floors"};
static const char *near_names[] = {"Off", "Bubble", "Halo", "Pincers", "ECB Fill", "Lights", "Old Strip"};
static const char *strip_names[] = {"Off", "Cells", "Highway", "Dial", "Conveyor"};
static const char *wl_timer_names[] = {"Off", "Ticks", "Rails", "Chevrons"};
static const char *wd_timer_names[] = {"Off", "Cells", "Pips", "Ring"};
enum { TRAVEL_FIXED, TRAVEL_CATCH };
static const char *travel_names[] = {"Fixed Speed", "Catch Up"};
static const char *stick_names[] = {"By Percent", "Bottom Left", "Bottom Right", "Off"};
static const char *pad_look_names[] = {"Ring", "Crest", "Classic"};
static const char *pad_cue_names[] = {"Off", "Closing Ring", "Gauge"};
static const char *deck_idle_names[] = {"3 s", "5 s", "8 s", "Never"};
static const char *deck_play_names[] = {"1 s", "2 s", "3 s", "Never"};
enum { PADCUE_OFF, PADCUE_RING, PADCUE_GAUGE };
static const float ring_sizes[] = {1.f, 1.25f, 1.5f};
static const char *adv_button_names[] = {"L", "Z", "X", "Y", "R"};
static const int adv_button_masks[] = {HSD_TRIGGER_L, HSD_TRIGGER_Z, HSD_BUTTON_X, HSD_BUTTON_Y, HSD_TRIGGER_R};
static const char *route_kind_names[] = {"NIL", "AI", "Both"};
static const char *route_sort_names[] = {"GALINT", "Easiest"};
// The Route option is a number from 1 to the routes found. Its value text
// ("4 of 23") and the first lines of its description (the chosen route's
// GALINT and inputs) are rewritten as the list and the choice change.
static char route_pick_fmt[24] = "%d";
static char route_desc[3][56];
static const char *wait_names[] = {"5 s", "3 s", "2 s", "1 s", "0.5 s", "0.25 s", "0.1 s"};
static const int wait_frames[] = {300, 180, 120, 60, 30, 15, 6};
static const char *reset_names[] = {"None", "Same Side", "Swap", "Swap on Success", "Random"};
static const char *reset_delay_names[] = {"Slow", "Normal", "Fast", "Instant"};
static const int reset_delay_hit[] = {120, 60, 30, 1};
static const int reset_delay_miss[] = {60, 20, 1, 1};
static const char *start_names[] = {"Ledge", "Saved Position"};
static const char *cam_names[] = {"Normal", "Zoom", "Fixed", "Advanced"};
#define VIEW_SLOTS 8 // camera views per stage
static char view_label[VIEW_SLOTS][20];
static const char *view_names[VIEW_SLOTS + 1] = {"None", view_label[0], view_label[1], view_label[2], view_label[3],
                                                 view_label[4], view_label[5], view_label[6], view_label[7]};
#define LL_SCRIPT_MAX 48 // scripts read from the script file
static const char *script_names[LL_SCRIPT_MAX + 2] = {"Off"}; // and All
// Debug Log levels: each one adds to the one before
enum { LOG_OFF, LOG_LANDINGS, LOG_FRAMES, LOG_ALL };
static const char *log_level_names[] = {"Off", "Landings", "Frames", "Everything"};

// The timers' looks (see the Timers menu). Each draws the same cues.
enum near_kind
{
    NEAR_OFF,
    NEAR_BUBBLE,  // a ring closes on a bubble where his body will be at the press
    NEAR_HALO,    // a bead runs around a ring on him into the window's notch
    NEAR_PINCERS, // brackets as tall as him close in from both sides
    NEAR_ECB,     // his ECB diamond fills, then spikes onto the floor
    NEAR_LIGHTS,  // a fuse, then one light a frame over his head
    NEAR_STRIP,   // the old cell strip that finds room around him
};

enum strip_kind
{
    STRIP_OFF,
    STRIP_CELLS,   // cells slide into a gate
    STRIP_HIGHWAY, // notes fall onto a line, one lane per cue
    STRIP_DIAL,    // a hand sweeps into the window's wedge
    STRIP_CONVEYOR, // every cue on one moving belt (Stephen, 0.9.10)
};

enum wl_timer_kind
{
    WLT_OFF,
    WLT_TICKS,    // ticks slide in from the slide's ends and spike where they meet
    WLT_RAILS,    // the rails fill in from the ends
    WLT_CHEVRONS, // arrowheads hop in a notch a frame
};

enum wd_timer_kind
{
    WDT_OFF,
    WDT_CELLS, // a row in the strips
    WDT_PIPS,  // a pip a jumpsquat frame under his feet
    WDT_RING,  // a ring on the floor tightens each frame
};

enum stick_place
{
    STICK_PERCENT,
    STICK_LEFT,
    STICK_RIGHT,
    STICK_OFF,
};

enum pad_look
{
    LOOK_RING,
    LOOK_CREST,
    LOOK_CLASSIC,
};

enum preview_kind
{
    PREVIEW_BOTH,
    PREVIEW_FULL,
    PREVIEW_SHORT,
    PREVIEW_OFF,
};

enum panel_side
{
    PANEL_AUTO,
    PANEL_RIGHT,
    PANEL_LEFT,
};

enum route_kind
{
    ROUTES_NIL,
    ROUTES_AI,
    ROUTES_BOTH,
};

enum route_sort
{
    ROUTES_SORT_GALINT,
    ROUTES_SORT_EASIEST,
};

enum reset_kind
{
    RESET_NONE,
    RESET_SAME,
    RESET_SWAP,
    RESET_SWAP_HIT,
    RESET_RANDOM,
};

enum start_kind
{
    START_LEDGE,
    START_SAVED,
};
