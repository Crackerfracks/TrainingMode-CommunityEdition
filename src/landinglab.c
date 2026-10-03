/* landinglab.c
 *
 * Landing Lab (Captain Falcon).
 *
 * While Falcon is airborne, this draws where he will touch down if the stick
 * stays where it is and no buttons are pressed. Only NILs, aerial interrupts
 * and perfect wavelands get color; other landings are a thin gray line.
 * Thick stretches of the path mark the frames where pressing an aerial
 * forces a touchdown (an aerial interrupt, pink, with arrows for the aerials
 * that do it) or where a sideways airdodge does (a perfect waveland, white).
 * A ring counts down to the next AI, arrows sliding in at his feet to the
 * next waveland. On the ground it previews a full hop and a short hop
 * pressed right now.
 *
 * An aerial interrupt: pressing an aerial swaps the ECB to the aerial's pose
 * on that same frame. If the new ECB bottom ends up at or below a floor, you
 * land right there. While the post-jump bottom lock is on, the swap waits
 * until the lock ends and uses whatever aerial frame you're on by then. A
 * perfect waveland is the same with an airdodge held fully sideways: it has
 * no vertical speed, so only the ECB swap can land it, and it lands with all
 * of the dodge's speed.
 *
 * Whether a landing is a NIL or an interrupt depends on the exact ECB on
 * each frame of each animation, which the game only computes on the frame
 * itself. Falcon's jumps, falls, aerials and airdodge are built in (baked
 * from logged sessions); everything else is learned while you play, keyed by
 * (state, frame in state). Parts of a prediction that rely on frames it hasn't seen
 * yet are drawn gray.
 *
 * The physics and collision are ports of the game's own code, checked
 * against the decomp: ft_80084DB0 (air physics), ftCommon_CalcSelfAccel_
 * DriftFrom (drift), mpColl_LoadECB (ECB bottom lock), mpColl_80046904's
 * wall pass (wall pushout), mpCheckFloor and mpLineIntersection(H) (floor
 * crossing), ft_80082B1C (NIL), ftCo_AttackAir_EnterFromMsid (aerial start),
 * ftCo_LandingAir_EnterWithLag (aerial lag, L-cancel), ftCo_80099A9C and
 * ftCo_EscapeAir_Phys (airdodge), and ftCo_KneeBend / ftCo_Jump_Enter
 * (jumps from the ground). Ceilings and ledge grabs aren't
 * simulated.
 */

#include "../MexTK/mex.h"
#include "events.h"

#define LL_SIM_FRAMES 240   // how far ahead to simulate
#define LL_STATE_FRAMES 128 // frames learned per state
#define LL_FALL_LOOP 8       // Falcon's Fall and FallAerial animations loop every 8 frames
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
#define COMMON_DODGE_DEADZONE 0x32C // Vec2: stick inside both is a neutral airdodge
#define COMMON_DODGE_FORCE 0x338    // float: airdodge starting speed
#define COMMON_DODGE_DECAY 0x33C    // float: airdodge speed multiplier per frame
#define COMMON_WAVELAND_LAG 0x344   // float: landing lag out of an airdodge
#define COMMON_FALL_LEAN_DEADZONE 0x444 // float: |x speed / drift max| above this leans the fall pose
#define COMMON_FALL_LEAN_RATE 0x448     // float: how fast the lean moves to its target each frame

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
    TS_ESCAPEAIR,

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
    ASID_ESCAPEAIR,
};

static const char *tracked_state_names[TS_COUNT] = {
    "JumpF", "JumpB", "DJumpF", "DJumpB", "Fall", "FallAerial",
    "Nair", "Fair", "Bair", "Uair", "Dair", "Airdodge",
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
    return ts >= TS_AIRN && ts <= TS_AIRLW;
}

// Jump, double jump and fall collision pass the drop-through callback
// (ftCo_80096CC8); aerials and the airdodge (ft_80081D0C) collide without it
// and always land on platforms.
static int Tracked_UsesPlatformDrop(int ts)
{
    return ts <= TS_FALLAERIAL;
}

// The state the game enters when this one's animation runs out, or -1 if it
// loops or isn't tracked. Jump and aerials go to Fall (ftCo_Fall_Enter),
// double jump goes to FallAerial (ftCo_FallAerial_Enter), the airdodge goes
// to FallSpecial, which isn't tracked. Once a state has been seen ending,
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
    LAND_PERFECT_WL, // a horizontal airdodge's ECB touched down the first time it was used
    LAND_WAVELAND,   // any other airdodge landing

    LAND_KIND_COUNT
};

static const char *land_kind_names[LAND_KIND_COUNT] = {
    "None", "NIL", "AI", "Land", "L-cancel", "Lag", "Other", "Perfect WL", "Waveland",
};

// The three things worth practicing each get a color no other cue uses:
// green for NIL, pink for aerial interrupts, ice white for perfect
// wavelands (Melee flashes a dodging fighter white). They also differ in
// shape: a NIL colors the whole path, an AI is a pink bar on it, a perfect
// waveland a wider white bar around it.
static const GXColor land_kind_colors[LAND_KIND_COUNT] = {
    {120, 160, 255, 255}, // none: blue
    {64, 255, 96, 255},   // NIL: green
    {255, 72, 196, 255},  // aerial interrupt: pink
    {255, 230, 0, 255},   // normal landing / auto-cancel: yellow
    {255, 140, 0, 255},   // L-cancel: orange
    {255, 48, 48, 255},   // full aerial lag: red
    {200, 200, 200, 255}, // other
    {205, 238, 255, 255}, // perfect waveland: ice white
    {200, 200, 200, 255}, // other waveland
};
static const GXColor color_learning = {130, 130, 130, 255};

// Only NILs, aerial interrupts and perfect wavelands are worth practicing
// toward.
static int Kind_Highlighted(int kind)
{
    return kind == LAND_NIL || kind == LAND_AI || kind == LAND_PERFECT_WL;
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
    if (ts == TS_FALL || ts == TS_FALLAERIAL)
        frame %= LL_FALL_LOOP; // the fall animations loop
    if (frame >= LL_STATE_FRAMES)
        frame = LL_STATE_FRAMES - 1;
    return &ecb_table[ts * LL_STATE_FRAMES + frame];
}

