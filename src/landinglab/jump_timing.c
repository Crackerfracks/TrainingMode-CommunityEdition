/* landinglab/jump_timing.c
 *
 * Holds jump timing: candidates for when to double jump, and simulating each.
 *
 * Part of Landing Gear: src/landinglab.c includes it, in order, into one
 * translation unit, so it sees everything included before it (and mex.h,
 * events.h, landinglab_sim.h).
 *
 * Uses: menus_practice, hud_text, timers, meter_placement, sim_dump,
 * ledge_routes.
 */

///////////////////////
/// Jump timing     ///
///////////////////////

// While Falcon falls with his double jump left, the jumps worth making: for
// each frame to jump on (JT_D of them from the anchor, the frame the search
// started from) and each stick to jump with, the fall is simulated on, and
// the touchdown it ends in is kept when it is a NIL or an aerial interrupt
// (AI). The fall before the jump is simulated once, keeping the stick as it
// was at the anchor, and each frame of it kept to jump from. The best jump
// of each kind is shown like a ledge route: its path, a JUMP row counting
// down to the frame, the aerial's press window as an AI shows it, and the
// panel's line. Frames are counted from the anchor's own frame (0), the
// same game frames the simulation steps through: the jump after d frames of
// fall is frame d + 1.

#define JT_D 46             // frames to jump on: d = 0 to 45 frames of fall first
#define JT_STICKS 5
#define JT_CANDIDATES (JT_D * JT_STICKS * 2) // frame, stick, and then holding its x or not
#define JT_SIM 80           // frames simulated after the jump
#define JT_BEST 8           // routes kept per kind
#define JT_BOTTOM 30.f      // how far under the lowest floor a fall is given up on
#define JT_REANCHOR (JT_D - 5) // from the ground: frames after which the anchor has too few jump frames left
#define JT_STICK_TOL 0.05f  // how far the stick may move before the anchor's fall is wrong
#define JT_DRIFT_TOL 0.05f  // ... and how far Falcon may be from the simulated fall
#define JT_HOP_STILL 0.1f   // from the ground, a stick this near the middle is at rest
// The search starts Jump_Lead() frames ahead (Warning): it does about one
// frame's jumps per frame, so starting at the next frame it would only ever
// find jumps due now.
#define JT_BUDGET LR_BUDGET // simulated frames per game frame (the time limit LR_TIME_US holds it too)
#define JT_TRY (1 + 5 * LR_WINDOWS)
#define JT_PATH (JT_D + 1 + JT_SIM + 10) // + a jumpsquat from the ground

// The stick on the jump: all the way up, diagonally up either way (a flick
// up and over), or all the way sideways. Afterwards it is let go, or its x
// is kept.
enum jt_stick
{
    JT_UP,
    JT_UPLEFT,
    JT_UPRIGHT,
    JT_LEFT,
    JT_RIGHT,
};
static const float jt_stick_xy[JT_STICKS][2] = {{0.f, 1.f}, {-0.7f, 0.7f}, {0.7f, 0.7f}, {-1.f, 0.f}, {1.f, 0.f}};
static const char *jt_stick_names[JT_STICKS] = {"up", "up-left", "up-right", "left", "right"};

typedef struct JumpRoute
{
    u8 valid;
    u8 kind;    // LAND_NIL or LAND_AI
    u8 stick;   // the stick on the jump: JT_*
    u8 hold;    // after the jump: 1 = keep the stick's x, 0 = let go
    u8 aerial;  // AI: the aerial (TS_AIR*)
    u8 press_w; // AI: frames the aerial's window lasts
    u8 changes; // stick changes the route asks for
    u8 where;   // what it lands on: JWHERE_* bits
    u8 hop;     // from the ground: GT_SH or GT_FH, 0 for a double jump from the air
    u8 seen;    // already shown (a standstill plan carried into the air): no Warning cutoff
    s8 hj;      // ... with a double jump hj frames after the takeoff, -1 for the hop alone
    u8 late;    // ... and that double jump comes once the hop is falling
    s16 hp;     // ... the frame the hop is pressed on
    u8 d;       // frames of fall before the jump
    s16 dj;     // frame of the double jump
    s16 press;  // AI: first frame of the aerial's window
    s16 land;   // touchdown
    s16 act;    // the frame Falcon can act: the touchdown for a NIL, the end of the landing lag for an AI
} JumpRoute;

