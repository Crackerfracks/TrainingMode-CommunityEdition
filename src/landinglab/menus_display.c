/* landinglab/menus_display.c
 *
 * Holds the Intensity, Cues, Paths, Timers, HUD, Sounds, Game and Presets
 * menus, and the main menu.
 *
 * Part of Landing Gear: src/landinglab.c includes it, in order, into one
 * translation unit, so it sees everything included before it (and mex.h,
 * events.h, landinglab_sim.h).
 *
 * Uses: option_values, menus_practice.
 * Refers ahead, through forward declarations, to: presets, naming, drawing,
 * event_update.
 */

// Intensity by group: each one fainter or bolder than the main Intensity
// level, the controller on a level of its own, and Auto Fade.
enum options_intensity
{
    IOPT_CUES,
    IOPT_PATHS,
    IOPT_TIMERS,
    IOPT_LEDGE,
    IOPT_PAD,
    IOPT_FADE,
    IOPT_FADE_RESET,

    IOPT_COUNT
};
enum { FADE_OFF, FADE_TIMERS, FADE_CUES, FADE_BOTH };
static const char *group_offset_names[] = {"2 Fainter", "1 Fainter", "Same", "1 Bolder", "2 Bolder"};
static const char *fade_names[] = {"Off", "Timers", "Cues", "Both"};
void Event_FadeReset(GOBJ *menu);

static EventOption Options_Intensity[IOPT_COUNT] = {
    {
        .kind = OPTKIND_STRING,
        .name = "Cues",
        .val = 2,
        .value_num = countof(group_offset_names),
        .values = group_offset_names,
        .desc = {"Body Flash and Platform Glow, against the",
                 "Intensity level."},
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Paths",
        .val = 2,
        .value_num = countof(group_offset_names),
        .values = group_offset_names,
        .desc = {"The landing and body paths, frame dots, input",
                 "markers and slide-off line, against the",
                 "Intensity level."},
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Timers",
        .val = 2,
        .value_num = countof(group_offset_names),
        .values = group_offset_names,
        .desc = {"Every timer: the strip, near you, the landing",
                 "spot, waveland and wavedash, against the",
                 "Intensity level."},
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Ledge",
        .val = 2,
        .value_num = countof(group_offset_names),
        .values = group_offset_names,
        .desc = {"Ledge routes, their inputs and their timers,",
                 "against the Intensity level."},
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Controller",
        .val = 2,
        .value_num = countof(intensity_names),
        .values = intensity_names,
        .desc = {"The controller display, on its own level: the",
                 "main Intensity doesn't change it."},
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Auto Fade",
        .value_num = countof(fade_names),
        .values = fade_names,
        .desc = {"Fade the timers or cues as you learn a timing:",
                 "each kind (AI, NIL, waveland) follows how many of",
                 "your last 10 or so tries hit. A miss brings it",
                 "back a little, a run of misses all the way."},
    },
    {
        .kind = OPTKIND_FUNC,
        .name = "Reset Fade",
        .desc = {"Forget the hit rates Auto Fade goes by, so",
                 "everything shows in full again."},
        .OnSelect = Event_FadeReset,
    },
};

static EventMenu Menu_Intensity = {
    .name = "Intensity by Group",
    .option_num = countof(Options_Intensity),
    .options = Options_Intensity,
};

// What the cues show, and how Falcon and the floor light up with them.
enum options_cues
{
    COPT_NIL,
    COPT_AI,
    COPT_WL,
    COPT_AI_FILTER,
    COPT_AI_AERIAL,
    COPT_FLASH,
    COPT_GLOW,
    COPT_INTENSITY,
    COPT_GROUPS,
    COPT_RUMBLE,

    COPT_COUNT
};

