/* landinglab/reset_drill.c
 *
 * Holds the drill's resets, and the ledge start and camera callbacks.
 *
 * Part of Landing Gear: src/landinglab.c includes it, in order, into one
 * translation unit, so it sees everything included before it (and mex.h,
 * events.h, landinglab_sim.h).
 *
 * Uses: option_values, menus_practice, menus_display, tracking_state,
 * hud_text, scripts, script_commands, ledge_routes.
 * Refers ahead, through forward declarations, to: assist.
 */

///////////////////////
/// Reset and drill ///
///////////////////////

static int Reset_Mode(void)
{
    int mode = Options_Ledge[LOPT_RESET].val;
    if (mode == RESET_NONE && Options_Ledge[LOPT_ASSIST].val)
        mode = RESET_SAME;
    return mode;
}

static void Drill_Finish(int success)
{
    attempt_active = 0;
    int mode = Reset_Mode();
    if (mode == RESET_NONE)
        return;
    int delay = Options_Ledge[LOPT_DELAY].val;
    drill_timer = success ? reset_delay_hit[delay] : reset_delay_miss[delay];
    drill_success = success;
    int facing = attempt_facing ? attempt_facing : drill_side;
    switch (mode)
    {
    case RESET_SAME:
        drill_side = facing;
        break;
    case RESET_SWAP:
        drill_side = -facing;
        break;
    case RESET_SWAP_HIT:
        drill_side = success ? -facing : facing;
        break;
    case RESET_RANDOM:
        drill_side = HSD_Randi(2) ? 1 : -1;
        break;
    }
}

static void Assist_Clear(void);

// Start over: on a ledge, or from the saved position.
static void Drill_Reset(void)
{
    GOBJ *ft = Fighter_GetGObj(0);
    drill_timer = -1;
    route_active = 0;
    Assist_Clear();
    if (reference_mode == 2)
    {
        // the regrab is the setup: play its preparation again
        Event_ChangeScript(0, Options_Dev[DOPT_SCRIPT].val);
        return;
    }
    if (Options_Ledge[LOPT_START].val == START_SAVED && event_vars->savestate->is_exist)
    {
        event_vars->Savestate_Load_v1(event_vars->savestate, 0);
        Script_ResetTracking();
        prev_state_id = ((FighterData *)ft->userdata)->state_id;
        return;
    }
    ScriptOp op = {0};
    op.facing = drill_side;
    Script_PlaceLedge(ft, &op);
    prev_state_id = ((FighterData *)ft->userdata)->state_id;
}

static void Drill_Think(void)
{
    if (drill_timer < 0)
        return;
    if (Reset_Mode() == RESET_NONE)
    {
        drill_timer = -1;
        return;
    }
    if (--drill_timer <= 0)
        Drill_Reset();
}

void Event_ChangeLedgeStart(GOBJ *menu, int value)
{
    drill_timer = -1;
}

void Event_ChangeCamera(GOBJ *menu, int value)
{
    MatchCamera *cam = stc_matchcam;
    if (menu)
        Options_Camera[CAMOPT_VIEW].val = 0; // a mode picked by hand leaves the view
    if (value == 0)
        Match_SetNormalCamera();
    else if (value == 1)
    {
        Match_SetFreeCamera(0, 3);
        cam->freecam_fov.X = 140;
        cam->freecam_rotate.Y = 10;
    }
    else if (value == 2)
        Match_SetFixedCamera();
    else
        Match_SetDevelopCamera();
    Match_CorrectCamera();
}
