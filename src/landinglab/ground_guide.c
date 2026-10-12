/* landinglab/ground_guide.c
 *
 * Holds the ground guide (only with LL_GROUND_GUIDE; shelved, see
 * docs/ground-guide.md).
 *
 * Part of Landing Lab: src/landinglab.c includes it, in order, into one
 * translation unit, so it sees everything included before it (and mex.h,
 * events.h, landinglab_sim.h).
 *
 * Uses: menus_practice, hud_text, drawing, meter_placement, debug_camera,
 * gx_draw, sim_dump, ledge_routes, jump_timing.
 */

#ifdef LL_GROUND_GUIDE // shelved until after 1.0 (Stephen, 2026-10-10): see docs/ground-guide.md
///////////////////////
/// Ground guide    ///
///////////////////////

// Jump timing from the ground, worked out ahead of time (Stephen, 0.8.3
// notes: takeoff bands plus reach marks). For each height a platform is over
// a floor on this stage, each hop, and five run speeds, every way on (the
// hop alone, or a double jump on each of its frames with each stick, its x
// kept or let go) is simulated once, from a takeoff press at x 0 in a world
// of just a floor and a platform at that height spanning everything. Where
// on the platform each one's NIL, rising AIs (their window's frames) and
// waveland windows (the hop alone) touch down is counted, by distance from
// the press. A spot on the floor then counts the timings that land on a
// platform's own stretch (Takeoff Bands), and a spot on the platform the
// ones that land there from where Falcon is (Reach Marks). Nothing waits on
// a search, and nothing picks one route: brightness is how many work.
//
// The tables never change for a character, so they can be baked: the script
// command "bake" works them out for every height from GD_LO to GD_HI and
// logs them (LLGB lines), tools/guide_bake.py turns the log into
// TM/llgdNN.bin (NN the character's kind), and a disc that has the file
// reads each height's table from it when a platform needs it, Fountain's
// moving ones too. Without the file they're worked out here, as before.
#define GD_SLOTS 8
#define GD_SPEEDS 5 // facing right: run back, walk back, still, walk, run; mirrored facing left
#define GD_HJ 24    // double jumps 1 to 24 frames after the takeoff
#define GD_DX 80    // distances -80 to 80
#define GD_BINS (2 * GD_DX + 1)
#define GD_COMBOS 9 // the double jump's stick and hold: up; the other four let go or held
#define GD_WAYS (2 + GD_HJ * GD_COMBOS) // the hop alone (stick kept or let go), then the double jumps
#define GD_PER_HEIGHT (2 * GD_SPEEDS * GD_WAYS)
#define GD_KINDS 7 // the AI with each aerial (Aerial_Order), then waveland, then NIL
#define GD_WL 5
#define GD_NIL 6
#define GD_BUDGET 1500
#define GD_TIME_US 1500
#define GD_BAKE_US 12000
#define GD_LO 10.f
#define GD_STEP 0.5f
#define GD_NUM 121 // 10 to 70
#define GD_MAGIC 0x4C4C4744 // "LLGD"
#define GD_VERSION 1
#define GD_DIMS (GD_SPEEDS | GD_KINDS << 8 | GD_DX << 16 | GD_HJ << 24)
typedef struct GuideTable
{
    u8 n[2][GD_SPEEDS][GD_KINDS][GD_BINS]; // [short 0 / full 1][speed][kind][distance]
} GuideTable;
#define GD_STRIDE ((sizeof(GuideTable) + 31) & ~31)
typedef struct GuideHead // the baked file's first 32 bytes; its tables follow, GD_STRIDE apart
{
    u32 magic, version, kind, num;
    float lo, step;
    u32 stride, dims;
} GuideHead;
static u8 *gd_mem;           // GD_SLOTS tables GD_STRIDE apart, 32-byte aligned for the disc
static float gd_height[GD_SLOTS];
static int gd_slice[GD_SLOTS]; // the baked height a slot holds, -1 none
static u32 gd_used[GD_SLOTS];  // the frame a slot was last drawn from
static int gd_heights = -1;    // -1: not looked at the stage yet
static int gd_next, gd_done;
static int gd_file = -1;       // the baked file's entry, -1 none (worked out here)
static GuideHead gd_head;

