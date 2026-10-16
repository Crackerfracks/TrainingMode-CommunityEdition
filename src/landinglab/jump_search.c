/* landinglab/jump_search.c
 *
 * Holds the jump search's candidates, solving, the chosen jump's text and
 * meter rows, the route and jump paths, and what the world shows of the
 * plan (its spot and lead).
 *
 * Part of Landing Gear: src/landinglab.c includes it, in order, into one
 * translation unit, so it sees everything included before it (and mex.h,
 * events.h, landinglab_sim.h).
 *
 * Uses: option_values, menus_practice, menus_display, tracking_state, rumble,
 * hud_text, drawing, meter, meter_placement, ledge_routes, jump_timing,
 * jump_tracking, hop_library.
 */

// Candidate i: 1 to try it, 0 to skip it, -1 when none after it can work.
static int Gt_Variant(int i, JumpCand *c)
{
    int want = JT_HOP_SETTING;
    c->hj = -1;
    c->neutral = 0;
    c->stick = JT_UP;
    c->v = i;
    {
        if (i < 4)
        {
            c->hop = i < 2 ? GT_SH : GT_FH;
            c->hold = i % 2;
            if (c->hold && fabs(jt_stick_x) < JT_HOP_STILL)
                return 0; // no stick to keep holding
        }
        else
        {
            static const u8 still_sticks[GT_STILL_STICKS] = {JT_UP, JT_UPLEFT, JT_UPRIGHT};
            i -= 4;
            c->hj = i / (2 * GT_STILL_STICKS); // the double jump's frame first, both hops at each
            i %= 2 * GT_STILL_STICKS;
            c->hop = i < GT_STILL_STICKS ? GT_SH : GT_FH;
            c->stick = still_sticks[i % GT_STILL_STICKS];
            c->hold = c->stick != JT_UP;
        }
        return !((c->hop == GT_SH && want == HOP_FULL) || (c->hop == GT_FH && want == HOP_SHORT));
    }
}

static int Jump_Cand(int i, JumpCand *c)
{
    c->neutral = 0;
    if (jt_ground && gt_still)
    {
        c->d = 0;
        return Gt_Variant(i, c);
    }
    c->hj = -1;
    if (jt_ground)
    {
        // moving: each frame's hops (every standstill variant), then its
        // double jumps off the floor's end
        c->d = i / GT_STRIDE;
        int v = i % GT_STRIDE;
        if (c->d >= jt_cache_n)
            return -1; // the floor ends, or the steps run out, before this frame
        if (v < GT_STILL_CANDIDATES)
        {
            if (gt_edge >= 0 && c->d >= gt_edge)
                return 0;
            return Gt_Variant(v, c);
        }
        v -= GT_STILL_CANDIDATES;
        if (gt_edge < 0 || c->d < gt_edge)
            return 0;
        // off the end: the double jump c->d - gt_edge frames into the fall
        c->stick = v / 2;
        c->hold = v % 2;
        c->hop = GT_RUNOFF;
        return !(c->stick == JT_UP && c->hold);
    }
    c->d = i / (JT_STICKS * 2);
    c->stick = (i / 2) % JT_STICKS;
    c->hold = i % 2;
    if (c->d >= jt_cache_n)
        return -1; // he lands before this frame
    c->hop = 0;
    return !(c->stick == JT_UP && c->hold); // up has no x to keep
}

// Search on: the candidates in order, the soonest first, until the budget
// of simulated frames or of real time is spent. A jump whose frame has
// gone by is not tried.
static void Jump_Solve(FighterData *fp)
{
    if (!jt_active || jt_done)
        return;
    int e = Jump_E();
    int total = Jump_Total();
    int start = sim_steps - jt_carry_steps;
    jt_carry_steps = 0;
    int t0 = OSGetTick();
    // the time limit holds for the ones laid out from the library too,
    // which simulate next to nothing (repro3e: all 8464 in one frame)
    while (jt_next < total && sim_steps - start < JT_BUDGET && OSTicksToMicroseconds(OSGetTick() - t0) < LR_TIME_US)
    {
        // on the ground the search starts at the next frame: a hop is the
        // player's to time, so it shows the soonest that works, as standing
        // still shows a hop now (Stephen, 0.9.28: moving, the Warning lead
        // pushed every hop 20 frames out, and none matched the standing plan)
        int lead = jt_ground ? 0 : Jump_Lead();
        if (!(jt_ground && gt_still) && jt_next < (e + lead) * Jump_Stride())
        {
            jt_next = (e + lead) * Jump_Stride();
            jt_twin = 0;
        }
        if (jt_next >= total)
            break;
        JumpCand c;
        int ok = Jump_Cand(jt_next, &c);
        // moving, a hop then a double jump straight up is tried twice: the
        // stick kept through the hop, then let go from the takeoff, the one
        // that drifts least (Stephen, 0.9.30: mostly vertical jumps are
        // what's played; walking, none was found at all)
        if (jt_twin)
        {
            c.neutral = 1;
            jt_twin = 0;
            jt_next++;
        }
        else if (ok > 0 && jt_ground && !gt_still && (c.hop == GT_SH || c.hop == GT_FH) && c.hj >= 0 && c.stick == JT_UP &&
                 fabs(jt_stick_x) >= JT_HOP_STILL)
            jt_twin = 1; // this one again next, let go
        else
            jt_next++;
        if (ok < 0)
        {
            jt_next = total;
            break;
        }
        if (!ok)
            continue;
        // moving, a hop the library has whole: its routes laid out from it,
        // to be predicted only if one comes up (Jump_Verify)
        if (jt_ground && !gt_still && (c.hop == GT_SH || c.hop == GT_FH))
        {
            JumpRoute direct[2 * GL_HEIGHTS];
            int dn = Gl_Direct(fp, &c, direct);
            if (dn >= 0)
            {
                gl_direct++;
                for (int i = 0; i < dn; i++)
                {
                    jt_found++;
                    if (Jump_OnTarget(&direct[i]))
                        Jump_Insert(jt_best[direct[i].kind == LAND_AI], &jt_best_n[direct[i].kind == LAND_AI], &direct[i]);
                }
                continue;
            }
        }
        // a hop: only when the library says it may land on something (moving,
        // its rows are built as needed; standing, once built in the background)
        if (jt_ground && (c.hop == GT_SH || c.hop == GT_FH) && !Gl_Worth(fp, &c, !gt_still))
        {
            gl_skip++;
            continue;
        }
        if (jt_ground && (c.hop == GT_SH || c.hop == GT_FH))
            gl_pass++;
        JumpRoute found[JT_TRY];
        int found_n;
        Jump_Try(fp, &c, found, &found_n, 0, 0, 0);
        for (int i = 0; i < found_n; i++)
        {
            jt_found++;
            if (!Jump_OnTarget(&found[i]))
                continue;
            Jump_Insert(jt_best[found[i].kind == LAND_AI], &jt_best_n[found[i].kind == LAND_AI], &found[i]);
        }
    }
    if (jt_next >= total)
    {
        jt_done = 1;
        if (cue_log)
            Jump_Log();
    }
}

