/* landinglab/jump_tracking.c
 *
 * Holds jump timing each frame: anchoring, run speed, extending and carrying
 * the search, following a jump.
 *
 * Part of Landing Gear: src/landinglab.c includes it, in order, into one
 * translation unit, so it sees everything included before it (and mex.h,
 * events.h, landinglab_sim.h).
 *
 * Uses: option_values, menus_practice, tracking_state, segments, hud_text,
 * timers, meter, meter_placement, scripts, jump_timing.
 * Refers ahead, through forward declarations, to: hop_library, jump_search.
 */

// The jumps still ahead, and the best of them of the kind Kind asks for.
static int jt_squat; // in the jumpsquat this frame (Jump_Update)
static void Jump_Prune(int e)
{
    for (int k = 0; k < 2; k++)
    {
        int n = 0;
        for (int i = 0; i < jt_best_n[k]; i++)
        {
            // a hop with a double jump stays through the jumpsquat, once
            // the hop is pressed; until then it goes when its hop has gone
            // by, a frame late in case the squat shows a frame after the
            // press (repro3, 2026-10-14: walking, the route up was one whose
            // hop was 4-5 frames gone, kept for its double jump)
            JumpRoute *r = &jt_best[k][i];
            int hop_dj = (r->hop == GT_SH || r->hop == GT_FH) && r->hj >= 0;
            if (hop_dj ? r->dj > e && (jt_squat || r->hp >= e) : Jump_First(r) > e)
                jt_best[k][n++] = *r;
        }
        jt_best_n[k] = n;
    }
}

// The route r (its frames counted from anchor) among the ones found now
// that are shown, or 0.
#define JT_SWITCH 3
#define JT_SETTLE 4 // ... in the first frames after it comes up (Warning frames ahead)
static int jt_route_abs; // the anchor the shown route's frames count from
static JumpRoute *Jump_Find(JumpRoute *r, int anchor)
{
    int kinds = Options_Jump[JOPT_KIND].val, shift = anchor - jt_anchor;
    for (int k = 0; k < 2; k++)
    {
        if ((k == 0 && kinds == ROUTES_AI) || (k == 1 && kinds == ROUTES_NIL))
            continue;
        for (int i = 0; i < jt_best_n[k]; i++)
        {
            JumpRoute *x = &jt_best[k][i];
            if (x->kind == r->kind && x->stick == r->stick && x->aerial == r->aerial && x->hop == r->hop &&
                x->dj == r->dj + shift && (r->kind != LAND_AI || x->press == r->press + shift))
                return x;
        }
    }
    return 0;
}

// The best jump that may come up now: one whose jump is still Warning
// frames off. One found in time but not shown then (another was better)
// would otherwise pop up late, once the one shown went by (Stephen, 0.8.5).
static JumpRoute *Jump_Pick(int e)
{
    int kinds = Options_Jump[JOPT_KIND].val;
    JumpRoute *best = 0;
    for (int k = 0; k < 2; k++)
    {
        if ((k == 0 && kinds == ROUTES_AI) || (k == 1 && kinds == ROUTES_NIL))
            continue;
        for (int i = 0; i < jt_best_n[k]; i++)
        {
            JumpRoute *x = &jt_best[k][i];
            // a hop from the ground has no Warning cutoff: it's pressed when
            // the player chooses, so a hop now is as useful moving as standing
            int ground_hop = jt_ground && (x->hop == GT_SH || x->hop == GT_FH);
            if (Jump_First(x) - e - 1 < Jump_Lead() && !x->seen && !(jt_ground && gt_still) && !ground_hop)
                continue;
            if (!best || Jump_Better(x, best))
                best = x;
            break; // best first: the rest of this kind are worse
        }
    }
    return best;
}

// Falcon with the fall as the anchor: the fall before the jump, stepped
// once with the stick held, and every frame of it kept. Starts the search
// over.
static void Jump_Anchor(FighterData *fp, int ts)
{
    jt_active = 1;
    jt_ground = 0;
    jt_anchor = event_vars->game_timer;
    jt_target = Options_Jump[JOPT_TARGET].val;
    Sim_FromFighter(fp, ts, frame_in_state, &jt_start);
    jt_stick_x = fp->input.lstick.X;
    jt_stick_down = Stick_Down(fp->input.lstick.Y);
    jt_stick_drop = Stick_Drop(fp->input.lstick.Y);

    // how far down is lost: under the lowest floor
    float low = 100000.f;
    for (int i = 0; i < floor_num; i++)
    {
        FloorLine *f = &floor_cache[i];
        float y = f->y0 < f->y1 ? f->y0 : f->y1;
        if (y < low)
            low = y;
    }
    jt_bottom = floor_num > 0 ? low - JT_BOTTOM : -100000.f;

    SimState s;
    SimStep step;
    Sim_Init(&jt_start, &s);
    jt_cache[0] = s;
    jt_cache_n = 1;
    for (int d = 1; d < JT_D; d++)
    {
        Sim_Step(fp, &jt_start, &s, -1, 0, &step);
        if (step.landed || step.ceiling)
            break;
        jt_cache[d] = s;
        jt_cache_n = d + 1;
    }

    jt_next = 0;
    jt_done = 0;
    jt_found = 0;
    jt_steps = 0;
    jt_tries = 0;
    jt_best_n[0] = 0;
    jt_best_n[1] = 0;
    jt_have = 0;
}