static GuideTable *Guide_Slot(int s)
{
    return (GuideTable *)(gd_mem + s * GD_STRIDE);
}

static void Guide_Add(GuideTable *t, int hop, int sp, int kind, float x, int count)
{
    int b = (int)(x + (x < 0 ? -0.5f : 0.5f)) + GD_DX;
    if (b < 0 || b >= GD_BINS)
        return;
    u8 *c = &t->n[hop][sp][kind][b];
    *c = *c + count > 255 ? 255 : *c + count;
}

// The heights platforms stand over the floors under them.
static void Guide_Heights(void)
{
    gd_heights = 0;
    for (int i = 0; i < floor_num; i++)
    {
        FloorLine *p = &floor_cache[i];
        if (!p->is_platform)
            continue;
        float py = (p->y0 + p->y1) * 0.5f;
        for (int j = 0; j < floor_num; j++)
        {
            FloorLine *f = &floor_cache[j];
            if (f->x1 < p->x0 - 40.f || f->x0 > p->x1 + 40.f)
                continue;
            float h = py - (f->y0 + f->y1) * 0.5f;
            if (h < 8.f || h > 90.f)
                continue;
            int have = 0;
            for (int k = 0; k < gd_heights; k++)
                if (fabs(gd_height[k] - h) < 1.f)
                    have = 1;
            if (!have && gd_heights < GD_SLOTS)
                gd_height[gd_heights++] = h;
        }
    }
    OSReport("LLGUIDE heights %d: %.1f %.1f %.1f %.1f\n", gd_heights, gd_heights > 0 ? gd_height[0] : 0,
             gd_heights > 1 ? gd_height[1] : 0, gd_heights > 2 ? gd_height[2] : 0, gd_heights > 3 ? gd_height[3] : 0);
}

static float Guide_Speed(FighterData *fp, int sp, float *stick)
{
    static const float stick_of[GD_SPEEDS] = {-1.f, -0.8f, 0.f, 0.8f, 1.f};
    float run = fp->attr.dashrun_terminal_velocity, walk = fp->attr.walk_maximum_velocity;
    float v[GD_SPEEDS] = {-run, -walk, 0, walk, run};
    *stick = stick_of[sp];
    return v[sp];
}

