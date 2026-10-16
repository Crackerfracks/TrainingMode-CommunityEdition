/* landinglab/menus_practice.c
 *
 * Holds the Ledge, Jump, Camera, Controls, Developer and Rumble menus.
 *
 * Part of Landing Gear: src/landinglab.c includes it, in order, into one
 * translation unit, so it sees everything included before it (and mex.h,
 * events.h, landinglab_sim.h).
 *
 * Uses: option_values.
 * Refers ahead, through forward declarations, to: camera_views, naming,
 * script_commands, ledge_practice, reset_drill, event_update.
 */

// Ledge practice: the routes off the ledge, the quicktime assist, and the
// ledgedash training's reset and camera options.
enum options_ledge
{
    LOPT_ROUTES,
    LOPT_KIND,
    LOPT_SORT,
    LOPT_PICK,
    LOPT_ASSIST,
    LOPT_WAIT,
    LOPT_START,
    LOPT_RESET,
    LOPT_DELAY,
    LOPT_INV,
    LOPT_DROP,
    LOPT_REFERENCE_DEMO,
    LOPT_REFERENCE_PRACTICE,

    LOPT_COUNT
};

static EventOption Options_Ledge[LOPT_COUNT] = {
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Show Routes",
        .val = 1,
        .desc = {"While you hang, show the fastest ways from",
                 "the ledge to the stage in the timer, with the",
                 "ledge intangibility (GALINT) each one keeps.",
                 "The chosen route is on top, the next ones under."},
        .OnChange = Event_ChangeRoutes,
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Route Kind",
        .val = ROUTES_AI,
        .value_num = countof(route_kind_names),
        .values = route_kind_names,
        .desc = {"NIL: land on the stage with no landing lag.",
                 "AI: land with an aerial interrupt.",
                 "Both: the two lists together."},
        .OnChange = Event_ChangeRoutes,
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Route Sort",
        .value_num = countof(route_sort_names),
        .values = route_sort_names,
        .desc = {"How the routes are put in order.",
                 "GALINT: the most ledge intangibility first.",
                 "Easiest: widest aerial window, no fastfall and",
                 "least waiting first, then by GALINT."},
        .OnChange = Event_ChangeRoutes,
    },
    {
        .kind = OPTKIND_INT,
        .name = "Route",
        .value_min = 1,
        .value_num = 1,
        .val = 1,
        .format = route_pick_fmt,
        .desc = {route_desc[0], route_desc[1], route_desc[2], "Left/right browse, or D-pad up on the ledge."},
        .OnChange = Event_ChangeRoutes,
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Assist",
        .desc = {"Quicktime practice: the game freezes on each",
                 "input of the route and waits for it, then plays",
                 "on at full speed. A slip only misses if it costs",
                 "the landing, or its timing is checked in Sounds."},
        .OnChange = Event_ChangeRoutes,
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Assist Wait",
        .value_num = countof(wait_names),
        .val = 2,
        .values = wait_names,
        .desc = {"How long Assist waits for each input before it",
                 "steps a frame or starts over. Shorten it as the",
                 "timing sinks in, down toward real time."},
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Starting Position",
        .value_num = countof(start_names),
        .values = start_names,
        .desc = {"Where Reset puts you after an attempt: on a",
                 "ledge, or where you saved with D-pad right."},
        .OnChange = Event_ChangeLedgeStart,
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Reset",
        .value_num = countof(reset_names),
        .values = reset_names,
        .desc = {"Start over after each attempt. Swap and Random",
                 "change ledges. Assist always starts over on the",
                 "same side when this is None."},
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Reset Delay",
        .value_num = countof(reset_delay_names),
        .val = 1,
        .values = reset_delay_names,
        .desc = {"How long the result stays before starting over."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Keep Ledge Invincibility",
        .desc = {"Keep the full intangibility while hanging, so",
                 "the routes don't shrink while you get ready."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Drop Drill",
        .desc = {"Time letting go of the ledge: a DRP row counts",
                 "down to the first frame you can. The stick has to",
                 "rest on the hang's first frame, then go down or",
                 "away. Graded, with how many of your last 10 hit."},
    },
    {
        .kind = OPTKIND_FUNC,
        .name = "15 GALINT Demo",
        .desc = {"Falcon on Battlefield: play the verified AI Bair",
                 "route from a natural regrab, then dash with 15",
                 "intangibility left. D-pad left plays it again.",
                 "Your cues stay on; helpers return afterward."},
        .OnSelect = Event_ReferenceDemo,
    },
    {
        .kind = OPTKIND_FUNC,
        .name = "Practice 15 GALINT",
        .desc = {"Falcon on Battlefield: the regrab is set up for",
                 "you, then Assist waits for each input of the 15",
                 "GALINT bair. Misses and landings set it up again.",
                 "Pick it again to stop."},
        .OnSelect = Event_ReferencePractice,
    },
};

static EventMenu Menu_Ledge = {
    .name = "Ledge Practice",
    .option_num = countof(Options_Ledge),
    .options = Options_Ledge,
};

// Jump timing: in a fall with the double jump left, when to jump, with which
// stick, and which aerial to press when, to land as an aerial interrupt or
// a NIL on a platform or the floor.
enum options_jump
{
    JOPT_SHOW,
    JOPT_KIND,
    JOPT_TARGET,
    JOPT_LEAD,
    JOPT_PATH,
    JOPT_TEXT,
    JOPT_PREFER,
#ifdef LL_GROUND_GUIDE
    JOPT_BANDS,
    JOPT_MARKS,
#endif

    JOPT_COUNT
};

// Where the jump lands: any floor, or one kind of platform. Left and right
// are of the stage's middle; Top is the highest platform.
enum jump_target
{
    JTGT_ANY,
    JTGT_TOP,
    JTGT_LEFT,
    JTGT_RIGHT,
    JTGT_PLATFORM,
    JTGT_FLOOR,
};
// From the ground: which hops to look at. The live search from the ground
// is off (Stephen, 0.8.3 notes: it made him stand still and wait, then sent
// him to a platform's edge); a guide worked out ahead of time replaces it.
enum { HOP_OFF, HOP_SHORT, HOP_FULL, HOP_BOTH };
// Standing still, both hops are searched and the plan is shown parked until
// takeoff (Stephen, 0.8.5); moving on the ground, nothing is searched.
#define JT_HOP_SETTING HOP_BOTH

static const char *jump_target_names[] = {"Any", "Top Platform", "Left Platform", "Right Platform", "Any Platform",
                                          "Main Floor"};

// How far ahead a jump must be to be shown. Seeing a cue and reacting to it
// takes about 12 to 15 frames on its own, so a jump found 3 frames before
// it's due (as the search used to find most of them) can't be timed, only
// chased (Stephen, 0.8.4). The search looks this far ahead instead.
static const char *jump_lead_names[] = {"12 Frames", "16 Frames", "20 Frames", "24 Frames", "30 Frames"};
static const u8 jump_leads[] = {12, 16, 20, 24, 30};

// Which jump to show when more than one lands: the one that acts soonest, a
// straight-up one when it's no more than about 10 frames later, or a
// straight-up one whenever there is one (Stephen, 0.9.30: mostly vertical
// jumps are what's played)
enum { PREFER_SOONEST, PREFER_BALANCED, PREFER_VERTICAL };
static const char *jump_prefer_names[] = {"Soonest", "Balanced", "Vertical"};

static EventOption Options_Jump[JOPT_COUNT] = {
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Show",
        .desc = {"While you fall with your double jump left,",
                 "show when to jump, with which stick, and which",
                 "aerial to press when, to land as an aerial",
                 "interrupt or a NIL on a platform or the floor."},
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Kind",
        .val = ROUTES_AI,
        .value_num = countof(route_kind_names),
        .values = route_kind_names,
        .desc = {"NIL: land with no landing lag.",
                 "AI: land with an aerial interrupt.",
                 "Both: whichever lets you act sooner."},
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Land On",
        .value_num = countof(jump_target_names),
        .values = jump_target_names,
        .desc = {"Where the jump should land. Any shows the one",
                 "that lets you act soonest, wherever it is.",
                 "Top is the highest platform; left and right",
                 "are either side of the stage's middle."},
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Warning",
        .val = 2,
        .value_num = countof(jump_lead_names),
        .values = jump_lead_names,
        .desc = {"Only show a jump at least this far ahead, so",
                 "there's time to see it coming. Jumps due sooner",
                 "are left out: there's no time to react to them."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Path",
        .desc = {"Draw the shown jump's landing spot, and its path",
                 "and gates as Path Detail (Paths) allows: a yellow",
                 "bar where you are on the jump's frame and a pink",
                 "one on the aerial's."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Plan Text", // off by default: the plan text was self-defeating (Stephen, 0.8.6)
        .desc = {"Spell the shown jump out in the info panel, as",
                 "in \"DJ up in 6f, nair +11-13: AI\"."},
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Prefer",
        .val = PREFER_VERTICAL, // what's played most (Stephen, 0.9.30)
        .value_num = countof(jump_prefer_names),
        .values = jump_prefer_names,
        .desc = {"Soonest: the jump that lets you act first.",
                 "Balanced: a straight-up double jump when it's",
                 "about 10 frames later at most. Vertical: a",
                 "straight-up one whenever one lands."},
    },
#ifdef LL_GROUND_GUIDE
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Takeoff Bands", // off by default: jump timing's furniture, not the AIs'
        .desc = {"On the ground: strips under the floor where a",
                 "hop (and a double jump) can start and still land",
                 "an AI, NIL or waveland on the platform above.",
                 "Nearer strip short hop, farther full hop."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Reach Marks", // likewise off by default
        .desc = {"On the ground: each platform lights the stretch a",
                 "hop from here can land on, brighter where more",
                 "timings work. Short hop nearer the surface."},
    },
#endif
};

static EventMenu Menu_Jump = {
    .name = "Jump Timing",
    .option_num = countof(Options_Jump),
    .options = Options_Jump,
};

// The Training Lab's camera modes. Presets come with saved settings.
enum options_camera
{
    CAMOPT_MODE,
    CAMOPT_VIEW,
    CAMOPT_SAVE,
    CAMOPT_NAME,

    CAMOPT_COUNT
};

#define CAM_ADVANCED 3 // cam_names

static EventOption Options_Camera[CAMOPT_COUNT] = {
    {
        .kind = OPTKIND_STRING,
        .name = "Camera Mode",
        .value_num = countof(cam_names),
        .values = cam_names,
        .desc = {"Adjust the camera's behavior.",
                 "In advanced mode, use C-Stick while holding",
                 "A/B/Y to pan, rotate and zoom, respectively."},
        .OnChange = Event_ChangeCamera,
    },
    {
        .kind = OPTKIND_STRING,
        .name = "View",
        .value_num = countof(view_names),
        .values = view_names,
        .desc = {"Jump the camera to a saved view (Advanced mode,",
                 "so it can still be moved). Each stage has its own",
                 "8 views. Presets keep which one they were saved",
                 "with."},
        .OnChange = Event_ChangeView,
    },
    {
        .kind = OPTKIND_FUNC,
        .name = "Save View",
        .desc = {"Keep the camera as it is now as the view picked",
                 "above, on this stage. Set it up in Advanced",
                 "mode, or pause on a moment you like."},
        .OnSelect = Event_SaveView,
    },
    {
        .kind = OPTKIND_FUNC,
        .name = "Name View",
        .desc = {"Give the view picked above a name (save it",
                 "first)."},
        .OnSelect = Event_NameView,
    },
};

static EventMenu Menu_Camera = {
    .name = "Camera",
    .option_num = countof(Options_Camera),
    .options = Options_Camera,
};

// Every button the event uses outside the menu.
static EventOption Options_Controls[] = {
    {
        .kind = OPTKIND_INFO,
        .name = "Menu",
        .desc = {"Start opens this menu."},
    },
    {
        .kind = OPTKIND_INFO,
        .name = "Save Position",
        .desc = {"Hold D-pad right to save where you are."},
    },
    {
        .kind = OPTKIND_INFO,
        .name = "Load Position",
        .desc = {"D-pad left puts you back where you saved.",
                 "When Reset starts from the ledge, it starts a",
                 "new attempt instead, and with a test script",
                 "chosen it plays the script again."},
    },
    {
        .kind = OPTKIND_INFO,
        .name = "Quick Menu",
        .desc = {"Click L or R all the way and press D-pad up or",
                 "down: your presets and main settings, over a",
                 "frozen game. The stick plays on, B closes it.",
                 "Trigger + A saves, trigger + X or Y saves new."},
    },
    {
        .kind = OPTKIND_INFO,
        .name = "Hide Everything",
        .desc = {"Click L and R both all the way and press D-pad",
                 "up or down: everything Landing Gear draws hides,",
                 "or comes back. With a trigger only partway in,",
                 "the D-pad does nothing."},
    },
    {
        .kind = OPTKIND_INFO,
        .name = "Next Route",
        .desc = {"While hanging from a ledge, D-pad up shows the",
                 "next ledge route in the list, and the first",
                 "again after the last. Ledge Practice > Route",
                 "picks any route directly."},
    },
    {
        .kind = OPTKIND_INFO,
        .name = "Frame Advance",
        .desc = {"D-pad down freezes the game, or lets it run."},
    },
    {
        .kind = OPTKIND_INFO,
        .name = "Step a Frame",
        .desc = {"While frozen, the Advance button steps one",
                 "frame; hold it to step slowly. It's Z unless",
                 "you change it in Speed."},
    },
};

static EventMenu Menu_Controls = {
    .name = "Controls",
    .option_num = countof(Options_Controls),
    .options = Options_Controls,
};

// Things only needed to test the event or report a wrong prediction.
enum options_dev
{
    DOPT_EXACT,
    DOPT_COLL,
    DOPT_LOG,
    DOPT_SCRIPT,
    DOPT_CLEAR,
    DOPT_INFO,

    DOPT_COUNT
};

static EventOption Options_Dev[DOPT_COUNT] = {
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Show Accuracy",
        .desc = {"Add a line to the panel counting how often the",
                 "prediction matched the landing exactly."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Collision Display",
        .desc = {"Show stage collision, your ECB, hitboxes and",
                 "hurtboxes, and hide your model so it doesn't",
                 "cover them."},
        .OnChange = Event_ChangeCollDisplay,
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Debug Log",
        .value_num = 4,
        .values = log_level_names,
        .desc = {"Extra lines in Dolphin's log. Landings: timer",
                 "presses and ledge routes. Frames: every airborne",
                 "frame too. Everything: camera too, slows play."},
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Script",
        .value_num = 1,
        .values = script_names,
        .desc = {"For testing: play inputs from TM/llscript.txt,",
                 "from a fixed spot and exact to the frame, with",
                 "Debug Log on. D-pad left plays it again. A shot",
                 "step freezes the game until D-pad down."},
        .OnChange = Event_ChangeScript,
    },
    {
        .kind = OPTKIND_FUNC,
        .name = "Forget Learned ECBs",
        .desc = {"Every fighter's jumps, falls, aerials and",
                 "airdodge are built in. This forgets what was",
                 "learned while playing and goes back to the",
                 "built-in data."},
        .OnSelect = Event_ClearLearned,
    },
    {
        .kind = OPTKIND_INFO,
        .name = "About",
        .desc = {"Gray paths rely on ECB frames the event hasn't",
                 "seen yet; it learns them as you play. With the",
                 "Debug Log on, D-pad up during Frame Advance hides",
                 "everything the event draws, for screenshots."},
    },
};

static EventMenu Menu_Dev = {
    .name = "Developer",
    .option_num = countof(Options_Dev),
    .options = Options_Dev,
};

// The main menu is a short list of groups, each its own page.

// A buzz that speeds up toward a window, per cue.
enum options_rumble
{
    RUOPT_AI,
    RUOPT_NIL,
    RUOPT_WL,
    RUOPT_NOTE,

    RUOPT_COUNT
};

static EventOption Options_Rumble[RUOPT_COUNT] = {
    {
        .kind = OPTKIND_TOGGLE,
        .name = "AI Rumble",
        .desc = {"Buzz the controller faster and faster up to an",
                 "AI window, stopping dead as it opens. Needs AI",
                 "Cues on."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "NIL Rumble",
        .desc = {"The same for NIL windows. Needs NIL Cues on."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Waveland Rumble",
        .desc = {"The same for perfect waveland and wavedash",
                 "windows. Needs Waveland Cues on."},
    },
    {
        .kind = OPTKIND_INFO,
        .name = "Game Rumble",
        .desc = {"While any of these is on, the game's own rumble",
                 "is off for your controller, so only the cues",
                 "buzz. Dolphin passes rumble only to a real",
                 "controller with rumble on in its settings."},
    },
};

static EventMenu Menu_Rumble = {
    .name = "Rumble",
    .option_num = countof(Options_Rumble),
    .options = Options_Rumble,
};
