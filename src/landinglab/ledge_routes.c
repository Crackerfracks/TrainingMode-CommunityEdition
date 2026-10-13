/* landinglab/ledge_routes.c
 *
 * Holds ledge routes: the ledges, the route search from the ledge, and the
 * routes' meter rows.
 *
 * Part of Landing Lab: src/landinglab.c includes it, in order, into one
 * translation unit, so it sees everything included before it (and mex.h,
 * events.h, landinglab_sim.h).
 *
 * Uses: menus_practice, tracking_state, logging, hud_text, timers, meter,
 * controller.
 * Refers ahead, through forward declarations, to: ledge_practice,
 * reset_drill.
 */

///////////////////////
/// Ledge routes    ///
///////////////////////

// From the ledge, the fastest ways onto the stage, ranked by the ledge
// intangibility they keep (GALINT): let go with the stick away or down,
// wait, fastfall, double jump toward the stage, then land with no lag (NIL)
// or press an aerial that interrupts the landing (AI). Every combination of
// up to LR_WAIT frames of waiting and LR_FF of fastfall is simulated once
// per ledge, a few hundred simulated frames per game frame, and every
// distinct route kept (up to LR_ROUTES of each kind), best first. The Route
// option browses them. The main stage's two ledges are searched as the event
// starts; any other ledge when Falcon first hangs from it.

#define LR_KEEP 3           // route rows shown: the chosen route and the next ones
#define LR_LEDGES 8         // ledges remembered
#define LR_WAIT 8           // most frames let go before the fastfall or jump
#define LR_FF 8             // most frames held down before the jump
#define LR_DJ_X 3           // sticks tried on the double jump
#define LR_CANDIDATES (2 * 2 * 2 * (LR_WAIT + 1) * (LR_FF + 1) * LR_DJ_X)
#define LR_ROUTES 48        // routes kept per ledge and kind (NIL, AI)
#define LR_WINDOWS 3        // press windows kept per aerial and candidate
#define LR_TRY (1 + 5 * LR_WINDOWS) // routes one candidate gives: a NIL and the aerials' windows
#define LR_SIM 45           // frames simulated after the jump
#define LR_BUDGET 1500      // simulated frames per game frame, on the ground or ledge
#define LR_BUDGET_AIR 400   // ... and while the live prediction runs too
#define LR_TIME_US 2000     // the most real time the search takes in a frame (it can run over by one route)

// The stick on the double jump: all the way in, 45 degrees down and in (a
// partial drift that clears the underside of stages like Battlefield's when
// a full one would bonk it), or let go.
enum lr_dj_x
{
    LR_DJ_IN,
    LR_DJ_DIAG,
    LR_DJ_UP,
};
static const float lr_dj_stick[LR_DJ_X] = {1.f, 0.7f, 0.f};
#define LR_PATH 72          // frames of the shown route's path
#define LR_HANG_X 2.4527f   // where Falcon hangs from a ledge: out from it
#define LR_HANG_Y 23.0962f  // ... and down
#define LR_GALINT_BIAS 0    // GALINT is read as ledgedash training reads it
#define LR_PLAIN 9999       // Route_Fail: the message as is

enum route_drop
{
    DROP_AWAY,
    DROP_DOWN,
};

typedef struct LedgeRoute
{
    u8 valid;
    u8 kind;    // LAND_NIL or LAND_AI
    u8 drop;    // DROP_AWAY or DROP_DOWN
    u8 wait;    // frames after the drop with the stick let go
    u8 ff;      // frames holding down after that; the fastfall starts on the first
    u8 hold;    // after the jump: 1 = keep holding toward the stage
    u8 dj_x;    // the stick on the jump: LR_DJ_IN, LR_DJ_DIAG or LR_DJ_UP
    u8 fall_away; // before the jump: drift away from the stage (down-away to fastfall)
    u8 aerial;  // AI: the aerial (TS_AIR*)
    u8 press_w; // AI: frames the aerial's window lasts
    s16 dj;     // frame of the double jump; the drop is frame 0
    s16 press;  // AI: first frame of the aerial's window
    s16 land;   // touchdown
    s16 act;    // the frame GALINT is read on: the touchdown for a NIL, the end of the landing lag for an AI
    s16 ff_at;  // frame the fastfall starts, -1 none
} LedgeRoute;