// The same from the ground: where Falcon goes as he stands, walks or runs
// with the stick held as it is, until the floor under him ends.
static void Gl_Anchor(FighterData *fp);
static int gl_builds, gl_pass, gl_skip;
static int Gt_Variant(int i, JumpCand *c);

// A dash or run's speed change this frame (ftCo_Dash_Phys, ftCo_Run_Phys,
// ftCommon_CalcGroundAccel_DashRun): toward the stick's share of the run
// speed, eased off as a run nears it
static float DashRun_Accel(FighterData *fp, float v, float stick, int run)
{
    float b = fp->attr.dashrun_acceleration_b;
    float accel = stick * fp->attr.dashrun_acceleration_a + (stick > 0 ? b : -b);
    float target = stick * fp->attr.dashrun_terminal_velocity;
    float friction = fp->attr.ground_friction * common_dashrun_friction;
    float vmax = fp->attr.grounded_max_horizontal_velocity;
    if (target == 0)
        return fabs(friction) > fabs(v) ? -v : v > 0 ? -friction : friction;
    if (run)
    {
        float f = v / target;
        if (f > 0 && f < 1)
            accel *= (1 - f) * common_run_taper;
    }
    if (v * accel < 0)
        return accel;
    if (accel > 0)
    {
        if (v + accel > target)
        {
            accel = -friction;
            if (v + accel < target)
                accel = target - v;
            if (v + accel > vmax)
                accel = vmax - v;
        }
    }
    else if (v + accel < target)
    {
        accel = friction;
        if (v + accel > target)
            accel = target - v;
        if (v + accel < -vmax)
            accel = -vmax - v;
    }
    return accel;
}

// The dash animation frame a held dash turns into a run on, learned from
// the first one seen (the dash's script sets the flag that allows it)
static float dash_run_frame = -1;
static float dash_last_frame = -1;
static void Dash_Learn(FighterData *fp, int sid, int prev_sid)
{
    if (prev_sid == ASID_DASH && sid == ASID_RUN && dash_last_frame >= 0 && dash_run_frame < 0)
    {
        dash_run_frame = dash_last_frame + 1;
        if (cue_log)
            OSReport("LLDASH run from dash frame %.0f\n", dash_run_frame);
    }
    dash_last_frame = sid == ASID_DASH ? fp->state.frame : -1;
}

// Frames from now until a dash's speed-up eases off as a run's does
static int Dash_RunAt(FighterData *fp, int sid)
{
    if (sid == ASID_RUN)
        return 1;
    if (sid != ASID_DASH || fp->input.lstick.X * fp->facing_direction < common_run_stick)
        return JT_D; // stays a dash
    if (fp->ftcmd_var.flag0)
        return 1;
    float at = dash_run_frame;
    if (at < 0)
    {
        Figatree *anim = fp->figatree_curr;
        if (!anim)
            return JT_D;
        at = anim->frame_num; // a guess until one is seen
    }
    int k = (int)(at - fp->state.frame + 0.999f);
    return k < 1 ? 1 : k;
}