static SimState *jt_cache;      // [JT_D], allocated in Event_Init: the fall to jump from, d frames in
static int jt_cache_n;          // ... how many of them: past that the fall lands or bonks
static SimStart jt_start;       // the anchor: Falcon as the search started
static int jt_anchor;           // its game frame
static int jt_next;             // the next candidate to simulate
static int jt_done;             // the search has been through every candidate
static int jt_found;            // routes it found
static float jt_bottom;         // how far down a fall is followed
static float jt_stick_x;        // the stick at the anchor: the fall assumes it held
static int jt_stick_down, jt_stick_drop;
static int jt_prev_ts = -1, jt_prev_tilt;
// The jump lane's ending, as the cues' rows have: the double jump on the
// frame the lane said is a hit (a burst, a white flash and a bright ghost at
// the gate), early or late folds it in the miss color. (Stephen, 0.8.6: the
// notes hit the bottom and vanished.)
static int jt_due = -1;      // last frame: frames to the double jump the lane showed, 0 the next one
static char jt_due_label[8]; // ... in this lane
static int jt_end_age = -1, jt_end_hit, jt_end_off;
static char jt_end_label[8];
static JumpRoute jt_best[2][JT_BEST]; // per kind (NIL, AI), best first
static int jt_best_n[2];
static JumpRoute jt_route;      // the one shown
static int jt_have;             // ... there is one
static Vec2 *jt_path;           // [JT_PATH], allocated in Event_Init: where it goes, from the anchor
static float *jt_path_bottom;
static int jt_path_num;
static JumpRoute jt_path_of;    // the route the path is for
static int jt_path_anchor = -1;
static int jt_target;           // Land On as the search started
static int jt_hop;              // From the Ground as the search started
static int jt_steps, jt_tries;  // the search's cost, for the log
static JumpRoute gt_plan;       // the standstill plan shown last, with its double jump
static u8 gt_plan_ok;
static u8 gt_squat_short;       // the jumpsquat is a short hop's

// From the ground (Hop Timing): the anchor is on the ground, and a
// candidate's d is the frames Falcon keeps moving before the jump is
// pressed. Where he is and how fast he goes, d frames on, assuming the
// stick stays as it is: a run or walk keeps its speed, and with the stick
// at rest friction slows him.
#define GT_SH 1
#define GT_FH 2
typedef struct GroundStep
{
    float x, y, v;
} GroundStep;
static u8 jt_ground;          // the anchor is on the ground
static GroundStep *gt_steps;  // [JT_D], allocated in Event_Init
static int gt_steps_n;
static int gt_sid;            // the state at the anchor
static u8 gt_still;           // ... standing still: every frame's hop is the same, so the search tries the
                              // next frame's alone, and the anchor stays while he does
// Running or sliding off the floor's end (Stephen, 0.8.6: no notes ahead of
// time on a platform at any speed). gt_edge is the frame he leaves it, -1
// none; gt_fall is his state as the fall starts, and jt_cache from gt_edge
// on holds the fall. A run-off candidate is the double jump j frames into
// it, searched like one in the air, so its notes come up while he still
// runs, Warning frames ahead, and move toward the gate with him.
#define GT_RUNOFF 3
static int gt_edge = -1;
static SimStart gt_fall;

// Frames since the anchor, as the routes count them: standing still, the
// anchor is always now.
static int Jump_Lead(void)
{
    return jump_leads[Options_Jump[JOPT_LEAD].val];
}

// In the air, the anchor is taken again once the search's horizon (Warning
// frames ahead) nears its last jump frame.
static int Jump_Reanchor(void)
{
    return JT_D - 2 - Jump_Lead();
}

static int Jump_E(void)
{
    return jt_ground && gt_still ? 0 : event_vars->game_timer - jt_anchor;
}

// What a floor is, for Land On.
#define JWHERE_FLOOR 1
#define JWHERE_PLATFORM 2
#define JWHERE_TOP 4
#define JWHERE_LEFT 8
#define JWHERE_RIGHT 16
#define JWHERE_SIDE 10.f // how far from the stage's middle a platform's middle is on a side

