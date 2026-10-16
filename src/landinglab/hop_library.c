/* landinglab/hop_library.c
 *
 * Holds the hop library: hops from the ground, built ahead of time per
 * height.
 *
 * Part of Landing Gear: src/landinglab.c includes it, in order, into one
 * translation unit, so it sees everything included before it (and mex.h,
 * events.h, landinglab_sim.h).
 *
 * Uses: tracking_state, sim_dump, ledge_routes, jump_timing, jump_tracking.
 * Refers ahead, through forward declarations, to: jump_search.
 */

///////////////////////
/// Hop library     ///
///////////////////////

// Hops from the ground at any speed (Stephen, 0.8.6: notes "grounded at any
// speed, on platform surfaces at any speed"). A hop has GT_STILL_CANDIDATES
// variants and a run has a hop start on every frame, too many to predict.
// But how high Falcon is never depends on how fast he moves sideways
// (gravity, jumps, fastfall and the states' lengths are all vertical; only
// jumping forward or back changes the animation, so the ECB), so whether a
// variant can NIL or AI onto a floor h above the takeoff, and on which
// frames, is the same from anywhere at any speed. The library predicts each
// variant once against an endless flat floor at each height the stage has,
// with no walls or ceilings, and keeps the touchdown frames. A candidate
// then only needs its sideways path, which is cheap, to see whether a
// touchdown falls on a real floor at that height; only those get the real
// prediction, so what's shown is exact and the library only picks what's
// worth trying. Plan: landing-trainer/plan-hop-library.md.
#define GL_HEIGHTS 12   // floor heights, relative to the floor jumped from
#define GL_TOUCH 5      // touchdown frames kept per variant and height
#define GL_MARGIN 3.f   // how far past a floor's end a touchdown still counts as maybe on it
#define GL_KEYS 2       // jumping forward or back
#define GL_X (JT_SIM + GT_HJ_MAX + 12) // sideways path frames from the takeoff
// The best route of a kind the variant has onto the endless floor, frames
// from the hop's press frame (d = 0), so a moving hop can lay it out without
// a prediction of its own (Gl_Direct).
typedef struct GlRoute
{
    u8 ok;
    u8 aerial;
    u8 press;
    u8 press_w;
    u8 land;
    u8 act;
    u8 late;
} GlRoute;
typedef struct GlEntry
{
    u8 built;
    u8 unsure; // the prediction relied on unlearned ECB frames: always worth trying
    u8 n;
    u8 t[GL_TOUCH]; // touchdown frames from the hop's press frame (d = 0)
    GlRoute r[2];   // the best NIL [0] and AI [1]
} GlEntry;
static GlEntry *gl_lib; // [GL_KEYS][GT_STILL_CANDIDATES][GL_HEIGHTS]
static float gl_hval[GL_HEIGHTS];
static u8 gl_hplat[GL_HEIGHTS];
static int gl_hnum;
static int gl_gen = -1; // the options the library was built under
static int gl_build_steps;
static int gl_idle_v, gl_idle_h; // where the background build is
static int gl_idle_sig = -1;     // ... for which key and rows

// The heights of the anchor's floor's neighbors, as library rows
static int gl_ah[GL_HEIGHTS];  // library height index of each
static int gl_an;
static float gl_ay;            // the anchor floor's height

// Facing left is facing right in a mirror: the same heights, with the
// double jump's up-left and up-right swapped (which one is backwards
// changes the animation).
static GlEntry *Gl_Entry(int key, int v, int h)
{
    if (jt_start.facing < 0 && v >= 4)
    {
        int r = (v - 4) % GT_STILL_STICKS;
        if (r != JT_UP)
            v += (r == JT_UPLEFT ? JT_UPRIGHT : JT_UPLEFT) - r;
    }
    return &gl_lib[(key * GT_STILL_CANDIDATES + v) * GL_HEIGHTS + h];
}

