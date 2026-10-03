/* landinglab.c
 *
 * Landing Lab (Captain Falcon).
 *
 * While Falcon is airborne, this draws where he will touch down if the stick
 * stays where it is and no buttons are pressed, colored by what kind of
 * landing it will be: NIL, normal landing, L-cancel or full aerial lag. Ticks
 * on that path mark the frames where pressing an aerial forces a touchdown
 * (an aerial interrupt). After a landing, the prediction made when you last
 * changed your input stays on screen next to the path you really took.
 *
 * An aerial interrupt: pressing an aerial swaps the ECB to the aerial's pose
 * on that same frame. If the new ECB bottom ends up at or below a floor, you
 * land right there. While the post-jump bottom lock is on, the swap waits
 * until the lock ends and uses whatever aerial frame you're on by then.
 *
 * Whether a landing is a NIL or an aerial interrupt depends on the exact ECB
 * bottom on each frame of each animation, which the game only computes on
 * the frame itself. So the event learns it while you play: every airborne
 * frame stores the ECB for (state, frame in state). Parts of a prediction that
 * rely on frames it hasn't seen yet are drawn gray.
 *
 * The physics and the floor test are ports of the game's own code, checked
 * against the decomp: ft_80084DB0 (air physics), ftCommon_CalcSelfAccel_
 * DriftFrom (drift), mpColl_LoadECB (ECB bottom lock), mpCheckFloor and
 * mpLineIntersection(H) (floor crossing), ft_80082B1C (NIL),
 * ftCo_AttackAir_EnterFromMsid (aerial start) and ftCo_LandingAir_EnterWithLag
 * (aerial lag, L-cancel).
 */

#include "../MexTK/mex.h"
#include "events.h"

#define LL_SIM_FRAMES 240   // how far ahead to simulate
#define LL_STATE_FRAMES 128 // frames learned per state
#define LL_TIMER_MAX 0xFE   // input timers stop counting here

// ftCommonData values. MexTK declares this struct with the wrong types for
// some of them, so they're read by offset.
#define COMMON_FASTFALL_STICK 0x88  // float: stick y must be <= -this to fastfall
#define COMMON_FASTFALL_WINDOW 0x8C // int: frames a down tilt still counts as a flick
#define COMMON_LCANCEL_WINDOW 0xE4  // int: L/R/Z within this many frames halves aerial lag
#define COMMON_LCANCEL_DIV 0xE8     // float: aerial lag divisor for an L-cancel
#define COMMON_PLATFORM_DROP 0x25C  // float: jump/fall pass through platforms at stick y <= this
#define COMMON_AERIAL_STICK_X 0xDC  // float: |stick x| below this (and y below the next) is a nair
#define COMMON_AERIAL_STICK_Y 0xE0  // float
#define COMMON_AERIAL_ANGLE 0x20    // float, radians: steeper than this is an uair or dair

// Runtime collision line flags (decomp mp/forward.h)
#define LINEFLAG_FLOOR (1u << 0)
#define LINEFLAG_EMPTY (1u << 7)
#define LINEFLAG_ENABLED (1u << 16)
#define LINEFLAG_HIDDEN (1u << 18)

// MexTK's CollLine splits the flag word into bitfields; read it as one word.
typedef struct RawCollLine
{
    CollLineDesc *desc;
    u32 flags;
} RawCollLine;

// MexTK names vs. what the CollData fields hold (checked against the decomp):
//   ecbCurr_*         ECB built from the bones this frame (decomp desired_ecb)
//   ecbCurrCorrect_*  ECB collision actually used this frame (decomp ecb)
//   ignore_line       platform being dropped through (decomp floor_skip)
//   u.ecb_bot_lock_frames  post-jump ECB bottom lock (decomp Fighter.ecb_lock)

///////////////////////
/// Tracked states  ///
///////////////////////

enum TrackedState
{
    TS_JUMPF,
    TS_JUMPB,
    TS_JUMPAERIALF,
    TS_JUMPAERIALB,
    TS_FALL,
    TS_FALLAERIAL,
    TS_AIRN,
    TS_AIRF,
    TS_AIRB,
    TS_AIRHI,
    TS_AIRLW,

    TS_COUNT
};

static const int tracked_state_ids[TS_COUNT] = {
    ASID_JUMPF,
    ASID_JUMPB,
    ASID_JUMPAERIALF,
    ASID_JUMPAERIALB,
    ASID_FALL,
    ASID_FALLAERIAL,
    ASID_ATTACKAIRN,
    ASID_ATTACKAIRF,
    ASID_ATTACKAIRB,
    ASID_ATTACKAIRHI,
    ASID_ATTACKAIRLW,
};

static const char *tracked_state_names[TS_COUNT] = {
    "JumpF", "JumpB", "DJumpF", "DJumpB", "Fall", "FallAerial",
    "Nair", "Fair", "Bair", "Uair", "Dair",
};

static int Tracked_Index(int state_id)
{
    for (int i = 0; i < TS_COUNT; i++)
    {
        if (tracked_state_ids[i] == state_id)
            return i;
    }
    return -1;
}

static int Tracked_IsAerial(int ts)
{
    return ts >= TS_AIRN;
}

// Jump, double jump and fall collision pass the drop-through callback
// (ftCo_80096CC8); aerials collide without it and always land on platforms.
static int Tracked_UsesPlatformDrop(int ts)
{
    return ts <= TS_FALLAERIAL;
}

// The state the game enters when this one's animation runs out, or -1 if it
// loops. Jump and aerials go to Fall (ftCo_Fall_Enter), double jump goes to
// FallAerial (ftCo_FallAerial_Enter). Once a state has been seen ending,
// state_next (below) holds what really followed.
static int Tracked_NaturalNext(int ts)
{
    switch (ts)
    {
    case TS_JUMPF:
    case TS_JUMPB:
    case TS_AIRN:
    case TS_AIRF:
    case TS_AIRB:
    case TS_AIRHI:
    case TS_AIRLW:
        return TS_FALL;
    case TS_JUMPAERIALF:
    case TS_JUMPAERIALB:
        return TS_FALLAERIAL;
    default:
        return -1;
    }
}

///////////////////////
/// Landing kinds   ///
///////////////////////

enum LandKind
{
    LAND_NONE,    // no landing within LL_SIM_FRAMES
    LAND_NIL,     // jump/fall touching down slowly enough: straight to Wait
    LAND_AI,      // aerial interrupt: an aerial's ECB touched down the first time it was used
    LAND_NORMAL,  // normal landing lag (jump/fall, or an aerial's auto-cancel window)
    LAND_LCANCEL, // aerial lag, halved
    LAND_AERIAL,  // full aerial lag
    LAND_OTHER,   // left the air some other way (ledge grab, ...)

    LAND_KIND_COUNT
};

static const char *land_kind_names[LAND_KIND_COUNT] = {
    "None", "NIL", "AI", "Land", "L-cancel", "Lag", "Other",
};

// colorblind-friendly set, like ledgedash's
static const GXColor land_kind_colors[LAND_KIND_COUNT] = {
    {120, 160, 255, 255}, // none: blue
    {64, 255, 96, 255},   // NIL: green
    {0, 220, 255, 255},   // aerial interrupt: cyan
    {255, 230, 0, 255},   // normal landing / auto-cancel: yellow
    {255, 140, 0, 255},   // L-cancel: orange
    {255, 48, 48, 255},   // full aerial lag: red
    {200, 200, 200, 255}, // other
};
static const GXColor color_learning = {130, 130, 130, 255};
static const GXColor color_actual = {255, 255, 255, 255};

///////////////////////
/// Learned data    ///
///////////////////////

typedef struct EcbSample
{
    float bottom;   // ECB bottom, relative to the fighter (valid if has_bottom)
    float top;      // ECB top
    float side_y;   // height of the left and right points
    float front;    // x of the front point, as if facing right
    float back;     // x of the back point, as if facing right
    u8 has_bottom;  // seen on a frame without the post-jump bottom lock
    u8 has_shape;   // top/side/front/back are valid
    u8 aerial_lag;  // landing on this frame of an aerial takes aerial lag
    u8 seen;        // times this frame has been seen (stops at 255)
} EcbSample;