// One way on, index i of a height's GD_PER_HEIGHT, counted into t. Every
// aerial is counted on its own, so the table doesn't depend on AI Aerial.
static void Guide_Try(FighterData *fp, GuideTable *t, float H, int i)
{
    int hop = i / (GD_SPEEDS * GD_WAYS);
    int sp = (i / GD_WAYS) % GD_SPEEDS;
    int way = i % GD_WAYS;
    float stick_x;
    float v = Guide_Speed(fp, sp, &stick_x);

    // the world: a floor at 0 and a platform at H, both all the way across
    floor_num = 2;
    floor_cache[0] = (FloorLine){-1000.f, 0, 1000.f, 0, 9000, 0};
    floor_cache[1] = (FloorLine){-1000.f, H, 1000.f, H, 9001, 1};
    ceil_num = 0;
    wall_num[0] = wall_num[1] = 0;

    float facing = fp->facing_direction;
    u8 only = ai_only;
    ai_only = 0;
    fp->facing_direction = 1.f;
    SimStart ps;
    float slide[12];
    Sim_GroundJumpAt(fp, hop == 0, 0, 0, v, stick_x, 1, &ps, slide);
    int alone = way < 2;
    if (alone)
        ps.stick_x = way ? stick_x : 0;
    else
    {
        int hj = 1 + (way - 2) / GD_COMBOS, combo = (way - 2) % GD_COMBOS;
        int stick = combo == 0 ? JT_UP : 1 + (combo - 1) / 2, hold = combo > 0 && (combo - 1) % 2;
        SimStart st = ps;
        SimState s;
        SimStep step;
        st.stick_x = stick_x;
        Sim_Init(&st, &s);
        for (int k = 0; k < hj; k++)
        {
            Sim_Step(fp, &st, &s, -1, 0, &step);
            if (step.landed || step.ceiling)
                goto done;
        }
        st.skip_line = -1;
        st.stick_x = jt_stick_xy[stick][0];
        st.stick_y = jt_stick_xy[stick][1];
        Sim_DoubleJump(fp, &st, &s, &step);
        if (step.landed || step.ceiling)
            goto done;
        st.stick_x = hold ? jt_stick_xy[stick][0] : 0;
        st.stick_y = 0;
        Sim_ToStart(&s, &st, &ps);
    }

    // a double jump only for its rising AIs and NIL; the hop alone for the
    // wavelands too, which need the falling frames
    sim_limit = JT_SIM;
    sim_bottom_y = -30.f;
    sim_rising_only = !alone;
    if (sim_rising_only)
        sim_stop_vy = Fighter_GetSoftLandVelocity(fp) - 0.001f;
    Predict(fp, &ps, pred_route, alone ? BR_ALL : BR_AI);
    sim_rising_only = 0;
    sim_stop_vy = -100000.f;
    sim_limit = LL_SIM_FRAMES;
    sim_bottom_y = -100000.f;

    Prediction *p = pred_route;
    float half = H * 0.5f;
    if (p->land_frame && p->land_kind == LAND_NIL && p->pos[p->land_frame].Y > half)
        Guide_Add(t, hop, sp, GD_NIL, p->pos[p->land_frame].X, 1);
    if (alone && p->wl_first && p->wl_first <= p->num && p->pos[p->wl_first].Y > half)
        Guide_Add(t, hop, sp, GD_WL, p->pos[p->wl_first].X, p->wl_width);

    int normal_lag = (int)fp->attr.normal_landing_lag;
    int hold_done = p->land_frame ? p->land_frame + p->lag : 2 * LL_SIM_FRAMES;
    int last = p->land_frame ? p->land_frame - 1 : p->num;
    for (int a = 0; a < 5; a++)
    {
        u8 bit = AERIAL_BIT(Aerial_Order(a));
        for (int k = 1; k <= last && k < p->uncertain_from; k++)
        {
            if (!(p->ai_mask[k] & ~p->ai_lag_mask[k] & bit))
                continue;
            int touch = k + Ai_Delay(p, k, Aerial_Order(a));
            int w = 1;
            while (k + w <= last && (p->ai_mask[k + w] & ~p->ai_lag_mask[k + w] & bit) &&
                   k + w + Ai_Delay(p, k + w, Aerial_Order(a)) == touch)
                w++;
            int at = touch <= p->num ? touch : p->num;
            int rising = !(touch <= p->num && p->pos[touch].Y <= p->pos[touch - 1].Y);
            if (rising && hold_done - (touch + normal_lag) >= LL_AI_MIN_GAIN && p->pos[at].Y > half)
                Guide_Add(t, hop, sp, a, p->pos[at].X, w);
            k += w - 1;
        }
    }
done:
    fp->facing_direction = facing;
    ai_only = only;
}

// The baked file for this character, if the disc has one that fits.
static void Guide_Open(FighterData *fp)
{
    char path[32];
    sprintf(path, "TM/llgd%02d.bin", fp->kind);
    int entry = DVDConvertPathToEntrynum(path);
    if (entry < 0)
    {
        OSReport("LLGUIDE no %s, working them out here\n", path);
        return;
    }
    GuideHead *h = (GuideHead *)gd_mem; // slot 0 for now, 32-byte aligned
    DCFlushRange(h, 32);
    File_ReadSync(entry, 0, h, 32, 0x21, 1);
    DCInvalidateRange(h, 32);
    gd_head = *h;
    if (gd_head.magic != GD_MAGIC || gd_head.version != GD_VERSION || gd_head.kind != (u32)fp->kind ||
        gd_head.stride != GD_STRIDE || gd_head.dims != GD_DIMS || gd_head.num == 0 || gd_head.step <= 0)
    {
        OSReport("LLGUIDE %s doesn't fit this build, working them out here\n", path);
        return;
    }
    gd_file = entry;
    OSReport("LLGUIDE baked %s: %d heights from %.1f\n", path, gd_head.num, gd_head.lo);
}