static int Gl_Key(void)
{
    return jt_stick_x * jt_start.facing <= -common_jump_back_stick; // jumping backwards
}

static void Gl_CheckGen(void)
{
    int gen = ai_only | (ai_show_all << 8);
    if (gen == gl_gen)
        return;
    gl_gen = gen;
    memset(gl_lib, 0, sizeof(GlEntry) * GL_KEYS * GT_STILL_CANDIDATES * GL_HEIGHTS);
    gl_hnum = 0;
    gl_idle_v = gl_idle_h = 0;
}

// The floors' heights from the anchor's floor, as library height rows
static void Gl_DjPaths(FighterData *fp);
static int gl_path_d[2] = {-1, -1}; // the frame each hop's sideways path is from (Gl_Path)
static void Gl_Anchor(FighterData *fp)
{
    Gl_CheckGen();
    gl_path_d[0] = gl_path_d[1] = -1;
    gl_pass = gl_skip = 0;
    gl_direct = gl_verified = gl_rejected = 0;
    Gl_DjPaths(fp);
    gl_an = 0;
    gl_ay = gt_steps[0].y;
    for (int i = 0; i < floor_num; i++)
    {
        FloorLine *f = &floor_cache[i];
        float h = (f->y0 + f->y1) * 0.5f - gl_ay;
        int k;
        for (k = 0; k < gl_hnum; k++)
            if (fabs(gl_hval[k] - h) < 0.05f && gl_hplat[k] == f->is_platform)
                break;
        if (k == gl_hnum)
        {
            if (gl_hnum >= GL_HEIGHTS)
                continue;
            gl_hval[gl_hnum] = h;
            gl_hplat[gl_hnum++] = f->is_platform;
        }
        int dup = 0;
        for (int j = 0; j < gl_an; j++)
            dup |= gl_ah[j] == k;
        if (!dup && gl_an < GL_HEIGHTS)
            gl_ah[gl_an++] = k;
    }
    // the background build starts over for a new set of rows
    int sig = Gl_Key();
    for (int j = 0; j < gl_an; j++)
        sig = sig * 31 + gl_ah[j] + 1;
    if (sig != gl_idle_sig)
    {
        gl_idle_sig = sig;
        gl_idle_v = gl_idle_h = 0;
    }
}

// One library entry: the variant from a standstill at 0, 0 onto an endless
// floor at that height
static void Gl_Build(FighterData *fp, int key, int v, int h)
{
    GlEntry *g = Gl_Entry(key, v, h);
    JumpCand c;
    memset(g, 0, sizeof(*g));
    g->built = 1;
    Gt_Variant(v, &c); // built whatever the hop setting, so a change needs no rebuild
    c.d = 0;
    FloorLine vf = {-100000.f, gl_hval[h], 100000.f, gl_hval[h], -2, gl_hplat[h]};
    FloorLine *fc = floor_cache;
    int fn = floor_num, cn = ceil_num, w0 = wall_num[0], w1 = wall_num[1];
    GroundStep g0 = gt_steps[0];
    float bot = jt_bottom;
    int edge = gt_edge;
    floor_cache = &vf;
    floor_num = 1;
    ceil_num = 0;
    wall_num[0] = wall_num[1] = 0;
    gt_steps[0] = (GroundStep){0, 0, 0};
    jt_bottom = (gl_hval[h] < 0 ? gl_hval[h] : 0) - JT_BOTTOM;
    gt_edge = -1;
    int steps0 = sim_steps;
    JumpRoute found[JT_TRY];
    int found_n;
    Jump_Try(fp, &c, found, &found_n, 0, 0, 0);
    gl_build_steps += sim_steps - steps0;
    gl_builds++;
    floor_cache = fc;
    floor_num = fn;
    ceil_num = cn;
    wall_num[0] = w0;
    wall_num[1] = w1;
    gt_steps[0] = g0;
    jt_bottom = bot;
    gt_edge = edge;
    if (jt_try_predicted)
    {
        Prediction *p = pred_route;
        if (p->uncertain_from <= (p->land_frame ? p->land_frame : p->num))
            g->unsure = 1;
    }
    for (int i = 0; i < found_n && g->n < GL_TOUCH; i++)
    {
        int t = found[i].land;
        int dup = 0;
        for (int j = 0; j < g->n; j++)
            dup |= g->t[j] == t;
        if (!dup && t > 0 && t < 256)
            g->t[g->n++] = t;
    }
    JumpRoute *best[2] = {0, 0};
    for (int i = 0; i < found_n; i++)
    {
        JumpRoute *f = &found[i];
        int k = f->kind == LAND_AI;
        if (f->valid && f->act < 256 && f->land < 256 && (!best[k] || Jump_Better(f, best[k])))
            best[k] = f;
    }
    for (int k = 0; k < 2; k++)
    {
        if (!best[k])
            continue;
        GlRoute *r = &g->r[k];
        r->ok = 1;
        r->aerial = best[k]->aerial;
        r->press = best[k]->press < 0 ? 255 : best[k]->press;
        r->press_w = best[k]->press_w;
        r->land = best[k]->land;
        r->act = best[k]->act;
        r->late = best[k]->late;
    }
}

