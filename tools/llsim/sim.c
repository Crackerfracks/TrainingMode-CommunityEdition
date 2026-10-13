/* sim.c: Landing Gear's simulation (src/landinglab_sim.h) built for a PC,
 * with the game's data loaded where the game keeps it. Only MexTK's view of
 * the C library is visible here (its memcpy and calloc differ from the
 * PC's); main.c does the file work and calls in through llsim.h.
 */

#define calloc ll_calloc // MexTK's takes one argument
#define memcpy ll_memcpy // ... and its sizes are ints
#define memset ll_memset
#include "../../MexTK/mex.h"
#include "llsim.h"

static float dump_softland;
float Fighter_GetSoftLandVelocity(FighterData *fp)
{
    return dump_softland;
}

static int wl_ground = 1;
static int Cues_WavelandGround(void);

#include "../../src/landinglab_sim.h"

static int Cues_WavelandGround(void)
{
    return wl_ground;
}

#define MAX_GROUPS 64
#define MAX_LINES 1024
#define MAX_VERTS 1024

static u8 common_buf[0x1000];
static u8 special_buf[0x100];
static ftCommonData *common_ptr = (ftCommonData *)common_buf;
static CollGroupDesc group_desc[MAX_GROUPS];
static CollGroup groups[MAX_GROUPS];
static int group_num;
static CollGroup *first_group;
static CollLineDesc line_desc[MAX_LINES];
static RawCollLine lines[MAX_LINES];
static CollLine *lines_ptr;
static CollVert verts[MAX_VERTS];
static CollVert *verts_ptr;
static FighterData fighter;

static float Bits_Float(u32 u)
{
    union
    {
        u32 u;
        float f;
    } v = {u};
    return v.f;
}

static void Words(u8 *buf, int size, int at, const unsigned *w, int n)
{
    for (int i = 0; i < n && at + 4 * i + 4 <= size; i++)
        *(u32 *)(buf + at + 4 * i) = w[i];
}

void llsim_common(int at, const unsigned *w, int n)
{
    Words(common_buf, sizeof(common_buf), at, w, n);
}

void llsim_attr(int at, const unsigned *w, int n)
{
    Words((u8 *)&fighter.attr, sizeof(fighter.attr), at, w, n);
}

void llsim_special(int at, const unsigned *w, int n)
{
    Words(special_buf, sizeof(special_buf), at, w, n);
}

void llsim_softland(unsigned bits)
{
    dump_softland = Bits_Float(bits);
}

int llsim_group(int g, const int *v)
{
    if (g < 0 || g >= MAX_GROUPS)
        return 0;
    CollGroupDesc *d = &group_desc[g];
    d->floor_start = v[0], d->floor_num = v[1];
    d->ceil_start = v[2], d->ceil_num = v[3];
    d->rwall_start = v[4], d->rwall_num = v[5];
    d->lwall_start = v[6], d->lwall_num = v[7];
    d->dyn_start = v[8], d->dyn_num = v[9];
    d->vert_start = v[10], d->vert_num = v[11];
    groups[g].desc = d;
    groups[g].is_enabled = v[12];
    if (g + 1 > group_num)
        group_num = g + 1;
    return 1;
}

int llsim_line(int i, const int *v, unsigned xc, unsigned xd, unsigned xe, unsigned flags)
{
    if (i < 0 || i >= MAX_LINES)
        return 0;
    CollLineDesc *d = &line_desc[i];
    d->vert_prev = v[0], d->vert_next = v[1];
    d->line_prev = v[2], d->line_next = v[3];
    d->line_prev_altgroup = v[4], d->line_next_altgroup = v[5];
    d->xc = xc;
    // the flag bytes in the game's bit order (MexTK lists each byte's bits
    // from 0x80 down)
    d->xd_1 = !!(xd & 0x80), d->xd_2 = !!(xd & 0x40), d->xd_3 = !!(xd & 0x20), d->disabled = !!(xd & 0x10);
    d->is_left = !!(xd & 0x08), d->is_right = !!(xd & 0x04), d->is_ceil = !!(xd & 0x02), d->is_floor = !!(xd & 0x01);
    d->xe_1 = !!(xe & 0x80), d->xe_2 = !!(xe & 0x40), d->xe_3 = !!(xe & 0x20), d->xe_4 = !!(xe & 0x10);
    d->xe_5 = !!(xe & 0x08), d->is_drop = !!(xe & 0x04), d->is_ledge = !!(xe & 0x02), d->is_unk = !!(xe & 0x01);
    lines[i].desc = d;
    lines[i].flags = flags;
    return 1;
}