// The slot with the table for a platform H over the floor, or -1.
static int Guide_Find(float H)
{
    if (gd_file < 0)
    {
        int h = -1;
        for (int j = 0; j < gd_heights; j++)
            if (fabs(gd_height[j] - H) < 3.f)
                h = j;
        return h;
    }
    float f = (H - gd_head.lo) / gd_head.step + 0.5f;
    if (f < 0)
        return -1;
    int s = (int)f;
    if (s >= (int)gd_head.num)
        return -1;
    int old = 0;
    for (int j = 0; j < GD_SLOTS; j++)
    {
        if (gd_slice[j] == s)
        {
            gd_used[j] = event_vars->game_timer;
            return j;
        }
        if (gd_slice[j] < 0 || (gd_slice[old] >= 0 && gd_used[j] < gd_used[old]))
            old = j;
    }
    u8 *t = (u8 *)Guide_Slot(old);
    DCFlushRange(t, GD_STRIDE);
    File_ReadSync(gd_file, sizeof(GuideHead) + s * GD_STRIDE, t, GD_STRIDE, 0x21, 1);
    DCInvalidateRange(t, GD_STRIDE);
    gd_slice[old] = s;
    gd_used[old] = event_vars->game_timer;
    OSReport("LLGUIDE read %.1f into %d\n", gd_head.lo + s * gd_head.step, old);
    return old;
}

// One baked table into the log, the rows that aren't all 0.
static void Guide_Dump(int slice)
{
    static const char hex[] = "0123456789abcdef";
    const u8 *t = (const u8 *)Guide_Slot(0);
    char line[48 * 2 + 1];
    for (int off = 0; off < (int)sizeof(GuideTable); off += 48)
    {
        int n = sizeof(GuideTable) - off < 48 ? sizeof(GuideTable) - off : 48, any = 0;
        for (int i = 0; i < n; i++)
        {
            any |= t[off + i];
            line[2 * i] = hex[t[off + i] >> 4];
            line[2 * i + 1] = hex[t[off + i] & 15];
        }
        line[2 * n] = 0;
        if (any)
            OSReport("LLGB %d %d %s\n", slice, off, line);
    }
}

static void Guide_BakeStart(FighterData *fp)
{
    gd_bake = 0;
    gd_next = 0;
    memset(gd_mem, 0, GD_STRIDE);
    OSReport("LLGBH %d %d %.2f %.2f %d %d\n", fp->kind, GD_NUM, GD_LO, GD_STEP, (int)GD_STRIDE, GD_DIMS);
}

// Bakes one table after another as fast as the frames allow.
static void Guide_Bake(FighterData *fp)
{
    int t0 = OSGetTick();
    while (OSTicksToMicroseconds(OSGetTick() - t0) < GD_BAKE_US)
    {
        Guide_Try(fp, Guide_Slot(0), GD_LO + gd_bake * GD_STEP, gd_next++);
        if (gd_next < GD_PER_HEIGHT)
            continue;
        Guide_Dump(gd_bake);
        memset(gd_mem, 0, GD_STRIDE);
        gd_next = 0;
        if (++gd_bake >= GD_NUM)
        {
            OSReport("LLGB end %d at %d\n", GD_NUM, event_vars->game_timer);
            gd_bake = -1;
            // what slot 0 held is gone: start over
            for (int j = 0; j < GD_SLOTS; j++)
                gd_slice[j] = -1;
            gd_heights = -1;
            gd_done = 0;
            break;
        }
    }
    Floor_BuildCache(); // the stage's own again
}