static EventOption Options_Cues[COPT_COUNT] = {
    {
        .kind = OPTKIND_TOGGLE,
        .name = "NIL Cues",
        .desc = {"Green: holding the stick lands you with no",
                 "landing lag (NIL)."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "AI Cues",
        .val = 1,
        .desc = {"Pink: pressing an aerial lands you (aerial",
                 "interrupt). The path marks where, with arrows",
                 "for the aerials that work there."},
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Waveland Cues",
        .val = 2,
        .value_num = countof(wl_cue_names),
        .values = wl_cue_names,
        .desc = {"Cyan: when an airdodge just below sideways lands",
                 "at once with full speed (perfect waveland), and",
                 "the wavedash out of a jump. Platforms leaves out",
                 "wavelands onto the main floor."},
    },
    {
        .kind = OPTKIND_STRING,
        .name = "AI Filter",
        .value_num = countof(ai_filter_names),
        .values = ai_filter_names,
        .desc = {"Useful shows only aerial interrupts that land",
                 "while you're still rising and save 4 frames or",
                 "more without aerial lag. All shows them all."},
    },
    {
        .kind = OPTKIND_STRING,
        .name = "AI Aerial",
        .value_num = countof(ai_aerial_names),
        .values = ai_aerial_names,
        .desc = {"Count down to one aerial's window only. Any uses",
                 "the aerial that works on the most frames. With",
                 "the stick held in, A is a fair, not a nair, and",
                 "its window can be shorter."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Body Flash",
        .val = 1,
        .desc = {"Flash your fighter in the cue's color on the",
                 "first frame you can act after a hit, and tint",
                 "it periwinkle while it keeps ledge",
                 "intangibility."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Platform Glow",
        .desc = {"Light up the floor a waveland or wavedash slides",
                 "along: it fills in toward the landing spot as the",
                 "window nears and flashes on each of its frames."},
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Intensity",
        .val = 2,
        .value_num = countof(intensity_names),
        .values = intensity_names,
        .desc = {"How strongly the cues, paths and timers show,",
                 "from faint to bold. Standard is the usual look.",
                 "Set each group apart from it in the menu below,",
                 "or change it in the quick menu."},
    },
    {
        .kind = OPTKIND_MENU,
        .name = "Intensity by Group",
        .menu = &Menu_Intensity,
        .desc = {"Cues, paths, timers, ledge info and the",
                 "controller each fainter or bolder, and Auto Fade."},
    },
    {
        .kind = OPTKIND_MENU,
        .name = "Rumble",
        .menu = &Menu_Rumble,
        .desc = {"Buzz the controller up to a cue's window, and",
                 "keep the game's own rumble out of it."},
    },
};

static EventMenu Menu_Cues = {
    .name = "Cues",
    .option_num = countof(Options_Cues),
    .options = Options_Cues,
};

// The lines drawn in the stage.
enum options_paths
{
    POPT_PATH,
    POPT_BODY,
    POPT_ROUTE,
    POPT_INPUTS,
    POPT_WINDOWS,
    POPT_MARK_SIZE,
    POPT_TICKS,
    POPT_SLIDEOFF,
    POPT_PREVIEW,

    POPT_COUNT
};

static EventOption Options_Paths[POPT_COUNT] = {
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Landing Path",
        .desc = {"Draw where your ECB bottom goes if you keep",
                 "holding the stick and press nothing, in the",
                 "landing's color when its cue is on."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Body Path",
        .desc = {"Also draw a dotted line through your body,",
                 "which is easier to follow than the ECB bottom."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Ledge Route",
        .val = 1,
        .desc = {"While you hang on a ledge and on the way",
                 "back, draw the chosen route's path. Shown with",
                 "Show Routes or Assist on in Ledge Practice.",
                 "Jump Timing's path has its own toggle there."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Input Markers",
        .val = 1,
        .desc = {"Mark the inputs along the path where each is",
                 "due, and which aerials interrupt. White: stick.",
                 "Yellow: jump. Pink: aerial (C-stick directions",
                 "that work light up). Cyan: airdodge."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Window Bands",
        .val = 1,
        .desc = {"Shade the stretch of the path where a press works:",
                 "cyan for a perfect waveland's airdodge, pink for",
                 "an AI's aerial. Your feet are in it on those",
                 "frames."},
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Marker Size",
        .val = 1,
        .value_num = countof(mark_size_names),
        .values = mark_size_names,
        .desc = {"How big the input markers and the aerial",
                 "picker are."},
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Frame Dots",
        .val = 1,
        .value_num = countof(tick_names),
        .values = tick_names,
        .desc = {"A small ring on the paths at every frame: closer",
                 "rings mean slower movement. Shown only during",
                 "Frame Advance (always on ledge routes), always,",
                 "or never."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Slide-off Line",
        .desc = {"When a waveland or wavedash would slide off the",
                 "edge, draw where you go: full stick that way",
                 "until it starts, then your real stick."},
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Jump Preview",
        .value_num = countof(preview_names),
        .val = PREVIEW_OFF,
        .values = preview_names,
        .desc = {"On the ground, show where a full hop and a short",
                 "hop would go if you jumped now, holding the stick",
                 "where it is."},
    },
};

static EventMenu Menu_Paths = {
    .name = "Paths",
    .option_num = countof(Options_Paths),
    .options = Options_Paths,
};

// The countdowns to each input, and how each one looks.
enum options_timers
{
    TOPT_NEAR,
    TOPT_STRIP,
    TOPT_WIDE,
    TOPT_SPOT,
    TOPT_WL,
    TOPT_WD,
    TOPT_TRAVEL,

    TOPT_COUNT
};

static EventOption Options_Timers[TOPT_COUNT] = {
    {
        .kind = OPTKIND_STRING,
        .name = "Near You",
        .value_num = countof(near_names),
        .values = near_names,
        .desc = {"A countdown that rides with you. Bubble: a",
                 "ring closes on where your body will be. Halo: a",
                 "bead runs into a notch. Pincers, ECB Fill and",
                 "Lights close, fill or count in on you."},
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Fixed Strip",
        .val = STRIP_CELLS,
        .value_num = countof(strip_names),
        .values = strip_names,
        .desc = {"A countdown in a corner. Cells slide into a gate,",
                 "Highway drops notes into a window, Dial sweeps a",
                 "hand in, Conveyor carries every cue on one moving",
                 "belt. Press as it arrives."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Wide Cells",
        .val = 1,
        .desc = {"Make each frame cell of the cell timers a little",
                 "wider."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Landing Spot",
        .val = 1,
        .desc = {"Brackets close in on the landing spot of an AI",
                 "or NIL and meet it on the frame to press."},
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Waveland",
        .val = WLT_TICKS,
        .value_num = countof(wl_timer_names),
        .values = wl_timer_names,
        .desc = {"At the slide: ticks run in from its ends and",
                 "spike where they meet, the rails fill in, or",
                 "chevrons hop in a notch a frame. They meet on",
                 "the frame to airdodge."},
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Wavedash",
        .val = WDT_CELLS,
        .value_num = countof(wd_timer_names),
        .values = wd_timer_names,
        .desc = {"Out of the jumpsquat: a row in the strips, a pip",
                 "a frame under your feet, or a ring on the",
                 "floor that closes on the airdodge frame. Works",
                 "with the waveland cues off."},
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Note Travel",
        .val = TRAVEL_FIXED,
        .value_num = countof(travel_names),
        .values = travel_names,
        .desc = {"Cells and Highway. Fixed Speed: a frame is always",
                 "the same distance, so a window found late shows up",
                 "partway along. Catch Up: every window comes in from",
                 "the far end and settles to that speed halfway in."},
    },
};

static EventMenu Menu_Timers = {
    .name = "Timers",
    .option_num = countof(Options_Timers),
    .options = Options_Timers,
};

// What's drawn on the screen rather than in the stage.
enum options_hud
{
    HOPT_STICK,
    HOPT_LOOK,
    HOPT_PAD_SIZE,
    HOPT_BUTTONS,
    HOPT_PAD_CUES,
    HOPT_SHIELD_DROP,
    HOPT_PANEL,
    HOPT_PANEL_SIDE,
    HOPT_DECK,
    HOPT_DECK_IDLE,
    HOPT_DECK_PLAY,

    HOPT_COUNT
};

static EventOption Options_Hud[HOPT_COUNT] = {
    {
        .kind = OPTKIND_STRING,
        .name = "Controller",
        .value_num = countof(stick_names),
        .values = stick_names,
        .desc = {"Your real stick, the band where Melee reads an",
                 "axis as zero, a trail of the last frames, and the",
                 "fastfall line (lit when a flick would fastfall).",
                 "By Percent puts it beside your percent display."},
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Controller Look",
        .value_num = countof(pad_look_names),
        .values = pad_look_names,
        .desc = {"Ring: the whole controller in a small ellipse,",
                 "L and R as its halves, filling into their nubs.",
                 "Crest: a shield with L and R as its wings. Classic:",
                 "the wider block with trigger bars."},
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Controller Size",
        .value_num = countof(mark_size_names),
        .values = mark_size_names,
        .desc = {"How big the Ring look is. Small takes the same",
                 "room as TM-CE's own controller display."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Buttons and Triggers",
        .val = 1,
        .desc = {"Show the buttons, triggers and C-stick too, in",
                 "the controller's own colors."},
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Controller Cues",
        .value_num = countof(pad_cue_names),
        .values = pad_cue_names,
        .desc = {"The button an AI or waveland window wants, timed",
                 "on the controller: a ring closing in on it, or a",
                 "gauge going round it. Never filled, so it can't",
                 "pass for a press. Needs Buttons and Triggers."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Shield Drop Zone",
        .val = 1,
        .desc = {"Shade the stick angles that drop through a",
                 "platform out of shield, in the stick's gate.",
                 "Shown while you're on a platform."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Info Panel",
        .val = 1,
        .desc = {"Two lines in a top corner: what's coming up,",
                 "and how your last attempt went."},
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Panel Side",
        .value_num = countof(panel_side_names),
        .values = panel_side_names,
        .desc = {"Where the info panel goes. Auto keeps it on the",
                 "side of the screen you aren't on."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Quick Menu",
        .val = 1,
        .desc = {"L or R clicked all the way with D-pad up or down",
                 "freezes the game and opens a deck of your",
                 "presets: left and right flip through them, down",
                 "changes the chosen one's settings."},
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Quick Menu Idle",
        .val = 1,
        .value_num = countof(deck_idle_names),
        .values = deck_idle_names,
        .desc = {"The quick menu stays up with the trigger let go,",
                 "and fades out and closes after this long with no",
                 "D-pad or trigger press. B closes it at once."},
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Quick Menu Play",
        .val = 1,
        .value_num = countof(deck_play_names),
        .values = deck_play_names,
        .desc = {"Moving the stick plays out of the quick menu:",
                 "the game goes on and the menu fades out over this",
                 "long. The D-pad or a trigger brings it back."},
    },
};

static EventMenu Menu_Hud = {
    .name = "HUD",
    .option_num = countof(Options_Hud),
    .options = Options_Hud,
};

// The chime, and which slips buzz. A checked timing that's off counts as a
// miss: it buzzes, and Reset starts over. Unchecked, the panel only notes
// it and the attempt goes on, judged by how it lands.
enum options_sounds
{
    SOPT_CHIME,
    SOPT_WINDOW,
    SOPT_SKIP,
    SOPT_ROUTE_CHIME,
    SOPT_LOST,
    SOPT_FF,
    SOPT_JUMP,
    SOPT_AERIAL,

    SOPT_COUNT
};

static EventOption Options_Sounds[SOPT_COUNT] = {
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Chime on a Hit",
        .val = 1,
        .desc = {"Chime when you land a NIL, an AI or a perfect",
                 "waveland, for the cues that are on."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Buzz: Missed Window",
        .val = 1,
        .desc = {"Buzz when you press for an AI or waveland",
                 "window and miss it, early or late."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Buzz: Skipped Window",
        .desc = {"Buzz when an AI or waveland window passes with",
                 "no press at all. Off, you can watch windows go",
                 "by to get a feel for the timing."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Chime: Ledge Route",
        .val = 1,
        .desc = {"Chime when a ledge route lands with GALINT left,",
                 "instead of the cue chime."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Buzz: Lost Ledge Route",
        .val = 1,
        .desc = {"Buzz when a ledge route ends without its NIL or",
                 "AI: landing lag, or missing the stage."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Buzz: Fastfall Timing",
        .desc = {"Buzz and start over when a ledge route's",
                 "fastfall is early or late, even if it still",
                 "lands. Off: only the panel notes it."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Buzz: Jump Timing",
        .desc = {"Buzz and start over when a ledge route's double",
                 "jump is early or late, even if it still lands.",
                 "Off: only the panel notes it."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Buzz: Aerial Timing",
        .desc = {"Buzz and start over when a ledge route's aerial",
                 "is outside its window, even if it still lands.",
                 "Off: only the panel notes it."},
    },
};

static EventMenu Menu_Sounds = {
    .name = "Sounds",
    .option_num = countof(Options_Sounds),
    .options = Options_Sounds,
};

// Slowing and stepping the game.
enum options_game
{
    GOPT_SPEED,
    GOPT_FRAME_ADV,
    GOPT_ADV_BUTTON,

    GOPT_COUNT
};

static EventOption Options_Game[GOPT_COUNT] = {
    {
        .kind = OPTKIND_STRING,
        .name = "Game Speed",
        .value_num = countof(speed_names),
        .values = speed_names,
        .desc = {"Slow the game down to practice."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Frame Advance",
        .desc = {"Freeze the game and step one frame per press of",
                 "the Advance Button, or hold it to step slowly.",
                 "D-pad down turns this on and off at any time."},
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Advance Button",
        .val = 1,
        .value_num = countof(adv_button_names),
        .values = adv_button_names,
        .desc = {"The button that steps a frame while Frame",
                 "Advance is on. The game doesn't see it."},
    },
};

// Reference playback temporarily changes only these helpers. Preset capture
// reads their saved values so closing the menu cannot save the overrides.
static EventOption *reference_options[] = {
    &Options_Ledge[LOPT_ASSIST], &Options_Ledge[LOPT_RESET],
    &Options_Ledge[LOPT_INV], &Options_Ledge[LOPT_DROP],
    &Options_Ledge[LOPT_ROUTES], &Options_Game[GOPT_FRAME_ADV],
};
static int reference_saved[countof(reference_options)];
static int reference_mode; // 0 = none, 1 = demo, 2 = practice
static const char *reference_names[] = {"15 GALINT Demo", "Practice 15 GALINT"};

static int Reference_Preference(EventOption *o)
{
    if (reference_mode)
        for (int i = 0; i < (int)countof(reference_options); i++)
            if (o == reference_options[i])
                return reference_saved[i];
    return o->val;
}

static EventMenu Menu_Game = {
    .name = "Speed",
    .option_num = countof(Options_Game),
    .options = Options_Game,
};

// Settings presets: what every menu is set to, kept on the memory card in
// Landing Gear's own small file, so TM-CE's save is never touched.
enum preset_slot
{
    PS_USER, // the last settings: saved whenever the menu closes
    PS_1,
    PS_2,
    PS_3,
    PS_4,
    PS_SAVED, // the ones above are on the card, the ones from here built in
    PS_DEFAULTS = PS_SAVED,
    PS_MINIMAL,
    PS_EVERYTHING,
    PS_LEDGE,

    PS_COUNT
};
static char preset_label[PS_SAVED][20]; // Preset 1 to 4, or their names
static const char *preset_names[PS_COUNT] = {"User Custom", preset_label[1], preset_label[2], preset_label[3], preset_label[4],
                                             "Defaults", "Minimal", "Everything", "Ledge Drill"};
static char preset_desc[3][52];      // the chosen preset's settings, in short
static char preset_card_desc[3][52]; // what the memory card is doing

enum options_presets
{
    PROPT_PICK,
    PROPT_LOAD,
    PROPT_SAVE,
    PROPT_NAME,
    PROPT_START,
    PROPT_CARD,

    PROPT_COUNT
};

static EventOption Options_Presets[PROPT_COUNT] = {
    {
        .kind = OPTKIND_STRING,
        .name = "Preset",
        .value_num = PS_COUNT,
        .values = preset_names,
        .desc = {preset_desc[0], preset_desc[1], preset_desc[2], "Load it or save to it below."},
        .OnChange = Event_ChangePreset,
    },
    {
        .kind = OPTKIND_FUNC,
        .name = "Load Preset",
        .desc = {"Set every menu the way the preset above has",
                 "it."},
        .OnSelect = Event_PresetLoad,
    },
    {
        .kind = OPTKIND_FUNC,
        .name = "Save to Preset",
        .desc = {"Keep the current settings in the preset above:",
                 "User Custom or Preset 1 to 4. Defaults,",
                 "Minimal, Everything and Ledge Drill are built",
                 "in and stay as they are."},
        .OnSelect = Event_PresetSave,
    },
    {
        .kind = OPTKIND_FUNC,
        .name = "Name Preset",
        .desc = {"Give the preset above a name: Preset 1 to 4",
                 "only."},
        .OnSelect = Event_PresetName,
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Load at Start",
        .value_num = PS_COUNT,
        .values = preset_names,
        .desc = {"The preset the event starts with. User Custom",
                 "is how you left the menus the last time you",
                 "closed them."},
        .OnChange = Event_ChangePresetStart,
    },
    {
        .kind = OPTKIND_INFO,
        .name = "Memory Card",
        .desc = {preset_card_desc[0], preset_card_desc[1], preset_card_desc[2]},
    },
};

static EventMenu Menu_Presets = {
    .name = "Presets",
    .option_num = countof(Options_Presets),
    .options = Options_Presets,
};

enum options_main
{
    OPT_PRESETS,
    OPT_CUES,
    OPT_TIMERS,
    OPT_PATHS,
    OPT_HUD,
    OPT_SOUNDS,
    OPT_LEDGE,
    OPT_JUMP,
    OPT_CAMERA,
    OPT_GAME,
    OPT_DEV,
    OPT_CONTROLS,
    OPT_HELP,
    OPT_EXIT,

    OPT_COUNT
};

static EventOption Options_Main[OPT_COUNT] = {
    {
        .kind = OPTKIND_MENU,
        .name = "Presets",
        .menu = &Menu_Presets,
        .desc = {"Load and save whole setups, and pick the one",
                 "the event starts with. Settings are kept on",
                 "the memory card in slot A."},
    },
    {
        .kind = OPTKIND_MENU,
        .name = "Cues",
        .menu = &Menu_Cues,
        .desc = {"Which landings get cues (NIL, AI, waveland), the",
                 "AI filter, Body Flash and Platform Glow."},
    },
    {
        .kind = OPTKIND_MENU,
        .name = "Timers",
        .menu = &Menu_Timers,
        .desc = {"The countdowns to each input: near you, in a",
                 "corner, at the landing spot, and for wavelands",
                 "and wavedashes."},
    },
    {
        .kind = OPTKIND_MENU,
        .name = "Paths",
        .menu = &Menu_Paths,
        .desc = {"The landing, body and ledge route paths, input",
                 "markers, frame dots, the slide-off line and jump",
                 "previews."},
    },
    {
        .kind = OPTKIND_MENU,
        .name = "HUD",
        .menu = &Menu_Hud,
        .desc = {"The controller display and the info panel."},
    },
    {
        .kind = OPTKIND_MENU,
        .name = "Sounds",
        .menu = &Menu_Sounds,
        .desc = {"The chime on a hit, and which slips buzz."},
    },
    {
        .kind = OPTKIND_MENU,
        .name = "Ledge Practice",
        .menu = &Menu_Ledge,
        .desc = {"Routes from the ledge with the most GALINT,",
                 "quicktime Assist, and reset options like the",
                 "ledgedash training."},
    },
    {
        .kind = OPTKIND_MENU,
        .name = "Jump Timing",
        .menu = &Menu_Jump,
        .desc = {"In a fall, when to double jump and which",
                 "aerial to press to land as an AI or a NIL."},
    },
    {
        .kind = OPTKIND_MENU,
        .name = "Camera",
        .menu = &Menu_Camera,
        .desc = {"The Training Lab's camera modes."},
    },
    {
        .kind = OPTKIND_MENU,
        .name = "Speed",
        .menu = &Menu_Game,
        .desc = {"Game speed and frame advance."},
    },
    {
        .kind = OPTKIND_MENU,
        .name = "Developer",
        .menu = &Menu_Dev,
        .desc = {"Accuracy count, collision view, debug log and",
                 "test scripts."},
    },
    {
        .kind = OPTKIND_MENU,
        .name = "Controls",
        .menu = &Menu_Controls,
        .desc = {"Every button Landing Gear uses outside the menu."},
    },
    {
        .kind = OPTKIND_INFO,
        .name = "Help",
        .desc = {"Green NIL, pink AI, cyan waveland. Solid cells:",
                 "press. Hollow: touchdown. Periwinkle: GALINT.",
                 "White gate: press now. Slate: missed. Short hop",
                 "path dashed, body path dotted."},
    },
    {
        .kind = OPTKIND_FUNC,
        .name = "Exit",
        .desc = {"Return to the Event Selection Screen."},
        .OnSelect = Event_Exit,
    },
};

static EventMenu Menu_Main = {
    .name = "Landing Gear",
    .option_num = countof(Options_Main),
    .options = Options_Main,
};

EventMenu *Event_Menu = &Menu_Main;