// The route's path, when the one shown has changed.
static void Jump_Path(FighterData *fp)
{
    JumpRoute *o = &jt_path_of, *r = &jt_route;
    if (jt_path_anchor == jt_anchor && jt_path_num > 0 && o->d == r->d && o->stick == r->stick && o->hold == r->hold &&
        o->hop == r->hop && o->hj == r->hj)
        return;
    JumpRoute found[JT_TRY];
    int found_n;
    JumpCand c = {r->d, r->stick, r->hold, r->hop, r->hj, 0, r->neutral};
    Jump_Try(fp, &c, found, &found_n, jt_path, jt_path_bottom, &jt_path_num);
    if (jt_path_num > r->land + 1 && r->land > 0)
        jt_path_num = r->land + 1; // up to its own touchdown
    jt_path_of = *r;
    jt_path_anchor = jt_anchor;
}

// "DJ up in 6f, nair +11-13: AI": the first press counts from now, as the
// row's cells do; each press after it from the one before.
static void Jump_Text(JumpRoute *r, int e)
{
    char aerial[8];
    int until = r->dj - e - 1;
    char *t = text_next;
    const char *hop = r->hop == GT_SH ? "SH" : "FH";
    int hop_in = r->hp - e - 1;
    // the first press counts from now, each after it from the one before
    // (Stephen, 0.9.2: "SH now, DJ up f34, nair f45" read as one count)
    int from = e + 1; // the frame "now" is
    if (r->hop && hop_in > 0)
        t += sprintf(t, "%s in %df", hop, hop_in);
    else if (r->hop)
        t += sprintf(t, "%s now", hop);
    if (r->hop && r->hp > from)
        from = r->hp;
    if (r->hop && r->hj >= 0)
        t += sprintf(t, "%s, DJ %s +%d", r->neutral ? ", let go" : "", jt_stick_names[r->stick], r->dj - from);
    else if (r->hop)
        ;
    else if (until > 0)
        t += sprintf(t, "DJ %s in %df", jt_stick_names[r->stick], until);
    else
        t += sprintf(t, "DJ %s now", jt_stick_names[r->stick]);
    if ((!r->hop || r->hj >= 0) && r->dj > from)
        from = r->dj;
    if (r->kind == LAND_AI)
    {
        sprintf(aerial, "%s", tracked_state_names[r->aerial]);
        aerial[0] |= 0x20; // lower case
        int first = r->press - from;
        if (r->press_w > 1)
            t += sprintf(t, ", %s +%d-%d: AI", aerial, first, first + r->press_w - 1);
        else
            t += sprintf(t, ", %s +%d: AI", aerial, first);
    }
    else
        sprintf(t, ": NIL");
    if (r->hop && r->hj < 0)
        sprintf(text_steps, r->hold ? "keep the stick as it is" : "then let the stick go");
    else if (r->hold && r->stick != JT_UP)
        sprintf(text_steps, "then hold %s", r->stick == JT_LEFT || r->stick == JT_UPLEFT ? "left" : "right");
    else
        sprintf(text_steps, "then let the stick go");
    next_kind = r->kind == LAND_AI ? CUE_AI : CUE_NIL;
}