static void Jump_AnchorGround(FighterData *fp, int sid)
{
    jt_active = 1;
    jt_ground = 1;
    jt_anchor = event_vars->game_timer;
    jt_target = Options_Jump[JOPT_TARGET].val;
    gt_sid = sid;
    Floor_BuildCache();
    memset(&jt_start, 0, sizeof(jt_start));
    jt_start.pos = (Vec2){fp->phys.pos.X, fp->phys.pos.Y};
    jt_start.facing = fp->facing_direction;
    jt_stick_x = fp->input.lstick.X;
    jt_stick_down = Stick_Down(fp->input.lstick.Y);
    jt_stick_drop = Stick_Drop(fp->input.lstick.Y);

    float low = 100000.f;
    for (int i = 0; i < floor_num; i++)
    {
        FloorLine *f = &floor_cache[i];
        float y = f->y0 < f->y1 ? f->y0 : f->y1;
        if (y < low)
            low = y;
    }
    jt_bottom = floor_num > 0 ? low - JT_BOTTOM : -100000.f;

    float x = fp->phys.pos.X, y = fp->phys.pos.Y, v = fp->phys.self_vel_ground.X;
    // no stick, or a state the stick doesn't drive (a waveland's slide):
    // friction slows him (ft_80084F3C)
    int driven = sid >= ASID_WALKSLOW && sid <= ASID_RUNBRAKE && sid != ASID_RUNBRAKE;
    // a dash or run speeds up toward its run speed, stick held or not
    int dashrun = sid == ASID_DASH || sid == ASID_RUN;
    int brake = sid == ASID_RUNBRAKE; // ftCo_RunBrake_Phys: the dash's friction, not the coast's (0.9.2 test)
    int coast = !dashrun && !brake && (fabs(jt_stick_x) < JT_HOP_STILL || !driven);
    int run_at = dashrun ? Dash_RunAt(fp, sid) : JT_D;
    int off_end = 0;
    gt_steps[0] = (GroundStep){x, y, v};
    gt_steps_n = 1;
    for (int d = 1; d < JT_D; d++)
    {
        if (dashrun)
            v += DashRun_Accel(fp, v, jt_stick_x, d >= run_at);
        else if (brake)
            v += DashRun_Accel(fp, v, 0, 0);
        else if (coast)
        {
            float friction = fp->attr.ground_friction;
            if (fabs(v) > fp->attr.walk_maximum_velocity)
                friction *= common_run_friction;
            v += fabs(friction) > fabs(v) ? -v : v > 0 ? -friction : friction;
        }
        x += v;
        Vec2 spot;
        float x0, x1;
        if (!Floor_Under(x, y + 3.f, &spot, &x0, &x1) || fabs(spot.Y - y) > 3.f)
        {
            off_end = 1; // off the end of the floor
            break;
        }
        y = spot.Y;
        gt_steps[d] = (GroundStep){x, y, v};
        gt_steps_n = d + 1;
    }
    jt_cache_n = gt_steps_n;
    gt_still = coast && fabs(v) < 0.0001f && fabs(gt_steps[0].v) < 0.0001f;
    if (gt_still)
        jt_cache_n = 1;

    // off the end: the frame he goes over keeps the ground's speed as the
    // air speed with none down, the fall's ECB under a fresh bottom lock
    // (as Sim_GroundJumpAt's takeoff), and the double jump is the one left
    gt_edge = -1;
    int walk = sid == ASID_WALKSLOW || sid == ASID_WALKMIDDLE || sid == ASID_WALKFAST; // walks teeter at the edge
    if (off_end && !gt_still && !walk && fabs(v) > 0.0001f)
    {
        gt_edge = gt_steps_n;
        SimStart *f = &gt_fall;
        memset(f, 0, sizeof(*f));
        f->pos = (Vec2){x, y};
        // ftCo_Fall_Enter clamps the speed to the drift max (ftCommon_ClampAirDrift):
        // Marth's 1.8 run goes over at 0.9 (2026-10-14 cast run)
        float max = fp->attr.aerial_drift_max;
        f->vel = (Vec2){v > max ? max : v < -max ? -max : v, 0};
        f->stick_x = jt_stick_x;
        f->stick_y = fp->input.lstick.Y;
        f->facing = fp->facing_direction;
        Ecb_FromFighter(fp, &f->ecb);
        if (Ecb_Get(TS_FALL, 0)->has_shape)
            Ecb_FromSample(Ecb_Get(TS_FALL, 0), fp->facing_direction, &f->ecb);
        f->ecb.bottom = 0;
        Ecb_Fix(&f->ecb);
        f->ts = TS_FALL;
        f->len = 0; // a fall loops, never ending by itself (as Sim_FromFighter)
        f->tilt_timer = LL_TIMER_MAX;
        f->trigger_timer = LL_TIMER_MAX;
        f->ecb_lock = 9;
        f->skip_line = -1;
        f->jumps = 1; // leaving the ground uses one (ftCommon_8007D5D4)
        SimState s;
        SimStep step;
        Sim_Init(f, &s);
        jt_cache[gt_edge] = s;
        jt_cache_n = gt_edge + 1;
        for (int d = gt_edge + 1; d < JT_D; d++)
        {
            Sim_Step(fp, f, &s, -1, 0, &step);
            if (step.landed || step.ceiling)
                break;
            jt_cache[d] = s;
            jt_cache_n = d + 1;
        }
    }
    Gl_Anchor(fp);

    jt_next = 0;
    jt_done = 0;
    jt_found = 0;
    jt_steps = 0;
    jt_tries = 0;
    jt_best_n[0] = 0;
    jt_best_n[1] = 0;
    jt_have = 0;
}

// Standing, walking, dashing or running: the states a jump comes straight
// out of.
static int Jump_GroundState(int sid)
{
    return sid >= ASID_WAIT && sid <= ASID_RUNBRAKE;
}

// Is Falcon in a fall the search is for: airborne with the double jump
// left, in a fall or the first jump, not hit, on a ledge route or in
// Assist.
static int Jump_Eligible(FighterData *fp, int ts, int tracked_air)
{
    if (!Options_Jump[JOPT_SHOW].val)
        return 0;
    if (fp->phys.air_state == 0)
    {
        // standing still (the standstill plan), or moving toward the floor's
        // end (a run-off: run, dash or a waveland's slide)
        int sid = fp->state_id;
        int moving = fabs(fp->phys.self_vel_ground.X) >= 0.0001f;
        if (JT_HOP_SETTING == HOP_OFF || hang_ledge >= 0 || route_active || Options_Ledge[LOPT_ASSIST].val)
            return 0;
        if (!moving)
            return Jump_GroundState(sid) && fabs(fp->input.lstick.X) < JT_HOP_STILL;
        return Jump_GroundState(sid) || sid == ASID_LANDINGFALLSPECIAL || sid == ASID_LANDING;
    }
    if (!tracked_air)
        return 0;
    if (ts != TS_FALL && ts != TS_JUMPF && ts != TS_JUMPB)
        return 0;
    if (fp->jump.jumps_used >= fp->attr.max_jumps)
        return 0;
    return !route_active && hang_ledge < 0 && !Options_Ledge[LOPT_ASSIST].val;
}