static EcbSample *ecb_table;      // [TS_COUNT][LL_STATE_FRAMES]
static s16 state_len[TS_COUNT];   // frames a state lasts before ending by itself, 0 = not seen
static s8 state_next[TS_COUNT];   // state that followed when it ended, -1 = not seen

// Did prev end by itself into ts? Its animation ending is the only way from
// jump, double jump or an aerial into a fall state.
static int Tracked_EndedInto(int prev, int ts)
{
    return Tracked_NaturalNext(prev) >= 0 && (ts == TS_FALL || ts == TS_FALLAERIAL);
}

static int Tracked_Next(int ts)
{
    return state_next[ts] >= 0 ? state_next[ts] : Tracked_NaturalNext(ts);
}

static void Learned_Clear(void)
{
    memset(ecb_table, 0, sizeof(EcbSample) * TS_COUNT * LL_STATE_FRAMES);
    memset(state_len, 0, sizeof(state_len));
    memset(state_next, -1, sizeof(state_next));
}

static EcbSample *Ecb_Get(int ts, int frame)
{
    if (frame >= LL_STATE_FRAMES)
        frame = LL_STATE_FRAMES - 1;
    return &ecb_table[ts * LL_STATE_FRAMES + frame];
}

static float Aerial_LandingLag(FighterData *fp, int ts)
{
    switch (ts)
    {
    case TS_AIRN:
        return fp->attr.n_air_landing_lag;
    case TS_AIRF:
        return fp->attr.f_air_landing_lag;
    case TS_AIRB:
        return fp->attr.b_air_landing_lag;
    case TS_AIRHI:
        return fp->attr.u_air_landing_lag;
    case TS_AIRLW:
        return fp->attr.d_air_landing_lag;
    default:
        return fp->attr.normal_landing_lag;
    }
}

static void Ecb_Record(FighterData *fp, int ts, int frame)
{
    if (frame >= LL_STATE_FRAMES)
        return;

    EcbSample *s = Ecb_Get(ts, frame);
    CollData *cd = &fp->coll_data;

    // While the bottom is locked the game keeps the old bottom, so only the
    // unlocked frames say where the bones put it.
    if (cd->u.ecb_bot_lock_frames <= 0)
    {
        s->bottom = cd->ecbCurr_bot.Y;
        s->has_bottom = 1;
    }

    s->top = cd->ecbCurr_top.Y;
    s->side_y = cd->ecbCurr_right.Y;
    if (fp->facing_direction > 0)
    {
        s->front = cd->ecbCurr_right.X;
        s->back = cd->ecbCurr_left.X;
    }
    else
    {
        s->front = -cd->ecbCurr_left.X;
        s->back = -cd->ecbCurr_right.X;
    }
    s->has_shape = 1;
    s->aerial_lag = fp->ftcmd_var.flag0 != 0;
    if (s->seen < 255)
        s->seen++;
}

///////////////////////
/// Stage floors    ///
///////////////////////

static float Dist2(Vec2 *a, Vec2 *b)
{
    float dx = a->X - b->X;
    float dy = a->Y - b->Y;
    return dx * dx + dy * dy;
}

// mpLineGetPrev / mpLineGetNext
static int Line_Prev(RawCollLine *lines, CollVert *verts, int id)
{
    CollLineDesc *desc = lines[id].desc;
    int alt = desc->line_prev_altgroup;
    if (alt != -1)
    {
        u32 flags = lines[alt].flags;
        if ((flags & LINEFLAG_ENABLED) && !(flags & LINEFLAG_HIDDEN))
        {
            Vec2 *v0 = &verts[(u16)desc->vert_prev].pos_curr;
            Vec2 *v1 = &verts[(u16)lines[alt].desc->vert_next].pos_curr;
            if (Dist2(v0, v1) < 4.0f)
                return alt;
        }
    }
    return desc->line_prev;
}

static int Line_Next(RawCollLine *lines, CollVert *verts, int id)
{
    CollLineDesc *desc = lines[id].desc;
    int alt = desc->line_next_altgroup;
    if (alt != -1)
    {
        u32 flags = lines[alt].flags;
        if ((flags & LINEFLAG_ENABLED) && !(flags & LINEFLAG_HIDDEN))
        {
            Vec2 *v1 = &verts[(u16)desc->vert_next].pos_curr;
            Vec2 *v0 = &verts[(u16)lines[alt].desc->vert_prev].pos_curr;
            if (Dist2(v1, v0) < 4.0f)
                return alt;
        }
    }
    return desc->line_next;
}

// mpLib_8004ED5C: line ends, pushed out one unit where the line connects to
// another one.
static void Line_GetEnds(RawCollLine *lines, CollVert *verts, int id, float *x0, float *y0, float *x1, float *y1)
{
    CollLineDesc *desc = lines[id].desc;
    float ax = verts[(u16)desc->vert_prev].pos_curr.X;
    float ay = verts[(u16)desc->vert_prev].pos_curr.Y;
    float bx = verts[(u16)desc->vert_next].pos_curr.X;
    float by = verts[(u16)desc->vert_next].pos_curr.Y;
    float dist = 0;
    int have_dist = 0;

    if (Line_Prev(lines, verts, id) != -1)
    {
        dist = sqrtf((ax - bx) * (ax - bx) + (ay - by) * (ay - by));
        if (dist > 0.001f)
        {
            ax += (ax - bx) / dist;
            ay += (ay - by) / dist;
        }
        have_dist = 1;
    }

    // like the game, this uses the already extended start with the original length
    if (Line_Next(lines, verts, id) != -1)
    {
        if (!have_dist)
            dist = sqrtf((ax - bx) * (ax - bx) + (ay - by) * (ay - by));
        if (dist > 0.001f)
        {
            bx += (bx - ax) / dist;
            by += (by - ay) / dist;
        }
    }

    *x0 = ax;
    *y0 = ay;
    *x1 = bx;
    *y1 = by;
}

static double dabs(double v)
{
    return v < 0 ? -v : v;
}

// mpLineIntersection: does the movement b0->b1 cross line a0->a1 from above?
static int Line_Cross(double a0x, double a0y, double a1x, double a1y,
                      double b0x, double b0y, double b1x, double b1y)
{
    int b0_slightly_below = 0;
    int b1_slightly_above = 0;

    // b entirely left/right of a
    if (a0x <= a1x)
    {
        if ((b0x < a0x && b1x < a0x) || (a1x < b0x && a1x < b1x))
            return 0;
    }
    else
    {
        if ((b0x < a1x && b1x < a1x) || (a0x < b0x && a0x < b1x))
            return 0;
    }

    // b entirely above/below a
    if (a0y <= a1y)
    {
        if ((b0y < a0y && b1y < a0y) || (a1y < b0y && a1y < b1y))
            return 0;
    }
    else
    {
        if ((b0y < a1y && b1y < a1y) || (a0y < b0y && a0y < b1y))
            return 0;
    }

    double ah = a1y - a0y;
    double aw = a1x - a0x;
    double d0x = b0x - a0x;
    double d0y = b0y - a0y;
    double side0 = (aw * d0y) - (ah * d0x);
    if (side0 < 0.0)
    {
        if (side0 < -0.1)
            return 0;
        b0_slightly_below = 1;
    }

    double d1x = b1x - a1x;
    double d1y = b1y - a1y;
    double side1 = (aw * d1y) - (ah * d1x);
    if (side1 > 0.0)
    {
        if (side1 > 0.1)
            return 0;
        b1_slightly_above = 1;
    }

    if (side0 == 0.0 && side1 == 0.0)
        return 0;

    double det = (d0x * d1y) - (d0y * d1x);
    if (det < side0)
    {
        if (det < side1)
            return 0;
    }
    else if (det > side0)
    {
        if (det > side1)
            return 0;
    }

    double bw = b1x - b0x;
    double bh = b1y - b0y;
    if ((bw == 0.0 && bh == 0.0) || (b0_slightly_below && b1_slightly_above) ||
        (side0 >= 0.0 && b1_slightly_above))
        return 0;

    double area = (bw * ah) - (bh * aw);
    return dabs(area) > 0.0001f;
}