// The 15 GALINT reference (Battlefield's right ledge, from a natural
// regrab): let go away on the first frame it works, double jump in on the
// next, bair 4 frames later, land 5 after that, and the normal landing's
// lag is over 4 frames on. Frames from the drop, as Codex's engine trace
// (release 1603, jump 1604, bair 1608) and dash probe (relative 63) have it.
static const LedgeRoute ref_15galint = {
    .valid = 1, .kind = LAND_AI, .drop = DROP_AWAY, .hold = 1, .dj_x = LR_DJ_IN,
    .aerial = TS_AIRB, .press_w = 1, .dj = 1, .press = 5, .land = 10, .act = 14, .ff_at = -1,
};

// The routes a search found: for each order the Route Sort option can ask
// for (GALINT, Easiest) and each kind (NIL, AI), the best LR_ROUTES in that
// order. Each order is kept on its own, so Easiest finds the easy routes
// that keep little GALINT too, not only the easiest of the best ones.
typedef struct RouteList
{
    int n[2][2];                    // routes kept: [order][kind]
    LedgeRoute r[2][2][LR_ROUTES];  // ... best first
} RouteList;

typedef struct LedgeEntry
{
    u8 used;
    u8 done;     // the search has been through every candidate
    u8 have;     // a search finished: list holds its routes
    u8 logged;   // its routes went to the log
    s8 facing;   // the way Falcon faces while hanging: toward the stage
    float x, y;  // where he hangs
    int next;    // the next candidate to simulate
    int version; // finished searches, so what was built from the routes knows they changed
    RouteList list; // the last finished search's routes; it keeps showing while the next one runs
    u8 has_pick;      // a route was chosen on this ledge ...
    LedgeRoute pick;  // ... this one, found again when the list is rebuilt
} LedgeEntry;

static LedgeEntry *ledges; // [LR_LEDGES], allocated in Event_Init
static RouteList *route_build;   // the search under way fills this, one ledge at a time
static int route_build_ledge = -1; // ... the ledge it is for, -1 none
static Prediction *pred_route;
static Vec2 *route_path;          // [LR_PATH]
static float *route_path_bottom;  // [LR_PATH]
static int route_path_num;
static LedgeRoute route_path_of;  // the route the path is for
static int route_path_ledge = -1;
static int ledges_found;

// the browser: the routes of one ledge in the order the menu chose
static LedgeRoute *route_list;    // [2 * LR_ROUTES], allocated in Event_Init
static int route_list_num;
static int route_list_ledge = -1; // ledge it was built for
static int route_list_version;    // ... from which search
static int route_list_kind = -1;  // ... and with which Route Kind and Route Sort
static int route_list_sort = -1;
static int route_browse = -1;     // the ledge the Route option browses: where Falcon hangs, or last did

// while hanging
static int hang_ledge = -1;   // the ledge Falcon hangs from, -1 none
static LedgeRoute route_show[LR_KEEP]; // the chosen route, then the ones after it
static int route_show_num;
static int route_show_rank;   // the chosen route's place in the list, 0 first

// after letting go
static LedgeRoute route_cur;
static int route_cur_num;     // its place in the browser's list, 1 first
static int route_cur_total;   // ... of this many
static int route_ledge;       // ledge it started from
static int route_drop;        // game frame of the drop (the first frame falling)
static int route_e;           // frames since the drop
static int route_hit;         // game frame of the last step done on time, for its burst
static int route_fail = -100; // game frame a step went wrong, for the implosion
static int route_landed = -1; // frames since the drop at touchdown, -1 not yet
static int route_act;         // ... and when Falcon could act after it
static int route_galint = -1; // measured GALINT
static int route_pred;        // GALINT predicted on the drop frame, for the log
static int route_aerial_done; // the route's aerial was pressed
static int route_ff_done;     // ... and its fastfall started (or was given up on)
static int route_off;         // a jump or aerial came off its frame: the cues take over from the route's row
static int route_bonked;      // hit a ceiling on the way
#define COLL_CEILING 0x6000   // CollData env flags: touching a ceiling (Collide_CeilingMask)
static int route_any_aerial;  // an aerial was pressed after the drop
static char route_note[24];   // the first slip that didn't count as a miss, for the result
static int prev_fastfall;

// a practice attempt from the ledge, for Reset
static int attempt_active;
static int attempt_facing;
static int drill_timer = -1; // frames until starting over, -1 none
static int drill_success;
static int drill_side = 1;   // facing on the ledge to start from

static int assist_wait; // real frames Assist has waited

static void Route_Text(LedgeRoute *r, int galint, int num, int total);
static void Drill_Finish(int success);