// Sideways only: where Falcon is each frame on from x, vx in state ts
// (frame, len), the stick at first for the first step and rest after. Mirrors
// Sim_Step's x: a state running out, the fall's clamp, drift.
static void Gl_Steps(FighterData *fp, float x, float vx, int ts, int frame, int len, float first, float rest, float *out,
                     int n)
{
    float stick = first;
    out[0] = x;
    for (int k = 1; k < n; k++)
    {
        frame++;
        if (len > 0 && frame >= len)
        {
            int next = Tracked_Next(ts);
            if (next == TS_FALL)
            {
                float max = fp->attr.aerial_drift_max;
                vx = vx < -max ? -max : vx > max ? max : vx;
            }
            ts = next;
            frame = 0;
            len = ts >= 0 ? state_len[ts] : 0;
        }
        vx += Is_MultiJump(ts) ? Mj_DriftAccel(fp, vx, stick) : Drift_Accel(fp, vx, stick);
        x += vx;
        out[k] = x;
        stick = rest;
    }
}

// From a frame's takeoff: x k frames on with the stick kept (gl_pre_sx) or
// let go (gl_pre_0); and, per anchor, x n frames after a double jump from
// 0 for each standstill stick (it sets the speed, so what came before
// doesn't matter)
static float *gl_pre_sx, *gl_pre_0; // [GL_X], the hop's own in gl_pre_buf
static float *gl_pre_buf;           // [2 hops][kept, let go][GL_X]
static float *gl_dj;                // [GT_STILL_STICKS][GL_X]
static int gl_path_until, gl_path_until2[2];

// A frame's hop's sideways paths, kept for each hop: the candidates go
// short hop, full hop, short hop... at each frame, and with one kept they
// were made again for nearly every one (repro3e, 2026-10-15: moving, a
// plan's frame took 87 ms)
static void Gl_Path(FighterData *fp, JumpCand *c)
{
    int h = c->hop == GT_FH;
    gl_pre_sx = &gl_pre_buf[(h * 2) * GL_X];
    gl_pre_0 = &gl_pre_buf[(h * 2 + 1) * GL_X];
    if (gl_path_d[h] != c->d)
    {
        GroundStep *gs = &gt_steps[c->d];
        SimStart ps;
        gl_path_until2[h] = Sim_GroundJumpAt(fp, c->hop == GT_SH, gs->x, gs->y, gs->v, jt_stick_x, 1, &ps, 0);
        Gl_Steps(fp, ps.pos.X, ps.vel.X, ps.ts, 0, ps.len, jt_stick_x, jt_stick_x, gl_pre_sx, GL_X);
        Gl_Steps(fp, ps.pos.X, ps.vel.X, ps.ts, 0, ps.len, 0, 0, gl_pre_0, GL_X);
        gl_path_d[h] = c->d;
    }
    gl_path_until = gl_path_until2[h];
}