// mpLineIntersectionH: the same test for a flat line at height a0y.
static int Line_CrossFlat(double a0x, double a0y, double a1x,
                          double b0x, double b0y, double b1x, double b1y)
{
    double min_ax, max_ax;

    if (a0x < a1x)
    {
        if ((b0x < a0x && b1x < a0x) || (a1x < b0x && a1x < b1x))
            return 0;
        if (b0y - a0y < -0.0001 || b1y - a0y > 0.0001)
            return 0;
        min_ax = a0x;
        max_ax = a1x;
    }
    else
    {
        if ((b0x < a1x && b1x < a1x) || (a0x < b0x && a0x < b1x))
            return 0;
        if (b1y - a0y < -0.0001 || b0y - a0y > 0.0001)
            return 0;
        min_ax = a1x;
        max_ax = a0x;
    }

    double dby = b1y - b0y;
    double dbx = b1x - b0x;
    if (dabs(dby) < 0.0001)
        return 0;

    double x = dbx / dby * (a0y - b0y) + b0x;
    if (x - min_ax < -0.1)
        return 0;
    if (x - max_ax > 0.1)
        return 0;
    return 1;
}

// The floors don't move within a frame, so their ends are worked out once per
// frame instead of on every simulated step.
#define LL_MAX_FLOORS 512

typedef struct FloorLine
{
    float x0, y0, x1, y1;
    int id;
    u8 is_platform;
} FloorLine;

static FloorLine *floor_cache; // [LL_MAX_FLOORS]
static int floor_num;

static void Floor_CacheRange(RawCollLine *lines, CollVert *verts, int start, int num)
{
    for (int i = 0; i < num && floor_num < LL_MAX_FLOORS; i++)
    {
        int id = start + i;
        u32 flags = lines[id].flags;
        if (!(flags & LINEFLAG_FLOOR) || !(flags & LINEFLAG_ENABLED) || (flags & LINEFLAG_EMPTY))
            continue;

        FloorLine *f = &floor_cache[floor_num++];
        Line_GetEnds(lines, verts, id, &f->x0, &f->y0, &f->x1, &f->y1);
        f->id = id;
        f->is_platform = lines[id].desc->is_unk; // is_unk is the platform flag
    }
}

static void Floor_BuildCache(void)
{
    RawCollLine *lines = (RawCollLine *)*stc_collline;
    CollVert *verts = *stc_collvert;

    floor_num = 0;
    for (CollGroup *group = *stc_firstcollgroup; group != 0; group = group->next)
    {
        CollGroupDesc *desc = group->desc;
        Floor_CacheRange(lines, verts, desc->floor_start, desc->floor_num);
        Floor_CacheRange(lines, verts, desc->dyn_start, desc->dyn_num);
    }
}

// mpCheckFloor: does the ECB bottom moving from a to b touch down on a floor?
static int Floor_Check(float ax, float ay, float bx, float by, int pass_platforms, int skip_line)
{
    for (int i = 0; i < floor_num; i++)
    {
        FloorLine *f = &floor_cache[i];
        if (f->id == skip_line || (pass_platforms && f->is_platform))
            continue;

        if (fabs(f->y0 - f->y1) > 0.0001f)
        {
            if (Line_Cross(f->x0, f->y0, f->x1, f->y1, ax, ay, bx, by))
                return 1;
        }
        else if (ay >= by && Line_CrossFlat(f->x0, f->y0, f->x1, ax, ay, bx, by))
            return 1;
    }
    return 0;
}

///////////////////////
/// Simulation      ///
///////////////////////

// Everything the simulation needs about the starting frame. Filled from the
// live fighter for now; later modes can start from a saved or chosen spot.
typedef struct SimStart
{
    Vec2 pos;
    Vec2 vel;
    float stick_x;
    float stick_y;
    float facing;
    float bottom;        // ECB bottom used on the starting frame
    float locked_bottom; // bottom kept while the post-jump lock lasts
    int ts;              // tracked state
    int frame;           // frames in that state
    int len;             // frames the state lasts, 0 = loops or unknown
    int len_estimated;   // len came from the animation length, not from play
    int fastfall;
    int tilt_timer;      // frames since the stick entered its y tilt zone
    int trigger_timer;   // frames since L/R/Z
    int ecb_lock;
    int skip_line;
} SimStart;

// The simulated fighter between two frames.
typedef struct SimState
{
    float x, y, vx, vy;
    float bottom;         // ECB bottom used on the last frame
    float prev_x, prev_y; // ECB bottom point of the last frame
    int ts;
    int frame;
    int len;
    int len_estimated;
    int fastfall;
    int tilt_timer;
    int trigger_timer;
    int lock;
    int aerial_pending; // in an aerial whose own ECB bottom hasn't been used yet (lock)
} SimState;

// What happened on one simulated frame.
typedef struct SimStep
{
    int landed;
    int first_aerial_ecb; // the aerial's own ECB bottom was used for the first time
    int unlearned;        // relied on data the event hasn't learned yet
    int fastfall_started;
    EcbSample *ecb;       // learned ECB for this frame
} SimStep;

// Aerials, as bits in the masks below
#define AERIAL_BIT(ts) (1 << ((ts) - TS_AIRN))
#define LL_AI_MAX_STEPS 12 // the bottom lock lasts at most 10 frames

typedef struct Prediction
{
    int num;            // last valid entry in the arrays below
    int land_frame;     // frame of touchdown, 0 = none within LL_SIM_FRAMES
    int land_kind;
    int lag;            // landing lag of the predicted landing
    int lcancel_lag;    // aerial lag if L-cancelled (aerial landings only)
    int uncertain_from; // first frame that relied on unlearned data
    int fastfall_frame; // frame the fastfall starts, 0 = none
    float facing;
    Vec2 pos[LL_SIM_FRAMES + 1];       // fighter position per frame, [0] = start
    float bottom[LL_SIM_FRAMES + 1];   // ECB bottom per frame
    s8 ts[LL_SIM_FRAMES + 1];          // tracked state per frame
    EcbSample land_ecb;                // ECB at touchdown

    // aerial interrupts: pressing an aerial on frame k whose ECB then
    // touches down the first time it is used
    u8 ai_mask[LL_SIM_FRAMES + 1];     // aerials that interrupt when pressed on frame k
    u8 ai_lag_mask[LL_SIM_FRAMES + 1]; // ... of those, the ones that land with aerial lag
    u8 ai_unlearned;                   // aerials skipped because their ECB isn't learned yet
    int ai_first;                      // first frame of the first window, 0 = none
    int ai_width;                      // frames in that window
    u8 ai_aerials;                     // aerials that work somewhere in that window
} Prediction;

static float common_fastfall_stick;
static int common_fastfall_window;
static int common_lcancel_window;
static float common_lcancel_div;
static float common_platform_drop;
static float common_aerial_stick_x;
static float common_aerial_stick_y;
static float common_aerial_angle;

static float Common_Float(int offset)
{
    return *(float *)((u8 *)*stc_ftcommon + offset);
}

static int Common_Int(int offset)
{
    return *(int *)((u8 *)*stc_ftcommon + offset);
}

// ftCommon_CalcSelfAccel_DriftFrom with the stick held at stick_x
static float Drift_Accel(FighterData *fp, float vel, float stick_x)
{
    float friction = fp->attr.aerial_friction;
    float target = stick_x * fp->attr.aerial_drift_max;
    float max_vel = fp->attr.horizontal_air_mobility_constant;
    float accel;

    if (target == 0)
    {
        // ftCommon_CalcSelfAccel_Deaccel
        accel = friction;
        if (fabs(accel) >= fabs(vel))
            accel = -vel;
        else if (vel > 0)
            accel = -accel;
        return accel;
    }

    accel = stick_x * fp->attr.aerial_drift_stick_mult;
    if (stick_x > 0)
        accel += fp->attr.aerial_drift_base;
    else
        accel -= fp->attr.aerial_drift_base;

    if (!(vel * accel < 0))
    {
        if (accel > 0)
        {
            if (vel + accel > target)
            {
                accel = -friction;
                if (vel + accel < target)
                    accel = target - vel;
                if (vel + accel > max_vel)
                    accel = max_vel - vel;
            }
        }
        else if (vel + accel < target)
        {
            accel = friction;
            if (vel + accel > target)
                accel = target - vel;
            if (vel + accel < -max_vel)
                accel = -max_vel - vel;
        }
    }
    return accel;
}