static int Routes_On(void)
{
    return Options_Ledge[LOPT_ROUTES].val || Options_Ledge[LOPT_ASSIST].val;
}

static int Ledge_Find(float x, float y, int facing)
{
    for (int i = 0; i < LR_LEDGES; i++)
    {
        LedgeEntry *L = &ledges[i];
        if (L->used && L->facing == facing && fabs(L->x - x) < 1.5f && fabs(L->y - y) < 1.5f)
            return i;
    }
    return -1;
}

static int Ledge_Add(float x, float y, int facing)
{
    int i = Ledge_Find(x, y, facing);
    if (i >= 0)
    {
        LedgeEntry *L = &ledges[i];
        if (fabs(L->x - x) > 0.01f || fabs(L->y - y) > 0.01f)
        {
            // learned where Falcon really hangs: search again from there.
            // The routes of the search before keep showing until this one
            // is done.
            L->x = x;
            L->y = y;
            L->done = 0;
            L->next = 0;
            if (route_build_ledge == i)
                route_build_ledge = -1; // what it found so far is for the old spot
        }
        return i;
    }
    for (i = 0; i < LR_LEDGES; i++)
    {
        LedgeEntry *L = &ledges[i];
        if (L->used)
            continue;
        memset(L, 0, sizeof(*L));
        L->used = 1;
        L->facing = facing;
        L->x = x;
        L->y = y;
        return i;
    }
    return -1;
}

// The main stage's ledges, as Script_PlaceLedge finds them: the outermost
// ends of the ledge floors. Falcon hangs at a fixed offset from them.
static void Ledge_FindMain(void)
{
    CollLine *lines = *stc_collline;
    for (int side = -1; side <= 1; side += 2)
    {
        int best = -1;
        Vec3 best_end = {0, 0, 0};
        for (CollGroup *group = *stc_firstcollgroup; group != 0; group = group->next)
        {
            int first = group->desc->floor_start;
            for (int i = first; i < first + group->desc->floor_num; i++)
            {
                if (!lines[i].desc->is_ledge)
                    continue;
                Vec3 end;
                if (side > 0)
                    GrColl_GetGroundLineEndLeft(i, &end);
                else
                    GrColl_GetGroundLineEndRight(i, &end);
                if (best < 0 || (side > 0 ? end.X < best_end.X : end.X > best_end.X))
                {
                    best = i;
                    best_end = end;
                }
            }
        }
        if (best >= 0)
            Ledge_Add(best_end.X - side * LR_HANG_X, best_end.Y - LR_HANG_Y, side);
    }
}

// Falcon on the first frame after letting go, before it runs: falling from
// where he hung, standing still.
static void Ledge_Start(LedgeEntry *L, SimStart *s)
{
    EcbSample *e = Ecb_Get(TS_FALL, 0);
    memset(s, 0, sizeof(*s));
    s->pos = (Vec2){L->x, L->y};
    s->facing = L->facing;
    s->bottom = e->has_bottom ? e->bottom : 2.f;
    s->locked_bottom = s->bottom;
    s->ecb = (SimEcb){s->bottom, s->bottom + 16.f, s->bottom + 8.f, -3.f, 3.f};
    if (e->has_shape)
        Ecb_FromSample(e, L->facing, &s->ecb);
    Ecb_Fix(&s->ecb);
    s->ts = TS_FALL;
    s->frame = -1;
    s->tilt_timer = LL_TIMER_MAX;
    s->trigger_timer = LL_TIMER_MAX;
    s->skip_line = -1;
    s->jumps = 1; // the ledge gives the jumps back
}

// The order an aerial is offered in when several interrupt in the same way:
// the easiest press first.
static int Aerial_Order(int rank)
{
    static const u8 order[5] = {TS_AIRN, TS_AIRF, TS_AIRB, TS_AIRLW, TS_AIRHI};
    return order[rank];
}

static int Aerial_Rank(int aerial)
{
    for (int i = 0; i < 5; i++)
    {
        if (Aerial_Order(i) == aerial)
            return i;
    }
    return 5;
}