int llsim_vert(int i, unsigned cx, unsigned cy, unsigned ox, unsigned oy)
{
    if (i < 0 || i >= MAX_VERTS)
        return 0;
    verts[i].pos_curr = (Vec2){Bits_Float(cx), Bits_Float(cy)};
    verts[i].pos_orig = (Vec2){Bits_Float(ox), Bits_Float(oy)};
    verts[i].pos_prev = verts[i].pos_curr;
    return 1;
}

int llsim_ready(int fighter_kind, int wlground)
{
    _Static_assert(sizeof(fighter.attr) == 0x184, "MexTK's attr block isn't the game's size here");
    if (group_num == 0)
        return 0;
    // the groups in order, as the game links them
    for (int g = 0; g < group_num; g++)
        groups[g].next = g + 1 < group_num ? &groups[g + 1] : 0;
    first_group = &groups[0];
    lines_ptr = (CollLine *)lines;
    verts_ptr = verts;
    stc_firstcollgroup = &first_group;
    stc_collline = &lines_ptr;
    stc_collvert = &verts_ptr;
    stc_ftcommon = &common_ptr;

    fighter.special_attributes = special_buf;
    // Kirby's and Jigglypuff's air jumps read their special attributes
    // (ftKb and ftPr OnLoad: x2D0 = dat_attrs)
    if (fighter_kind == FTKIND_KIRBY || fighter_kind == FTKIND_JIGGLYPUFF)
        fighter.multi_jump_desc = (FtMultiJumpDesc *)special_buf;
    fighter.kind = fighter_kind;
    fighter.facing_direction = 1;
    fighter.input.timer_trigger_any_ignore_hitlag = LL_TIMER_MAX;
    fighter.input.timer_lstick_tilt_y = LL_TIMER_MAX;
    wl_ground = wlground;
    Sim_Setup();
    Sim_ForFighter(fighter_kind); // as the event does on its first frame
    Floor_BuildCache();
    return 1;
}

void llsim_check(void)
{
    static Prediction p;
    Sim_Check(&fighter, &p);
}

// A character's own ECB table (a capture's LLECB and LLLEN lines) in place
// of the one in use. The fall leans stay as they are (Falcon's for him,
// none for another fighter, as in a capture) unless keep_lean is 0.
void llsim_table_begin(int keep_lean)
{
    memset(ecb_table, 0, sizeof(EcbSample) * TS_COUNT * LL_STATE_FRAMES);
    memset(state_len, 0, sizeof(state_len));
    memset(state_next, -1, sizeof(state_next));
    fall_step = 1;
    fall_blend = 0;
    if (!keep_lean)
        Lean_Drop();
}

// Falls keep their plain pose (-nolean).
void llsim_nolean(void)
{
    Lean_Drop();
}

int llsim_ecb(int ts, int frame, int flags, const unsigned *w)
{
    if (ts < 0 || ts >= TS_COUNT || frame < 0 || frame >= LL_STATE_FRAMES)
        return 0;
    EcbSample *e = &ecb_table[ts * LL_STATE_FRAMES + frame];
    e->bottom = Bits_Float(w[0]);
    e->top = Bits_Float(w[1]);
    e->side_y = Bits_Float(w[2]);
    e->front = Bits_Float(w[3]);
    e->back = Bits_Float(w[4]);
    e->has_bottom = (flags & BAKED_BOTTOM) != 0;
    e->has_shape = (flags & BAKED_SHAPE) != 0;
    e->aerial_lag = (flags & BAKED_LAG) != 0;
    e->seen = 1;
    return 1;
}