// Frames until the current animation runs out, from its length and rate.
// Only used until the event has seen the state end once.
static int State_EstimateLength(FighterData *fp, int frame)
{
    Figatree *anim = fp->figatree_curr;
    float rate = fp->state.rate;
    if (anim == 0 || rate <= 0)
        return 0;

    int k = 1;
    while (fp->state.frame + k * rate < anim->frame_num && k < LL_SIM_FRAMES)
        k++;
    return frame + k;
}

static void Sim_FromFighter(FighterData *fp, int ts, int frame, SimStart *s)
{
    CollData *cd = &fp->coll_data;

    s->pos.X = fp->phys.pos.X;
    s->pos.Y = fp->phys.pos.Y;
    s->vel.X = fp->phys.self_vel.X;
    s->vel.Y = fp->phys.self_vel.Y;
    s->stick_x = fp->input.lstick.X;
    s->stick_y = fp->input.lstick.Y;
    s->facing = fp->facing_direction;
    s->bottom = cd->ecbCurrCorrect_bot.Y;
    s->locked_bottom = cd->ecbCurr_bot.Y;
    s->ts = ts;
    s->frame = frame;
    s->len = 0;
    s->len_estimated = 0;
    if (Tracked_NaturalNext(ts) >= 0)
    {
        s->len = state_len[ts];
        if (s->len == 0)
        {
            s->len = State_EstimateLength(fp, frame);
            s->len_estimated = 1;
        }
    }
    s->fastfall = fp->flags.is_fastfall;
    s->tilt_timer = (u8)fp->input.timer_lstick_tilt_y;
    s->trigger_timer = (u8)fp->input.timer_trigger_any_ignore_hitlag;
    s->ecb_lock = cd->u.ecb_bot_lock_frames;
    s->skip_line = cd->ignore_line;
}

static void Sim_Init(SimStart *start, SimState *s)
{
    s->x = start->pos.X;
    s->y = start->pos.Y;
    s->vx = start->vel.X;
    s->vy = start->vel.Y;
    s->bottom = start->bottom;
    s->prev_x = s->x;
    s->prev_y = s->y + s->bottom;
    s->ts = start->ts;
    s->frame = start->frame;
    s->len = start->len;
    s->len_estimated = start->len_estimated;
    s->fastfall = start->fastfall;
    s->tilt_timer = start->tilt_timer;
    s->trigger_timer = start->trigger_timer;
    s->lock = start->ecb_lock;
    // an aerial is only ever entered in the air, after the lock was set, so
    // a lock still running means its bones haven't moved the bottom yet
    s->aerial_pending = Tracked_IsAerial(start->ts) && start->ecb_lock > 0;
}

// One frame, in the game's order: animation (the state can end), interrupt
// (press, an aerial replaces the state), input timers, physics, ECB, floor
// test. press is the aerial pressed this frame, or -1.
static void Sim_Step(FighterData *fp, SimStart *start, SimState *s, int press, SimStep *out)
{
    out->landed = 0;
    out->first_aerial_ecb = 0;
    out->unlearned = 0;
    out->fastfall_started = 0;

    // animation: the state ends by itself when its animation runs out
    s->frame++;
    if (s->len > 0 && s->frame >= s->len)
    {
        int next = Tracked_Next(s->ts);
        if (s->len_estimated)
            out->unlearned = 1;

        if (next == TS_FALL)
        {
            // ftCo_Fall_Enter clamps horizontal speed to the drift max
            float max = fp->attr.aerial_drift_max;
            if (s->vx < -max)
                s->vx = -max;
            else if (s->vx > max)
                s->vx = max;
        }
        else if (next == TS_FALLAERIAL)
        {
            // ftCo_FallAerial_Enter doesn't keep a fastfall
            s->fastfall = 0;
        }

        s->ts = next;
        s->frame = 0;
        s->len = 0;
        s->len_estimated = 0;
        s->aerial_pending = 0;
    }

    // interrupt: ftCo_AttackAir_EnterFromMsid keeps the fastfall and starts
    // the aerial's animation on this frame, before physics and collision
    if (press >= 0)
    {
        s->ts = press;
        s->frame = 0;
        s->len = state_len[press];
        s->len_estimated = 0;
        s->aerial_pending = 1;
    }

    // input: the stick is held, so its timers keep counting
    if (s->tilt_timer < LL_TIMER_MAX)
        s->tilt_timer++;
    if (s->trigger_timer < LL_TIMER_MAX)
        s->trigger_timer++;

    // physics (ft_80084DB0)
    if (!s->fastfall && s->vy < 0 && start->stick_y <= -common_fastfall_stick && s->tilt_timer < common_fastfall_window)
    {
        s->fastfall = 1;
        s->tilt_timer = LL_TIMER_MAX;
        out->fastfall_started = 1;
    }
    if (s->fastfall)
        s->vy = -fp->attr.fastfall_velocity;
    else
    {
        s->vy -= fp->attr.gravity;
        if (s->vy < -fp->attr.terminal_velocity)
            s->vy = -fp->attr.terminal_velocity;
    }
    s->vx += Drift_Accel(fp, s->vx, start->stick_x);
    s->x += s->vx;
    s->y += s->vy;

    // ECB bottom: the lock counts down just before collision
    EcbSample *e = Ecb_Get(s->ts, s->frame);
    out->ecb = e;
    if (s->lock > 0)
        s->lock--;
    if (s->lock > 0)
        s->bottom = start->locked_bottom;
    else if (e->has_bottom)
        s->bottom = e->bottom;
    else
        out->unlearned = 1; // not learned yet: keep the last bottom
    if (Tracked_IsAerial(s->ts))
    {
        if (!e->seen)
            out->unlearned = 1;
        if (s->aerial_pending && s->lock <= 0)
        {
            out->first_aerial_ecb = 1;
            s->aerial_pending = 0;
        }
    }

    // floor test (mpColl_80044628_Floor)
    int pass_platforms = Tracked_UsesPlatformDrop(s->ts) && start->stick_y <= common_platform_drop;
    float bx = s->x;
    float by = s->y + s->bottom;
    out->landed = Floor_Check(s->prev_x, s->prev_y, bx, by, pass_platforms, start->skip_line);
    s->prev_x = bx;
    s->prev_y = by;
}

static void Mark_Uncertain(Prediction *p, int frame)
{
    if (p->uncertain_from > frame)
        p->uncertain_from = frame;
}

// What a touchdown in state ts gives: ft_80082B1C for jumps and falls,
// ftCo_LandingAir_EnterWithLag for aerials. An aerial whose own ECB touches
// down the first time it's used is an aerial interrupt, whatever its lag.
static int Landing_Kind(FighterData *fp, int ts, float vel_y, int trigger_timer, EcbSample *e,
                        int first_aerial_ecb, int *lag, int *lcancel_lag)
{
    float normal_lag = fp->attr.normal_landing_lag;
    *lcancel_lag = 0;

    if (!Tracked_IsAerial(ts))
    {
        if (vel_y > Fighter_GetSoftLandVelocity(fp))
        {
            *lag = 0;
            return LAND_NIL;
        }
        *lag = (int)normal_lag;
        return LAND_NORMAL;
    }

    if (!e->aerial_lag)
    {
        *lag = (int)normal_lag;
        return first_aerial_ecb ? LAND_AI : LAND_NORMAL;
    }

    float full = Aerial_LandingLag(fp, ts);
    int halved = (int)(full / common_lcancel_div);
    if (halved == 0)
        halved = 1;
    *lcancel_lag = halved;

    int lcancel = trigger_timer < common_lcancel_window;
    *lag = lcancel ? halved : (int)full;
    if (first_aerial_ecb)
        return LAND_AI;
    return lcancel ? LAND_LCANCEL : LAND_AERIAL;
}