// One route: the drop, wait frames let go, ff frames held down, the double
// jump (stick in, down-in or let go), then holding toward the stage or not.
// With fall_away the stick drifts away from the stage until the jump (down-
// away for the fastfall), for room under the stage's lip. Gives, in out
// (up to LR_TRY, n of them), a NIL when it lands with no lag, and for each
// aerial the first few windows of press frames that touch down together
// with no aerial lag. Of those the best is the touchdown soonest, then the
// widest window, then the aerial Aerial_Order offers first (Route_Better
// sorts them that way). With path, also records where Falcon goes, frame by
// frame from the drop to the touchdown.
static void Ledge_Try(FighterData *fp, LedgeEntry *L, int drop, int wait, int ff, int hold, int dj_x, int fall_away,
                      LedgeRoute *out, int *n_out, Vec2 *path, float *bottom, int *num)
{
    SimStart st;
    SimState s;
    SimStep step;
    Ledge_Start(L, &st);
    Sim_Init(&st, &s);
    *n_out = 0;
    int dj = 1 + wait + ff;
    int prev_down = 0, ff_at = -1, n = 0;
    float tilt_dz = Common_Float(0xC);
    if (path)
    {
        path[n] = (Vec2){s.x, s.y};
        bottom[n++] = s.bottom;
    }

    for (int f = 0; f < dj; f++)
    {
        float sx = 0, sy = 0;
        if (f == 0)
        {
            if (drop == DROP_AWAY)
                sx = -L->facing;
            else
                sy = -1.f;
        }
        else if (f > wait)
        {
            sy = fall_away ? -0.7f : -1.f;
            sx = fall_away ? -0.7f * L->facing : 0;
        }
        else if (fall_away)
            sx = -L->facing;
        st.stick_x = sx;
        st.stick_y = sy;
        // the stick's timer counts frames since it was pulled down
        int down = sy <= -tilt_dz;
        s.tilt_timer = down ? (prev_down ? s.tilt_timer : -1) : LL_TIMER_MAX;
        prev_down = down;
        Sim_Step(fp, &st, &s, -1, 0, &step);
        if (step.fastfall_started)
            ff_at = f;
        if (path && n < LR_PATH)
        {
            path[n] = (Vec2){s.x, s.y};
            bottom[n++] = s.bottom;
        }
        if (step.landed)
            return;
    }
    if (ff > 0 && ff_at < 0)
        return; // held down without a fastfall: the same as waiting

    st.stick_x = L->facing * lr_dj_stick[dj_x];
    st.stick_y = dj_x == LR_DJ_DIAG ? -0.7f : 0;
    Sim_DoubleJump(fp, &st, &s, &step);
    if (path && n < LR_PATH)
    {
        path[n] = (Vec2){s.x, s.y};
        bottom[n++] = s.bottom;
    }
    if (step.landed || step.ceiling)
        return;

    st.stick_x = hold ? L->facing : 0;
    st.stick_y = 0;
    SimStart ps;
    Sim_ToStart(&s, &st, &ps);
    sim_limit = LR_SIM;
    sim_bottom_y = L->y - 40.f;
    Predict(fp, &ps, pred_route, BR_AI);
    sim_limit = LL_SIM_FRAMES;
    sim_bottom_y = -100000.f;

    Prediction *p = pred_route;
    if (path)
    {
        int last = p->land_frame ? p->land_frame : p->num;
        for (int k = 1; k <= last && n < LR_PATH; k++)
        {
            path[n] = p->pos[k];
            bottom[n++] = p->bottom[k];
        }
        *num = n;
    }

    LedgeRoute base = {0};
    base.drop = drop;
    base.wait = wait;
    base.ff = ff;
    base.hold = hold;
    base.dj_x = dj_x;
    base.fall_away = fall_away;
    base.dj = dj;
    base.press = -1;
    base.ff_at = ff_at;

    int nr = 0;
    if (p->land_frame && p->land_kind == LAND_NIL && p->uncertain_from > p->land_frame)
    {
        LedgeRoute *r = &out[nr++];
        *r = base;
        r->valid = 1;
        r->kind = LAND_NIL;
        r->land = dj + p->land_frame;
        r->act = r->land;
    }

    // An aerial pressed on frame k touches down when its own ECB comes into
    // use, at k + its delay: during the post-jump lock that's when the lock
    // runs out, so presses on several frames can share one touchdown (a
    // window). The aerials touch down at different frames, so each is
    // followed on its own.
    int last = p->land_frame ? p->land_frame - 1 : p->num;
    for (int a = 0; a < 5; a++)
    {
        int aerial = Aerial_Order(a);
        u8 bit = AERIAL_BIT(aerial);
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
            LedgeRoute *r = &out[nr++];
            *r = base;
            r->valid = 1;
            r->kind = LAND_AI;
            r->aerial = aerial;
            r->press = dj + k;
            r->press_w = w;
            r->land = dj + touch;
            r->act = r->land + (int)fp->attr.normal_landing_lag;
            windows++;
            k += w - 1;
        }
    }
    *n_out = nr;
}

