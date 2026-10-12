/* landinglab/frame_advance.c
 *
 * Holds Frame Advance: pausing and stepping the game.
 *
 * Part of Landing Lab: src/landinglab.c includes it, in order, into one
 * translation unit, so it sees everything included before it (and mex.h,
 * events.h, landinglab_sim.h).
 *
 * Uses: option_values, menus_display, naming, controller, gx_draw.
 */

///////////////////////
/// Frame advance   ///
///////////////////////

// The game's debug pause, as the lab uses it: while Frame Advance is on the
// game stays paused, and each press of the advance button runs one frame
// (held for half a second, it steps 10 times a second). The pause and
// step checks run every frame, paused or not; the scene resets them when the
// event ends.
#define LL_ADVANCE_HOLD 30   // frames held before it repeats
#define LL_ADVANCE_REPEAT 6  // then one step every this many frames

static int Advance_Port(void)
{
    return Fighter_GetControllerPort(0);
}

static int assist_advance; // Assist runs one frame further into the window

static int Advance_CheckPause(void)
{
    HSD_Update *update = stc_hsd_update;
    int paused = update->pause_kind & 1;
    return paused != (Options_Game[GOPT_FRAME_ADV].val || assist_frozen || namer.on || deck_frozen);
}

static int Advance_CheckStep(void)
{
    static int timer;
    int port = Advance_Port();
    HSD_Pad *pad = PadGetMaster(port);
    HSD_Pad *engine = PadGetEngine(port);
    int button = adv_button_masks[Options_Game[GOPT_ADV_BUTTON].val];

    if (namer.on || deck_frozen)
        return 0; // the name grid or the quick menu has the buttons
    if (assist_advance)
    {
        assist_advance = 0;
        return 1;
    }
    // with Frame Advance off the button is the game's (L airdodges)
    if (Pause_CheckStatus(1) == 2 || !(Options_Game[GOPT_FRAME_ADV].val || assist_frozen) || !(pad->held & button))
    {
        timer = 0;
        return 0;
    }
    timer++;
    if (timer != 1 && (timer < LL_ADVANCE_HOLD || (timer - LL_ADVANCE_HOLD) % LL_ADVANCE_REPEAT))
        return 0;

    // the game doesn't see the advance button
    pad->down &= ~button;
    pad->held &= ~button;
    engine->down &= ~button;
    engine->held &= ~button;
    if (button == HSD_TRIGGER_L)
    {
        pad->triggerLeft = 0;
        pad->ftriggerLeft = 0;
        engine->triggerLeft = 0;
        engine->ftriggerLeft = 0;
    }
    else if (button == HSD_TRIGGER_R)
    {
        pad->triggerRight = 0;
        pad->ftriggerRight = 0;
        engine->triggerRight = 0;
        engine->ftriggerRight = 0;
    }
    return 1;
}