static void Guide_Solve(FighterData *fp)
{
    if (!gd_mem)
        return;
    if (gd_bake == -2)
        Guide_BakeStart(fp);
    if (gd_bake >= 0)
    {
        Guide_Bake(fp);
        return;
    }
    if (gd_done || !(Options_Jump[JOPT_BANDS].val || Options_Jump[JOPT_MARKS].val))
        return;
    if (gd_heights < 0)
    {
        Guide_Open(fp);
        if (gd_file >= 0)
        {
            gd_heights = 0;
            gd_done = 1;
            return;
        }
        Floor_BuildCache();
        Guide_Heights();
    }
    Floor_BuildCache();
    int total = gd_heights * GD_PER_HEIGHT, start = sim_steps, t0 = OSGetTick();
    while (gd_next < total && sim_steps - start < GD_BUDGET && OSTicksToMicroseconds(OSGetTick() - t0) < GD_TIME_US)
    {
        Guide_Try(fp, Guide_Slot(gd_next / GD_PER_HEIGHT), gd_height[gd_next / GD_PER_HEIGHT], gd_next % GD_PER_HEIGHT);
        gd_next++;
    }
    Floor_BuildCache(); // the stage's own again
    if (gd_next >= total)
    {
        gd_done = 1;
        OSReport("LLGUIDE done %d at %d\n", total, event_vars->game_timer);
    }
}

// The count for a distance, facing either way (the table faces right). The
// AI counts the aerials AI Aerial lets through.
static int Guide_N(int h, int hop, int sp, int kind, int face, int dx)
{
    if (face < 0)
    {
        sp = GD_SPEEDS - 1 - sp;
        dx = -dx;
    }
    dx += GD_DX;
    if (dx < 0 || dx >= GD_BINS)
        return 0;
    GuideTable *t = Guide_Slot(h);
    if (kind == CUE_NIL)
        return t->n[hop][sp][GD_NIL][dx];
    if (kind == CUE_WL)
        return t->n[hop][sp][GD_WL][dx];
    int n = 0;
    for (int a = 0; a < 5; a++)
        if (!ai_only || (ai_only & AERIAL_BIT(Aerial_Order(a))))
            n += t->n[hop][sp][a][dx];
    return n;
}

// Falcon's speed bucket, as the table counts it facing his way.
static int Guide_Bucket(FighterData *fp)
{
    float v = fp->phys.self_vel_ground.X * (fp->facing_direction < 0 ? -1.f : 1.f), stick;
    int best = 2;
    float bd = 100.f;
    for (int sp = 0; sp < GD_SPEEDS; sp++)
    {
        float d = fabs(Guide_Speed(fp, sp, &stick) - v);
        if (d < bd)
        {
            bd = d;
            best = sp;
        }
    }
    return best;
}

// A strip of cells along y from x0, one a unit wide, each cell's color's
// alpha scaled by a[i] (0 skips it).
static void Guide_Strip(float x0, int n, const float *a, float y0, float y1, GXColor c)
{
    int cells = 0;
    for (int i = 0; i < n; i++)
        cells += a[i] > 0.01f;
    if (!cells)
        return;
    World_Start(cells * 4, GX_QUADS, 0);
    for (int i = 0; i < n; i++)
    {
        if (a[i] <= 0.01f)
            continue;
        GXColor k = Color_Fill(c, a[i]);
        float xa = x0 + i, xb = xa + 1.f;
        World_Vtx(xa, y0, GLOW_Z, k);
        World_Vtx(xb, y0, GLOW_Z, k);
        World_Vtx(xb, y1, GLOW_Z, k);
        World_Vtx(xa, y1, GLOW_Z, k);
    }
}

static int Guide_Floor(float x)
{
    int i = (int)x;
    return x < i ? i - 1 : i;
}