void llsim_loops(const int *loops)
{
    for (int i = 0; i < 3; i++)
        fall_loop[i] = loops[i] > 0 && loops[i] <= LL_STATE_FRAMES ? loops[i] : LL_FALL_LOOP;
}

// The falls' lean step (fall_step), as a capture's LLFALLANIM lines show it.
void llsim_step(int step)
{
    fall_step = step > 1 ? step : 1;
}

int llsim_fall_step(void)
{
    return fall_step;
}

// The falls' blend-in (fall_blend), as a capture's LLFALLANIM lines show it.
void llsim_blend(int blend)
{
    fall_blend = blend > 0 && blend < 64 ? blend : 0;
}

int llsim_fall_blend(void)
{
    return fall_blend;
}

int llsim_len(int ts, int len, int next)
{
    if (ts < 0 || ts >= TS_COUNT)
        return 0;
    state_len[ts] = len;
    state_next[ts] = next;
    return 1;
}

// Each aerial landing on a frame with its lag (Landing_Kind), trigger left
// alone and then with L just pressed: one LLLAG line each, the aerial, the
// landing it gives and its lag, then the same with L (Game & Watch's own
// nair, bair and uair don't L-cancel, Aerial_LCancels).
void llsim_lag(void)
{
    SimState s;
    EcbSample e;
    memset(&s, 0, sizeof(s));
    memset(&e, 0, sizeof(e));
    e.aerial_lag = 1;
    e.seen = 1;
    for (int ts = TS_AIRN; ts <= TS_AIRLW; ts++)
    {
        int lag, lc_lag, kind, l_lag, l_lc_lag, l_kind;
        s.ts = ts;
        s.trigger_timer = LL_TIMER_MAX;
        kind = Landing_Kind(&fighter, &s, &e, 0, &lag, &lc_lag);
        s.trigger_timer = 0;
        l_kind = Landing_Kind(&fighter, &s, &e, 0, &l_lag, &l_lc_lag);
        OSReport("LLLAG %s %s %d, L pressed %s %d\n", tracked_state_names[ts], land_kind_names[kind], lag,
                 land_kind_names[l_kind], l_lag);
    }
}

// Jumps in the air from standing still at (0, 100) in a fall with one jump
// used: the next jump pressed on frame 2 and every gap frames after while
// any are left (none with gap 0), the stick held at (sx, sy) throughout.
// One LLJUMPS line a frame: frame, state, jumps used, facing, position and
// speed, to set against a trace of the same inputs (script trace on).
void llsim_jumps(float sx, float sy, int gap)
{
    SimStart st;
    SimState s;
    SimStep step;
    EcbSample *e = Ecb_Get(TS_FALL, 0);
    memset(&st, 0, sizeof(st));
    st.pos = (Vec2){0, 100};
    st.facing = 1;
    st.bottom = e->has_bottom ? e->bottom : 2.f;
    st.locked_bottom = st.bottom;
    st.ecb = (SimEcb){st.bottom, st.bottom + 16.f, st.bottom + 8.f, -3.f, 3.f};
    st.ts = TS_FALL;
    st.frame = -1;
    st.tilt_timer = LL_TIMER_MAX;
    st.trigger_timer = LL_TIMER_MAX;
    st.skip_line = -1;
    st.jumps = 1;
    st.stick_x = sx;
    st.stick_y = sy;
    Sim_Init(&st, &s);
    for (int k = 1; k <= 200; k++)
    {
        if (gap > 0 && k >= 2 && (k - 2) % gap == 0 && s.jumps < fighter.attr.max_jumps)
            Sim_DoubleJump(&fighter, &st, &s, &step);
        else
            Sim_Step(&fighter, &st, &s, -1, 0, &step);
        OSReport("LLJUMPS %d %s %d %d %.4f %.4f %.5f %.5f%s\n", k, tracked_state_names[s.ts], s.jumps, s.face > 0 ? 1 : -1,
                 s.x, s.y, s.vx, s.vy, step.unlearned ? " unlearned" : "");
    }
}