static void Gl_DjPaths(FighterData *fp)
{
    for (int st = 0; st < GT_STILL_STICKS; st++)
    {
        float dj_x = jt_stick_xy[st][0];
        int ts = Dj_State(dj_x * fp->facing_direction);
        float vx = dj_x * fp->attr.air_jump_h_multiplier;
        if (sim_mj)
        {
            // Kirby's and Jigglypuff's first air jump, after the hop (Mj_Enter)
            ts = TS_MJ1;
            vx = dj_x * fp->multi_jump_desc->vel_stick_x_mult;
        }
        int len = state_len[ts] ? state_len[ts] : LL_GUESS_JUMP_LEN;
        // Gt_Variant holds the x of a sideways double jump, lets go of an up one
        Gl_Steps(fp, 0, vx, ts, -1, len, dj_x, st != JT_UP ? dj_x : 0, &gl_dj[st * GL_X], GL_X);
    }
}

// Whether a moving candidate is worth predicting: one of its touchdowns
// from the library falls on a real floor at that height.
static int Gl_Worth(FighterData *fp, JumpCand *c, int build)
{
    int key = Gl_Key();
    int v = c->v;
    // the library rows first: one not built yet means it can't say
    int any = 0;
    for (int j = 0; j < gl_an; j++)
    {
        GlEntry *g = Gl_Entry(key, v, gl_ah[j]);
        if (!g->built)
        {
            if (!build)
                return 1;
            Gl_Build(fp, key, v, gl_ah[j]);
        }
        if (g->unsure)
            return 1;
        any |= g->n > 0;
    }
    if (!any)
        return 0;

    GroundStep *gs = &gt_steps[c->d];
    Gl_Path(fp, c);
    for (int j = 0; j < gl_an; j++)
    {
        GlEntry *g = Gl_Entry(key, v, gl_ah[j]);
        float y = gs->y + gl_hval[gl_ah[j]];
        for (int i = 0; i < g->n; i++)
        {
            int k = g->t[i] - gl_path_until; // frames after the takeoff
            float x;
            if (c->hj < 0)
            {
                if (k < 1 || k >= GL_X)
                    continue;
                x = (c->hold ? gl_pre_sx : gl_pre_0)[k];
            }
            else
            {
                int n = k - c->hj; // frames from the double jump's own, which is 1
                if (c->hj >= GL_X || n < 1 || n >= GL_X)
                    continue;
                x = (c->neutral ? gl_pre_0 : gl_pre_sx)[c->hj] + gl_dj[c->stick * GL_X + n];
            }
            for (int f = 0; f < floor_num; f++)
            {
                FloorLine *fl = &floor_cache[f];
                float fy = (fl->y0 + fl->y1) * 0.5f;
                float lo = fl->x0 < fl->x1 ? fl->x0 : fl->x1, hi = fl->x0 < fl->x1 ? fl->x1 : fl->x0;
                if (fabs(fy - y) < 0.1f && x >= lo - GL_MARGIN && x <= hi + GL_MARGIN)
                    return 1;
            }
        }
    }
    return 0;
}

