/* sim.c: Landing Lab's simulation (src/landinglab_sim.h) built for a PC,
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
    fighter.kind = fighter_kind;
    fighter.facing_direction = 1;
    fighter.input.timer_trigger_any_ignore_hitlag = LL_TIMER_MAX;
    fighter.input.timer_lstick_tilt_y = LL_TIMER_MAX;
    wl_ground = wlground;
    Sim_Setup();
    Floor_BuildCache();
    return 1;
}

void llsim_check(void)
{
    static Prediction p;
    Sim_Check(&fighter, &p);
}

// A character's own ECB table (a capture's LLECB and LLLEN lines) in place
// of Falcon's baked one. The fall leans were measured on Falcon only, so
// another character falls in its plain pose.
void llsim_table_begin(int keep_lean)
{
    memset(ecb_table, 0, sizeof(EcbSample) * TS_COUNT * LL_STATE_FRAMES);
    memset(state_len, 0, sizeof(state_len));
    memset(state_next, -1, sizeof(state_next));
    if (!keep_lean)
    {
        memset(lean_levels, 0, sizeof(lean_levels));
        memset(onset_levels, 0, sizeof(onset_levels));
    }
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

int llsim_len(int ts, int len, int next)
{
    if (ts < 0 || ts >= TS_COUNT)
        return 0;
    state_len[ts] = len;
    state_next[ts] = next;
    return 1;
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
    OSReport("LLECB end\n");
}