static u32 Float_Bits(float f)
{
    union
    {
        float f;
        u32 u;
    } v = {f};
    return v.u;
}

// A fighter's packed table (TM/llecb.bin, whole) in place of the one in
// use, as the event loads it; the rows it took, -1 if the file has none.
int llsim_packed(const unsigned char *file, int size, int fighter_kind)
{
    int offset, len;
    if (size < LLE_DIR_BYTES || !Ecb_Find(file, fighter_kind, &offset, &len) || offset + len > size)
        return -1;
    own_table = file + offset;
    own_size = len;
    return Ecb_Unpack(own_table, own_size);
}

// The table in use, as the game's Ecb_Dump writes it (its LLECB and LLLEN
// lines), so a capture can be diffed against it.
void llsim_ecb_print(int fighter_kind)
{
    OSReport("LLECB begin char %d\n", fighter_kind);
    for (int ts = 0; ts < TS_COUNT; ts++)
    {
        for (int f = 0; f < LL_STATE_FRAMES; f++)
        {
            EcbSample *e = &ecb_table[ts * LL_STATE_FRAMES + f];
            if (!e->seen)
                continue;
            int flags = (e->has_bottom ? BAKED_BOTTOM : 0) | (e->has_shape ? BAKED_SHAPE : 0) | (e->aerial_lag ? BAKED_LAG : 0);
            OSReport("LLECB %d %d %d %08x %08x %08x %08x %08x\n", ts, f, flags, Float_Bits(e->bottom), Float_Bits(e->top),
                     Float_Bits(e->side_y), Float_Bits(e->front), Float_Bits(e->back));
        }
        OSReport("LLLEN %d %d %d\n", ts, state_len[ts], state_next[ts]);
    }
    OSReport("LLLOOP %d %d %d\n", fall_loop[0], fall_loop[1], fall_loop[2]);
    if (lean_rows == lean_own) // another fighter's leans, as packed
        for (int i = 0; i < lean_count; i++)
        {
            const LeanEcb *l = &lean_rows[i];
            OSReport("LLLEANGRID %d %d %d %08x %08x %08x %08x %08x %08x\n", l->fall, l->back, l->k, Float_Bits(l->lean),
                     Float_Bits(l->bottom), Float_Bits(l->top), Float_Bits(l->side), Float_Bits(l->front), Float_Bits(l->back_x));
        }
    for (int d = 0; d < 2; d++) // a double-jump-cancel fighter's double jump, as packed
        for (int f = 0; f < LL_STATE_FRAMES; f++)
        {
            DjMove *m = Dj_Get(TS_JUMPAERIALF + d, f);
            if (m->seen)
                OSReport("LLDJMOVE %d %d %08x %08x\n", TS_JUMPAERIALF + d, f, Float_Bits(m->x), Float_Bits(m->y));
        }
    OSReport("LLECB end\n");
}

// The ledge route search (src/landinglab/ledge_routes.c, Ledge_Try) from a
// hang at (hx, hy) off the corner (cx, cy), facing the stage: every drop,
// wait, fastfall, double-jump stick, hold and fall-away it tries, with the
// touchdown each gives. One LLLEDGE line per candidate that touches down on
// the stage (NIL or aerial interrupt, or "lag" for a landing with lag),
// act frames from the drop and the GALINT left of cliff_invuln_time.
static int Ll_Stick(int drop, int wait, int ff, int hold, int dj_x, int fall_away, int facing, int f, float *sx, float *sy)
{
    static const float dj_stick[3] = {1.f, 0.7f, 0.f};
    int dj = 1 + wait + ff;
    *sx = 0;
    *sy = 0;
    if (f == 0)
    {
        if (drop == 0)
            *sx = -facing;
        else
            *sy = -1.f;
    }
    else if (f < dj && f > wait)
    {
        *sy = fall_away ? -0.7f : -1.f;
        *sx = fall_away ? -0.7f * facing : 0;
    }
    else if (f < dj)
    {
        if (fall_away)
            *sx = -facing;
    }
    else if (f == dj)
    {
        *sx = facing * dj_stick[dj_x];
        *sy = dj_x == 1 ? -0.7f : 0;
    }
    else
        *sx = hold ? facing : 0;
    return dj;
}