// A route laid out from the hop library, predicted for real: the
// prediction's own routes take its place, or it goes if it doesn't hold up.
// Moves the lists around, so pointers into them go stale.
static void Jump_Real(FighterData *fp, JumpRoute *b)
{
    JumpRoute was = *b;
    int k = was.kind == LAND_AI;
    // out of the list first, then the prediction's own routes in
    int n = 0;
    for (int i = 0; i < jt_best_n[k]; i++)
        if (&jt_best[k][i] != b)
            jt_best[k][n++] = jt_best[k][i];
    jt_best_n[k] = n;
    JumpCand c = {was.d, was.stick, was.hold, was.hop, was.hj, 0, was.neutral};
    JumpRoute found[JT_TRY];
    int found_n, kept = 0;
    Jump_Try(fp, &c, found, &found_n, 0, 0, 0);
    for (int i = 0; i < found_n; i++)
    {
        if (!Jump_OnTarget(&found[i]))
            continue;
        found[i].seen = was.seen;
        Jump_Insert(jt_best[found[i].kind == LAND_AI], &jt_best_n[found[i].kind == LAND_AI], &found[i]);
        kept |= found[i].kind == was.kind && found[i].aerial == was.aerial;
    }
    if (kept)
        gl_verified++;
    else
        gl_rejected++;
    if (Log_Level() >= LOG_ALL)
        OSReport("LLJUMPV %d anchor %d hop %d hj %d d %d stick %d aerial %d %s (verified %d rejected %d)\n",
                 event_vars->game_timer, jt_anchor, was.hop, was.hj, was.d, was.stick, was.aerial, kept ? "kept" : "gone",
                 gl_verified, gl_rejected);
}

// The best jump that may come up now, predicted for real if it was laid
// out from the hop library. A few a frame; one still unchecked waits.
#define JT_VERIFY 2
static JumpRoute *Jump_Verify(FighterData *fp, int e)
{
    for (int tries = 0; tries < JT_VERIFY; tries++)
    {
        JumpRoute *b = Jump_Pick(e);
        if (!b || !b->lib)
            return b;
        Jump_Real(fp, b);
    }
    JumpRoute *b = Jump_Pick(e);
    return b && b->lib ? 0 : b;
}

// Moving, the hop shown has come and gone unpressed: the same plan from
// the next frame, if it still works, so a hop that works from anywhere
// along the way stays "now" rather than handing over to some later one,
// whose notes come in, slide, and jump back when it hands over in turn
// (repro-conveyor-f77, 2026-10-15: walking, the press kept sliding 16
// frames out). 1 with it in out.
static int Jump_Again(FighterData *fp, JumpRoute *cur, int e, JumpRoute *out)
{
    JumpRoute was = *cur;
    int k = was.kind == LAND_AI, made = 0;
    for (int tries = 0; tries < JT_VERIFY; tries++)
    {
        JumpRoute *x = 0;
        for (int i = 0; i < jt_best_n[k] && !x; i++)
        {
            JumpRoute *y = &jt_best[k][i];
            if (y->hop == was.hop && y->hj == was.hj && y->stick == was.stick && y->hold == was.hold && y->neutral == was.neutral &&
                y->aerial == was.aerial && y->hp == e + 1)
                x = y;
        }
        if (!x)
        {
            // not among the ones kept (the lists fill with other shapes,
            // or the search hasn't come to that frame): predict it now.
            // Without it the search's later hop took over for a frame or
            // two, then the next anchor brought "now" back, 11-15 times a
            // walk under a platform (0.9.34, CHURN-0934)
            if (made || e + 1 >= JT_D)
                return 0;
            made = 1;
            JumpCand c = {e, was.stick, was.hold, was.hop, was.hj, 0, was.neutral};
            JumpRoute found[JT_TRY];
            int found_n;
            Jump_Try(fp, &c, found, &found_n, 0, 0, 0);
            for (int i = 0; i < found_n; i++)
                if (Jump_OnTarget(&found[i]))
                    Jump_Insert(jt_best[found[i].kind == LAND_AI], &jt_best_n[found[i].kind == LAND_AI], &found[i]);
            tries--; // the prediction isn't a try: look again
            continue;
        }
        if (!x->lib)
        {
            *out = *x;
            return 1;
        }
        Jump_Real(fp, x);
    }
    return 0;
}