#define GD_CELLS 200
static void Guide_Draw(FighterData *fp)
{
    int bands = Options_Jump[JOPT_BANDS].val, marks = Options_Jump[JOPT_MARKS].val;
    if (!gd_mem || !gd_done || gd_bake >= 0 || (!bands && !marks) ||
        fp->phys.air_state != 0 || fp->state_id < ASID_WAIT || fp->state_id > ASID_RUNBRAKE || hang_ledge >= 0 || route_active)
        return;
    // the floor under him
    FloorLine *S = 0;
    for (int i = 0; i < floor_num; i++)
        if (floor_cache[i].id == fp->coll_data.ground_index)
            S = &floor_cache[i];
    if (!S)
        return;
    float fx = fp->phys.pos.X, fy = fp->phys.pos.Y;
    int face = fp->facing_direction < 0 ? -1 : 1, sp = Guide_Bucket(fp);
    static const int kinds[CUE_NUM] = {CUE_AI, CUE_NIL, CUE_WL};
    static float band[2][GD_CELLS];
    int s0 = Guide_Floor(S->x0), sn = (int)(S->x1 - S->x0);
    if (sn > GD_CELLS)
        sn = GD_CELLS;
    float k_was = vis_k;
    int row = 0;
    for (int ki = 0; ki < CUE_NUM; ki++)
    {
        int kind = kinds[ki];
        if ((kind == CUE_AI && !Cues_Ai()) || (kind == CUE_NIL && !Cues_Nil()) || (kind == CUE_WL && !Cues_Waveland()))
            continue;
        memset(band, 0, sizeof(band));
        float most = 0;
        for (int pi = 0; pi < floor_num; pi++)
        {
            FloorLine *P = &floor_cache[pi];
            if (!P->is_platform || P == S)
                continue;
            float H = (P->y0 + P->y1) * 0.5f - fy;
            int h = Guide_Find(H);
            if (h < 0)
                continue;
            int p0 = Guide_Floor(P->x0) + 1, p1 = Guide_Floor(P->x1);
            // A: a spot on the floor, the timings landing anywhere on the
            // platform (a running sum over the distances, so each spot is
            // one subtraction)
            for (int hop = 0; hop < 2 && bands; hop++)
            {
                static int cum[GD_BINS + 1];
                cum[0] = 0;
                for (int b = 0; b < GD_BINS; b++)
                    cum[b + 1] = cum[b] + Guide_N(h, hop, sp, kind, face, b - GD_DX);
                for (int t = 0; t < sn; t++)
                {
                    int T = s0 + t;
                    int lo = p0 - T + GD_DX, hi = p1 - T + GD_DX; // bins lo..hi
                    if (lo < 0)
                        lo = 0;
                    if (hi > GD_BINS - 1)
                        hi = GD_BINS - 1;
                    if (hi < lo)
                        continue;
                    band[hop][t] += cum[hi + 1] - cum[lo];
                    if (band[hop][t] > most)
                        most = band[hop][t];
                }
            }
            // C: a spot on the platform, the timings landing there from here
            if (marks)
            {
                float m[2][GD_CELLS], mm = 0;
                int n = p1 - p0 + 1;
                if (n > GD_CELLS)
                    n = GD_CELLS;
                for (int hop = 0; hop < 2; hop++)
                    for (int x = 0; x < n; x++)
                    {
                        m[hop][x] = Guide_N(h, hop, sp, kind, face, Guide_Floor(p0 + x - fx + 0.5f));
                        if (m[hop][x] > mm)
                            mm = m[hop][x];
                    }
                if (mm > 0)
                {
                    vis_k = Kind_K(VG_CUES, kind);
                    GXColor c = Cue_Color(kind);
                    float norm = mm < 6.f ? 6.f : mm;
                    for (int hop = 0; hop < 2; hop++)
                    {
                        for (int x = 0; x < n; x++)
                            m[hop][x] = m[hop][x] > 0 ? 0.3f + 0.7f * sqrtf(m[hop][x] / norm) : 0;
                        // short hop nearer the surface, full hop above it,
                        // each kind its own pair
                        float y = (P->y0 + P->y1) * 0.5f + 0.15f + row * 1.6f + hop * 0.75f;
                        Guide_Strip((float)p0, n, m[hop], y, y + 0.6f, c);
                    }
                }
            }
        }
        if (bands && most > 0)
        {
            vis_k = Kind_K(VG_CUES, kind);
            GXColor c = Cue_Color(kind);
            float norm = most < 6.f ? 6.f : most;
            for (int hop = 0; hop < 2; hop++)
            {
                for (int t = 0; t < sn; t++)
                    band[hop][t] = band[hop][t] > 0 ? 0.25f + 0.75f * sqrtf(band[hop][t] / norm) : 0;
                float y = fy - 0.4f - row * 1.8f - hop * 0.85f;
                Guide_Strip((float)s0, sn, band[hop], y - 0.7f, y, c);
            }
        }
        row++;
    }
    vis_k = k_was;
}

#endif