// Better: acts sooner; then a wider aerial window (a NIL has no aerial to
// time, so it's the widest); then fewer inputs.
static int Route_Wide(LedgeRoute *r)
{
    return r->kind == LAND_NIL ? 99 : r->press_w;
}

static int Route_Better(LedgeRoute *a, LedgeRoute *b)
{
    if (!b->valid)
        return a->valid;
    if (!a->valid)
        return 0;
    if (a->act != b->act)
        return a->act < b->act;
    if (Route_Wide(a) != Route_Wide(b))
        return Route_Wide(a) > Route_Wide(b);
    if ((a->ff > 0) != (b->ff > 0))
        return a->ff == 0;
    if (a->wait + a->ff != b->wait + b->ff)
        return a->wait + a->ff < b->wait + b->ff;
    if (a->fall_away != b->fall_away)
        return !a->fall_away;
    if (a->dj_x != b->dj_x)
        return a->dj_x < b->dj_x;
    // nothing left to tell them apart by, so that the order never depends
    // on the order they were found in
    if (a->drop != b->drop)
        return a->drop < b->drop;
    if (a->hold != b->hold)
        return a->hold < b->hold;
    if (a->kind != b->kind)
        return a->kind == LAND_NIL;
    if (a->aerial != b->aerial)
        return Aerial_Rank(a->aerial) < Aerial_Rank(b->aerial);
    return a->press < b->press;
}

// Easier: fewer frame-exact inputs. A wider aerial window first, then no
// fastfall, then less waiting, then the one that keeps more GALINT.
static int Route_Easier(LedgeRoute *a, LedgeRoute *b)
{
    if (Route_Wide(a) != Route_Wide(b))
        return Route_Wide(a) > Route_Wide(b);
    if ((a->ff > 0) != (b->ff > 0))
        return a->ff == 0;
    if (a->wait + a->ff != b->wait + b->ff)
        return a->wait + a->ff < b->wait + b->ff;
    return Route_Better(a, b);
}

// The same route: the same inputs and the same press. Holding toward the
// stage after the jump or not doesn't make another one.
static int Route_Same(LedgeRoute *a, LedgeRoute *b)
{
    return a->kind == b->kind && a->drop == b->drop && a->wait == b->wait && a->ff == b->ff && a->dj_x == b->dj_x &&
           a->fall_away == b->fall_away && a->aerial == b->aerial && a->press == b->press;
}

static int Route_Galint(LedgeRoute *r, int e, int intang);

// r goes before b in the order Route Sort asked for.
static int Route_Order(LedgeRoute *a, LedgeRoute *b, int order)
{
    return order == ROUTES_SORT_EASIEST ? Route_Easier(a, b) : Route_Better(a, b);
}

// Put a route in one of the lists, in order. A route already there with the
// same inputs and press stays, or goes if this one comes before it; past
// LR_ROUTES the last one falls off.
static void Route_Insert(LedgeRoute *a, int *count, LedgeRoute *r, int order)
{
    int n = *count;
    for (int i = 0; i < n; i++)
    {
        if (!Route_Same(r, &a[i]))
            continue;
        if (!Route_Order(r, &a[i], order))
            return;
        for (int j = i; j < n - 1; j++)
            a[j] = a[j + 1];
        n--;
        break;
    }
    int at = n;
    while (at > 0 && Route_Order(r, &a[at - 1], order))
        at--;
    if (at < LR_ROUTES)
    {
        if (n == LR_ROUTES)
            n--;
        for (int j = n; j > at; j--)
            a[j] = a[j - 1];
        a[at] = *r;
        n++;
    }
    *count = n;
}

// Add a route the search found to the lists of both orders. One that keeps
// no GALINT even right after the grab isn't a route.
static void Ledge_Keep(RouteList *list, LedgeRoute *r)
{
    if (!r->valid || Route_Galint(r, -1, (*stc_ftcommon)->cliff_invuln_time) <= 0)
        return;
    int kind = r->kind == LAND_AI;
    for (int order = 0; order < 2; order++)
        Route_Insert(list->r[order][kind], &list->n[order][kind], r, order);
}