// After the search of this frame: the best jump still ahead is the one
// shown, whether the search is done or not.
static void Jump_Publish(FighterData *fp)
{
    if (!jt_active)
    {
        jt_have = 0;
        return;
    }
    int e = Jump_E();
    // its press gone by unpressed, the same plan a frame on, when it works
    // (before Jump_Prune, which drops a hop with no double jump right away)
    JumpRoute again;
    int has_again = 0;
    if (jt_have)
    {
        JumpRoute *was = Jump_Find(&jt_route, jt_route_abs);
        if (was && Jump_HopGone(was, e))
            has_again = Jump_Again(fp, was, e, &again);
    }
    Jump_Prune(e);
    if (jt_ground && gt_still && !jt_done)
    {
        jt_have = 0; // standing still: the plan comes up once, when the search is through, and stays
        return;
    }
    JumpRoute *b = Jump_Verify(fp, e);
    // The one up stays up while the search goes on: another takes its place
    // only when it costs JT_SWITCH frames less (Jump_Cost) and came up no more than
    // JT_SETTLE frames ago, or when it no longer works. (Stephen, 0.8.3: the
    // route changed every frame as the search found slightly better ones.)
    JumpRoute *cur = jt_have ? Jump_Find(&jt_route, jt_route_abs) : 0;
    // frames the same hop has been offered "now" (Meter_AddJump)
    jt_hop_now_age = has_again && Jump_Find(&again, jt_anchor) ? jt_hop_now_age + 1 : 0;
    if (has_again)
        cur = Jump_Find(&again, jt_anchor);
    else if (cur && b && Jump_HopGone(cur, e))
        cur = 0; // its press has come: the next hop takes over
    else if (!cur && jt_have)
        cur = Jump_Near(&jt_route, jt_route_abs, e); // the same plan, a frame or two off
    // (Prefer Vertical: a straight-up one always takes a drifting one's
    // place, which would otherwise stay up, being sooner; 14b02c7, dashing
    // and running showed drift for half the way)
    int to_vert = b && cur && Options_Jump[JOPT_PREFER].val == PREFER_VERTICAL && Jump_Vertical(b) && !Jump_Vertical(cur);
    if (cur && !to_vert &&
        (!b || (b != cur && !(Jump_Cost(b) + JT_SWITCH <= Jump_Cost(cur) && cur->dj - e - 1 >= Jump_Lead() - JT_SETTLE))))
        b = cur;
    jt_have = b != 0;
    if (jt_ground)
    {
        gt_plan_ok = b && (b->hop == GT_SH || b->hop == GT_FH) && b->hj >= 0; // its double jump goes on after the takeoff
        if (gt_plan_ok)
            gt_plan = *b;
    }
    if (!b)
        return;
    if (Log_Level() >= LOG_ALL && (!Jump_Same(b, &jt_route) || b->dj != jt_route.dj))
        OSReport("LLJUMPR %d anchor %d hop %d hj %d dj %d stick %d hold %d aerial %d press %d land %d on %d kind %d neutral %d floor %d "
                 "spot %.2f %.2f cost %d changes %d\n",
                 event_vars->game_timer, jt_anchor, b->hop, b->hj, b->dj, b->stick, b->hold, b->aerial, b->press, b->land,
                 b->where, b->kind, b->neutral, b->floor, b->spot_x, b->spot_y, Jump_Cost(b), b->changes);
    jt_route = *b;
    jt_route_abs = jt_anchor;
    Jump_Path(fp);
    if (Options_Jump[JOPT_TEXT].val)
        Jump_Text(&jt_route, e);
}

// A gate across a path on frame i: a short bar square to it, which the
// bottom of Falcon's ECB (about his feet) passes through on that frame.
// Falcon is the moving note and the gate the line: press as he crosses it.
#define GATE_HALF 4.f
static void Draw_Gate(Vec2 *pos, float *bottom, int num, int i, GXColor c)
{
    int a = i > 0 ? i - 1 : i, b = i < num - 1 ? i + 1 : i;
    float tx = pos[b].X - pos[a].X, ty = pos[b].Y + bottom[b] - pos[a].Y - bottom[a];
    float len = sqrtf(tx * tx + ty * ty);
    if (len < 0.05f)
    {
        tx = 0;
        ty = 1;
        len = 1;
    }
    float gx = -ty / len * GATE_HALF, gy = tx / len * GATE_HALF;
    float x = pos[i].X, y = pos[i].Y + bottom[i];
    GXColor edge = Color_Fill(color_plate, 0.75f);
    World_Start(2, GX_LINES, 54);
    World_Vtx(x - gx * 1.08f, y - gy * 1.08f, 0, edge);
    World_Vtx(x + gx * 1.08f, y + gy * 1.08f, 0, edge);
    World_Start(2, GX_LINES, 30);
    World_Vtx(x - gx, y - gy, 0, c);
    World_Vtx(x + gx, y + gy, 0, c);
}

// Falcon is committed to the plan, or nothing moves: in the jumpsquat, in
// the air, or standing still. The world then shows the plan at once, its
// lead with its spot; moving on the ground it waits until the plan holds
// (Fable's live paths plan).
static int Plan_Committed(void)
{
    return jt_active && (!jt_ground || gt_still || jt_squat);
}

// The next press with a gate on the path, as a frame from the anchor, -1
// none: the double jump, then the aerial. (A hop has none: it's pressed
// where Falcon stands.)
static int Jump_NextGate(JumpRoute *r, int e)
{
    if ((!r->hop || r->hj >= 0) && r->dj > e)
        return r->dj;
    if (r->kind == LAND_AI && r->press > e)
        return r->press;
    return -1;
}

// The lead's last frame: two past the next press, at most LEAD_LEN frames
// from the feet, and never past the touchdown. Nothing after the gate.
#define LEAD_LEN 8
static int Lead_End(int from, int gate, int land, int num)
{
    int to = from + LEAD_LEN;
    if (gate >= 0 && gate + 2 < to)
        to = gate + 2;
    if (land > 0 && land < to)
        to = land;
    if (to > num - 1)
        to = num - 1;
    return to;
}

// The lead's moment, Jump Timing's Path on or not (the input markers keep
// to it too): Path Detail is Lead (Full draws the whole path), the world
// shows the plan, and Falcon is committed to it.
static int Jump_LeadOn(void)
{
    return Jump_Showing() && jt_path_num >= 2 && Path_Detail() == DETAIL_LEAD && Plan_Committed() && jt_world.alpha > 0;
}

// The jump's lead as it's drawn this frame: its frames and its gate (-1
// none), or 0 when none shows.
static int Jump_LeadRange(int *from, int *to, int *gate)
{
    *from = *to = *gate = -1;
    if (!Options_Jump[JOPT_PATH].val || !Jump_LeadOn())
        return 0;
    int e = Jump_E() > 0 ? Jump_E() : 0;
    if (e >= jt_path_num - 1)
        return 0;
    *from = e;
    *gate = Jump_NextGate(&jt_route, e);
    if (*gate >= jt_path_num)
        *gate = -1;
    *to = Lead_End(e, *gate, jt_route.land, jt_path_num);
    return *to > e;
}

