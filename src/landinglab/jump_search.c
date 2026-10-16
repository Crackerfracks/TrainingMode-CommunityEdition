/* landinglab/jump_search.c
 *
 * Holds the jump search's candidates, solving, the chosen jump's text and
 * meter rows, and the route and jump paths.
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
    int k = was.kind == LAND_AI;
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
            return 0;
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
    if (cur && (!b || (b != cur && !(Jump_Cost(b) + JT_SWITCH <= Jump_Cost(cur) && cur->dj - e - 1 >= Jump_Lead() - JT_SETTLE))))
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
        OSReport("LLJUMPR %d anchor %d hop %d hj %d dj %d stick %d hold %d aerial %d press %d land %d on %d kind %d neutral %d\n",
                 event_vars->game_timer, jt_anchor, b->hop, b->hj, b->dj, b->stick, b->hold, b->aerial, b->press, b->land,
                 b->where, b->kind, b->neutral);
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
            Route_Cell(row, -t0 - 1, r->dj, CELL_PRESS, GLYPH_JUMP);
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
        Route_Cell(row, -e - 1, at, CELL_PRESS, GLYPH_JUMP);
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
    // a dark edge under it, so it reads over Falcon and any stage
    Draw_Path(route_path, route_path_bottom, 0, route_path_num - 1, Color_Fill(color_plate, 0.75f), 42);
    Draw_Path(route_path, route_path_bottom, 0, route_path_num - 1, c, 24);
    if (Options_Paths[POPT_TICKS].val != 2)
        Draw_Ticks(route_path, route_path_bottom, 0, route_path_num - 1, c, 9);
}