// One candidate frame by frame (llsim ledge with LLTRACE=<candidate>): the
// state, its frame, where the fighter is, and what the step didn't know.
static void Ll_Trace(SimStart *st, SimState *s0, int drop, int wait, int ff, int hold, int dj_x, int fall_away, int facing)
{
    SimState s = *s0;
    SimStep step;
    for (int k = 1; k <= 45; k++)
    {
        Ll_Stick(drop, wait, ff, hold, dj_x, fall_away, facing, 1 + wait + ff + k, &st->stick_x, &st->stick_y);
        Sim_Step(&fighter, st, &s, -1, 0, &step);
        EcbSample *e = Ecb_Get(s.ts, s.frame);
        int djs = Is_DoubleJump(s.ts) ? Dj_Get(s.ts, s.frame)->seen : -1;
        OSReport("LLTRACE %d %s f%d x %.3f y %.3f vy %.4f bottom %.3f lock %d ecb seen %d bottom %d shape %d dj %d%s%s\n", k,
                 tracked_state_names[s.ts], s.frame, s.x, s.y, s.vy, s.bottom, s.lock, e->seen, e->has_bottom, e->has_shape,
                 djs, step.unlearned ? " UNLEARNED" : "", step.landed ? " LANDED" : "");
        if (step.landed)
            break;
    }
}