// Search on: the first ledge not done yet, until the budget of simulated
// frames or of real time is spent, whichever comes first. The routes of a
// finished search take the place of the ones shown before it all at once.
static void Ledge_Solve(FighterData *fp, int budget)
{
    if (!Routes_On())
        return;
    int at = -1;
    if (hang_ledge >= 0 && !ledges[hang_ledge].done)
        at = hang_ledge; // the ledge Falcon hangs from first
    for (int i = 0; i < LR_LEDGES && at < 0; i++)
    {
        if (ledges[i].used && !ledges[i].done)
            at = i;
    }
    if (at < 0)
        return;
    LedgeEntry *L = &ledges[at];

    // one search is kept at a time: moving to another ledge starts that one
    // over, and this one again later
    if (route_build_ledge != at)
    {
        if (route_build_ledge >= 0)
            ledges[route_build_ledge].next = 0;
        memset(route_build, 0, sizeof(RouteList));
        route_build_ledge = at;
        L->next = 0;
    }

    Floor_BuildCache();
    int start = sim_steps;
    int t0 = OSGetTick();
    while (L->next < LR_CANDIDATES && sim_steps - start < budget)
    {
        int c = L->next++;
        int drop = c % 2, hold = (c / 2) % 2, fall_away = (c / 4) % 2;
        c /= 8;
        int wait = c % (LR_WAIT + 1), ff = (c / (LR_WAIT + 1)) % (LR_FF + 1);
        int dj_x = c / ((LR_WAIT + 1) * (LR_FF + 1));
        LedgeRoute found[LR_TRY];
        int found_n;
        Ledge_Try(fp, L, drop, wait, ff, hold, dj_x, fall_away, found, &found_n, 0, 0, 0);
        for (int i = 0; i < found_n; i++)
            Ledge_Keep(route_build, &found[i]);
        if (OSTicksToMicroseconds(OSGetTick() - t0) >= LR_TIME_US)
            break;
    }
    if (L->next >= LR_CANDIDATES)
    {
        memcpy(&L->list, route_build, sizeof(RouteList));
        route_build_ledge = -1;
        L->done = 1;
        L->have = 1;
        L->version++;
        L->logged = 0;
    }
}

static void Route_MenuText(void);

// The ledge's routes in the order the Route Sort option chose, of the kind
// Route Kind chose, for the Route option to browse. Rebuilt when the ledge,
// its search or either option changes, and then the Route number is held to
// the routes there are.
// The same inputs (and aerial) as r: the route found again after a new
// search or in another order. Its frames may have moved by one, so an exact
// match wins over the first one with the same inputs. -1 none.
static int Route_Find(LedgeRoute *list, int n, LedgeRoute *r)
{
    int near = -1;
    for (int i = 0; i < n; i++)
    {
        LedgeRoute *a = &list[i];
        if (a->kind != r->kind || a->drop != r->drop || a->wait != r->wait || a->ff != r->ff || a->hold != r->hold ||
            a->dj_x != r->dj_x || a->fall_away != r->fall_away || (r->kind == LAND_AI && a->aerial != r->aerial))
            continue;
        if (a->dj == r->dj && a->press == r->press)
            return i;
        if (near < 0)
            near = i;
    }
    return near;
}

// Returns 1 when it built the list anew.
static int Routes_Update(int ledge)
{
    int kinds = Options_Ledge[LOPT_KIND].val;
    int sort = Options_Ledge[LOPT_SORT].val;
    LedgeEntry *L = ledge >= 0 ? &ledges[ledge] : 0;
    int have = L && L->have;
    int version = have ? L->version : -1;
    if (ledge == route_list_ledge && version == route_list_version && kinds == route_list_kind && sort == route_list_sort)
        return 0;
    route_list_ledge = ledge;
    route_list_version = version;
    route_list_kind = kinds;
    route_list_sort = sort;
    route_list_num = 0;

    if (have)
    {
        // each kind's list is in the chosen order already: merge them
        RouteList *list = &L->list;
        int n = 0, i = 0, j = 0;
        int ni = kinds == ROUTES_AI ? 0 : list->n[sort][0];
        int nj = kinds == ROUTES_NIL ? 0 : list->n[sort][1];
        while (i < ni || j < nj)
        {
            if (j >= nj || (i < ni && Route_Order(&list->r[sort][0][i], &list->r[sort][1][j], sort)))
                route_list[n++] = list->r[sort][0][i++];
            else
                route_list[n++] = list->r[sort][1][j++];
        }
        route_list_num = n;

        // the Route number is 1 to the routes found. The route chosen on
        // this ledge stays chosen when the list is put in another order or
        // searched again. Each order keeps its own best routes, so it may
        // not be in this one: then the list starts at its first route, and
        // the choice comes back with an order that has it. A ledge with
        // none chosen keeps the number.
        EventOption *o = &Options_Ledge[LOPT_PICK];
        o->value_num = n > 0 ? n : 1;
        if (L->has_pick)
        {
            int found = Route_Find(route_list, n, &L->pick);
            o->val = found >= 0 ? found + 1 : 1;
        }
        if (o->val > o->value_num)
            o->val = o->value_num;
        if (o->val < 1)
            o->val = 1;
    }
    Route_MenuText();
    return 1;
}