// The player did something the anchor's fall didn't assume (Segment_
// InputChanged's checks, with a little give on the stick's x so a hand at
// rest doesn't keep starting the search over).
static int Jump_InputChanged(FighterData *fp, int ts)
{
    if (fabs(fp->input.lstick.X - jt_stick_x) > JT_STICK_TOL)
        return 1;
    if (Stick_Down(fp->input.lstick.Y) != jt_stick_down)
        return 1;
    if (Stick_Drop(fp->input.lstick.Y) != jt_stick_drop)
        return 1;
    if ((u8)fp->input.timer_lstick_tilt_y < jt_prev_tilt) // a new flick
        return 1;
    if (ts != jt_prev_ts && !Tracked_EndedInto(jt_prev_ts, ts))
        return 1;
    return 0;
}

// Falcon is not where the anchor's fall has him (Segment_CheckDrift's
// check): wind, a push, a model gap.
static int Jump_Drifted(FighterData *fp, int e)
{
    if (e < 1 || e >= jt_cache_n)
        return 0;
    return fabs(fp->phys.pos.X - jt_cache[e].x) > JT_DRIFT_TOL || fabs(fp->phys.pos.Y - jt_cache[e].y) > JT_DRIFT_TOL;
}

// Too few of the anchor's jump frames are left, but Falcon is still on its
// fall: anchor again here to search the frames past it, and keep the jumps
// already found, counted from the new anchor, so the one shown doesn't
// drop out while the new search runs.
static void Jump_Log(void);
static void Jump_Extend(FighterData *fp, int ts, int e)
{
    if (!jt_done && cue_log)
        Jump_Log();
    static JumpRoute keep[2][JT_BEST];
    int keep_n[2];
    for (int k = 0; k < 2; k++)
    {
        keep_n[k] = 0;
        for (int i = 0; i < jt_best_n[k]; i++)
        {
            JumpRoute r = jt_best[k][i];
            if (r.dj <= e)
                continue;
            r.d -= e;
            r.dj -= e;
            if (r.hop)
                r.hp -= e;
            r.land -= e;
            r.act -= e;
            if (r.press >= 0)
                r.press -= e;
            keep[k][keep_n[k]++] = r;
        }
    }
    int had = jt_have;
    if (jt_ground)
        Jump_AnchorGround(fp, fp->state_id);
    else
        Jump_Anchor(fp, ts);
    for (int k = 0; k < 2; k++)
    {
        for (int i = 0; i < keep_n[k]; i++)
            jt_best[k][i] = keep[k][i];
        jt_best_n[k] = keep_n[k];
    }
    // the one shown stays up, however near its jump now is: anchoring
    // again forgot it, and the Warning cutoff hid it, so a double jump due
    // more than the extension away vanished before its frame (FoD's air
    // routes, 2026-10-14)
    jt_have = had;
    JumpRoute *cur = had ? Jump_Find(&jt_route, jt_route_abs) : 0;
    if (cur)
        cur->seen = 1;
}

// A standstill plan with a double jump, carried into the air: on the
// hop's first frames the search hasn't got to its jump yet, so the plan's
// jump is tried first, as the air search counts it.
static void Jump_Seed(FighterData *fp, int ts)
{
    if (!gt_plan_ok || (ts != TS_JUMPF && ts != TS_JUMPB))
        return;
    gt_plan_ok = 0;
    if (gt_plan.hop != (gt_squat_short ? GT_SH : GT_FH))
        return;
    JumpCand c = {gt_plan.hj - frame_in_state, gt_plan.stick, gt_plan.hold, 0, -1, 0};
    if (c.d < 0 || c.d >= jt_cache_n)
        return;
    JumpRoute found[JT_TRY];
    int found_n;
    Jump_Try(fp, &c, found, &found_n, 0, 0, 0);
    for (int i = 0; i < found_n; i++)
    {
        found[i].seen = 1; // shown parked before the takeoff
        if (Jump_OnTarget(&found[i]))
            Jump_Insert(jt_best[found[i].kind == LAND_AI], &jt_best_n[found[i].kind == LAND_AI], &found[i]);
    }
    if (cue_log)
        OSReport("LLJUMP seed %d d %d stick %d found %d\n", event_vars->game_timer, c.d, c.stick, found_n);
}