void llsim_ledge(float cx, float cy, int facing, float hx, float hy, int verbose, int trace)
{
    static Prediction pred;
    int invuln = (*stc_ftcommon)->cliff_invuln_time ? (*stc_ftcommon)->cliff_invuln_time : 30; // dumps leave it out
    int best[3] = {999, 999, 999}, count[3] = {0, 0, 0}, tried = 0, unsure = 0, none = 0;
    static int unsure_ts[TS_COUNT];
    memset(unsure_ts, 0, sizeof(unsure_ts));
    float tilt_dz = Common_Float(0xC);
    for (int c = 0; c < 2 * 2 * 2 * 9 * 9 * 3; c++)
    {
        int drop = c % 2, hold = (c / 2) % 2, fall_away = (c / 4) % 2, r = c / 8;
        int wait = r % 9, ff = (r / 9) % 9, dj_x = r / 81;
        SimStart st;
        SimState s;
        SimStep step;
        EcbSample *e = Ecb_Get(TS_FALL, 0);
        memset(&st, 0, sizeof(st));
        st.pos = (Vec2){cx + hx * facing, cy + hy};
        st.facing = facing;
        st.bottom = e->has_bottom ? e->bottom : 2.f;
        st.locked_bottom = st.bottom;
        st.ecb = (SimEcb){st.bottom, st.bottom + 16.f, st.bottom + 8.f, -3.f, 3.f};
        if (e->has_shape)
            Ecb_FromSample(e, facing, &st.ecb);
        Ecb_Fix(&st.ecb);
        st.ts = TS_FALL;
        st.frame = -1;
        st.tilt_timer = LL_TIMER_MAX;
        st.trigger_timer = LL_TIMER_MAX;
        st.skip_line = -1;
        st.jumps = 1;
        Sim_Init(&st, &s);
        int dj = 1 + wait + ff, prev_down = 0, ff_at = -1, out = 0;
        for (int f = 0; f < dj && !out; f++)
        {
            Ll_Stick(drop, wait, ff, hold, dj_x, fall_away, facing, f, &st.stick_x, &st.stick_y);
            int down = st.stick_y <= -tilt_dz;
            s.tilt_timer = down ? (prev_down ? s.tilt_timer : -1) : LL_TIMER_MAX;
            prev_down = down;
            Sim_Step(&fighter, &st, &s, -1, 0, &step);
            if (step.fastfall_started)
                ff_at = f;
            out = step.landed;
        }
        if (out || (ff > 0 && ff_at < 0))
            continue;
        Ll_Stick(drop, wait, ff, hold, dj_x, fall_away, facing, dj, &st.stick_x, &st.stick_y);
        Sim_DoubleJump(&fighter, &st, &s, &step);
        if (step.landed || step.ceiling)
            continue;
        if (c == trace)
        {
            OSReport("LLTRACE candidate %d: drop %d wait %d ff %d dj_x %d hold %d away %d, after the jump:\n", c, drop, wait, ff,
                     dj_x, hold, fall_away);
            Ll_Trace(&st, &s, drop, wait, ff, hold, dj_x, fall_away, facing);
        }
        Ll_Stick(drop, wait, ff, hold, dj_x, fall_away, facing, dj + 1, &st.stick_x, &st.stick_y);
        SimStart ps;
        Sim_ToStart(&s, &st, &ps);
        sim_limit = 45;
        sim_bottom_y = st.pos.Y - 40.f;
        Predict(&fighter, &ps, &pred, BR_AI);
        sim_limit = LL_SIM_FRAMES;
        sim_bottom_y = -100000.f;
        Prediction *p = &pred;
        tried++;
        if (p->uncertain_from <= p->num)
        {
            unsure++;
            unsure_ts[p->ts[p->uncertain_from]]++;
        }
        if (!p->land_frame)
            none++;
        if (p->land_frame)
        {
            int act = dj + p->land_frame + (p->land_kind == LAND_NIL ? 0 : p->lag);
            int k = p->land_kind == LAND_NIL ? 0 : 2;
            count[k]++;
            if (act < best[k])
                best[k] = act;
            if (verbose)
                OSReport("LLLEDGE %s drop %d wait %d ff %d dj_x %d hold %d away %d land %d act %d galint %d%s\n",
                         land_kind_names[p->land_kind], drop, wait, ff, dj_x, hold, fall_away, dj + p->land_frame, act,
                         invuln - act, p->uncertain_from <= p->land_frame ? " uncertain" : "");
        }
        int last = p->land_frame ? p->land_frame - 1 : p->num;
        for (int k = 1; k <= last && k < p->uncertain_from; k++)
        {
            for (int a = TS_AIRN; a <= TS_AIRLW; a++)
            {
                u8 bit = AERIAL_BIT(a);
                if (!(p->ai_mask[k] & ~p->ai_lag_mask[k] & bit))
                    continue;
                int act = dj + k + Ai_Delay(p, k, a) + (int)fighter.attr.normal_landing_lag;
                count[1]++;
                if (act < best[1])
                {
                    best[1] = act;
                    if (verbose)
                        OSReport("LLLEDGE AI %s drop %d wait %d ff %d dj_x %d hold %d away %d press %d act %d galint %d\n",
                                 tracked_state_names[a], drop, wait, ff, dj_x, hold, fall_away, dj + k, act, invuln - act);
                }
            }
        }
    }
    OSReport("LLLEDGE invuln %d: NIL %d found, best act %d (GALINT %d); AI %d, best act %d (GALINT %d); other landings %d, best act %d\n",
             invuln, count[0], best[0], invuln - best[0], count[1], best[1], invuln - best[1], count[2], best[2]);
    OSReport("LLLEDGE %d jumps simulated, %d never touched down, %d relied on unlearned data:", tried, none, unsure);
    for (int ts = 0; ts < TS_COUNT; ts++)
        if (unsure_ts[ts])
            OSReport(" %s %d", tracked_state_names[ts], unsure_ts[ts]);
    OSReport("\n");
}