// What Falcon lands on with his feet at (x, y): the floor at x nearest
// that height. For an AI, (x, y) is where the plain fall has him on the
// frame the aerial touches down, which for a rising one can be well above
// the floor or, coming up through a platform, a little under it. Platforms
// move on some stages, so this is worked out each time.
static int Jump_Where(float x, float y)
{
    FloorLine *best = 0;
    float best_y = 0, l, ly, r, ry;
    for (int i = 0; i < floor_num; i++)
    {
        FloorLine *f = &floor_cache[i];
        Floor_Ends(f, &l, &ly, &r, &ry);
        if (r - l < 0.001f || x < l - 0.5f || x > r + 0.5f)
            continue;
        float h = ly + (ry - ly) * (x - l) / (r - l);
        if (h <= y + 15.f && h >= y - 40.f && (!best || fabs(h - y) < fabs(best_y - y)))
        {
            best = f;
            best_y = h;
        }
    }
    if (!best)
        return 0;
    if (!best->is_platform)
        return JWHERE_FLOOR;
    int where = JWHERE_PLATFORM;
    float top = -100000.f;
    for (int i = 0; i < floor_num; i++)
    {
        FloorLine *f = &floor_cache[i];
        float h = f->y0 > f->y1 ? f->y0 : f->y1;
        if (f->is_platform && h > top)
            top = h;
    }
    if ((best->y0 > best->y1 ? best->y0 : best->y1) >= top - 1.f)
        where |= JWHERE_TOP;
    float mid = (best->x0 + best->x1) * 0.5f;
    if (mid < -JWHERE_SIDE)
        where |= JWHERE_LEFT;
    else if (mid > JWHERE_SIDE)
        where |= JWHERE_RIGHT;
    return where;
}

// Does a route land where Land On asks?
static int Jump_OnTarget(JumpRoute *r)
{
    static const u8 need[] = {0, JWHERE_TOP, JWHERE_LEFT, JWHERE_RIGHT, JWHERE_PLATFORM, JWHERE_FLOOR};
    return jt_target <= JTGT_ANY || jt_target >= (int)countof(need) || (r->where & need[jt_target]);
}

static int Jump_Showing(void)
{
    return jt_active && jt_have && Options_Jump[JOPT_SHOW].val && !route_active && hang_ledge < 0;
}

// Frames to the shown route's next jump press (0: this frame), or -1.
static int Jump_PressAhead(void)
{
    if (!Jump_Showing())
        return -1;
    JumpRoute *r = &jt_route;
    int e = Jump_E();
    if (r->hop && r->hp - e - 1 >= 0)
        return r->hp - e - 1;
    if (r->hop && r->hj < 0)
        return -1;
    int u = r->dj - e - 1;
    return u >= 0 ? u : -1;
}

// Stick changes a jump asks for: to the jump's stick, and from it to rest
// (or to its x alone, which is no change for a stick that has no y).
static int Jump_Changes(int stick, int hold)
{
    return 1 + !(hold && (stick == JT_LEFT || stick == JT_RIGHT));
}

// Better: acts sooner; then a wider aerial window (a NIL has no aerial to
// time, so it's the widest); then fewer stick changes; then the one with
// more time to get ready.
static int Jump_Wide(JumpRoute *r)
{
    return r->kind == LAND_NIL ? 99 : r->press_w;
}

static int Jump_Better(JumpRoute *a, JumpRoute *b)
{
    if (!b->valid)
        return a->valid;
    if (!a->valid)
        return 0;
    if (a->late != b->late)
        return b->late;
    if (a->act != b->act)
        return a->act < b->act;
    if (Jump_Wide(a) != Jump_Wide(b))
        return Jump_Wide(a) > Jump_Wide(b);
    if (a->changes != b->changes)
        return a->changes < b->changes;
    // nothing left to tell them apart by, so that the order never depends
    // on the order they were found in
    if (a->dj != b->dj)
        return a->dj > b->dj;
    if (a->stick != b->stick)
        return a->stick < b->stick;
    if (a->kind != b->kind)
        return a->kind == LAND_NIL;
    if (a->aerial != b->aerial)
        return Aerial_Rank(a->aerial) < Aerial_Rank(b->aerial);
    if (a->press != b->press)
        return a->press < b->press;
    return a->hold < b->hold;
}

// The same route: the same jump and press. Keeping the stick's x after the
// jump or not doesn't make another one.
static int Jump_Same(JumpRoute *a, JumpRoute *b)
{
    return a->kind == b->kind && a->d == b->d && a->stick == b->stick && a->aerial == b->aerial && a->press == b->press &&
           a->hop == b->hop && a->hj == b->hj;
}

// The first input a route asks for: the hop, or the double jump.
static int Jump_First(JumpRoute *r)
{
    return r->hop ? r->hp : r->dj;
}