// Would pressing each aerial on frame k force a touchdown? before is the
// state just before frame k. The aerial's ECB is used right away, or when the
// lock runs out; it's an aerial interrupt if that first use touches down.
static void Branch_Aerials(FighterData *fp, SimStart *start, SimState *before, Prediction *p, int k)
{
    // aerials can't be interrupted by another aerial (until their IASA,
    // which isn't tracked), but jumps and falls can on any frame
    if (Tracked_IsAerial(p->ts[k]))
        return;

    for (int a = TS_AIRN; a <= TS_AIRLW; a++)
    {
        SimState b = *before;
        SimStep step;

        for (int n = 0; n < LL_AI_MAX_STEPS; n++)
        {
            Sim_Step(fp, start, &b, n == 0 ? a : -1, &step);

            // the aerial's frames must have been seen, and its bottom too
            // once the lock is over
            if (!step.ecb->seen || (b.lock <= 0 && !step.ecb->has_bottom))
            {
                p->ai_unlearned |= AERIAL_BIT(a);
                break;
            }
            if (step.landed)
            {
                if (step.first_aerial_ecb)
                {
                    p->ai_mask[k] |= AERIAL_BIT(a);
                    if (step.ecb->aerial_lag)
                        p->ai_lag_mask[k] |= AERIAL_BIT(a);
                }
                break; // touching down on the locked bottom isn't an interrupt
            }
            if (!b.aerial_pending)
                break; // the aerial's ECB is in use and stayed in the air
        }
    }
}

// The first run of frames where some aerial interrupts.
static void Ai_Summarize(Prediction *p)
{
    int last = p->land_frame ? p->land_frame - 1 : p->num;

    p->ai_first = 0;
    p->ai_width = 0;
    p->ai_aerials = 0;
    for (int k = 1; k <= last; k++)
    {
        if (p->ai_mask[k])
        {
            if (p->ai_first == 0)
                p->ai_first = k;
            p->ai_width++;
            p->ai_aerials |= p->ai_mask[k];
        }
        else if (p->ai_first)
            break;
    }
}

// Simulate keeping the stick where it is and pressing nothing. With
// branches, also try every aerial on every frame along the way.
static void Predict(FighterData *fp, SimStart *start, Prediction *p, int branches)
{
    SimState s;
    Sim_Init(start, &s);

    p->num = 0;
    p->land_frame = 0;
    p->land_kind = LAND_NONE;
    p->lag = 0;
    p->lcancel_lag = 0;
    p->uncertain_from = LL_SIM_FRAMES + 1;
    p->fastfall_frame = 0;
    p->facing = start->facing;
    p->pos[0].X = s.x;
    p->pos[0].Y = s.y;
    p->bottom[0] = s.bottom;
    p->ts[0] = s.ts;
    p->ai_mask[0] = 0;
    p->ai_lag_mask[0] = 0;
    p->ai_unlearned = 0;

    for (int k = 1; k <= LL_SIM_FRAMES; k++)
    {
        SimState before = s;
        SimStep step;
        Sim_Step(fp, start, &s, -1, &step);

        if (step.unlearned)
            Mark_Uncertain(p, k);
        if (step.fastfall_started)
            p->fastfall_frame = k;

        p->pos[k].X = s.x;
        p->pos[k].Y = s.y;
        p->bottom[k] = s.bottom;
        p->ts[k] = s.ts;
        p->ai_mask[k] = 0;
        p->ai_lag_mask[k] = 0;
        p->num = k;

        if (step.landed)
        {
            p->land_frame = k;
            p->land_ecb = *step.ecb;
            p->land_kind = Landing_Kind(fp, s.ts, s.vy, s.trigger_timer, step.ecb,
                                        step.first_aerial_ecb, &p->lag, &p->lcancel_lag);
            break;
        }

        // pressing an aerial on the frame you'd land anyway isn't an interrupt
        if (branches)
            Branch_Aerials(fp, start, &before, p, k);
    }

    Ai_Summarize(p);
}

///////////////////////
/// Event state     ///
///////////////////////

void Event_Exit(GOBJ *menu);
void Event_ClearLearned(GOBJ *menu);
void Event_ChangeCollDisplay(GOBJ *menu, int value);

static const char *speed_names[] = {"1", "5/6", "2/3", "1/2", "1/4"};
static const float speed_values[] = {1.f, 5.f / 6.f, 2.f / 3.f, 1.f / 2.f, 1.f / 4.f};

enum options_main
{
    OPT_PATH,
    OPT_PANEL,
    OPT_SOUND,
    OPT_SPEED,
    OPT_COLL,
    OPT_LOG,
    OPT_CLEAR,
    OPT_HELP,
    OPT_EXIT,

    OPT_COUNT
};

