/* landinglab/position_chords.c
 *
 * Holds saving and loading position, hiding the HUD, and button chords.
 *
 * Part of Landing Gear: src/landinglab.c includes it, in order, into one
 * translation unit, so it sees everything included before it (and mex.h,
 * events.h, landinglab_sim.h).
 *
 * Uses: option_values, menus_display, presets, tracking_state, corner_panel,
 * scripts.
 */

// Save and load position like the lab: hold D-pad right to save, press
// D-pad left to load. The event's own count of frames in Falcon's state
// goes with the save, since the predictor reads the state's ECB by it.
#define LL_SAVE_HOLD 10
static int save_hold;
static int saved_frame_in_state;

static void Position_Update(int held, int down)
{
    if (held & HSD_BUTTON_DPAD_RIGHT)
    {
        if (++save_hold == LL_SAVE_HOLD &&
            event_vars->Savestate_Save_v1(event_vars->savestate, 0))
            saved_frame_in_state = frame_in_state;
    }
    else
        save_hold = 0;

    if ((down & HSD_BUTTON_DPAD_LEFT) && event_vars->savestate->is_exist)
    {
        event_vars->Savestate_Load_v1(event_vars->savestate, 0);
        Script_ResetTracking();
        prev_state_id = ((FighterData *)Fighter_GetGObj(0)->userdata)->state_id;
        frame_in_state = saved_frame_in_state;
    }
}

// The quick toggles' steps: each list goes round, up or right forward.
typedef struct CueSet
{
    u8 ai, nil, wl;
    const char *name;
} CueSet;
static const CueSet cue_sets[] = {
    {1, 0, 1, "AI and waveland cues"},
    {1, 0, 0, "AI cues"},
    {0, 1, 0, "NIL cues"},
    {0, 0, 1, "Waveland cues"},
    {1, 1, 1, "All cues"},
    {0, 0, 0, "Cues off"},
};
// the paths in the order the toggle steps through them: body, landing, then
// both, first with the frame dots only in Frame Advance, then always
static const char *path_set_names[] = {"Paths off", "Body path", "Landing path", "Body and landing paths",
                                       "Body path with dots", "Landing path with dots", "Both paths with dots"};
static const u8 path_sets[][3] = {{0, 0, 0}, {0, 1, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 1}, {1, 0, 1}, {1, 1, 1}}; // landing, body, dots
static const char *pad_set_names[] = {"Controller off", "Ring, small", "Ring, medium", "Ring, large",
                                      "Crest, small", "Crest, medium", "Crest, large",
                                      "Classic, small", "Classic, medium", "Classic, large"};
static int chord_pad_place; // where the controller goes when it comes back on

// Everything the event draws, hidden or back.
static void Hide_Toggle(void)
{
    hide_all ^= 1;
    Toast(hide_all ? "Landing Gear hidden" : "Landing Gear shown");
    SFX_PlayCommon(2);
}

static int Chord_Step(int i, int n, int dir)
{
    return i < 0 ? (dir > 0 ? 0 : n - 1) : (i + dir + n) % n;
}

static void Chord(int lr, int z, int down)
{
    int dir = down & (HSD_BUTTON_DPAD_UP | HSD_BUTTON_DPAD_RIGHT) ? 1 : down & (HSD_BUTTON_DPAD_DOWN | HSD_BUTTON_DPAD_LEFT) ? -1 : 0;
    if (!dir)
        return;
    int vert = (down & (HSD_BUTTON_DPAD_UP | HSD_BUTTON_DPAD_DOWN)) != 0;
    char buf[40];
    if (lr && z && vert)
    {
        // everything the event draws, hidden or back
        hide_all ^= 1;
        Toast(hide_all ? "Landing Gear hidden" : "Landing Gear shown");
    }
    else if (lr && z)
    {
        // the next preset that has settings in it
        int cur = Options_Presets[PROPT_PICK].val;
        for (int n = 0; n < PS_COUNT; n++)
        {
            cur = Chord_Step(cur, PS_COUNT, dir);
            if (Preset_Slot(cur)->used)
                break;
        }
        Options_Presets[PROPT_PICK].val = cur;
        Preset_Apply(Preset_Slot(cur));
        Preset_Use(cur);
        Preset_Describe(cur);
        sprintf(buf, "Preset: %s", preset_names[cur]);
        Toast(buf);
    }
    else if (lr && vert)
    {
        // the cue set
        int wl = Options_Cues[COPT_WL].val, cur = -1;
        for (int i = 0; i < (int)countof(cue_sets); i++)
            if (cue_sets[i].ai == Options_Cues[COPT_AI].val && cue_sets[i].nil == Options_Cues[COPT_NIL].val && cue_sets[i].wl == (wl != 0))
                cur = i;
        const CueSet *c = &cue_sets[Chord_Step(cur, countof(cue_sets), dir)];
        Options_Cues[COPT_AI].val = c->ai;
        Options_Cues[COPT_NIL].val = c->nil;
        Options_Cues[COPT_WL].val = c->wl ? (wl ? wl : 2) : 0;
        Toast(c->name);
    }
    else if (lr)
    {
        // the landing and body paths, and the frame dots on them
        int dots = Options_Paths[POPT_TICKS].val == 1, cur = -1;
        for (int i = 0; i < (int)countof(path_sets); i++)
            if (path_sets[i][0] == Options_Paths[POPT_PATH].val && path_sets[i][1] == Options_Paths[POPT_BODY].val &&
                (path_sets[i][2] == dots || i == 0))
                cur = i;
        int next = Chord_Step(cur, countof(path_sets), dir);
        Options_Paths[POPT_PATH].val = path_sets[next][0];
        Options_Paths[POPT_BODY].val = path_sets[next][1];
        if (next != 0)
            Options_Paths[POPT_TICKS].val = path_sets[next][2] ? 1 : 0;
        Toast(path_set_names[next]);
    }
    else if (vert)
    {
        // the controller: off, then each look at each size
        int place = Options_Hud[HOPT_STICK].val;
        int cur = place == 3 ? 0 : 1 + 3 * Options_Hud[HOPT_LOOK].val + Options_Hud[HOPT_PAD_SIZE].val;
        if (place != 3)
            chord_pad_place = place;
        int next = Chord_Step(cur, countof(pad_set_names), dir);
        if (next == 0)
            Options_Hud[HOPT_STICK].val = 3;
        else
        {
            Options_Hud[HOPT_STICK].val = chord_pad_place;
            Options_Hud[HOPT_LOOK].val = (next - 1) / 3;
            Options_Hud[HOPT_PAD_SIZE].val = (next - 1) % 3;
        }
        Toast(pad_set_names[next]);
    }
    else
    {
        // how strongly everything shows
        EventOption *o = &Options_Cues[COPT_INTENSITY];
        o->val = o->val + dir < 0 ? 0 : o->val + dir >= o->value_num ? o->value_num - 1 : o->val + dir;
        sprintf(buf, "Intensity %s", intensity_names[o->val]);
        Toast(buf);
    }
    SFX_PlayCommon(2);
}