// Put a route in a kind's list, in order, as Route_Insert does.
static void Jump_Insert(JumpRoute *a, int *count, JumpRoute *r)
{
    int n = *count;
    for (int i = 0; i < n; i++)
    {
        if (!Jump_Same(r, &a[i]))
            continue;
        if (!Jump_Better(r, &a[i]))
            return;
        for (int j = i; j < n - 1; j++)
            a[j] = a[j + 1];
        n--;
        break;
    }
    int at = n;
    while (at > 0 && Jump_Better(r, &a[at - 1]))
        at--;
    if (at < JT_BEST)
    {
        if (n == JT_BEST)
            n--;
        for (int j = n; j > at; j--)
            a[j] = a[j - 1];
        a[at] = *r;
        n++;
    }
    *count = n;
}

// One jump: d frames of fall with the stick as at the anchor, the double
// jump with a stick, then holding its x or not. Gives, in out (up to
// JT_TRY, n of them), a NIL when it lands with no lag, and for each aerial
// the first few windows of press frames that touch down together with no
// aerial lag, the way Ledge_Try does. The aerials the AI Filter and AI
// Aerial options leave out are left out, and unless the filter shows all of
// them, so are the ones that are not worth the press (Windows_Summarize).
// With path, also records where Falcon goes, frame by frame from the
// anchor to the touchdown.
typedef struct JumpCand
{
    int d;     // frames before the jump (in the air) or the hop (on the ground)
    int stick; // the double jump's stick, JT_*
    int hold;  // after the double jump (or the hop): keep its x
    int hop;   // on the ground: GT_SH or GT_FH
    int hj;    // ... a double jump this many frames after the takeoff, or -1
    int v;     // ... which of the standstill variants (Gt_Variant) it is
} JumpCand;