static EventOption Options_Main[OPT_COUNT] = {
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Landing Path",
        .val = 1,
        .desc = {"Draw where Falcon lands if you keep holding the",
                 "stick and press nothing, colored by landing type.",
                 "Ticks mark where pressing an aerial lands you."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Info Panel",
        .val = 1,
        .desc = {"Show the predicted landing, which aerials can",
                 "interrupt, how your last landing compared, and",
                 "how often they matched."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Success Sound",
        .val = 1,
        .desc = {"Play a sound when you land a NIL or an",
                 "aerial interrupt."},
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Game Speed",
        .value_num = countof(speed_names),
        .values = speed_names,
        .desc = {"Slow the game down to practice."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Collision Display",
        .desc = {"Show stage collision lines and ECBs."},
        .OnChange = Event_ChangeCollDisplay,
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "ECB Log",
        .desc = {"Print each airborne frame's state, position,",
                 "speed and ECB to the Dolphin log."},
    },
    {
        .kind = OPTKIND_FUNC,
        .name = "Forget Learned ECBs",
        .desc = {"Clear the ECB data learned this session."},
        .OnSelect = Event_ClearLearned,
    },
    {
        .kind = OPTKIND_INFO,
        .name = "Help",
        .desc = {"Path: green NIL, yellow landing, orange L-cancel,",
                 "red full lag. Cyan tick: an aerial pressed there",
                 "lands you (AI). Gray: still learning, so do each",
                 "jump and aerial once, high up. White: your path."},
    },
    {
        .kind = OPTKIND_FUNC,
        .name = "Exit",
        .desc = {"Return to the Event Selection Screen."},
        .OnSelect = Event_Exit,
    },
};

static EventMenu Menu_Main = {
    .name = "Landing Lab",
    .option_num = countof(Options_Main),
    .options = Options_Main,
};

EventMenu *Event_Menu = &Menu_Main;

// live tracking
static int prev_state_id = -1;
static int prev_ts = -1;
static int prev_tracked_air;
static int prev_tilt_timer;
static int prev_lock;
static int frame_in_state;
static int attributes_logged;

// predictions: live is recomputed every airborne frame; seg is the one made
// when the player last changed input, which a landing is judged against.
// These and the other big buffers are allocated in Event_Init: large static
// arrays make hmex crash in release builds.
static Prediction *pred_live;
static Prediction *pred_seg;
static int live_visible;
static int seg_valid;
static int seg_start_timer;
static int seg_drift_logged;
static float seg_stick_x;
static int seg_stick_down;
static int seg_stick_drop;

// real path since the segment started, shown with pred_seg after landing
static Vec2 *actual_pos;      // [LL_SIM_FRAMES + 1]
static float *actual_bottom;  // [LL_SIM_FRAMES + 1]
static int actual_num;
static int ghost_visible;

// HUD
static int stat_total;
static int stat_exact;
static char text_predict[32] = "-";
static char text_ai[32] = "-";
static char text_last[32] = "-";
static char text_exact[32] = "-";
static char *panel_labels[] = {"Prediction", "AI window", "Last landing", "Exact"};
static char *panel_info[] = {text_predict, text_ai, text_last, text_exact};

///////////////////////
/// Logging         ///
///////////////////////

static void Log(char *buf)
{
    OSReport("%s", buf);
}

static void Log_Attributes(FighterData *fp)
{
    char buf[200];

    sprintf(buf, "LandingLab attrs: gravity %.5f terminal %.5f fastfall %.5f\n",
            fp->attr.gravity, fp->attr.terminal_velocity, fp->attr.fastfall_velocity);
    Log(buf);
    sprintf(buf, "LandingLab attrs: drift mult %.5f base %.5f max %.5f friction %.5f air max %.5f\n",
            fp->attr.aerial_drift_stick_mult, fp->attr.aerial_drift_base, fp->attr.aerial_drift_max,
            fp->attr.aerial_friction, fp->attr.horizontal_air_mobility_constant);
    Log(buf);
    sprintf(buf, "LandingLab attrs: jump h %.5f v %.5f hop v %.5f dj v mult %.5f dj h mult %.5f\n",
            fp->attr.jump_h_initial_velocity, fp->attr.jump_v_initial_velocity, fp->attr.hop_v_initial_velocity,
            fp->attr.air_jump_v_multiplier, fp->attr.air_jump_h_multiplier);
    Log(buf);
    sprintf(buf, "LandingLab attrs: landing lag %.1f nair %.1f fair %.1f bair %.1f uair %.1f dair %.1f\n",
            fp->attr.normal_landing_lag, fp->attr.n_air_landing_lag, fp->attr.f_air_landing_lag,
            fp->attr.b_air_landing_lag, fp->attr.u_air_landing_lag, fp->attr.d_air_landing_lag);
    Log(buf);
    sprintf(buf, "LandingLab common: NIL above vel.y %.5f, fastfall stick %.4f window %d, platform drop stick %.4f, L-cancel window %d div %.2f\n",
            Fighter_GetSoftLandVelocity(fp), common_fastfall_stick, common_fastfall_window,
            common_platform_drop, common_lcancel_window, common_lcancel_div);
    Log(buf);
    sprintf(buf, "LandingLab common: nair below stick x %.4f y %.4f, uair/dair above %.4f rad\n",
            common_aerial_stick_x, common_aerial_stick_y, common_aerial_angle);
    Log(buf);
}

static void Log_Frame(FighterData *fp, int ts, int frame)
{
    char buf[256];
    CollData *cd = &fp->coll_data;

    sprintf(buf, "LL %d %s f%d pos %.4f %.4f vel %.5f %.5f ff%d lock%d ecb top %.4f bot %.4f l %.4f r %.4f side %.4f used bot %.4f ac%d stick %.4f %.4f\n",
            event_vars->game_timer, tracked_state_names[ts], frame,
            fp->phys.pos.X, fp->phys.pos.Y, fp->phys.self_vel.X, fp->phys.self_vel.Y,
            fp->flags.is_fastfall, cd->u.ecb_bot_lock_frames,
            cd->ecbCurr_top.Y, cd->ecbCurr_bot.Y, cd->ecbCurr_left.X, cd->ecbCurr_right.X, cd->ecbCurr_right.Y,
            cd->ecbCurrCorrect_bot.Y, fp->ftcmd_var.flag0 != 0,
            fp->input.lstick.X, fp->input.lstick.Y);
    Log(buf);
}

///////////////////////
/// Segments        ///
///////////////////////

static int Stick_Down(float y)
{
    return y <= -common_fastfall_stick;
}

static int Stick_Drop(float y)
{
    return y <= common_platform_drop;
}

// The player did something "keep holding" didn't assume.
static int Segment_InputChanged(FighterData *fp, int ts)
{
    if (!seg_valid)
        return 1;
    if (event_vars->game_timer - seg_start_timer >= LL_SIM_FRAMES)
        return 1;
    if (fp->input.lstick.X != seg_stick_x)
        return 1;
    if (Stick_Down(fp->input.lstick.Y) != seg_stick_down)
        return 1;
    if (Stick_Drop(fp->input.lstick.Y) != seg_stick_drop)
        return 1;
    if ((u8)fp->input.timer_lstick_tilt_y < prev_tilt_timer) // a new flick
        return 1;
    if (ts != prev_ts && !Tracked_EndedInto(prev_ts, ts)) // new aerial, double jump, ...
        return 1;
    return 0;
}

static void Segment_Start(FighterData *fp)
{
    memcpy(pred_seg, pred_live, sizeof(*pred_seg));
    seg_valid = 1;
    seg_start_timer = event_vars->game_timer;
    seg_drift_logged = 0;
    seg_stick_x = fp->input.lstick.X;
    seg_stick_down = Stick_Down(fp->input.lstick.Y);
    seg_stick_drop = Stick_Drop(fp->input.lstick.Y);
    actual_num = 0;
}

static void Segment_AddActual(FighterData *fp)
{
    int k = event_vars->game_timer - seg_start_timer;
    if (k < 0 || k > LL_SIM_FRAMES || k > actual_num)
        return;
    actual_pos[k].X = fp->phys.pos.X;
    actual_pos[k].Y = fp->phys.pos.Y;
    actual_bottom[k] = fp->coll_data.ecbCurrCorrect_bot.Y;
    actual_num = k + 1;
}

// Log the first frame where the real path leaves the predicted one while the
// input is unchanged. That points at a gap in the model.
static void Segment_CheckDrift(FighterData *fp, int ts)
{
    int k = event_vars->game_timer - seg_start_timer;
    if (seg_drift_logged || k < 1 || k > pred_seg->num)
        return;

    float dx = fp->phys.pos.X - pred_seg->pos[k].X;
    float dy = fp->phys.pos.Y - pred_seg->pos[k].Y;
    if (fabs(dx) < 0.001f && fabs(dy) < 0.001f && pred_seg->ts[k] == ts)
        return;

    char buf[200];
    sprintf(buf, "LandingLab drift at +%d: predicted %s %.4f %.4f, actual %s %.4f %.4f vel %.5f %.5f\n",
            k, tracked_state_names[(int)pred_seg->ts[k]], pred_seg->pos[k].X, pred_seg->pos[k].Y,
            tracked_state_names[ts], fp->phys.pos.X, fp->phys.pos.Y,
            fp->phys.self_vel.X, fp->phys.self_vel.Y);
    Log(buf);
    seg_drift_logged = 1;
}

///////////////////////
/// HUD text        ///
///////////////////////

static void Text_Prediction(Prediction *p)
{
    if (p->land_frame == 0)
        sprintf(text_predict, "No landing");
    else if (p->uncertain_from <= p->land_frame)
        sprintf(text_predict, "Learning...");
    else if (p->land_kind == LAND_NIL)
        sprintf(text_predict, "NIL");
    else if (p->land_kind == LAND_AERIAL)
        sprintf(text_predict, "Lag %d (L: %d)", p->lag, p->lcancel_lag);
    else
        sprintf(text_predict, "%s, %d lag", land_kind_names[p->land_kind], p->lag);
}

// Which aerials can interrupt in the first window ahead, and for how many
// frames. Stays the same while the window comes closer.
static void Text_Ai(Prediction *p)
{
    static const char letters[] = "NFBUD";
    u8 mask = p->ai_aerials;
    char *prefix = "";

    if (p->ai_first && p->ai_first >= p->uncertain_from)
    {
        sprintf(text_ai, "Learning...");
        return;
    }
    if (!p->ai_first)
    {
        if (!p->ai_unlearned)
        {
            sprintf(text_ai, "-");
            return;
        }
        mask = p->ai_unlearned; // name the aerials still to learn
        prefix = "Learn ";
    }

    int n = 0;
    while (prefix[n])
    {
        text_ai[n] = prefix[n];
        n++;
    }
    for (int a = 0; a < 5; a++)
    {
        if (!(mask & (1 << a)))
            continue;
        if (n > 0 && text_ai[n - 1] != ' ')
            text_ai[n++] = ' ';
        text_ai[n++] = letters[a];
    }
    text_ai[n] = 0;
    if (p->ai_first)
        sprintf(text_ai + n, ", %df", p->ai_width);
}

// L/R/Z doesn't change the path, so a full-lag prediction that you then
// L-cancelled still counts as a match.
static int Kind_Matches(int a, int b)
{
    if (a == b)
        return 1;
    return (a == LAND_LCANCEL || a == LAND_AERIAL) && (b == LAND_LCANCEL || b == LAND_AERIAL);
}

static void Text_Exact(void)
{
    sprintf(text_exact, "%d / %d", stat_exact, stat_total);
}

// The aerial an A press or C-stick flick starts on this frame, or -1
// (ftCo_AttackAir_CheckItemThrowInput, ftCo_800DF478 and
// ftCo_AttackAir_GetMsidFromCStick).
static int Aerial_Pressed(FighterData *fp)
{
    float cx = fp->input.cstick.X;
    float cy = fp->input.cstick.Y;
    int cstick = (fabs(fp->input.cstick_prev.X) < common_aerial_stick_x && fabs(cx) >= common_aerial_stick_x) ||
                 (fabs(fp->input.cstick_prev.Y) < common_aerial_stick_y && fabs(cy) >= common_aerial_stick_y);
    if (!cstick && !(fp->input.down & HSD_BUTTON_A))
        return -1;

    float x = cstick ? cx : fp->input.lstick.X;
    float y = cstick ? cy : fp->input.lstick.Y;
    if (fabs(x) < common_aerial_stick_x && fabs(y) < common_aerial_stick_y)
        return TS_AIRN;

    float angle = atan2(y, fabs(x));
    if (angle > common_aerial_angle)
        return TS_AIRHI;
    if (angle < -common_aerial_angle)
        return TS_AIRLW;
    return x * fp->facing_direction >= 0 ? TS_AIRF : TS_AIRB;
}

// An aerial pressed in the air, on a frame where the prediction said it would
// interrupt, should have touched down right away. If its ECB isn't in use
// yet (lock), the landing itself is judged later instead.
static void Ai_CheckMissed(FighterData *fp, int ts)
{
    if (!seg_valid || !Tracked_IsAerial(ts) || Tracked_IsAerial(prev_ts))
        return;
    if (fp->coll_data.u.ecb_bot_lock_frames > 0)
        return;

    int k = event_vars->game_timer - seg_start_timer;
    if (k < 1 || k > pred_seg->num || k >= pred_seg->uncertain_from)
        return;
    if (!(pred_seg->ai_mask[k] & AERIAL_BIT(ts)))
        return;

    stat_total++;
    Text_Exact();
    sprintf(text_last, "No AI, predicted");

    char buf[200];
    sprintf(buf, "LandingLab ai miss: %s at %d x %.4f y %.4f stayed in the air, predicted an aerial interrupt (from %d)\n",
            tracked_state_names[ts], event_vars->game_timer, fp->phys.pos.X, fp->phys.pos.Y, seg_start_timer);
    Log(buf);
}

// The fighter left the tracked air states this frame: judge the landing.
static void Landing_Resolve(FighterData *fp)
{
    int sid = fp->state_id;
    int is_landing = sid == ASID_LANDING || (sid >= ASID_LANDINGAIRN && sid <= ASID_LANDINGAIRLW);

    sprintf(text_ai, "-");
    if (sid != ASID_WAIT && !is_landing)
    {
        // ledge grab, airdodge, special, ...: not a landing
        live_visible = 0;
        seg_valid = 0;
        sprintf(text_predict, "-");
        return;
    }

    // An aerial pressed on this very frame from a jump or fall: its ECB
    // touched down at once, so the event never saw the aerial itself. It's
    // only an interrupt if keeping on holding wouldn't have landed here too.
    int pressed = -1;
    if (is_landing && !Tracked_IsAerial(prev_ts) && pred_live->land_frame != 1)
        pressed = Aerial_Pressed(fp);

    int kind;
    if (sid == ASID_WAIT)
        kind = LAND_NIL;
    else if (pressed >= 0)
        kind = LAND_AI;
    else if (Tracked_IsAerial(prev_ts) && prev_lock == 1)
        kind = LAND_AI; // aerial pressed during the lock; its ECB came in on this frame
    else if (sid == ASID_LANDING)
        kind = LAND_NORMAL;
    else
        kind = (u8)fp->input.timer_trigger_any_ignore_hitlag < common_lcancel_window ? LAND_LCANCEL : LAND_AERIAL;

    Segment_AddActual(fp);
    live_visible = 0;
    ghost_visible = seg_valid;

    if (Options_Main[OPT_SOUND].val && (kind == LAND_NIL || kind == LAND_AI))
        SFX_PlayRaw(303, 255, 128, 20, 3); // laserland's success sound

    char buf[200];
    int k = event_vars->game_timer - seg_start_timer;

    if (pressed >= 0 && seg_valid)
    {
        // judged against the ticks the prediction drew
        int in_range = k >= 1 && k <= pred_seg->num;
        int predicted = in_range && (pred_seg->ai_mask[k] & AERIAL_BIT(pressed));
        int learning = !predicted && (!in_range || k >= pred_seg->uncertain_from ||
                                      (pred_seg->ai_unlearned & AERIAL_BIT(pressed)));
        if (learning)
            sprintf(text_last, "AI (learning)");
        else
        {
            stat_total++;
            if (predicted)
                stat_exact++;
            sprintf(text_last, predicted ? "AI, as predicted" : "AI, not predicted");
            Text_Exact();
        }

        sprintf(buf, "LandingLab landing: AI with %s pressed at %d x %.4f y %.4f, %s (from %d)%s\n",
                tracked_state_names[pressed], event_vars->game_timer, fp->phys.pos.X, fp->phys.pos.Y,
                predicted ? "predicted" : "not predicted", seg_start_timer, learning ? " learning" : "");
        Log(buf);
        return;
    }

    if (!seg_valid || pred_seg->land_frame == 0)
    {
        sprintf(text_last, "%s", land_kind_names[kind]);
        return;
    }

    int predicted = seg_start_timer + pred_seg->land_frame;
    int diff = event_vars->game_timer - predicted;
    int learning = pred_seg->uncertain_from <= pred_seg->land_frame;

    if (learning)
        sprintf(text_last, "%s (learning)", land_kind_names[kind]);
    else
    {
        stat_total++;
        if (Kind_Matches(kind, pred_seg->land_kind) && diff == 0)
        {
            stat_exact++;
            sprintf(text_last, "%s, as predicted", land_kind_names[kind]);
        }
        else if (!Kind_Matches(kind, pred_seg->land_kind))
            sprintf(text_last, "%s, not %s", land_kind_names[kind], land_kind_names[pred_seg->land_kind]);
        else
            sprintf(text_last, "%s, %df %s", land_kind_names[kind], diff > 0 ? diff : -diff, diff > 0 ? "late" : "early");
        Text_Exact();
    }

    sprintf(buf, "LandingLab landing: %s at %d x %.4f, predicted %s at %d x %.4f (from %d)%s\n",
            land_kind_names[kind], event_vars->game_timer, fp->phys.pos.X,
            land_kind_names[pred_seg->land_kind], predicted, pred_seg->pos[pred_seg->land_frame].X,
            seg_start_timer, learning ? " learning" : "");
    Log(buf);
}

///////////////////////
/// Drawing         ///
///////////////////////

static void Draw_Path(Vec2 *pos, float *bottom, int from, int to, GXColor color, u8 size)
{
    int count = to - from + 1;
    if (count < 2)
        return;

    event_vars->GFX_Start(count, (GFX_Params){.shape = GX_LINESTRIP, .size = size});
    for (int i = from; i <= to; i++)
        GFX_AddVtx(pos[i].X, pos[i].Y + bottom[i], 0, color);
}

static void Draw_Ecb(Vec2 pos, float bottom, EcbSample *s, float facing, GXColor color)
{
    if (!s->has_shape)
    {
        // shape unknown: a small cross at the touchdown point
        float y = pos.Y + bottom;
        event_vars->GFX_Start(4, (GFX_Params){.shape = GX_LINES, .size = 24});
        GFX_AddVtx(pos.X - 1.5f, y - 1.5f, 0, color);
        GFX_AddVtx(pos.X + 1.5f, y + 1.5f, 0, color);
        GFX_AddVtx(pos.X - 1.5f, y + 1.5f, 0, color);
        GFX_AddVtx(pos.X + 1.5f, y - 1.5f, 0, color);
        return;
    }

    float right_x = facing > 0 ? s->front : -s->back;
    float left_x = facing > 0 ? s->back : -s->front;

    event_vars->GFX_Start(5, (GFX_Params){.shape = GX_LINESTRIP, .size = 24});
    GFX_AddVtx(pos.X, pos.Y + s->top, 0, color);
    GFX_AddVtx(pos.X + right_x, pos.Y + s->side_y, 0, color);
    GFX_AddVtx(pos.X, pos.Y + bottom, 0, color);
    GFX_AddVtx(pos.X + left_x, pos.Y + s->side_y, 0, color);
    GFX_AddVtx(pos.X, pos.Y + s->top, 0, color);
}

// Ticks across the path on the frames where pressing an aerial forces a
// touchdown: cyan, or orange if every aerial that works there would land
// with aerial lag.
static void Draw_AiMarks(Prediction *p, int last)
{
    int count = 0;
    for (int k = 1; k <= last; k++)
    {
        if (p->ai_mask[k])
            count += 2;
    }
    if (count == 0)
        return;

    event_vars->GFX_Start(count, (GFX_Params){.shape = GX_LINES, .size = 24});
    for (int k = 1; k <= last; k++)
    {
        if (!p->ai_mask[k])
            continue;

        GXColor color = land_kind_colors[LAND_AI];
        if (k >= p->uncertain_from)
            color = color_learning;
        else if (p->ai_lag_mask[k] == p->ai_mask[k])
            color = land_kind_colors[LAND_LCANCEL];

        // at right angles to the path, so ticks stay apart at any speed
        int k1 = k < p->num ? k + 1 : k;
        float dx = p->pos[k1].X - p->pos[k - 1].X;
        float dy = (p->pos[k1].Y + p->bottom[k1]) - (p->pos[k - 1].Y + p->bottom[k - 1]);
        float len = sqrtf(dx * dx + dy * dy);
        float nx = 1, ny = 0;
        if (len > 0.001f)
        {
            nx = -dy / len;
            ny = dx / len;
        }

        float x = p->pos[k].X;
        float y = p->pos[k].Y + p->bottom[k];
        GFX_AddVtx(x - nx * 2.5f, y - ny * 2.5f, 0, color);
        GFX_AddVtx(x + nx * 2.5f, y + ny * 2.5f, 0, color);
    }
}

static void Draw_Prediction(Prediction *p)
{
    int last = p->land_frame ? p->land_frame : p->num;
    int known = p->uncertain_from - 1; // last frame drawn in color
    if (known > last)
        known = last;
    if (known < 0)
        known = 0;

    GXColor color = land_kind_colors[p->land_kind];
    Draw_Path(p->pos, p->bottom, 0, known, color, 24);
    Draw_Path(p->pos, p->bottom, known, last, color_learning, 24);
    Draw_AiMarks(p, p->land_frame ? p->land_frame - 1 : p->num);

    if (p->land_frame)
    {
        int k = p->land_frame;
        Draw_Ecb(p->pos[k], p->bottom[k], &p->land_ecb, p->facing,
                 p->uncertain_from <= k ? color_learning : color);
    }

    // short white tick where a pending fastfall kicks in
    if (p->fastfall_frame && p->fastfall_frame <= last)
    {
        int k = p->fastfall_frame;
        float y = p->pos[k].Y + p->bottom[k];
        event_vars->GFX_Start(2, (GFX_Params){.shape = GX_LINES, .size = 24});
        GFX_AddVtx(p->pos[k].X - 2.f, y, 0, color_actual);
        GFX_AddVtx(p->pos[k].X + 2.f, y, 0, color_actual);
    }
}

static void World_GX(GOBJ *gobj, int pass)
{
    if (pass != 2 || !Options_Main[OPT_PATH].val)
        return;

    if (live_visible)
        Draw_Prediction(pred_live);
    else if (ghost_visible)
    {
        Draw_Prediction(pred_seg);
        Draw_Path(actual_pos, actual_bottom, 0, actual_num - 1, color_actual, 12);
    }
}

static void Hud_GX(GOBJ *gobj, int pass)
{
    if (pass != 2 || !Options_Main[OPT_PANEL].val)
        return;

    event_vars->HUD_DrawInfoPanel((const char **)panel_labels, (const char **)panel_info, countof(panel_labels));
}

///////////////////////
/// Event callbacks ///
///////////////////////

void Event_Init(GOBJ *gobj)
{
    common_fastfall_stick = Common_Float(COMMON_FASTFALL_STICK);
    common_fastfall_window = Common_Int(COMMON_FASTFALL_WINDOW);
    common_lcancel_window = Common_Int(COMMON_LCANCEL_WINDOW);
    common_lcancel_div = Common_Float(COMMON_LCANCEL_DIV);
    common_platform_drop = Common_Float(COMMON_PLATFORM_DROP);
    common_aerial_stick_x = Common_Float(COMMON_AERIAL_STICK_X);
    common_aerial_stick_y = Common_Float(COMMON_AERIAL_STICK_Y);
    common_aerial_angle = Common_Float(COMMON_AERIAL_ANGLE);

    ecb_table = calloc(sizeof(EcbSample) * TS_COUNT * LL_STATE_FRAMES);
    Learned_Clear();
    pred_live = calloc(sizeof(Prediction));
    pred_seg = calloc(sizeof(Prediction));
    actual_pos = calloc(sizeof(Vec2) * (LL_SIM_FRAMES + 1));
    actual_bottom = calloc(sizeof(float) * (LL_SIM_FRAMES + 1));
    floor_cache = calloc(sizeof(FloorLine) * LL_MAX_FLOORS);

    // HUD panel on the event gobj, paths on their own gobj in world space
    GObj_AddGXLink(gobj, Hud_GX, GXLINK_HUD, 80);
    GOBJ *draw_gobj = GObj_Create(0, 0, 0);
    GObj_AddGXLink(draw_gobj, World_GX, 5, 0);
}

void Event_Think(GOBJ *event)
{
    GOBJ *ft = Fighter_GetGObj(0);
    FighterData *fp = ft->userdata;

    if (!attributes_logged)
    {
        Log_Attributes(fp);
        attributes_logged = 1;
    }

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

    int tracked_air = ts >= 0 && airborne && !disturbed;

    if (prev_tracked_air && !tracked_air)
        Landing_Resolve(fp);

    if (tracked_air)
    {
        Ecb_Record(fp, ts, frame_in_state);
        if (Options_Main[OPT_LOG].val)
            Log_Frame(fp, ts, frame_in_state);

        Floor_BuildCache();
        SimStart start;
        Sim_FromFighter(fp, ts, frame_in_state, &start);
        Predict(fp, &start, pred_live, 1);
        Text_Prediction(pred_live);
        Text_Ai(pred_live);

        if (prev_tracked_air && ts != prev_ts)
            Ai_CheckMissed(fp, ts);
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
        // in the air but in a state we don't predict (airdodge, up-B, hitstun)
        live_visible = 0;
        seg_valid = 0;
        sprintf(text_ai, "-");
    }

    prev_state_id = sid;
    prev_ts = ts;
    prev_tracked_air = tracked_air;
    prev_tilt_timer = (u8)fp->input.timer_lstick_tilt_y;
    prev_lock = fp->coll_data.u.ecb_bot_lock_frames;
}

void Event_Update(void)
{
    if (Pause_CheckStatus(1) != 2)
        HSD_SetSpeedEasy(speed_values[Options_Main[OPT_SPEED].val]);
    else
        HSD_SetSpeedEasy(1.0);
}

void Event_ChangeCollDisplay(GOBJ *menu, int value)
{
    stc_matchcam->show_coll = value;
}

void Event_ClearLearned(GOBJ *menu)
{
    Learned_Clear();
    stat_total = 0;
    stat_exact = 0;
    sprintf(text_exact, "-");
    SFX_PlayCommon(1);
}

void Event_Exit(GOBJ *menu)
{
    stc_match->state = 3;
    Match_EndVS();
}
