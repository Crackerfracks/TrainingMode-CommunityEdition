/* landinglab/event_update.c
 *
 * Holds Event_Update (drawing each frame), the collision display and clear
 * callbacks, and Event_Exit.
 *
 * Part of Landing Lab: src/landinglab.c includes it, in order, into one
 * translation unit, so it sees everything included before it (and mex.h,
 * events.h, landinglab_sim.h).
 *
 * Uses 21 of the 49 modules before it.
 */

void Event_Update(void)
{
    if (Pause_CheckStatus(1) != 2)
        HSD_SetSpeedEasy(speed_values[Options_Game[GOPT_SPEED].val]);
    else
        HSD_SetSpeedEasy(1.0);

    // runs every frame, frozen or not: Assist waits for its input, D-pad
    // down toggles frame advance, D-pad left plays the chosen script again,
    // or else the D-pad saves and loads position (or puts Falcon back on
    // the ledge, when Reset starts from one)
    Presets_Update();
    if (toast_timer > 0)
        toast_timer--;
    Rumble_Update(Fighter_GetGObj(0)->userdata,
                  Pause_CheckStatus(1) == 2 || Options_Game[GOPT_FRAME_ADV].val || assist_frozen || deck_frozen);
    // while a script runs, a line in the log every 5 seconds, frozen or not:
    // whoever watches the log tells a hang (no lines) from a shot waiting,
    // and which shot, should its own line have gone missing
    static int script_beat;
    if (script_cur >= 0 && ++script_beat >= 300)
    {
        script_beat = 0;
        char buf[128];
        if (Options_Game[GOPT_FRAME_ADV].val)
            sprintf(buf, "LLBEAT %d frozen at shot \"%s\", waiting for D-pad down\n", event_vars->game_timer, shot_label);
        else
            sprintf(buf, "LLBEAT %d running\n", event_vars->game_timer);
        Log(buf);
    }
    if (card_probe > 0)
    {
        if (script_cur >= 0)
        {
            char buf[48];
            sprintf(buf, "LLPROBE update %d at %d\n", CARD_PROBE - card_probe, event_vars->game_timer);
            Log(buf);
        }
        card_probe--;
    }
    if (namer.on)
    {
        if (namer.leave_menu)
        {
            namer.leave_menu = 0;
            if (Pause_CheckStatus(1) == 2)
                event_vars->Menu_Exit(event_vars->menu_gobj);
            return;
        }
        Namer_Think();
        return;
    }
    if (deck_on)
    {
        if (Pause_CheckStatus(1) == 2)
            Deck_Close("pause");
        else
        {
            Deck_Think();
            if (deck_on)
                return; // the menu has the buttons, frozen or playing out
        }
    }
    if (Pause_CheckStatus(1) == 2)
        return;
    Assist_Update();
    HSD_Pad *pad = PadGetMaster(Advance_Port());
    int down = pad->down;
    // L or R clicked all the way (a light press doesn't count, so the
    // D-pad while shielding does nothing) with D-pad up or down opens the
    // quick menu; both triggers with it hide everything or bring it back.
    // The D-pad does nothing else meanwhile.
    int lh = (pad->held & HSD_TRIGGER_L) != 0, rh = (pad->held & HSD_TRIGGER_R) != 0;
    if (lh || rh)
    {
        if (down & (HSD_BUTTON_DPAD_UP | HSD_BUTTON_DPAD_DOWN))
        {
            if (lh && rh)
                Hide_Toggle();
            else if (Options_Hud[HOPT_DECK].val)
                Deck_Open();
        }
        save_hold = 0;
        return;
    }
    // a trigger partway in (shielding, or on its way to the click that
    // opens the quick menu): the D-pad does nothing, so it can't toggle
    // Frame Advance or load a position by chance
    if (pad->ftriggerLeft > 0.3f || pad->ftriggerRight > 0.3f)
    {
        save_hold = 0;
        return;
    }
    if (down & HSD_BUTTON_DPAD_DOWN)
        Options_Game[GOPT_FRAME_ADV].val ^= 1;
    // a clean frame for mockups, the same frame as the one with cues. Only
    // for the Developer's captures, and never left on past Frame Advance:
    // pressed by chance, everything the event draws would stay hidden.
    if ((down & HSD_BUTTON_DPAD_UP) && Options_Game[GOPT_FRAME_ADV].val && Log_Level())
        capture_clean ^= 1;
    else if ((down & HSD_BUTTON_DPAD_UP) && hang_ledge >= 0 && route_list_num > 1 && Routes_On())
    {
        // hanging: the next route in the list, back to the first after the last
        EventOption *o = &Options_Ledge[LOPT_PICK];
        o->val = o->val >= route_list_num ? 1 : o->val + 1;
        Route_RememberPick();
        Route_MenuText();
        SFX_PlayCommon(2);
    }
    if (!Options_Game[GOPT_FRAME_ADV].val)
        capture_clean = 0;
    if ((down & HSD_BUTTON_DPAD_LEFT) && Options_Dev[DOPT_SCRIPT].val)
    {
        Event_ChangeScript(0, Options_Dev[DOPT_SCRIPT].val);
    }
    else if ((down & HSD_BUTTON_DPAD_LEFT) && Reset_Mode() != RESET_NONE &&
             Options_Ledge[LOPT_START].val == START_LEDGE)
        Drill_Reset();
    else if (script_cur < 0)
        Position_Update(pad->held, down);
}

void Event_ChangeCollDisplay(GOBJ *menu, int value)
{
    FighterData *fp = Fighter_GetGObj(0)->userdata;

    stc_matchcam->show_coll = value;
    fp->show_model = !value;
    fp->show_hit = value;
}

void Event_ClearLearned(GOBJ *menu)
{
    Learned_Reset();
    stat_total = 0;
    stat_exact = 0;
    sprintf(text_exact, "-");
    SFX_PlayCommon(1);
}

static void Presets_KeepUser(void);
static void Card_Flush(void);

void Event_Exit(GOBJ *menu)
{
    // leaving from the menu: it never closes, so keep the settings now
    Presets_KeepUser();
    Card_Flush();
    Rumble_Release();
    stc_match->state = 3;
    Match_EndVS();
}