// The stick moved or Falcon left the anchor's fall: anchor again here, but
// first try the jumps already found once more from the new fall, so one that
// still works stays up instead of vanishing for Warning frames while the
// search catches up (Stephen, 0.8.5). Drift moves Falcon sideways; how high
// he is on each frame doesn't change, so the jump usually still works.
#define JT_CARRY 2 // jumps of each kind tried again, best first
static int jt_carry_steps; // what that cost this frame, taken off the search's budget
static void Jump_Carry(FighterData *fp, int ts, int e)
{
    int steps0 = sim_steps;
    JumpCand keep[2 * JT_CARRY + 1];
    u8 keep_seen[2 * JT_CARRY + 1]; // the one on screen: it stays, however near its jump now is
    int n = 0, now = event_vars->game_timer;
    for (int k = -1; k < 2; k++)
    {
        for (int i = 0; i < (k < 0 ? 1 : jt_best_n[k]) && i < JT_CARRY; i++)
        {
            JumpRoute *r = k < 0 ? &jt_route : &jt_best[k][i];
            if (k < 0 && !jt_have)
                break;
            if (r->hop)
                continue;
            // its frames count from its own anchor: the shown one's may be older
            int d = r->d + (k < 0 ? jt_route_abs : jt_anchor) - now;
            if (d < 0)
                continue;
            int dup = 0;
            for (int j = 0; j < n; j++)
                dup |= keep[j].d == d && keep[j].stick == r->stick && keep[j].hold == r->hold;
            if (!dup)
            {
                keep_seen[n] = k < 0;
                keep[n++] = (JumpCand){d, r->stick, r->hold, 0, -1, 0};
            }
        }
    }
    int had = jt_have;
    Jump_Anchor(fp, ts);
    jt_have = had; // so the one shown is found again among them, not picked afresh
    int found_n = 0, total = 0;
    for (int j = 0; j < n; j++)
    {
        if (keep[j].d >= jt_cache_n)
            continue;
        JumpRoute found[JT_TRY];
        Jump_Try(fp, &keep[j], found, &found_n, 0, 0, 0);
        for (int i = 0; i < found_n; i++)
        {
            if (!Jump_OnTarget(&found[i]))
                continue;
            found[i].seen = keep_seen[j];
            Jump_Insert(jt_best[found[i].kind == LAND_AI], &jt_best_n[found[i].kind == LAND_AI], &found[i]);
            total++;
        }
    }
    jt_carry_steps = sim_steps - steps0;
    if (cue_log)
        OSReport("LLJUMP carry %d e %d tried %d kept %d steps %d\n", now, e, n, total, jt_carry_steps);
}

// Whether the double jump came out on the frame the jump lane showed.
static void Jump_Ending(FighterData *fp, int sid, int prev_sid)
{
    if (jt_end_age >= 0 && ++jt_end_age > LL_FADE + 2)
        jt_end_age = -1;
    int jts = Tracked_Index(sid); // Kirby's and Jigglypuff's air jumps are their own states
    int dj = (Is_DoubleJump(jts) || Is_MultiJump(jts)) && prev_sid != sid;
    if (jt_due >= 0 && (dj || jt_due == 0))
    {
        jt_end_age = 0;
        jt_end_hit = dj && jt_due == 0;
        jt_end_off = dj ? jt_due : -1; // frames early, -1 late
        strcpy(jt_end_label, jt_due_label);
        OSReport("LLJUMPEND %d %s %s %d\n", event_vars->game_timer, jt_end_label,
                 jt_end_hit ? "hit" : dj ? "early" : "late", jt_end_off);
    }
    jt_due = -1;
}

// The ending on its lane's row.
static void Row_JumpEnd(MeterRow *row)
{
    int a = jt_end_age;
    if (jt_end_hit)
    {
        if (a < LL_BURST)
            row->burst = a;
        if (a < LL_GHOST)
            Row_Ghost(row, a, 1);
        if (a < 4)
            row->flash = a;
        sprintf(row->info, "hit");
    }
    else
    {
        row->implode = a;
        // a press too soon is a miss; none on the frame is a skip, gray
        row->implode_tone = jt_end_off > 0 ? TONE_MISS : TONE_SKIP;
        if (jt_end_off > 0)
            sprintf(row->info, "early %d", jt_end_off);
        else
            sprintf(row->info, "late");
    }
}

static void Jump_KeepHop(FighterData *fp, JumpRoute *was, int d); // at the end, after the candidate counts