// A moving hop's routes straight from the library, without a prediction:
// its touchdown's x from the sideways path, kept only when it's well inside
// a floor at that height (an edge, a wall or a ledge needs the real thing).
// Returns -1 when the library can't say (a row not built, or unsure), else
// how many routes it laid out. Each is marked lib and predicted for real
// before it's shown (Jump_Verify). (Stephen, 0.9.29: plans took too long
// while moving; he still had to stand still for them.)
#define GL_INSIDE 6.f // how far in from a floor's end the touchdown must be
static int Gl_Direct(FighterData *fp, JumpCand *c, JumpRoute *out)
{
    int key = Gl_Key(), v = c->v, n = 0;
    for (int j = 0; j < gl_an; j++)
    {
        GlEntry *g = Gl_Entry(key, v, gl_ah[j]);
        if (!g->built || g->unsure)
            return -1;
    }
    GroundStep *gs = &gt_steps[c->d];
    Gl_Path(fp, c);
    for (int j = 0; j < gl_an && n < 2 * GL_HEIGHTS; j++)
    {
        GlEntry *g = Gl_Entry(key, v, gl_ah[j]);
        float y = gs->y + gl_hval[gl_ah[j]];
        for (int k = 0; k < 2; k++)
        {
            GlRoute *lr = &g->r[k];
            if (!lr->ok)
                continue;
            int t = lr->land - gl_path_until; // frames after the takeoff
            float x;
            if (c->hj < 0)
            {
                if (t < 1 || t >= GL_X)
                    continue;
                x = (c->hold ? gl_pre_sx : gl_pre_0)[t];
            }
            else
            {
                int m = t - c->hj;
                if (c->hj >= GL_X || m < 1 || m >= GL_X)
                    continue;
                x = (c->neutral ? gl_pre_0 : gl_pre_sx)[c->hj] + gl_dj[c->stick * GL_X + m];
            }
            int inside = 0;
            for (int f = 0; f < floor_num && !inside; f++)
            {
                FloorLine *fl = &floor_cache[f];
                float fy = (fl->y0 + fl->y1) * 0.5f;
                float lo = fl->x0 < fl->x1 ? fl->x0 : fl->x1, hi = fl->x0 < fl->x1 ? fl->x1 : fl->x0;
                inside = fabs(fy - y) < 0.1f && x >= lo + GL_INSIDE && x <= hi - GL_INSIDE;
            }
            if (!inside)
                continue;
            JumpRoute *r = &out[n++];
            memset(r, 0, sizeof(*r));
            r->valid = 1;
            r->lib = 1;
            r->kind = k ? LAND_AI : LAND_NIL;
            r->stick = c->stick;
            r->hold = c->hold;
            r->hop = c->hop;
            r->hj = c->hj;
            r->neutral = c->hj >= 0 && c->neutral;
            r->hp = c->d + 1;
            r->d = c->d;
            r->dj = c->hj >= 0 ? c->d + gl_path_until + c->hj + 1 : c->d + 1;
            r->changes = c->hj >= 0 ? 1 + Jump_Changes(c->stick, c->hold) + (c->neutral != 0) : 1 + !c->hold;
            r->late = lr->late;
            r->aerial = lr->aerial;
            r->press = lr->press == 255 ? -1 : c->d + lr->press;
            r->press_w = lr->press_w;
            r->land = c->d + lr->land;
            r->act = c->d + lr->act;
            r->where = Jump_Where(r, x, y);
        }
    }
    return n;
}

// While nothing else searches: build the library for where Falcon stands,
// so his hops are ready before he moves.
static void Gl_Idle(FighterData *fp, int budget)
{
    if (!jt_ground || gl_an == 0)
        return;
    int key = Gl_Key(), start = sim_steps, t0 = OSGetTick();
    while (gl_idle_v < GT_STILL_CANDIDATES && sim_steps - start < budget &&
           OSTicksToMicroseconds(OSGetTick() - t0) < LR_TIME_US)
    {
        GlEntry *g = Gl_Entry(key, gl_idle_v, gl_ah[gl_idle_h]);
        if (!g->built || g->unsure)
            Gl_Build(fp, key, gl_idle_v, gl_ah[gl_idle_h]);
        if (++gl_idle_h >= gl_an)
        {
            gl_idle_h = 0;
            if (++gl_idle_v >= GT_STILL_CANDIDATES && cue_log)
                OSReport("LLLIB built key %d heights %d: %d predictions, %d steps\n", key, gl_an, gl_builds,
                         gl_build_steps);
        }
    }
}
