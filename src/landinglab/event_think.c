/* landinglab/event_think.c
 *
 * Holds Event_Init and Event_Think: setup, and everything each frame.
 *
 * Part of Landing Lab: src/landinglab.c includes it, in order, into one
 * translation unit, so it sees everything included before it (and mex.h,
 * events.h, landinglab_sim.h).
 *
 * Uses 32 of the 46 modules before it.
 */

void Event_Init(GOBJ *gobj)
{
    int init_tick = OSGetTick(); // the start-up work, timed for the log
    Cues_Clear();
    Sim_Setup();
    pred_live = calloc(sizeof(Prediction));
    pred_seg = calloc(sizeof(Prediction));
    pred_fh = calloc(sizeof(Prediction));
    pred_sh = calloc(sizeof(Prediction));
    actual_pos = calloc(sizeof(Vec2) * (LL_SIM_FRAMES + 1));
    actual_bottom = calloc(sizeof(float) * (LL_SIM_FRAMES + 1));
    quads = calloc(sizeof(Quad) * LL_QUADS);
    ledges = calloc(sizeof(LedgeEntry) * LR_LEDGES);
    pred_route = calloc(sizeof(Prediction));
    route_list = calloc(sizeof(LedgeRoute) * 2 * LR_ROUTES);
    route_build = calloc(sizeof(RouteList));
    route_path = calloc(sizeof(Vec2) * LR_PATH);
    route_path_bottom = calloc(sizeof(float) * LR_PATH);
    jt_cache = calloc(sizeof(SimState) * JT_D);
    gt_steps = calloc(sizeof(GroundStep) * JT_D);
    gl_lib = calloc(sizeof(GlEntry) * GL_KEYS * GT_STILL_CANDIDATES * GL_HEIGHTS);
    gl_pre_sx = calloc(sizeof(float) * GL_X);
    gl_pre_0 = calloc(sizeof(float) * GL_X);
    gl_dj = calloc(sizeof(float) * GT_STILL_STICKS * GL_X);
    gl_gen = -1;
#ifdef LL_GROUND_GUIDE
    gd_mem = (u8 *)(((u32)calloc(GD_STRIDE * GD_SLOTS + 32) + 31) & ~31);
    for (int j = 0; j < GD_SLOTS; j++)
        gd_slice[j] = -1;
#endif
    jt_path = calloc(sizeof(Vec2) * JT_PATH);
    jt_path_bottom = calloc(sizeof(float) * JT_PATH);
    slide_pos = calloc(sizeof(Vec2) * 2 * SLIDE_MAX);
    slide_bottom = calloc(sizeof(float) * 2 * SLIDE_MAX);

    // HUD panel on the event gobj, paths on their own gobj in world space
    GObj_AddGXLink(gobj, Hud_GX, GXLINK_HUD, 80);
    GOBJ *draw_gobj = GObj_Create(0, 0, 0);
    GObj_AddGXLink(draw_gobj, World_GX, 5, 0);

    // frame advance, and scripts fed in before the fighters read their
    // controllers (like the lab's recordings)
    HSD_Update *update = stc_hsd_update;
    update->checkPause = Advance_CheckPause;
    update->checkAdvance = Advance_CheckStep;
    Script_Load();
    Presets_Init();
    GOBJ *script_gobj = GObj_Create(0, 7, 0);
    GObj_AddProc(script_gobj, Script_Think, 3);

    char buf[64];
    sprintf(buf, "LLPERF init %.2f ms\n", OSTicksToMicroseconds(OSGetTick() - init_tick) / 1000.f);
    Log(buf);
}

static void Event_ThinkFrame(GOBJ *event);

// The event's work each frame, timed for the log: the most it took over
// each second, all of it and the ledge route search in it.
static float perf_think, perf_solve;
static int perf_frames;