// Each frame, before the search: is Falcon in a fall to search, and is the
// anchor still right for it. It is taken again when he comes back to a fall
// after leaving one, when the stick or his path leaves what it assumed, and
// when too few of its jump frames are left.
static void Jump_Update(FighterData *fp, int ts, int tracked_air)
{
    int e = event_vars->game_timer - jt_anchor;
    jt_squat = fp->state_id == ASID_KNEEBEND;
    if (fp->state_id == ASID_KNEEBEND)
        gt_squat_short = fp->state_var.state_var1 != 0; // let go of the jump: a short hop
    if (fp->state_id == ASID_KNEEBEND && jt_active && jt_ground && (gt_still || (jt_have && jt_route.hop == GT_SH) ||
                                                                     (jt_have && jt_route.hop == GT_FH)))
    {
        // the jumpsquat: the standstill plan stays parked, and a hop on the
        // move keeps counting, until the takeoff
        jt_prev_ts = ts;
        jt_prev_tilt = (u8)fp->input.timer_lstick_tilt_y;
        return;
    }
    if (!Jump_Eligible(fp, ts, tracked_air))
    {
        // why not, once a second, while it's on and Falcon is in the air
        if (Options_Jump[JOPT_SHOW].val && fp->phys.air_state == 1 && cue_log && event_vars->game_timer % 60 == 0)
            OSReport("LLJUMPX %d not searching: tracked %d ts %d jumps %d of %d route %d ledge %d assist %d\n",
                     event_vars->game_timer, tracked_air, ts, fp->jump.jumps_used, fp->attr.max_jumps, route_active,
                     hang_ledge, Options_Ledge[LOPT_ASSIST].val);
        jt_active = 0;
        jt_have = 0;
    }
    else if (fp->phys.air_state == 0)
    {
        // from the ground: anchored again when the stick, the state or
        // the speed leaves what it assumed, or the hop setting changes
        int sid = fp->state_id;
        int at = gt_still ? 0 : e;
        int same = jt_active && jt_ground && e >= 0 && at < gt_steps_n &&
                   fabs(fp->input.lstick.X - jt_stick_x) <= JT_STICK_TOL &&
                   (sid == gt_sid || (gt_sid == ASID_DASH && sid == ASID_RUN)) &&
                   fabs(fp->phys.pos.X - gt_steps[at].x) <= JT_DRIFT_TOL &&
                   fabs(fp->phys.pos.Y - gt_steps[at].y) <= 1.f && Options_Jump[JOPT_TARGET].val == jt_target &&
                   JT_HOP_SETTING == jt_hop;
        if (!same)
        {
            // a dash speeds up, so its run-off is anchored again most
            // frames: the jump shown keeps its frame into the fall, tried
            // again from the new edge, so it stays up instead of flickering
            if (cue_log && jt_active && jt_ground && at < gt_steps_n)
                OSReport("LLJUMPG %d reanchor sid %d was %d e %d dx %.4f v %.4f model %.4f\n", event_vars->game_timer,
                         sid, gt_sid, e, fp->phys.pos.X - gt_steps[at].x, fp->phys.self_vel_ground.X, gt_steps[at].v);
            int had = jt_have && jt_active && jt_ground && !gt_still && gt_edge >= 0 && !jt_route.hop;
            // ... and a hop shown (standing or moving) is tried again on its
            // own press frame from the new anchor, so it stays up while he
            // walks instead of dropping out each time the anchor moves
            int had_hop = jt_have && jt_active && jt_ground && (jt_route.hop == GT_SH || jt_route.hop == GT_FH);
            int hop_at = had_hop ? (gt_still ? event_vars->game_timer + 1 : jt_route_abs + jt_route.hp) : 0;
            JumpRoute was = jt_route;
            int j = (jt_route_abs + was.d) - (jt_anchor + gt_edge);
            Jump_AnchorGround(fp, sid);
            if (had_hop && !gt_still)
                Jump_KeepHop(fp, &was, hop_at - 1 - jt_anchor);
            if (had && gt_edge >= 0 && j >= 0 && gt_edge + j < jt_cache_n)
            {
                JumpCand c = {gt_edge + j, was.stick, was.hold, GT_RUNOFF, -1, 0};
                JumpRoute found[JT_TRY];
                int found_n;
                Jump_Try(fp, &c, found, &found_n, 0, 0, 0);
                for (int i = 0; i < found_n; i++)
                {
                    if (!Jump_OnTarget(&found[i]))
                        continue;
                    found[i].seen = 1;
                    Jump_Insert(jt_best[found[i].kind == LAND_AI], &jt_best_n[found[i].kind == LAND_AI], &found[i]);
                }
                jt_have = 1;
            }
        }
        else if (!gt_still && e >= JT_REANCHOR)
            Jump_Extend(fp, ts, e);
        jt_hop = JT_HOP_SETTING;
    }
    else if (!jt_active || jt_ground || e < 0 || Jump_InputChanged(fp, ts) || Jump_Drifted(fp, e) ||
             Options_Jump[JOPT_TARGET].val != jt_target)
    {
        if (Log_Level() >= LOG_ALL)
        {
            const char *why = !jt_active ? "new" : jt_ground ? "takeoff" : e < 0 ? "time" : Jump_InputChanged(fp, ts) ? "input"
                              : Jump_Drifted(fp, e) ? "drift" : "target";
            float dx = !jt_ground && e >= 0 && e < jt_cache_n ? fp->phys.pos.X - jt_cache[e].x : 0;
            float dy = !jt_ground && e >= 0 && e < jt_cache_n ? fp->phys.pos.Y - jt_cache[e].y : 0;
            OSReport("LLJUMPA %d air %s e %d tried %d dx %.4f dy %.4f ts %d prev %d\n", event_vars->game_timer, why, e,
                     jt_tries, dx, dy, ts, jt_prev_ts);
        }
        // off the floor's end: the run-off's jumps are carried into the fall
        // a hop isn't a run-off, even with a floor's end ahead: its plan's
        // double jump is seeded as standing still's is (Ice Climbers lost
        // their SH+DJ route on the takeoff, 2026-10-14)
        int hopped = ts == TS_JUMPF || ts == TS_JUMPB;
        int runoff = jt_active && jt_ground && !gt_still && gt_edge >= 0 && !hopped;
        int seed = (jt_ground && !runoff) || !jt_active;
        if (!seed && e >= 0)
            Jump_Carry(fp, ts, e);
        else
            Jump_Anchor(fp, ts);
        if (seed)
            Jump_Seed(fp, ts);
    }
    else if (e >= Jump_Reanchor())
        Jump_Extend(fp, ts, e);
    jt_prev_ts = ts;
    jt_prev_tilt = (u8)fp->input.timer_lstick_tilt_y;
}