// Put a kind's list back in order after what a route costs changed
// (JT_STAY follows the floor the world shows).
static void Jump_Resort(void)
{
    for (int k = 0; k < 2; k++)
        for (int i = 1; i < jt_best_n[k]; i++)
        {
            JumpRoute x = jt_best[k][i];
            int j = i;
            while (j > 0 && Jump_Better(&x, &jt_best[k][j - 1]))
            {
                jt_best[k][j] = jt_best[k][j - 1];
                j--;
            }
            jt_best[k][j] = x;
        }
}

// The live AI or NIL cue counting down to the world's spot, or 0.
static Cue *World_Cue(JumpWorld *w, int *ck)
{
    *ck = w->kind == LAND_AI ? CUE_AI : CUE_NIL;
    Cue *c = &cue_live[*ck];
    if ((c->phase != PH_COUNT && c->phase != PH_WINDOW) || !Cue_KindOn(*ck, c) || fabs(c->spot.X - w->tx) > 2.f * SPOT_HW ||
        fabs(c->spot.Y - w->ty) > 4.f)
        return 0;
    return c;
}

// The stick's turns: each time its x goes past half the other way from the
// side it was last past half on (a dash dance's turns), the game frame,
// the last DD_TURNS of them.
#define DD_TURNS 4
#define DD_WINDOW 24 // two turns in this many frames is a dash dance
#define DD_CALM 12   // ... over once this many go by with none
static int dd_side;
static int dd_turn[DD_TURNS] = {-1000, -1000, -1000, -1000};
static void Stick_Track(float x)
{
    int side = x > 0.5f ? 1 : x < -0.5f ? -1 : 0;
    if (!side)
        return;
    if (dd_side && side != dd_side)
    {
        for (int i = DD_TURNS - 1; i > 0; i--)
            dd_turn[i] = dd_turn[i - 1];
        dd_turn[0] = event_vars->game_timer;
    }
    dd_side = side;
}

// The stick's turns in the last n frames.
static int Stick_Reversals(int n)
{
    int c = 0;
    for (int i = 0; i < DD_TURNS; i++)
        c += event_vars->game_timer - dd_turn[i] < n;
    return c;
}

// The aerial habit's part of what a route costs (Jump_Cost): whether a
// kept AI route has an aerial pressed this session. 1 when it changed, so
// the lists are put back in order.
static int Jump_Habit(void)
{
    JumpWorld *w = &jt_world;
    int habit = 0, mask = 0;
    for (int a = 0; a < 5; a++)
        mask |= ai_used[a] ? 1 << a : 0;
    if (Options_Jump[JOPT_PREFER].val != PREFER_SOONEST)
        for (int i = 0; i < jt_best_n[1] && !habit; i++)
            habit = ai_used[jt_best[1][i].aerial - TS_AIRN] != 0;
    int changed = habit != w->habit || (habit && mask != w->used_mask);
    w->habit = habit;
    w->used_mask = mask;
    return changed;
}