// Falcon's ECB on every frame of his jumps, double jumps, falls and aerials,
// from play sessions on a vanilla ISO (logged by v0.1 and v0.2,
// 2026-10-03). Bottoms are missing on the frames the post-jump lock hid
// them. The falls are an 8-frame loop each, from captures that kept Falcon's
// speed at 0, so they're the plain pose; drifting leans it (Lean_Ecb).
// The endings of the longer jumps and the aerials (frames the play sessions
// never reached) come from captures that ran each move out in the air
// (2026-10-03). The airdodge comes from a v0.4 session; its frames don't
// depend on the state it was pressed from.
#define BAKED_BOTTOM 1
#define BAKED_SHAPE 2
#define BAKED_LAG 4 // landing on this aerial frame takes aerial lag; on an
                    // airdodge frame, its speed no longer decays

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
    {TS_JUMPB, 35, BAKED_BOTTOM|BAKED_SHAPE, 2.792f, 9.931f, 6.362f, 4.641f, -4.641f},
    {TS_JUMPB, 36, BAKED_BOTTOM|BAKED_SHAPE, 2.533f, 10.236f, 6.384f, 3.524f, -3.524f},
    {TS_JUMPB, 37, BAKED_BOTTOM|BAKED_SHAPE, 2.335f, 10.302f, 6.319f, 2.872f, -2.872f},
    {TS_JUMPB, 38, BAKED_BOTTOM|BAKED_SHAPE, 2.159f, 10.036f, 6.097f, 2.830f, -2.830f},
    {TS_JUMPB, 39, BAKED_BOTTOM|BAKED_SHAPE, 2.004f, 9.963f, 5.983f, 2.762f, -2.762f},
    {TS_JUMPB, 40, BAKED_BOTTOM|BAKED_SHAPE, 1.927f, 9.775f, 5.851f, 2.678f, -2.678f},
    {TS_JUMPB, 41, BAKED_BOTTOM|BAKED_SHAPE, 1.875f, 10.092f, 5.984f, 2.558f, -2.558f},
    {TS_JUMPB, 42, BAKED_BOTTOM|BAKED_SHAPE, 1.845f, 10.459f, 6.152f, 2.564f, -2.564f},
    {TS_JUMPB, 43, BAKED_BOTTOM|BAKED_SHAPE, 1.828f, 10.674f, 6.251f, 3.231f, -3.231f},
    {TS_JUMPB, 44, BAKED_BOTTOM|BAKED_SHAPE, 1.817f, 10.991f, 6.404f, 4.016f, -4.016f},
    {TS_JUMPB, 45, BAKED_BOTTOM|BAKED_SHAPE, 1.808f, 11.074f, 6.441f, 4.312f, -4.312f},
    {TS_JUMPB, 46, BAKED_BOTTOM|BAKED_SHAPE, 1.875f, 11.003f, 6.439f, 4.367f, -4.367f},
    {TS_JUMPB, 47, BAKED_BOTTOM|BAKED_SHAPE, 1.927f, 10.906f, 6.416f, 4.267f, -4.267f},
    {TS_JUMPB, 48, BAKED_BOTTOM|BAKED_SHAPE, 1.963f, 10.827f, 6.395f, 4.155f, -4.155f},
    {TS_JUMPB, 49, BAKED_BOTTOM|BAKED_SHAPE, 1.987f, 10.784f, 6.385f, 4.000f, -4.000f},
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
    {TS_JUMPAERIALF, 48, BAKED_BOTTOM|BAKED_SHAPE, 3.268f, 11.036f, 7.152f, 6.224f, -3.884f},
    {TS_JUMPAERIALF, 49, BAKED_BOTTOM|BAKED_SHAPE, 2.995f, 11.156f, 7.076f, 6.289f, -4.527f},
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
    {TS_JUMPAERIALB, 32, BAKED_BOTTOM|BAKED_SHAPE, 3.516f, 11.443f, 7.479f, 4.433f, -4.433f},
    {TS_JUMPAERIALB, 33, BAKED_BOTTOM|BAKED_SHAPE, 3.087f, 11.720f, 7.403f, 4.870f, -4.870f},
    {TS_JUMPAERIALB, 34, BAKED_BOTTOM|BAKED_SHAPE, 2.797f, 11.617f, 7.207f, 6.079f, -4.420f},
    // BEGIN generated fall data (lean captures, 2026-10-03)
    {TS_FALL, 0, BAKED_BOTTOM|BAKED_SHAPE, 1.9980f, 10.7793f, 6.3886f, 3.9122f, -3.9122f},
    {TS_FALL, 1, BAKED_BOTTOM|BAKED_SHAPE, 2.0777f, 10.7642f, 6.4209f, 4.0309f, -4.0309f},
    {TS_FALL, 2, BAKED_BOTTOM|BAKED_SHAPE, 2.4055f, 11.0242f, 6.7149f, 4.2774f, -4.2774f},
    {TS_FALL, 3, BAKED_BOTTOM|BAKED_SHAPE, 2.7215f, 11.2778f, 6.9996f, 4.4340f, -4.4340f},
    {TS_FALL, 4, BAKED_BOTTOM|BAKED_SHAPE, 2.7574f, 11.2699f, 7.0137f, 4.4809f, -4.4809f},
    {TS_FALL, 5, BAKED_BOTTOM|BAKED_SHAPE, 2.5889f, 11.2180f, 6.9035f, 4.5836f, -4.5836f},
    {TS_FALL, 6, BAKED_BOTTOM|BAKED_SHAPE, 2.3175f, 11.0125f, 6.6650f, 4.5531f, -4.5531f},
    {TS_FALL, 7, BAKED_BOTTOM|BAKED_SHAPE, 2.1016f, 10.7649f, 6.4332f, 4.1938f, -4.1938f},
    {TS_FALLAERIAL, 0, BAKED_BOTTOM|BAKED_SHAPE, 2.7303f, 11.2273f, 6.9788f, 6.3030f, -4.7787f},
    {TS_FALLAERIAL, 1, BAKED_BOTTOM|BAKED_SHAPE, 2.8241f, 11.4565f, 7.1403f, 6.5248f, -4.7942f},
    {TS_FALLAERIAL, 2, BAKED_BOTTOM|BAKED_SHAPE, 3.2168f, 11.6490f, 7.4329f, 6.8133f, -4.8704f},
    {TS_FALLAERIAL, 3, BAKED_BOTTOM|BAKED_SHAPE, 3.6513f, 11.6814f, 7.6664f, 6.7157f, -4.9615f},
    {TS_FALLAERIAL, 4, BAKED_BOTTOM|BAKED_SHAPE, 3.8598f, 11.5121f, 7.6859f, 6.5846f, -4.9643f},
    {TS_FALLAERIAL, 5, BAKED_BOTTOM|BAKED_SHAPE, 4.0346f, 11.5246f, 7.7796f, 6.5223f, -4.9538f},
    {TS_FALLAERIAL, 6, BAKED_BOTTOM|BAKED_SHAPE, 3.9235f, 11.3793f, 7.6514f, 6.4291f, -4.8820f},
    {TS_FALLAERIAL, 7, BAKED_BOTTOM|BAKED_SHAPE, 3.1963f, 11.2052f, 7.2007f, 6.3251f, -4.8031f},
    // END generated fall data
    {TS_AIRN, 0, BAKED_BOTTOM|BAKED_SHAPE, 2.407f, 12.501f, 7.454f, 2.714f, -2.714f},
    {TS_AIRN, 1, BAKED_BOTTOM|BAKED_SHAPE, 2.703f, 12.960f, 7.832f, 2.152f, -2.152f},
    {TS_AIRN, 2, BAKED_BOTTOM|BAKED_SHAPE, 2.091f, 12.592f, 7.341f, 4.900f, -4.900f},
    {TS_AIRN, 3, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 1.716f, 14.711f, 8.214f, 3.367f, -3.367f},
    {TS_AIRN, 4, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 2.001f, 15.100f, 8.550f, 2.525f, -2.525f},
    {TS_AIRN, 5, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 3.317f, 14.831f, 9.074f, 3.585f, -3.585f},
    {TS_AIRN, 6, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 4.923f, 14.157f, 9.540f, 6.659f, -3.876f},
    {TS_AIRN, 7, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 4.949f, 14.346f, 9.648f, 6.693f, -3.723f},
    {TS_AIRN, 8, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 4.846f, 14.477f, 9.662f, 6.621f, -3.581f},
    {TS_AIRN, 9, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 4.771f, 14.535f, 9.653f, 4.999f, -4.999f},
    {TS_AIRN, 10, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 4.753f, 14.550f, 9.652f, 4.777f, -4.777f},
    {TS_AIRN, 11, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 4.800f, 14.574f, 9.687f, 4.498f, -4.498f},
    {TS_AIRN, 12, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 5.195f, 14.680f, 9.938f, 4.540f, -4.540f},
    {TS_AIRN, 13, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 4.243f, 15.036f, 9.639f, 3.526f, -3.526f},
    {TS_AIRN, 14, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 4.295f, 14.949f, 9.622f, 3.658f, -3.658f},
    {TS_AIRN, 15, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 4.250f, 13.516f, 8.883f, 4.923f, -4.923f},
    {TS_AIRN, 16, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 4.034f, 10.896f, 7.465f, 6.156f, -4.251f},
    {TS_AIRN, 17, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 4.204f, 10.641f, 7.423f, 2.975f, -2.975f},
    {TS_AIRN, 18, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 5.279f, 10.554f, 7.916f, 4.283f, -4.283f},
    {TS_AIRN, 19, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 4.360f, 10.493f, 7.427f, 4.767f, -4.767f},
    {TS_AIRN, 20, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 3.987f, 10.495f, 7.241f, 4.888f, -4.888f},
    {TS_AIRN, 21, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 3.633f, 10.500f, 7.067f, 4.971f, -4.971f},
    {TS_AIRN, 22, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 3.301f, 10.509f, 6.905f, 6.618f, -3.429f},
    {TS_AIRN, 23, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 2.991f, 10.515f, 6.753f, 6.576f, -3.604f},
    {TS_AIRN, 24, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 2.706f, 10.509f, 6.608f, 6.515f, -3.775f},
    {TS_AIRN, 25, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 2.446f, 10.496f, 6.471f, 6.436f, -3.932f},
    {TS_AIRN, 26, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 2.214f, 10.485f, 6.349f, 6.340f, -4.072f},
    {TS_AIRN, 27, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 2.011f, 10.479f, 6.245f, 6.230f, -4.197f},
    {TS_AIRN, 28, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 1.837f, 10.478f, 6.158f, 6.105f, -4.307f},
    {TS_AIRN, 29, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 1.696f, 10.485f, 6.090f, 5.968f, -4.402f},
    {TS_AIRN, 30, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 1.440f, 10.754f, 6.097f, 4.144f, -4.144f},
    {TS_AIRN, 31, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 1.789f, 10.941f, 6.365f, 2.756f, -2.756f},
    {TS_AIRN, 32, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 2.706f, 11.525f, 7.115f, 2.769f, -2.769f},
    {TS_AIRN, 33, BAKED_BOTTOM|BAKED_SHAPE, 3.630f, 12.873f, 8.252f, 4.346f, -4.346f},
    {TS_AIRN, 34, BAKED_BOTTOM|BAKED_SHAPE, 4.059f, 12.879f, 8.469f, 4.818f, -5.452f},
    {TS_AIRN, 35, BAKED_BOTTOM|BAKED_SHAPE, 4.121f, 12.526f, 8.323f, 3.903f, -3.903f},
    {TS_AIRN, 36, BAKED_BOTTOM|BAKED_SHAPE, 4.434f, 13.278f, 8.856f, 3.689f, -3.689f},
    {TS_AIRN, 37, BAKED_BOTTOM|BAKED_SHAPE, 4.581f, 13.158f, 8.870f, 4.508f, -4.508f},
    {TS_AIRN, 38, BAKED_BOTTOM|BAKED_SHAPE, 4.559f, 12.388f, 8.474f, 7.172f, -4.519f},
    {TS_AIRN, 39, BAKED_BOTTOM|BAKED_SHAPE, 4.440f, 11.873f, 8.156f, 7.400f, -4.104f},
    {TS_AIRN, 40, BAKED_BOTTOM|BAKED_SHAPE, 4.309f, 11.662f, 7.985f, 8.230f, -3.371f},
    {TS_AIRN, 41, BAKED_BOTTOM|BAKED_SHAPE, 3.533f, 11.423f, 7.478f, 9.337f, -2.718f},
    {TS_AIRN, 42, BAKED_BOTTOM|BAKED_SHAPE, 2.708f, 11.187f, 6.947f, 8.444f, -2.273f},
    {TS_AIRN, 43, BAKED_BOTTOM|BAKED_SHAPE, 2.196f, 10.972f, 6.584f, 4.362f, -4.362f},
    {TS_AIRF, 0, BAKED_BOTTOM|BAKED_SHAPE, 2.884f, 10.539f, 6.711f, 3.864f, -3.864f},
    {TS_AIRF, 1, BAKED_BOTTOM|BAKED_SHAPE, 4.539f, 10.217f, 7.378f, 3.762f, -3.762f},
    {TS_AIRF, 2, BAKED_BOTTOM|BAKED_SHAPE, 5.339f, 11.564f, 8.451f, 4.344f, -4.344f},
    {TS_AIRF, 3, BAKED_BOTTOM|BAKED_SHAPE, 6.095f, 10.655f, 8.375f, 4.152f, -4.152f},
    {TS_AIRF, 4, BAKED_BOTTOM|BAKED_SHAPE, 4.685f, 10.500f, 7.593f, 3.711f, -3.711f},
    {TS_AIRF, 5, BAKED_BOTTOM|BAKED_SHAPE, 3.970f, 10.332f, 7.152f, 3.367f, -3.367f},
    {TS_AIRF, 6, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 3.323f, 10.213f, 6.768f, 3.194f, -3.194f},
    {TS_AIRF, 7, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 2.846f, 10.217f, 6.532f, 3.141f, -3.141f},
    {TS_AIRF, 8, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 2.551f, 10.405f, 6.478f, 3.135f, -3.135f},
    {TS_AIRF, 9, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 2.493f, 12.263f, 7.378f, 3.163f, -3.163f},
    {TS_AIRF, 10, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 1.469f, 13.705f, 7.587f, 2.521f, -2.521f},
    {TS_AIRF, 11, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 1.545f, 12.978f, 7.261f, 3.009f, -3.009f},
    {TS_AIRF, 12, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 1.732f, 12.920f, 7.326f, 4.176f, -4.176f},
    {TS_AIRF, 13, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 1.677f, 12.843f, 7.260f, 4.496f, -4.496f},
    {TS_AIRF, 14, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 1.675f, 12.793f, 7.234f, 4.551f, -4.551f},
    {TS_AIRF, 15, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 2.280f, 12.783f, 7.531f, 4.869f, -4.869f},
    {TS_AIRF, 16, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 2.440f, 12.796f, 7.618f, 7.186f, -2.836f},
    {TS_AIRF, 17, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 2.320f, 12.800f, 7.560f, 4.984f, -4.984f},
    {TS_AIRF, 18, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 1.859f, 12.767f, 7.313f, 4.962f, -4.962f},
    {TS_AIRF, 19, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 2.263f, 12.722f, 7.493f, 6.285f, -3.902f},
    {TS_AIRF, 20, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 2.286f, 12.671f, 7.479f, 6.543f, -3.856f},
    {TS_AIRF, 21, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 2.333f, 12.610f, 7.472f, 6.776f, -3.805f},
    {TS_AIRF, 22, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 2.391f, 12.545f, 7.468f, 6.978f, -3.762f},
    {TS_AIRF, 23, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 2.448f, 12.483f, 7.465f, 7.150f, -3.739f},
    {TS_AIRF, 24, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 2.496f, 12.430f, 7.463f, 7.293f, -3.733f},
    {TS_AIRF, 25, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 2.531f, 12.395f, 7.463f, 7.406f, -3.737f},
    {TS_AIRF, 26, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 2.552f, 12.386f, 7.469f, 7.490f, -3.715f},
    {TS_AIRF, 27, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 2.484f, 12.407f, 7.445f, 7.544f, -3.621f},
    {TS_AIRF, 28, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 2.432f, 12.461f, 7.447f, 7.531f, -3.084f},
    {TS_AIRF, 29, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 2.436f, 12.672f, 7.554f, 7.425f, -3.029f},
    {TS_AIRF, 30, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 1.750f, 12.489f, 7.120f, 7.304f, -3.506f},
    {TS_AIRF, 31, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 1.260f, 12.138f, 6.699f, 7.014f, -3.507f},
    {TS_AIRF, 32, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 1.313f, 12.162f, 6.737f, 6.832f, -3.262f},
    {TS_AIRF, 33, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 1.934f, 11.999f, 6.966f, 6.685f, -3.473f},
    {TS_AIRF, 34, BAKED_BOTTOM|BAKED_SHAPE, 2.842f, 11.644f, 7.243f, 6.433f, -3.778f},
    {TS_AIRF, 35, BAKED_BOTTOM|BAKED_SHAPE, 3.281f, 11.216f, 7.248f, 6.543f, -3.730f},
    {TS_AIRF, 36, BAKED_BOTTOM|BAKED_SHAPE, 3.445f, 10.893f, 7.169f, 6.741f, -3.272f},
    {TS_AIRF, 37, BAKED_BOTTOM|BAKED_SHAPE, 2.646f, 10.764f, 6.705f, 4.510f, -4.510f},
    {TS_AIRF, 38, BAKED_BOTTOM|BAKED_SHAPE, 2.146f, 10.755f, 6.450f, 4.054f, -4.054f},
    {TS_AIRB, 0, BAKED_BOTTOM|BAKED_SHAPE, 2.558f, 11.932f, 7.245f, 4.712f, -4.712f},
    {TS_AIRB, 1, BAKED_BOTTOM|BAKED_SHAPE, 1.592f, 12.240f, 6.916f, 4.757f, -4.757f},
    {TS_AIRB, 2, BAKED_BOTTOM|BAKED_SHAPE, 1.161f, 12.218f, 6.689f, 4.767f, -4.767f},
    {TS_AIRB, 3, BAKED_BOTTOM|BAKED_SHAPE, 1.165f, 12.116f, 6.641f, 4.648f, -4.648f},
    {TS_AIRB, 4, BAKED_BOTTOM|BAKED_SHAPE, 1.171f, 12.060f, 6.616f, 4.357f, -4.357f},
    {TS_AIRB, 5, BAKED_BOTTOM|BAKED_SHAPE, 1.171f, 12.324f, 6.748f, 3.871f, -3.871f},
    {TS_AIRB, 6, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 1.159f, 13.464f, 7.312f, 3.031f, -3.031f},
    {TS_AIRB, 7, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 1.464f, 15.448f, 8.456f, 2.0f, -2.0f},
    {TS_AIRB, 8, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 2.166f, 14.141f, 8.154f, 4.279f, -4.279f},
    {TS_AIRB, 9, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 2.680f, 14.822f, 8.751f, 2.021f, -11.766f},
    {TS_AIRB, 10, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 2.628f, 14.902f, 8.765f, 2.020f, -10.823f},
    {TS_AIRB, 11, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 2.599f, 15.045f, 8.822f, 2.031f, -11.389f},
    {TS_AIRB, 12, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 2.590f, 15.173f, 8.882f, 2.053f, -10.925f},
    {TS_AIRB, 13, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 2.602f, 15.318f, 8.960f, 2.088f, -10.943f},
    {TS_AIRB, 14, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 2.632f, 15.499f, 9.066f, 2.135f, -10.909f},
    {TS_AIRB, 15, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 2.673f, 15.688f, 9.181f, 2.184f, -10.879f},
    {TS_AIRB, 16, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 2.715f, 15.875f, 9.295f, 2.227f, -10.846f},
    {TS_AIRB, 17, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 2.749f, 16.052f, 9.400f, 2.252f, -10.806f},
    {TS_AIRB, 18, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 2.775f, 16.210f, 9.492f, 2.256f, -10.764f},
    {TS_AIRB, 19, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 2.796f, 16.339f, 9.568f, 2.237f, -10.803f},
    {TS_AIRB, 20, BAKED_BOTTOM|BAKED_SHAPE, 2.810f, 16.420f, 9.615f, 2.186f, -10.825f},
    {TS_AIRB, 21, BAKED_BOTTOM|BAKED_SHAPE, 2.813f, 16.415f, 9.614f, 2.093f, -10.934f},
    {TS_AIRB, 22, BAKED_BOTTOM|BAKED_SHAPE, 2.893f, 16.107f, 9.500f, 2.131f, -9.273f},
    {TS_AIRB, 23, BAKED_BOTTOM|BAKED_SHAPE, 3.123f, 16.108f, 9.615f, 2.881f, -2.881f},
    {TS_AIRB, 24, BAKED_BOTTOM|BAKED_SHAPE, 3.462f, 16.464f, 9.963f, 2.794f, -2.794f},
    {TS_AIRB, 25, BAKED_BOTTOM|BAKED_SHAPE, 3.869f, 16.920f, 10.395f, 3.249f, -3.249f},
    {TS_AIRB, 26, BAKED_BOTTOM|BAKED_SHAPE, 3.587f, 16.409f, 9.998f, 3.557f, -3.557f},
    {TS_AIRB, 27, BAKED_BOTTOM|BAKED_SHAPE, 2.739f, 15.176f, 8.957f, 3.708f, -3.708f},
    {TS_AIRB, 28, BAKED_BOTTOM|BAKED_SHAPE, 2.148f, 14.299f, 8.223f, 4.500f, -4.500f},
    {TS_AIRB, 29, BAKED_BOTTOM|BAKED_SHAPE, 1.775f, 14.166f, 7.970f, 4.444f, -5.870f},
    {TS_AIRB, 30, BAKED_BOTTOM|BAKED_SHAPE, 1.702f, 14.017f, 7.859f, 5.158f, -4.930f},
    {TS_AIRB, 31, BAKED_BOTTOM|BAKED_SHAPE, 1.705f, 13.227f, 7.466f, 4.467f, -4.467f},
    {TS_AIRB, 32, BAKED_BOTTOM|BAKED_SHAPE, 1.774f, 12.002f, 6.888f, 4.384f, -4.384f},
    {TS_AIRB, 33, BAKED_BOTTOM|BAKED_SHAPE, 1.878f, 10.854f, 6.366f, 4.141f, -4.141f},
    {TS_AIRB, 34, BAKED_BOTTOM|BAKED_SHAPE, 1.973f, 10.850f, 6.412f, 3.966f, -3.966f},
    {TS_AIRHI, 0, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 2.650f, 8.222f, 5.436f, 2.885f, -2.885f},
    {TS_AIRHI, 1, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 2.432f, 10.995f, 6.714f, 3.324f, -3.324f},
    {TS_AIRHI, 2, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 3.196f, 18.670f, 10.933f, 3.114f, -3.114f},
    {TS_AIRHI, 3, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 4.246f, 12.944f, 8.595f, 4.698f, -4.698f},
    {TS_AIRHI, 4, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 5.428f, 11.691f, 8.559f, 4.763f, -4.763f},
    {TS_AIRHI, 5, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 5.178f, 10.608f, 7.893f, 4.989f, -4.989f},
    {TS_AIRHI, 6, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 5.014f, 12.867f, 8.941f, 4.736f, -4.736f},
    {TS_AIRHI, 7, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 4.967f, 15.232f, 10.100f, 4.320f, -4.320f},
    {TS_AIRHI, 8, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 4.922f, 16.616f, 10.769f, 4.273f, -4.273f},
    {TS_AIRHI, 9, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 4.891f, 16.915f, 10.903f, 4.170f, -4.170f},
    {TS_AIRHI, 10, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 4.938f, 16.044f, 10.491f, 4.060f, -4.060f},
    {TS_AIRHI, 11, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 5.051f, 15.390f, 10.220f, 4.119f, -4.119f},
    {TS_AIRHI, 12, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 5.208f, 16.120f, 10.664f, 4.266f, -4.266f},
    {TS_AIRHI, 13, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 5.466f, 16.064f, 10.765f, 4.640f, -4.640f},
    {TS_AIRHI, 14, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 5.906f, 14.871f, 10.389f, 4.800f, -4.800f},
    {TS_AIRHI, 15, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 5.971f, 12.606f, 9.288f, 4.909f, -4.909f},
    {TS_AIRHI, 16, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 4.568f, 11.183f, 7.875f, 3.560f, -7.331f},
    {TS_AIRHI, 17, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 3.369f, 11.742f, 7.556f, 3.804f, -6.988f},
    {TS_AIRHI, 18, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 3.638f, 12.146f, 7.892f, 4.836f, -4.836f},
    {TS_AIRHI, 19, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 3.528f, 12.536f, 8.032f, 4.054f, -4.054f},
    {TS_AIRHI, 20, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 2.898f, 12.835f, 7.867f, 3.453f, -3.453f},
    {TS_AIRHI, 21, BAKED_BOTTOM|BAKED_SHAPE, 2.678f, 13.149f, 7.914f, 3.061f, -3.061f},
    {TS_AIRHI, 22, BAKED_BOTTOM|BAKED_SHAPE, 2.555f, 13.664f, 8.110f, 2.727f, -2.727f},
    {TS_AIRHI, 23, BAKED_BOTTOM|BAKED_SHAPE, 2.508f, 14.022f, 8.265f, 3.032f, -3.032f},
    {TS_AIRHI, 24, BAKED_BOTTOM|BAKED_SHAPE, 2.544f, 13.970f, 8.257f, 3.584f, -3.584f},
    {TS_AIRHI, 25, BAKED_BOTTOM|BAKED_SHAPE, 2.686f, 13.554f, 8.120f, 3.833f, -3.833f},
    {TS_AIRHI, 26, BAKED_BOTTOM|BAKED_SHAPE, 2.993f, 13.006f, 8.000f, 4.142f, -4.142f},
    {TS_AIRHI, 27, BAKED_BOTTOM|BAKED_SHAPE, 3.365f, 12.331f, 7.848f, 4.197f, -4.197f},
    {TS_AIRHI, 28, BAKED_BOTTOM|BAKED_SHAPE, 3.338f, 11.654f, 7.496f, 4.170f, -4.170f},
    {TS_AIRHI, 29, BAKED_BOTTOM|BAKED_SHAPE, 2.975f, 11.052f, 7.013f, 4.090f, -4.090f},
    {TS_AIRHI, 30, BAKED_BOTTOM|BAKED_SHAPE, 2.657f, 10.750f, 6.704f, 4.066f, -4.066f},
    {TS_AIRHI, 31, BAKED_BOTTOM|BAKED_SHAPE, 2.405f, 10.764f, 6.585f, 3.954f, -3.954f},
    {TS_AIRHI, 32, BAKED_BOTTOM|BAKED_SHAPE, 2.230f, 10.765f, 6.498f, 3.881f, -3.881f},
    {TS_AIRLW, 0, BAKED_BOTTOM|BAKED_SHAPE, 3.531f, 10.922f, 7.226f, 4.141f, -4.141f},
    {TS_AIRLW, 1, BAKED_BOTTOM|BAKED_SHAPE, 5.875f, 12.150f, 9.013f, 4.449f, -4.449f},
    {TS_AIRLW, 2, BAKED_BOTTOM|BAKED_SHAPE, 5.816f, 12.326f, 9.071f, 4.128f, -4.128f},
    {TS_AIRLW, 3, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 5.810f, 12.389f, 9.100f, 4.101f, -4.101f},
    {TS_AIRLW, 4, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 5.807f, 12.408f, 9.107f, 4.111f, -4.111f},
    {TS_AIRLW, 5, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 5.808f, 12.402f, 9.105f, 4.128f, -4.128f},
    {TS_AIRLW, 6, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 5.813f, 12.386f, 9.099f, 4.146f, -4.146f},
    {TS_AIRLW, 7, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 5.823f, 12.368f, 9.095f, 4.165f, -4.165f},
    {TS_AIRLW, 8, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 5.839f, 12.359f, 9.099f, 4.188f, -4.188f},
    {TS_AIRLW, 9, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 5.863f, 12.356f, 9.110f, 4.210f, -4.210f},
    {TS_AIRLW, 10, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 5.895f, 12.356f, 9.126f, 4.225f, -4.225f},
    {TS_AIRLW, 11, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 5.938f, 12.366f, 9.152f, 4.235f, -4.235f},
    {TS_AIRLW, 12, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 5.984f, 12.399f, 9.192f, 4.237f, -4.237f},
    {TS_AIRLW, 13, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 5.329f, 13.195f, 9.262f, 4.054f, -4.054f},
    {TS_AIRLW, 14, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 3.296f, 15.551f, 9.424f, 3.468f, -7.317f},
    {TS_AIRLW, 15, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 1.737f, 11.774f, 6.755f, 4.461f, -4.461f},
    {TS_AIRLW, 16, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 0.347f, 11.301f, 5.824f, 4.380f, -4.380f},
    {TS_AIRLW, 17, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 0.834f, 11.154f, 5.994f, 4.424f, -4.424f},
    {TS_AIRLW, 18, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 1.435f, 11.568f, 6.501f, 4.319f, -4.319f},
    {TS_AIRLW, 19, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 1.827f, 13.237f, 7.532f, 4.733f, -4.733f},
    {TS_AIRLW, 20, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 3.109f, 16.144f, 9.627f, 4.262f, -6.280f},
    {TS_AIRLW, 21, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 3.982f, 17.853f, 10.918f, 4.782f, -4.782f},
    {TS_AIRLW, 22, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 4.293f, 18.321f, 11.307f, 4.420f, -4.420f},
    {TS_AIRLW, 23, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 4.549f, 18.545f, 11.547f, 4.090f, -4.090f},
    {TS_AIRLW, 24, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 4.753f, 18.616f, 11.684f, 3.858f, -3.858f},
    {TS_AIRLW, 25, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 4.911f, 18.668f, 11.790f, 3.753f, -3.753f},
    {TS_AIRLW, 26, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 5.027f, 18.664f, 11.846f, 3.687f, -3.687f},
    {TS_AIRLW, 27, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 5.109f, 18.575f, 11.842f, 3.553f, -3.553f},
    {TS_AIRLW, 28, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 5.188f, 18.402f, 11.795f, 3.421f, -3.421f},
    {TS_AIRLW, 29, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 5.257f, 18.039f, 11.648f, 3.151f, -3.151f},
    {TS_AIRLW, 30, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 5.313f, 17.438f, 11.376f, 2.805f, -2.805f},
    {TS_AIRLW, 31, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 5.391f, 16.684f, 11.037f, 2.358f, -2.358f},
    {TS_AIRLW, 32, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 5.504f, 15.330f, 10.417f, 2.291f, -2.291f},
    {TS_AIRLW, 33, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 5.714f, 13.285f, 9.499f, 2.740f, -2.740f},
    {TS_AIRLW, 34, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 5.912f, 11.067f, 8.489f, 2.778f, -2.778f},
    {TS_AIRLW, 35, BAKED_BOTTOM|BAKED_SHAPE, 6.096f, 10.020f, 8.058f, 2.702f, -2.702f},
    {TS_AIRLW, 36, BAKED_BOTTOM|BAKED_SHAPE, 5.595f, 9.261f, 7.428f, 2.292f, -2.292f},
    {TS_AIRLW, 37, BAKED_BOTTOM|BAKED_SHAPE, 4.176f, 7.485f, 5.830f, 2.218f, -2.218f},
    {TS_AIRLW, 38, BAKED_BOTTOM|BAKED_SHAPE, 3.560f, 7.763f, 5.661f, 2.465f, -2.465f},
    {TS_AIRLW, 39, BAKED_BOTTOM|BAKED_SHAPE, 4.538f, 9.678f, 7.108f, 2.000f, -2.000f},
    {TS_AIRLW, 40, BAKED_BOTTOM|BAKED_SHAPE, 4.566f, 8.717f, 6.642f, 2.921f, -2.921f},
    {TS_AIRLW, 41, BAKED_BOTTOM|BAKED_SHAPE, 4.111f, 11.395f, 7.753f, 2.251f, -2.251f},
    {TS_AIRLW, 42, BAKED_BOTTOM|BAKED_SHAPE, 6.350f, 11.134f, 8.742f, 4.147f, -4.147f},
    {TS_AIRLW, 43, BAKED_BOTTOM|BAKED_SHAPE, 3.526f, 10.804f, 7.165f, 4.211f, -4.211f},
    {TS_ESCAPEAIR, 0, BAKED_BOTTOM|BAKED_SHAPE, 1.7125f, 11.2112f, 6.4618f, 2.1920f, -2.1920f},
    {TS_ESCAPEAIR, 1, BAKED_BOTTOM|BAKED_SHAPE, 1.5205f, 12.0751f, 6.7978f, 2.7077f, -2.7077f},
    {TS_ESCAPEAIR, 2, BAKED_BOTTOM|BAKED_SHAPE, 1.5984f, 12.7514f, 7.1749f, 3.9936f, -3.9936f},
    {TS_ESCAPEAIR, 3, BAKED_BOTTOM|BAKED_SHAPE, 1.8968f, 13.0760f, 7.4864f, 3.5424f, -3.5424f},
    {TS_ESCAPEAIR, 4, BAKED_BOTTOM|BAKED_SHAPE, 2.0405f, 13.1340f, 7.5872f, 3.4526f, -3.4526f},
    {TS_ESCAPEAIR, 5, BAKED_BOTTOM|BAKED_SHAPE, 2.0366f, 13.0670f, 7.5518f, 3.4632f, -3.4632f},
    {TS_ESCAPEAIR, 6, BAKED_BOTTOM|BAKED_SHAPE, 2.0215f, 13.0307f, 7.5261f, 3.4800f, -3.4800f},
    {TS_ESCAPEAIR, 7, BAKED_BOTTOM|BAKED_SHAPE, 1.9961f, 13.0072f, 7.5016f, 3.4972f, -3.4972f},
    {TS_ESCAPEAIR, 8, BAKED_BOTTOM|BAKED_SHAPE, 1.9618f, 12.9879f, 7.4749f, 3.5085f, -3.5085f},
    {TS_ESCAPEAIR, 9, BAKED_BOTTOM|BAKED_SHAPE, 1.9202f, 12.9719f, 7.4461f, 3.5073f, -3.5073f},
    {TS_ESCAPEAIR, 10, BAKED_BOTTOM|BAKED_SHAPE, 1.8727f, 12.9581f, 7.4154f, 3.4955f, -3.4955f},
    {TS_ESCAPEAIR, 11, BAKED_BOTTOM|BAKED_SHAPE, 1.8210f, 12.9497f, 7.3853f, 3.4806f, -3.4806f},
    {TS_ESCAPEAIR, 12, BAKED_BOTTOM|BAKED_SHAPE, 1.7665f, 12.9486f, 7.3576f, 3.4634f, -3.4634f},
    {TS_ESCAPEAIR, 13, BAKED_BOTTOM|BAKED_SHAPE, 1.7108f, 12.9507f, 7.3308f, 3.4433f, -3.4433f},
    {TS_ESCAPEAIR, 14, BAKED_BOTTOM|BAKED_SHAPE, 1.6552f, 12.9550f, 7.3051f, 3.4210f, -3.4210f},
    {TS_ESCAPEAIR, 15, BAKED_BOTTOM|BAKED_SHAPE, 1.6009f, 12.9604f, 7.2807f, 3.3973f, -3.3973f},
    {TS_ESCAPEAIR, 16, BAKED_BOTTOM|BAKED_SHAPE, 1.5491f, 12.9663f, 7.2577f, 3.3730f, -3.3730f},
    {TS_ESCAPEAIR, 17, BAKED_BOTTOM|BAKED_SHAPE, 1.5007f, 12.9723f, 7.2365f, 3.3488f, -3.3488f},
    {TS_ESCAPEAIR, 18, BAKED_BOTTOM|BAKED_SHAPE, 1.4558f, 12.9780f, 7.2169f, 3.3251f, -3.3251f},
    {TS_ESCAPEAIR, 19, BAKED_BOTTOM|BAKED_SHAPE, 1.4139f, 12.9834f, 7.1986f, 3.3010f, -3.3010f},
    {TS_ESCAPEAIR, 20, BAKED_BOTTOM|BAKED_SHAPE, 1.3750f, 12.9895f, 7.1822f, 3.2761f, -3.2761f},
    {TS_ESCAPEAIR, 21, BAKED_BOTTOM|BAKED_SHAPE, 1.3390f, 12.9976f, 7.1683f, 3.2499f, -3.2499f},
    {TS_ESCAPEAIR, 22, BAKED_BOTTOM|BAKED_SHAPE, 1.3064f, 13.0088f, 7.1576f, 3.2219f, -3.2219f},
    {TS_ESCAPEAIR, 23, BAKED_BOTTOM|BAKED_SHAPE, 1.2777f, 13.0239f, 7.1508f, 3.1916f, -3.1916f},
    {TS_ESCAPEAIR, 24, BAKED_BOTTOM|BAKED_SHAPE, 1.2541f, 13.0509f, 7.1525f, 3.1586f, -3.1586f},
    {TS_ESCAPEAIR, 25, BAKED_BOTTOM|BAKED_SHAPE, 1.2452f, 13.0902f, 7.1677f, 3.1588f, -3.1588f},
    {TS_ESCAPEAIR, 26, BAKED_BOTTOM|BAKED_SHAPE, 1.2525f, 13.1428f, 7.1976f, 3.1434f, -3.1434f},
    {TS_ESCAPEAIR, 27, BAKED_BOTTOM|BAKED_SHAPE, 1.2625f, 13.1851f, 7.2238f, 3.0896f, -3.0896f},
    {TS_ESCAPEAIR, 28, BAKED_BOTTOM|BAKED_SHAPE, 1.2418f, 13.1735f, 7.2076f, 2.9275f, -2.9275f},
    {TS_ESCAPEAIR, 29, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 1.1859f, 13.0525f, 7.1192f, 3.1029f, -3.1029f},
    {TS_ESCAPEAIR, 30, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 1.0186f, 12.7096f, 6.8641f, 3.7882f, -3.7882f},
    {TS_ESCAPEAIR, 31, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 0.7817f, 11.8884f, 6.3350f, 4.0406f, -4.0406f},
    {TS_ESCAPEAIR, 32, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 0.6813f, 11.2458f, 5.9635f, 3.7472f, -3.7472f},
    {TS_ESCAPEAIR, 33, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 0.6630f, 10.6904f, 5.6767f, 4.3554f, -4.3554f},
    {TS_ESCAPEAIR, 34, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 0.7088f, 10.3973f, 5.5530f, 4.3448f, -4.3448f},
    {TS_ESCAPEAIR, 35, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 0.7792f, 10.2622f, 5.5207f, 4.5438f, -4.5438f},
    {TS_ESCAPEAIR, 36, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 0.8813f, 10.5350f, 5.7082f, 5.4860f, -5.2777f},
    {TS_ESCAPEAIR, 37, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 1.3755f, 10.9383f, 6.1569f, 6.7767f, -5.6612f},
    {TS_ESCAPEAIR, 38, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 1.1522f, 11.2475f, 6.1998f, 7.2082f, -6.0917f},
    {TS_ESCAPEAIR, 39, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 0.8826f, 11.2995f, 6.0910f, 7.2594f, -5.9332f},
    {TS_ESCAPEAIR, 40, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 0.9024f, 11.3121f, 6.1073f, 7.3281f, -5.9779f},
    {TS_ESCAPEAIR, 41, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 1.2359f, 11.3083f, 6.2721f, 7.4785f, -5.8770f},
    {TS_ESCAPEAIR, 42, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 1.5895f, 11.2966f, 6.4431f, 7.6400f, -5.4653f},
    {TS_ESCAPEAIR, 43, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 1.9857f, 11.2457f, 6.6157f, 7.9126f, -4.8335f},
    {TS_ESCAPEAIR, 44, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 1.8570f, 11.1147f, 6.4858f, 8.0217f, -4.3561f},
    {TS_ESCAPEAIR, 45, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 1.8211f, 11.7621f, 6.7916f, 7.8098f, -3.8454f},
    {TS_ESCAPEAIR, 46, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 1.9671f, 12.5610f, 7.2640f, 7.5193f, -3.2823f},
    {TS_ESCAPEAIR, 47, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 2.1325f, 13.4229f, 7.7777f, 7.4470f, -2.5792f},
    {TS_ESCAPEAIR, 48, BAKED_BOTTOM|BAKED_SHAPE|BAKED_LAG, 2.2812f, 14.2740f, 8.2776f, 4.4994f, -4.4994f},
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
        e->aerial_lag = (b->flags & BAKED_LAG) != 0;
        e->seen = 1;
    }

    // how long each runs before ending by itself, from captures run to the
    // end (2026-10-03); aerials end in Fall, double jumps in FallAerial
    static const s8 baked_len[][2] = {
        {TS_JUMPF, 35},
        {TS_JUMPB, 50},
        {TS_JUMPAERIALF, 50},
        {TS_JUMPAERIALB, 35},
        {TS_AIRN, 44},
        {TS_AIRF, 39},
        {TS_AIRB, 35},
        {TS_AIRHI, 33},
        {TS_AIRLW, 44},
    };
    for (int i = 0; i < (int)countof(baked_len); i++)
    {
        int ts = baked_len[i][0];
        state_len[ts] = baked_len[i][1];
        state_next[ts] = Tracked_NaturalNext(ts);
    }
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