// The facing Falcon has as he presses the aerial: a jump backwards turns
// him around.
static float Jump_Facing(JumpRoute *r)
{
    float face = jt_start.facing > 0 ? 1.f : -1.f;
    if (r->hop && r->hj < 0)
        return face; // a hop keeps the facing it has on the ground
    return jt_stick_xy[r->stick][0] * face > -common_jump_back_stick ? face : -face;
}

// The test scripts' "follow": press what the shown route asks on its frames
// (the hop, the double jump with its stick, the aerial), on top of the
// step's own inputs, so LLJUMPEND and the landing show whether the route
// works as shown. A route's frame k is game frame anchor + k.
static int follow_fh_until = -1; // a full hop holds jump through the squat
static int follow_hold_x = -1000; // after the hop or double jump: the stick's x kept (-1000: the step's own)
static int follow_set_t = -100;
static int follow_aerial_t = -1, follow_aerial; // the aerial, kept from the jump (the route goes once the DJ is used)
static float follow_face;
static void Jump_Follow(ScriptInput *in)
{
    int t = event_vars->game_timer;
    FighterData *fp = Fighter_GetGObj(0)->userdata;
    if (t == follow_aerial_t)
    {
        if (follow_aerial == TS_AIRN)
        {
            in->buttons |= HSD_BUTTON_A;
            in->lx = 0; // a neutral A
            in->ly = 0;
        }
        else
        {
            in->cx = follow_aerial == TS_AIRF ? (s8)(80 * follow_face) : follow_aerial == TS_AIRB ? (s8)(-80 * follow_face) : 0;
            in->cy = follow_aerial == TS_AIRHI ? 80 : follow_aerial == TS_AIRLW ? -80 : 0;
        }
        OSReport("LLFOLLOW %d aerial %s\n", t, tracked_state_names[follow_aerial]);
        follow_aerial_t = -1;
        return;
    }
    if (follow_fh_until >= t)
        in->buttons |= HSD_BUTTON_X;
    // back on the ground after it: the step's own stick again
    if (follow_hold_x != -1000 && fp->phys.air_state == 0 && fp->state_id != ASID_KNEEBEND && t > follow_set_t + 1)
        follow_hold_x = -1000;
    if (follow_hold_x != -1000)
        in->lx = follow_hold_x;
    // what follow sees, each time it changes (0.9.28 FoD: plans were found
    // but follow never pressed, so say whether a route was up and when)
    static int seen_state = -1, seen_abs = -1, seen_dj = -1;
    int state = (jt_active ? 1 : 0) | (jt_have ? 2 : 0) | (route_active ? 4 : 0) | (hang_ledge >= 0 ? 8 : 0);
    if (state != seen_state || (jt_have && (jt_route_abs != seen_abs || jt_route.dj != seen_dj)))
    {
        OSReport("LLFOLLOWSEE %d active %d have %d route %d hang %d ground %d still %d abs %d hop %d hp %d hj %d dj %d press %d kind %d e %d\n",
                 t, jt_active, jt_have, route_active, hang_ledge >= 0, jt_ground, gt_still, jt_route_abs, jt_route.hop,
                 jt_route.hp, jt_route.hj, jt_route.dj, jt_route.press, jt_route.kind, Jump_E());
        seen_state = state;
        seen_abs = jt_route_abs;
        seen_dj = jt_route.dj;
    }
    if (!Jump_Showing())
        return;
    JumpRoute *r = &jt_route;
    int abs = jt_route_abs;
    if (jt_ground && gt_still)
        abs = t - r->hp; // standing still: the plan's hop is always on the next frame
    if (r->kind == LAND_AI && (!r->hop || r->hj >= 0) && t == abs + r->dj)
    {
        follow_aerial_t = abs + r->press;
        follow_aerial = r->aerial;
        follow_face = Jump_Facing(r);
    }
    // once: a standing plan's hop is always "now", and jump held on is a full hop
    if ((r->hop == GT_SH || r->hop == GT_FH) && t == abs + r->hp && t > follow_set_t + 10)
    {
        // a hop with no double jump: its aerial counts from this press, once
        // (gifrun7: standing, the plan's hop stays "now" through the squat,
        // and Game & Watch's Bair slid a frame later each frame of it)
        if (r->kind == LAND_AI && r->hj < 0)
        {
            follow_aerial_t = abs + r->press;
            follow_aerial = r->aerial;
            follow_face = Jump_Facing(r);
        }
        in->buttons |= HSD_BUTTON_X;
        follow_fh_until = r->hop == GT_FH ? t + 6 : -1;
        if (!r->hold && r->hj < 0)
            follow_hold_x = 0;
        follow_set_t = t;
        OSReport("LLFOLLOW %d hop %s\n", t, r->hop == GT_SH ? "SH" : "FH");
    }
    // a double jump is pressed in the air only: on the ground it would be a
    // hop with the stick tilted (repro3: a route whose hop went by)
    if ((!r->hop || r->hj >= 0) && t == abs + r->dj && fp->phys.air_state == 1)
    {
        in->buttons |= HSD_BUTTON_X;
        in->lx = (s8)(jt_stick_xy[r->stick][0] * 80.f);
        in->ly = (s8)(jt_stick_xy[r->stick][1] * 80.f);
        follow_hold_x = r->hold ? in->lx : 0;
        follow_set_t = t;
        OSReport("LLFOLLOW %d dj %s\n", t, jt_stick_names[r->stick]);
    }
}