// Each frame after Jump_Publish: what the world shows of the plan
// (jt_world). See JumpWorld.
static void Jump_World(FighterData *fp)
{
    JumpWorld *w = &jt_world;
    JumpRoute *r = &jt_route;
    int was_floor = w->floor, was_on = w->alpha > 0;
    int have = Jump_Showing() && r->floor >= 0;
    // Quiet while dash dancing: two turns of the stick in DD_WINDOW frames
    // hide the plan's spot, lead, marker and the hop previews until DD_CALM
    // frames go by with none. The player isn't committing, and the world
    // doesn't offer until they do; the stability gate alone left a dim
    // spot flickering at the edge of sight (Fable's live paths plan). The
    // plan's stability counts again from then, so with turns less than
    // about 17 frames apart nothing shows. A jump commits: it ends it at
    // once. The strips run on.
    Stick_Track(fp->input.lstick.X);
    if (Stick_Reversals(DD_WINDOW) >= 2)
        w->quiet = 1;
    else if (w->quiet && event_vars->game_timer - dd_turn[0] >= DD_CALM)
        w->quiet = 0;
    if (jt_squat || (jt_active && !jt_ground))
        w->quiet = 0;
    w->calm = w->quiet ? (w->calm > 0.25f ? w->calm - 0.25f : 0.f) : (w->calm < 0.75f ? w->calm + 0.25f : 1.f);
    if (w->leave_age >= 0 && ++w->leave_age >= 4)
        w->leave_age = -1;
    if (have)
    {
        int same = r->floor == w->floor && r->kind == w->kind && r->aerial == w->aerial && r->hop == w->hop && r->stick == w->stick;
        w->stable = same ? w->stable + 1 : 0;
        if (!same || w->gone > JT_WORLD_GRACE)
            w->handed = 0; // a new plan, or the next jump's
        if (r->floor != w->floor)
        {
            // another floor: the old spot fades out in place while the new
            // one comes in
            if (w->alpha > 0 && !w->handed)
            {
                w->leave_x = w->x;
                w->leave_y = w->y;
                w->leave_kind = w->kind;
                w->leave_alpha = w->alpha;
                w->leave_age = 0;
            }
            w->alpha = 0;
            w->x = r->spot_x;
            w->y = r->spot_y;
        }
        else if (w->alpha <= 0)
        {
            w->x = r->spot_x;
            w->y = r->spot_y;
        }
        else
        {
            // the same floor: it slides, as the Conveyor's notes do
            w->x += (r->spot_x - w->x) / 3.f;
            w->y += (r->spot_y - w->y) / 3.f;
            if (fabs(r->spot_x - w->x) < 0.1f)
                w->x = r->spot_x;
            if (fabs(r->spot_y - w->y) < 0.1f)
                w->y = r->spot_y;
        }
        w->tx = r->spot_x;
        w->ty = r->spot_y;
        w->floor = r->floor;
        w->kind = r->kind;
        w->aerial = r->aerial;
        w->hop = r->hop;
        w->stick = r->stick;
        w->have = 1;
        w->gone = 0;
        w->follow = 0;
    }
    else
    {
        // no plan: gone for good after a re-anchor's gap, unless the live
        // cue counts down to the same spot (the double jump used, its AI
        // or NIL on the way): it's held there for the cue's bracket
        int ck, was_follow = w->follow;
        w->have = 0;
        w->gone++;
        w->follow = w->alpha > 0 && !w->handed && World_Cue(w, &ck);
        if (was_follow && !w->follow)
            w->handed = 1; // the cue it waited for has ended: gone with it, with no ending of its own
        if (!w->follow && w->gone > JT_WORLD_GRACE)
            w->stable = 0;
    }
    // the live cue's bracket takes over in the same place and shape: one
    // object from "there" to "now", whose endings are the cue's own
    int ck;
    Cue *c = w->alpha > 0 && !w->handed ? World_Cue(w, &ck) : 0;
    if (c && Options_Timers[TOPT_SPOT].val && Spot_BracketK(ck, c) >= 0)
        w->handed = 1;
    // (handed over, the spot is the cue's; the lead and the marker go on
    // while the plan still has a press ahead)
    if (w->quiet)
        w->stable = 0;
    int shown = !w->quiet && (w->have || w->follow || w->gone <= JT_WORLD_GRACE) &&
                (w->stable >= JT_WORLD_STABLE || Plan_Committed() || w->follow);
    if (shown)
        w->alpha = w->alpha + 0.25f > 1.f ? 1.f : w->alpha + 0.25f;
    else
        w->alpha = w->alpha - 0.25f < 0.f ? 0.f : w->alpha - 0.25f;
    int on = w->alpha > 0;
    int habit = Jump_Habit();
    if (habit || on != was_on || (on && w->floor != was_floor))
        Jump_Resort();
    if (Log_Level() >= LOG_ALL && (Options_Jump[JOPT_SHOW].val || Options_Paths[POPT_PREVIEW].val != PREVIEW_OFF))
    {
        // the lead's frames and gate, and the input markers the jump puts
        // up (Markers_Draw), as the next draw will have them
        int lf, lt, lg, marks = 0, e = Jump_E();
        Jump_LeadRange(&lf, &lt, &lg);
        if (Options_Paths[POPT_INPUTS].val && Jump_Showing() && jt_path_num >= 2)
        {
            if (Path_Detail() == DETAIL_FULL)
                marks = (r->hop && r->hp > e) + (!r->hop || r->hj >= 0) + (r->kind == LAND_AI && r->press > e);
            else
                marks = Jump_LeadOn();
        }
        OSReport("LLWORLD %d have %d floor %d kind %d aerial %d hop %d stick %d stable %d alpha %.2f x %.2f y %.2f tx %.2f ty "
                 "%.2f shown %d committed %d detail %d handed %d follow %d gone %d leave %d %.2f %.2f lead %d %d gate %d marks %d "
                 "quiet %d turns %d calm %.2f habit %d used %x\n",
                 event_vars->game_timer, w->have, w->floor, w->kind, w->aerial, w->hop, w->stick, w->stable, w->alpha, w->x,
                 w->y, w->tx, w->ty, shown, Plan_Committed(), Path_Detail(), w->handed, w->follow, w->gone, w->leave_age,
                 w->leave_x, w->leave_y, lf, lt, lg, marks, w->quiet, Stick_Reversals(DD_WINDOW), w->calm, w->habit,
                 w->used_mask);
    }
}

// The plan's spot, on the HUD under the live cues' brackets: the one
// fading out from another floor, and the one shown.
static void World_SpotDraw(void)
{
    JumpWorld *w = &jt_world;
    if (!Options_Jump[JOPT_SHOW].val || !Options_Jump[JOPT_PATH].val || capture_clean)
        return;
    if (w->leave_age >= 0)
        Spot_Rest(w->leave_x, w->leave_y, land_kind_colors[w->leave_kind], w->leave_alpha * (1.f - w->leave_age / 4.f), 0);
    if (!w->handed && w->alpha > 0)
        Spot_Rest(w->x, w->y, land_kind_colors[w->kind], w->alpha, 0);
}

// The hop previews at full speed: only where each lands, a short hop's
// hollow (Fable's live paths plan). The whole paths are Full's, and Frame
// Advance's.
static void Preview_Spots(void)
{
    if (Path_Detail() == DETAIL_FULL || live_visible || ghost_visible || capture_clean || jt_world.calm <= 0)
        return;
    for (int sh = 0; sh < 2; sh++)
    {
        Prediction *p = sh ? pred_sh : pred_fh;
        if (!(sh ? preview_sh : preview_fh) || !p->land_frame)
            continue;
        int k = p->land_frame;
        int shown = p->uncertain_from > k && Kind_Shown(p->land_kind);
        Spot_Rest(p->pos[k].X, p->pos[k].Y + p->bottom[k], shown ? land_kind_colors[p->land_kind] : color_neutral,
                  (shown ? 1.f : 0.6f) * jt_world.calm, sh); // (gone while dash dancing: Jump_World)
    }
}