// Could an aerial or airdodge pressed with the ECB bottom at (x, y) touch
// down on this frame? Only if some floor is a few units around it: an ECB
// swap moves the bottom by less than 10. Lets the branches skip the frames
// high above everything.
static int Floor_Near(float x, float y)
{
    for (int i = 0; i < floor_num; i++)
    {
        FloorLine *f = &floor_cache[i];
        float left = f->x0 < f->x1 ? f->x0 : f->x1;
        float right = f->x0 < f->x1 ? f->x1 : f->x0;
        float low = f->y0 < f->y1 ? f->y0 : f->y1;
        float high = f->y0 < f->y1 ? f->y1 : f->y0;
        if (x >= left - 10.f && x <= right + 10.f && high >= y - 15.f && low <= y + 5.f)
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
    float lean;          // a fall's blend toward its leaning pose (mv.co.fall.x4)
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
    int ecb_pending; // in an aerial or airdodge whose own ECB bottom hasn't been used yet (lock)
    int dodge_flat;  // the airdodge started with no vertical speed
    float lean;      // a fall's blend toward its leaning pose
    int lean_side;   // LEAN_FORWARD, LEAN_BACK or LEAN_NONE
} SimState;

// What happened on one simulated frame.
typedef struct SimStep
{
    int landed;
    int first_ecb;        // an aerial's or airdodge's own ECB bottom was used for the first time
    int unlearned;        // relied on data the event hasn't learned yet
    int fastfall_started;
    EcbSample *ecb;       // learned ECB for this frame
    EcbSample lean_ecb;   // ... when it's a leaning fall's mix, it's kept here
} SimStep;

// Aerials, as bits in the masks below
#define AERIAL_BIT(ts) (1 << ((ts) - TS_AIRN))
#define LL_AI_MAX_STEPS 12 // the bottom lock lasts at most 10 frames
#define LL_AI_MIN_GAIN 4   // an AI must finish its landing this many frames sooner to be shown

// Horizontal airdodge directions, as bits
#define DODGE_RIGHT 1
#define DODGE_LEFT 2

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
    u8 ai_delay[LL_SIM_FRAMES + 1];    // frames from k until that touchdown (lock)
    u8 ai_show[LL_SIM_FRAMES + 1];     // ... the ones worth showing: no aerial lag, and done
                                       // at least LL_AI_MIN_GAIN frames before holding would be
    u8 ai_unlearned;                   // aerials skipped because their ECB isn't learned yet
    int ai_first;                      // first frame of the first shown window, 0 = none
    int ai_width;                      // frames in that window
    u8 ai_aerials;                     // aerials that work somewhere in that window

    // perfect wavelands: a horizontal airdodge on frame k whose ECB touches
    // down the first time it is used, keeping all of the dodge's speed
    u8 wl_mask[LL_SIM_FRAMES + 1]; // DODGE_RIGHT / DODGE_LEFT that work when pressed on frame k
    u8 wl_unlearned;               // the airdodge's ECB isn't learned yet
    int wl_first;                  // first frame of the first window, 0 = none
    int wl_width;
    u8 wl_dirs;                    // directions that work somewhere in that window
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
static Vec2 common_dodge_deadzone;
static float common_dodge_force;
static float common_dodge_decay;
static float common_waveland_lag;
static float common_fall_lean_deadzone;
static float common_fall_lean_rate;

static int ai_show_all; // the AI Filter option is on All

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

// A fall leans its pose toward a forward or backward version of its
// animation as Falcon drifts (ftCo_Fall_Anim_Inner, also FallAerial): past a
// deadzone, his x speed as a fraction of the drift max sets a target, the
// blend moves part of the way there each frame, and the pose is the plain
// one mixed with the leaning one by that blend. Entering the fall starts it
// at 0, and the animation step that moves it runs before physics, so it
// sees last frame's speed. The bones are mixed, not the ECB, so the ECB
// isn't a straight line between the two poses: it's measured at several
// blends (below) and read between the nearest two.
#define LEAN_NONE 0
#define LEAN_FORWARD 1
#define LEAN_BACK 2

typedef struct LeanEcb
{
    u8 fall; // 0 Fall, 1 FallAerial
    u8 back; // leaning backward
    u8 k;    // animation frame in the loop
    float lean, bottom, top, side, front, back_x;
} LeanEcb;

// Falcon at steady drift speeds, facing right, from Fall and FallAerial
// captures (a 0.4.1 test build, 2026-10-03), in order of fall, direction,
// blend and frame.
static const LeanEcb lean_ecb[] = {
    // BEGIN generated lean data (lean captures, 2026-10-03)
    {0, 0, 0, 0.22222f, 1.7768f, 10.7010f, 6.2389f, 4.0270f, -4.0270f},
    {0, 0, 1, 0.22222f, 1.9366f, 10.7268f, 6.3317f, 4.0694f, -4.0694f},
    {0, 0, 2, 0.22222f, 2.2779f, 10.9668f, 6.6224f, 4.2658f, -4.2658f},
    {0, 0, 3, 0.22222f, 2.5675f, 11.1714f, 6.8694f, 4.4085f, -4.4085f},
    {0, 0, 4, 0.22222f, 2.5573f, 11.2166f, 6.8870f, 4.4582f, -4.4582f},
    {0, 0, 5, 0.22222f, 2.3362f, 11.0849f, 6.7106f, 4.5560f, -4.5560f},
    {0, 0, 6, 0.22222f, 2.0353f, 10.8760f, 6.4557f, 4.5539f, -4.5539f},
    {0, 0, 7, 0.22222f, 1.8191f, 10.6802f, 6.2496f, 4.2839f, -4.2839f},
    {0, 0, 0, 0.33333f, 1.6888f, 10.6611f, 6.1750f, 4.0801f, -4.0801f},
    {0, 0, 1, 0.33333f, 1.8847f, 10.7082f, 6.2965f, 4.0830f, -4.0830f},
    {0, 0, 2, 0.33333f, 2.2313f, 10.9389f, 6.5851f, 4.2500f, -4.2500f},
    {0, 0, 3, 0.33333f, 2.5062f, 11.1465f, 6.8264f, 4.3812f, -4.3812f},
    {0, 0, 4, 0.33333f, 2.4732f, 11.2182f, 6.8457f, 4.4359f, -4.4359f},
    {0, 0, 5, 0.33333f, 2.2283f, 11.0448f, 6.6366f, 4.5343f, -4.5343f},
    {0, 0, 6, 0.33333f, 1.9153f, 10.8084f, 6.3618f, 4.5496f, -4.5496f},
    {0, 0, 7, 0.33333f, 1.7013f, 10.6375f, 6.1694f, 4.3276f, -4.3276f},
    {0, 0, 0, 0.44271f, 1.6174f, 10.6213f, 6.1193f, 4.1293f, -4.1293f},
    {0, 0, 1, 0.44358f, 1.8458f, 10.6897f, 6.2678f, 4.0925f, -4.0925f},
    {0, 0, 2, 0.44401f, 2.1964f, 10.9764f, 6.5864f, 4.2279f, -4.2279f},
    {0, 0, 3, 0.44423f, 2.4556f, 11.1876f, 6.8216f, 4.3446f, -4.3446f},
    {0, 0, 4, 0.44434f, 2.3997f, 11.2174f, 6.8085f, 4.4066f, -4.4066f},
    {0, 0, 5, 0.44439f, 2.1329f, 11.0033f, 6.5681f, 4.5073f, -4.5073f},
    {0, 0, 6, 0.44442f, 1.8095f, 10.7410f, 6.2752f, 4.5424f, -4.5424f},
    {0, 0, 7, 0.44443f, 1.5997f, 10.5946f, 6.0971f, 4.3704f, -4.3704f},
    {0, 0, 0, 0.55555f, 1.5597f, 10.5797f, 6.0697f, 4.1766f, -4.1766f},
    {0, 0, 1, 0.55555f, 1.8192f, 10.6709f, 6.2451f, 4.0980f, -4.0980f},
    {0, 0, 2, 0.55555f, 2.1729f, 11.0502f, 6.6115f, 4.1992f, -4.1992f},
    {0, 0, 3, 0.55555f, 2.4153f, 11.2254f, 6.8204f, 4.2988f, -4.2988f},
    {0, 0, 4, 0.55556f, 2.3366f, 11.2142f, 6.7754f, 4.3703f, -4.3703f},
    {0, 0, 5, 0.55556f, 2.0497f, 10.9606f, 6.5052f, 4.4753f, -4.4753f},
    {0, 0, 6, 0.55556f, 1.7180f, 10.6739f, 6.1960f, 4.5325f, -4.5325f},
    {0, 0, 7, 0.55556f, 1.5144f, 10.5512f, 6.0328f, 4.4123f, -4.4123f},
    {0, 0, 0, 0.66666f, 1.5193f, 10.5381f, 6.0287f, 4.2194f, -4.2194f},
    {0, 0, 1, 0.66666f, 1.8059f, 10.6782f, 6.2421f, 4.0993f, -4.0993f},
    {0, 0, 2, 0.66666f, 2.1612f, 11.1214f, 6.6413f, 4.1645f, -4.1645f},
    {0, 0, 3, 0.66667f, 2.3856f, 11.2598f, 6.8227f, 4.2442f, -4.2442f},
    {0, 0, 4, 0.66667f, 2.2841f, 11.2090f, 6.7465f, 4.3274f, -4.3274f},
    {0, 0, 5, 0.66667f, 1.9790f, 10.9169f, 6.4479f, 4.4386f, -4.4386f},
    {0, 0, 6, 0.66667f, 1.6411f, 10.6071f, 6.1241f, 4.5199f, -4.5199f},
    {0, 0, 7, 0.66667f, 1.4458f, 10.5076f, 5.9767f, 4.4533f, -4.4533f},
    {0, 0, 0, 0.83332f, 1.4899f, 10.4743f, 5.9821f, 4.2760f, -4.2760f},
    {0, 0, 1, 0.83333f, 1.8109f, 10.8188f, 6.3149f, 4.0932f, -4.0932f},
    {0, 0, 2, 0.83333f, 2.1658f, 11.2242f, 6.6950f, 4.1015f, -4.1015f},
    {0, 0, 3, 0.83333f, 2.3608f, 11.3056f, 6.8332f, 4.1467f, -4.1467f},
    {0, 0, 4, 0.83333f, 2.2248f, 11.1974f, 6.7111f, 4.2513f, -4.2513f},
    {0, 0, 5, 0.83333f, 1.8960f, 10.8498f, 6.3729f, 4.3749f, -4.3749f},
    {0, 0, 6, 0.83333f, 1.5531f, 10.5073f, 6.0302f, 4.4965f, -4.4965f},
    {0, 0, 7, 0.83333f, 1.3749f, 10.4412f, 5.9081f, 4.5130f, -4.5130f},
    {0, 0, 0, 0.99609f, 1.4985f, 10.5281f, 6.0133f, 4.3217f, -4.3217f},
    {0, 0, 1, 0.99805f, 1.8460f, 10.9572f, 6.4016f, 4.0777f, -4.0777f},
    {0, 0, 2, 0.99902f, 2.1968f, 11.3218f, 6.7593f, 4.0265f, -4.0265f},
    {0, 0, 3, 0.99951f, 2.3595f, 11.3448f, 6.8521f, 4.0322f, -4.0322f},
    {0, 0, 4, 0.99976f, 2.1887f, 11.1820f, 6.6853f, 4.1621f, -4.1621f},
    {0, 0, 5, 0.99988f, 1.8405f, 10.7815f, 6.3110f, 4.3018f, -4.3018f},
    {0, 0, 6, 0.99994f, 1.4983f, 10.4086f, 5.9534f, 4.4680f, -4.4680f},
    {0, 0, 7, 0.99997f, 1.3435f, 10.3738f, 5.8587f, 4.5706f, -4.5706f},
    {0, 1, 0, 0.22222f, 2.3857f, 11.5406f, 6.9632f, 4.0671f, -4.0671f},
    {0, 1, 1, 0.22222f, 2.4853f, 11.5777f, 7.0315f, 4.1654f, -4.1654f},
    {0, 1, 2, 0.22222f, 2.7828f, 11.8150f, 7.2989f, 4.4266f, -4.4266f},
    {0, 1, 3, 0.22222f, 3.0142f, 12.0100f, 7.5121f, 4.5858f, -4.5858f},
    {0, 1, 4, 0.22222f, 2.9998f, 12.0175f, 7.5087f, 4.6420f, -4.6420f},
    {0, 1, 5, 0.22222f, 2.8248f, 11.9420f, 7.3834f, 4.7086f, -4.7086f},
    {0, 1, 6, 0.22222f, 2.6004f, 11.7445f, 7.1724f, 4.6665f, -4.6665f},
    {0, 1, 7, 0.22222f, 2.4553f, 11.5508f, 7.0030f, 4.3595f, -4.3595f},
    {0, 1, 0, 0.33333f, 2.6030f, 11.8746f, 7.2388f, 4.1181f, -4.1181f},
    {0, 1, 1, 0.33333f, 2.7131f, 11.9379f, 7.3255f, 4.1997f, -4.1997f},
    {0, 1, 2, 0.33333f, 2.9939f, 12.1655f, 7.5797f, 4.4597f, -4.4597f},
    {0, 1, 3, 0.33333f, 3.1823f, 12.3333f, 7.7578f, 4.6197f, -4.6197f},
    {0, 1, 4, 0.33333f, 3.1516f, 12.3476f, 7.7496f, 4.6783f, -4.6783f},
    {0, 1, 5, 0.33333f, 2.9735f, 12.2585f, 7.6160f, 4.7264f, -4.7264f},
    {0, 1, 6, 0.33333f, 2.7722f, 12.0625f, 7.4174f, 4.6787f, -4.6787f},
    {0, 1, 7, 0.33333f, 2.6609f, 11.8923f, 7.2766f, 4.4049f, -4.4049f},
    {0, 1, 0, 0.44444f, 2.8350f, 12.1708f, 7.5029f, 4.1524f, -4.1524f},
    {0, 1, 1, 0.44444f, 2.9558f, 12.2603f, 7.6081f, 4.2140f, -4.2140f},
    {0, 1, 2, 0.44444f, 3.2191f, 12.4793f, 7.8492f, 4.4673f, -4.4673f},
    {0, 1, 3, 0.44444f, 3.3640f, 12.6215f, 7.9927f, 4.6268f, -4.6268f},
    {0, 1, 4, 0.44444f, 3.3224f, 12.6404f, 7.9814f, 4.6867f, -4.6867f},
    {0, 1, 5, 0.44444f, 3.1412f, 12.5362f, 7.8387f, 4.7166f, -4.7166f},
    {0, 1, 6, 0.44444f, 2.9629f, 12.3402f, 7.6515f, 4.6636f, -4.6636f},
    {0, 1, 7, 0.44444f, 2.8841f, 12.1911f, 7.5376f, 4.4264f, -4.4264f},
    {0, 1, 0, 0.55555f, 3.0806f, 12.4251f, 7.7529f, 4.1712f, -4.1712f},
    {0, 1, 1, 0.55555f, 3.2126f, 12.5405f, 7.8766f, 4.2102f, -4.2102f},
    {0, 1, 2, 0.55555f, 3.4577f, 12.7521f, 8.1049f, 4.4514f, -4.4514f},
    {0, 1, 3, 0.55555f, 3.5586f, 12.8704f, 8.2145f, 4.6088f, -4.6088f},
    {0, 1, 4, 0.55556f, 3.5111f, 12.8902f, 8.2007f, 4.6687f, -4.6687f},
    {0, 1, 5, 0.55556f, 3.3266f, 12.7697f, 8.0481f, 4.6817f, -4.6817f},
    {0, 1, 6, 0.55556f, 3.1709f, 12.5720f, 7.8714f, 4.6234f, -4.6234f},
    {0, 1, 7, 0.55556f, 3.1236f, 12.4418f, 7.7827f, 4.4252f, -4.4252f},
    {0, 1, 0, 0.66666f, 3.3390f, 12.6338f, 7.9864f, 4.1760f, -4.1760f},
    {0, 1, 1, 0.66666f, 3.4827f, 12.7748f, 8.1287f, 4.1904f, -4.1904f},
    {0, 1, 2, 0.66666f, 3.7090f, 12.9804f, 8.3447f, 4.4142f, -4.4142f},
    {0, 1, 3, 0.66667f, 3.7655f, 13.0765f, 8.4210f, 4.5672f, -4.5672f},
    {0, 1, 4, 0.66667f, 3.7167f, 13.0927f, 8.4047f, 4.6264f, -4.6264f},
    {0, 1, 5, 0.66667f, 3.5284f, 12.9540f, 8.2412f, 4.6239f, -4.6239f},
    {0, 1, 6, 0.66667f, 3.3950f, 12.7533f, 8.0741f, 4.5609f, -4.5609f},
    {0, 1, 7, 0.66667f, 3.3781f, 12.6395f, 8.0088f, 4.4032f, -4.4032f},
    {0, 1, 0, 0.83332f, 3.7483f, 12.8555f, 8.3019f, 4.1605f, -4.1605f},
    {0, 1, 1, 0.83333f, 3.9105f, 13.0342f, 8.4723f, 4.1358f, -4.1358f},
    {0, 1, 2, 0.83333f, 4.1078f, 13.2341f, 8.6710f, 4.3241f, -4.3241f},
    {0, 1, 3, 0.83333f, 4.0972f, 13.3001f, 8.6987f, 4.4655f, -4.4655f},
    {0, 1, 4, 0.83333f, 4.0535f, 13.2999f, 8.6767f, 4.5220f, -4.5220f},
    {0, 1, 5, 0.83333f, 3.8587f, 13.1305f, 8.4946f, 4.5009f, -4.5009f},
    {0, 1, 6, 0.83333f, 3.7578f, 12.9228f, 8.3403f, 4.4326f, -4.4326f},
    {0, 1, 7, 0.83333f, 3.7848f, 12.8293f, 8.3070f, 4.3364f, -4.3364f},
    {0, 1, 0, 0.99998f, 4.1802f, 12.9629f, 8.5716f, 4.1224f, -4.1224f},
    {0, 1, 1, 0.99999f, 4.3624f, 13.1786f, 8.7705f, 4.0581f, -4.0581f},
    {0, 1, 2, 1.00000f, 4.5301f, 13.3773f, 8.9537f, 4.2010f, -4.2010f},
    {0, 1, 3, 1.00000f, 4.4520f, 13.4174f, 8.9347f, 4.3236f, -4.3236f},
    {0, 1, 4, 1.00000f, 4.4204f, 13.3854f, 8.9029f, 4.3761f, -4.3761f},
    {0, 1, 5, 1.00000f, 4.2175f, 13.1808f, 8.6991f, 4.3440f, -4.3440f},
    {0, 1, 6, 1.00000f, 4.1479f, 12.9629f, 8.5554f, 4.2744f, -4.2744f},
    {0, 1, 7, 1.00000f, 4.2168f, 12.8845f, 8.5506f, 4.2368f, -4.2368f},
    {1, 0, 0, 0.22222f, 2.4356f, 11.6216f, 7.0286f, 6.2078f, -4.9071f},
    {1, 0, 1, 0.22222f, 2.6266f, 11.8683f, 7.2475f, 6.3668f, -4.7809f},
    {1, 0, 2, 0.22222f, 3.0424f, 12.0746f, 7.5585f, 6.5830f, -4.8331f},
    {1, 0, 3, 0.22222f, 3.4401f, 12.1525f, 7.7963f, 6.4821f, -4.9503f},
    {1, 0, 4, 0.22222f, 3.5292f, 11.9794f, 7.7543f, 6.3564f, -5.0119f},
    {1, 0, 5, 0.22222f, 3.5242f, 11.7092f, 7.6167f, 6.3186f, -5.0921f},
    {1, 0, 6, 0.22222f, 3.3045f, 11.6169f, 7.4607f, 6.2701f, -5.1050f},
    {1, 0, 7, 0.22222f, 2.7040f, 11.5858f, 7.1449f, 6.2284f, -5.0691f},
    {1, 0, 0, 0.33333f, 2.2994f, 11.8653f, 7.0824f, 6.1336f, -4.9409f},
    {1, 0, 1, 0.33333f, 2.5348f, 12.0745f, 7.3047f, 6.2653f, -4.7521f},
    {1, 0, 2, 0.33333f, 2.9601f, 12.2892f, 7.6246f, 6.4488f, -4.7936f},
    {1, 0, 3, 0.33333f, 3.3389f, 12.3942f, 7.8665f, 6.3518f, -4.9205f},
    {1, 0, 4, 0.33333f, 3.3713f, 12.2314f, 7.8013f, 6.2317f, -5.0089f},
    {1, 0, 5, 0.33333f, 3.2836f, 11.9801f, 7.6319f, 6.2042f, -5.1286f},
    {1, 0, 6, 0.33333f, 3.0158f, 11.8859f, 7.4509f, 6.1728f, -5.1794f},
    {1, 0, 7, 0.33333f, 2.4774f, 11.8588f, 7.1681f, 6.1568f, -5.1641f},
    {1, 0, 0, 0.44271f, 2.1733f, 12.1074f, 7.1403f, 6.0447f, -4.9537f},
    {1, 0, 1, 0.44358f, 2.4487f, 12.2790f, 7.3638f, 6.1512f, -4.7088f},
    {1, 0, 2, 0.44401f, 2.8818f, 12.5035f, 7.6927f, 6.3041f, -4.7405f},
    {1, 0, 3, 0.44423f, 3.2411f, 12.6388f, 7.9399f, 6.2142f, -4.8742f},
    {1, 0, 4, 0.44434f, 3.2190f, 12.4891f, 7.8540f, 6.1015f, -4.9870f},
    {1, 0, 5, 0.44439f, 3.0541f, 12.2593f, 7.6567f, 6.0826f, -5.1417f},
    {1, 0, 6, 0.44442f, 2.7432f, 12.1656f, 7.4544f, 6.0645f, -5.2267f},
    {1, 0, 7, 0.44443f, 2.2653f, 12.1413f, 7.2033f, 6.0709f, -5.2318f},
    {1, 0, 0, 0.55555f, 2.0519f, 12.3586f, 7.2052f, 5.9376f, -4.9454f},
    {1, 0, 1, 0.55555f, 2.3666f, 12.4862f, 7.4264f, 6.0227f, -4.6500f},
    {1, 0, 2, 0.55555f, 2.8068f, 12.7197f, 7.7633f, 6.1483f, -4.6733f},
    {1, 0, 3, 0.55555f, 3.1463f, 12.8872f, 8.0168f, 6.0700f, -4.8109f},
    {1, 0, 4, 0.55556f, 3.0722f, 12.7527f, 7.9125f, 5.9666f, -4.9457f},
    {1, 0, 5, 0.55556f, 2.8362f, 12.5462f, 7.6912f, 5.9543f, -5.1303f},
    {1, 0, 6, 0.55556f, 2.4880f, 12.4551f, 7.4715f, 5.9460f, -5.2457f},
    {1, 0, 7, 0.55556f, 2.0686f, 12.4320f, 7.2503f, 5.9714f, -5.2709f},
    {1, 0, 0, 0.66666f, 1.9415f, 12.6068f, 7.2742f, 5.8181f, -4.9158f},
    {1, 0, 1, 0.66666f, 2.2908f, 12.6910f, 7.4909f, 5.8838f, -4.5771f},
    {1, 0, 2, 0.66666f, 2.7362f, 12.9349f, 7.8355f, 5.9846f, -4.5929f},
    {1, 0, 3, 0.66667f, 3.0555f, 13.1373f, 8.0964f, 5.9211f, -4.7310f},
    {1, 0, 4, 0.66667f, 2.9318f, 13.0207f, 7.9763f, 5.8285f, -4.8847f},
    {1, 0, 5, 0.66667f, 2.6311f, 12.8393f, 7.7352f, 5.8205f, -5.0937f},
    {1, 0, 6, 0.66667f, 2.2518f, 12.7527f, 7.5023f, 5.8181f, -5.2353f},
    {1, 0, 7, 0.66667f, 1.8885f, 12.7295f, 7.3090f, 5.8591f, -5.2807f},
    {1, 0, 0, 0.83332f, 1.7944f, 12.9794f, 7.3869f, 5.6154f, -4.8318f},
    {1, 0, 1, 0.83333f, 2.1885f, 12.9959f, 7.5922f, 5.6571f, -4.4413f},
    {1, 0, 2, 0.83333f, 2.6389f, 13.2560f, 7.9474f, 5.7266f, -4.4479f},
    {1, 0, 3, 0.83333f, 2.9268f, 13.5153f, 8.2210f, 5.6916f, -4.5799f},
    {1, 0, 4, 0.83333f, 2.7337f, 13.4295f, 8.0816f, 5.6172f, -4.7557f},
    {1, 0, 5, 0.83333f, 2.3491f, 13.2880f, 7.8185f, 5.6114f, -4.9901f},
    {1, 0, 6, 0.83333f, 1.9363f, 13.2112f, 7.5738f, 5.6106f, -5.1630f},
    {1, 0, 7, 0.83333f, 1.6518f, 13.1853f, 7.4186f, 5.6688f, -5.2394f},
    {1, 0, 0, 0.99998f, 1.6711f, 13.3501f, 7.5106f, 5.3884f, -4.7016f},
    {1, 0, 1, 0.99805f, 2.1022f, 13.2930f, 7.6976f, 4.8464f, -4.8464f},
    {1, 0, 2, 0.99902f, 2.5530f, 13.5720f, 8.0625f, 4.8683f, -4.8683f},
    {1, 0, 3, 0.99951f, 2.8082f, 13.8932f, 8.3507f, 4.9261f, -4.9261f},
    {1, 0, 4, 0.99976f, 2.5518f, 13.8431f, 8.1974f, 4.9933f, -4.9933f},
    {1, 0, 5, 0.99988f, 2.1002f, 13.7431f, 7.9216f, 5.3950f, -4.8276f},
    {1, 0, 6, 0.99994f, 1.6712f, 13.6793f, 7.6752f, 5.3871f, -5.0218f},
    {1, 0, 7, 0.99997f, 1.4583f, 13.6477f, 7.5530f, 5.4550f, -5.1314f},
    {1, 1, 0, 0.22222f, 3.0218f, 11.8345f, 7.4281f, 6.5148f, -5.3804f},
    {1, 1, 1, 0.22222f, 3.1441f, 12.0886f, 7.6163f, 6.7122f, -5.2579f},
    {1, 1, 2, 0.22222f, 3.5104f, 12.1770f, 7.8437f, 6.9335f, -5.2588f},
    {1, 1, 3, 0.22222f, 3.8803f, 12.0743f, 7.9773f, 6.8755f, -5.2808f},
    {1, 1, 4, 0.22222f, 4.0789f, 11.8841f, 7.9815f, 6.7822f, -5.3001f},
    {1, 1, 5, 0.22222f, 4.2255f, 11.7256f, 7.9755f, 6.7168f, -5.3883f},
    {1, 1, 6, 0.22222f, 4.0415f, 11.7382f, 7.8899f, 6.6250f, -5.4525f},
    {1, 1, 7, 0.22222f, 3.3880f, 11.7374f, 7.5627f, 6.5319f, -5.4938f},
    {1, 1, 0, 0.33333f, 3.1737f, 12.2049f, 7.6893f, 6.6104f, -5.6479f},
    {1, 1, 1, 0.33333f, 3.3102f, 12.4129f, 7.8615f, 6.7937f, -5.4680f},
    {1, 1, 2, 0.33333f, 3.6626f, 12.4454f, 8.0540f, 6.9811f, -5.4379f},
    {1, 1, 3, 0.33333f, 3.9997f, 12.2704f, 8.1350f, 6.9411f, -5.4281f},
    {1, 1, 4, 0.33333f, 4.1951f, 12.0717f, 8.1334f, 6.8652f, -5.4507f},
    {1, 1, 5, 0.33333f, 4.3267f, 11.9840f, 8.1553f, 6.7999f, -5.5783f},
    {1, 1, 6, 0.33333f, 4.0574f, 12.0625f, 8.0600f, 6.7102f, -5.7027f},
    {1, 1, 7, 0.33333f, 3.4879f, 12.1001f, 7.7940f, 6.6247f, -5.7999f},
    {1, 1, 0, 0.44444f, 3.3296f, 12.5851f, 7.9573f, 6.6986f, -5.8917f},
    {1, 1, 1, 0.44444f, 3.4804f, 12.7395f, 8.1099f, 6.8666f, -5.6635f},
    {1, 1, 2, 0.44444f, 3.8183f, 12.7150f, 8.2667f, 7.0203f, -5.6070f},
    {1, 1, 3, 0.44444f, 4.1222f, 12.4659f, 8.2940f, 6.9966f, -5.5669f},
    {1, 1, 4, 0.44444f, 4.3155f, 12.2548f, 8.2852f, 6.9371f, -5.5893f},
    {1, 1, 5, 0.44444f, 4.4315f, 12.2360f, 8.3338f, 6.8732f, -5.7502f},
    {1, 1, 6, 0.44444f, 3.8964f, 12.3913f, 8.1439f, 6.7866f, -5.9282f},
    {1, 1, 7, 0.44444f, 3.4072f, 12.4762f, 7.9417f, 6.7101f, -6.0758f},
    {1, 1, 0, 0.55555f, 3.2887f, 12.9699f, 8.1293f, 6.7794f, -6.1105f},
    {1, 1, 1, 0.55555f, 3.6275f, 13.0659f, 8.3467f, 6.9306f, -5.8443f},
    {1, 1, 2, 0.55555f, 3.9774f, 12.9844f, 8.4809f, 7.0511f, -5.7662f},
    {1, 1, 3, 0.55555f, 4.2478f, 12.6604f, 8.4541f, 7.0419f, -5.6970f},
    {1, 1, 4, 0.55556f, 4.4400f, 12.4331f, 8.4365f, 6.9973f, -5.7155f},
    {1, 1, 5, 0.55556f, 4.3161f, 12.4799f, 8.3980f, 6.9360f, -5.9043f},
    {1, 1, 6, 0.55556f, 3.7410f, 12.7212f, 8.2311f, 6.8539f, -6.1281f},
    {1, 1, 7, 0.55556f, 3.2837f, 12.8604f, 8.0720f, 6.7879f, -6.3186f},
    {1, 1, 0, 0.66666f, 3.2358f, 13.3538f, 8.2948f, 6.8523f, -6.3036f},
    {1, 1, 1, 0.66666f, 3.6099f, 13.3894f, 8.4996f, 6.9856f, -6.0106f},
    {1, 1, 2, 0.66666f, 4.1396f, 13.2521f, 8.6959f, 7.0735f, -5.9157f},
    {1, 1, 3, 0.66667f, 4.3763f, 12.8537f, 8.6150f, 7.0765f, -5.8181f},
    {1, 1, 4, 0.66667f, 4.5685f, 12.6060f, 8.5872f, 7.0453f, -5.8293f},
    {1, 1, 5, 0.66667f, 4.1593f, 12.7136f, 8.4364f, 6.9880f, -6.0413f},
    {1, 1, 6, 0.66667f, 3.5914f, 13.0482f, 8.3198f, 6.9118f, -6.3021f},
    {1, 1, 7, 0.66667f, 3.1663f, 13.2467f, 8.2065f, 6.8580f, -6.5262f},
    {1, 1, 0, 0.83332f, 3.1632f, 13.9161f, 8.5396f, 6.9463f, -6.5436f},
    {1, 1, 1, 0.83333f, 3.5897f, 13.8631f, 8.7264f, 7.0500f, -6.2329f},
    {1, 1, 2, 0.83333f, 4.2061f, 13.6469f, 8.9265f, 7.0911f, -6.1222f},
    {1, 1, 3, 0.83333f, 4.5746f, 13.1402f, 8.8574f, 7.1080f, -5.9828f},
    {1, 1, 4, 0.83333f, 4.4400f, 12.8542f, 8.6471f, 7.0931f, -5.9766f},
    {1, 1, 5, 0.83333f, 3.9340f, 13.0406f, 8.4873f, 7.0445f, -6.2165f},
    {1, 1, 6, 0.83333f, 3.3780f, 13.5246f, 8.4513f, 6.9803f, -6.5146f},
    {1, 1, 7, 0.83333f, 3.0017f, 13.8173f, 8.4095f, 6.9482f, -6.7677f},
    {1, 1, 0, 0.99998f, 3.0985f, 14.4461f, 8.7723f, 7.0211f, -6.7228f},
    {1, 1, 1, 0.99805f, 3.5772f, 14.3095f, 8.9433f, 7.0915f, -6.4214f},
    {1, 1, 2, 0.99902f, 4.2371f, 14.0263f, 9.1317f, 7.0893f, -6.3072f},
    {1, 1, 3, 0.99951f, 4.5840f, 13.4201f, 9.0021f, 7.1140f, -6.1270f},
    {1, 1, 4, 0.99976f, 4.2629f, 13.0867f, 8.6748f, 7.1107f, -6.0962f},
    {1, 1, 5, 0.99988f, 3.7212f, 13.3325f, 8.5268f, 7.0738f, -6.3584f},
    {1, 1, 6, 0.99994f, 3.1781f, 13.9704f, 8.5743f, 7.0261f, -6.6704f},
    {1, 1, 7, 0.99997f, 2.8511f, 14.3591f, 8.6051f, 7.0198f, -6.9216f},
    // END generated lean data
};

// The fall's first frame doesn't fit the steady poses: it's the frame the
// animation step first sets the blend, and its ECB at a blend is measured on
// its own (a fall entered already drifting, in order of fall, direction and
// blend).
static const LeanEcb lean_onset[] = {
    // BEGIN generated onset data (lean captures, 2026-10-03)
    {0, 0, 1, 0.11111f, 1.9035f, 10.7109f, 6.3072f, 4.0847f, -4.0847f},
    {0, 0, 1, 0.16667f, 1.8417f, 10.6876f, 6.2646f, 4.1052f, -4.1052f},
    {0, 0, 1, 0.22222f, 1.7942f, 10.6663f, 6.2303f, 4.1217f, -4.1217f},
    {0, 0, 1, 0.27778f, 1.7592f, 10.6472f, 6.2032f, 4.1345f, -4.1345f},
    {0, 0, 1, 0.33333f, 1.7351f, 10.6303f, 6.1827f, 4.1439f, -4.1439f},
    {0, 0, 1, 0.41667f, 1.7161f, 10.6087f, 6.1624f, 4.1520f, -4.1520f},
    {0, 0, 1, 0.50000f, 1.7133f, 10.6402f, 6.1767f, 4.1539f, -4.1539f},
    {0, 1, 1, 0.11111f, 2.4385f, 11.5211f, 6.9798f, 4.1696f, -4.1696f},
    {0, 1, 1, 0.16667f, 2.6241f, 11.8305f, 7.2273f, 4.2064f, -4.2064f},
    {0, 1, 1, 0.22222f, 2.8091f, 12.0955f, 7.4523f, 4.2268f, -4.2268f},
    {0, 1, 1, 0.27778f, 2.9910f, 12.3189f, 7.6549f, 4.2341f, -4.2341f},
    {0, 1, 1, 0.33333f, 3.1674f, 12.5042f, 7.8358f, 4.2315f, -4.2315f},
    {0, 1, 1, 0.41667f, 3.4174f, 12.7197f, 8.0685f, 4.2146f, -4.2146f},
    {0, 1, 1, 0.50000f, 3.6451f, 12.8735f, 8.2593f, 4.1886f, -4.1886f},
    {1, 0, 1, 0.11111f, 2.5851f, 11.8410f, 7.2131f, 6.3784f, -4.8370f},
    {1, 0, 1, 0.16667f, 2.4856f, 12.0183f, 7.2519f, 6.2933f, -4.8348f},
    {1, 0, 1, 0.22222f, 2.3983f, 12.1850f, 7.2916f, 6.2041f, -4.8202f},
    {1, 0, 1, 0.27778f, 2.3226f, 12.3405f, 7.3316f, 6.1132f, -4.7954f},
    {1, 0, 1, 0.33333f, 2.2578f, 12.4847f, 7.3712f, 6.0227f, -4.7625f},
    {1, 0, 1, 0.41667f, 2.1791f, 12.6791f, 7.4291f, 5.8917f, -4.7023f},
    {1, 0, 1, 0.50000f, 2.1209f, 12.8469f, 7.4839f, 5.7708f, -4.6343f},
    {1, 1, 1, 0.11111f, 3.1024f, 12.0509f, 7.5767f, 6.6912f, -5.2910f},
    {1, 1, 1, 0.16667f, 3.2356f, 12.3324f, 7.7840f, 6.7587f, -5.4948f},
    {1, 1, 1, 0.22222f, 3.3640f, 12.5991f, 7.9815f, 6.8169f, -5.6719f},
    {1, 1, 1, 0.27778f, 3.4866f, 12.8486f, 8.1676f, 6.8668f, -5.8247f},
    {1, 1, 1, 0.33333f, 3.5134f, 13.0794f, 8.2964f, 6.9093f, -5.9555f},
    {1, 1, 1, 0.41667f, 3.4877f, 13.3882f, 8.4379f, 6.9609f, -6.1152f},
    {1, 1, 1, 0.50000f, 3.4736f, 13.6510f, 8.5623f, 7.0003f, -6.2369f},
    // END generated onset data
};

static int lean_first[2][2]; // first row of each fall and direction
static int lean_levels[2][2]; // blends measured for it
static int onset_first[2][2];
static int onset_levels[2][2];

static void Lean_Index(void)
{
    for (int i = (int)countof(lean_ecb) - 1; i >= 0; i--)
    {
        const LeanEcb *l = &lean_ecb[i];
        lean_first[l->fall][l->back] = i;
        if (l->k == 0)
            lean_levels[l->fall][l->back]++;
    }
    for (int i = (int)countof(lean_onset) - 1; i >= 0; i--)
    {
        const LeanEcb *l = &lean_onset[i];
        onset_first[l->fall][l->back] = i;
        onset_levels[l->fall][l->back]++;
    }
}

static int Lean_Target(FighterData *fp, float vx, float facing, float *target)
{
    float frac = vx / fp->attr.aerial_drift_max;
    if (frac > 1.f)
        frac = 1.f;
    else if (frac < -1.f)
        frac = -1.f;

    *target = 0;
    if (fabs(frac) <= common_fall_lean_deadzone)
        return LEAN_NONE;
    *target = (fabs(frac) - common_fall_lean_deadzone) / (1.f - common_fall_lean_deadzone);
    return frac * facing > 0 ? LEAN_FORWARD : LEAN_BACK;
}

static int Lean_Update(FighterData *fp, float vx, float facing, float *lean)
{
    float target;
    int side = Lean_Target(fp, vx, facing, &target);
    *lean += common_fall_lean_rate * (target - *lean);
    return side;
}

// The ECB at a lean, between the two nearest measured blends (rows, one per
// blend, stride apart; the plain pose is blend 0).
static EcbSample *Lean_Mix(EcbSample *plain, const LeanEcb *rows, int levels, int stride, float lean, EcbSample *out)
{
    float w0 = 0, b0 = plain->bottom, t0 = plain->top, s0 = plain->side_y, f0 = plain->front, k0 = plain->back;
    const LeanEcb *hi = 0;
    for (int i = 0; i < levels; i++)
    {
        hi = &rows[i * stride];
        if (lean <= hi->lean || i == levels - 1)
            break;
        w0 = hi->lean, b0 = hi->bottom, t0 = hi->top, s0 = hi->side, f0 = hi->front, k0 = hi->back_x;
    }
    float t = hi->lean > w0 ? (lean - w0) / (hi->lean - w0) : 1.f;
    if (t > 1.f)
        t = 1.f;

    *out = *plain;
    out->bottom = b0 + (hi->bottom - b0) * t;
    out->top = t0 + (hi->top - t0) * t;
    out->side_y = s0 + (hi->side - s0) * t;
    out->front = f0 + (hi->front - f0) * t;
    out->back = k0 + (hi->back_x - k0) * t;
    return out;
}

// The fall's ECB on this frame with the given lean.
static EcbSample *Lean_Ecb(EcbSample *plain, int ts, int side, int frame, float lean, EcbSample *out)
{
    int fall = ts == TS_FALLAERIAL, back = side == LEAN_BACK;
    if (side == LEAN_NONE || lean <= 0.f || !plain->has_bottom || !plain->has_shape)
        return plain;
    if (frame == 1 && onset_levels[fall][back] > 0)
        return Lean_Mix(plain, &lean_onset[onset_first[fall][back]], onset_levels[fall][back], 1, lean, out);
    if (lean_levels[fall][back] == 0)
        return plain;
    int k = frame % LL_FALL_LOOP;
    return Lean_Mix(plain, &lean_ecb[lean_first[fall][back] + k], lean_levels[fall][back], LL_FALL_LOOP, lean, out);
}

static int Is_Fall(int ts)
{
    return ts == TS_FALL || ts == TS_FALLAERIAL;
}

// The live fall's blend (mv.co.fall.x4, the second state variable).
static float Lean_FromFighter(FighterData *fp, int ts)
{
    float lean = 0;
    if (Is_Fall(ts))
        memcpy(&lean, &fp->state_var.state_var2, sizeof(lean));
    return lean;
}

// Whether the live frame's ECB can be learned: a fall only teaches its plain
// pose. vx is the speed the animation saw (last frame's).
static int Lean_IsPlain(FighterData *fp, int ts, float vx)
{
    if (!Is_Fall(ts))
        return 1;
    float target;
    return Lean_FromFighter(fp, ts) < 0.002f || Lean_Target(fp, vx, fp->facing_direction, &target) == LEAN_NONE;
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
    s->lean = Lean_FromFighter(fp, ts);
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
    // an aerial or airdodge is only ever entered in the air, after the lock
    // was set, so a lock still running means its bones haven't moved the
    // bottom yet
    s->ecb_pending = (Tracked_IsAerial(start->ts) || start->ts == TS_ESCAPEAIR) && start->ecb_lock > 0;
    s->dodge_flat = start->ts == TS_ESCAPEAIR && start->vel.Y == 0 && start->vel.X != 0;
    s->lean = start->lean;
    s->lean_side = LEAN_NONE;
}

// One frame, in the game's order: animation (the state can end), interrupt
// (press, an aerial or airdodge replaces the state), input timers, physics,
// ECB, floor test. press is the aerial or TS_ESCAPEAIR pressed this frame,
// or -1; dodge_x is the airdodge's direction (+1 right, -1 left), always
// horizontal.
static void Sim_Step(FighterData *fp, SimStart *start, SimState *s, int press, float dodge_x, SimStep *out)
{
    out->landed = 0;
    out->first_ecb = 0;
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
        s->ecb_pending = 0;
    }

    // interrupt: ftCo_AttackAir_EnterFromMsid keeps the fastfall and starts
    // the aerial's animation on this frame, before physics and collision.
    // ftCo_80099A9C (airdodge) replaces the speed with the dodge's; a
    // horizontal dodge has no vertical speed at all.
    if (press >= 0)
    {
        s->ts = press;
        s->frame = 0;
        s->len = state_len[press];
        s->len_estimated = 0;
        s->ecb_pending = 1;
        if (press == TS_ESCAPEAIR)
        {
            s->vx = dodge_x * common_dodge_force;
            s->vy = 0;
            s->dodge_flat = 1;
        }
    }

    // a fall's lean: back to plain on entering, then toward the drift speed
    if (Is_Fall(s->ts))
    {
        if (s->frame == 0)
        {
            s->lean = 0;
            s->lean_side = LEAN_NONE;
        }
        else
            s->lean_side = Lean_Update(fp, s->vx, start->facing, &s->lean);
    }

    // input: the stick is held, so its timers keep counting
    if (s->tilt_timer < LL_TIMER_MAX)
        s->tilt_timer++;
    if (s->trigger_timer < LL_TIMER_MAX)
        s->trigger_timer++;

    // this frame's learned ECB; for the airdodge, its first command variable
    // is the flag that ends the speed decay
    EcbSample *e = Ecb_Get(s->ts, s->frame);
    if (Is_Fall(s->ts))
        e = Lean_Ecb(e, s->ts, s->lean_side, s->frame, s->lean, &out->lean_ecb);
    out->ecb = e;

    if (s->ts == TS_ESCAPEAIR && !e->aerial_lag)
    {
        // ftCo_EscapeAir_Phys: the dodge's speed decays, no gravity or drift
        s->vx *= common_dodge_decay;
        s->vy *= common_dodge_decay;
    }
    else
    {
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
    }
    s->x += s->vx;
    s->y += s->vy;

    // ECB bottom: the lock counts down just before collision
    if (s->lock > 0)
        s->lock--;
    if (s->lock > 0)
        s->bottom = start->locked_bottom;
    else if (e->has_bottom)
        s->bottom = e->bottom;
    else
        out->unlearned = 1; // not learned yet: keep the last bottom
    if (Tracked_IsAerial(s->ts) || s->ts == TS_ESCAPEAIR)
    {
        if (!e->seen)
            out->unlearned = 1;
        if (s->ecb_pending && s->lock <= 0)
        {
            out->first_ecb = 1;
            s->ecb_pending = 0;
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
// ftCo_LandingAir_EnterWithLag for aerials, ftCo_LandingFallSpecial_Enter
// for the airdodge. An aerial whose own ECB touches down the first time it's
// used is an aerial interrupt, whatever its lag; a horizontal airdodge that
// does the same is a perfect waveland.
static int Landing_Kind(FighterData *fp, SimState *s, EcbSample *e, int first_ecb, int *lag, int *lcancel_lag)
{
    float normal_lag = fp->attr.normal_landing_lag;
    *lcancel_lag = 0;

    if (s->ts == TS_ESCAPEAIR)
    {
        *lag = (int)common_waveland_lag;
        return first_ecb && s->dodge_flat ? LAND_PERFECT_WL : LAND_WAVELAND;
    }

    if (!Tracked_IsAerial(s->ts))
    {
        if (s->vy > Fighter_GetSoftLandVelocity(fp))
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
        return first_ecb ? LAND_AI : LAND_NORMAL;
    }

    float full = Aerial_LandingLag(fp, s->ts);
    int halved = (int)(full / common_lcancel_div);
    if (halved == 0)
        halved = 1;
    *lcancel_lag = halved;

    int lcancel = s->trigger_timer < common_lcancel_window;
    *lag = lcancel ? halved : (int)full;
    if (first_ecb)
        return LAND_AI;
    return lcancel ? LAND_LCANCEL : LAND_AERIAL;
}

// Would pressing an aerial or airdodge on frame k touch down at once?
// before is the state just before frame k. Its ECB is used right away, or
// when the lock runs out, and only that first use counts. Returns the
// frames from k to that touchdown, or -1 for none.
static int Branch_Press(FighterData *fp, SimStart *start, SimState *before, int press, float dodge_x,
                        int *unlearned, int *lag)
{
    SimState b = *before;
    SimStep step;

    for (int n = 0; n < LL_AI_MAX_STEPS; n++)
    {
        Sim_Step(fp, start, &b, n == 0 ? press : -1, dodge_x, &step);

        // its frames must have been seen, and its bottom too once the lock
        // is over
        if (!step.ecb->seen || (b.lock <= 0 && !step.ecb->has_bottom))
        {
            *unlearned = 1;
            return -1;
        }
        if (step.landed)
        {
            // touching down on the locked bottom isn't an interrupt
            if (!step.first_ecb)
                return -1;
            *lag = step.ecb->aerial_lag;
            return n;
        }
        if (!b.ecb_pending)
            return -1; // its ECB is in use and stayed in the air
    }
    return -1;
}

// Try every aerial and both horizontal airdodges on frame k. Aerials and the
// airdodge can't be pressed out of an aerial (until its IASA, which isn't
// tracked) or an airdodge, but jumps and falls can on any frame. An aerial
// pressed on the frame you'd land anyway keeps falling and lands anyway, so
// it isn't an interrupt; a horizontal airdodge stops the fall, so it's
// tried on that frame too.
static void Branch_Actions(FighterData *fp, SimStart *start, SimState *before, Prediction *p, int k, int landing)
{
    int ts = p->ts[k];
    if (Tracked_IsAerial(ts) || ts == TS_ESCAPEAIR)
        return;

    if (!landing)
    {
        for (int a = TS_AIRN; a <= TS_AIRLW; a++)
        {
            int unlearned = 0, lag = 0;
            int n = Branch_Press(fp, start, before, a, 0, &unlearned, &lag);
            if (unlearned)
                p->ai_unlearned |= AERIAL_BIT(a);
            if (n < 0)
                continue;
            p->ai_mask[k] |= AERIAL_BIT(a);
            p->ai_delay[k] = n;
            if (lag)
                p->ai_lag_mask[k] |= AERIAL_BIT(a);
        }
    }

    for (int d = 0; d < 2; d++)
    {
        int unlearned = 0, lag = 0;
        int n = Branch_Press(fp, start, before, TS_ESCAPEAIR, d == 0 ? 1.f : -1.f, &unlearned, &lag);
        if (unlearned)
            p->wl_unlearned = 1;
        if (n >= 0)
            p->wl_mask[k] |= d == 0 ? DODGE_RIGHT : DODGE_LEFT;
    }
}

// Which aerial interrupts are worth showing, and the first window of each
// kind. An AI that lands with aerial lag (uair, or a lock that ran into a
// fair's or bair's lag frames) never helps, and one that finishes its
// landing lag barely sooner than just holding would (an aerial near the end
// of a fall) isn't worth the press. The AI Filter option can show them all.
static void Windows_Summarize(FighterData *fp, Prediction *p)
{
    int normal_lag = (int)fp->attr.normal_landing_lag;
    int hold_done = p->land_frame ? p->land_frame + p->lag : 2 * LL_SIM_FRAMES;
    int last_ai = p->land_frame ? p->land_frame - 1 : p->num;
    int last_wl = p->land_frame ? p->land_frame : p->num;

    p->ai_first = 0;
    p->ai_width = 0;
    p->ai_aerials = 0;
    for (int k = 1; k <= last_ai; k++)
    {
        u8 m = p->ai_mask[k];
        if (!ai_show_all)
        {
            m &= ~p->ai_lag_mask[k];
            if (m && hold_done - (k + p->ai_delay[k] + normal_lag) < LL_AI_MIN_GAIN)
                m = 0;
        }
        p->ai_show[k] = m;

        if (m && (p->ai_first == 0 || k == p->ai_first + p->ai_width))
        {
            if (p->ai_first == 0)
                p->ai_first = k;
            p->ai_width++;
            p->ai_aerials |= m;
        }
    }

    p->wl_first = 0;
    p->wl_width = 0;
    p->wl_dirs = 0;
    for (int k = 1; k <= last_wl; k++)
    {
        u8 m = p->wl_mask[k];
        if (m && (p->wl_first == 0 || k == p->wl_first + p->wl_width))
        {
            if (p->wl_first == 0)
                p->wl_first = k;
            p->wl_width++;
            p->wl_dirs |= m;
        }
    }
}

// Simulate keeping the stick where it is and pressing nothing. With
// branches, also try every aerial and airdodge on every frame along the way.
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
    p->ai_show[0] = 0;
    p->ai_unlearned = 0;
    p->wl_mask[0] = 0;
    p->wl_unlearned = 0;

    for (int k = 1; k <= LL_SIM_FRAMES; k++)
    {
        SimState before = s;
        SimStep step;
        Sim_Step(fp, start, &s, -1, 0, &step);

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
        p->ai_delay[k] = 0;
        p->ai_show[k] = 0;
        p->wl_mask[k] = 0;
        p->num = k;

        if (branches && (before.lock > 0 || Floor_Near(s.x, s.y + s.bottom)))
            Branch_Actions(fp, start, &before, p, k, step.landed);

        if (step.landed)
        {
            p->land_frame = k;
            p->land_ecb = *step.ecb;
            p->land_kind = Landing_Kind(fp, &s, step.ecb, step.first_ecb, &p->lag, &p->lcancel_lag);
            break;
        }
    }

    Windows_Summarize(fp, p);
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
void Event_ChangeScript(GOBJ *menu, int value);

static const char *speed_names[] = {"1", "5/6", "2/3", "1/2", "1/4"};
static const float speed_values[] = {1.f, 5.f / 6.f, 2.f / 3.f, 1.f / 2.f, 1.f / 4.f};
static const char *preview_names[] = {"Both", "Full hop", "Short hop", "Off"};
static const char *panel_side_names[] = {"Auto", "Right", "Left"};
static const char *cue_names[] = {"AI + waveland", "AI only", "Waveland only"};
static const char *sound_names[] = {"Hit and miss", "Hit only", "Off"};
static const char *ai_filter_names[] = {"Useful", "All"};
static const char *adv_button_names[] = {"L", "Z", "X", "Y", "R"};
static const int adv_button_masks[] = {HSD_TRIGGER_L, HSD_TRIGGER_Z, HSD_BUTTON_X, HSD_BUTTON_Y, HSD_TRIGGER_R};
#define LL_SCRIPT_MAX 48 // scripts read from the script file
static const char *script_names[LL_SCRIPT_MAX + 2] = {"Off"}; // and All

enum cue_kind
{
    CUES_BOTH,
    CUES_AI,
    CUES_WAVELAND,
};

enum sound_kind
{
    SOUNDS_ALL,
    SOUNDS_HIT,
    SOUNDS_OFF,
};

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
    OPT_CUES,
    OPT_AI_FILTER,
    OPT_PREVIEW,
    OPT_RING,
    OPT_BEEPS,
    OPT_PANEL,
    OPT_PANEL_SIDE,
    OPT_SOUND,
    OPT_SPEED,
    OPT_FRAME_ADV,
    OPT_ADV_BUTTON,
    OPT_COLL,
    OPT_LOG,
    OPT_SCRIPT,
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
                 "NIL, pink for an aerial interrupt, white for a",
                 "perfect waveland, thin gray for anything else."},
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
        .name = "Cues",
        .value_num = countof(cue_names),
        .values = cue_names,
        .desc = {"Which windows to mark on the path: pink where an",
                 "aerial lands you (AI), white where a sideways",
                 "airdodge lands you with full speed (perfect",
                 "waveland), or both."},
    },
    {
        .kind = OPTKIND_STRING,
        .name = "AI Filter",
        .value_num = countof(ai_filter_names),
        .values = ai_filter_names,
        .desc = {"Useful hides aerial interrupts that save fewer",
                 "than 4 frames over just landing, and ones that",
                 "land with aerial lag (uair). All shows them all."},
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
        .desc = {"Count down to the next window: a pink ring closes",
                 "in on Falcon (AI), white arrows slide in to posts",
                 "at his feet (waveland). Press while it's lit. A",
                 "thin target is 1 frame, a thick one is longer."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Timing Beeps",
        .desc = {"Beep 20 and 10 frames before the next window,",
                 "and on the frame to press."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Info Panel",
        .val = 1,
        .desc = {"Show the prediction, the next window, and your",
                 "last landing. Exact counts how often the",
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
        .kind = OPTKIND_STRING,
        .name = "Sounds",
        .value_num = countof(sound_names),
        .values = sound_names,
        .desc = {"Chime when you land a NIL, an AI or a perfect",
                 "waveland. Buzz when a window passes without the",
                 "press, or you press too early or the wrong aerial."},
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
        .name = "Frame Advance",
        .desc = {"Freeze the game and step one frame per press of",
                 "the Advance Button, or hold it to keep going.",
                 "D-pad down turns this on and off at any time."},
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Advance Button",
        .value_num = countof(adv_button_names),
        .values = adv_button_names,
        .desc = {"The button that steps a frame while Frame",
                 "Advance is on. The game doesn't see it."},
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
        .kind = OPTKIND_STRING,
        .name = "Script",
        .value_num = 1,
        .values = script_names,
        .desc = {"For testing: play inputs from TM/llscript.txt,",
                 "from a fixed spot and exact to the frame, with",
                 "Debug Log on. D-pad left plays it again. A shot",
                 "step freezes the game until D-pad down."},
        .OnChange = Event_ChangeScript,
    },
    {
        .kind = OPTKIND_FUNC,
        .name = "Forget Learned ECBs",
        .desc = {"Falcon's jumps, falls, aerials and airdodge are",
                 "built in. This forgets what was learned while",
                 "playing and goes back to the built-in data."},
        .OnSelect = Event_ClearLearned,
    },
    {
        .kind = OPTKIND_INFO,
        .name = "Help",
        .desc = {"Pink bar: an aerial there lands you (arrows: the",
                 "C-stick aerials, square: nair). White bar: sideways",
                 "airdodge there. Dim bars come after the next one.",
                 "Gray: still learning. Short hop path: ticked line."},
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
static Vec2 prev_vel;
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

// timing ring (AI) and slide-in arrows (waveland), and beeps for the next
// window of either
#define LL_RING_FRAMES 24 // the countdown starts this many frames before the press
static int ring_frames; // frames until the aerial press, 0 = no ring
static int slide_frames; // frames until the airdodge press, 0 = none
static int ring_wide;    // the window lasts more than one frame
static int slide_wide;
static u8 slide_dirs;    // DODGE_RIGHT / DODGE_LEFT
static Vec2 ring_center;  // Falcon's body
static Vec2 slide_center; // Falcon's ECB bottom
static int beep_target = -100;
static int beep_done;

// last frame's next windows, to tell when one was skipped or missed
static int prev_ai_first;
static int prev_wl_first;
static u8 prev_ai_now;  // aerials that interrupt if pressed on this frame
static u8 prev_wl_now;  // airdodge directions that do

static int panel_left;

// HUD
static int stat_total;
static int stat_exact;
static char text_predict[32] = "-";
static char text_ai[32] = "-";
static char text_last[32] = "-";
static char text_exact[32] = "-";
static char *panel_labels[] = {"Prediction", "Next window", "Last landing", "Exact"};
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
    sprintf(buf, "LandingLab common: fall lean deadzone %.5f rate %.5f, max jumps %d\n",
            common_fall_lean_deadzone, common_fall_lean_rate, fp->attr.max_jumps);
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

    // a fall leans its pose toward FallF or FallB with the drift speed
    // (ftCo_Fall_Anim_Inner): which one, and how far
    if (ts == TS_FALL || ts == TS_FALLAERIAL)
    {
        float weight;
        memcpy(&weight, &fp->state_var.state_var2, sizeof(weight));
        sprintf(buf, "LLLEAN %d sm %d w %.5f\n", event_vars->game_timer, fp->state_var.state_var1, weight);
        Log(buf);
    }
}

// FallSpecial (after an airdodge or up-B) isn't predicted yet; its frames are
// logged in the same form so it can be built in later. It leans with drift
// like the other falls (ftCo_Fall_Anim_Inner, smid and blend in the same
// state variables).
static void Log_FallSpecial(FighterData *fp, int sid, int frame)
{
    char buf[256];
    CollData *cd = &fp->coll_data;
    static const char *names[] = {"FallSpecial", "FallSpecialF", "FallSpecialB"};

    sprintf(buf, "LL %d %s f%d pos %.4f %.4f vel %.5f %.5f ff%d lock%d ecb top %.4f bot %.4f l %.4f r %.4f side %.4f used bot %.4f ac%d stick %.4f %.4f face %d\n",
            event_vars->game_timer, names[sid - ASID_FALLSPECIAL], frame,
            fp->phys.pos.X, fp->phys.pos.Y, fp->phys.self_vel.X, fp->phys.self_vel.Y,
            fp->flags.is_fastfall, cd->u.ecb_bot_lock_frames,
            cd->ecbCurr_top.Y, cd->ecbCurr_bot.Y, cd->ecbCurr_left.X, cd->ecbCurr_right.X, cd->ecbCurr_right.Y,
            cd->ecbCurrCorrect_bot.Y, fp->ftcmd_var.flag0 != 0,
            fp->input.lstick.X, fp->input.lstick.Y, fp->facing_direction > 0 ? 1 : -1);
    Log(buf);

    float weight;
    memcpy(&weight, &fp->state_var.state_var2, sizeof(weight));
    sprintf(buf, "LLLEAN %d sm %d w %.5f\n", event_vars->game_timer, fp->state_var.state_var1, weight);
    Log(buf);
}

// A jumpsquat frame: animation frame, position, ground speed, stick and
// buttons, so a ground preview can be replayed.
static void Log_Squat(FighterData *fp)
{
    char buf[200];
    sprintf(buf, "LLSQ %d f%.2f pos %.4f %.4f gvel %.5f stick %.4f %.4f held %x face %d\n",
            event_vars->game_timer, fp->state.frame, fp->phys.pos.X, fp->phys.pos.Y,
            fp->phys.self_vel_ground.X, fp->input.lstick.X, fp->input.lstick.Y, fp->input.held,
            fp->facing_direction > 0 ? 1 : -1);
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

static int Cues_Ai(void)
{
    return Options_Main[OPT_CUES].val != CUES_WAVELAND;
}

static int Cues_Waveland(void)
{
    return Options_Main[OPT_CUES].val != CUES_AI;
}

static char *Append(char *t, const char *str)
{
    sprintf(t, "%s", str);
    return t + strlen(t);
}

// The next window of each kind: the aerials that interrupt there and how
// many frames it lasts, like "NFD 2f", and "WL 1f" for a perfect waveland.
// Stays the same while the windows come closer.
static void Text_Windows(Prediction *p)
{
    static const char letters[] = "NFBUD";
    char *t = text_ai;
    int ai = Cues_Ai() && p->ai_first;
    int wl = Cues_Waveland() && p->wl_first;

    text_ai[0] = 0;
    if (ai)
    {
        if (p->ai_first >= p->uncertain_from)
            t = Append(t, "AI ?");
        else
        {
            for (int a = 0; a < 5; a++)
            {
                if (p->ai_aerials & (1 << a))
                    *t++ = letters[a];
            }
            sprintf(t, " %df", p->ai_width);
            t += strlen(t);
        }
    }
    if (wl)
    {
        if (ai)
            t = Append(t, ", ");
        if (p->wl_first >= p->uncertain_from)
            t = Append(t, "WL ?");
        else
        {
            sprintf(t, "WL %df", p->wl_width);
            t += strlen(t);
        }
    }
    if (ai || wl)
        return;

    // nothing ahead: name what's still to learn
    u8 learn = Cues_Ai() ? p->ai_unlearned : 0;
    int learn_wl = Cues_Waveland() && p->wl_unlearned;
    if (!learn && !learn_wl)
    {
        sprintf(text_ai, "-");
        return;
    }
    t = Append(t, "Learn");
    for (int a = 0; a < 5; a++)
    {
        if (learn & (1 << a))
        {
            *t++ = ' ';
            *t++ = letters[a];
        }
    }
    *t = 0;
    if (learn_wl)
        Append(t, " dodge");
}

// "NIL", "AI", "PWL", "?" while learning, "-" for anything else.
static const char *Kind_Short(Prediction *p)
{
    if (!p->land_frame)
        return "-";
    if (p->uncertain_from <= p->land_frame)
        return "?";
    if (p->land_kind == LAND_PERFECT_WL)
        return "PWL";
    if (Kind_Highlighted(p->land_kind))
        return land_kind_names[p->land_kind];
    return "-";
}

// The next windows as letters: the AI's aerials, then W for a perfect
// waveland, like "NFDW".
static void Window_Letters(Prediction *p, char *out)
{
    static const char letters[] = "NFBUD";
    int n = 0;
    int ai = Cues_Ai() && p->ai_first;
    int wl = Cues_Waveland() && p->wl_first;

    if (ai && p->ai_first >= p->uncertain_from)
        out[n++] = '?';
    else if (ai)
    {
        for (int a = 0; a < 5; a++)
        {
            if (p->ai_aerials & (1 << a))
                out[n++] = letters[a];
        }
    }
    if (wl)
        out[n++] = p->wl_first >= p->uncertain_from ? '?' : 'W';
    if (n == 0)
        out[n++] = '-';
    out[n] = 0;
}

// On the ground: what a full hop (FH) and a short hop (SH) would give.
static void Text_Preview(void)
{
    char fh[8], sh[8];
    Window_Letters(pred_fh, fh);
    Window_Letters(pred_sh, sh);

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

// The horizontal direction of an airdodge with the stick at (x, y): only a
// fully sideways stick gives a dodge with no vertical speed (ftCo_80099A9C
// uses the stick's exact angle; inside the deadzone it's a neutral dodge).
// 0 for any other angle.
static int Dodge_Dir(float x, float y)
{
    if (y != 0 || fabs(x) < common_dodge_deadzone.X)
        return 0;
    return x > 0 ? DODGE_RIGHT : DODGE_LEFT;
}

static int Jump_Or_Fall(int ts)
{
    return ts >= 0 && !Tracked_IsAerial(ts) && ts != TS_ESCAPEAIR;
}

#define LL_NEAR_MISS 4 // how far from a window a press still counts as aimed at it

// How many frames a press on frame k missed the nearest window in mask by:
// negative is early, positive is late, 0 is none within LL_NEAR_MISS.
static int Window_Offset(Prediction *p, u8 *mask, int k, u8 bit)
{
    for (int d = 1; d <= LL_NEAR_MISS; d++)
    {
        if (k + d <= p->num && k + d < p->uncertain_from && (mask[k + d] & bit))
            return -d;
        if (k - d >= 1 && (mask[k - d] & bit))
            return d;
    }
    return 0;
}

// An aerial or sideways airdodge pressed in the air, on a frame where the
// prediction said it would touch down, should have landed right away. If its
// ECB isn't in use yet (lock), the landing itself is judged later instead.
// Pressed a few frames off a window instead, the panel says by how much.
static void Press_CheckMissed(FighterData *fp, int ts)
{
    if (!seg_valid || !Jump_Or_Fall(prev_ts) || Jump_Or_Fall(ts))
        return;
    if (fp->coll_data.u.ecb_bot_lock_frames > 0)
        return;

    int k = event_vars->game_timer - seg_start_timer;
    if (k < 1 || k > pred_seg->num || k >= pred_seg->uncertain_from)
        return;

    int dodge = ts == TS_ESCAPEAIR;
    u8 bit = dodge ? Dodge_Dir(fp->input.lstick.X, fp->input.lstick.Y) : AERIAL_BIT(ts);
    u8 *mask = dodge ? pred_seg->wl_mask : pred_seg->ai_mask;
    u8 *shown = dodge ? pred_seg->wl_mask : pred_seg->ai_show;
    char *name = dodge ? "WL" : "AI";
    char buf[200];
    if (!bit)
        return;
    if (!(mask[k] & bit))
    {
        if (!(dodge ? Cues_Waveland() : Cues_Ai()))
            return;
        int off = Window_Offset(pred_seg, shown, k, bit);
        if (off == 0)
            return;
        sprintf(text_last, "No %s, %df %s", name, off < 0 ? -off : off, off < 0 ? "early" : "late");
        sprintf(buf, "LandingLab press: %s at %d stayed in the air, %df %s for the window (from %d)\n",
                tracked_state_names[ts], event_vars->game_timer, off < 0 ? -off : off, off < 0 ? "early" : "late",
                seg_start_timer);
        Log(buf);
        return;
    }

    char *what;
    if (dodge)
    {
        what = "perfect waveland";
        sprintf(text_last, "No WL, predicted");
    }
    else
    {
        what = "aerial interrupt";
        sprintf(text_last, "No AI, predicted");
    }

    stat_total++;
    Text_Exact();

    sprintf(buf, "LandingLab ai miss: %s at %d x %.4f y %.4f stayed in the air, predicted a %s (from %d)\n",
            tracked_state_names[ts], event_vars->game_timer, fp->phys.pos.X, fp->phys.pos.Y, what, seg_start_timer);
    Log(buf);
}

// Buzz when a window passes without its press, or an aerial or airdodge
// comes while the countdown runs but doesn't touch down. Called on tracked
// air frames, after pred_live is updated.
static void Window_Feedback(FighterData *fp, int ts)
{
    if (Options_Main[OPT_SOUND].val != SOUNDS_ALL)
        return;

    int miss = 0;
    if (Jump_Or_Fall(prev_ts) && Tracked_IsAerial(ts))
        miss = prev_ai_first && prev_ai_first <= LL_RING_FRAMES && !(prev_ai_now & AERIAL_BIT(ts));
    else if (Jump_Or_Fall(prev_ts) && ts == TS_ESCAPEAIR)
        miss = prev_wl_first && prev_wl_first <= LL_RING_FRAMES &&
               !(prev_wl_now & Dodge_Dir(fp->input.lstick.X, fp->input.lstick.Y));
    else if (Jump_Or_Fall(ts))
    {
        // this was the frame to press, and pressing on the next one is too late
        Prediction *p = pred_live;
        int ai_next = Cues_Ai() && p->ai_first == 1 && p->uncertain_from > 1;
        int wl_next = Cues_Waveland() && p->wl_first == 1 && p->uncertain_from > 1;
        miss = (prev_ai_first == 1 || prev_wl_first == 1) && !ai_next && !wl_next;
    }
    if (miss)
        SFX_PlayCommon(3);
}

// What Window_Feedback needs from this frame's prediction on the next one.
static void Window_Remember(Prediction *p)
{
    prev_ai_first = Cues_Ai() && p->ai_first < p->uncertain_from ? p->ai_first : 0;
    prev_wl_first = Cues_Waveland() && p->wl_first < p->uncertain_from ? p->wl_first : 0;
    prev_ai_now = p->num >= 1 ? p->ai_mask[1] : 0;
    prev_wl_now = p->num >= 1 ? p->wl_mask[1] : 0;
}

static void Window_Forget(void)
{
    prev_ai_first = 0;
    prev_wl_first = 0;
    prev_ai_now = 0;
    prev_wl_now = 0;
}

#define LL_GHOST_FRAMES 90

// The fighter left the tracked air states this frame: judge the landing.
static void Landing_Resolve(FighterData *fp)
{
    int sid = fp->state_id;
    int landing_air = sid >= ASID_LANDINGAIRN && sid <= ASID_LANDINGAIRLW;
    int is_landing = sid == ASID_LANDING || sid == ASID_LANDINGFALLSPECIAL || landing_air;

    sprintf(text_ai, "-");
    if (sid != ASID_WAIT && !is_landing)
    {
        // ledge grab, special, ...: not a landing
        live_visible = 0;
        seg_valid = 0;
        sprintf(text_predict, "-");
        return;
    }

    // An aerial or airdodge pressed on this very frame from a jump or fall:
    // its ECB touched down at once, so the event never saw it in the air. An
    // aerial is only an interrupt if keeping on holding wouldn't have landed
    // here too; a sideways airdodge stops the fall, so it counts either way.
    int pressed = -1; // the aerial
    int dodge = -1;   // the airdodge's direction, 0 = not sideways
    if (Jump_Or_Fall(prev_ts) && sid == ASID_LANDINGFALLSPECIAL)
        dodge = Dodge_Dir(fp->input.lstick.X, fp->input.lstick.Y);
    else if (is_landing && Jump_Or_Fall(prev_ts) && pred_live->land_frame != 1)
        pressed = Aerial_Pressed(fp);

    int kind;
    if (sid == ASID_WAIT)
        kind = LAND_NIL;
    else if (sid == ASID_LANDINGFALLSPECIAL)
    {
        // perfect: a sideways dodge whose own ECB touched down the first
        // time it was used, now or as the lock ran out
        int perfect = dodge > 0 || (prev_ts == TS_ESCAPEAIR && prev_lock == 1 && prev_vel.Y == 0 && prev_vel.X != 0);
        kind = perfect ? LAND_PERFECT_WL : LAND_WAVELAND;
    }
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

    // keep the attempt on screen for a moment, but only when a NIL, an
    // aerial interrupt or a perfect waveland was predicted or happened
    ghost_visible = seg_valid && (Kind_Highlighted(kind) || Kind_Highlighted(pred_seg->land_kind));
    ghost_timer = LL_GHOST_FRAMES;

    // an AI into aerial lag (uair) is still an AI, but nothing to cheer
    int hit = kind == LAND_NIL || kind == LAND_PERFECT_WL || (kind == LAND_AI && !landing_air);
    if (Options_Main[OPT_SOUND].val != SOUNDS_OFF && hit)
        SFX_PlayRaw(303, 255, 128, 20, 3); // laserland's success sound

    char buf[200];
    int k = event_vars->game_timer - seg_start_timer;

    if ((pressed >= 0 || dodge > 0) && seg_valid)
    {
        // judged against the windows the prediction drew
        int in_range = k >= 1 && k <= pred_seg->num;
        int predicted, learning;
        if (pressed >= 0)
        {
            predicted = in_range && (pred_seg->ai_mask[k] & AERIAL_BIT(pressed));
            learning = !predicted && (!in_range || k >= pred_seg->uncertain_from ||
                                      (pred_seg->ai_unlearned & AERIAL_BIT(pressed)));
        }
        else
        {
            predicted = in_range && (pred_seg->wl_mask[k] & dodge);
            learning = !predicted && (!in_range || k >= pred_seg->uncertain_from || pred_seg->wl_unlearned);
        }
        const char *name = pressed >= 0 ? "AI" : land_kind_names[kind];
        if (learning)
            sprintf(text_last, "%s (learning)", name);
        else
        {
            stat_total++;
            if (predicted)
                stat_exact++;
            sprintf(text_last, "%s, %s", name, predicted ? "as predicted" : "not predicted");
            Text_Exact();
        }

        sprintf(buf, "LandingLab landing: %s with %s pressed at %d x %.4f y %.4f, %s (from %d)%s\n",
                land_kind_names[kind], pressed >= 0 ? tracked_state_names[pressed] : (dodge == DODGE_RIGHT ? "airdodge right" : "airdodge left"),
                event_vars->game_timer, fp->phys.pos.X, fp->phys.pos.Y,
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

    // Something "keep holding" couldn't know happened on the landing frame
    // itself, so the landing isn't judged: the stick moved into or out of
    // fastfall or platform drop (the usual late fastfall), or an aerial or
    // an angled airdodge was pressed as Falcon touched down anyway.
    int fastfall = Stick_Down(fp->input.lstick.Y) != seg_stick_down || (u8)fp->input.timer_lstick_tilt_y < prev_tilt_timer;
    int pressed_late = Jump_Or_Fall(prev_ts) && (landing_air || sid == ASID_LANDINGFALLSPECIAL);
    int late = fastfall || pressed_late || Stick_Drop(fp->input.lstick.Y) != seg_stick_drop;

    if (learning)
        sprintf(text_last, "%s (learning)", land_kind_names[kind]);
    else if (late)
        sprintf(text_last, "%s, %s", land_kind_names[kind],
                pressed_late ? "pressed late" : fastfall ? "FF on landing" : "stick moved");
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

    sprintf(buf, "LandingLab landing: %s at %d x %.4f, predicted %s at %d x %.4f (from %d)%s%s\n",
            land_kind_names[kind], event_vars->game_timer, fp->phys.pos.X,
            land_kind_names[pred_seg->land_kind], predicted, pred_seg->pos[pred_seg->land_frame].X,
            seg_start_timer, learning ? " learning" : "", late ? " late-input" : "");
    Log(buf);
}

///////////////////////
/// Drawing         ///
///////////////////////

static const GXColor color_neutral = {150, 150, 150, 255};
static const GXColor color_body = {130, 150, 200, 255};
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

// A path with a short tick across it on every frame: the short hop
// preview, so it stays readable where it runs along the full hop's solid
// line (a straight-up jump). The spacing of the ticks shows the speed.
#define LL_TICK 1.2f

static void Draw_Ticked(Vec2 *pos, float *bottom, int from, int to, GXColor color, u8 size)
{
    int count = to - from + 1;
    if (count < 2)
        return;

    Draw_Path(pos, bottom, from, to, color, 8);
    event_vars->GFX_Start(count * 2, (GFX_Params){.shape = GX_LINES, .size = size});
    for (int i = from; i <= to; i++)
    {
        int a = i > from ? i - 1 : i;
        int b = i < to ? i + 1 : i;
        float dx = pos[b].X - pos[a].X;
        float dy = pos[b].Y + bottom[b] - pos[a].Y - bottom[a];
        float len = sqrtf(dx * dx + dy * dy);
        float nx = len > 0.001f ? -dy / len : 0.f;
        float ny = len > 0.001f ? dx / len : 1.f;
        float x = pos[i].X;
        float y = pos[i].Y + bottom[i];
        GFX_AddVtx(x - nx * LL_TICK, y - ny * LL_TICK, 0, color);
        GFX_AddVtx(x + nx * LL_TICK, y + ny * LL_TICK, 0, color);
    }
}

static void Draw_Line(Prediction *p, int from, int to, GXColor color, u8 size, int ticked)
{
    if (ticked)
        Draw_Ticked(p->pos, p->bottom, from, to, color, size);
    else
        Draw_Path(p->pos, p->bottom, from, to, color, size);
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

// Windows after the next one of their kind: the same color at 40%
// (premultiplied).
static GXColor Color_Dim(GXColor c)
{
    c.r = c.r * 2 / 5;
    c.g = c.g * 2 / 5;
    c.b = c.b * 2 / 5;
    c.a = 102;
    return c;
}

// A thick stretch of the path over frames k to e, from half a frame before
// the window to half a frame after it.
static void Draw_Bar(Prediction *p, int k, int e, GXColor color, u8 size)
{
    int before = k - 1;
    int after = e < p->num ? e + 1 : e;
    event_vars->GFX_Start(e - k + 3, (GFX_Params){.shape = GX_LINESTRIP, .size = size});
    GFX_AddVtx((p->pos[before].X + p->pos[k].X) / 2,
               (p->pos[before].Y + p->bottom[before] + p->pos[k].Y + p->bottom[k]) / 2, 0, color);
    for (int i = k; i <= e; i++)
        GFX_AddVtx(p->pos[i].X, p->pos[i].Y + p->bottom[i], 0, color);
    GFX_AddVtx((p->pos[e].X + p->pos[after].X) / 2,
               (p->pos[e].Y + p->bottom[e] + p->pos[after].Y + p->bottom[after]) / 2, 0, color);
}

// The windows on the path. A perfect waveland window is a wide ice-white
// bar; an aerial interrupt window is a narrower pink bar drawn over it, with
// the aerials that work at its start. Where both work, the pink sits inside
// the white. The next window of each kind is bright, later ones are dim.
static void Draw_Windows(Prediction *p)
{
    int last_ai = p->land_frame ? p->land_frame - 1 : p->num;
    int last_wl = p->land_frame ? p->land_frame : p->num;

    if (Cues_Waveland())
    {
        int index = 0;
        for (int k = 1; k <= last_wl; k++)
        {
            if (!p->wl_mask[k])
                continue;
            int e = k;
            while (e < last_wl && p->wl_mask[e + 1])
                e++;

            GXColor color = k >= p->uncertain_from ? color_learning : land_kind_colors[LAND_PERFECT_WL];
            if (index++ > 0)
                color = Color_Dim(color);
            Draw_Bar(p, k, e, color, 108);
            k = e;
        }
    }

    if (Cues_Ai())
    {
        int index = 0;
        for (int k = 1; k <= last_ai; k++)
        {
            if (!p->ai_show[k])
                continue;
            int e = k;
            u8 mask = p->ai_show[k];
            while (e < last_ai && p->ai_show[e + 1])
                mask |= p->ai_show[++e];

            GXColor color = k >= p->uncertain_from ? color_learning : land_kind_colors[LAND_AI];
            if (index++ > 0)
                color = Color_Dim(color);
            Draw_Bar(p, k, e, color, 60);
            Draw_AerialArrows(p->pos[k].X, p->pos[k].Y + p->bottom[k], mask, p->facing, color);
            k = e;
        }
    }
}

static void Draw_Prediction(Prediction *p, int body, int ticked)
{
    int last = p->land_frame ? p->land_frame : p->num;
    int certain = p->land_frame && p->uncertain_from > p->land_frame;
    int highlight = certain && Kind_Highlighted(p->land_kind);

    if (body && Options_Main[OPT_BODY].val)
        Draw_BodyPath(p, last);

    if (highlight)
        Draw_Line(p, 0, last, land_kind_colors[p->land_kind], 24, ticked);
    else
    {
        int known = p->uncertain_from - 1;
        if (known > last)
            known = last;
        if (known < 0)
            known = 0;
        Draw_Line(p, 0, known, color_neutral, 12, ticked);
        Draw_Line(p, known, last, color_learning, 12, ticked);
    }

    Draw_Windows(p);

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

// The countdowns share one look. Before a window, the target says what kind
// it is: thin with small ticks for a 1-frame window, thick for a longer one.
// While a press works (the frame on screen is the one to press on, so the
// press lands on the next), the target lights up and stays lit until the
// window is gone.
#define LL_RING_INNER 4.f
#define LL_RING_STEP 0.75f

// A color at a fraction of its strength, for fills (colors are
// premultiplied).
static GXColor Color_Fill(GXColor c, float a)
{
    c.r = c.r * a;
    c.g = c.g * a;
    c.b = c.b * a;
    c.a = 255 * a;
    return c;
}

// A ring band from r0 out to r1, open in the middle.
static void Draw_Band(float x, float y, float r0, float r1, GXColor color)
{
    event_vars->GFX_Start(LL_CIRCLE_SEGS * 4, (GFX_Params){.shape = GX_QUADS});
    for (int i = 0; i < LL_CIRCLE_SEGS; i++)
    {
        Vec2 *a = &circle[i], *b = &circle[i + 1];
        GFX_AddVtx(x + a->X * r0, y + a->Y * r0, 0, color);
        GFX_AddVtx(x + a->X * r1, y + a->Y * r1, 0, color);
        GFX_AddVtx(x + b->X * r1, y + b->Y * r1, 0, color);
        GFX_AddVtx(x + b->X * r0, y + b->Y * r0, 0, color);
    }
}

// AI: the outer ring closes in on the inner one around Falcon's body and
// meets it on the frame to press. While the window is open the ring glows
// as a band, leaving Falcon himself in plain view.
static void Draw_TimingRing(void)
{
    GXColor color = land_kind_colors[LAND_AI];
    float x = ring_center.X;
    float y = ring_center.Y;
    float r = LL_RING_INNER;

    if (ring_frames <= 1)
    {
        Draw_Band(x, y, r, r + (ring_wide ? 2.f : 1.2f), Color_Fill(color, 0.45f));
        Draw_Circle(x, y, r, color, ring_wide ? 96 : 60);
        return;
    }

    if (ring_wide)
        Draw_Circle(x, y, r, color, 48);
    else
    {
        // thin, with four ticks pointing out: a 1-frame window
        Draw_Circle(x, y, r, color, 12);
        event_vars->GFX_Start(8, (GFX_Params){.shape = GX_LINES, .size = 12});
        for (int i = 0; i < 4; i++)
        {
            Vec2 *c = &circle[(i * 2 + 1) * LL_CIRCLE_SEGS / 8];
            GFX_AddVtx(x + c->X * r, y + c->Y * r, 0, color);
            GFX_AddVtx(x + c->X * (r + 1.f), y + c->Y * (r + 1.f), 0, color);
        }
    }
    Draw_Circle(x, y, r + (ring_frames - 1) * LL_RING_STEP, color, 36);
}

// Waveland: at Falcon's feet, an arrow slides in from the side toward a
// post for each direction that works (the left one points right: dodge
// right), on the same clock as the ring. Low and sideways, so it never sits
// on the ring. While the window is open, the strip between the posts is lit.
#define LL_SLIDE_POST 6.f
#define LL_SLIDE_SIZE 1.6f

static void Draw_SlideArrows(void)
{
    GXColor color = land_kind_colors[LAND_PERFECT_WL];
    float x = slide_center.X;
    float y = slide_center.Y;
    int now = slide_frames <= 1;
    float d = LL_SLIDE_POST + (now ? 0 : (slide_frames - 1) * LL_RING_STEP);
    float h = LL_SLIDE_SIZE;
    float p = LL_SLIDE_POST;

    if (now)
    {
        GXColor fill = Color_Fill(color, 0.3f);
        event_vars->GFX_Start(4, (GFX_Params){.shape = GX_QUADS});
        GFX_AddVtx(x - p, y - h, 0, fill);
        GFX_AddVtx(x - p, y + h, 0, fill);
        GFX_AddVtx(x + p, y + h, 0, fill);
        GFX_AddVtx(x + p, y - h, 0, fill);
    }

    // the posts: thin with a notch for a 1-frame window, thick for longer
    u8 post = now ? (slide_wide ? 72 : 48) : (slide_wide ? 48 : 12);
    event_vars->GFX_Start(4, (GFX_Params){.shape = GX_LINES, .size = post});
    GFX_AddVtx(x - p, y - h, 0, color);
    GFX_AddVtx(x - p, y + h, 0, color);
    GFX_AddVtx(x + p, y - h, 0, color);
    GFX_AddVtx(x + p, y + h, 0, color);
    if (!now && !slide_wide)
    {
        event_vars->GFX_Start(4, (GFX_Params){.shape = GX_LINES, .size = 12});
        GFX_AddVtx(x - p - 0.8f, y + h, 0, color);
        GFX_AddVtx(x - p + 0.8f, y + h, 0, color);
        GFX_AddVtx(x + p - 0.8f, y + h, 0, color);
        GFX_AddVtx(x + p + 0.8f, y + h, 0, color);
    }

    // the arrows, tip toward the post
    for (int side = -1; side <= 1; side += 2)
    {
        if (!(slide_dirs & (side < 0 ? DODGE_RIGHT : DODGE_LEFT)))
            continue;
        float tip = x + side * d;
        float back = tip + side * h;
        event_vars->GFX_Start(3, (GFX_Params){.shape = GX_LINESTRIP, .size = now ? 48 : 30});
        GFX_AddVtx(back, y + h, 0, color);
        GFX_AddVtx(tip, y, 0, color);
        GFX_AddVtx(back, y - h, 0, color);
    }
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
        Draw_Prediction(pred_live, 1, 0);
        if (Options_Main[OPT_RING].val && slide_frames)
            Draw_SlideArrows();
        if (Options_Main[OPT_RING].val && ring_frames)
            Draw_TimingRing();
    }
    else if (ghost_visible)
    {
        Draw_Prediction(pred_seg, 0, 0);
        Draw_Path(actual_pos, actual_bottom, 0, actual_num - 1, color_actual, 12);
    }
    else
    {
        // the short hop is ticked, and only has a body line of its own
        // when the full hop isn't shown
        if (preview_fh)
            Draw_Prediction(pred_fh, 1, 0);
        if (preview_sh)
            Draw_Prediction(pred_sh, !preview_fh, 1);
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
/// Frame advance   ///
///////////////////////

// The game's debug pause, as the lab uses it: while Frame Advance is on the
// game stays paused, and each press of the advance button runs one frame
// (holding it runs frames at full speed after half a second). The pause and
// step checks run every frame, paused or not; the scene resets them when the
// event ends.
#define LL_ADVANCE_HOLD 30

static int Advance_Port(void)
{
    return Fighter_GetControllerPort(0);
}

static int Advance_CheckPause(void)
{
    HSD_Update *update = stc_hsd_update;
    int paused = update->pause_kind & 1;
    return paused != (Options_Main[OPT_FRAME_ADV].val != 0);
}

static int Advance_CheckStep(void)
{
    static int timer;
    int port = Advance_Port();
    HSD_Pad *pad = PadGetMaster(port);
    HSD_Pad *engine = PadGetEngine(port);
    int button = adv_button_masks[Options_Main[OPT_ADV_BUTTON].val];

    if (Pause_CheckStatus(1) == 2 || !(pad->held & button))
    {
        timer = 0;
        return 0;
    }
    timer++;
    if (timer != 1 && timer <= LL_ADVANCE_HOLD)
        return 0;

    // the game doesn't see the advance button
    pad->down &= ~button;
    pad->held &= ~button;
    engine->down &= ~button;
    engine->held &= ~button;
    if (button == HSD_TRIGGER_L)
    {
        pad->triggerLeft = 0;
        pad->ftriggerLeft = 0;
        engine->triggerLeft = 0;
        engine->ftriggerLeft = 0;
    }
    else if (button == HSD_TRIGGER_R)
    {
        pad->triggerRight = 0;
        pad->ftriggerRight = 0;
        engine->triggerRight = 0;
        engine->ftriggerRight = 0;
    }
    return 1;
}

///////////////////////
/// Test scripts    ///
///////////////////////

// Input scripts for testing, read from TM/llscript.txt on the disc. A script
// stands Falcon at a fixed spot, then feeds exact inputs into his controller
// port frame by frame, so it plays out the same way every time. One line per
// step; '#' starts a comment:
//   script <name>              starts a script (the name shows in the menu)
//   start <x> <y> <left|right> stand Falcon on the floor at x, at height y,
//                              facing that way, as if nothing was held
//   air <x> <y> <left|right> [<vx> <vy> [<jumps used> [<lock>]]]
//                              put Falcon in the air at (x, y), falling (out
//                              of jumps: FallAerial), with that speed (0 0),
//                              jumps used (1) and frames his ECB bottom stays
//                              at his feet, as after leaving the ground (0)
//   <n> [inputs]               hold the inputs for n frames (none: let go)
//   wl <offset> [inputs]       keep the stick of the step before until the
//                              next perfect waveland window, then press the
//                              inputs for one frame, offset frames after the
//                              window's first (-1: a frame early)
//   ai <offset> [inputs]       the same for the next aerial interrupt window
//   land <max> [inputs]        hold the inputs until Falcon is on the ground
//   shot [label]               freeze the game after the frame before; D-pad
//                              down goes on, the advance button steps
//   mark [label]               only write a line to the log
//   autorun                    (anywhere) play All, or the only script, as
//                              soon as the event starts
// Inputs: A B X Y Z L R (L and R fully pressed), s:x,y (stick), c:x,y
// (C-stick), lt:v (light press, no click), with x, y, v from -1 to 1. A
// stick value is round(80 v), pulled back onto the rim if it's past it.
// Every script starts from a clean slate: what Falcon's ECB was learned
// while playing is forgotten, so a script plays the same alone or in All.
#define LL_SCRIPT_FILE "TM/llscript.txt"
#define LL_SCRIPT_BYTES 0x30000 // the biggest script file read
#define LL_SCRIPT_WAIT 300 // frames a wl/ai/land step waits at most

enum script_op_kind
{
    SOP_INPUT,
    SOP_START,
    SOP_AIR,
    SOP_WL,
    SOP_AI,
    SOP_LAND,
    SOP_SHOT,
    SOP_MARK,
};

typedef struct ScriptInput
{
    s8 lx, ly, cx, cy; // as the controller reads them, 80 is all the way
    u8 trigger;        // light press, 0 to 140
    u16 buttons;
} ScriptInput;

typedef struct ScriptOp
{
    u8 kind;
    s8 facing;
    s16 count; // frames, window offset, most frames or jumps used
    s16 lock;
    ScriptInput in;
    float x, y, vx, vy;
    char *label;
} ScriptOp;

typedef struct Script
{
    char *name;
    int first;
    int num;
} Script;

static char *script_text;
static ScriptOp *script_ops;
static int script_op_num;
static Script *scripts;
static int script_num;
static int script_shown; // the first ones are listed in the menu
static int script_autorun;

static int script_cur = -1; // running script, -1 = none
static int script_pc;       // its current step
static int script_left;     // frames left in that step
static int script_wait;     // frames a wl/ai/land step has waited
static int script_air;      // Falcon has been in the air during a land step
static int script_window;   // the frame a wl/ai step's window opened, -1 = not yet
static int script_width;    // how many frames the prediction gave it
static int script_pending;  // start the chosen script on the next frame
static ScriptInput script_last; // the last frame's inputs
static int live_timer;      // the frame pred_live was made on

static int Script_Is(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

static char *Script_Skip(char *t)
{
    while (*t == ' ' || *t == '\t' || *t == '\r')
        t++;
    return t;
}

// The rest of a line without the spaces around it.
static char *Script_Trim(char *t)
{
    t = Script_Skip(t);
    char *e = t + strlen(t);
    while (e > t && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
        *--e = 0;
    return t;
}

// The next word of a line, cut off in place, or 0 at the end.
static char *Script_Word(char **t)
{
    char *s = Script_Skip(*t);
    if (*s == 0)
        return 0;
    char *e = s;
    while (*e && *e != ' ' && *e != '\t' && *e != '\r')
        e++;
    if (*e)
        *e++ = 0;
    *t = e;
    return s;
}

static float Script_Number(char **t)
{
    char *s = *t;
    float sign = 1.f, v = 0.f;
    if (*s == '-' || *s == '+')
        sign = *s++ == '-' ? -1.f : 1.f;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    if (*s == '.')
    {
        float f = 0.1f;
        for (s++; *s >= '0' && *s <= '9'; s++, f *= 0.1f)
            v += (*s - '0') * f;
    }
    *t = s;
    return v * sign;
}

static int Script_Pair(char *t, float *x, float *y)
{
    *x = Script_Number(&t);
    if (*t++ != ',')
        return 0;
    *y = Script_Number(&t);
    return *t == 0;
}

static s8 Script_Round(float v)
{
    return (s8)(v < 0 ? v - 0.5f : v + 0.5f);
}

static int Script_Stick(char *t, s8 *x, s8 *y)
{
    float fx, fy;
    if (!Script_Pair(t, &fx, &fy))
        return 0;
    fx *= 80.f;
    fy *= 80.f;
    float r = sqrtf(fx * fx + fy * fy);
    if (r > 80.f)
    {
        fx *= 80.f / r;
        fy *= 80.f / r;
    }
    *x = Script_Round(fx);
    *y = Script_Round(fy);
    if (*x * *x + *y * *y > 80 * 80)
    {
        *x = (s8)fx;
        *y = (s8)fy;
    }
    return 1;
}

static int Script_Input(char *w, ScriptInput *in)
{
    static const char names[] = "ABXYZLR";
    static const u16 bits[] = {HSD_BUTTON_A, HSD_BUTTON_B, HSD_BUTTON_X, HSD_BUTTON_Y,
                               HSD_TRIGGER_Z, HSD_TRIGGER_L, HSD_TRIGGER_R};
    if (w[1] == 0)
    {
        for (int i = 0; i < 7; i++)
        {
            if (w[0] == names[i])
            {
                in->buttons |= bits[i];
                return 1;
            }
        }
        return 0;
    }
    if (w[0] == 's' && w[1] == ':')
        return Script_Stick(w + 2, &in->lx, &in->ly);
    if (w[0] == 'c' && w[1] == ':')
        return Script_Stick(w + 2, &in->cx, &in->cy);
    if (w[0] == 'l' && w[1] == 't' && w[2] == ':')
    {
        char *t = w + 3;
        float v = Script_Number(&t) * 140.f;
        in->trigger = v < 0 ? 0 : v > 140.f ? 140 : (u8)(v + 0.5f);
        return *t == 0;
    }
    return 0;
}

static void Script_Error(int line, char *what)
{
    char buf[120];
    sprintf(buf, "LLSCRIPT error on line %d: %s\n", line, what);
    Log(buf);
}

static void Script_Parse(void)
{
    char *t = script_text;
    for (int line = 1; *t; line++)
    {
        // cut the line off, and its comment
        char *end = t;
        while (*end && *end != '\n')
            end++;
        char *next = *end ? end + 1 : end;
        *end = 0;
        for (char *c = t; *c; c++)
        {
            if (*c == '#')
            {
                *c = 0;
                break;
            }
        }

        char *rest = t;
        char *w = Script_Word(&rest);
        t = next;
        if (!w)
            continue;

        if (Script_Is(w, "script"))
        {
            // the rest of the line is the name
            char *name = Script_Trim(rest);
            scripts[script_num].name = *name ? name : "Unnamed";
            scripts[script_num].first = script_op_num;
            scripts[script_num].num = 0;
            script_num++;
            continue;
        }
        if (Script_Is(w, "autorun"))
        {
            script_autorun = 1;
            continue;
        }
        if (script_num == 0)
        {
            Script_Error(line, "step before the first script line");
            continue;
        }
        ScriptOp *op = &script_ops[script_op_num];
        memset(op, 0, sizeof(*op));
        int ok = 1;
        int inputs = 0;
        if (Script_Is(w, "start"))
        {
            char *x = Script_Word(&rest), *y = Script_Word(&rest), *f = Script_Word(&rest);
            op->kind = SOP_START;
            ok = x && y && f && (Script_Is(f, "left") || Script_Is(f, "right"));
            if (ok)
            {
                op->x = Script_Number(&x);
                op->y = Script_Number(&y);
                op->facing = f[0] == 'l' ? -1 : 1;
            }
        }
        else if (Script_Is(w, "air"))
        {
            char *x = Script_Word(&rest), *y = Script_Word(&rest), *f = Script_Word(&rest);
            char *vx = Script_Word(&rest), *vy = Script_Word(&rest);
            char *jumps = Script_Word(&rest), *lock = Script_Word(&rest);
            op->kind = SOP_AIR;
            ok = x && y && f && (Script_Is(f, "left") || Script_Is(f, "right")) && (vx == 0) == (vy == 0);
            if (ok)
            {
                op->x = Script_Number(&x);
                op->y = Script_Number(&y);
                op->facing = f[0] == 'l' ? -1 : 1;
                op->vx = vx ? Script_Number(&vx) : 0;
                op->vy = vy ? Script_Number(&vy) : 0;
                op->count = jumps ? (int)Script_Number(&jumps) : 1;
                op->lock = lock ? (int)Script_Number(&lock) : 0;
            }
        }
        else if (Script_Is(w, "shot") || Script_Is(w, "mark"))
        {
            op->kind = w[0] == 's' ? SOP_SHOT : SOP_MARK;
            op->label = Script_Trim(rest);
        }
        else if (Script_Is(w, "wl") || Script_Is(w, "ai") || Script_Is(w, "land"))
        {
            char *n = Script_Word(&rest);
            op->kind = w[0] == 'w' ? SOP_WL : w[0] == 'a' ? SOP_AI : SOP_LAND;
            ok = n != 0;
            if (ok)
                op->count = (int)Script_Number(&n);
            inputs = 1;
        }
        else if (w[0] >= '0' && w[0] <= '9')
        {
            op->kind = SOP_INPUT;
            op->count = (int)Script_Number(&w);
            ok = op->count > 0;
            inputs = 1;
        }
        else
            ok = 0;

        for (char *in; ok && inputs && (in = Script_Word(&rest));)
            ok = Script_Input(in, &op->in);

        if (!ok)
        {
            Script_Error(line, "can't read this step");
            continue;
        }
        script_op_num++;
        scripts[script_num - 1].num++;
    }
}

// Reads the script file if the disc has one, and lists its scripts in the
// Script option, with All after them.
static void Script_Load(void)
{
    char msg[120];
    Options_Main[OPT_SCRIPT].value_num = 1;

    int entry = DVDConvertPathToEntrynum(LL_SCRIPT_FILE);
    if (entry < 0)
        return;
    int size = File_GetSize(LL_SCRIPT_FILE);
    if (size <= 0 || size > LL_SCRIPT_BYTES)
    {
        sprintf(msg, "LLSCRIPT the script file is %d bytes, the most is %d\n", size, LL_SCRIPT_BYTES);
        Log(msg);
        return;
    }

    // the disc reads whole 32-byte blocks into a 32-byte aligned buffer
    int read = (size + 31) & ~31;
    char *buf = calloc(read + 64);
    script_text = (char *)(((u32)buf + 31) & ~31);
    DCFlushRange(script_text, read);
    File_ReadSync(entry, 0, script_text, read, 0x21, 1);
    DCInvalidateRange(script_text, read);
    script_text[size] = 0;

    // room for every line that isn't blank or a comment, and every script
    int lines = 0, names = 0;
    for (char *t = script_text; *t;)
    {
        t = Script_Skip(t);
        if (*t && *t != '\n' && *t != '#')
        {
            lines++;
            names += t[0] == 's' && t[1] == 'c' && t[2] == 'r' && t[3] == 'i' && t[4] == 'p' && t[5] == 't' &&
                     (t[6] == ' ' || t[6] == '\t' || t[6] == '\r' || t[6] == '\n' || t[6] == 0);
        }
        while (*t && *t != '\n')
            t++;
        if (*t)
            t++;
    }
    script_ops = calloc(sizeof(ScriptOp) * (lines + 1));
    scripts = calloc(sizeof(Script) * (names + 1));
    Script_Parse();

    // the menu lists the first ones, and All plays every one
    script_shown = script_num < LL_SCRIPT_MAX ? script_num : LL_SCRIPT_MAX;
    for (int i = 0; i < script_shown; i++)
        script_names[i + 1] = scripts[i].name;
    if (script_num > 1)
        script_names[script_shown + 1] = "All";
    Options_Main[OPT_SCRIPT].value_num = script_shown + 1 + (script_num > 1);
    if (script_autorun && script_num > 0)
    {
        Options_Main[OPT_SCRIPT].val = script_num > 1 ? script_shown + 1 : 1;
        script_pending = 1;
    }

    sprintf(msg, "LLSCRIPT loaded %d scripts, %d steps, %d bytes\n", script_num, script_op_num, size);
    Log(msg);
}

// The same as a recorded input playing back in the lab (Record_SetInputs):
// the engine pad the fighter reads this frame, and the raw pad UCF reads.
static void Script_Apply(FighterData *fp, ScriptInput *in)
{
    HSD_Pad *pad = PadGetEngine(fp->pad_index);
    u8 left = in->buttons & HSD_TRIGGER_L ? 140 : in->trigger;
    u8 right = in->buttons & HSD_TRIGGER_R ? 140 : 0;

    pad->held = in->buttons;
    pad->stickX = in->lx;
    pad->stickY = in->ly;
    pad->substickX = in->cx;
    pad->substickY = in->cy;
    pad->fstickX = pad->stickX / 80.f;
    pad->fstickY = pad->stickY / 80.f;
    pad->fsubstickX = pad->substickX / 80.f;
    pad->fsubstickY = pad->substickY / 80.f;
    pad->triggerLeft = left;
    pad->triggerRight = right;
    pad->ftriggerLeft = left / 140.f;
    pad->ftriggerRight = right / 140.f;

    PADStatus *raw = PadGetRaw(fp->pad_index);
    raw->button = pad->held;
    raw->stickX = pad->stickX;
    raw->stickY = pad->stickY;
    raw->substickX = pad->substickX;
    raw->substickY = pad->substickY;
    raw->triggerLeft = left;
    raw->triggerRight = right;
    raw->analogA = 0;
    raw->analogB = 0;
}

static void Script_UpdatePosition(GOBJ *fighter)
{
    FighterData *data = fighter->userdata;

    Vec3 pos = data->phys.pos;
    data->coll_data.topN_Curr = pos;
    data->coll_data.topN_CurrCorrect = pos;
    data->coll_data.topN_Prev = pos;
    data->coll_data.topN_Proj = pos;
    data->coll_data.coll_test = *stc_colltest;

    JOBJ *jobj = fighter->hsd_object;
    jobj->trans = data->phys.pos;
    JOBJ_SetMtxDirtySub(jobj);
    Fighter_SetPosition(data->ply, data->flags.ms, &data->phys.pos);
}

// After Falcon is moved, the event starts over as if he'd just appeared:
// no landing for where he was, no windows or ghost left from it.
static void Script_ResetTracking(void)
{
    prev_state_id = -1;
    prev_ts = -1;
    prev_tracked_air = 0;
    seg_valid = 0;
    live_visible = 0;
    ghost_visible = 0;
    ring_frames = 0;
    slide_frames = 0;
    beep_target = -100;
    Window_Forget();
}

// Input timers as if nothing had been touched for a while.
static void Script_ResetTimers(FighterData *fp)
{
    char keep[3] = {fp->input.x679, fp->input.x67A, fp->input.x67B};
    for (char *t = &fp->input.timer_lstick_tilt_x; t <= &fp->input.timer_specialn; t++)
        *t = LL_TIMER_MAX;
    fp->input.x679 = keep[0];
    fp->input.x67A = keep[1];
    fp->input.x67B = keep[2];
    fp->input.timer_jump_lockout = 0;
    fp->input.timer_specialhi_lockout = 0;
}

static void Script_FixCamera(GOBJ *ft)
{
    FighterData *fp = ft->userdata;
    Fighter_UpdateCameraBox(ft);
    CmSubject *subject = fp->camera_subject;
    subject->boundleft_curr = subject->boundleft_proj;
    subject->boundright_curr = subject->boundright_proj;
    Match_CorrectCamera();
}

// Stand Falcon on the floor under (x, y), as techchase.c resets a player.
static int Script_Place(GOBJ *ft, float x, float y, float facing)
{
    FighterData *fp = ft->userdata;
    Vec3 ground, unk;
    int line, kind;

    if (!GrColl_RaycastGround(&ground, &line, &kind, &unk, -1, -1, -1, 0, x, y + 8.f, x, y - 8.f, 0))
        return 0;

    fp->phys.pos = ground;
    fp->facing_direction = facing;
    Fighter_KillAllVelocity(ft);
    Script_UpdatePosition(ft);
    fp->coll_data.ground_index = line;
    EnvironmentCollision_WaitLanding(ft);
    Fighter_SetGrounded(fp);
    Fighter_EnterWait(ft);
    fp->flags.is_fastfall = 0;
    fp->jump.jumps_used = 0;
    Script_ResetTimers(fp);
    Script_FixCamera(ft);
    Script_ResetTracking();
    return 1;
}

// Put Falcon in the air at (x, y), falling with the given speed, as
// ledgedash.c drops a player above the ledge.
static void Script_PlaceAir(GOBJ *ft, ScriptOp *op)
{
    FighterData *fp = ft->userdata;

    fp->phys.pos.X = op->x;
    fp->phys.pos.Y = op->y;
    fp->phys.pos.Z = 0;
    fp->facing_direction = op->facing;
    Fighter_KillAllVelocity(ft);
    Script_UpdatePosition(ft);
    Fighter_SetAirborne(fp);
    fp->jump.jumps_used = op->count;
    if (op->count >= fp->attr.max_jumps)
        Fighter_EnterFallAerial(ft);
    else
        Fighter_EnterFall(ft);
    fp->phys.self_vel.X = op->vx;
    fp->phys.self_vel.Y = op->vy;
    fp->flags.is_fastfall = 0;
    // Leaving the ground locks the ECB bottom, and only the game's own
    // countdown lets go of it (setting 0 here would keep it at the feet for
    // good). It counts down before this frame's collision, so one extra.
    fp->coll_data.u.ecb_bot_lock_frames = op->lock + 1;
    Script_ResetTimers(fp);
    Script_FixCamera(ft);
    Script_ResetTracking();
}

static void Script_Next(void);

static void Script_Begin(int index)
{
    char buf[120];
    script_cur = index;
    script_pc = -1;
    Script_Next();
    memset(&script_last, 0, sizeof(script_last));
    Learned_Clear();
    Learned_Bake();
    sprintf(text_last, "-");
    sprintf(buf, "LLRUN %d %s at %d\n", index + 1, scripts[index].name, event_vars->game_timer);
    Log(buf);
}

static void Script_End(void)
{
    char buf[120];
    Script *sc = &scripts[script_cur];
    sprintf(buf, "LLSCRIPT end %s at %d, last landing: %s\n", sc->name, event_vars->game_timer, text_last);
    Log(buf);

    // All plays every script in turn
    int all = script_num > 1 && Options_Main[OPT_SCRIPT].val == script_shown + 1;
    if (all && script_cur + 1 < script_num)
    {
        Script_Begin(script_cur + 1);
        return;
    }
    if (all)
    {
        sprintf(buf, "LLDONE played all %d scripts at %d\n", script_num, event_vars->game_timer);
        Log(buf);
    }
    script_cur = -1;
}

static ScriptOp *Script_Op(void)
{
    Script *sc = &scripts[script_cur];
    return script_pc < sc->num ? &script_ops[sc->first + script_pc] : 0;
}

static void Script_Next(void)
{
    script_pc++;
    script_wait = 0;
    script_window = -1;
    script_air = 0;
    ScriptOp *op = Script_Op();
    script_left = op ? op->count : 0;
}

// Steps that take no frame. Before a frame (pre) a start or air places
// Falcon; after one, a shot or mark goes right after the inputs before it,
// and a start or air waits for the next frame. Returns 0 once the script is over.
static int Script_Instant(GOBJ *ft, int pre)
{
    char buf[160];
    for (;;)
    {
        ScriptOp *op = Script_Op();
        if (!op)
        {
            Script_End();
            return 0;
        }
        if (op->kind == SOP_START)
        {
            if (!pre)
                return 1;
            if (!Script_Place(ft, op->x, op->y, op->facing))
            {
                sprintf(buf, "LLSCRIPT no floor at %.2f %.2f, skipping %s\n", op->x, op->y, scripts[script_cur].name);
                Log(buf);
                Script_End();
                return 0;
            }
        }
        else if (op->kind == SOP_AIR)
        {
            if (!pre)
                return 1;
            Script_PlaceAir(ft, op);
        }
        else if (op->kind == SOP_SHOT)
        {
            Options_Main[OPT_FRAME_ADV].val = 1;
            sprintf(buf, "LLSHOT %s %s at %d\n", scripts[script_cur].name, op->label, event_vars->game_timer);
            Log(buf);
        }
        else if (op->kind == SOP_MARK)
        {
            sprintf(buf, "LLMARK %s %s at %d\n", scripts[script_cur].name, op->label, event_vars->game_timer);
            Log(buf);
        }
        else
            return 1;
        Script_Next();
    }
}

// The aerial an input starts when pressed with nothing held before
// (Aerial_Pressed), or -1.
static int Script_Aerial(ScriptInput *in, float facing)
{
    float cx = in->cx / 80.f, cy = in->cy / 80.f;
    int cstick = (fabs(script_last.cx / 80.f) < common_aerial_stick_x && fabs(cx) >= common_aerial_stick_x) ||
                 (fabs(script_last.cy / 80.f) < common_aerial_stick_y && fabs(cy) >= common_aerial_stick_y);
    if (!cstick && !(in->buttons & HSD_BUTTON_A))
        return -1;

    float x = cstick ? cx : in->lx / 80.f;
    float y = cstick ? cy : in->ly / 80.f;
    if (fabs(x) < common_aerial_stick_x && fabs(y) < common_aerial_stick_y)
        return TS_AIRN;
    float angle = atan2(y, fabs(x));
    if (angle > common_aerial_angle)
        return TS_AIRHI;
    if (angle < -common_aerial_angle)
        return TS_AIRLW;
    return x * facing >= 0 ? TS_AIRF : TS_AIRB;
}

// The frame the next window for a wl/ai step's own airdodge or aerial opens
// on, by the prediction made last frame (its step k is frame live_timer +
// k), or -1. Every window counts, whatever the AI Filter shows. Its length
// by that prediction goes in width.
static int Script_WindowStart(ScriptOp *op, FighterData *fp, int *width)
{
    Prediction *p = pred_live;
    if (!live_visible || live_timer != event_vars->game_timer - 1)
        return -1;

    int wl = op->kind == SOP_WL;
    u8 *mask = wl ? p->wl_mask : p->ai_mask;
    int last = p->land_frame ? p->land_frame - !wl : p->num;
    int bit;
    if (wl)
        bit = Dodge_Dir(op->in.lx / 80.f, op->in.ly / 80.f);
    else
    {
        int aerial = Script_Aerial(&op->in, fp->facing_direction);
        bit = aerial >= 0 ? AERIAL_BIT(aerial) : 0;
    }
    if (!bit)
        bit = 0xFF;

    for (int k = 1; k <= last && k < p->uncertain_from; k++)
    {
        if (mask[k] & bit)
        {
            *width = 0;
            while (k + *width <= last && k + *width < p->uncertain_from && (mask[k + *width] & bit))
                (*width)++;
            return live_timer + k;
        }
    }
    return -1;
}

// Runs before the fighters read their controllers (the lab's Record_Think
// slot), so the inputs count on this very frame.
static void Script_Think(GOBJ *gobj)
{
    char buf[160];

    // (autorun gives the match a moment to settle first)
    if (script_pending && event_vars->game_timer >= 30)
    {
        script_pending = 0;
        int sel = Options_Main[OPT_SCRIPT].val;
        if (sel > 0 && script_num > 0)
            Script_Begin(sel <= script_shown ? sel - 1 : 0);
    }
    if (script_cur < 0)
        return;

    GOBJ *ft = Fighter_GetGObj(0);
    FighterData *fp = ft->userdata;
    ScriptInput in;
    int done = 0;
    ScriptOp *op;

    for (;;)
    {
        if (!Script_Instant(ft, 1))
            return;
        op = Script_Op();
        // land: over once Falcon is back on the ground, and this frame is
        // the next step's
        if (op->kind == SOP_LAND && fp->phys.air_state == 1)
            script_air = 1;
        if (op->kind == SOP_LAND && script_air && fp->phys.air_state == 0)
        {
            Script_Next();
            continue;
        }
        break;
    }

    switch (op->kind)
    {
    case SOP_INPUT:
        in = op->in;
        done = --script_left <= 0;
        break;
    case SOP_LAND:
        in = op->in;
        done = ++script_wait >= (op->count > 0 ? op->count : LL_SCRIPT_WAIT);
        break;
    default: // SOP_WL, SOP_AI
    {
        // once the window has opened, its start is kept: the predictions
        // after it only see what's left of it
        int width = script_width;
        int start = script_window >= 0 ? script_window : Script_WindowStart(op, fp, &width);
        if (start >= 0 && start <= event_vars->game_timer && script_window < 0)
        {
            script_window = start;
            script_width = width;
        }
        int target = start >= 0 ? start + op->count : -1;
        if (target >= 0 && event_vars->game_timer >= target)
        {
            in = op->in;
            done = 1;
            sprintf(buf, "LLSCRIPT %s window opens at %d, %d frames predicted, pressed at %d (offset %d)\n",
                    op->kind == SOP_WL ? "wl" : "ai", start, width, event_vars->game_timer,
                    event_vars->game_timer - start);
            Log(buf);
        }
        else
        {
            // keep the stick, let go of the rest
            memset(&in, 0, sizeof(in));
            in.lx = script_last.lx;
            in.ly = script_last.ly;
            if (++script_wait >= LL_SCRIPT_WAIT)
            {
                sprintf(buf, "LLSCRIPT no %s window came, going on\n", op->kind == SOP_WL ? "wl" : "ai");
                Log(buf);
                done = 1;
            }
        }
        break;
    }
    }

    Script_Apply(fp, &in);
    script_last = in;
    sprintf(buf, "LLIN %d s %d %d c %d %d lt %d btn %x\n", event_vars->game_timer,
            in.lx, in.ly, in.cx, in.cy, in.trigger, in.buttons);
    Log(buf);

    if (done)
    {
        Script_Next();
        Script_Instant(ft, 0);
    }
}

void Event_ChangeScript(GOBJ *menu, int value)
{
    script_cur = -1;
    script_pending = value != 0;
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
    int resting = sid == ASID_WAIT || sid == ASID_LANDING || sid == ASID_LANDINGFALLSPECIAL ||
                  (sid >= ASID_LANDINGAIRN && sid <= ASID_LANDINGAIRLW);
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

    // where Falcon is on screen: the camera turns as well as moves, so
    // comparing with its eye position isn't enough
    COBJ *cobj = *stc_matchcam_cobj;
    Vec3 pos = {fp->phys.pos.X, fp->phys.pos.Y + body_offset, 0};
    Vec3 screen;
    HSD_GXProject(cobj, &pos, &screen, 1);
    float dx = screen.X - (cobj->viewport_left + cobj->viewport_right) / 2;
    if (dx > 24.f) // a little slack so it doesn't flicker near the middle
        panel_left = 1;
    else if (dx < -24.f)
        panel_left = 0;
}

// The ring counts down to the next AI window, the slide-in arrows to the
// next perfect waveland window, and the beeps to whichever comes first.
static void Timing_Update(FighterData *fp, Prediction *p)
{
    static const int beats[3] = {21, 11, 1};

    int ai = Cues_Ai() && p->ai_first < p->uncertain_from ? p->ai_first : 0;
    int wl = Cues_Waveland() && p->wl_first < p->uncertain_from ? p->wl_first : 0;

    // a window's width is fixed before it opens; once open, what's left of
    // it shrinks, so keep the look it had
    if (ai && ai <= LL_RING_FRAMES)
    {
        if (ai > 1 || ring_frames == 0)
            ring_wide = p->ai_width > 1;
        ring_frames = ai;
        ring_center.X = fp->phys.pos.X;
        ring_center.Y = fp->phys.pos.Y + body_offset;
    }
    else
        ring_frames = 0;
    if (wl && wl <= LL_RING_FRAMES)
    {
        if (wl > 1 || slide_frames == 0)
            slide_wide = p->wl_width > 1;
        slide_frames = wl;
        slide_dirs = p->wl_dirs;
        slide_center.X = fp->phys.pos.X;
        slide_center.Y = fp->phys.pos.Y + fp->coll_data.ecbCurrCorrect_bot.Y;
    }
    else
        slide_frames = 0;

    int next = ai && (!wl || ai < wl) ? ai : wl;
    if (!next || !Options_Main[OPT_BEEPS].val)
        return;

    // the same window can shift by a frame as the prediction updates
    int target = event_vars->game_timer + next;
    if (target - beep_target > 2 || beep_target - target > 2)
    {
        beep_target = target;
        beep_done = 0;
    }
    for (int i = 0; i < 3; i++)
    {
        int after = i < 2 ? beats[i + 1] : 0;
        if (next <= beats[i] && next > after && !(beep_done & (1 << i)))
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
    common_dodge_deadzone.X = Common_Float(COMMON_DODGE_DEADZONE);
    common_dodge_deadzone.Y = Common_Float(COMMON_DODGE_DEADZONE + 4);
    common_dodge_force = Common_Float(COMMON_DODGE_FORCE);
    common_dodge_decay = Common_Float(COMMON_DODGE_DECAY);
    common_waveland_lag = Common_Float(COMMON_WAVELAND_LAG);
    common_fall_lean_deadzone = Common_Float(COMMON_FALL_LEAN_DEADZONE);
    common_fall_lean_rate = Common_Float(COMMON_FALL_LEAN_RATE);

    for (int i = 0; i <= LL_CIRCLE_SEGS; i++)
    {
        float t = i * (6.2831853f / LL_CIRCLE_SEGS);
        circle[i].X = cos(t);
        circle[i].Y = sin(t);
    }

    ecb_table = calloc(sizeof(EcbSample) * TS_COUNT * LL_STATE_FRAMES);
    Learned_Clear();
    Learned_Bake();
    Lean_Index();
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

    // frame advance, and scripts fed in before the fighters read their
    // controllers (like the lab's recordings)
    HSD_Update *update = stc_hsd_update;
    update->checkPause = Advance_CheckPause;
    update->checkAdvance = Advance_CheckStep;
    Script_Load();
    GOBJ *script_gobj = GObj_Create(0, 7, 0);
    GObj_AddProc(script_gobj, Script_Think, 3);
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

    ai_show_all = Options_Main[OPT_AI_FILTER].val == 1;
    int logging = Options_Main[OPT_LOG].val || script_cur >= 0;

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
    if (!tracked_air)
    {
        ring_frames = 0;
        slide_frames = 0;
    }
    preview_fh = 0;
    preview_sh = 0;

    if (tracked_air)
    {
        if (Lean_IsPlain(fp, ts, prev_vel.X))
            Ecb_Record(fp, ts, frame_in_state);
        if (logging)
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
        live_timer = event_vars->game_timer;
        Text_Prediction(pred_live);
        Text_Windows(pred_live);
        Timing_Update(fp, pred_live);

        if (prev_tracked_air)
            Window_Feedback(fp, ts);
        Window_Remember(pred_live);
        if (prev_tracked_air && ts != prev_ts)
            Press_CheckMissed(fp, ts);
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
        // in the air but in a state we don't predict (up-B, hitstun, ...)
        live_visible = 0;
        seg_valid = 0;
        sprintf(text_ai, "-");
        if (logging && !disturbed && sid >= ASID_FALLSPECIAL && sid <= ASID_FALLSPECIALB)
            Log_FallSpecial(fp, sid, frame_in_state);
    }
    else if (Ground_CanJump(sid) && !disturbed && !ghost_visible)
        Ground_Preview(fp);
    if (!tracked_air)
        Window_Forget();

    // the jumpsquat and takeoff, to check the ground previews against
    if (sid == ASID_KNEEBEND && logging)
        Log_Squat(fp);

    prev_state_id = sid;
    prev_ts = ts;
    prev_tracked_air = tracked_air;
    prev_tilt_timer = (u8)fp->input.timer_lstick_tilt_y;
    prev_lock = fp->coll_data.u.ecb_bot_lock_frames;
    prev_vel = (Vec2){fp->phys.self_vel.X, fp->phys.self_vel.Y};
}

void Event_Update(void)
{
    if (Pause_CheckStatus(1) != 2)
        HSD_SetSpeedEasy(speed_values[Options_Main[OPT_SPEED].val]);
    else
        HSD_SetSpeedEasy(1.0);

    // runs every frame, frozen or not: D-pad down toggles frame advance,
    // D-pad left plays the chosen script again
    if (Pause_CheckStatus(1) == 2)
        return;
    int down = PadGetMaster(Advance_Port())->down;
    if (down & HSD_BUTTON_DPAD_DOWN)
        Options_Main[OPT_FRAME_ADV].val ^= 1;
    if ((down & HSD_BUTTON_DPAD_LEFT) && Options_Main[OPT_SCRIPT].val)
    {
        script_cur = -1;
        script_pending = 1;
    }
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