// The search's one line in the log: the best jump found for an anchor,
// frames counted from the anchor.
static void Jump_Log(void)
{
    JumpRoute *b = Jump_Pick(-1000); // the best of all, however near
    char buf[320];
    int n = sprintf(buf, "LLJUMP anchor %d %.3f %.3f %s%s%s at %d tried %d of %d steps %d target %d found %d edge %d", jt_anchor,
                    jt_start.pos.X, jt_start.pos.Y, jt_ground ? "ground " : "", jt_ground && gt_still ? "still " : "",
                    jt_done ? "done" : "partial", event_vars->game_timer, jt_tries, jt_next, jt_steps, jt_target, jt_found,
                    jt_ground ? gt_edge : -1);
    if (jt_ground)
        n += sprintf(buf + n, " lib pass %d skip %d builds %d", gl_pass, gl_skip, gl_builds);
    if (!b)
        sprintf(buf + n, " best none\n");
    else if (b->kind == LAND_AI)
        sprintf(buf + n, " best d %d stick %d hold %d aerial %d press %d-%d land %d on %d kind AI\n", b->d, b->stick, b->hold,
                b->aerial, b->press, b->press + b->press_w - 1, b->land, b->where);
    else
        sprintf(buf + n, " best d %d stick %d aerial none press none land %d on %d kind NIL\n", b->d, b->stick, b->land,
                b->where);
    OSReport("%s", buf);
}

// Search on: the candidates in the order of the jump frame, the soonest
// first, until the budget of simulated frames or of real time is spent. A
// jump whose frame has gone by is not tried.
// The candidates, in the order they're tried. In the air: by the frame of
// the jump, then its stick and whether its x is kept. On the ground while
// moving: by the frame of the hop, then short or full, and whether the
// stick is kept. Standing still, every frame's hop is the same, so the
// hops alone come first, then each hop with a double jump on each of its
// frames (the stick of the double jump, and keeping its x or not).
// Standing still: the hops alone, then for each double jump frame up to
// GT_HJ_MAX after the takeoff, both hops with the double jump straight up,
// or up and over either way holding that side. All of JT_D frames and every
// stick took over two seconds standing still (0.8.6 test: 599 of 924 tried
// in 94 frames), so the plan never came up.
#define GT_HJ_MAX 30
#define GT_STILL_STICKS 3
#define GT_STILL_CANDIDATES (4 + GT_HJ_MAX * 2 * GT_STILL_STICKS)

#define GT_STRIDE (GT_STILL_CANDIDATES + JT_STICKS * 2) // moving: a frame's hops, then its run-off double jumps

static int Jump_Total(void)
{
    return jt_ground && gt_still ? GT_STILL_CANDIDATES : jt_ground ? JT_D * GT_STRIDE : JT_CANDIDATES;
}

// Candidates per frame of the anchor, for skipping the ones already gone by.
static int Jump_Stride(void)
{
    return jt_ground ? GT_STRIDE : JT_STICKS * 2;
}

// A hop that was up, tried again from a new ground anchor at frame d (its
// press is d + 1): its variant found by matching the route, as Gt_Variant
// lays them out. Kept, and marked seen, only if it still lands as before.
static void Jump_KeepHop(FighterData *fp, JumpRoute *was, int d)
{
    if (d < 0 || d >= jt_cache_n || (gt_edge >= 0 && d >= gt_edge))
        return;
    for (int v = 0; v < GT_STILL_CANDIDATES; v++)
    {
        JumpCand c;
        if (Gt_Variant(v, &c) <= 0)
            continue;
        if (c.hop != was->hop || c.hold != was->hold || c.hj != was->hj || (c.hj >= 0 && c.stick != was->stick))
            continue;
        c.d = d;
        JumpRoute found[JT_TRY];
        int found_n;
        Jump_Try(fp, &c, found, &found_n, 0, 0, 0);
        for (int i = 0; i < found_n; i++)
        {
            if (!Jump_OnTarget(&found[i]) || found[i].kind != was->kind || found[i].aerial != was->aerial)
                continue;
            found[i].seen = 1;
            Jump_Insert(jt_best[found[i].kind == LAND_AI], &jt_best_n[found[i].kind == LAND_AI], &found[i]);
        }
        return;
    }
}
