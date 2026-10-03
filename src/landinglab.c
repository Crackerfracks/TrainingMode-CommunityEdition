/* landinglab.c
 *
 * Landing Lab (Captain Falcon).
 *
 * While Falcon is airborne, this draws where he will touch down if the stick
 * stays where it is and no buttons are pressed. Only NILs and aerial
 * interrupts get color; other landings are a thin gray line. Thick stretches
 * of the path mark the frames where pressing an aerial forces a touchdown
 * (an aerial interrupt), with arrows for the aerials that do it, and a ring
 * counts down to the next one. On the ground it previews a full hop and a
 * short hop pressed right now.
 *
 * An aerial interrupt: pressing an aerial swaps the ECB to the aerial's pose
 * on that same frame. If the new ECB bottom ends up at or below a floor, you
 * land right there. While the post-jump bottom lock is on, the swap waits
 * until the lock ends and uses whatever aerial frame you're on by then.
 *
 * Whether a landing is a NIL or an aerial interrupt depends on the exact ECB
 * on each frame of each animation, which the game only computes on the frame
 * itself. Falcon's jumps and falls are built in (baked from a logged
 * session); everything else is learned while you play, keyed by (state,
 * frame in state). Parts of a prediction that rely on frames it hasn't seen
 * yet are drawn gray.
 *
 * The physics and collision are ports of the game's own code, checked
 * against the decomp: ft_80084DB0 (air physics), ftCommon_CalcSelfAccel_
 * DriftFrom (drift), mpColl_LoadECB (ECB bottom lock), mpColl_80046904's
 * wall pass (wall pushout), mpCheckFloor and mpLineIntersection(H) (floor
 * crossing), ft_80082B1C (NIL), ftCo_AttackAir_EnterFromMsid (aerial start),
 * ftCo_LandingAir_EnterWithLag (aerial lag, L-cancel), and ftCo_KneeBend /
 * ftCo_Jump_Enter (jumps from the ground). Ceilings and ledge grabs aren't
 * simulated.
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
#define COMMON_RUN_FRICTION 0x6C    // float: ground friction multiplier above walk speed
#define COMMON_JUMP_BACK_STICK 0x78 // float: stick x * facing <= -this jumps backwards

// Runtime collision line flags (decomp mp/forward.h)
#define LINEFLAG_FLOOR (1u << 0)
#define LINEFLAG_CEIL (1u << 1)
#define LINEFLAG_RWALL (1u << 2) // faces right: the right side of a solid
#define LINEFLAG_LWALL (1u << 3)
#define LINEFLAG_KIND 0xFu
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

// Only NIL and aerial interrupts are worth practicing toward.
static int Kind_Highlighted(int kind)
{
    return kind == LAND_NIL || kind == LAND_AI;
}
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

// Falcon's ECB on every frame of his jumps, double jumps and falls, from a
// play session on a vanilla ISO (logged by v0.1, 2026-10-03). Bottoms are
// missing on the frames the post-jump lock hid them. Fall's pose blends
// from whatever came before, so its row is the typical frame, not an exact
// one; play replaces it with what it sees. Aerials still have to be learned.
#define BAKED_BOTTOM 1
#define BAKED_SHAPE 2

typedef struct BakedEcb
{
    s8 ts;
    u8 frame;
    u8 flags;
    float bottom, top, side, front, back;
} BakedEcb;

static const BakedEcb baked_ecb[] = {
    {TS_JUMPF, 0, BAKED_SHAPE, 0.f, 17.462f, 10.696f, 4.895f, -4.895f},
    {TS_JUMPF, 1, BAKED_SHAPE, 0.f, 17.607f, 10.721f, 4.966f, -4.966f},
    {TS_JUMPF, 2, BAKED_SHAPE, 0.f, 17.729f, 10.736f, 4.489f, -5.587f},
    {TS_JUMPF, 3, BAKED_SHAPE, 0.f, 17.827f, 10.743f, 4.443f, -5.780f},
    {TS_JUMPF, 4, BAKED_SHAPE, 0.f, 17.904f, 10.742f, 4.403f, -5.965f},
    {TS_JUMPF, 5, BAKED_SHAPE, 0.f, 17.962f, 10.734f, 4.370f, -6.139f},
    {TS_JUMPF, 6, BAKED_SHAPE, 0.f, 18.004f, 10.720f, 4.345f, -6.299f},
    {TS_JUMPF, 7, BAKED_SHAPE, 0.f, 18.032f, 10.701f, 4.327f, -6.441f},
    {TS_JUMPF, 8, BAKED_SHAPE, 0.f, 18.048f, 10.679f, 4.316f, -6.562f},
    {TS_JUMPF, 9, BAKED_BOTTOM|BAKED_SHAPE, 3.252f, 18.058f, 10.655f, 4.313f, -6.656f},
    {TS_JUMPF, 10, BAKED_BOTTOM|BAKED_SHAPE, 3.197f, 18.062f, 10.630f, 4.316f, -6.717f},
    {TS_JUMPF, 11, BAKED_BOTTOM|BAKED_SHAPE, 3.144f, 18.067f, 10.605f, 4.325f, -6.738f},
    {TS_JUMPF, 12, BAKED_BOTTOM|BAKED_SHAPE, 3.092f, 18.074f, 10.583f, 4.339f, -6.719f},
    {TS_JUMPF, 13, BAKED_BOTTOM|BAKED_SHAPE, 3.041f, 17.926f, 10.484f, 4.358f, -6.666f},
    {TS_JUMPF, 14, BAKED_BOTTOM|BAKED_SHAPE, 2.990f, 17.436f, 10.213f, 4.381f, -6.583f},
    {TS_JUMPF, 15, BAKED_BOTTOM|BAKED_SHAPE, 2.937f, 16.824f, 9.881f, 4.406f, -6.471f},
    {TS_JUMPF, 16, BAKED_BOTTOM|BAKED_SHAPE, 2.729f, 16.159f, 9.444f, 4.432f, -6.332f},
    {TS_JUMPF, 17, BAKED_BOTTOM|BAKED_SHAPE, 2.562f, 15.444f, 9.003f, 4.459f, -6.167f},
    {TS_JUMPF, 18, BAKED_BOTTOM|BAKED_SHAPE, 2.424f, 14.885f, 8.654f, 4.486f, -5.980f},
    {TS_JUMPF, 19, BAKED_BOTTOM|BAKED_SHAPE, 2.305f, 14.392f, 8.349f, 4.517f, -5.790f},
    {TS_JUMPF, 20, BAKED_BOTTOM|BAKED_SHAPE, 2.204f, 14.120f, 8.162f, 4.941f, -4.941f},
    {TS_JUMPF, 21, BAKED_BOTTOM|BAKED_SHAPE, 2.123f, 14.097f, 8.110f, 4.417f, -4.417f},
    {TS_JUMPF, 22, BAKED_BOTTOM|BAKED_SHAPE, 2.079f, 14.047f, 8.063f, 3.603f, -3.603f},
    {TS_JUMPF, 23, BAKED_BOTTOM|BAKED_SHAPE, 2.093f, 13.944f, 8.019f, 2.684f, -2.684f},
    {TS_JUMPF, 24, BAKED_BOTTOM|BAKED_SHAPE, 2.237f, 13.773f, 8.005f, 2.607f, -2.607f},
    {TS_JUMPF, 25, BAKED_BOTTOM|BAKED_SHAPE, 2.547f, 13.531f, 8.039f, 2.580f, -2.580f},
    {TS_JUMPF, 26, BAKED_BOTTOM|BAKED_SHAPE, 2.995f, 13.229f, 8.112f, 3.672f, -3.672f},
    {TS_JUMPF, 27, BAKED_BOTTOM|BAKED_SHAPE, 3.496f, 12.897f, 8.197f, 4.504f, -4.504f},
    {TS_JUMPF, 28, BAKED_BOTTOM|BAKED_SHAPE, 3.857f, 12.540f, 8.198f, 4.856f, -4.856f},
    {TS_JUMPF, 29, BAKED_BOTTOM|BAKED_SHAPE, 4.177f, 12.179f, 8.178f, 5.241f, -5.233f},
    {TS_JUMPF, 30, BAKED_BOTTOM|BAKED_SHAPE, 3.865f, 11.847f, 7.856f, 5.603f, -4.951f},
    {TS_JUMPF, 31, BAKED_BOTTOM|BAKED_SHAPE, 3.344f, 11.550f, 7.447f, 5.804f, -4.347f},
    {TS_JUMPF, 32, BAKED_BOTTOM|BAKED_SHAPE, 2.875f, 11.294f, 7.085f, 4.736f, -4.736f},
    {TS_JUMPF, 33, BAKED_BOTTOM|BAKED_SHAPE, 2.486f, 11.081f, 6.783f, 4.377f, -4.377f},
    {TS_JUMPF, 34, BAKED_BOTTOM|BAKED_SHAPE, 2.192f, 10.909f, 6.550f, 4.096f, -4.096f},
    {TS_JUMPB, 0, BAKED_SHAPE, 0.f, 17.113f, 10.487f, 2.135f, -2.135f},
    {TS_JUMPB, 1, BAKED_SHAPE, 0.f, 15.294f, 9.481f, 3.574f, -3.574f},
    {TS_JUMPB, 2, BAKED_SHAPE, 0.f, 12.368f, 7.839f, 4.041f, -4.041f},
    {TS_JUMPB, 3, BAKED_SHAPE, 0.f, 11.779f, 7.344f, 4.213f, -4.213f},
    {TS_JUMPB, 4, BAKED_SHAPE, 0.f, 12.424f, 7.465f, 4.219f, -4.219f},
    {TS_JUMPB, 5, BAKED_SHAPE, 0.f, 12.638f, 7.378f, 4.240f, -4.240f},
    {TS_JUMPB, 6, BAKED_SHAPE, 0.f, 12.333f, 7.035f, 4.349f, -4.349f},
    {TS_JUMPB, 7, BAKED_SHAPE, 0.f, 11.895f, 6.652f, 4.528f, -4.528f},
    {TS_JUMPB, 8, BAKED_SHAPE, 0.f, 11.288f, 6.227f, 4.862f, -4.862f},
    {TS_JUMPB, 9, BAKED_BOTTOM|BAKED_SHAPE, 1.066f, 10.606f, 5.836f, 2.000f, -8.901f},
    {TS_JUMPB, 10, BAKED_BOTTOM|BAKED_SHAPE, 1.179f, 9.753f, 5.466f, 2.268f, -9.168f},
    {TS_JUMPB, 11, BAKED_BOTTOM|BAKED_SHAPE, 1.565f, 9.423f, 5.494f, 2.974f, -9.467f},
    {TS_JUMPB, 12, BAKED_BOTTOM|BAKED_SHAPE, 2.242f, 9.466f, 5.854f, 3.628f, -9.751f},
    {TS_JUMPB, 13, BAKED_BOTTOM|BAKED_SHAPE, 3.186f, 9.925f, 6.555f, 4.174f, -9.837f},
    {TS_JUMPB, 14, BAKED_BOTTOM|BAKED_SHAPE, 4.324f, 10.473f, 7.398f, 4.556f, -9.859f},
    {TS_JUMPB, 15, BAKED_BOTTOM|BAKED_SHAPE, 5.579f, 10.777f, 8.178f, 4.735f, -9.723f},
    {TS_JUMPB, 16, BAKED_BOTTOM|BAKED_SHAPE, 6.227f, 10.688f, 8.458f, 4.690f, -9.788f},
    {TS_JUMPB, 17, BAKED_BOTTOM|BAKED_SHAPE, 6.399f, 10.294f, 8.347f, 4.423f, -9.944f},
    {TS_JUMPB, 18, BAKED_BOTTOM|BAKED_SHAPE, 6.592f, 10.472f, 8.532f, 4.098f, -10.222f},
    {TS_JUMPB, 19, BAKED_BOTTOM|BAKED_SHAPE, 6.800f, 10.585f, 8.693f, 3.737f, -10.291f},
    {TS_JUMPB, 20, BAKED_BOTTOM|BAKED_SHAPE, 7.017f, 10.766f, 8.892f, 3.385f, -10.460f},
    {TS_JUMPB, 21, BAKED_BOTTOM|BAKED_SHAPE, 7.237f, 11.430f, 9.333f, 2.982f, -10.573f},
    {TS_JUMPB, 22, BAKED_BOTTOM|BAKED_SHAPE, 7.452f, 12.158f, 9.805f, 2.322f, -10.600f},
    {TS_JUMPB, 23, BAKED_BOTTOM|BAKED_SHAPE, 6.298f, 12.972f, 9.635f, 2.000f, -10.402f},
    {TS_JUMPB, 24, BAKED_BOTTOM|BAKED_SHAPE, 4.790f, 13.642f, 9.216f, 4.885f, -4.885f},
    {TS_JUMPB, 25, BAKED_BOTTOM|BAKED_SHAPE, 3.568f, 13.837f, 8.703f, 4.386f, -4.386f},
    {TS_JUMPB, 26, BAKED_BOTTOM|BAKED_SHAPE, 2.740f, 13.394f, 8.067f, 3.932f, -3.932f},
    {TS_JUMPB, 27, BAKED_BOTTOM|BAKED_SHAPE, 2.361f, 12.417f, 7.389f, 3.402f, -3.402f},
    {TS_JUMPB, 28, BAKED_BOTTOM|BAKED_SHAPE, 2.485f, 11.089f, 6.787f, 4.107f, -4.107f},
    {TS_JUMPB, 29, BAKED_BOTTOM|BAKED_SHAPE, 2.345f, 9.537f, 5.941f, 4.751f, -5.578f},
    {TS_JUMPB, 30, BAKED_BOTTOM|BAKED_SHAPE, 2.891f, 8.597f, 5.744f, 6.601f, -5.569f},
    {TS_JUMPB, 31, BAKED_BOTTOM|BAKED_SHAPE, 3.172f, 8.503f, 5.837f, 8.079f, -5.177f},
    {TS_JUMPB, 32, BAKED_BOTTOM|BAKED_SHAPE, 3.826f, 8.397f, 6.112f, 8.909f, -4.611f},
    {TS_JUMPB, 33, BAKED_BOTTOM|BAKED_SHAPE, 4.404f, 8.283f, 6.344f, 9.148f, -3.993f},
    {TS_JUMPB, 34, BAKED_BOTTOM|BAKED_SHAPE, 3.737f, 9.209f, 6.473f, 8.923f, -3.380f},
    {TS_JUMPAERIALF, 0, BAKED_SHAPE, 0.f, 10.776f, 6.387f, 3.934f, -3.934f},
    {TS_JUMPAERIALF, 1, BAKED_SHAPE, 0.f, 10.565f, 6.415f, 4.279f, -4.279f},
    {TS_JUMPAERIALF, 2, BAKED_SHAPE, 0.f, 11.928f, 7.449f, 4.235f, -4.235f},
    {TS_JUMPAERIALF, 3, BAKED_SHAPE, 0.f, 13.252f, 8.545f, 3.645f, -3.645f},
    {TS_JUMPAERIALF, 4, BAKED_SHAPE, 0.f, 12.207f, 7.869f, 3.018f, -3.018f},
    {TS_JUMPAERIALF, 5, BAKED_SHAPE, 0.f, 12.181f, 7.828f, 3.445f, -3.445f},
    {TS_JUMPAERIALF, 6, BAKED_SHAPE, 0.f, 12.882f, 8.729f, 3.790f, -3.790f},
    {TS_JUMPAERIALF, 7, BAKED_SHAPE, 0.f, 11.061f, 8.000f, 4.284f, -4.284f},
    {TS_JUMPAERIALF, 8, BAKED_SHAPE, 0.f, 9.328f, 6.882f, 3.855f, -3.855f},
    {TS_JUMPAERIALF, 9, BAKED_BOTTOM|BAKED_SHAPE, 4.077f, 9.080f, 6.578f, 2.657f, -2.657f},
    {TS_JUMPAERIALF, 10, BAKED_BOTTOM|BAKED_SHAPE, 5.167f, 11.493f, 8.330f, 2.633f, -2.633f},
    {TS_JUMPAERIALF, 11, BAKED_BOTTOM|BAKED_SHAPE, 7.308f, 12.886f, 10.097f, 2.338f, -2.338f},
    {TS_JUMPAERIALF, 12, BAKED_BOTTOM|BAKED_SHAPE, 6.693f, 11.958f, 9.325f, 2.970f, -2.970f},
    {TS_JUMPAERIALF, 13, BAKED_BOTTOM|BAKED_SHAPE, 6.756f, 11.118f, 8.937f, 2.667f, -2.667f},
    {TS_JUMPAERIALF, 14, BAKED_BOTTOM|BAKED_SHAPE, 4.647f, 9.821f, 7.234f, 2.658f, -2.658f},
    {TS_JUMPAERIALF, 15, BAKED_BOTTOM|BAKED_SHAPE, 3.497f, 8.279f, 5.888f, 2.156f, -2.156f},
    {TS_JUMPAERIALF, 16, BAKED_BOTTOM|BAKED_SHAPE, 3.551f, 9.011f, 6.281f, 2.428f, -2.428f},
    {TS_JUMPAERIALF, 17, BAKED_BOTTOM|BAKED_SHAPE, 4.893f, 9.333f, 7.113f, 2.488f, -2.488f},
    {TS_JUMPAERIALF, 18, BAKED_BOTTOM|BAKED_SHAPE, 5.154f, 9.175f, 7.164f, 2.574f, -2.574f},
    {TS_JUMPAERIALF, 19, BAKED_BOTTOM|BAKED_SHAPE, 6.128f, 11.239f, 8.683f, 2.668f, -2.668f},
    {TS_JUMPAERIALF, 20, BAKED_BOTTOM|BAKED_SHAPE, 7.625f, 12.345f, 9.985f, 2.161f, -2.161f},
    {TS_JUMPAERIALF, 21, BAKED_BOTTOM|BAKED_SHAPE, 7.253f, 12.521f, 9.887f, 2.091f, -2.091f},
    {TS_JUMPAERIALF, 22, BAKED_BOTTOM|BAKED_SHAPE, 6.794f, 12.117f, 9.455f, 2.563f, -2.563f},
    {TS_JUMPAERIALF, 23, BAKED_BOTTOM|BAKED_SHAPE, 6.634f, 10.933f, 8.784f, 2.404f, -2.404f},
    {TS_JUMPAERIALF, 24, BAKED_BOTTOM|BAKED_SHAPE, 6.779f, 10.838f, 8.809f, 2.544f, -2.544f},
    {TS_JUMPAERIALF, 25, BAKED_BOTTOM|BAKED_SHAPE, 5.209f, 10.161f, 7.685f, 2.724f, -2.724f},
    {TS_JUMPAERIALF, 26, BAKED_BOTTOM|BAKED_SHAPE, 4.004f, 9.091f, 6.548f, 2.396f, -2.396f},
    {TS_JUMPAERIALF, 27, BAKED_BOTTOM|BAKED_SHAPE, 3.540f, 8.262f, 5.901f, 2.153f, -2.153f},
    {TS_JUMPAERIALF, 28, BAKED_BOTTOM|BAKED_SHAPE, 3.445f, 8.763f, 6.104f, 2.134f, -2.134f},
    {TS_JUMPAERIALF, 29, BAKED_BOTTOM|BAKED_SHAPE, 3.729f, 9.128f, 6.428f, 2.519f, -2.519f},
    {TS_JUMPAERIALF, 30, BAKED_BOTTOM|BAKED_SHAPE, 4.604f, 9.316f, 6.960f, 2.530f, -2.530f},
    {TS_JUMPAERIALF, 31, BAKED_BOTTOM|BAKED_SHAPE, 4.996f, 9.317f, 7.156f, 2.347f, -2.347f},
    {TS_JUMPAERIALF, 32, BAKED_BOTTOM|BAKED_SHAPE, 5.200f, 9.251f, 7.226f, 2.610f, -2.610f},
    {TS_JUMPAERIALF, 33, BAKED_BOTTOM|BAKED_SHAPE, 5.736f, 10.638f, 8.187f, 2.728f, -2.728f},
    {TS_JUMPAERIALF, 34, BAKED_BOTTOM|BAKED_SHAPE, 6.450f, 10.749f, 8.600f, 2.540f, -2.540f},
    {TS_JUMPAERIALF, 35, BAKED_BOTTOM|BAKED_SHAPE, 7.189f, 11.638f, 9.414f, 2.579f, -2.579f},
    {TS_JUMPAERIALF, 36, BAKED_BOTTOM|BAKED_SHAPE, 7.561f, 12.188f, 9.875f, 2.244f, -2.244f},
    {TS_JUMPAERIALF, 37, BAKED_BOTTOM|BAKED_SHAPE, 6.852f, 12.601f, 9.727f, 2.180f, -2.180f},
    {TS_JUMPAERIALF, 38, BAKED_BOTTOM|BAKED_SHAPE, 5.746f, 12.944f, 9.345f, 3.256f, -3.256f},
    {TS_JUMPAERIALF, 39, BAKED_BOTTOM|BAKED_SHAPE, 5.852f, 12.163f, 9.007f, 4.499f, -4.499f},
    {TS_JUMPAERIALF, 40, BAKED_BOTTOM|BAKED_SHAPE, 6.763f, 11.220f, 8.992f, 4.543f, -6.718f},
    {TS_JUMPAERIALF, 41, BAKED_BOTTOM|BAKED_SHAPE, 5.853f, 11.429f, 8.641f, 4.642f, -6.758f},
    {TS_JUMPAERIALF, 42, BAKED_BOTTOM|BAKED_SHAPE, 4.777f, 11.689f, 8.233f, 4.967f, -4.967f},
    {TS_JUMPAERIALF, 43, BAKED_BOTTOM|BAKED_SHAPE, 3.966f, 11.819f, 7.892f, 3.951f, -3.951f},
    {TS_JUMPAERIALF, 44, BAKED_BOTTOM|BAKED_SHAPE, 3.410f, 11.764f, 7.587f, 3.394f, -3.394f},
    {TS_JUMPAERIALF, 45, BAKED_BOTTOM|BAKED_SHAPE, 3.120f, 11.544f, 7.332f, 3.421f, -3.421f},
    {TS_JUMPAERIALF, 46, BAKED_BOTTOM|BAKED_SHAPE, 3.086f, 11.266f, 7.176f, 3.787f, -3.787f},
    {TS_JUMPAERIALF, 47, BAKED_BOTTOM|BAKED_SHAPE, 3.139f, 11.071f, 7.105f, 4.478f, -4.478f},
    {TS_JUMPAERIALB, 0, BAKED_SHAPE, 0.f, 10.871f, 6.435f, 3.935f, -3.935f},
    {TS_JUMPAERIALB, 1, BAKED_SHAPE, 0.f, 12.305f, 7.572f, 4.733f, -4.733f},
    {TS_JUMPAERIALB, 2, BAKED_SHAPE, 0.f, 13.849f, 8.811f, 4.008f, -4.008f},
    {TS_JUMPAERIALB, 3, BAKED_SHAPE, 0.f, 13.610f, 8.870f, 4.107f, -4.107f},
    {TS_JUMPAERIALB, 4, BAKED_SHAPE, 0.f, 11.406f, 7.570f, 5.621f, -6.795f},
    {TS_JUMPAERIALB, 5, BAKED_SHAPE, 0.f, 7.941f, 5.386f, 4.516f, -6.869f},
    {TS_JUMPAERIALB, 6, BAKED_SHAPE, 0.f, 8.280f, 5.919f, 5.523f, -5.689f},
    {TS_JUMPAERIALB, 7, BAKED_SHAPE, 0.f, 8.582f, 6.430f, 6.362f, -5.894f},
    {TS_JUMPAERIALB, 8, BAKED_SHAPE, 0.f, 8.912f, 6.963f, 6.936f, -5.759f},
    {TS_JUMPAERIALB, 9, BAKED_BOTTOM|BAKED_SHAPE, 4.740f, 9.444f, 7.092f, 7.255f, -4.216f},
    {TS_JUMPAERIALB, 10, BAKED_BOTTOM|BAKED_SHAPE, 4.910f, 10.736f, 7.823f, 7.037f, -3.436f},
    {TS_JUMPAERIALB, 11, BAKED_BOTTOM|BAKED_SHAPE, 5.037f, 12.766f, 8.902f, 4.688f, -4.688f},
    {TS_JUMPAERIALB, 12, BAKED_BOTTOM|BAKED_SHAPE, 5.103f, 14.404f, 9.753f, 4.214f, -4.214f},
    {TS_JUMPAERIALB, 13, BAKED_BOTTOM|BAKED_SHAPE, 5.187f, 16.212f, 10.700f, 3.987f, -3.987f},
    {TS_JUMPAERIALB, 14, BAKED_BOTTOM|BAKED_SHAPE, 5.188f, 16.828f, 11.008f, 3.570f, -3.570f},
    {TS_JUMPAERIALB, 15, BAKED_BOTTOM|BAKED_SHAPE, 5.052f, 15.274f, 10.163f, 4.086f, -4.086f},
    {TS_JUMPAERIALB, 16, BAKED_BOTTOM|BAKED_SHAPE, 5.007f, 15.753f, 10.380f, 4.092f, -4.092f},
    {TS_JUMPAERIALB, 17, BAKED_BOTTOM|BAKED_SHAPE, 5.090f, 16.262f, 10.676f, 4.577f, -4.577f},
    {TS_JUMPAERIALB, 18, BAKED_BOTTOM|BAKED_SHAPE, 5.323f, 16.481f, 10.902f, 4.843f, -4.843f},
    {TS_JUMPAERIALB, 19, BAKED_BOTTOM|BAKED_SHAPE, 5.606f, 16.238f, 10.922f, 4.921f, -4.921f},
    {TS_JUMPAERIALB, 20, BAKED_BOTTOM|BAKED_SHAPE, 6.077f, 15.570f, 10.823f, 4.841f, -4.841f},
    {TS_JUMPAERIALB, 21, BAKED_BOTTOM|BAKED_SHAPE, 6.824f, 14.466f, 10.645f, 4.644f, -4.644f},
    {TS_JUMPAERIALB, 22, BAKED_BOTTOM|BAKED_SHAPE, 7.335f, 12.611f, 9.973f, 4.828f, -4.828f},
    {TS_JUMPAERIALB, 23, BAKED_BOTTOM|BAKED_SHAPE, 6.981f, 11.857f, 9.419f, 4.791f, -4.791f},
    {TS_JUMPAERIALB, 24, BAKED_BOTTOM|BAKED_SHAPE, 6.273f, 11.638f, 8.955f, 4.833f, -4.833f},
    {TS_JUMPAERIALB, 25, BAKED_BOTTOM|BAKED_SHAPE, 5.379f, 11.594f, 8.486f, 4.796f, -4.796f},
    {TS_JUMPAERIALB, 26, BAKED_BOTTOM|BAKED_SHAPE, 4.967f, 11.094f, 8.030f, 4.525f, -4.525f},
    {TS_JUMPAERIALB, 27, BAKED_BOTTOM|BAKED_SHAPE, 4.857f, 9.688f, 7.272f, 3.205f, -3.205f},
    {TS_JUMPAERIALB, 28, BAKED_BOTTOM|BAKED_SHAPE, 4.685f, 10.132f, 7.408f, 2.996f, -2.996f},
    {TS_JUMPAERIALB, 29, BAKED_BOTTOM|BAKED_SHAPE, 4.807f, 10.822f, 7.815f, 2.846f, -2.846f},
    {TS_JUMPAERIALB, 30, BAKED_BOTTOM|BAKED_SHAPE, 4.590f, 11.217f, 7.904f, 2.770f, -2.770f},
    {TS_JUMPAERIALB, 31, BAKED_BOTTOM|BAKED_SHAPE, 4.093f, 11.309f, 7.701f, 3.772f, -3.772f},
    {TS_FALL, 0, BAKED_BOTTOM|BAKED_SHAPE, 1.998f, 10.779f, 6.389f, 3.933f, -3.933f},
    {TS_FALL, 1, BAKED_BOTTOM|BAKED_SHAPE, 2.078f, 10.764f, 6.421f, 4.064f, -4.064f},
    {TS_FALL, 2, BAKED_BOTTOM|BAKED_SHAPE, 2.405f, 11.024f, 6.715f, 4.328f, -4.328f},
    {TS_FALL, 3, BAKED_BOTTOM|BAKED_SHAPE, 2.764f, 11.400f, 7.082f, 4.520f, -4.520f},
    {TS_FALL, 4, BAKED_BOTTOM|BAKED_SHAPE, 2.803f, 11.438f, 7.120f, 4.584f, -4.584f},
    {TS_FALL, 5, BAKED_BOTTOM|BAKED_SHAPE, 2.663f, 11.458f, 7.056f, 4.695f, -4.695f},
    {TS_FALL, 6, BAKED_BOTTOM|BAKED_SHAPE, 2.198f, 10.821f, 6.393f, 4.586f, -4.586f},
    {TS_FALL, 7, BAKED_BOTTOM|BAKED_SHAPE, 1.748f, 10.560f, 6.046f, 4.382f, -4.382f},
    {TS_FALL, 8, BAKED_BOTTOM|BAKED_SHAPE, 1.732f, 10.591f, 6.082f, 4.198f, -4.198f},
    {TS_FALL, 9, BAKED_BOTTOM|BAKED_SHAPE, 1.827f, 10.759f, 6.345f, 4.139f, -4.139f},
    {TS_FALL, 10, BAKED_BOTTOM|BAKED_SHAPE, 2.182f, 11.086f, 6.683f, 4.256f, -4.256f},
    {TS_FALL, 11, BAKED_BOTTOM|BAKED_SHAPE, 2.434f, 11.268f, 6.840f, 4.346f, -4.346f},
    {TS_FALL, 12, BAKED_BOTTOM|BAKED_SHAPE, 2.373f, 11.213f, 6.793f, 4.433f, -4.433f},
    {TS_FALL, 13, BAKED_BOTTOM|BAKED_SHAPE, 2.105f, 10.985f, 6.545f, 4.543f, -4.543f},
    {TS_FALL, 14, BAKED_BOTTOM|BAKED_SHAPE, 1.788f, 10.726f, 6.257f, 4.592f, -4.592f},
    {TS_FALL, 15, BAKED_BOTTOM|BAKED_SHAPE, 1.587f, 10.589f, 6.088f, 4.434f, -4.434f},
    {TS_FALL, 16, BAKED_BOTTOM|BAKED_SHAPE, 1.613f, 10.619f, 6.116f, 4.159f, -4.159f},
    {TS_FALL, 17, BAKED_BOTTOM|BAKED_SHAPE, 1.839f, 10.719f, 6.273f, 4.126f, -4.126f},
    {TS_FALL, 18, BAKED_BOTTOM|BAKED_SHAPE, 2.192f, 11.043f, 6.642f, 4.121f, -4.121f},
    {TS_FALL, 19, BAKED_BOTTOM|BAKED_SHAPE, 2.442f, 11.276f, 6.826f, 4.164f, -4.164f},
    {TS_FALL, 20, BAKED_BOTTOM|BAKED_SHAPE, 2.384f, 11.213f, 6.798f, 4.281f, -4.281f},
    {TS_FALL, 21, BAKED_BOTTOM|BAKED_SHAPE, 2.154f, 11.007f, 6.580f, 4.416f, -4.416f},
    {TS_FALL, 22, BAKED_BOTTOM|BAKED_SHAPE, 1.815f, 10.745f, 6.280f, 4.540f, -4.540f},
    {TS_FALL, 23, BAKED_BOTTOM|BAKED_SHAPE, 2.102f, 10.765f, 6.433f, 4.546f, -4.546f},
    {TS_FALL, 24, BAKED_BOTTOM|BAKED_SHAPE, 2.022f, 10.835f, 6.429f, 4.303f, -4.303f},
    {TS_FALL, 25, BAKED_BOTTOM|BAKED_SHAPE, 2.093f, 10.803f, 6.446f, 4.132f, -4.132f},
    {TS_FALL, 26, BAKED_BOTTOM|BAKED_SHAPE, 2.417f, 11.122f, 6.734f, 4.175f, -4.175f},
    {TS_FALL, 27, BAKED_BOTTOM|BAKED_SHAPE, 2.726f, 11.291f, 7.008f, 4.270f, -4.270f},
    {TS_FALL, 28, BAKED_BOTTOM|BAKED_SHAPE, 2.757f, 11.270f, 7.013f, 4.382f, -4.382f},
    {TS_FALL, 29, BAKED_BOTTOM|BAKED_SHAPE, 2.834f, 11.988f, 7.411f, 4.514f, -4.514f},
    {TS_FALL, 30, BAKED_BOTTOM|BAKED_SHAPE, 2.576f, 11.728f, 7.152f, 4.580f, -4.580f},
    {TS_FALL, 31, BAKED_BOTTOM|BAKED_SHAPE, 2.428f, 11.576f, 7.002f, 4.412f, -4.412f},
    {TS_FALL, 32, BAKED_BOTTOM|BAKED_SHAPE, 2.374f, 11.543f, 6.958f, 4.146f, -4.146f},
    {TS_FALL, 33, BAKED_BOTTOM|BAKED_SHAPE, 2.495f, 11.576f, 7.036f, 4.123f, -4.123f},
    {TS_FALL, 34, BAKED_BOTTOM|BAKED_SHAPE, 2.744f, 11.736f, 7.240f, 4.287f, -4.287f},
    {TS_FALL, 35, BAKED_BOTTOM, 3.319f, 12.555f, 7.937f, 0.f, 0.f},
    {TS_FALL, 36, BAKED_BOTTOM, 3.201f, 12.441f, 7.821f, 0.f, 0.f},
    {TS_FALL, 37, BAKED_BOTTOM, 2.951f, 12.215f, 7.583f, 0.f, 0.f},
    {TS_FALL, 38, BAKED_BOTTOM, 2.673f, 11.887f, 7.280f, 0.f, 0.f},
};

static void Learned_Bake(void)
{
    for (int i = 0; i < (int)countof(baked_ecb); i++)
    {
        const BakedEcb *b = &baked_ecb[i];
        EcbSample *e = Ecb_Get(b->ts, b->frame);
        if (b->flags & BAKED_BOTTOM)
        {
            e->bottom = b->bottom;
            e->has_bottom = 1;
        }
        e->top = b->top;
        e->side_y = b->side;
        if (b->flags & BAKED_SHAPE)
        {
            e->front = b->front;
            e->back = b->back;
            e->has_shape = 1;
        }
        e->seen = 1;
    }

    // JumpF was seen running out into Fall after 35 frames
    state_len[TS_JUMPF] = 35;
    state_next[TS_JUMPF] = TS_FALL;
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

// mpLineIntersection: does the movement b0->b1 cross line a0->a1 from above
// (the left of a0->a1)? If so, where.
static int Line_Intersect(double a0x, double a0y, double a1x, double a1y,
                          double b0x, double b0y, double b1x, double b1y,
                          float *int_x, float *int_y)
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
    if (dabs(area) <= 0.0001f)
        return 0;

    double t = ((bw * d0y) - (bh * d0x)) / area;
    if (t > 0.0)
    {
        if (t < 1.0)
        {
            *int_x = (aw * t) + a0x;
            *int_y = (ah * t) + a0y;
        }
        else
        {
            *int_x = a1x;
            *int_y = a1y;
        }
    }
    else
    {
        *int_x = a0x;
        *int_y = a0y;
    }
    return 1;
}

static int Line_Cross(double a0x, double a0y, double a1x, double a1y,
                      double b0x, double b0y, double b1x, double b1y)
{
    float int_x, int_y;
    return Line_Intersect(a0x, a0y, a1x, a1y, b0x, b0y, b1x, b1y, &int_x, &int_y);
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

static void Wall_BuildCache(RawCollLine *lines, CollVert *verts);

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

    Wall_BuildCache(lines, verts);
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
/// Stage walls     ///
///////////////////////

// mpColl_80046904's wall part, for the simulated ECB. A right wall faces
// right (the right side of a solid) and pushes the fighter right; its line
// runs top to bottom. A left wall runs bottom to top. The stage is assumed
// to stand still, so a vertex's previous position is its current one.

// The ECB used on one frame, relative to the fighter. The top and bottom
// points are at x 0, the side points at height side.
typedef struct SimEcb
{
    float bottom;
    float top;
    float side;
    float left;  // x of the left point
    float right; // x of the right point
} SimEcb;

#define LL_MAX_WALLS 64

typedef struct WallBox
{
    float min_x, min_y, max_x, max_y;
} WallBox;

static s16 *wall_ids[2]; // [0] right walls, [1] left walls
static WallBox *wall_box[2];
static int wall_num[2];
static s16 near_ids[2][LL_MAX_WALLS]; // the walls near the ECB this frame
static int near_num[2];
static RawCollLine *coll_lines;
static CollVert *coll_verts;
static int wall_list[2][8]; // walls touched this frame, like mpColl_80458810
static int wall_list_num[2];

static u32 Line_Kind(int id)
{
    return coll_lines[id].flags & LINEFLAG_KIND;
}

// mpLib_80054ED8
static int Line_Usable(int id)
{
    if (id == -1)
        return 0;
    u32 flags = coll_lines[id].flags;
    return (flags & LINEFLAG_ENABLED) && !(flags & LINEFLAG_HIDDEN);
}

static Vec2 *Line_V0(int id)
{
    return &coll_verts[(u16)coll_lines[id].desc->vert_prev].pos_curr;
}

static Vec2 *Line_V1(int id)
{
    return &coll_verts[(u16)coll_lines[id].desc->vert_next].pos_curr;
}

static int Wall_Prev(int id)
{
    return Line_Prev(coll_lines, coll_verts, id);
}

static int Wall_Next(int id)
{
    return Line_Next(coll_lines, coll_verts, id);
}

// mpLinePrevNonRightWall, mpLineNextNonCeiling, ...: the first line in that
// direction that isn't of this kind.
static int Line_SkipKind(int id, u32 kind, int forward)
{
    int start = id;
    id = forward ? Wall_Next(id) : Wall_Prev(id);
    while (id != -1 && id != start && (coll_lines[id].flags & kind))
        id = forward ? Wall_Next(id) : Wall_Prev(id);
    return id == start ? -1 : id;
}

// mpLinesConnected: is target in the run of same-kind lines through start?
static int Lines_Connected(int start, int target)
{
    if (start == target)
        return 1;
    u32 kind = Line_Kind(start);
    for (int id = Wall_Next(start); id != -1 && Line_Kind(id) == kind; id = Wall_Next(id))
    {
        if (id == target)
            return 1;
        if (id == start)
            break;
    }
    for (int id = Wall_Prev(start); id != -1 && Line_Kind(id) == kind; id = Wall_Prev(id))
    {
        if (id == target)
            return 1;
        if (id == start)
            break;
    }
    return 0;
}

// The top and bottom of the run of walls through id (mpRightWallGetTop,
// mpRightWallGetBottom, mpLeftWallGetTop, mpLeftWallGetBottom).
static Vec2 Wall_End(int id, int side, int top)
{
    u32 kind = Line_Kind(id);
    int towards_v0 = (side == 0) == top; // right walls start at their top
    int last = id;
    for (int next = id;;)
    {
        next = towards_v0 ? Wall_Prev(next) : Wall_Next(next);
        if (next == -1 || next == id || Line_Kind(next) != kind)
            break;
        last = next;
    }
    return towards_v0 ? *Line_V0(last) : *Line_V1(last);
}

// mpLib_8004E684_RightWall / mpLib_8004E398_LeftWall: how far x must move to
// put the point (x, y) on the run of walls through id. Returns -1 if y is
// past the run's ends.
static int Wall_DistanceX(int id, int side, float x, float y, float *dist)
{
    int dir = 0;
    float wy = y;
    float y0, y1;
    u32 flag = side == 0 ? LINEFLAG_RWALL : LINEFLAG_LWALL;

    while (1)
    {
        y0 = Line_V0(id)->Y;
        y1 = Line_V1(id)->Y;
        // right walls: v0 is the top, so above y0 means the previous line
        int above = side == 0 ? y > y0 : y > y1;
        int below = side == 0 ? y < y1 : y < y0;
        if (side == 0 ? above : below)
        {
            // towards v0
            if (dir != (side == 0 ? -1 : 1))
            {
                int prev = Wall_Prev(id);
                if (prev == -1 || !(coll_lines[prev].flags & flag))
                {
                    if (side == 0 ? y - y0 > 0.1 : y - y0 < -0.1)
                        return -1;
                    wy = y0;
                    break;
                }
                id = prev;
                dir = side == 0 ? 1 : -1;
                continue;
            }
            if (side == 0)
                wy = y0;
        }
        else if (side == 0 ? below : above)
        {
            // towards v1
            if (dir != (side == 0 ? 1 : -1))
            {
                int next = Wall_Next(id);
                if (next == -1 || !(coll_lines[next].flags & flag))
                {
                    if (side == 0 ? y - y1 < -0.1 : y - y1 > 0.1)
                        return -1;
                    wy = y1;
                    break;
                }
                id = next;
                dir = side == 0 ? -1 : 1;
                continue;
            }
            if (side == 0)
                wy = y1;
        }
        break;
    }

    float x0 = Line_V0(id)->X;
    float x1 = Line_V1(id)->X;
    *dist = x0 + ((x1 - x0) * (wy - y0)) / (y1 - y0) - x;
    return id;
}

// mpLineIntersectionV: the movement b0->b1 against a vertical line at x a0x.
static int Line_IntersectV(float *int_x, float *int_y, float a0x, float a0y, float a1y,
                           float b0x, float b0y, float b1x, float b1y)
{
    float min_ay, max_ay;

    if (a0y < a1y)
    {
        if ((b0y < a0y && b1y < a0y) || (a1y < b0y && a1y < b1y))
            return 0;
        if (b1x - a0x < -0.0001 || b0x - a0x > 0.0001)
            return 0;
        min_ay = a0y;
        max_ay = a1y;
    }
    else
    {
        if ((b0y < a1y && b1y < a1y) || (a0y < b0y && a0y < b1y))
            return 0;
        if (b0x - a0x < -0.0001 || b1x - a0x > 0.0001)
            return 0;
        min_ay = a1y;
        max_ay = a0y;
    }

    double dby = b1y - b0y;
    double dbx = b1x - b0x;
    if (dabs(dbx) < 0.0001)
        return 0;

    double new_y = (dby / dbx * (a0x - b0x)) + b0y;
    double dy = new_y - min_ay;
    if (dy < 0.0)
    {
        if (dy < -0.1)
            return 0;
        new_y = min_ay;
    }
    dy = new_y - max_ay;
    if (dy > 0.0)
    {
        if (dy > 0.1)
            return 0;
        new_y = max_ay;
    }
    *int_x = a0x;
    *int_y = new_y;
    return 1;
}

// mpCheckRightWall / mpCheckLeftWall: the nearest wall the movement a->b
// runs into.
static int Wall_Check(int side, float ax, float ay, float bx, float by, int *line_out)
{
    float min_dist2 = 3.4e38f;
    int hit = 0;

    for (int i = 0; i < near_num[side]; i++)
    {
        int id = near_ids[side][i];
        float x0 = Line_V0(id)->X, y0 = Line_V0(id)->Y;
        float x1 = Line_V1(id)->X, y1 = Line_V1(id)->Y;
        float int_x, int_y;
        int found;

        if (fabs(x0 - x1) > 0.0001)
            found = Line_Intersect(x0, y0, x1, y1, ax, ay, bx, by, &int_x, &int_y);
        else if (side == 0 ? ax >= bx : ax <= bx)
            found = Line_IntersectV(&int_x, &int_y, x0, y0, y1, ax, ay, bx, by);
        else
            found = 0;

        if (found)
        {
            float dist2 = (int_x - ax) * (int_x - ax) + (int_y - ay) * (int_y - ay);
            if (min_dist2 > dist2)
            {
                min_dist2 = dist2;
                *line_out = id;
                hit = 1;
            }
        }
    }
    return hit;
}

// mpRemap2d: point p relative to line a0->a1, moved to line b0->b1.
static void Remap2d(float *x_out, float *y_out, float ax0, float ay0, float ax1, float ay1,
                    float bx0, float by0, float bx1, float by1, float px, float py)
{
    double dx = ax1 - ax0;
    double dy = ay1 - ay0;
    float fx = px - ax0;
    float fy = py - ay0;
    double dist2 = (dy * dy) + (dx * dx);

    if (dabs(dist2) > 0.0001)
    {
        double t = (dy * fy + dx * fx) / dist2;
        if (t > 1.0)
            t = 1.0;
        else if (t < 0.0)
            t = 0.0;
        *x_out = px + (1.0 - t) * (bx0 - ax0) + t * (bx1 - ax1);
        *y_out = py + (1.0 - t) * (by0 - ay0) + t * (by1 - ay1);
    }
    else
    {
        *x_out = px + (bx0 - ax0) + (bx1 - ax0);
        *y_out = py + (by0 - ay0) + (by1 - ay0);
    }
}

// mpLib_800511A4_RightWall / mpLib_800515A0_LeftWall: does a wall's end
// pass through the ECB edge as it moves from a->b to c->d?
static int Wall_CheckSwept(int side, float ax, float ay, float bx, float by,
                           float cx, float cy, float dx, float dy, int *line_out)
{
    float min_dist2 = 3.4e38f;
    int hit = 0;

    for (int i = 0; i < near_num[side]; i++)
    {
        int id = near_ids[side][i];
        for (int end = 0; end < 2; end++)
        {
            Vec2 *v = end == 0 ? Line_V0(id) : Line_V1(id);
            float x0 = v->X, y0 = v->Y;
            float x1 = x0, y1 = y0; // where it was last frame
            float x, y, int_x, int_y;

            Remap2d(&x, &y, ax, ay, bx, by, cx, cy, dx, dy, x1, y1);
            float vdx = x0 - x;
            float vdy = y0 - y;
            if (vdx * vdx + vdy * vdy <= 0.001f)
                continue;
            if (!Line_Intersect(cx, cy, dx, dy, x, y, x0, y0, &int_x, &int_y))
                continue;

            float dist2 = (int_x - x1) * (int_x - x1) + (int_y - y1) * (int_y - y1);
            if ((vdx * (int_x - x1)) + (vdy * (int_y - y1)) < 0.0f)
                dist2 = -dist2;
            if (min_dist2 > dist2)
            {
                min_dist2 = dist2;
                *line_out = id;
                hit = 1;
            }
        }
    }
    return hit;
}

// mpColl_RightWall_inline: remember a wall unless its run is already listed.
static void Wall_Add(int side, int id)
{
    for (int i = 0; i < wall_list_num[side]; i++)
    {
        if (Lines_Connected(wall_list[side][i], id))
            return;
    }
    if (wall_list_num[side] < 8)
        wall_list[side][wall_list_num[side]++] = id;
}

// mpColl_80044E10_RightWall / mpColl_80045B74_LeftWall: list the walls the
// ECB ran into moving from (px, py) with prev to (x, y) with cur.
static int Wall_Find(int side, float px, float py, SimEcb *prev, float x, float y, SimEcb *cur, int tiny)
{
    int id;
    int hit = 0;
    float sx = x + (side == 0 ? cur->left : cur->right); // the side point facing the wall
    float sy = y + cur->side;
    float psx = px + (side == 0 ? prev->left : prev->right);
    float psy = py + prev->side;
    float bot_x = x, bot_y = y + cur->bottom;
    float pbot_x = px, pbot_y = py + prev->bottom;
    float top_x = x, top_y = y + cur->top;
    float ptop_x = px, ptop_y = py + prev->top;

    wall_list_num[side] = 0;

    if (Wall_Check(side, psx, psy, sx, sy, &id))
        Wall_Add(side, id), hit = 1;
    if (Wall_Check(side, pbot_x, pbot_y, bot_x, bot_y, &id))
        Wall_Add(side, id), hit = 1;
    if (Wall_Check(side, ptop_x, ptop_y, top_x, top_y, &id))
        Wall_Add(side, id), hit = 1;

    if (Wall_Check(side, bot_x, bot_y, sx, sy, &id))
        Wall_Add(side, id), hit = 1;
    if (!tiny)
    {
        int found = side == 0 ? Wall_CheckSwept(side, pbot_x, pbot_y, psx, psy, bot_x, bot_y, sx, sy, &id)
                              : Wall_CheckSwept(side, psx, psy, pbot_x, pbot_y, sx, sy, bot_x, bot_y, &id);
        if (found)
            Wall_Add(side, id), hit = 1;
    }

    if (Wall_Check(side, top_x, top_y, sx, sy, &id))
        Wall_Add(side, id), hit = 1;
    if (!tiny)
    {
        int found = side == 0 ? Wall_CheckSwept(side, psx, psy, ptop_x, ptop_y, sx, sy, top_x, top_y, &id)
                              : Wall_CheckSwept(side, ptop_x, ptop_y, psx, psy, top_x, top_y, sx, sy, &id);
        if (found)
            Wall_Add(side, id), hit = 1;
    }
    return hit;
}

// Keeps the furthest push: for right walls the largest x, for left walls the
// smallest.
static void Wall_Keep(int side, float *best, float x)
{
    if (side == 0 ? *best < x : *best > x)
        *best = x;
}

// mpColl_800454A4_RightWall / mpColl_80046224_LeftWall: push x out of the
// listed walls. Returns 1 if it moved.
static int Wall_Push(int side, float *x, float y, SimEcb *cur)
{
    float best = side == 0 ? -3.4e38f : 3.4e38f;
    float sx = side == 0 ? cur->left : cur->right;
    float dist;

    for (int i = 0; i < wall_list_num[side]; i++)
    {
        int wall = wall_list[side][i];

        // the whole run is below or above the ECB: line up with its end
        Vec2 top = Wall_End(wall, side, 1);
        if (top.Y < y + cur->bottom)
        {
            if ((side == 0 ? best < top.X : best > top.X) && Wall_DistanceX(wall, side, top.X, top.Y, &dist) != -1)
                best = top.X;
            continue;
        }
        Vec2 bottom = Wall_End(wall, side, 0);
        if (bottom.Y > y + cur->top)
        {
            if ((side == 0 ? best < bottom.X : best > bottom.X) && Wall_DistanceX(wall, side, bottom.X, bottom.Y, &dist) != -1)
                best = bottom.X;
            continue;
        }

        // the bottom, side and top points onto the wall
        if (Wall_DistanceX(wall, side, *x, y + cur->bottom, &dist) != -1)
            Wall_Keep(side, &best, *x + dist);
        if (Wall_DistanceX(wall, side, *x + sx, y + cur->side, &dist) != -1)
            Wall_Keep(side, &best, *x + dist);
        float px = *x, py = y + cur->top; // the top point, used below
        if (Wall_DistanceX(wall, side, px, py, &dist) != -1)
            Wall_Keep(side, &best, *x + dist);

        // a ceiling over a right wall: keep the top point under its corner
        // (the left wall version checks the wrong line and never runs)
        if (side == 0)
        {
            int ceil = Line_SkipKind(wall, LINEFLAG_RWALL, 0);
            if (Line_Usable(ceil) && (Line_Kind(ceil) & LINEFLAG_CEIL) && py > top.Y)
            {
                int line = Line_SkipKind(ceil, LINEFLAG_CEIL, 1);
                if (Line_Usable(line) && (Line_Kind(line) & LINEFLAG_RWALL))
                {
                    // mpLineGetNormal
                    float nx = -(Line_V1(line)->Y - Line_V0(line)->Y);
                    float ny = Line_V1(line)->X - Line_V0(line)->X;
                    float len = sqrtf(nx * nx + ny * ny);
                    if (len > 0)
                    {
                        nx /= len;
                        ny /= len;
                        float d = (py - top.Y) / nx * -ny + top.X - px + 0.5f;
                        Wall_Keep(side, &best, *x + d);
                    }
                }
            }
        }

        // wall corners inside the ECB's height: keep them outside its edges
        float top_y = y + cur->top;
        float mid_y = y + cur->side;
        float bot_y = y + cur->bottom;
        float lower = sx / (cur->side - cur->bottom); // x per y, bottom to side
        float upper = sx / (cur->side - cur->top);    // x per y, side to top
        for (int pass = 0; pass < 2; pass++)
        {
            // right walls: down the run through the bottom ends, then up
            // through the top ends; left walls the other way round
            int down = pass == 0;
            int towards_v1 = (side == 0) == down;
            for (int id = wall; id != -1 && Line_Kind(id) == (side == 0 ? LINEFLAG_RWALL : LINEFLAG_LWALL);
                 id = towards_v1 ? Wall_Next(id) : Wall_Prev(id))
            {
                Vec2 *v = towards_v1 ? Line_V1(id) : Line_V0(id);
                float ex;
                if (bot_y <= v->Y && v->Y <= mid_y)
                    ex = lower * (v->Y - bot_y);
                else if (mid_y <= v->Y && v->Y <= top_y)
                    ex = upper * (v->Y - top_y);
                else if (down ? v->Y < bot_y : v->Y > top_y)
                    break;
                else
                    continue;
                Wall_Keep(side, &best, v->X - ex);
            }
        }
    }

    if (side == 0 ? *x < best : *x > best)
    {
        *x = best;
        return 1;
    }
    return 0;
}

// Walls are checked left, right, left, right, and again until nothing
// changes (mpColl_80046904).
static void Sim_Walls(float *x, float y, float px, float py, SimEcb *prev, SimEcb *cur)
{
    // only walls near the ECB's path can be hit. A wall's end can poke
    // through an ECB edge from up to a frame's movement away, so the margin
    // is generous.
    float min_x = px + prev->left, max_x = px + prev->right;
    float min_y = py + prev->bottom, max_y = py + prev->top;
    if (min_x > *x + cur->left)
        min_x = *x + cur->left;
    if (max_x < *x + cur->right)
        max_x = *x + cur->right;
    if (min_y > y + cur->bottom)
        min_y = y + cur->bottom;
    if (max_y < y + cur->top)
        max_y = y + cur->top;
    min_x -= 10.f;
    min_y -= 10.f;
    max_x += 10.f;
    max_y += 10.f;

    int any = 0;
    for (int side = 0; side < 2; side++)
    {
        near_num[side] = 0;
        for (int i = 0; i < wall_num[side]; i++)
        {
            WallBox *b = &wall_box[side][i];
            if (b->max_x < min_x || b->min_x > max_x || b->max_y < min_y || b->min_y > max_y)
                continue;
            near_ids[side][near_num[side]++] = wall_ids[side][i];
            any = 1;
        }
    }
    if (!any)
        return;

    int tiny = cur->top - cur->bottom < 6.f;
    int flags = 0;
    for (int pass = 0; pass < 4; pass++)
    {
        int old = flags;
        flags = 0;
        for (int i = 0; i < 4; i++)
        {
            int side = i & 1 ? 0 : 1; // left walls first
            if (Wall_Find(side, px, py, prev, *x, y, cur, tiny) && Wall_Push(side, x, y, cur))
                flags |= side == 0 ? 4 : 8;
        }
        if (flags == old)
            break;
    }
}

static void Wall_CacheRange(int start, int num)
{
    for (int i = 0; i < num; i++)
    {
        int id = start + i;
        u32 flags = coll_lines[id].flags;
        if (!(flags & LINEFLAG_ENABLED) || (flags & LINEFLAG_EMPTY))
            continue;
        int side = (flags & LINEFLAG_RWALL) ? 0 : (flags & LINEFLAG_LWALL) ? 1 : -1;
        if (side < 0 || wall_num[side] >= LL_MAX_WALLS)
            continue;
        Vec2 *v0 = Line_V0(id), *v1 = Line_V1(id);
        WallBox *b = &wall_box[side][wall_num[side]];
        b->min_x = v0->X < v1->X ? v0->X : v1->X;
        b->max_x = v0->X < v1->X ? v1->X : v0->X;
        b->min_y = v0->Y < v1->Y ? v0->Y : v1->Y;
        b->max_y = v0->Y < v1->Y ? v1->Y : v0->Y;
        wall_ids[side][wall_num[side]++] = id;
    }
}

static void Wall_BuildCache(RawCollLine *lines, CollVert *verts)
{
    coll_lines = lines;
    coll_verts = verts;
    wall_num[0] = 0;
    wall_num[1] = 0;

    for (CollGroup *group = *stc_firstcollgroup; group != 0; group = group->next)
    {
        CollGroupDesc *desc = group->desc;
        Wall_CacheRange(desc->rwall_start, desc->rwall_num);
        Wall_CacheRange(desc->lwall_start, desc->lwall_num);
        Wall_CacheRange(desc->dyn_start, desc->dyn_num);
    }
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
    SimEcb ecb;          // the whole ECB used on the starting frame
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
    float pos_x, pos_y;   // fighter position of the last frame
    SimEcb ecb;           // ECB used on the last frame
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
static float common_run_friction;
static float common_jump_back_stick;

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

// The ECB the game used this frame.
static void Ecb_FromFighter(FighterData *fp, SimEcb *ecb)
{
    CollData *cd = &fp->coll_data;
    ecb->bottom = cd->ecbCurrCorrect_bot.Y;
    ecb->top = cd->ecbCurrCorrect_top.Y;
    ecb->side = cd->ecbCurrCorrect_right.Y;
    ecb->left = cd->ecbCurrCorrect_left.X;
    ecb->right = cd->ecbCurrCorrect_right.X;
}

// A learned frame's shape, turned the way the fighter faces. The bottom
// stays as it is (the lock decides it).
static void Ecb_FromSample(EcbSample *e, float facing, SimEcb *ecb)
{
    ecb->top = e->top;
    ecb->side = e->side_y;
    if (facing > 0)
    {
        ecb->right = e->front;
        ecb->left = e->back;
    }
    else
    {
        ecb->right = -e->back;
        ecb->left = -e->front;
    }
}

// mpColl_80042384: keeps the side points between the top and the bottom,
// which matters when the lock holds the bottom somewhere else.
static void Ecb_Fix(SimEcb *ecb)
{
    if (fabs(ecb->top - ecb->bottom) < 1.f)
    {
        ecb->top += 1.f;
        ecb->side = 0.5f * (ecb->top + ecb->bottom);
    }
    if (ecb->top < 1.f)
        ecb->top = 1.f;
    if (ecb->left > -1.f)
        ecb->left = -1.f;
    if (ecb->right < 1.f)
        ecb->right = 1.f;
    if (ecb->top < ecb->bottom)
        ecb->top = ecb->bottom + 1.f;
    if (ecb->side > ecb->top || ecb->side < ecb->bottom)
        ecb->side = 0.5f * (ecb->top + ecb->bottom);
    if (ecb->top - ecb->side < 0.001f || ecb->side - ecb->bottom < 0.001f)
        ecb->side = 0.5f * (ecb->top + ecb->bottom);
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
    Ecb_FromFighter(fp, &s->ecb);
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
    s->pos_x = s->x;
    s->pos_y = s->y;
    s->ecb = start->ecb;
    s->ecb.bottom = start->bottom;
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

    // the rest of the ECB comes from the bones; keep the last shape until
    // this frame's has been seen
    SimEcb ecb = s->ecb;
    ecb.bottom = s->bottom;
    if (e->has_shape)
        Ecb_FromSample(e, start->facing, &ecb);
    Ecb_Fix(&ecb);

    // walls push the fighter out before the floor test (mpColl_80046904)
    Sim_Walls(&s->x, s->y, s->pos_x, s->pos_y, &s->ecb, &ecb);
    s->ecb = ecb;
    s->pos_x = s->x;
    s->pos_y = s->y;

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

#define LL_GUESS_JUMP_LEN 40 // until a jump has been seen ending

// A jump pressed on the next frame (or the jumpsquat already going): the
// squat frames slide on the ground with friction (ftCo_KneeBend_Phys), then
// takeoff (ftCo_Jump_Enter, ftCo_800CB110) sets the speed and moves the
// fighter without gravity on that frame (ftCo_Jump_Phys skips its first
// frame). s is the state at the end of the takeoff frame. Assumes a flat
// floor. Returns the frames until takeoff.
static int Sim_GroundJump(FighterData *fp, int short_hop, SimStart *s)
{
    float x = fp->phys.pos.X;
    float y = fp->phys.pos.Y;
    float gr_vel = fp->phys.self_vel_ground.X;
    float stick_x = fp->input.lstick.X;
    float startup = fp->attr.jump_startup_time;

    // KneeBend_Anim takes off once the animation frame reaches the startup
    // time; a new press starts the squat at frame 0 on the next frame
    float squat_left = startup;
    int until = 1;
    if (fp->state_id == ASID_KNEEBEND)
    {
        squat_left = startup - fp->state.frame;
        until = 0;
    }
    int squat = (int)squat_left;
    if (squat < squat_left)
        squat++;
    if (squat < 1)
        squat = 1;
    until += squat;
    int slide = until - 1; // ground frames before takeoff

    for (int i = 0; i < slide; i++)
    {
        // ft_80084F3C
        float friction = fp->attr.ground_friction;
        if (fabs(gr_vel) > fp->attr.walk_maximum_velocity)
            friction *= common_run_friction;
        float accel;
        if (fabs(friction) > fabs(gr_vel))
            accel = -gr_vel;
        else
            accel = gr_vel > 0 ? -friction : friction;
        gr_vel += accel;
        x += gr_vel;
    }

    float vx = gr_vel * fp->attr.ground_to_air_jump_momentum_multiplier + stick_x * fp->attr.jump_h_initial_velocity;
    float max = fp->attr.jump_h_max_velocity;
    if (vx > max)
        vx = max;
    else if (vx < -max)
        vx = -max;
    float vy = short_hop ? fp->attr.hop_v_initial_velocity : fp->attr.jump_v_initial_velocity;
    x += vx;
    y += vy;

    int ts = stick_x * fp->facing_direction > -common_jump_back_stick ? TS_JUMPF : TS_JUMPB;
    int trigger = (u8)fp->input.timer_trigger_any_ignore_hitlag + until;

    s->pos.X = x;
    s->pos.Y = y;
    s->vel.X = vx;
    s->vel.Y = vy;
    s->stick_x = stick_x;
    s->stick_y = fp->input.lstick.Y;
    s->facing = fp->facing_direction;
    s->bottom = 0; // the lock keeps the grounded bottom
    s->locked_bottom = 0;
    Ecb_FromFighter(fp, &s->ecb);
    if (Ecb_Get(ts, 0)->has_shape)
        Ecb_FromSample(Ecb_Get(ts, 0), fp->facing_direction, &s->ecb);
    s->ecb.bottom = 0;
    Ecb_Fix(&s->ecb);
    s->ts = ts;
    s->frame = 0;
    s->len = state_len[ts];
    s->len_estimated = 0;
    if (s->len == 0)
    {
        s->len = LL_GUESS_JUMP_LEN;
        s->len_estimated = 1;
    }
    s->fastfall = 0;
    s->tilt_timer = LL_TIMER_MAX; // ftCo_800CB110 resets the stick timer
    s->trigger_timer = trigger < LL_TIMER_MAX ? trigger : LL_TIMER_MAX;
    s->ecb_lock = 9; // set to 10 on takeoff, counted down before collision
    s->skip_line = -1;
    return until;
}

///////////////////////
/// Event state     ///
///////////////////////

void Event_Exit(GOBJ *menu);
void Event_ClearLearned(GOBJ *menu);
void Event_ChangeCollDisplay(GOBJ *menu, int value);

static const char *speed_names[] = {"1", "5/6", "2/3", "1/2", "1/4"};
static const float speed_values[] = {1.f, 5.f / 6.f, 2.f / 3.f, 1.f / 2.f, 1.f / 4.f};
static const char *preview_names[] = {"Both", "Full hop", "Short hop", "Off"};
static const char *panel_side_names[] = {"Auto", "Right", "Left"};

enum preview_kind
{
    PREVIEW_BOTH,
    PREVIEW_FULL,
    PREVIEW_SHORT,
    PREVIEW_OFF,
};

enum panel_side
{
    PANEL_AUTO,
    PANEL_RIGHT,
    PANEL_LEFT,
};

enum options_main
{
    OPT_PATH,
    OPT_BODY,
    OPT_PREVIEW,
    OPT_RING,
    OPT_BEEPS,
    OPT_PANEL,
    OPT_PANEL_SIDE,
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
        .desc = {"Draw where Falcon's ECB bottom lands if you keep",
                 "holding the stick and press nothing. Green for a",
                 "NIL, cyan for an aerial interrupt, thin gray for",
                 "anything else."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Body Path",
        .val = 1,
        .desc = {"Also draw a smooth line through Falcon's body,",
                 "which is easier to follow than the ECB bottom."},
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Jump Preview",
        .value_num = countof(preview_names),
        .values = preview_names,
        .desc = {"On the ground, show where a full hop and a short",
                 "hop would go if you jumped now, holding the stick",
                 "where it is."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Timing Ring",
        .val = 1,
        .desc = {"A ring around Falcon closes in on the next aerial",
                 "interrupt. Press the aerial when it meets the",
                 "inner ring."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Timing Beeps",
        .desc = {"Beep 20 and 10 frames before the next aerial",
                 "interrupt, and on the frame to press."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Info Panel",
        .val = 1,
        .desc = {"Show the prediction, which aerials can interrupt,",
                 "and your last landing. Exact counts how often the",
                 "prediction matched what really happened."},
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Panel Side",
        .value_num = countof(panel_side_names),
        .values = panel_side_names,
        .desc = {"Where the info panel goes. Auto keeps it on the",
                 "side of the screen Falcon isn't on."},
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
        .desc = {"Show stage collision, Falcon's ECB, hitboxes and",
                 "hurtboxes, and hide his model so it doesn't",
                 "cover them."},
        .OnChange = Event_ChangeCollDisplay,
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Debug Log",
        .desc = {"Write every airborne frame's state, position,",
                 "speed and ECB to Dolphin's log. Only needed to",
                 "report a wrong prediction."},
    },
    {
        .kind = OPTKIND_FUNC,
        .name = "Forget Learned ECBs",
        .desc = {"Falcon's jumps and falls are built in; his aerials",
                 "are learned as you play. This forgets what was",
                 "learned and goes back to the built-in data."},
        .OnSelect = Event_ClearLearned,
    },
    {
        .kind = OPTKIND_INFO,
        .name = "Help",
        .desc = {"Cyan stretch: press an aerial there to land (AI).",
                 "Its arrows are the C-stick aerials that work, a",
                 "square for nair. Gray: other landings, or still",
                 "learning: do each aerial once, high up."},
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
static int stage_logged;

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
static int ghost_timer;

// jump previews while on the ground
static Prediction *pred_fh;
static Prediction *pred_sh;
static int preview_fh;
static int preview_sh;

// body line: the fighter's position raised to the middle of his ECB
static float body_offset = 9.f;

// timing ring and beeps for the next aerial interrupt
static int ring_frames; // frames until the press, 0 = no ring
static Vec2 ring_center;
static int beep_target = -100;
static int beep_done;

static int panel_left;

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

    sprintf(buf, "LL %d %s f%d pos %.4f %.4f vel %.5f %.5f ff%d lock%d ecb top %.4f bot %.4f l %.4f r %.4f side %.4f used bot %.4f ac%d stick %.4f %.4f face %d\n",
            event_vars->game_timer, tracked_state_names[ts], frame,
            fp->phys.pos.X, fp->phys.pos.Y, fp->phys.self_vel.X, fp->phys.self_vel.Y,
            fp->flags.is_fastfall, cd->u.ecb_bot_lock_frames,
            cd->ecbCurr_top.Y, cd->ecbCurr_bot.Y, cd->ecbCurr_left.X, cd->ecbCurr_right.X, cd->ecbCurr_right.Y,
            cd->ecbCurrCorrect_bot.Y, fp->ftcmd_var.flag0 != 0,
            fp->input.lstick.X, fp->input.lstick.Y, fp->facing_direction > 0 ? 1 : -1);
    Log(buf);
}

// Every collision line of the stage, so a wrong prediction near a wall or
// ledge can be replayed exactly: id, kind flags, ends, previous and next
// line (and the other group's, if any).
static void Log_Stage(void)
{
    char buf[160];
    RawCollLine *lines = (RawCollLine *)*stc_collline;
    CollVert *verts = *stc_collvert;

    for (CollGroup *group = *stc_firstcollgroup; group != 0; group = group->next)
    {
        CollGroupDesc *d = group->desc;
        int starts[5] = {d->floor_start, d->ceil_start, d->rwall_start, d->lwall_start, d->dyn_start};
        int nums[5] = {d->floor_num, d->ceil_num, d->rwall_num, d->lwall_num, d->dyn_num};
        for (int r = 0; r < 5; r++)
        {
            for (int i = 0; i < nums[r]; i++)
            {
                int id = starts[r] + i;
                CollLineDesc *desc = lines[id].desc;
                Vec2 *v0 = &verts[(u16)desc->vert_prev].pos_curr;
                Vec2 *v1 = &verts[(u16)desc->vert_next].pos_curr;
                sprintf(buf, "LLLINE %d flags %x %.4f %.4f %.4f %.4f prev %d next %d alt %d %d plat %d\n",
                        id, lines[id].flags, v0->X, v0->Y, v1->X, v1->Y,
                        desc->line_prev, desc->line_next, desc->line_prev_altgroup, desc->line_next_altgroup,
                        desc->is_unk);
                Log(buf);
            }
        }
    }
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

// "NIL", "AI", "?" while learning, "-" for anything else.
static const char *Kind_Short(Prediction *p)
{
    if (!p->land_frame)
        return "-";
    if (p->uncertain_from <= p->land_frame)
        return "?";
    if (Kind_Highlighted(p->land_kind))
        return land_kind_names[p->land_kind];
    return "-";
}

// The aerials of the first AI window as letters, like "NUD".
static void Ai_Letters(Prediction *p, char *out)
{
    static const char letters[] = "NFBUD";
    int n = 0;

    if (!p->ai_first)
        out[n++] = '-';
    else if (p->ai_first >= p->uncertain_from)
        out[n++] = '?';
    else
    {
        for (int a = 0; a < 5; a++)
        {
            if (p->ai_aerials & (1 << a))
                out[n++] = letters[a];
        }
    }
    out[n] = 0;
}

// On the ground: what a full hop (FH) and a short hop (SH) would give.
static void Text_Preview(void)
{
    char fh[8], sh[8];
    Ai_Letters(pred_fh, fh);
    Ai_Letters(pred_sh, sh);

    if (preview_fh && preview_sh)
    {
        sprintf(text_predict, "FH %s, SH %s", Kind_Short(pred_fh), Kind_Short(pred_sh));
        sprintf(text_ai, "FH %s, SH %s", fh, sh);
    }
    else if (preview_fh)
    {
        sprintf(text_predict, "FH %s", Kind_Short(pred_fh));
        sprintf(text_ai, "FH %s", fh);
    }
    else
    {
        sprintf(text_predict, "SH %s", Kind_Short(pred_sh));
        sprintf(text_ai, "SH %s", sh);
    }
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

#define LL_GHOST_FRAMES 90

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

    // keep the attempt on screen for a moment, but only when a NIL or an
    // aerial interrupt was predicted or happened
    ghost_visible = seg_valid && (Kind_Highlighted(kind) || Kind_Highlighted(pred_seg->land_kind));
    ghost_timer = LL_GHOST_FRAMES;

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

static const GXColor color_neutral = {150, 150, 150, 255};
static const GXColor color_body = {190, 200, 255, 255};
static const GXColor color_ecb = {255, 230, 0, 255};

#define LL_CIRCLE_SEGS 24
static Vec2 circle[LL_CIRCLE_SEGS + 1]; // unit circle, filled in Event_Init

static void Draw_Path(Vec2 *pos, float *bottom, int from, int to, GXColor color, u8 size)
{
    int count = to - from + 1;
    if (count < 2)
        return;

    event_vars->GFX_Start(count, (GFX_Params){.shape = GX_LINESTRIP, .size = size});
    for (int i = from; i <= to; i++)
        GFX_AddVtx(pos[i].X, pos[i].Y + bottom[i], 0, color);
}

// The fighter's position raised by a fixed amount: a smooth arc through his
// body, unlike the ECB bottom, which moves with his legs.
static void Draw_BodyPath(Prediction *p, int last)
{
    int count = last + 1;
    if (count < 2)
        return;

    event_vars->GFX_Start(count, (GFX_Params){.shape = GX_LINESTRIP, .size = 12});
    for (int i = 0; i <= last; i++)
        GFX_AddVtx(p->pos[i].X, p->pos[i].Y + body_offset, 0, color_body);
}

static void Draw_Circle(float x, float y, float r, GXColor color, u8 size)
{
    event_vars->GFX_Start(LL_CIRCLE_SEGS + 1, (GFX_Params){.shape = GX_LINESTRIP, .size = size});
    for (int i = 0; i <= LL_CIRCLE_SEGS; i++)
        GFX_AddVtx(x + circle[i].X * r, y + circle[i].Y * r, 0, color);
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

// Falcon's ECB right now, for the collision view (his model is hidden).
static void Draw_CurrentEcb(FighterData *fp)
{
    CollData *cd = &fp->coll_data;
    float x = fp->phys.pos.X;
    float y = fp->phys.pos.Y;

    event_vars->GFX_Start(5, (GFX_Params){.shape = GX_LINESTRIP, .size = 24});
    GFX_AddVtx(x + cd->ecbCurrCorrect_top.X, y + cd->ecbCurrCorrect_top.Y, 0, color_ecb);
    GFX_AddVtx(x + cd->ecbCurrCorrect_right.X, y + cd->ecbCurrCorrect_right.Y, 0, color_ecb);
    GFX_AddVtx(x + cd->ecbCurrCorrect_bot.X, y + cd->ecbCurrCorrect_bot.Y, 0, color_ecb);
    GFX_AddVtx(x + cd->ecbCurrCorrect_left.X, y + cd->ecbCurrCorrect_left.Y, 0, color_ecb);
    GFX_AddVtx(x + cd->ecbCurrCorrect_top.X, y + cd->ecbCurrCorrect_top.Y, 0, color_ecb);
}

// The C-stick direction of each aerial in mask, drawn from (x, y): arrows
// for fair, bair, uair and dair, a small square for nair.
static void Draw_AerialArrows(float x, float y, u8 mask, float facing, GXColor color)
{
    static const float dirs[5][2] = {{0, 0}, {1, 0}, {-1, 0}, {0, 1}, {0, -1}}; // N F B U D
    int count = 0;
    for (int a = 0; a < 5; a++)
    {
        if (mask & (1 << a))
            count += a == 0 ? 8 : 6;
    }
    if (count == 0)
        return;

    event_vars->GFX_Start(count, (GFX_Params){.shape = GX_LINES, .size = 24});
    for (int a = 0; a < 5; a++)
    {
        if (!(mask & (1 << a)))
            continue;

        if (a == 0)
        {
            float r = 1.2f;
            GFX_AddVtx(x - r, y - r, 0, color);
            GFX_AddVtx(x + r, y - r, 0, color);
            GFX_AddVtx(x + r, y - r, 0, color);
            GFX_AddVtx(x + r, y + r, 0, color);
            GFX_AddVtx(x + r, y + r, 0, color);
            GFX_AddVtx(x - r, y + r, 0, color);
            GFX_AddVtx(x - r, y + r, 0, color);
            GFX_AddVtx(x - r, y - r, 0, color);
            continue;
        }

        float dx = dirs[a][0] * facing; // forward and back follow facing
        float dy = dirs[a][1];
        float tip_x = x + dx * 6.f;
        float tip_y = y + dy * 6.f;
        // arrowhead: back from the tip, 40 degrees either side
        float bx = -dx * 0.766f, by = -dy * 0.766f;
        float px = -dy * 0.643f, py = dx * 0.643f;

        GFX_AddVtx(x + dx * 1.5f, y + dy * 1.5f, 0, color);
        GFX_AddVtx(tip_x, tip_y, 0, color);
        GFX_AddVtx(tip_x, tip_y, 0, color);
        GFX_AddVtx(tip_x + (bx + px) * 2.f, tip_y + (by + py) * 2.f, 0, color);
        GFX_AddVtx(tip_x, tip_y, 0, color);
        GFX_AddVtx(tip_x + (bx - px) * 2.f, tip_y + (by - py) * 2.f, 0, color);
    }
}

// Each run of frames where an aerial interrupts: a thick stretch of the
// path, cyan (orange if every aerial there lands with aerial lag), with the
// aerials that work drawn at its start.
static void Draw_AiWindows(Prediction *p, int last)
{
    for (int k = 1; k <= last; k++)
    {
        if (!p->ai_mask[k])
            continue;

        int e = k;
        u8 mask = 0;
        int all_lag = 1;
        while (e <= last && p->ai_mask[e])
        {
            mask |= p->ai_mask[e];
            if (p->ai_lag_mask[e] != p->ai_mask[e])
                all_lag = 0;
            e++;
        }
        e--;

        GXColor color = land_kind_colors[LAND_AI];
        if (k >= p->uncertain_from)
            color = color_learning;
        else if (all_lag)
            color = land_kind_colors[LAND_LCANCEL];

        // from half a frame before the window to half a frame after it
        int before = k - 1;
        int after = e < p->num ? e + 1 : e;
        event_vars->GFX_Start(e - k + 3, (GFX_Params){.shape = GX_LINESTRIP, .size = 72});
        GFX_AddVtx((p->pos[before].X + p->pos[k].X) / 2,
                   (p->pos[before].Y + p->bottom[before] + p->pos[k].Y + p->bottom[k]) / 2, 0, color);
        for (int i = k; i <= e; i++)
            GFX_AddVtx(p->pos[i].X, p->pos[i].Y + p->bottom[i], 0, color);
        GFX_AddVtx((p->pos[e].X + p->pos[after].X) / 2,
                   (p->pos[e].Y + p->bottom[e] + p->pos[after].Y + p->bottom[after]) / 2, 0, color);

        Draw_AerialArrows(p->pos[k].X, p->pos[k].Y + p->bottom[k], mask, p->facing, color);
        k = e;
    }
}

static void Draw_Prediction(Prediction *p, int body)
{
    int last = p->land_frame ? p->land_frame : p->num;
    int certain = p->land_frame && p->uncertain_from > p->land_frame;
    int highlight = certain && Kind_Highlighted(p->land_kind);

    if (body && Options_Main[OPT_BODY].val)
        Draw_BodyPath(p, last);

    if (highlight)
        Draw_Path(p->pos, p->bottom, 0, last, land_kind_colors[p->land_kind], 24);
    else
    {
        int known = p->uncertain_from - 1;
        if (known > last)
            known = last;
        if (known < 0)
            known = 0;
        Draw_Path(p->pos, p->bottom, 0, known, color_neutral, 12);
        Draw_Path(p->pos, p->bottom, known, last, color_learning, 12);
    }

    Draw_AiWindows(p, p->land_frame ? p->land_frame - 1 : p->num);

    if (highlight)
    {
        int k = p->land_frame;
        Draw_Ecb(p->pos[k], p->bottom[k], &p->land_ecb, p->facing, land_kind_colors[p->land_kind]);
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

// The outer ring closes in on the inner one, meeting it on the frame before
// the press, so the press lands as the rings touch.
#define LL_RING_FRAMES 24
#define LL_RING_INNER 4.f
#define LL_RING_STEP 0.75f

static void Draw_TimingRing(void)
{
    GXColor color = land_kind_colors[LAND_AI];
    float x = ring_center.X;
    float y = ring_center.Y;

    if (ring_frames <= 1)
    {
        Draw_Circle(x, y, LL_RING_INNER, color, 72);
        return;
    }
    Draw_Circle(x, y, LL_RING_INNER, color, 12);
    Draw_Circle(x, y, LL_RING_INNER + (ring_frames - 1) * LL_RING_STEP, color, 36);
}

static void World_GX(GOBJ *gobj, int pass)
{
    if (pass != 2)
        return;

    if (Options_Main[OPT_COLL].val)
        Draw_CurrentEcb(Fighter_GetGObj(0)->userdata);

    if (!Options_Main[OPT_PATH].val)
        return;

    if (live_visible)
    {
        Draw_Prediction(pred_live, 1);
        if (Options_Main[OPT_RING].val && ring_frames)
            Draw_TimingRing();
    }
    else if (ghost_visible)
    {
        Draw_Prediction(pred_seg, 0);
        Draw_Path(actual_pos, actual_bottom, 0, actual_num - 1, color_actual, 12);
    }
    else
    {
        if (preview_fh)
            Draw_Prediction(pred_fh, 1);
        if (preview_sh)
            Draw_Prediction(pred_sh, 1);
    }
}

static void Hud_GX(GOBJ *gobj, int pass)
{
    if (pass != 2 || !Options_Main[OPT_PANEL].val)
        return;

    float x = panel_left ? -26.7f : 18.f;
    event_vars->HUD_DrawInfoPanelAt((const char **)panel_labels, (const char **)panel_info, countof(panel_labels), x);
}

///////////////////////
/// Event callbacks ///
///////////////////////

// Grounded states a jump can start from.
static int Ground_CanJump(int sid)
{
    return (sid >= ASID_WAIT && sid <= ASID_KNEEBEND) || (sid >= ASID_SQUAT && sid <= ASID_SQUATRV);
}

// The ghost of the last attempt stays until it times out or you move.
static void Ghost_Update(int sid)
{
    if (!ghost_visible)
        return;
    int resting = sid == ASID_WAIT || sid == ASID_LANDING || (sid >= ASID_LANDINGAIRN && sid <= ASID_LANDINGAIRLW);
    if (--ghost_timer <= 0 || !resting)
        ghost_visible = 0;
}

// Auto keeps the panel on the half of the screen Falcon isn't on.
static void Panel_UpdateSide(FighterData *fp)
{
    int side = Options_Main[OPT_PANEL_SIDE].val;
    if (side != PANEL_AUTO)
    {
        panel_left = side == PANEL_LEFT;
        return;
    }

    Vec3 eye;
    COBJ_GetEyePosition(*stc_matchcam_cobj, &eye);
    float dx = fp->phys.pos.X - eye.X;
    if (dx > 4.f) // a little slack so it doesn't flicker near the middle
        panel_left = 1;
    else if (dx < -4.f)
        panel_left = 0;
}

// Ring and beeps count down to the first frame of the next AI window.
static void Timing_Update(FighterData *fp, Prediction *p)
{
    static const int beats[3] = {21, 11, 1};

    ring_frames = 0;
    if (!p->ai_first || p->ai_first >= p->uncertain_from)
        return;

    if (p->ai_first <= LL_RING_FRAMES)
    {
        ring_frames = p->ai_first;
        ring_center.X = fp->phys.pos.X;
        ring_center.Y = fp->phys.pos.Y + body_offset;
    }

    if (!Options_Main[OPT_BEEPS].val)
        return;

    // the same window can shift by a frame as the prediction updates
    int target = event_vars->game_timer + p->ai_first;
    if (target - beep_target > 2 || beep_target - target > 2)
    {
        beep_target = target;
        beep_done = 0;
    }
    for (int i = 0; i < 3; i++)
    {
        int next = i < 2 ? beats[i + 1] : 0;
        if (p->ai_first <= beats[i] && p->ai_first > next && !(beep_done & (1 << i)))
        {
            beep_done |= 1 << i;
            SFX_PlayCommon(i == 2 ? 2 : 1);
        }
    }
}

static void Ground_Preview(FighterData *fp)
{
    int opt = Options_Main[OPT_PREVIEW].val;
    SimStart start;

    preview_fh = opt == PREVIEW_BOTH || opt == PREVIEW_FULL;
    preview_sh = opt == PREVIEW_BOTH || opt == PREVIEW_SHORT;
    if (!preview_fh && !preview_sh)
        return;

    Floor_BuildCache();
    if (preview_fh)
    {
        Sim_GroundJump(fp, 0, &start);
        Predict(fp, &start, pred_fh, 1);
    }
    if (preview_sh)
    {
        Sim_GroundJump(fp, 1, &start);
        Predict(fp, &start, pred_sh, 1);
    }
    Text_Preview();
}

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
    common_run_friction = Common_Float(COMMON_RUN_FRICTION);
    common_jump_back_stick = Common_Float(COMMON_JUMP_BACK_STICK);

    for (int i = 0; i <= LL_CIRCLE_SEGS; i++)
    {
        float t = i * (6.2831853f / LL_CIRCLE_SEGS);
        circle[i].X = cos(t);
        circle[i].Y = sin(t);
    }

    ecb_table = calloc(sizeof(EcbSample) * TS_COUNT * LL_STATE_FRAMES);
    Learned_Clear();
    Learned_Bake();
    pred_live = calloc(sizeof(Prediction));
    pred_seg = calloc(sizeof(Prediction));
    pred_fh = calloc(sizeof(Prediction));
    pred_sh = calloc(sizeof(Prediction));
    actual_pos = calloc(sizeof(Vec2) * (LL_SIM_FRAMES + 1));
    actual_bottom = calloc(sizeof(float) * (LL_SIM_FRAMES + 1));
    floor_cache = calloc(sizeof(FloorLine) * LL_MAX_FLOORS);
    for (int side = 0; side < 2; side++)
    {
        wall_ids[side] = calloc(sizeof(s16) * LL_MAX_WALLS);
        wall_box[side] = calloc(sizeof(WallBox) * LL_MAX_WALLS);
    }

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
    else
        Ghost_Update(sid);

    Panel_UpdateSide(fp);
    if (sid == ASID_WAIT && fp->coll_data.ecbCurr_right.Y > 1.f)
        body_offset = fp->coll_data.ecbCurr_right.Y;
    ring_frames = 0;
    preview_fh = 0;
    preview_sh = 0;

    if (tracked_air)
    {
        Ecb_Record(fp, ts, frame_in_state);
        if (Options_Main[OPT_LOG].val)
        {
            if (!stage_logged)
            {
                Log_Stage();
                stage_logged = 1;
            }
            Log_Frame(fp, ts, frame_in_state);
        }

        Floor_BuildCache();
        SimStart start;
        Sim_FromFighter(fp, ts, frame_in_state, &start);
        Predict(fp, &start, pred_live, 1);
        Text_Prediction(pred_live);
        Text_Ai(pred_live);
        Timing_Update(fp, pred_live);

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
    else if (Ground_CanJump(sid) && !disturbed && !ghost_visible)
        Ground_Preview(fp);

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
    FighterData *fp = Fighter_GetGObj(0)->userdata;

    stc_matchcam->show_coll = value;
    fp->show_model = !value;
    fp->show_hit = value;
}

void Event_ClearLearned(GOBJ *menu)
{
    Learned_Clear();
    Learned_Bake();
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