static u8 jt_try_predicted; // the last Jump_Try got as far as its prediction (pred_route is its)
static void Jump_Try(FighterData *fp, JumpCand *c, JumpRoute *out, int *n_out, Vec2 *path, float *bottom, int *num)
{
    jt_try_predicted = 0;
    int late_dj = 0;
    int d = c->d, stick = c->stick, hold = c->hold;
    SimStart ps;
    int n = 0, off;
    *n_out = 0;
    if (path)
        *num = 0;
    int air_like = !jt_ground || c->hop == GT_RUNOFF;
    if (!air_like)
    {
        // d frames on the ground, the press, the squat, the takeoff
        GroundStep *g = &gt_steps[d];
        float slide[12];
        int until = Sim_GroundJumpAt(fp, c->hop == GT_SH, g->x, g->y, g->v, jt_stick_x, 1, &ps, slide);
        if (until - 1 > (int)countof(slide))
            return;
        ps.stick_x = hold ? jt_stick_x : 0;
        ps.stick_y = 0;
        off = d + until;
        if (path)
        {
            for (int i = 0; i <= d; i++)
            {
                path[n] = (Vec2){gt_steps[i].x, gt_steps[i].y};
                bottom[n++] = 0;
            }
            for (int i = 0; i < until - 1; i++)
            {
                path[n] = (Vec2){slide[i], g->y};
                bottom[n++] = 0;
            }
            path[n] = ps.pos;
            bottom[n++] = 0;
        }
        if (c->hj >= 0)
        {
            // the hop with the stick as on the ground, then the double jump
            SimStart st = ps;
            SimState s;
            SimStep step;
            st.stick_x = jt_stick_x;
            Sim_Init(&st, &s);
            for (int i = 0; i < c->hj; i++)
            {
                Sim_Step(fp, &st, &s, -1, 0, &step);
                if (step.landed || step.ceiling)
                    return;
                if (path)
                {
                    path[n] = (Vec2){s.x, s.y};
                    bottom[n++] = s.bottom;
                }
            }
            // a double jump once the hop is coming down ranks after the
            // rest (Stephen, 0.9.2: a mindgame, not a habit)
            late_dj = s.vy < 0;
            st.skip_line = -1;
            st.stick_x = jt_stick_xy[stick][0];
            st.stick_y = jt_stick_xy[stick][1];
            Sim_DoubleJump(fp, &st, &s, &step);
            if (path)
            {
                path[n] = (Vec2){s.x, s.y};
                bottom[n++] = s.bottom;
            }
            if (step.landed || step.ceiling)
                return;
            st.stick_x = hold ? jt_stick_xy[stick][0] : 0;
            st.stick_y = 0;
            Sim_ToStart(&s, &st, &ps);
            off += c->hj + 1;
        }
    }
    else
    {
        // in the air, or running off the floor's end and falling d - gt_edge frames
        SimStart st = jt_ground ? gt_fall : jt_start;
        SimState s = jt_cache[d];
        SimStep step;
        if (path)
        {
            for (int i = 0; i <= d; i++)
            {
                int floor = jt_ground && i < gt_edge;
                path[n] = floor ? (Vec2){gt_steps[i].x, gt_steps[i].y} : (Vec2){jt_cache[i].x, jt_cache[i].y};
                bottom[n++] = floor ? 0 : jt_cache[i].bottom;
            }
        }

        // the game forgets the platform dropped through on any state change
        st.skip_line = -1;
        st.stick_x = jt_stick_xy[stick][0];
        st.stick_y = jt_stick_xy[stick][1];
        Sim_DoubleJump(fp, &st, &s, &step);
        if (path)
        {
            path[n] = (Vec2){s.x, s.y};
            bottom[n++] = s.bottom;
        }
        if (step.landed || step.ceiling)
            return;

        st.stick_x = hold ? jt_stick_xy[stick][0] : 0;
        st.stick_y = 0;
        Sim_ToStart(&s, &st, &ps);
        off = d + 1;
    }
    sim_limit = JT_SIM;
    sim_bottom_y = jt_bottom;
    sim_rising_only = !ai_show_all;
    if (sim_rising_only)
        sim_stop_vy = Fighter_GetSoftLandVelocity(fp) - 0.001f;
    int steps0 = sim_steps;
    Predict(fp, &ps, pred_route, BR_AI);
    jt_try_predicted = 1;
    jt_steps += sim_steps - steps0;
    jt_tries++;
    sim_rising_only = 0;
    sim_stop_vy = -100000.f;
    sim_limit = LL_SIM_FRAMES;
    sim_bottom_y = -100000.f;

    Prediction *p = pred_route;
    if (path)
    {
        int last = p->land_frame ? p->land_frame : p->num;
        for (int k = 1; k <= last && n < JT_PATH; k++)
        {
            path[n] = p->pos[k];
            bottom[n++] = p->bottom[k];
        }
        *num = n;
    }

    JumpRoute base = {0};
    base.stick = stick;
    base.hold = hold;
    base.hop = air_like ? 0 : c->hop;
    base.hj = air_like ? -1 : c->hj;
    base.hp = air_like ? 0 : d + 1;
    base.d = d;
    base.dj = !air_like && c->hj >= 0 ? off : d + 1;
    base.press = -1;
    base.changes = air_like ? Jump_Changes(stick, hold) : c->hj >= 0 ? 1 + Jump_Changes(stick, hold) : 1 + !hold;
    base.late = late_dj;

    int nr = 0;
    if (p->land_frame && p->land_kind == LAND_NIL && p->uncertain_from > p->land_frame)
    {
        JumpRoute *r = &out[nr++];
        *r = base;
        r->valid = 1;
        r->kind = LAND_NIL;
        r->land = off + p->land_frame;
        r->act = r->land;
        r->where = Jump_Where(p->pos[p->land_frame].X, p->pos[p->land_frame].Y);
    }

    int normal_lag = (int)fp->attr.normal_landing_lag;
    int hold_done = p->land_frame ? p->land_frame + p->lag : 2 * LL_SIM_FRAMES;
    int last = p->land_frame ? p->land_frame - 1 : p->num;
    for (int a = 0; a < 5; a++)
    {
        int aerial = Aerial_Order(a);
        u8 bit = AERIAL_BIT(aerial);
        if (ai_only && !(ai_only & bit))
            continue;
        int windows = 0;
        for (int k = 1; k <= last && k < p->uncertain_from && windows < LR_WINDOWS; k++)
        {
            if (!(p->ai_mask[k] & ~p->ai_lag_mask[k] & bit))
                continue;
            int touch = k + Ai_Delay(p, k, aerial);
            int w = 1;
            while (k + w <= last && (p->ai_mask[k + w] & ~p->ai_lag_mask[k + w] & bit) &&
                   k + w + Ai_Delay(p, k + w, aerial) == touch)
                w++;
            // worth it: done with the landing sooner than holding would be,
            // and rising as it touches down
            int worth = ai_show_all || (hold_done - (touch + normal_lag) >= LL_AI_MIN_GAIN &&
                                        !(touch <= p->num && p->pos[touch].Y <= p->pos[touch - 1].Y));
            if (worth)
            {
                JumpRoute *r = &out[nr++];
                *r = base;
                r->valid = 1;
                r->kind = LAND_AI;
                r->aerial = aerial;
                r->press = off + k;
                r->press_w = w;
                r->land = off + touch;
                r->act = r->land + normal_lag;
                int at = touch <= p->num ? touch : p->num;
                r->where = Jump_Where(p->pos[at].X, p->pos[at].Y);
                windows++;
            }
            k += w - 1;
        }
    }
    *n_out = nr;
}