// The shown jump in the stage: all of its path and gates (Path Detail
// Full, and Frame Advance), or its lead, a stub of the next few frames from
// the feet with the next press's gate (Lead, once Falcon is committed to
// it). Nothing at full speed while he only moves on the ground: the spot
// says where (World_SpotDraw). (Stephen, 0.9.35: the path line was "WAY
// too distracting", drawn all over the screen while dash dancing.)
static void Draw_JumpPath(void)
{
    if (!Jump_Showing() || !Options_Jump[JOPT_PATH].val || jt_path_num < 2)
        return;
    int from = Jump_E();
    if (from < 0)
        from = 0;
    if (from >= jt_path_num - 1)
        return;
    GXColor c = land_kind_colors[jt_route.kind];
    if (Path_Detail() != DETAIL_FULL)
    {
        int lf, lt, gate;
        if (!Jump_LeadRange(&lf, &lt, &gate))
            return;
        float a = jt_world.alpha, base = vis_k;
        Draw_Lead(jt_path, jt_path_bottom, lf, lt, Color_Fill(color_plate, 0.75f), 42, a);
        Draw_Lead(jt_path, jt_path_bottom, lf, lt, c, 24, a);
        if (gate >= 0)
        {
            vis_k = base * a;
            Draw_Gate(jt_path, jt_path_bottom, jt_path_num, gate, gate == jt_route.dj ? color_in_jump : color_in_aerial);
            vis_k = base;
        }
        return;
    }
    // a dark edge under it, so it reads over Falcon and any stage
    Draw_Path(jt_path, jt_path_bottom, from, jt_path_num - 1, Color_Fill(color_plate, 0.75f), 42);
    Draw_Path(jt_path, jt_path_bottom, from, jt_path_num - 1, c, 24);
    if (Options_Paths[POPT_TICKS].val != 2)
        Draw_Ticks(jt_path, jt_path_bottom, from, jt_path_num - 1, c, 9);

    // a gate where Falcon is on the jump's frame, and one on the aerial's
    JumpRoute *r = &jt_route;
    if ((!r->hop || r->hj >= 0) && r->dj > from && r->dj < jt_path_num)
        Draw_Gate(jt_path, jt_path_bottom, jt_path_num, r->dj, color_in_jump);
    if (r->kind == LAND_AI && r->press > from && r->press < jt_path_num)
        Draw_Gate(jt_path, jt_path_bottom, jt_path_num, r->press, color_in_aerial);
}

// The route's cells from the next frame on, e frames after the anchor: the
// frames in the air, the aerial's window, the touchdown, the landing lag
// and the first frame Falcon can act.
static void Row_Jump(MeterRow *row, JumpRoute *r, int e)
{
    int base = -e - 1;
    int act = r->kind == LAND_AI ? r->act : r->land + 1;
    for (int n = e + 1; n < r->land; n++)
        Route_Cell(row, base, n, CELL_AIR, 0);
    Route_Cell(row, base, r->land, CELL_LAND, 0);
    for (int n = r->land + 1; n < act; n++)
        Route_Cell(row, base, n, CELL_LAG, 0);
    Route_Cell(row, base, act, CELL_ACT, 0);
    if (r->kind == LAND_AI)
    {
        for (int n = r->press; n < r->press + r->press_w; n++)
            Route_Cell(row, base, n, CELL_PRESS, n == r->press ? GLYPH_AERIAL : 0);
        int next = e + 1;
        if (next >= r->press && next < r->press + r->press_w)
            row->hot = 2;
        sprintf(row->info, "%s", tracked_state_names[r->aerial]);
    }
}

// The double jump's glyph: the button's arrow as the stick, a bar under it
// for drift held after the jump, so a drifting plan says why it drifts
// (Fable's Conveyor feel plan, 2026-10-16)
static int Jump_DJGlyph(JumpRoute *r)
{
    static const u8 g[JT_STICKS] = {GLYPH_JUMP, GLYPH_JUMP_UL, GLYPH_JUMP_UR, GLYPH_JUMP_L, GLYPH_JUMP_R};
    return g[r->stick] | (r->hold && r->stick != JT_UP ? GLYPH_HELD : 0);
}