// Remember the route the Route option shows as the one chosen on its ledge.
static void Route_RememberPick(void)
{
    int pick = Options_Ledge[LOPT_PICK].val - 1;
    if (route_list_ledge < 0 || pick < 0 || pick >= route_list_num)
        return;
    LedgeEntry *L = &ledges[route_list_ledge];
    L->pick = route_list[pick];
    L->has_pick = 1;
}

// The routes the rows show: the chosen route, then the ones after it in
// the list's order.
static void Routes_Show(void)
{
    route_show_num = 0;
    route_show_rank = 0;
    if (route_list_num == 0)
        return;
    int pick = Options_Ledge[LOPT_PICK].val - 1;
    if (pick >= route_list_num)
        pick = route_list_num - 1;
    if (pick < 0)
        pick = 0;
    route_show_rank = pick;
    for (int i = 0; i < LR_KEEP && pick + i < route_list_num; i++)
        route_show[route_show_num++] = route_list[pick + i];
}

// GALINT the route keeps if it goes as planned, e frames after the drop
// (-1 while still hanging).
static int Route_Galint(LedgeRoute *r, int e, int intang)
{
    return intang - (r->act - e) + LR_GALINT_BIAS;
}

static void Route_Path(FighterData *fp, int ledge, LedgeRoute *r)
{
    LedgeRoute *o = &route_path_of;
    if (route_path_ledge == ledge && o->drop == r->drop && o->wait == r->wait && o->ff == r->ff && o->hold == r->hold &&
        o->dj_x == r->dj_x && o->fall_away == r->fall_away && o->kind == r->kind)
        return;
    LedgeRoute found[LR_TRY];
    int found_n;
    Floor_BuildCache();
    Ledge_Try(fp, &ledges[ledge], r->drop, r->wait, r->ff, r->hold, r->dj_x, r->fall_away, found, &found_n, route_path,
              route_path_bottom, &route_path_num);
    route_path_of = *r;
    route_path_ledge = ledge;
    if (cue_log)
    {
        // where the route's simulation has Falcon, frame by frame from the
        // drop, to check against a scripted run of it
        char buf[400];
        for (int k0 = 0; k0 < route_path_num; k0 += 10)
        {
            int n = sprintf(buf, "LLRPATH drop %d wait %d ff %d dj_x %d away %d hold %d %d", r->drop, r->wait, r->ff,
                            r->dj_x, r->fall_away, r->hold, k0);
            for (int k = k0; k < k0 + 10 && k < route_path_num; k++)
                n += sprintf(buf + n, " %.3f,%.3f,%.3f", route_path[k].X, route_path[k].Y, route_path_bottom[k]);
            sprintf(buf + n, "\n");
            Log(buf);
        }
    }
}

static void Route_Cell(MeterRow *row, int base, int n, int kind, int glyph)
{
    int k = base + n;
    Row_Set(row, k, kind, TONE_CUE, 1);
    if (k >= 0 && k < MT_CELLS)
        row->glyph[k] = glyph;
}

// The frame the route's fastfall starts, and the last frame its GALINT
// lasts (counting the frame it's read on, like Row_Tail).
static int Route_FF(LedgeRoute *r)
{
    return r->ff_at >= 0 ? r->ff_at : r->wait + 1;
}

static int Route_TailEnd(LedgeRoute *r, int galint)
{
    int act = r == &route_cur && route_landed >= 0 ? route_act : r->act;
    return act + galint - 1;
}

// The frame of the route's ACT cell: the first one Falcon can act on.
static int Route_ActCell(LedgeRoute *r)
{
    if (r == &route_cur && route_landed >= 0)
        return route_act == route_landed ? route_landed + 1 : route_act; // as it went
    return r->kind == LAND_AI ? r->act : r->land + 1;
}