void Event_Think(GOBJ *event)
{
    int t0 = OSGetTick();
    Event_ThinkFrame(event);
    float ms = OSTicksToMicroseconds(OSGetTick() - t0) / 1000.f;
    if (ms > perf_think)
        perf_think = ms;
    if (++perf_frames >= 60)
    {
        if (Log_Level())
        {
            char buf[96];
            sprintf(buf, "LLPERF %d think %.2f ms solve %.2f ms quads %d\n", event_vars->game_timer, perf_think, perf_solve, quad_peak);
            Log(buf);
        }
        perf_frames = 0;
        perf_think = 0;
        perf_solve = 0;
        quad_peak = 0;
    }
}

// Zelda and Sheik changing into each other: the other one's tables, and
// the ledges' routes and the hops found again for it.
static void Fighter_Changed(FighterData *fp)
{
    Ecb_LoadDisc(fp);
    Log_Attributes(fp);
    memset(ledges, 0, sizeof(LedgeEntry) * LR_LEDGES);
    ledges_found = 0;
    route_build_ledge = route_list_ledge = route_path_ledge = route_browse = -1;
    gl_gen = -1;
    Script_ResetTracking();
}

static void Event_ThinkFrame(GOBJ *event)
{
    GOBJ *ft = Fighter_GetGObj(0);
    FighterData *fp = ft->userdata;
    Script_TraceFrame(fp);
    if (card_probe > 0 && script_cur >= 0)
    {
        char buf[48];
        sprintf(buf, "LLPROBE think %d at %d\n", CARD_PROBE - card_probe, event_vars->game_timer);
        Log(buf);
    }

    if (!attributes_logged)
    {
        Ecb_LoadDisc(fp);
        Log_Attributes(fp);
        attributes_logged = 1;
    }
    else if (fp->kind != sim_kind)
        Fighter_Changed(fp);

    ai_show_all = Options_Cues[COPT_AI_FILTER].val == 1;
    ai_only = Options_Cues[COPT_AI_AERIAL].val ? AERIAL_BIT(TS_AIRN + Options_Cues[COPT_AI_AERIAL].val - 1) : 0;
    int logging = Log_Level() >= LOG_FRAMES;
    cue_log = Log_Level() >= LOG_LANDINGS;

    int sid = fp->state_id;
    int ts = Tracked_Index(sid);
    int airborne = fp->phys.air_state == 1;
    int disturbed = fp->flags.hitlag || fp->flags.hitstun ||
                    fp->phys.kb_vel.X != 0 || fp->phys.kb_vel.Y != 0;

    // count frames in the current state, and learn how long a state lasts
    // when it ends by itself
    if (sid != prev_state_id)
    {
        if (prev_tracked_air && ts >= 0 && Tracked_EndedInto(prev_ts, ts))
        {
            state_len[prev_ts] = frame_in_state + 1;
            state_next[prev_ts] = ts;
        }
        frame_in_state = 0;
    }
    else if (!fp->flags.hitlag)
        frame_in_state++;
    {
        const char *name = ts >= 0 ? tracked_state_names[ts] : State_Name(sid);
        if (name)
            sprintf(text_frame, "%s frame %d", name, frame_in_state + 1);
        else
            sprintf(text_frame, "State %d frame %d", sid, frame_in_state + 1);
    }

    int tracked_air = ts >= 0 && airborne && !disturbed;
    if (sid == ASID_ESCAPEAIR && prev_state_id != sid)
    {
        dodge_start = event_vars->game_timer;
        dodge_late_ok = prev_wl_late;
    }

    // a wavedash's airdodge works on the takeoff frame itself (KneeBend goes
    // straight into the airdodge): in the jumpsquat, count down to it
    // (KneeBend takes off once its frame reaches the startup time). Its
    // window is the frame before, as for any press; on the takeoff frame a
    // jump with no airdodge yet is a frame late
    squat_wd = 0;
    wd_preload = 0;
    if (Cues_Wd() && !disturbed)
    {
        if (sid == ASID_KNEEBEND)
        {
            float left = fp->attr.jump_startup_time - fp->state.frame;
            int until = (int)left;
            if (until < left)
                until++;
            squat_wd = until < 1 ? 1 : until;
        }
        else if (airborne && prev_state_id == ASID_KNEEBEND)
            squat_wd = 1;
        else if (!airborne && Ground_CanJump(sid))
        {
            // a jump pressed now: its jumpsquat starts a frame on from
            // here, at frame 0 (as squat_wd above sees it; 0.9.12 counted
            // from frame 1 and preloaded a frame short)
            float left = fp->attr.jump_startup_time;
            int until = (int)left;
            if (until < left)
                until++;
            wd_preload = (until < 1 ? 1 : until) + 1;
        }
        if (cue_log && sid == ASID_KNEEBEND && prev_state_id != ASID_KNEEBEND)
            OSReport("LLWDPRE %d squat_wd %d frame %.1f was %d\n", event_vars->game_timer, squat_wd, fp->state.frame,
                     wd_last);
        wd_last = wd_preload;
    }

    galint_now = sid == ASID_CLIFFWAIT ? 0 : fp->hurt.intang_frames.ledge;
    sprintf(text_next, "-");
    text_steps[0] = 0;
    next_kind = -1;

    Cues_Begin();

    // an airdodge on the takeoff frame or the one after is the wavedash,
    // whatever the jump's prediction made of it
    Cue *wdc = &cue_live[CUE_WL];
    if (wdc->wd && wdc->phase == PH_WINDOW && !wdc->held && !wdc->dim && prev_state_id != sid &&
        (sid == ASID_ESCAPEAIR || sid == ASID_LANDINGFALLSPECIAL))
    {
        wd_late = prev_state_id != ASID_KNEEBEND;
        Cue_Pressed(CUE_WL);
    }
    // landing on the takeoff frame shows no air frame at all
    int wd_landed = prev_state_id == ASID_KNEEBEND && sid == ASID_LANDINGFALLSPECIAL;
    if (wd_landed)
        seg_valid = 0;
    if ((prev_tracked_air || wd_landed) && !tracked_air)
        Landing_Resolve(fp);
    else
        Ghost_Update(sid);

    Panel_UpdateSide(fp);
    // once, from a moment of the idle animation that's always the same:
    // following it every frame would bob the paths with his breathing
    if (sid == ASID_WAIT && frame_in_state == 10 && !body_learned && fp->coll_data.ecbCurr_right.Y > 1.f)
    {
        body_offset = fp->coll_data.ecbCurr_right.Y;
        body_learned = 1;
    }
    preview_fh = 0;
    preview_sh = 0;

    if (tracked_air)
    {
        if (frame_in_state == 0)
            Fall_Learn(fp, ts);
        if (Lean_IsPlain(fp, ts, prev_vel.X))
            Ecb_Record(fp, ts, frame_in_state);
        if (capture_on && Is_Fall(ts))
            Lean_Log(fp, ts, frame_in_state);
        if (capture_on && Is_DoubleJump(ts))
            Dj_Log(fp, ts, frame_in_state);
        if (Tracked_IsUpB(ts))
            Upb_Record(fp, ts, frame_in_state);
        if (logging)
        {
            if (!stage_logged)
            {
                Log_Stage();
                stage_logged = 1;
            }
            Log_Frame(fp, ts, frame_in_state);
        }

        Floor_BuildCache();
        SimStart start;
        Sim_FromFighter(fp, ts, frame_in_state, &start);
        start.dodge_late = ts == TS_ESCAPEAIR && frame_in_state == 0 && dodge_late_ok;
        Predict(fp, &start, pred_live, BR_ALL);
        live_timer = event_vars->game_timer;
        Text_Prediction(pred_live);
        Text_Windows(pred_live);
        Text_Next(pred_live);
        Timing_Update(fp, pred_live, 0);

        if (prev_tracked_air)
            Window_Feedback(fp, ts);
        Window_Remember(pred_live);
        if (!prev_tracked_air || (ts != prev_ts && !Tracked_EndedInto(prev_ts, ts)))
            press_note = 0; // a new jump or press: the old note is stale
        if (prev_tracked_air && ts != prev_ts)
            Press_CheckMissed(fp, ts);
        if (!prev_tracked_air || Segment_InputChanged(fp, ts))
            Segment_Start(fp);
        else
            Segment_CheckDrift(fp, ts);
        Segment_AddActual(fp);

        live_visible = 1;
        ghost_visible = 0;
    }
    else if (airborne)
    {
        // in the air but in a state we don't predict (hitstun, side-B, ...)
        live_visible = 0;
        seg_valid = 0;
        sprintf(text_ai, "-");
    }
    else if (Ground_CanJump(sid) && !disturbed && !ghost_visible)
        Ground_Preview(fp);
    if (!tracked_air)
        Window_Forget();

    // nothing can be pressed from an airdodge or a helpless fall: a muted
    // waveland timer stops now, not at the touchdown
    if ((ts == TS_ESCAPEAIR || Tracked_IsHelpless(ts)) && cue_live[CUE_WL].dim)
        Cue_Finish(CUE_WL, PH_CUT, cue_live[CUE_WL].dim);

    if (squat_wd)
        Cue_Set(CUE_WL, squat_wd, 2, DODGE_RIGHT | DODGE_LEFT, 1, fp->phys.pos.X, fp->phys.pos.Y + 1.f);
    Cues_End();

    Ledge_Think(fp, sid);
    Drop_Think(fp, sid, prev_state_id);
    Dash_Learn(fp, sid, prev_state_id);
    Assist_Think(fp, sid);
    // dropping through a platform (Pass) falls as a fall does: Jump Timing
    // takes it as one, so its jumps show from the drop on
    if (sid == ASID_PASS && airborne && !disturbed)
        Jump_Update(fp, TS_FALL, 1);
    else
        Jump_Update(fp, ts, tracked_air);
    Jump_Ending(fp, sid, prev_state_id);
    Body_Flash(fp, sid);
    Slide_Update(fp);
    Pad_Record(PadGetEngine(fp->pad_index));
    int t_solve = OSGetTick();
    Jump_Solve(fp);
    Jump_Publish(fp);
    if (jt_active && jt_ground && jt_done && Options_Jump[JOPT_SHOW].val)
        Gl_Idle(fp, LR_BUDGET); // the hop library, in the frames the jump search leaves
#ifdef LL_GROUND_GUIDE
    if (!jt_active || jt_done)
        Guide_Solve(fp); // worked out once, in the frames the jump search leaves
#endif
    Ledge_Solve(fp, tracked_air ? LR_BUDGET_AIR : LR_BUDGET);
    float solve_ms = OSTicksToMicroseconds(OSGetTick() - t_solve) / 1000.f;
    if (solve_ms > perf_solve)
        perf_solve = solve_ms;

    // the jumpsquat and takeoff, to check the ground previews against
    if (sid == ASID_KNEEBEND && logging)
        Log_Squat(fp);

    prev_state_id = sid;
    prev_ts = ts;
    prev_tracked_air = tracked_air;
    prev_tilt_timer = (u8)fp->input.timer_lstick_tilt_y;
    prev_lock = fp->coll_data.u.ecb_bot_lock_frames;
    prev_wl_late = live_visible && Cues_Waveland() && pred_live->wl_late && pred_live->wl_first == 1;
    prev_vel = (Vec2){fp->phys.self_vel.X, fp->phys.self_vel.Y};
    Spec_Think(fp, sid, frame_in_state);
    Hover_Think(fp);
    if (hover_on)
        prev_vel = (Vec2){fp->phys.self_vel.X, 0};

    Drill_Think();
    // Finish only after the old frame's learning and landing resolution;
    // All may then reset caches for the next trial without contamination.
    if (script_trace_end_pending)
        Script_End();
}