// Its rows: JUMP counts down to the jump, the one under it is the aerial's
// window as an AI shows it (or the NIL's touchdown).
static void Meter_AddJump(void)
{
    if (!Jump_Showing())
    {
        // an empty JUMP lane while Jump Timing is on, so a jump comes in
        // along a lane that's already there rather than the lane popping
        // in and out with it (Stephen, 0.8.5)
        if (Options_Jump[JOPT_SHOW].val && !route_rows_active && hang_ledge < 0)
        {
            MeterRow *row = Meter_Add(color_in_jump, jt_end_age >= 0 ? jt_end_label : "JUMP");
            if (row && jt_end_age >= 0)
                Row_JumpEnd(row);
        }
        return;
    }
    JumpRoute *r = &jt_route;
    int e = Jump_E();
    MeterRow *row;
    // moving, a hop due now counts down as any other: the same plan a frame
    // on takes its place each frame (Jump_Again), so its notes hold still.
    // Parked as standing still's, they sat a jumpsquat higher, and jumped
    // between the two (repro-conveyor-f77, 2026-10-15)
    if (r->hop && gt_still)
    {
        // standing still: what a hop now leads to, parked and dim, laid out
        // as it will be at the takeoff, in the lanes the air countdown uses,
        // so it starts moving where it sits when Falcon leaves the ground
        int t0 = r->hj >= 0 ? r->dj - r->hj - 1 : r->hp; // the takeoff
        row = Meter_Add(color_in_jump, "JUMP");
        if (!row)
            return;
        row->fade = 0.85f; // parked, not moving: dimmed a little, never grayed (Stephen, 0.8.6)
        row->parked = 1;
        if (r->hj >= 0)
        {
            for (int n = t0 + 1; n < r->dj; n++)
                Route_Cell(row, -t0 - 1, n, CELL_AIR, 0);
            Route_Cell(row, -t0 - 1, r->dj, CELL_PRESS, Jump_DJGlyph(r));
        }
        sprintf(row->info, "%s", r->hop == GT_SH ? "SH" : "FH");
        route_rows_active = 1;
        jump_rows_active = 1;
        row = Meter_Add(land_kind_colors[r->kind], r->kind == LAND_AI ? "AI" : "NIL");
        if (!row)
            return;
        row->fade = 0.85f * Fade_Factor(VG_TIMERS, r->kind == LAND_AI ? CUE_AI : CUE_NIL);
        row->parked = 1;
        Row_Jump(row, r, t0);
        return;
    }
    // from the ground: the hop's row, then (with a double jump) the jump's
    for (int step = r->hop ? 0 : 1; step < 2; step++)
    {
        if (step == 1 && r->hop && r->hj < 0)
            break;
        int at = step == 0 ? r->hp : r->dj;
        int until = at - e - 1; // cells to the press: 0 is the next frame
        // the double jump in the JUMP lane, as it goes on in the air after the takeoff
        // one HOP lane for either hop (0.9.2 test: SH and FH lanes both stayed up)
        row = Meter_Add(color_in_jump, step == 0 ? "HOP" : "JUMP");
        if (!row)
            return;
        for (int n = e + 1; n < at; n++)
            Route_Cell(row, -e - 1, n, CELL_AIR, 0);
        Route_Cell(row, -e - 1, at, CELL_PRESS, step == 0 ? GLYPH_JUMP : Jump_DJGlyph(r));
        row->held = jt_hop_now_age > 0; // the same plan a frame on: its notes hold still
        // a hop offered "now" frame after frame while he walks is an offer,
        // not an alarm: white only on its first frame, its own color after
        // (Fable's Conveyor feel plan, 2026-10-16)
        if (until == 0)
            row->hot = step == 0 && jt_hop_now_age > 0 ? 1 : 2;
        const char *what = step == 0 ? (r->hop == GT_SH ? "SH " : "FH ") : "";
        if (until > 0)
            sprintf(row->info, "%s%df", what, until);
        else if (until == 0)
            sprintf(row->info, "%snow", what);
        if (step == 1 && until >= 0)
        {
            jt_due = until;
            strcpy(jt_due_label, row->label);
        }
        if (jt_end_age >= 0 && Str_Same(jt_end_label, row->label))
            Row_JumpEnd(row);
    }
    route_rows_active = 1;
    jump_rows_active = 1;

    row = Meter_Add(land_kind_colors[r->kind], r->kind == LAND_AI ? "AI" : "NIL");
    if (!row)
        return;
    row->fade = Fade_Factor(VG_TIMERS, r->kind == LAND_AI ? CUE_AI : CUE_NIL); // as the AI or NIL timer's own
    row->held = jt_hop_now_age > 0;
    Row_Jump(row, r, e);
}

static void Draw_RoutePath(void)
{
    if (!Routes_On() || !Options_Paths[POPT_ROUTE].val || route_path_num < 2)
        return;
    int show = (hang_ledge >= 0 && route_show_num > 0) || (route_active && !route_dj_done);
    if (!show)
        return;
    GXColor c = land_kind_colors[route_path_of.kind];
    // hanging, the route is static from a fixed anchor, and stays whole; on
    // the way back the lead's rules hold (Fable's live paths plan)
    if (hang_ledge < 0 && route_active && Path_Detail() != DETAIL_FULL)
    {
        LedgeRoute *r = &route_path_of;
        int from = route_e > 0 ? route_e : 0;
        if (Path_Detail() != DETAIL_LEAD || from >= route_path_num - 1)
            return;
        int gate = r->dj > from ? r->dj : r->kind == LAND_AI && r->press > from ? r->press : -1;
        if (gate >= route_path_num)
            gate = -1;
        int to = Lead_End(from, gate, r->land, route_path_num);
        Draw_Lead(route_path, route_path_bottom, from, to, Color_Fill(color_plate, 0.75f), 42, 1.f);
        Draw_Lead(route_path, route_path_bottom, from, to, c, 24, 1.f);
        if (gate >= 0)
            Draw_Gate(route_path, route_path_bottom, route_path_num, gate, gate == r->dj ? color_in_jump : color_in_aerial);
        return;
    }
    // a dark edge under it, so it reads over Falcon and any stage
    Draw_Path(route_path, route_path_bottom, 0, route_path_num - 1, Color_Fill(color_plate, 0.75f), 42);
    Draw_Path(route_path, route_path_bottom, 0, route_path_num - 1, c, 24);
    if (Options_Paths[POPT_TICKS].val != 2)
        Draw_Ticks(route_path, route_path_bottom, 0, route_path_num - 1, c, 9);
}