// The route's cells, e frames after the drop (-1 while hanging): its inputs
// with their glyphs, frames in the air, the touchdown, the landing lag, the
// first frame Falcon can act and the GALINT left after it.
static void Row_Route(MeterRow *row, LedgeRoute *r, int e, int facing, int galint)
{
    int base = -e - 1; // cell of the drop
    int away = facing > 0 ? GLYPH_LEFT : GLYPH_RIGHT;
    int ff = r->ff > 0 ? Route_FF(r) : -1;
    int act = Route_ActCell(r);
    int ff_glyph = !r->fall_away ? GLYPH_FF : facing > 0 ? GLYPH_FF_DOWN_LEFT : GLYPH_FF_DOWN_RIGHT;
    Route_Cell(row, base, 0, CELL_PRESS, r->drop == DROP_AWAY ? away : GLYPH_DOWN);
    for (int n = 1; n < r->land; n++)
        Route_Cell(row, base, n, n == ff || n == r->dj ? CELL_PRESS : CELL_AIR,
                   n == ff ? ff_glyph : n == r->dj ? GLYPH_JUMP : 0);
    Route_Cell(row, base, r->land, CELL_LAND, 0);
    for (int n = r->land + 1; n < act; n++)
        Route_Cell(row, base, n, CELL_LAG, 0);
    Route_Cell(row, base, act, CELL_ACT, 0);
    for (int n = act + 1; n <= Route_TailEnd(r, galint); n++)
        Route_Cell(row, base, n, CELL_TAIL, 0);
    // the aerial's window, which can end on the touchdown itself
    if (r->kind == LAND_AI)
    {
        for (int n = r->press; n < r->press + r->press_w; n++)
            Route_Cell(row, base, n, CELL_PRESS, n == r->press ? GLYPH_AERIAL : 0);
    }

    // the press is next: light the gate
    int next = e + 1;
    int due = next == 0 || next == r->dj || next == ff ||
              (r->kind == LAND_AI && next >= r->press && next < r->press + r->press_w);
    if (due && (e >= 0 || assist_frozen))
        row->hot = 2;
    if (galint > 0)
        sprintf(row->info, "%d GALINT", galint);
}

// 1st, 2nd, 3rd, 4th ... 11th, 12th, 13th ... 21st
static const char *Ordinal(int n)
{
    static char text[16];
    int last = n % 10;
    const char *suffix = last == 1 ? "st" : last == 2 ? "nd" : last == 3 ? "rd" : "th";
    if (n % 100 >= 11 && n % 100 <= 13)
        suffix = "th";
    sprintf(text, "%d%s", n, suffix);
    return text;
}

static void Meter_AddRoutes(void)
{
    if (!Routes_On())
        return;
    FighterData *fp = Fighter_GetGObj(0)->userdata;
    int intang = fp->hurt.intang_frames.ledge;

    if (route_active && route_off)
        return; // off the route's frames: the cues' own rows time the rest
    if (route_active)
    {
        LedgeRoute *r = &route_cur;
        MeterRow *row = Meter_Add(land_kind_colors[r->kind], r->kind == LAND_AI ? "AI" : "NIL");
        if (!row)
            return;
        Row_Route(row, r, route_e, ledges[route_ledge].facing, route_galint >= 0 ? route_galint : Route_Galint(r, route_e, intang));
        int age = event_vars->game_timer - route_hit;
        if (age >= 0 && age < LL_BURST)
            row->burst = age;
        if (assist_frozen)
            Row_Ghost(row, (assist_wait / 2) % LL_GHOST, 1);
        int act_cell = Route_ActCell(r) - route_e - 1;
        if (act_cell <= 0 && act_cell > -4)
            row->flash = -act_cell;
        route_rows_active = 1;
        return;
    }

    int age = event_vars->game_timer - route_fail;
    if (age >= 0 && age < LL_FADE)
    {
        MeterRow *row = Meter_Add(land_kind_colors[route_cur.kind], "Route");
        if (row)
        {
            row->implode = age;
            row->implode_tone = TONE_MISS;
            row->len = 1;
            sprintf(row->info, "miss");
        }
    }

    if (hang_ledge < 0 || route_show_num == 0)
        return;
    for (int i = 0; i < route_show_num; i++)
    {
        LedgeRoute *r = &route_show[i];
        MeterRow *row = Meter_Add(land_kind_colors[r->kind], Ordinal(route_show_rank + i + 1));
        if (!row)
            return;
        Row_Route(row, r, -1, ledges[hang_ledge].facing, Route_Galint(r, -1, intang));
        row->dim = i > 0;
        if (i == 0 && assist_frozen)
            Row_Ghost(row, (assist_wait / 2) % LL_GHOST, 1);
    }
    route_rows_active = 1;
}
