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
#define COMMON_AIR_FRICTION_OOB 0x1FC   // float: air friction while faster than the drift max
#define COMMON_UPB_DRIFT_STICK 0x258    // float: |stick x| below this stops a Falcon Dive's drift
#define COMMON_DROP_STICK 0x464         // float: stick y <= -this drops through a platform out of shield
#define COMMON_DROP_WINDOW 0x468        // frames the flick may take to get there (the decomp types it float)
#define COMMON_SPOT_STICK 0x314         // float: stick y <= this is a spotdodge
#define COMMON_SPOT_WINDOW 0x318        // int: frames the flick may take for that

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
    TS_FALLSPECIAL, // helpless fall: after a Falcon Dive, or an airdodge that ran out
    TS_UPB,         // Falcon Dive started on the ground, once he's airborne
    TS_UPBA,        // Falcon Dive started in the air

    TS_COUNT
};

#define ASID_CAPTAIN_SPECIALHI 353    // Falcon's own states start at 341
#define ASID_CAPTAIN_SPECIALAIRHI 354

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
    ASID_FALLSPECIAL,
    ASID_CAPTAIN_SPECIALHI,
    ASID_CAPTAIN_SPECIALAIRHI,
};

static const char *tracked_state_names[TS_COUNT] = {
    "JumpF", "JumpB", "DJumpF", "DJumpB", "Fall", "FallAerial",
    "Nair", "Fair", "Bair", "Uair", "Dair", "Airdodge",
    "FallSpecial", "UpB", "UpBAir",
};

static int Tracked_Index(int state_id)
{
    if (state_id == ASID_FALLSPECIALF || state_id == ASID_FALLSPECIALB)
        return TS_FALLSPECIAL;
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

static int Tracked_IsUpB(int ts)
{
    return ts == TS_UPB || ts == TS_UPBA;
}

// Nothing can be pressed out of a Falcon Dive or the helpless fall after
// it: no aerial, airdodge or jump.
static int Tracked_IsHelpless(int ts)
{
    return ts >= TS_FALLSPECIAL;
}

// Jump, double jump, fall and helpless fall collision pass the drop-through
// callback (ftCo_80096CC8); aerials, the airdodge (ft_80081D0C) and Falcon
// Dive (ft_CheckGroundAndLedge) collide without it and always land on
// platforms.
static int Tracked_UsesPlatformDrop(int ts)
{
    return ts <= TS_FALLAERIAL || ts == TS_FALLSPECIAL;
}

// The state the game enters when this one's animation runs out, or -1 if it
// loops or isn't tracked. Jump and aerials go to Fall (ftCo_Fall_Enter),
// double jump goes to FallAerial (ftCo_FallAerial_Enter), the airdodge and
// Falcon Dive go to FallSpecial (ftCo_80096900). Once a state has been seen
// ending, state_next (below) holds what really followed.
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
    case TS_ESCAPEAIR:
    case TS_UPB:
    case TS_UPBA:
        return TS_FALLSPECIAL;
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
// green for NIL, pink for aerial interrupts, cyan for perfect wavelands and
// wavedashes. They also differ in shape: a NIL colors the whole path, an AI
// is a pink bar on it, a perfect waveland a wider cyan bar around it.
static const GXColor land_kind_colors[LAND_KIND_COUNT] = {
    {120, 160, 255, 255}, // none: blue
    {64, 240, 120, 255},  // NIL: green
    {255, 64, 170, 255},  // aerial interrupt: pink
    {255, 230, 0, 255},   // normal landing / auto-cancel: yellow
    {255, 140, 0, 255},   // L-cancel: orange
    {255, 48, 48, 255},   // full aerial lag: red
    {200, 200, 200, 255}, // other
    {40, 220, 255, 255},  // perfect waveland: cyan
    {200, 200, 200, 255}, // other waveland
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
// jump, double jump, an aerial, the airdodge or Falcon Dive into a fall
// state.
static int Tracked_EndedInto(int prev, int ts)
{
    return Tracked_NaturalNext(prev) >= 0 && (ts == TS_FALL || ts == TS_FALLAERIAL || ts == TS_FALLSPECIAL);
}

static int Tracked_Next(int ts)
{
    return state_next[ts] >= 0 ? state_next[ts] : Tracked_NaturalNext(ts);
}

static void Upb_Clear(void);
static void Upb_Bake(void);

static void Learned_Clear(void)
{
    Upb_Clear();
    memset(ecb_table, 0, sizeof(EcbSample) * TS_COUNT * LL_STATE_FRAMES);
    memset(state_len, 0, sizeof(state_len));
    memset(state_next, -1, sizeof(state_next));
}

// Where a frame is learned.
static EcbSample *Ecb_Slot(int ts, int frame)
{
    if (ts == TS_FALL || ts == TS_FALLAERIAL || ts == TS_FALLSPECIAL)
        frame %= LL_FALL_LOOP; // the fall animations loop
    if (frame >= LL_STATE_FRAMES)
        frame = LL_STATE_FRAMES - 1;
    return &ecb_table[ts * LL_STATE_FRAMES + frame];
}

// What the simulation uses for a frame: the helpless fall's pose, until
// it's been seen, is the plain fall's. It only decides the frame of a
// landing with lag.
static EcbSample *Ecb_Get(int ts, int frame)
{
    EcbSample *e = Ecb_Slot(ts, frame);
    if (ts == TS_FALLSPECIAL && !e->seen)
        e = Ecb_Slot(TS_FALL, frame);
    return e;
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
    // Falcon Dive in the air (from the ground from frame 13, when he lifts
    // off) and the helpless fall's plain 8-frame loop, from a scripted
    // capture (2026-10-05). Frames 13-16 keep the bottom from before.
    {TS_UPB, 13, BAKED_SHAPE, 0.0000f, 8.1604f, 4.0802f, 4.4296f, -4.4296f},
    {TS_UPB, 14, BAKED_SHAPE, 0.0000f, 10.6500f, 5.3250f, 3.3929f, -3.3929f},
    {TS_UPB, 15, BAKED_SHAPE, 0.0000f, 19.2369f, 10.5359f, 3.1052f, -3.1052f},
    {TS_UPB, 16, BAKED_SHAPE, 0.0000f, 19.5940f, 10.8937f, 3.1246f, -3.1246f},
    {TS_UPB, 17, BAKED_SHAPE|BAKED_BOTTOM, 2.3288f, 19.7247f, 11.0268f, 3.1696f, -3.1696f},
    {TS_UPB, 18, BAKED_SHAPE|BAKED_BOTTOM, 2.3028f, 19.6934f, 10.9981f, 3.2295f, -3.2295f},
    {TS_UPB, 19, BAKED_SHAPE|BAKED_BOTTOM, 2.1788f, 19.5644f, 10.8716f, 3.3094f, -3.3094f},
    {TS_UPB, 20, BAKED_SHAPE|BAKED_BOTTOM, 2.0204f, 19.4023f, 10.7113f, 3.3977f, -3.3977f},
    {TS_UPB, 21, BAKED_SHAPE|BAKED_BOTTOM, 1.8911f, 19.2714f, 10.5813f, 3.4839f, -3.4839f},
    {TS_UPB, 22, BAKED_SHAPE|BAKED_BOTTOM, 1.8542f, 19.2363f, 10.5452f, 3.5755f, -3.5755f},
    {TS_UPB, 23, BAKED_SHAPE|BAKED_BOTTOM, 2.0144f, 19.2362f, 10.6253f, 3.8499f, -3.8499f},
    {TS_UPB, 24, BAKED_SHAPE|BAKED_BOTTOM, 2.3423f, 19.1915f, 10.7669f, 4.1488f, -4.1488f},
    {TS_UPB, 25, BAKED_SHAPE|BAKED_BOTTOM, 2.7811f, 19.1071f, 10.9441f, 4.3859f, -4.3859f},
    {TS_UPB, 26, BAKED_SHAPE|BAKED_BOTTOM, 3.2598f, 18.6334f, 10.9466f, 4.6656f, -4.6656f},
    {TS_UPB, 27, BAKED_SHAPE|BAKED_BOTTOM, 3.7039f, 17.6724f, 10.6882f, 4.6162f, -4.6162f},
    {TS_UPB, 28, BAKED_SHAPE|BAKED_BOTTOM, 4.0941f, 19.2829f, 11.6885f, 4.5386f, -4.5386f},
    {TS_UPB, 29, BAKED_SHAPE|BAKED_BOTTOM, 4.4665f, 20.3604f, 12.4134f, 4.7037f, -4.7037f},
    {TS_UPB, 30, BAKED_SHAPE|BAKED_BOTTOM, 4.8335f, 20.8578f, 12.8457f, 4.5893f, -4.5893f},
    {TS_UPB, 31, BAKED_SHAPE|BAKED_BOTTOM, 4.9177f, 20.7514f, 12.8345f, 3.7503f, -3.7503f},
    {TS_UPB, 32, BAKED_SHAPE|BAKED_BOTTOM, 5.2077f, 20.0487f, 12.6282f, 3.1405f, -3.1405f},
    {TS_UPB, 33, BAKED_SHAPE|BAKED_BOTTOM, 5.9174f, 18.6492f, 12.2833f, 3.9376f, -3.9376f},
    {TS_UPB, 34, BAKED_SHAPE|BAKED_BOTTOM, 6.6842f, 16.8173f, 11.7508f, 4.5602f, -4.5602f},
    {TS_UPB, 35, BAKED_SHAPE|BAKED_BOTTOM, 7.1691f, 14.7178f, 10.9435f, 10.8109f, -2.0000f},
    {TS_UPB, 36, BAKED_SHAPE|BAKED_BOTTOM, 7.2426f, 12.8845f, 10.0635f, 10.4583f, -2.0000f},
    {TS_UPB, 37, BAKED_SHAPE|BAKED_BOTTOM, 7.6481f, 13.4430f, 10.5456f, 4.9802f, -4.9802f},
    {TS_UPB, 38, BAKED_SHAPE|BAKED_BOTTOM, 8.4507f, 14.7134f, 11.5821f, 3.9806f, -3.9806f},
    {TS_UPB, 39, BAKED_SHAPE|BAKED_BOTTOM, 8.6402f, 15.5272f, 12.0837f, 3.3716f, -3.3716f},
    {TS_UPB, 40, BAKED_SHAPE|BAKED_BOTTOM, 9.2960f, 15.5592f, 12.4276f, 2.9335f, -2.9335f},
    {TS_UPB, 41, BAKED_SHAPE|BAKED_BOTTOM, 10.9848f, 17.9472f, 14.4660f, 3.6769f, -3.6769f},
    {TS_UPB, 42, BAKED_SHAPE|BAKED_BOTTOM, 13.0311f, 21.2544f, 17.1427f, 3.1017f, -3.1017f},
    {TS_UPB, 43, BAKED_SHAPE|BAKED_BOTTOM, 12.4750f, 22.0293f, 17.2522f, 2.9029f, -2.9029f},
    {TS_UPB, 44, BAKED_SHAPE|BAKED_BOTTOM, 12.4047f, 21.7174f, 17.0610f, 4.0376f, -4.0376f},
    {TS_UPB, 45, BAKED_SHAPE|BAKED_BOTTOM, 12.8465f, 22.0650f, 17.4557f, 4.5948f, -4.5948f},
    {TS_UPB, 46, BAKED_SHAPE|BAKED_BOTTOM, 13.5443f, 21.4167f, 17.4805f, 5.9173f, -5.0136f},
    {TS_UPB, 47, BAKED_SHAPE|BAKED_BOTTOM, 14.3430f, 20.0716f, 17.2073f, 5.6589f, -5.9363f},
    {TS_UPB, 48, BAKED_SHAPE|BAKED_BOTTOM, 13.1542f, 19.3151f, 16.2346f, 5.5669f, -6.1039f},
    {TS_UPB, 49, BAKED_SHAPE|BAKED_BOTTOM, 11.5959f, 18.3337f, 14.9648f, 6.4005f, -5.3434f},
    {TS_UPB, 50, BAKED_SHAPE|BAKED_BOTTOM, 9.9269f, 15.6031f, 12.7650f, 3.9335f, -3.9335f},
    {TS_UPB, 51, BAKED_SHAPE|BAKED_BOTTOM, 7.7268f, 12.2857f, 10.0063f, 2.5684f, -2.5684f},
    {TS_UPB, 52, BAKED_SHAPE|BAKED_BOTTOM, 6.4143f, 11.8865f, 9.1504f, 2.0181f, -2.0181f},
    {TS_UPB, 53, BAKED_SHAPE|BAKED_BOTTOM, 6.5881f, 11.7692f, 9.1786f, 2.0000f, -2.0000f},
    {TS_UPB, 54, BAKED_SHAPE|BAKED_BOTTOM, 7.8066f, 12.3797f, 10.0932f, 2.3789f, -2.3789f},
    {TS_UPB, 55, BAKED_SHAPE|BAKED_BOTTOM, 9.4036f, 14.5532f, 11.9784f, 2.7759f, -2.7759f},
    {TS_UPB, 56, BAKED_SHAPE|BAKED_BOTTOM, 11.8997f, 16.9006f, 14.4001f, 2.3293f, -2.3293f},
    {TS_UPB, 57, BAKED_SHAPE|BAKED_BOTTOM, 11.6275f, 16.8228f, 14.2252f, 2.6130f, -2.6130f},
    {TS_UPB, 58, BAKED_SHAPE|BAKED_BOTTOM, 10.7898f, 16.2599f, 13.5248f, 2.5098f, -2.5098f},
    {TS_UPB, 59, BAKED_SHAPE|BAKED_BOTTOM, 9.7823f, 14.4689f, 12.1256f, 2.1326f, -2.1326f},
    {TS_UPB, 60, BAKED_SHAPE|BAKED_BOTTOM, 8.7493f, 13.0281f, 10.8887f, 2.5772f, -2.5772f},
    {TS_UPB, 61, BAKED_SHAPE|BAKED_BOTTOM, 7.6332f, 11.2262f, 9.4297f, 2.8173f, -2.8173f},
    {TS_UPB, 62, BAKED_SHAPE|BAKED_BOTTOM, 4.9690f, 11.3863f, 8.1777f, 2.9519f, -2.9519f},
    {TS_UPB, 63, BAKED_SHAPE|BAKED_BOTTOM, 3.1936f, 13.5086f, 8.3511f, 3.6430f, -3.6430f},
    {TS_UPBA, 0, BAKED_SHAPE|BAKED_BOTTOM, 3.0555f, 11.0717f, 7.0636f, 4.6822f, -4.6822f},
    {TS_UPBA, 1, BAKED_SHAPE|BAKED_BOTTOM, 4.4012f, 11.4629f, 7.9321f, 3.2647f, -3.2647f},
    {TS_UPBA, 2, BAKED_SHAPE|BAKED_BOTTOM, 4.6451f, 11.5290f, 8.0871f, 3.2518f, -3.2518f},
    {TS_UPBA, 3, BAKED_SHAPE|BAKED_BOTTOM, 4.3990f, 11.2975f, 7.8483f, 8.7902f, -4.0142f},
    {TS_UPBA, 4, BAKED_SHAPE|BAKED_BOTTOM, 2.1716f, 10.5536f, 6.3626f, 9.2829f, -3.6621f},
    {TS_UPBA, 5, BAKED_SHAPE|BAKED_BOTTOM, 2.0420f, 10.3964f, 6.2192f, 9.3124f, -3.5309f},
    {TS_UPBA, 6, BAKED_SHAPE|BAKED_BOTTOM, 1.9571f, 10.2973f, 6.1272f, 9.3287f, -3.4281f},
    {TS_UPBA, 7, BAKED_SHAPE|BAKED_BOTTOM, 1.9131f, 10.2545f, 6.0838f, 9.3343f, -3.3639f},
    {TS_UPBA, 8, BAKED_SHAPE|BAKED_BOTTOM, 1.9061f, 10.2575f, 6.0818f, 9.3321f, -3.3420f},
    {TS_UPBA, 9, BAKED_SHAPE|BAKED_BOTTOM, 1.9323f, 10.2951f, 6.1137f, 9.3226f, -3.3649f},
    {TS_UPBA, 10, BAKED_SHAPE|BAKED_BOTTOM, 1.9878f, 10.3568f, 6.1723f, 9.3029f, -3.4308f},
    {TS_UPBA, 11, BAKED_SHAPE|BAKED_BOTTOM, 2.0689f, 10.4399f, 6.2544f, 9.2723f, -3.5406f},
    {TS_UPBA, 12, BAKED_SHAPE|BAKED_BOTTOM, 2.1716f, 10.5441f, 6.3578f, 9.2255f, -3.6938f},
    {TS_UPBA, 13, BAKED_SHAPE, 0.0000f, 12.1894f, 6.9468f, 4.4818f, -4.4818f},
    {TS_UPBA, 14, BAKED_SHAPE, 0.0000f, 13.1398f, 7.0260f, 2.7994f, -2.7994f},
    {TS_UPBA, 15, BAKED_SHAPE, 0.0000f, 19.2367f, 10.5356f, 3.1050f, -3.1050f},
    {TS_UPBA, 16, BAKED_SHAPE, 0.0000f, 19.4623f, 10.7529f, 3.1559f, -3.1559f},
    {TS_UPBA, 17, BAKED_SHAPE|BAKED_BOTTOM, 2.0919f, 19.5165f, 10.8042f, 3.2133f, -3.2133f},
    {TS_UPBA, 18, BAKED_SHAPE|BAKED_BOTTOM, 2.0345f, 19.4571f, 10.7458f, 3.2776f, -3.2776f},
    {TS_UPBA, 19, BAKED_SHAPE|BAKED_BOTTOM, 1.9265f, 19.3417f, 10.6341f, 3.3464f, -3.3464f},
    {TS_UPBA, 20, BAKED_SHAPE|BAKED_BOTTOM, 1.8233f, 19.2279f, 10.5256f, 3.4177f, -3.4177f},
    {TS_UPBA, 21, BAKED_SHAPE|BAKED_BOTTOM, 1.7804f, 19.1734f, 10.4769f, 3.4913f, -3.4913f},
    {TS_UPBA, 22, BAKED_SHAPE|BAKED_BOTTOM, 1.8533f, 19.2360f, 10.5447f, 3.5768f, -3.5768f},
    {TS_UPBA, 23, BAKED_SHAPE|BAKED_BOTTOM, 2.0941f, 19.3039f, 10.6990f, 3.8592f, -3.8592f},
    {TS_UPBA, 24, BAKED_SHAPE|BAKED_BOTTOM, 2.4376f, 19.2675f, 10.8525f, 4.1486f, -4.1486f},
    {TS_UPBA, 25, BAKED_SHAPE|BAKED_BOTTOM, 2.8468f, 19.1572f, 11.0020f, 4.3865f, -4.3865f},
    {TS_UPBA, 26, BAKED_SHAPE|BAKED_BOTTOM, 3.2853f, 18.6493f, 10.9673f, 4.6660f, -4.6660f},
    {TS_UPBA, 27, BAKED_SHAPE|BAKED_BOTTOM, 3.7127f, 17.6754f, 10.6940f, 4.6156f, -4.6156f},
    {TS_UPBA, 28, BAKED_SHAPE|BAKED_BOTTOM, 4.1113f, 19.2861f, 11.6987f, 4.5511f, -4.5511f},
    {TS_UPBA, 29, BAKED_SHAPE|BAKED_BOTTOM, 4.4680f, 20.3624f, 12.4152f, 4.6993f, -4.6993f},
    {TS_UPBA, 30, BAKED_SHAPE|BAKED_BOTTOM, 4.8230f, 20.8584f, 12.8407f, 4.5740f, -4.5740f},
    {TS_UPBA, 31, BAKED_SHAPE|BAKED_BOTTOM, 4.9176f, 20.7506f, 12.8341f, 3.7549f, -3.7549f},
    {TS_UPBA, 32, BAKED_SHAPE|BAKED_BOTTOM, 5.2076f, 20.0330f, 12.6203f, 3.1484f, -3.1484f},
    {TS_UPBA, 33, BAKED_SHAPE|BAKED_BOTTOM, 5.9174f, 18.6256f, 12.2715f, 3.9448f, -3.9448f},
    {TS_UPBA, 34, BAKED_SHAPE|BAKED_BOTTOM, 6.6841f, 16.7832f, 11.7337f, 4.5457f, -4.5457f},
    {TS_UPBA, 35, BAKED_SHAPE|BAKED_BOTTOM, 7.1689f, 14.6674f, 10.9182f, 11.0110f, -2.0000f},
    {TS_UPBA, 36, BAKED_SHAPE|BAKED_BOTTOM, 7.2424f, 12.8844f, 10.0634f, 10.6240f, -2.0000f},
    {TS_UPBA, 37, BAKED_SHAPE|BAKED_BOTTOM, 7.6480f, 13.4429f, 10.5454f, 4.9755f, -4.9755f},
    {TS_UPBA, 38, BAKED_SHAPE|BAKED_BOTTOM, 8.4476f, 14.7464f, 11.5970f, 3.9803f, -3.9803f},
    {TS_UPBA, 39, BAKED_SHAPE|BAKED_BOTTOM, 8.5446f, 15.5572f, 12.0509f, 3.3604f, -3.3604f},
    {TS_UPBA, 40, BAKED_SHAPE|BAKED_BOTTOM, 9.1296f, 15.5742f, 12.3519f, 2.9427f, -2.9427f},
    {TS_UPBA, 41, BAKED_SHAPE|BAKED_BOTTOM, 11.2095f, 17.9689f, 14.5892f, 3.6853f, -3.6853f},
    {TS_UPBA, 42, BAKED_SHAPE|BAKED_BOTTOM, 13.1787f, 21.2524f, 17.2156f, 3.1006f, -3.1006f},
    {TS_UPBA, 43, BAKED_SHAPE|BAKED_BOTTOM, 12.4758f, 22.0323f, 17.2541f, 2.7566f, -2.7566f},
    {TS_UPBA, 44, BAKED_SHAPE|BAKED_BOTTOM, 12.4295f, 21.8440f, 17.1367f, 3.8939f, -3.8939f},
    {TS_UPBA, 45, BAKED_SHAPE|BAKED_BOTTOM, 12.8642f, 22.2277f, 17.5459f, 4.6004f, -4.6004f},
    {TS_UPBA, 46, BAKED_SHAPE|BAKED_BOTTOM, 13.5591f, 21.6127f, 17.5859f, 6.0037f, -4.9484f},
    {TS_UPBA, 47, BAKED_SHAPE|BAKED_BOTTOM, 14.3572f, 20.1898f, 17.2735f, 5.7370f, -5.8363f},
    {TS_UPBA, 48, BAKED_SHAPE|BAKED_BOTTOM, 13.1611f, 18.9347f, 16.0479f, 5.6334f, -6.0442f},
    {TS_UPBA, 49, BAKED_SHAPE|BAKED_BOTTOM, 11.5961f, 17.9518f, 14.7739f, 6.4880f, -5.3040f},
    {TS_UPBA, 50, BAKED_SHAPE|BAKED_BOTTOM, 9.9264f, 15.5997f, 12.7631f, 3.9387f, -3.9387f},
    {TS_UPBA, 51, BAKED_SHAPE|BAKED_BOTTOM, 7.7271f, 12.2858f, 10.0064f, 2.5843f, -2.5843f},
    {TS_UPBA, 52, BAKED_SHAPE|BAKED_BOTTOM, 6.4143f, 11.8865f, 9.1504f, 2.0000f, -2.0000f},
    {TS_UPBA, 53, BAKED_SHAPE|BAKED_BOTTOM, 6.5879f, 11.7691f, 9.1785f, 2.0000f, -2.0000f},
    {TS_UPBA, 54, BAKED_SHAPE|BAKED_BOTTOM, 8.2609f, 12.4515f, 10.3562f, 2.3789f, -2.3789f},
    {TS_UPBA, 55, BAKED_SHAPE|BAKED_BOTTOM, 9.8475f, 14.5531f, 12.2003f, 2.7759f, -2.7759f},
    {TS_UPBA, 56, BAKED_SHAPE|BAKED_BOTTOM, 12.0901f, 16.9134f, 14.5018f, 2.3810f, -2.3810f},
    {TS_UPBA, 57, BAKED_SHAPE|BAKED_BOTTOM, 11.6258f, 16.8103f, 14.2181f, 2.3955f, -2.3955f},
    {TS_UPBA, 58, BAKED_SHAPE|BAKED_BOTTOM, 10.7667f, 16.2046f, 13.4856f, 2.3052f, -2.3052f},
    {TS_UPBA, 59, BAKED_SHAPE|BAKED_BOTTOM, 9.7304f, 13.9406f, 11.8355f, 2.1955f, -2.1955f},
    {TS_UPBA, 60, BAKED_SHAPE|BAKED_BOTTOM, 8.7297f, 12.3754f, 10.5526f, 2.6375f, -2.6375f},
    {TS_UPBA, 61, BAKED_SHAPE|BAKED_BOTTOM, 7.3496f, 10.6797f, 9.0147f, 2.8061f, -2.8061f},
    {TS_UPBA, 62, BAKED_SHAPE|BAKED_BOTTOM, 4.8378f, 11.4973f, 8.1675f, 2.8991f, -2.8991f},
    {TS_UPBA, 63, BAKED_SHAPE|BAKED_BOTTOM, 3.1949f, 13.3378f, 8.2664f, 3.6351f, -3.6351f},
    {TS_FALLSPECIAL, 0, BAKED_SHAPE|BAKED_BOTTOM, 2.3062f, 14.6213f, 8.4638f, 3.8894f, -3.8894f},
    {TS_FALLSPECIAL, 1, BAKED_SHAPE|BAKED_BOTTOM, 2.3489f, 14.4178f, 8.3834f, 4.0366f, -4.0366f},
    {TS_FALLSPECIAL, 2, BAKED_SHAPE|BAKED_BOTTOM, 2.5954f, 14.4364f, 8.5159f, 4.3550f, -4.3550f},
    {TS_FALLSPECIAL, 3, BAKED_SHAPE|BAKED_BOTTOM, 2.8486f, 14.4099f, 8.6293f, 4.6503f, -4.6503f},
    {TS_FALLSPECIAL, 4, BAKED_SHAPE|BAKED_BOTTOM, 2.9127f, 14.3312f, 8.6220f, 4.8356f, -4.8356f},
    {TS_FALLSPECIAL, 5, BAKED_SHAPE|BAKED_BOTTOM, 2.9072f, 14.4049f, 8.6561f, 4.5973f, -4.5973f},
    {TS_FALLSPECIAL, 6, BAKED_SHAPE|BAKED_BOTTOM, 2.6730f, 14.3948f, 8.5339f, 4.2984f, -4.2984f},
    {TS_FALLSPECIAL, 7, BAKED_SHAPE|BAKED_BOTTOM, 2.4065f, 14.4815f, 8.4440f, 4.0367f, -4.0367f},
};

static void Learned_Bake(void)
{
    for (int i = 0; i < (int)countof(baked_ecb); i++)
    {
        const BakedEcb *b = &baked_ecb[i];
        EcbSample *e = Ecb_Slot(b->ts, b->frame);
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
    // end (2026-10-03, 2026-10-05); aerials end in Fall, double jumps in
    // FallAerial, the airdodge and Falcon Dive in FallSpecial
    static const s8 baked_len[][2] = {
        {TS_ESCAPEAIR, 49},
        {TS_UPB, 64},
        {TS_UPBA, 64},
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
    Upb_Bake();
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

    EcbSample *s = Ecb_Slot(ts, frame);
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

// Falcon Dive moves by its animation (ft_80085134) plus a drift speed of its
// own, which the stick steers (ftCa_SpecialHi_Phys). The animation's part is
// learned from every frame seen: one table for the dive started on the
// ground, one for the air. Its state variables (mv.ca.specialhi): a short,
// a byte of flags, a byte, then the drift speed.
#define UPB_FLAGS 2          // byte holding x2_b1, set by its IASA: from then on
#define UPB_CAN_LAND 0x40    // touching the floor lands, and he may turn around
#define UPB_DRIFT 4          // Vec2
#define FALLSPECIAL_LAG 0x14 // mv.co.fallspecial.landing_lag

// Falcon's own attributes (ftCaptain_DatAttrs) for Falcon Dive
#define CA_UPB_DRIFT_ACCEL 0x40 // times the air drift stick multiplier
#define CA_UPB_DRIFT_MAX 0x44   // times the air drift max
#define CA_UPB_LANDING_LAG 0x4C
#define CA_UPB_TURN_STICK 0x58  // |stick x| above this at its IASA turns him that way

typedef struct UpbFrame
{
    float x, y;  // the animation's movement, as if facing right
    u8 seen;
    u8 can_land;
} UpbFrame;

static UpbFrame *upb_table; // [2][LL_STATE_FRAMES]
static s16 upb_turn[2];     // its first can_land frame, -1 = not seen

static float Captain_Attr(FighterData *fp, int offset)
{
    return *(float *)((u8 *)fp->special_attributes + offset);
}

static UpbFrame *Upb_Get(int ts, int frame)
{
    if (frame >= LL_STATE_FRAMES)
        frame = LL_STATE_FRAMES - 1;
    return &upb_table[(ts - TS_UPB) * LL_STATE_FRAMES + frame];
}

static Vec2 Upb_Drift(FighterData *fp)
{
    Vec2 v;
    memcpy(&v, (u8 *)&fp->state_var + UPB_DRIFT, sizeof(v));
    return v;
}

static void Upb_Clear(void)
{
    memset(upb_table, 0, sizeof(UpbFrame) * 2 * LL_STATE_FRAMES);
    upb_turn[0] = upb_turn[1] = -1;
}

static void Upb_Record(FighterData *fp, int ts, int frame)
{
    if (frame >= LL_STATE_FRAMES)
        return;
    UpbFrame *a = Upb_Get(ts, frame);
    Vec2 drift = Upb_Drift(fp);
    a->x = (fp->phys.self_vel.X - drift.X) * fp->facing_direction;
    a->y = fp->phys.self_vel.Y - drift.Y;
    a->can_land = (((u8 *)&fp->state_var)[UPB_FLAGS] & UPB_CAN_LAND) != 0;
    a->seen = 1;
    int k = ts - TS_UPB;
    if (a->can_land && (upb_turn[k] < 0 || frame < upb_turn[k]))
        upb_turn[k] = frame;
}

// Its animation's movement on every frame in the air, from the same
// capture as its ECB. The IASA comes on frame 12 for both, which for the
// one from the ground is still on the ground.
#define UPB_IASA_FRAME 12
#define UPB_LIFTOFF_FRAME 13 // ftCommon_8007D60C: in the air, out of jumps,
#define UPB_LIFTOFF_LOCK 5   // and the ECB bottom kept for 5 frames

typedef struct BakedUpb
{
    s8 ts;
    u8 frame;
    u8 can_land;
    float x, y;
} BakedUpb;

static const BakedUpb baked_upb[] = {
    {TS_UPB, 13, 1, 0.24060f, 5.08534f},
    {TS_UPB, 14, 1, 0.24060f, 4.69613f},
    {TS_UPB, 15, 1, 0.24060f, 4.32428f},
    {TS_UPB, 16, 1, 0.24060f, 3.96980f},
    {TS_UPB, 17, 1, 0.24060f, 3.63268f},
    {TS_UPB, 18, 1, 0.24060f, 3.31294f},
    {TS_UPB, 19, 1, 0.24060f, 3.01056f},
    {TS_UPB, 20, 1, 0.24060f, 2.72555f},
    {TS_UPB, 21, 1, 0.24060f, 2.45791f},
    {TS_UPB, 22, 1, 0.24060f, 2.20763f},
    {TS_UPB, 23, 1, 0.24060f, 1.97473f},
    {TS_UPB, 24, 1, 0.24060f, 1.75920f},
    {TS_UPB, 25, 1, 0.24060f, 1.56103f},
    {TS_UPB, 26, 1, 0.24060f, 1.38022f},
    {TS_UPB, 27, 1, 0.24060f, 1.21680f},
    {TS_UPB, 28, 1, 0.24060f, 1.07074f},
    {TS_UPB, 29, 1, 0.24060f, 0.94204f},
    {TS_UPB, 30, 1, 0.24060f, 0.83073f},
    {TS_UPB, 31, 1, 0.24060f, 0.73674f},
    {TS_UPB, 32, 1, 0.24060f, 0.66018f},
    {TS_UPB, 33, 1, 0.24060f, 0.60094f},
    {TS_UPB, 34, 1, 0.24060f, 0.46750f},
    {TS_UPB, 35, 1, 0.24060f, 0.25555f},
    {TS_UPB, 36, 1, 0.24060f, 0.05447f},
    {TS_UPB, 37, 1, 0.24060f, -0.13570f},
    {TS_UPB, 38, 1, 0.24060f, -0.31498f},
    {TS_UPB, 39, 1, 0.24060f, -0.48337f},
    {TS_UPB, 40, 1, 0.24060f, -0.64086f},
    {TS_UPB, 41, 1, 0.24060f, -0.78747f},
    {TS_UPB, 42, 1, 0.24060f, -0.92316f},
    {TS_UPB, 43, 1, 0.24060f, -1.04797f},
    {TS_UPB, 44, 1, 0.24060f, -1.16189f},
    {TS_UPB, 45, 1, 0.24060f, -1.26490f},
    {TS_UPB, 46, 1, 0.24060f, -1.35703f},
    {TS_UPB, 47, 1, 0.24060f, -1.43826f},
    {TS_UPB, 48, 1, 0.24060f, -1.50860f},
    {TS_UPB, 49, 1, 0.24060f, -1.56804f},
    {TS_UPB, 50, 1, 0.24060f, -1.61658f},
    {TS_UPB, 51, 1, 0.24060f, -1.65424f},
    {TS_UPB, 52, 1, 0.24060f, -1.68100f},
    {TS_UPB, 53, 1, 0.24060f, -1.69686f},
    {TS_UPB, 54, 1, 0.24060f, -1.70183f},
    {TS_UPB, 55, 1, 0.24060f, -1.69590f},
    {TS_UPB, 56, 1, 0.24060f, -1.67908f},
    {TS_UPB, 57, 1, 0.24060f, -1.65137f},
    {TS_UPB, 58, 1, 0.24060f, -1.61276f},
    {TS_UPB, 59, 1, 0.24060f, -1.56326f},
    {TS_UPB, 60, 1, 0.24060f, -1.50285f},
    {TS_UPB, 61, 1, 0.24060f, -1.43156f},
    {TS_UPB, 62, 1, 0.24060f, -1.34938f},
    {TS_UPB, 63, 1, 0.24060f, -1.25629f},
    {TS_UPBA, 0, 0, 0.00000f, 0.00000f},
    {TS_UPBA, 1, 0, 0.00000f, 0.00000f},
    {TS_UPBA, 2, 0, 0.00000f, 0.00000f},
    {TS_UPBA, 3, 0, 0.00000f, 0.00000f},
    {TS_UPBA, 4, 0, 0.00000f, 0.00000f},
    {TS_UPBA, 5, 0, -0.00191f, -0.00078f},
    {TS_UPBA, 6, 0, -0.00464f, -0.00189f},
    {TS_UPBA, 7, 0, -0.00573f, -0.00233f},
    {TS_UPBA, 8, 0, -0.00518f, -0.00211f},
    {TS_UPBA, 9, 0, -0.00300f, -0.00122f},
    {TS_UPBA, 10, 0, 0.00082f, 0.00033f},
    {TS_UPBA, 11, 0, 0.00628f, 0.00255f},
    {TS_UPBA, 12, 1, 0.01337f, 0.00544f},
    {TS_UPBA, 13, 1, 0.34782f, 2.55553f},
    {TS_UPBA, 14, 1, 0.35129f, 2.45635f},
    {TS_UPBA, 15, 1, 0.35466f, 2.35818f},
    {TS_UPBA, 16, 1, 0.35790f, 2.26101f},
    {TS_UPBA, 17, 1, 0.36104f, 2.16485f},
    {TS_UPBA, 18, 1, 0.36407f, 2.06970f},
    {TS_UPBA, 19, 1, 0.36698f, 1.97556f},
    {TS_UPBA, 20, 1, 0.36979f, 1.88242f},
    {TS_UPBA, 21, 1, 0.37248f, 1.79029f},
    {TS_UPBA, 22, 1, 0.37506f, 1.69916f},
    {TS_UPBA, 23, 1, 0.37753f, 1.60904f},
    {TS_UPBA, 24, 1, 0.37989f, 1.51993f},
    {TS_UPBA, 25, 1, 0.38213f, 1.43183f},
    {TS_UPBA, 26, 1, 0.38427f, 1.34473f},
    {TS_UPBA, 27, 1, 0.38629f, 1.25863f},
    {TS_UPBA, 28, 1, 0.38820f, 1.17355f},
    {TS_UPBA, 29, 1, 0.39000f, 1.08947f},
    {TS_UPBA, 30, 1, 0.39169f, 1.00640f},
    {TS_UPBA, 31, 1, 0.39326f, 0.92434f},
    {TS_UPBA, 32, 1, 0.39473f, 0.84328f},
    {TS_UPBA, 33, 1, 0.39608f, 0.76322f},
    {TS_UPBA, 34, 1, 0.39732f, 0.68419f},
    {TS_UPBA, 35, 1, 0.39845f, 0.60614f},
    {TS_UPBA, 36, 1, 0.39947f, 0.52912f},
    {TS_UPBA, 37, 1, 0.40038f, 0.45309f},
    {TS_UPBA, 38, 1, 0.40117f, 0.37807f},
    {TS_UPBA, 39, 1, 0.40186f, 0.30406f},
    {TS_UPBA, 40, 1, 0.40243f, 0.23106f},
    {TS_UPBA, 41, 1, 0.40290f, 0.15906f},
    {TS_UPBA, 42, 1, 0.40324f, 0.08807f},
    {TS_UPBA, 43, 1, 0.40349f, 0.01808f},
    {TS_UPBA, 44, 1, 0.40361f, -0.05088f},
    {TS_UPBA, 45, 1, 0.40362f, -0.11886f},
    {TS_UPBA, 46, 1, 0.40353f, -0.27163f},
    {TS_UPBA, 47, 1, 0.40332f, -0.49663f},
    {TS_UPBA, 48, 1, 0.40300f, -0.70176f},
    {TS_UPBA, 49, 1, 0.40257f, -0.88704f},
    {TS_UPBA, 50, 1, 0.40203f, -1.05243f},
    {TS_UPBA, 51, 1, 0.40137f, -1.19797f},
    {TS_UPBA, 52, 1, 0.40061f, -1.32366f},
    {TS_UPBA, 53, 1, 0.39973f, -1.42945f},
    {TS_UPBA, 54, 1, 0.39874f, -1.51540f},
    {TS_UPBA, 55, 1, 0.39764f, -1.58148f},
    {TS_UPBA, 56, 1, 0.39643f, -1.62769f},
    {TS_UPBA, 57, 1, 0.39511f, -1.65403f},
    {TS_UPBA, 58, 1, 0.39367f, -1.66051f},
    {TS_UPBA, 59, 1, 0.39212f, -1.64713f},
    {TS_UPBA, 60, 1, 0.39047f, -1.61388f},
    {TS_UPBA, 61, 1, 0.38869f, -1.56076f},
    {TS_UPBA, 62, 1, 0.38682f, -1.48778f},
    {TS_UPBA, 63, 1, 0.38482f, -1.39494f},
};

static void Upb_Bake(void)
{
    for (int i = 0; i < (int)countof(baked_upb); i++)
    {
        const BakedUpb *b = &baked_upb[i];
        UpbFrame *a = Upb_Get(b->ts, b->frame);
        a->x = b->x;
        a->y = b->y;
        a->can_land = b->can_land;
        a->seen = 1;
    }
    upb_turn[0] = upb_turn[1] = UPB_IASA_FRAME;
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

#define LL_MAX_CEILS 128
static FloorLine *ceil_cache; // [LL_MAX_CEILS], the stage's undersides
static int ceil_num;

static void Ceil_CacheRange(RawCollLine *lines, CollVert *verts, int start, int num)
{
    for (int i = 0; i < num && ceil_num < LL_MAX_CEILS; i++)
    {
        int id = start + i;
        u32 flags = lines[id].flags;
        if (!(flags & LINEFLAG_CEIL) || !(flags & LINEFLAG_ENABLED) || (flags & LINEFLAG_EMPTY))
            continue;
        FloorLine *c = &ceil_cache[ceil_num++];
        // mpCheckCeiling reads the ends as floors do (mpLib_8004ED5C): a
        // unit longer where another line joins, so a top point rising past
        // a ceiling's corner with a wall still catches it
        Line_GetEnds(lines, verts, id, &c->x0, &c->y0, &c->x1, &c->y1);
        c->id = id;
        c->is_platform = 0;
    }
}

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
    ceil_num = 0;
    for (CollGroup *group = *stc_firstcollgroup; group != 0; group = group->next)
    {
        CollGroupDesc *desc = group->desc;
        Floor_CacheRange(lines, verts, desc->floor_start, desc->floor_num);
        Floor_CacheRange(lines, verts, desc->dyn_start, desc->dyn_num);
        Ceil_CacheRange(lines, verts, desc->ceil_start, desc->ceil_num);
        Ceil_CacheRange(lines, verts, desc->dyn_start, desc->dyn_num);
    }

    Wall_BuildCache(lines, verts);
}

// mpCheckFloor: does the ECB bottom moving from a to b touch down on a floor?
static int floor_hit_platform; // the floor Floor_Check last found is a platform
static int floor_hit_id;       // ... its line
static float floor_hit_x;      // ... and where the bottom crossed it

static int Floor_Check(float ax, float ay, float bx, float by, int pass_platforms, int skip_line)
{
    for (int i = 0; i < floor_num; i++)
    {
        FloorLine *f = &floor_cache[i];
        if (f->id == skip_line || (pass_platforms && f->is_platform))
            continue;

        int hit;
        if (fabs(f->y0 - f->y1) > 0.0001f)
            hit = Line_Cross(f->x0, f->y0, f->x1, f->y1, ax, ay, bx, by);
        else
            hit = ay >= by && Line_CrossFlat(f->x0, f->y0, f->x1, ax, ay, bx, by);
        if (hit)
        {
            floor_hit_platform = f->is_platform;
            floor_hit_id = f->id;
            double lx = f->x1 - f->x0, ly = f->y1 - f->y0;
            double sa = lx * (ay - f->y0) - ly * (ax - f->x0);
            double sb = lx * (by - f->y0) - ly * (bx - f->x0);
            floor_hit_x = sa != sb ? ax + (bx - ax) * sa / (sa - sb) : bx;
            return 1;
        }
    }
    return 0;
}

// mpCheckCeiling: does the ECB top rising from a to b hit the underside of
// something? Jumps and falls then bonk (ftCo_StopCeil) and fall, and an
// aerial is pushed back down: either way, not the path predicted.
static int Ceil_Check(float ax, float ay, float bx, float by)
{
    if (by <= ay)
        return 0;
    for (int i = 0; i < ceil_num; i++)
    {
        FloorLine *c = &ceil_cache[i];
        if (Line_Cross(c->x0, c->y0, c->x1, c->y1, ax, ay, bx, by))
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

// Is x past the end of floor id, on the unit the game adds to it where it
// meets a wall or nothing (mpLib_8004ED5C), rather than on the floor or the
// next one?
static int Floor_PastEnd(int id, float x)
{
    Vec2 *v0 = Line_V0(id), *v1 = Line_V1(id);
    int rightward = v0->X < v1->X;
    float lo = rightward ? v0->X : v1->X, hi = rightward ? v1->X : v0->X;
    if (x >= lo && x <= hi)
        return 0;
    int end = (x < lo) == rightward ? Wall_Prev(id) : Wall_Next(id);
    return end == -1 || !(Line_Kind(end) & LINEFLAG_FLOOR);
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
    Vec2 drift;          // Falcon Dive's own drift speed (mv.ca.specialhi.vel)
    float special_lag;   // the helpless fall's landing lag (mv.co.fallspecial.landing_lag)
    int dodge_late;      // an airdodge's first frame, pressed in a late window
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
    int dodge_low;   // ... or at the shallowest angle down (WL_LOW_*)
    float lean;      // a fall's blend toward its leaning pose
    int lean_side;   // LEAN_FORWARD, LEAN_BACK or LEAN_NONE
    float locked_bottom; // bottom kept while the lock lasts
    float face;          // facing: Falcon Dive can turn him around
    Vec2 drift;          // Falcon Dive's drift speed
    float special_lag;   // landing lag once helpless
    int dodge_late;      // landing on the airdodge's second frame is perfect too
} SimState;

// What happened on one simulated frame.
typedef struct SimStep
{
    int landed;
    int ceiling;          // the ECB top hit a ceiling (and didn't land)
    int first_ecb;        // an aerial's or airdodge's own ECB bottom was used for the first time
    int platform;         // it landed on a platform (not the main floor)
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

// The waveland the cues time: the airdodge at the shallowest angle below
// sideways that isn't flat. Melee reads a stick axis within 0.2875 of the
// middle as 0, so y = -0.3 with the stick out to the gate's edge (about
// 0.925) is as flat as a down angle gets: about 18 degrees. It lands from
// a little higher than a flat dodge and slides 95% as fast. Pressed up to
// WL_LOW_MAX_TAN (22 degrees), a dodge that lands right away is judged
// perfect.
#define WL_LOW_COS 0.95122f
#define WL_LOW_SIN 0.30851f
#define WL_LOW_MAX_TAN 0.40403f

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
    float top[LL_SIM_FRAMES + 1];      // ECB top per frame, for the log
    s8 ts[LL_SIM_FRAMES + 1];          // tracked state per frame
    EcbSample land_ecb;                // ECB at touchdown

    // aerial interrupts: pressing an aerial on frame k whose ECB then
    // touches down the first time it is used
    u8 ai_mask[LL_SIM_FRAMES + 1];     // aerials that interrupt when pressed on frame k
    u8 ai_lag_mask[LL_SIM_FRAMES + 1]; // ... of those, the ones that land with aerial lag
    u32 ai_delay[LL_SIM_FRAMES + 1];   // frames from k until each aerial's touchdown (lock), 4 bits per aerial: Ai_Delay
    u8 ai_show[LL_SIM_FRAMES + 1];     // ... the ones worth showing: no aerial lag, and done
                                       // at least LL_AI_MIN_GAIN frames before holding would be
    u8 ai_unlearned;                   // aerials skipped because their ECB isn't learned yet
    int ai_first;                      // first frame of the first shown window, 0 = none
    int ai_width;                      // frames in that window
    u8 ai_aerials;                     // aerials that work somewhere in that window

    // perfect wavelands: an airdodge sideways or at the shallowest angle
    // down, on frame k, whose ECB touches down the first time it is used,
    // keeping all (or 95%) of the dodge's speed. Off a plain fall that's
    // there on about every other jump, so when it isn't the window is the
    // frames where it lands one frame later instead (wl_late).
    u8 wl_mask[LL_SIM_FRAMES + 1];   // DODGE_RIGHT / DODGE_LEFT that work when pressed on frame k
    u8 wl_late_mask[LL_SIM_FRAMES + 1]; // ... that land on the dodge's second frame
    u8 wl_ground[LL_SIM_FRAMES + 1]; // ... of those, the ones landing on the main floor
    u8 wl_unlearned;                 // the airdodge's ECB isn't learned yet
    int wl_first;                    // first frame of the first window, 0 = none
    int wl_width;
    u8 wl_dirs;                      // directions that work somewhere in that window
    int wl_late;                     // the window lands a frame late
} Prediction;

// Frames from pressing an aerial (TS_AIR*) on frame k to its touchdown. The
// aerials touch down at different frames, so each keeps its own, in 4 bits
// (a press is followed for LL_AI_MAX_STEPS frames at most, under 16).
static int Ai_Delay(Prediction *p, int k, int aerial)
{
    return (p->ai_delay[k] >> (4 * (aerial - TS_AIRN))) & 15;
}

// The soonest touchdown among the aerials in mask, pressed on frame k.
static int Ai_FirstDelay(Prediction *p, int k, u8 mask)
{
    int first = 99;
    for (int a = TS_AIRN; a <= TS_AIRLW; a++)
    {
        if ((mask & AERIAL_BIT(a)) && Ai_Delay(p, k, a) < first)
            first = Ai_Delay(p, k, a);
    }
    return first == 99 ? 0 : first;
}

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
static float common_air_friction_oob;
static float common_upb_drift_stick;
static float common_drop_stick;  // as a positive number: stick y <= -this
static int common_drop_window;
static float common_spot_stick;  // as a negative number: stick y <= this
static int common_spot_window;

static int ai_show_all; // the AI Filter option is on All
static u8 ai_only;      // the AI Aerial option: the one aerial to count down to, 0 = any

static float Common_Float(int offset)
{
    return *(float *)((u8 *)*stc_ftcommon + offset);
}

static int Common_Int(int offset)
{
    return *(int *)((u8 *)*stc_ftcommon + offset);
}

// A frame count the decomp may type as a float (the shield drop's): a word
// that isn't a small int is read as a float.
static int Common_Frames(int offset)
{
    int n = Common_Int(offset);
    return n >= 0 && n < 256 ? n : (int)Common_Float(offset);
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

// Falcon Dive's drift speed with the stick held at stick_x
// (ftCa_SpecialHi_Phys): over its max it slows by the out-of-bounds friction
// (DeaccelQuick); otherwise it goes straight for the stick's target
// (DriftSimple_NoFriction), and stops at once with the stick near the middle.
static float Upb_DriftAccel(FighterData *fp, float vel, float stick_x)
{
    float max = Captain_Attr(fp, CA_UPB_DRIFT_MAX) * fp->attr.aerial_drift_max;
    float accel;
    if (fabs(vel) > max)
    {
        accel = common_air_friction_oob;
        if (fabs(accel) >= fabs(vel))
            return -vel;
        return vel > 0 ? -accel : accel;
    }
    if (fabs(stick_x) < common_upb_drift_stick)
        return -vel;

    // ftCommon_CalcSelfAccel_AccelToVel
    accel = stick_x * fp->attr.aerial_drift_stick_mult * Captain_Attr(fp, CA_UPB_DRIFT_ACCEL);
    float target = stick_x * max;
    if (!(vel * accel < 0))
    {
        if (accel > 0)
        {
            if (vel + accel > target)
                accel = target - vel;
        }
        else if (vel + accel < target)
            accel = target - vel;
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

// The live fall's blend (mv.co.fall.x4, the second state variable; the
// helpless fall keeps its own in the same place).
static float Lean_FromFighter(FighterData *fp, int ts)
{
    float lean = 0;
    if (Is_Fall(ts) || ts == TS_FALLSPECIAL)
        memcpy(&lean, &fp->state_var.state_var2, sizeof(lean));
    return lean;
}

// Whether the live frame's ECB can be learned: a fall only teaches its plain
// pose. vx is the speed the animation saw (last frame's). The helpless
// fall's leaning poses aren't simulated, so it only learns its plain one.
static int Lean_IsPlain(FighterData *fp, int ts, float vx)
{
    if (!Is_Fall(ts) && ts != TS_FALLSPECIAL)
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
    s->drift = (Vec2){0, 0};
    s->special_lag = 0;
    s->dodge_late = 0;
    if (Tracked_IsUpB(ts))
        s->drift = Upb_Drift(fp);
    else if (ts == TS_FALLSPECIAL)
        memcpy(&s->special_lag, (u8 *)&fp->state_var + FALLSPECIAL_LAG, sizeof(float));
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
    s->dodge_low = start->ts == TS_ESCAPEAIR && start->vel.Y < 0 && -start->vel.Y <= fabs(start->vel.X) * WL_LOW_MAX_TAN;
    s->lean = start->lean;
    s->lean_side = LEAN_NONE;
    s->locked_bottom = start->locked_bottom;
    s->face = start->facing;
    s->drift = start->drift;
    s->special_lag = start->special_lag;
    s->dodge_late = start->dodge_late;
}

// One frame, in the game's order: animation (the state can end), interrupt
// (press, an aerial or airdodge replaces the state), input timers, physics,
// ECB, floor test. press is the aerial or TS_ESCAPEAIR pressed this frame,
// or -1; dodge_x is the airdodge's direction (+1 right, -1 left),
// horizontal unless sim_dodge_y is set (then both are the unit direction's
// parts).
static int sim_steps; // simulated frames, to spread the ledge route search over game frames
static float sim_dodge_y;

static void Sim_Step(FighterData *fp, SimStart *start, SimState *s, int press, float dodge_x, SimStep *out)
{
    sim_steps++;
    out->landed = 0;
    out->ceiling = 0;
    out->first_ecb = 0;
    out->platform = 0;
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
        else if (next == TS_FALLSPECIAL)
        {
            // ftCo_80096900 keeps the speed and fastfall; the landing lag
            // comes from what ran out
            s->special_lag = s->ts == TS_ESCAPEAIR ? common_waveland_lag : Captain_Attr(fp, CA_UPB_LANDING_LAG);
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
            s->vy = sim_dodge_y * common_dodge_force;
            s->dodge_flat = sim_dodge_y == 0;
            s->dodge_low = sim_dodge_y != 0;
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
            s->lean_side = Lean_Update(fp, s->vx, s->face, &s->lean);
    }

    // Falcon Dive's IASA turns him toward a stick held far enough sideways
    // (ftCommon_UpdateFacing), before its movement this frame
    if (Tracked_IsUpB(s->ts) && s->frame == upb_turn[s->ts - TS_UPB] &&
        fabs(start->stick_x) > Captain_Attr(fp, CA_UPB_TURN_STICK))
        s->face = start->stick_x >= 0 ? 1.f : -1.f;
    // ... and lifts off, keeping the ECB bottom it had
    if (Tracked_IsUpB(s->ts) && s->frame == UPB_LIFTOFF_FRAME)
    {
        s->lock = UPB_LIFTOFF_LOCK;
        s->locked_bottom = s->bottom;
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
    else if (Tracked_IsUpB(s->ts))
    {
        // ftCa_SpecialHi_Phys: its own drift speed, with the animation's
        // movement on top; no gravity
        UpbFrame *a = Upb_Get(s->ts, s->frame);
        if (!a->seen)
            out->unlearned = 1;
        s->drift.X += Upb_DriftAccel(fp, s->drift.X, start->stick_x);
        s->vx = a->x * s->face + s->drift.X;
        s->vy = a->y + s->drift.Y;
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
        s->bottom = s->locked_bottom;
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
        Ecb_FromSample(e, s->face, &ecb);
    Ecb_Fix(&ecb);

    // walls push the fighter out before the floor test (mpColl_80046904)
    Sim_Walls(&s->x, s->y, s->pos_x, s->pos_y, &s->ecb, &ecb);
    int ceiling = Ceil_Check(s->pos_x, s->pos_y + s->ecb.top, s->x, s->y + ecb.top);
    s->ecb = ecb;
    s->pos_x = s->x;
    s->pos_y = s->y;

    // floor test (mpColl_80044628_Floor)
    int pass_platforms = Tracked_UsesPlatformDrop(s->ts) && start->stick_y <= common_platform_drop;
    float bx = s->x;
    float by = s->y + s->bottom;
    out->landed = Floor_Check(s->prev_x, s->prev_y, bx, by, pass_platforms, start->skip_line);
    // An aerial's own ECB, used for the first time, that only meets the
    // floor on the unit the game adds past its end doesn't touch down in
    // game, though the decomp reads as if it would. Seen on Battlefield's
    // left ledge: nair pressed a frame before the real AI window, rising
    // past the corner, landed there in the sim only (twice, on two routes).
    if (out->landed && out->first_ecb && Tracked_IsAerial(s->ts) && Floor_PastEnd(floor_hit_id, floor_hit_x))
        out->landed = 0;
    out->platform = out->landed && floor_hit_platform;
    out->ceiling = ceiling && !out->landed; // a touchdown is handled first (ft_800835B0)
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
// for the airdodge, Falcon Dive and the helpless fall. An aerial whose own
// ECB touches down the first time it's used is an aerial interrupt,
// whatever its lag; a horizontal airdodge that does the same is a perfect
// waveland.
static int Landing_Kind(FighterData *fp, SimState *s, EcbSample *e, int first_ecb, int *lag, int *lcancel_lag)
{
    float normal_lag = fp->attr.normal_landing_lag;
    *lcancel_lag = 0;

    if (Tracked_IsHelpless(s->ts))
    {
        // before its IASA, Falcon Dive goes on along the floor instead
        // (ft_80083B68)
        if (Tracked_IsUpB(s->ts))
        {
            UpbFrame *a = Upb_Get(s->ts, s->frame);
            if (a->seen && !a->can_land)
            {
                *lag = 0;
                return LAND_OTHER;
            }
            *lag = (int)Captain_Attr(fp, CA_UPB_LANDING_LAG);
        }
        else
            *lag = (int)s->special_lag;
        return LAND_NORMAL;
    }

    if (s->ts == TS_ESCAPEAIR)
    {
        // pressed in a late window, landing a frame later counts (as the
        // judge does)
        int now = first_ecb || (s->dodge_late && s->frame == 1);
        *lag = (int)common_waveland_lag;
        return now && (s->dodge_flat || s->dodge_low) ? LAND_PERFECT_WL : LAND_WAVELAND;
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
static int branch_platform; // where Branch_Press landed: a platform
static int branch_first;    // ... with its own ECB's first use (not late)

// late: frames its own ECB may already be in use and still count (the
// late waveland window).
static int Branch_Press(FighterData *fp, SimStart *start, SimState *before, int press, float dodge_x, int late,
                        int *unlearned, int *lag)
{
    SimState b = *before;
    SimStep step;
    int used = 0; // frames its ECB was in use without landing

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
        if (step.ceiling)
            return -1;
        if (step.landed)
        {
            // touching down on the locked bottom isn't an interrupt
            if (!step.first_ecb && !(late && used > 0))
                return -1;
            *lag = step.ecb->aerial_lag;
            branch_platform = step.platform;
            branch_first = step.first_ecb;
            return n;
        }
        if (!b.ecb_pending && ++used > late)
            return -1; // its ECB is in use and stayed in the air
    }
    return -1;
}

// What Predict tries on the way: aerials, sideways airdodges, and every frame
// of the post-jump lock even far from a floor (a press there lands when the
// lock runs out, somewhere else).
#define BR_AI 1
#define BR_WL 2
#define BR_LOCK 4
#define BR_ALL (BR_AI | BR_WL | BR_LOCK)

// Try every aerial and both horizontal airdodges on frame k. Aerials and the
// airdodge can't be pressed out of an aerial (until its IASA, which isn't
// tracked) or an airdodge, but jumps and falls can on any frame. An aerial
// pressed on the frame you'd land anyway keeps falling and lands anyway, so
// it isn't an interrupt; a horizontal airdodge stops the fall, so it's
// tried on that frame too.
static void Branch_Actions(FighterData *fp, SimStart *start, SimState *before, Prediction *p, int k, int landing, int branches)
{
    int ts = p->ts[k];
    if (Tracked_IsAerial(ts) || ts == TS_ESCAPEAIR || Tracked_IsHelpless(ts))
        return;

    if (!landing && (branches & BR_AI))
    {
        for (int a = TS_AIRN; a <= TS_AIRLW; a++)
        {
            int unlearned = 0, lag = 0;
            int n = Branch_Press(fp, start, before, a, 0, 0, &unlearned, &lag);
            if (unlearned)
                p->ai_unlearned |= AERIAL_BIT(a);
            if (n < 0)
                continue;
            p->ai_mask[k] |= AERIAL_BIT(a);
            p->ai_delay[k] |= (u32)(n & 15) << (4 * (a - TS_AIRN));
            if (lag)
                p->ai_lag_mask[k] |= AERIAL_BIT(a);
        }
    }

    // the shallowest airdodge down lands wherever a flat one does, and a
    // little higher too
    sim_dodge_y = -WL_LOW_SIN;
    for (int d = 0; d < 2 && (branches & BR_WL); d++)
    {
        u8 bit = d == 0 ? DODGE_RIGHT : DODGE_LEFT;
        float dx = d == 0 ? WL_LOW_COS : -WL_LOW_COS;
        int unlearned = 0, lag = 0;
        int n = Branch_Press(fp, start, before, TS_ESCAPEAIR, dx, 1, &unlearned, &lag);
        if (n >= 0 && branch_first)
            p->wl_mask[k] |= bit;
        else if (n >= 0)
            p->wl_late_mask[k] |= bit;
        if (n >= 0 && !branch_platform)
            p->wl_ground[k] |= bit;
        if (unlearned)
            p->wl_unlearned = 1;
    }
    sim_dodge_y = 0;
}

// Which aerial interrupts are worth showing, and the first window of each
// kind. An AI that lands with aerial lag (uair, or a lock that ran into a
// fair's or bair's lag frames) never helps, one that finishes its landing
// lag barely sooner than just holding would isn't worth the press, and
// neither is one that lands while falling. The AI Filter option can show
// them all.
static int Cues_WavelandGround(void);

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
        if (ai_only)
            m &= ai_only;
        if (!ai_show_all)
        {
            m &= ~p->ai_lag_mask[k];
            for (int a = TS_AIRN; a <= TS_AIRLW; a++)
            {
                if (!(m & AERIAL_BIT(a)))
                    continue;
                int t = k + Ai_Delay(p, k, a);
                if (hold_done - (t + normal_lag) < LL_AI_MIN_GAIN)
                    m &= ~AERIAL_BIT(a);
                // falling, an aerial's ECB is at most about a unit lower
                // than the fall's: a frame sooner at best, and no better
                // than a NIL. The ones that look like big savings while
                // falling are platform catches (holding down drops through
                // a platform an aerial lands on). Only rising ones are
                // worth the press.
                else if (t <= p->num && p->pos[t].Y <= p->pos[t - 1].Y)
                    m &= ~AERIAL_BIT(a);
            }
        }
        p->ai_show[k] = m;
    }

    // Useful: when the aerials' windows differ, the first window keeps to
    // the frames of the aerial that works on most of them (nair first on a
    // tie), so the timer doesn't open on a frame only another aerial has
    // (off Battlefield's left ledge, dair can land a frame before nair)
    if (!ai_show_all)
    {
        int a = 1;
        while (a <= last_ai && !p->ai_show[a])
            a++;
        int b = a;
        while (b < last_ai && p->ai_show[b + 1])
            b++;
        if (a <= last_ai)
        {
            static const u8 order[5] = {TS_AIRN, TS_AIRF, TS_AIRB, TS_AIRLW, TS_AIRHI};
            u8 main = 0;
            int most = 0;
            for (int i = 0; i < 5; i++)
            {
                u8 bit = AERIAL_BIT(order[i]);
                int n = 0;
                for (int k = a; k <= b; k++)
                    n += (p->ai_show[k] & bit) != 0;
                if (n > most)
                    most = n, main = bit;
            }
            while (!(p->ai_show[a] & main))
                p->ai_show[a++] = 0;
            while (!(p->ai_show[b] & main))
                p->ai_show[b--] = 0;
        }
    }

    for (int k = 1; k <= last_ai; k++)
    {
        u8 m = p->ai_show[k];
        if (m && (p->ai_first == 0 || k == p->ai_first + p->ai_width))
        {
            if (p->ai_first == 0)
                p->ai_first = k;
            p->ai_width++;
            p->ai_aerials |= m;
        }
    }

    // the first frame-one window; without one, the first one-frame-late one
    p->wl_late = 0;
    for (int late = 0; late < 2; late++)
    {
        p->wl_first = 0;
        p->wl_width = 0;
        p->wl_dirs = 0;
        for (int k = 1; k <= last_wl; k++)
        {
            u8 m = late ? p->wl_late_mask[k] : p->wl_mask[k];
            if (!Cues_WavelandGround())
                m &= ~p->wl_ground[k];
            if (m && (p->wl_first == 0 || k == p->wl_first + p->wl_width))
            {
                if (p->wl_first == 0)
                    p->wl_first = k;
                p->wl_width++;
                p->wl_dirs |= m;
            }
        }
        if (p->wl_first)
        {
            p->wl_late = late;
            break;
        }
    }
}

// The airdodge directions that work on frame k in the kind of window the
// prediction shows (frame one, or a frame late), on the floors the cues
// cover.
static u8 WL_Mask(Prediction *p, int k)
{
    u8 m = p->wl_late ? p->wl_late_mask[k] : p->wl_mask[k];
    if (!Cues_WavelandGround())
        m &= ~p->wl_ground[k];
    return m;
}

// How far Predict goes: all of LL_SIM_FRAMES, and until it falls below
// sim_bottom_y. The ledge route search looks less far.
static int sim_limit = LL_SIM_FRAMES;
static float sim_bottom_y = -100000.f;

// Simulate keeping the stick where it is and pressing nothing. With
// branches (BR_*), also try aerials and airdodges on the frames along the way.
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
    p->top[0] = s.ecb.top;
    p->ts[0] = s.ts;
    p->ai_mask[0] = 0;
    p->ai_lag_mask[0] = 0;
    p->ai_show[0] = 0;
    p->ai_unlearned = 0;
    p->wl_mask[0] = 0;
    p->wl_late_mask[0] = 0;
    p->wl_ground[0] = 0;
    p->wl_unlearned = 0;

    for (int k = 1; k <= sim_limit; k++)
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
        p->top[k] = s.ecb.top;
        p->ts[k] = s.ts;
        p->ai_mask[k] = 0;
        p->ai_lag_mask[k] = 0;
        p->ai_delay[k] = 0;
        p->ai_show[k] = 0;
        p->wl_mask[k] = 0;
        p->wl_late_mask[k] = 0;
        p->wl_ground[k] = 0;
        p->num = k;

        if (branches && (((branches & BR_LOCK) && before.lock > 0) || Floor_Near(s.x, s.y + s.bottom)))
            Branch_Actions(fp, start, &before, p, k, step.landed, branches);

        if (step.landed)
        {
            p->land_frame = k;
            p->land_ecb = *step.ecb;
            p->land_kind = Landing_Kind(fp, &s, step.ecb, step.first_ecb, &p->lag, &p->lcancel_lag);
            break;
        }
        if (step.ceiling)
        {
            Mark_Uncertain(p, k); // bonks: no landing on this path
            break;
        }
        if (s.vy < 0 && s.y < sim_bottom_y)
            break;
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
    s->lean = 0;
    s->drift = (Vec2){0, 0};
    s->special_lag = 0;
    s->dodge_late = 0;
    return until;
}

// A double jump pressed on this frame from a jump or fall
// (ftCo_JumpAerial_Enter_Basic): its speed replaces the old one, the ECB
// bottom stays where it was for 10 frames (ftCommon_8007D5D4) and the stick
// timer resets, then the frame plays out with the usual air physics.
// start's stick is the one held on this frame.
static void Sim_DoubleJump(FighterData *fp, SimStart *start, SimState *s, SimStep *out)
{
    int ts = start->stick_x * start->facing > -common_jump_back_stick ? TS_JUMPAERIALF : TS_JUMPAERIALB;
    s->ts = ts;
    s->frame = -1;
    s->len = state_len[ts];
    s->len_estimated = 0;
    if (s->len == 0)
    {
        s->len = LL_GUESS_JUMP_LEN;
        s->len_estimated = 1;
    }
    s->vx = start->stick_x * fp->attr.air_jump_h_multiplier;
    s->vy = fp->attr.jump_v_initial_velocity * fp->attr.air_jump_v_multiplier;
    s->fastfall = 0;
    s->tilt_timer = LL_TIMER_MAX;
    s->lock = 10;
    s->locked_bottom = s->bottom;
    s->ecb_pending = 0;
    Sim_Step(fp, start, s, -1, 0, out);
}

// Go on from a simulated frame as if it were the live one, holding the stick
// in start.
static void Sim_ToStart(SimState *s, SimStart *start, SimStart *out)
{
    *out = *start;
    out->pos.X = s->x;
    out->pos.Y = s->y;
    out->vel.X = s->vx;
    out->vel.Y = s->vy;
    out->bottom = s->bottom;
    out->locked_bottom = s->locked_bottom;
    out->ecb = s->ecb;
    out->ts = s->ts;
    out->frame = s->frame;
    out->len = s->len;
    out->len_estimated = s->len_estimated;
    out->fastfall = s->fastfall;
    out->tilt_timer = s->tilt_timer;
    out->trigger_timer = s->trigger_timer;
    out->ecb_lock = s->lock;
    out->lean = s->lean;
    out->facing = s->face;
    out->drift = s->drift;
    out->special_lag = s->special_lag;
    out->dodge_late = 0;
}

///////////////////////
/// Event state     ///
///////////////////////

void Event_Exit(GOBJ *menu);
void Event_ClearLearned(GOBJ *menu);
void Event_ChangeCollDisplay(GOBJ *menu, int value);
void Event_ChangeScript(GOBJ *menu, int value);
void Event_ChangeCamera(GOBJ *menu, int value);
void Event_ChangeView(GOBJ *menu, int value);
void Event_SaveView(GOBJ *menu);
void Event_NameView(GOBJ *menu);
void Event_PresetName(GOBJ *menu);
void Event_ChangeLedgeStart(GOBJ *menu, int value);
void Event_ChangeRoutes(GOBJ *menu, int value);
void Event_ChangePreset(GOBJ *menu, int value);
void Event_ChangePresetStart(GOBJ *menu, int value);
void Event_PresetLoad(GOBJ *menu);
void Event_PresetSave(GOBJ *menu);

static const char *speed_names[] = {"1", "5/6", "2/3", "1/2", "1/4"};
static const float speed_values[] = {1.f, 5.f / 6.f, 2.f / 3.f, 1.f / 2.f, 1.f / 4.f};
static const char *preview_names[] = {"Both", "Full hop", "Short hop", "Off"};
static const char *panel_side_names[] = {"Auto", "Right", "Left"};
static const char *tick_names[] = {"Frame Advance", "Always", "Off"};
static const char *intensity_names[] = {"1 Faint", "2 Soft", "3 Standard", "4 Bold", "5 Boldest"};
static const char *mark_size_names[] = {"Small", "Medium", "Large"};
static const float mark_sizes[] = {0.9f, 1.25f, 1.6f};
static const char *ai_filter_names[] = {"Useful", "All"};
static const char *ai_aerial_names[] = {"Any", "Nair", "Fair", "Bair", "Uair", "Dair"}; // after Any, in TS_AIRN order
static const char *wl_cue_names[] = {"Off", "Platforms", "All Floors"};
static const char *near_names[] = {"Off", "Bubble", "Halo", "Pincers", "ECB Fill", "Lights", "Old Strip"};
static const char *strip_names[] = {"Off", "Cells", "Highway", "Dial"};
static const char *wl_timer_names[] = {"Off", "Ticks", "Rails", "Chevrons"};
static const char *wd_timer_names[] = {"Off", "Cells", "Pips", "Ring"};
static const char *stick_names[] = {"By Percent", "Bottom Left", "Bottom Right", "Off"};
static const char *pad_look_names[] = {"Ring", "Crest", "Classic"};
static const char *pad_cue_names[] = {"Off", "Closing Ring", "Gauge"};
enum { PADCUE_OFF, PADCUE_RING, PADCUE_GAUGE };
static const float ring_sizes[] = {1.f, 1.25f, 1.5f};
static const char *adv_button_names[] = {"L", "Z", "X", "Y", "R"};
static const int adv_button_masks[] = {HSD_TRIGGER_L, HSD_TRIGGER_Z, HSD_BUTTON_X, HSD_BUTTON_Y, HSD_TRIGGER_R};
static const char *route_kind_names[] = {"NIL", "AI", "Both"};
static const char *route_sort_names[] = {"GALINT", "Easiest"};
// The Route option is a number from 1 to the routes found. Its value text
// ("4 of 23") and the first lines of its description (the chosen route's
// GALINT and inputs) are rewritten as the list and the choice change.
static char route_pick_fmt[24] = "%d";
static char route_desc[3][56];
static const char *wait_names[] = {"5 s", "3 s", "2 s", "1 s", "0.5 s", "0.25 s", "0.1 s"};
static const int wait_frames[] = {300, 180, 120, 60, 30, 15, 6};
static const char *reset_names[] = {"None", "Same Side", "Swap", "Swap on Success", "Random"};
static const char *reset_delay_names[] = {"Slow", "Normal", "Fast", "Instant"};
static const int reset_delay_hit[] = {120, 60, 30, 1};
static const int reset_delay_miss[] = {60, 20, 1, 1};
static const char *start_names[] = {"Ledge", "Saved Position"};
static const char *cam_names[] = {"Normal", "Zoom", "Fixed", "Advanced"};
#define VIEW_SLOTS 8 // camera views per stage
static char view_label[VIEW_SLOTS][20];
static const char *view_names[VIEW_SLOTS + 1] = {"None", view_label[0], view_label[1], view_label[2], view_label[3],
                                                 view_label[4], view_label[5], view_label[6], view_label[7]};
#define LL_SCRIPT_MAX 48 // scripts read from the script file
static const char *script_names[LL_SCRIPT_MAX + 2] = {"Off"}; // and All
// Debug Log levels: each one adds to the one before
enum { LOG_OFF, LOG_LANDINGS, LOG_FRAMES, LOG_ALL };
static const char *log_level_names[] = {"Off", "Landings", "Frames", "Everything"};

// The timers' looks (see the Timers menu). Each draws the same cues.
enum near_kind
{
    NEAR_OFF,
    NEAR_BUBBLE,  // a ring closes on a bubble where his body will be at the press
    NEAR_HALO,    // a bead runs around a ring on him into the window's notch
    NEAR_PINCERS, // brackets as tall as him close in from both sides
    NEAR_ECB,     // his ECB diamond fills, then spikes onto the floor
    NEAR_LIGHTS,  // a fuse, then one light a frame over his head
    NEAR_STRIP,   // the old cell strip that finds room around him
};

enum strip_kind
{
    STRIP_OFF,
    STRIP_CELLS,   // cells slide into a gate
    STRIP_HIGHWAY, // notes fall onto a line, one lane per cue
    STRIP_DIAL,    // a hand sweeps into the window's wedge
};

enum wl_timer_kind
{
    WLT_OFF,
    WLT_TICKS,    // ticks slide in from the slide's ends and spike where they meet
    WLT_RAILS,    // the rails fill in from the ends
    WLT_CHEVRONS, // arrowheads hop in a notch a frame
};

enum wd_timer_kind
{
    WDT_OFF,
    WDT_CELLS, // a row in the strips
    WDT_PIPS,  // a pip a jumpsquat frame under his feet
    WDT_RING,  // a ring on the floor tightens each frame
};

enum stick_place
{
    STICK_PERCENT,
    STICK_LEFT,
    STICK_RIGHT,
    STICK_OFF,
};

enum pad_look
{
    LOOK_RING,
    LOOK_CREST,
    LOOK_CLASSIC,
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

enum route_kind
{
    ROUTES_NIL,
    ROUTES_AI,
    ROUTES_BOTH,
};

enum route_sort
{
    ROUTES_SORT_GALINT,
    ROUTES_SORT_EASIEST,
};

enum reset_kind
{
    RESET_NONE,
    RESET_SAME,
    RESET_SWAP,
    RESET_SWAP_HIT,
    RESET_RANDOM,
};

enum start_kind
{
    START_LEDGE,
    START_SAVED,
};

// Ledge practice: the routes off the ledge, the quicktime assist, and the
// ledgedash training's reset and camera options.
enum options_ledge
{
    LOPT_ROUTES,
    LOPT_KIND,
    LOPT_SORT,
    LOPT_PICK,
    LOPT_ASSIST,
    LOPT_WAIT,
    LOPT_START,
    LOPT_RESET,
    LOPT_DELAY,
    LOPT_INV,
    LOPT_DROP,

    LOPT_COUNT
};

static EventOption Options_Ledge[LOPT_COUNT] = {
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Show Routes",
        .val = 1,
        .desc = {"While Falcon hangs, show the fastest ways from",
                 "the ledge to the stage in the timer, with the",
                 "ledge intangibility (GALINT) each one keeps.",
                 "The chosen route is on top, the next ones under."},
        .OnChange = Event_ChangeRoutes,
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Route Kind",
        .val = ROUTES_AI,
        .value_num = countof(route_kind_names),
        .values = route_kind_names,
        .desc = {"NIL: land on the stage with no landing lag.",
                 "AI: land with an aerial interrupt.",
                 "Both: the two lists together."},
        .OnChange = Event_ChangeRoutes,
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Route Sort",
        .value_num = countof(route_sort_names),
        .values = route_sort_names,
        .desc = {"How the routes are put in order.",
                 "GALINT: the most ledge intangibility first.",
                 "Easiest: widest aerial window, no fastfall and",
                 "least waiting first, then by GALINT."},
        .OnChange = Event_ChangeRoutes,
    },
    {
        .kind = OPTKIND_INT,
        .name = "Route",
        .value_min = 1,
        .value_num = 1,
        .val = 1,
        .format = route_pick_fmt,
        .desc = {route_desc[0], route_desc[1], route_desc[2], "Left/right browse, or D-pad up on the ledge."},
        .OnChange = Event_ChangeRoutes,
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Assist",
        .desc = {"Quicktime practice: the game freezes on each",
                 "input of the route and waits for it, then plays",
                 "on at full speed. A slip only misses if it costs",
                 "the landing, or its timing is checked in Sounds."},
        .OnChange = Event_ChangeRoutes,
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Assist Wait",
        .value_num = countof(wait_names),
        .val = 2,
        .values = wait_names,
        .desc = {"How long Assist waits for each input before it",
                 "steps a frame or starts over. Shorten it as the",
                 "timing sinks in, down toward real time."},
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Starting Position",
        .value_num = countof(start_names),
        .values = start_names,
        .desc = {"Where Reset puts Falcon after an attempt: on a",
                 "ledge, or where you saved with D-pad right."},
        .OnChange = Event_ChangeLedgeStart,
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Reset",
        .value_num = countof(reset_names),
        .values = reset_names,
        .desc = {"Start over after each attempt. Swap and Random",
                 "change ledges. Assist always starts over on the",
                 "same side when this is None."},
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Reset Delay",
        .value_num = countof(reset_delay_names),
        .val = 1,
        .values = reset_delay_names,
        .desc = {"How long the result stays before starting over."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Keep Ledge Invincibility",
        .desc = {"Keep the full intangibility while hanging, so",
                 "the routes don't shrink while you get ready."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Drop Drill",
        .desc = {"Time letting go of the ledge: a DRP row counts",
                 "down to the first frame you can. The stick has to",
                 "rest on the hang's first frame, then go down or",
                 "away. Graded, with how many of your last 10 hit."},
    },
};

static EventMenu Menu_Ledge = {
    .name = "Ledge Practice",
    .option_num = countof(Options_Ledge),
    .options = Options_Ledge,
};

// Jump timing: in a fall with the double jump left, when to jump, with which
// stick, and which aerial to press when, to land as an aerial interrupt or
// a NIL on a platform or the floor.
enum options_jump
{
    JOPT_SHOW,
    JOPT_KIND,

    JOPT_COUNT
};

static EventOption Options_Jump[JOPT_COUNT] = {
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Show",
        .desc = {"While Falcon falls with his double jump left,",
                 "show when to jump, with which stick, and which",
                 "aerial to press when, to land as an aerial",
                 "interrupt or a NIL on a platform or the floor."},
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Kind",
        .val = ROUTES_AI,
        .value_num = countof(route_kind_names),
        .values = route_kind_names,
        .desc = {"NIL: land with no landing lag.",
                 "AI: land with an aerial interrupt.",
                 "Both: whichever lets Falcon act sooner."},
    },
};

static EventMenu Menu_Jump = {
    .name = "Jump Timing",
    .option_num = countof(Options_Jump),
    .options = Options_Jump,
};

// The Training Lab's camera modes. Presets come with saved settings.
enum options_camera
{
    CAMOPT_MODE,
    CAMOPT_VIEW,
    CAMOPT_SAVE,
    CAMOPT_NAME,

    CAMOPT_COUNT
};

#define CAM_ADVANCED 3 // cam_names

static EventOption Options_Camera[CAMOPT_COUNT] = {
    {
        .kind = OPTKIND_STRING,
        .name = "Camera Mode",
        .value_num = countof(cam_names),
        .values = cam_names,
        .desc = {"Adjust the camera's behavior.",
                 "In advanced mode, use C-Stick while holding",
                 "A/B/Y to pan, rotate and zoom, respectively."},
        .OnChange = Event_ChangeCamera,
    },
    {
        .kind = OPTKIND_STRING,
        .name = "View",
        .value_num = countof(view_names),
        .values = view_names,
        .desc = {"Jump the camera to a saved view (Advanced mode,",
                 "so it can still be moved). Each stage has its own",
                 "8 views. Presets keep which one they were saved",
                 "with."},
        .OnChange = Event_ChangeView,
    },
    {
        .kind = OPTKIND_FUNC,
        .name = "Save View",
        .desc = {"Keep the camera as it is now as the view picked",
                 "above, on this stage. Set it up in Advanced",
                 "mode, or pause on a moment you like."},
        .OnSelect = Event_SaveView,
    },
    {
        .kind = OPTKIND_FUNC,
        .name = "Name View",
        .desc = {"Give the view picked above a name (save it",
                 "first)."},
        .OnSelect = Event_NameView,
    },
};

static EventMenu Menu_Camera = {
    .name = "Camera",
    .option_num = countof(Options_Camera),
    .options = Options_Camera,
};

// Every button the event uses outside the menu.
static EventOption Options_Controls[] = {
    {
        .kind = OPTKIND_INFO,
        .name = "Menu",
        .desc = {"Start opens this menu."},
    },
    {
        .kind = OPTKIND_INFO,
        .name = "Save Position",
        .desc = {"Hold D-pad right to save where Falcon is."},
    },
    {
        .kind = OPTKIND_INFO,
        .name = "Load Position",
        .desc = {"D-pad left puts Falcon back where you saved.",
                 "When Reset starts from the ledge, it starts a",
                 "new attempt instead, and with a test script",
                 "chosen it plays the script again."},
    },
    {
        .kind = OPTKIND_INFO,
        .name = "Quick: Cues and Paths",
        .desc = {"Hold L or R clicked all the way and press the",
                 "D-pad. Up and down go round the cue sets, left",
                 "and right round the paths, with and without",
                 "frame dots. Best between attempts."},
    },
    {
        .kind = OPTKIND_INFO,
        .name = "Quick: Pad and Intensity",
        .desc = {"Hold Z and press the D-pad. Up and down go",
                 "round the controller looks and sizes, left and",
                 "right make the cues, paths and timers fainter",
                 "or bolder."},
    },
    {
        .kind = OPTKIND_INFO,
        .name = "Quick: Presets and Hide",
        .desc = {"Hold Z and L or R, and press the D-pad. Left",
                 "and right load the previous or next preset; up",
                 "or down hides everything Landing Lab draws, or",
                 "brings it back."},
    },
    {
        .kind = OPTKIND_INFO,
        .name = "Next Route",
        .desc = {"While hanging from a ledge, D-pad up shows the",
                 "next ledge route in the list, and the first",
                 "again after the last. Ledge Practice > Route",
                 "picks any route directly."},
    },
    {
        .kind = OPTKIND_INFO,
        .name = "Frame Advance",
        .desc = {"D-pad down freezes the game, or lets it run."},
    },
    {
        .kind = OPTKIND_INFO,
        .name = "Step a Frame",
        .desc = {"While frozen, the Advance button steps one",
                 "frame; hold it to step slowly. It's Z unless",
                 "you change it in Speed."},
    },
};

static EventMenu Menu_Controls = {
    .name = "Controls",
    .option_num = countof(Options_Controls),
    .options = Options_Controls,
};

// Things only needed to test the event or report a wrong prediction.
enum options_dev
{
    DOPT_EXACT,
    DOPT_COLL,
    DOPT_LOG,
    DOPT_SCRIPT,
    DOPT_CLEAR,
    DOPT_INFO,

    DOPT_COUNT
};

static EventOption Options_Dev[DOPT_COUNT] = {
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Show Accuracy",
        .desc = {"Add a line to the panel counting how often the",
                 "prediction matched the landing exactly."},
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
        .kind = OPTKIND_STRING,
        .name = "Debug Log",
        .value_num = 4,
        .values = log_level_names,
        .desc = {"Extra lines in Dolphin's log. Landings: timer",
                 "presses and ledge routes. Frames: every airborne",
                 "frame too. Everything: camera too, slows play."},
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
        .name = "About",
        .desc = {"Gray paths rely on ECB frames the event hasn't",
                 "seen yet; it learns them as you play. D-pad up",
                 "during Frame Advance hides everything the event",
                 "draws, for screenshots."},
    },
};

static EventMenu Menu_Dev = {
    .name = "Developer",
    .option_num = countof(Options_Dev),
    .options = Options_Dev,
};

// The main menu is a short list of groups, each its own page.

// A buzz that speeds up toward a window, per cue.
enum options_rumble
{
    RUOPT_AI,
    RUOPT_NIL,
    RUOPT_WL,
    RUOPT_NOTE,

    RUOPT_COUNT
};

static EventOption Options_Rumble[RUOPT_COUNT] = {
    {
        .kind = OPTKIND_TOGGLE,
        .name = "AI Rumble",
        .desc = {"Buzz the controller faster and faster up to an",
                 "AI window, stopping dead as it opens. Needs AI",
                 "Cues on."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "NIL Rumble",
        .desc = {"The same for NIL windows. Needs NIL Cues on."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Waveland Rumble",
        .desc = {"The same for perfect waveland and wavedash",
                 "windows. Needs Waveland Cues on."},
    },
    {
        .kind = OPTKIND_INFO,
        .name = "Game Rumble",
        .desc = {"While any of these is on, the game's own rumble",
                 "is off for your controller, so only the cues",
                 "buzz. Dolphin passes rumble only to a real",
                 "controller with rumble on in its settings."},
    },
};

static EventMenu Menu_Rumble = {
    .name = "Rumble",
    .option_num = countof(Options_Rumble),
    .options = Options_Rumble,
};

// Intensity by group: each one fainter or bolder than the main Intensity
// level, the controller on a level of its own, and Auto Fade.
enum options_intensity
{
    IOPT_CUES,
    IOPT_PATHS,
    IOPT_TIMERS,
    IOPT_LEDGE,
    IOPT_PAD,
    IOPT_FADE,
    IOPT_FADE_RESET,

    IOPT_COUNT
};
enum { FADE_OFF, FADE_TIMERS, FADE_CUES, FADE_BOTH };
static const char *group_offset_names[] = {"2 Fainter", "1 Fainter", "Same", "1 Bolder", "2 Bolder"};
static const char *fade_names[] = {"Off", "Timers", "Cues", "Both"};
void Event_FadeReset(GOBJ *menu);

static EventOption Options_Intensity[IOPT_COUNT] = {
    {
        .kind = OPTKIND_STRING,
        .name = "Cues",
        .val = 2,
        .value_num = countof(group_offset_names),
        .values = group_offset_names,
        .desc = {"Body Flash and Platform Glow, against the",
                 "Intensity level."},
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Paths",
        .val = 2,
        .value_num = countof(group_offset_names),
        .values = group_offset_names,
        .desc = {"The landing and body paths, frame dots, input",
                 "markers and slide-off line, against the",
                 "Intensity level."},
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Timers",
        .val = 2,
        .value_num = countof(group_offset_names),
        .values = group_offset_names,
        .desc = {"Every timer: the strip, near Falcon, the landing",
                 "spot, waveland and wavedash, against the",
                 "Intensity level."},
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Ledge",
        .val = 2,
        .value_num = countof(group_offset_names),
        .values = group_offset_names,
        .desc = {"Ledge routes, their inputs and their timers,",
                 "against the Intensity level."},
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Controller",
        .val = 2,
        .value_num = countof(intensity_names),
        .values = intensity_names,
        .desc = {"The controller display, on its own level: the",
                 "main Intensity doesn't change it."},
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Auto Fade",
        .value_num = countof(fade_names),
        .values = fade_names,
        .desc = {"Fade the timers or cues as you learn a timing:",
                 "each kind (AI, NIL, waveland) follows how many of",
                 "your last 10 or so tries hit. A miss brings it",
                 "back a little, a run of misses all the way."},
    },
    {
        .kind = OPTKIND_FUNC,
        .name = "Reset Fade",
        .desc = {"Forget the hit rates Auto Fade goes by, so",
                 "everything shows in full again."},
        .OnSelect = Event_FadeReset,
    },
};

static EventMenu Menu_Intensity = {
    .name = "Intensity by Group",
    .option_num = countof(Options_Intensity),
    .options = Options_Intensity,
};

// What the cues show, and how Falcon and the floor light up with them.
enum options_cues
{
    COPT_NIL,
    COPT_AI,
    COPT_WL,
    COPT_AI_FILTER,
    COPT_AI_AERIAL,
    COPT_FLASH,
    COPT_GLOW,
    COPT_INTENSITY,
    COPT_GROUPS,
    COPT_RUMBLE,

    COPT_COUNT
};

static EventOption Options_Cues[COPT_COUNT] = {
    {
        .kind = OPTKIND_TOGGLE,
        .name = "NIL Cues",
        .desc = {"Green: holding the stick lands you with no",
                 "landing lag (NIL)."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "AI Cues",
        .val = 1,
        .desc = {"Pink: pressing an aerial lands you (aerial",
                 "interrupt). The path marks where, with arrows",
                 "for the aerials that work there."},
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Waveland Cues",
        .val = 2,
        .value_num = countof(wl_cue_names),
        .values = wl_cue_names,
        .desc = {"Cyan: when an airdodge just below sideways lands",
                 "at once with full speed (perfect waveland), and",
                 "the wavedash out of a jump. Platforms leaves out",
                 "wavelands onto the main floor."},
    },
    {
        .kind = OPTKIND_STRING,
        .name = "AI Filter",
        .value_num = countof(ai_filter_names),
        .values = ai_filter_names,
        .desc = {"Useful shows only aerial interrupts that land",
                 "while Falcon is still rising and save 4 frames or",
                 "more without aerial lag. All shows them all."},
    },
    {
        .kind = OPTKIND_STRING,
        .name = "AI Aerial",
        .value_num = countof(ai_aerial_names),
        .values = ai_aerial_names,
        .desc = {"Count down to one aerial's window only. Any uses",
                 "the aerial that works on the most frames. With",
                 "the stick held in, A is a fair, not a nair, and",
                 "its window can be shorter."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Body Flash",
        .val = 1,
        .desc = {"Flash Falcon in the cue's color on the first",
                 "frame he can act after a hit, and tint him",
                 "periwinkle while he keeps ledge intangibility."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Platform Glow",
        .desc = {"Light up the floor a waveland or wavedash slides",
                 "along: it fills in toward the landing spot as the",
                 "window nears and flashes on each of its frames."},
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Intensity",
        .val = 2,
        .value_num = countof(intensity_names),
        .values = intensity_names,
        .desc = {"How strongly the cues, paths and timers show,",
                 "from faint to bold. Standard is the usual look.",
                 "Set each group apart from it in the menu below.",
                 "Quick toggle: hold Z, D-pad left or right."},
    },
    {
        .kind = OPTKIND_MENU,
        .name = "Intensity by Group",
        .menu = &Menu_Intensity,
        .desc = {"Cues, paths, timers, ledge info and the",
                 "controller each fainter or bolder, and Auto Fade."},
    },
    {
        .kind = OPTKIND_MENU,
        .name = "Rumble",
        .menu = &Menu_Rumble,
        .desc = {"Buzz the controller up to a cue's window, and",
                 "keep the game's own rumble out of it."},
    },
};

static EventMenu Menu_Cues = {
    .name = "Cues",
    .option_num = countof(Options_Cues),
    .options = Options_Cues,
};

// The lines drawn in the stage.
enum options_paths
{
    POPT_PATH,
    POPT_BODY,
    POPT_ROUTE,
    POPT_INPUTS,
    POPT_MARK_SIZE,
    POPT_TICKS,
    POPT_SLIDEOFF,
    POPT_PREVIEW,

    POPT_COUNT
};

static EventOption Options_Paths[POPT_COUNT] = {
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Landing Path",
        .desc = {"Draw where Falcon's ECB bottom goes if you keep",
                 "holding the stick and press nothing, in the",
                 "landing's color when its cue is on."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Body Path",
        .desc = {"Also draw a dotted line through Falcon's body,",
                 "which is easier to follow than the ECB bottom."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Ledge Route",
        .val = 1,
        .desc = {"While Falcon hangs on a ledge and on the way",
                 "back, draw the chosen route's path. Shown with",
                 "Show Routes or Assist on in Ledge Practice, and",
                 "for the jump chosen in Jump Timing."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Input Markers",
        .val = 1,
        .desc = {"Mark the inputs along the path where each is",
                 "due, and which aerials interrupt. White: stick.",
                 "Yellow: jump. Pink: aerial (C-stick directions",
                 "that work light up). Cyan: airdodge."},
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Marker Size",
        .val = 1,
        .value_num = countof(mark_size_names),
        .values = mark_size_names,
        .desc = {"How big the input markers and the aerial",
                 "picker are."},
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Frame Dots",
        .val = 1,
        .value_num = countof(tick_names),
        .values = tick_names,
        .desc = {"A small ring on the paths at every frame: closer",
                 "rings mean slower movement. Shown only during",
                 "Frame Advance (always on ledge routes), always,",
                 "or never."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Slide-off Line",
        .desc = {"When a waveland or wavedash would slide off the",
                 "edge, draw where Falcon goes: full stick that way",
                 "until it starts, then your real stick."},
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Jump Preview",
        .value_num = countof(preview_names),
        .val = PREVIEW_OFF,
        .values = preview_names,
        .desc = {"On the ground, show where a full hop and a short",
                 "hop would go if you jumped now, holding the stick",
                 "where it is."},
    },
};

static EventMenu Menu_Paths = {
    .name = "Paths",
    .option_num = countof(Options_Paths),
    .options = Options_Paths,
};

// The countdowns to each input, and how each one looks.
enum options_timers
{
    TOPT_NEAR,
    TOPT_STRIP,
    TOPT_WIDE,
    TOPT_SPOT,
    TOPT_WL,
    TOPT_WD,

    TOPT_COUNT
};

static EventOption Options_Timers[TOPT_COUNT] = {
    {
        .kind = OPTKIND_STRING,
        .name = "Near Falcon",
        .value_num = countof(near_names),
        .values = near_names,
        .desc = {"A countdown that rides with Falcon. Bubble: a",
                 "ring closes on where his body will be. Halo: a",
                 "bead runs into a notch. Pincers, ECB Fill and",
                 "Lights close, fill or count in on him."},
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Fixed Strip",
        .val = STRIP_CELLS,
        .value_num = countof(strip_names),
        .values = strip_names,
        .desc = {"A countdown in a corner. Cells slide into a gate,",
                 "Highway drops notes onto a line, Dial sweeps a",
                 "hand into the window. Press as it arrives."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Wide Cells",
        .val = 1,
        .desc = {"Make each frame cell of the cell timers a little",
                 "wider."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Landing Spot",
        .val = 1,
        .desc = {"Brackets close in on the landing spot of an AI",
                 "or NIL and meet it on the frame to press."},
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Waveland",
        .val = WLT_TICKS,
        .value_num = countof(wl_timer_names),
        .values = wl_timer_names,
        .desc = {"At the slide: ticks run in from its ends and",
                 "spike where they meet, the rails fill in, or",
                 "chevrons hop in a notch a frame. They meet on",
                 "the frame to airdodge."},
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Wavedash",
        .val = WDT_CELLS,
        .value_num = countof(wd_timer_names),
        .values = wd_timer_names,
        .desc = {"Out of the jumpsquat: a row in the strips, a pip",
                 "a frame under Falcon's feet, or a ring on the",
                 "floor that closes on the airdodge frame."},
    },
};

static EventMenu Menu_Timers = {
    .name = "Timers",
    .option_num = countof(Options_Timers),
    .options = Options_Timers,
};

// What's drawn on the screen rather than in the stage.
enum options_hud
{
    HOPT_STICK,
    HOPT_LOOK,
    HOPT_PAD_SIZE,
    HOPT_BUTTONS,
    HOPT_PAD_CUES,
    HOPT_SHIELD_DROP,
    HOPT_PANEL,
    HOPT_PANEL_SIDE,

    HOPT_COUNT
};

static EventOption Options_Hud[HOPT_COUNT] = {
    {
        .kind = OPTKIND_STRING,
        .name = "Controller",
        .value_num = countof(stick_names),
        .values = stick_names,
        .desc = {"Your real stick, the band where Melee reads an",
                 "axis as zero, a trail of the last frames, and the",
                 "fastfall line (lit when a flick would fastfall).",
                 "By Percent puts it beside your percent display."},
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Controller Look",
        .value_num = countof(pad_look_names),
        .values = pad_look_names,
        .desc = {"Ring: the whole controller in a small ellipse,",
                 "L and R as its halves, filling into their nubs.",
                 "Crest: a shield with L and R as its wings. Classic:",
                 "the wider block with trigger bars."},
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Controller Size",
        .value_num = countof(mark_size_names),
        .values = mark_size_names,
        .desc = {"How big the Ring look is. Small takes the same",
                 "room as TM-CE's own controller display."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Buttons and Triggers",
        .val = 1,
        .desc = {"Show the buttons, triggers and C-stick too, in",
                 "the controller's own colors."},
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Controller Cues",
        .value_num = countof(pad_cue_names),
        .values = pad_cue_names,
        .desc = {"The button an AI or waveland window wants, timed",
                 "on the controller: a ring closing in on it, or a",
                 "gauge going round it. Never filled, so it can't",
                 "pass for a press. Needs Buttons and Triggers."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Shield Drop Zone",
        .val = 1,
        .desc = {"Shade the stick angles that drop through a",
                 "platform out of shield, in the stick's gate.",
                 "Shown while Falcon is on a platform."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Info Panel",
        .val = 1,
        .desc = {"Two lines in a top corner: what's coming up,",
                 "and how your last attempt went."},
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Panel Side",
        .value_num = countof(panel_side_names),
        .values = panel_side_names,
        .desc = {"Where the info panel goes. Auto keeps it on the",
                 "side of the screen Falcon isn't on."},
    },
};

static EventMenu Menu_Hud = {
    .name = "HUD",
    .option_num = countof(Options_Hud),
    .options = Options_Hud,
};

// The chime, and which slips buzz. A checked timing that's off counts as a
// miss: it buzzes, and Reset starts over. Unchecked, the panel only notes
// it and the attempt goes on, judged by how it lands.
enum options_sounds
{
    SOPT_CHIME,
    SOPT_WINDOW,
    SOPT_SKIP,
    SOPT_ROUTE_CHIME,
    SOPT_LOST,
    SOPT_FF,
    SOPT_JUMP,
    SOPT_AERIAL,

    SOPT_COUNT
};

static EventOption Options_Sounds[SOPT_COUNT] = {
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Chime on a Hit",
        .val = 1,
        .desc = {"Chime when you land a NIL, an AI or a perfect",
                 "waveland, for the cues that are on."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Buzz: Missed Window",
        .val = 1,
        .desc = {"Buzz when you press for an AI or waveland",
                 "window and miss it, early or late."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Buzz: Skipped Window",
        .desc = {"Buzz when an AI or waveland window passes with",
                 "no press at all. Off, you can watch windows go",
                 "by to get a feel for the timing."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Chime: Ledge Route",
        .val = 1,
        .desc = {"Chime when a ledge route lands with GALINT left,",
                 "instead of the cue chime."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Buzz: Lost Ledge Route",
        .val = 1,
        .desc = {"Buzz when a ledge route ends without its NIL or",
                 "AI: landing lag, or missing the stage."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Buzz: Fastfall Timing",
        .desc = {"Buzz and start over when a ledge route's",
                 "fastfall is early or late, even if it still",
                 "lands. Off: only the panel notes it."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Buzz: Jump Timing",
        .desc = {"Buzz and start over when a ledge route's double",
                 "jump is early or late, even if it still lands.",
                 "Off: only the panel notes it."},
    },
    {
        .kind = OPTKIND_TOGGLE,
        .name = "Buzz: Aerial Timing",
        .desc = {"Buzz and start over when a ledge route's aerial",
                 "is outside its window, even if it still lands.",
                 "Off: only the panel notes it."},
    },
};

static EventMenu Menu_Sounds = {
    .name = "Sounds",
    .option_num = countof(Options_Sounds),
    .options = Options_Sounds,
};

// Slowing and stepping the game.
enum options_game
{
    GOPT_SPEED,
    GOPT_FRAME_ADV,
    GOPT_ADV_BUTTON,

    GOPT_COUNT
};

static EventOption Options_Game[GOPT_COUNT] = {
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
                 "the Advance Button, or hold it to step slowly.",
                 "D-pad down turns this on and off at any time."},
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Advance Button",
        .val = 1,
        .value_num = countof(adv_button_names),
        .values = adv_button_names,
        .desc = {"The button that steps a frame while Frame",
                 "Advance is on. The game doesn't see it."},
    },
};

static EventMenu Menu_Game = {
    .name = "Speed",
    .option_num = countof(Options_Game),
    .options = Options_Game,
};

// Settings presets: what every menu is set to, kept on the memory card in
// Landing Lab's own small file, so TM-CE's save is never touched.
enum preset_slot
{
    PS_USER, // the last settings: saved whenever the menu closes
    PS_1,
    PS_2,
    PS_3,
    PS_4,
    PS_SAVED, // the ones above are on the card, the ones from here built in
    PS_DEFAULTS = PS_SAVED,
    PS_MINIMAL,
    PS_EVERYTHING,
    PS_LEDGE,

    PS_COUNT
};
static char preset_label[PS_SAVED][20]; // Preset 1 to 4, or their names
static const char *preset_names[PS_COUNT] = {"User Custom", preset_label[1], preset_label[2], preset_label[3], preset_label[4],
                                             "Defaults", "Minimal", "Everything", "Ledge Drill"};
static char preset_desc[3][52];      // the chosen preset's settings, in short
static char preset_card_desc[3][52]; // what the memory card is doing

enum options_presets
{
    PROPT_PICK,
    PROPT_LOAD,
    PROPT_SAVE,
    PROPT_NAME,
    PROPT_START,
    PROPT_CARD,

    PROPT_COUNT
};

static EventOption Options_Presets[PROPT_COUNT] = {
    {
        .kind = OPTKIND_STRING,
        .name = "Preset",
        .value_num = PS_COUNT,
        .values = preset_names,
        .desc = {preset_desc[0], preset_desc[1], preset_desc[2], "Load it or save to it below."},
        .OnChange = Event_ChangePreset,
    },
    {
        .kind = OPTKIND_FUNC,
        .name = "Load Preset",
        .desc = {"Set every menu the way the preset above has",
                 "it."},
        .OnSelect = Event_PresetLoad,
    },
    {
        .kind = OPTKIND_FUNC,
        .name = "Save to Preset",
        .desc = {"Keep the current settings in the preset above:",
                 "User Custom or Preset 1 to 4. Defaults,",
                 "Minimal, Everything and Ledge Drill are built",
                 "in and stay as they are."},
        .OnSelect = Event_PresetSave,
    },
    {
        .kind = OPTKIND_FUNC,
        .name = "Name Preset",
        .desc = {"Give the preset above a name: Preset 1 to 4",
                 "only."},
        .OnSelect = Event_PresetName,
    },
    {
        .kind = OPTKIND_STRING,
        .name = "Load at Start",
        .value_num = PS_COUNT,
        .values = preset_names,
        .desc = {"The preset the event starts with. User Custom",
                 "is how you left the menus the last time you",
                 "closed them."},
        .OnChange = Event_ChangePresetStart,
    },
    {
        .kind = OPTKIND_INFO,
        .name = "Memory Card",
        .desc = {preset_card_desc[0], preset_card_desc[1], preset_card_desc[2]},
    },
};

static EventMenu Menu_Presets = {
    .name = "Presets",
    .option_num = countof(Options_Presets),
    .options = Options_Presets,
};

enum options_main
{
    OPT_PRESETS,
    OPT_CUES,
    OPT_TIMERS,
    OPT_PATHS,
    OPT_HUD,
    OPT_SOUNDS,
    OPT_LEDGE,
    OPT_JUMP,
    OPT_CAMERA,
    OPT_GAME,
    OPT_DEV,
    OPT_CONTROLS,
    OPT_HELP,
    OPT_EXIT,

    OPT_COUNT
};

static EventOption Options_Main[OPT_COUNT] = {
    {
        .kind = OPTKIND_MENU,
        .name = "Presets",
        .menu = &Menu_Presets,
        .desc = {"Load and save whole setups, and pick the one",
                 "the event starts with. Settings are kept on",
                 "the memory card in slot A."},
    },
    {
        .kind = OPTKIND_MENU,
        .name = "Cues",
        .menu = &Menu_Cues,
        .desc = {"Which landings get cues (NIL, AI, waveland), the",
                 "AI filter, Body Flash and Platform Glow."},
    },
    {
        .kind = OPTKIND_MENU,
        .name = "Timers",
        .menu = &Menu_Timers,
        .desc = {"The countdowns to each input: near Falcon, in a",
                 "corner, at the landing spot, and for wavelands",
                 "and wavedashes."},
    },
    {
        .kind = OPTKIND_MENU,
        .name = "Paths",
        .menu = &Menu_Paths,
        .desc = {"The landing, body and ledge route paths, input",
                 "markers, frame dots, the slide-off line and jump",
                 "previews."},
    },
    {
        .kind = OPTKIND_MENU,
        .name = "HUD",
        .menu = &Menu_Hud,
        .desc = {"The controller display and the info panel."},
    },
    {
        .kind = OPTKIND_MENU,
        .name = "Sounds",
        .menu = &Menu_Sounds,
        .desc = {"The chime on a hit, and which slips buzz."},
    },
    {
        .kind = OPTKIND_MENU,
        .name = "Ledge Practice",
        .menu = &Menu_Ledge,
        .desc = {"Routes from the ledge with the most GALINT,",
                 "quicktime Assist, and reset options like the",
                 "ledgedash training."},
    },
    {
        .kind = OPTKIND_MENU,
        .name = "Jump Timing",
        .menu = &Menu_Jump,
        .desc = {"In a fall, when to double jump and which",
                 "aerial to press to land as an AI or a NIL."},
    },
    {
        .kind = OPTKIND_MENU,
        .name = "Camera",
        .menu = &Menu_Camera,
        .desc = {"The Training Lab's camera modes."},
    },
    {
        .kind = OPTKIND_MENU,
        .name = "Speed",
        .menu = &Menu_Game,
        .desc = {"Game speed and frame advance."},
    },
    {
        .kind = OPTKIND_MENU,
        .name = "Developer",
        .menu = &Menu_Dev,
        .desc = {"Accuracy count, collision view, debug log and",
                 "test scripts."},
    },
    {
        .kind = OPTKIND_MENU,
        .name = "Controls",
        .menu = &Menu_Controls,
        .desc = {"Every button Landing Lab uses outside the menu."},
    },
    {
        .kind = OPTKIND_INFO,
        .name = "Help",
        .desc = {"Green NIL, pink AI, cyan waveland. Solid cells:",
                 "press. Hollow: touchdown. Periwinkle: GALINT.",
                 "White gate: press now. Slate: missed. Short hop",
                 "path dashed, body path dotted."},
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

///////////////////////
/// Presets         ///
///////////////////////

// A preset is a list of (option, value) pairs, each option known by a hash
// of its menu's and its own name, so a later version with options added,
// removed or moved still reads an older file: what it doesn't know stays as
// it is.
typedef struct PresetOpt
{
    u32 key;
    s16 val;
    u16 pad;
} PresetOpt;

#define PRESET_OPTS 72
typedef struct PresetSlot
{
    u16 used;
    u16 num;
    PresetOpt opt[PRESET_OPTS];
} PresetSlot;

#define PRESET_MAGIC 0x4C4C5052 // "LLPR"
#define PRESET_VERSION 3
#define NAME_LEN 12   // letters in a view's or preset's name
#define VIEW_POOL 48  // views kept, all stages together
typedef struct OldView // version 2's, one set for every stage
{
    u8 used;
    u8 pad[3];
    Vec3 eye, interest;
    float fov;
} OldView;
typedef struct CamView
{
    u8 used;
    u8 stage; // external stage id
    u8 slot;  // 1 to VIEW_SLOTS
    u8 pad;
    char name[NAME_LEN + 4];
    Vec3 eye, interest;
    float fov;
} CamView;
typedef struct PresetFile
{
    u32 magic;
    u16 version;
    u8 start; // the preset loaded at start
    u8 pad;
    PresetSlot slot[PS_SAVED];
    OldView old_view[4];                   // version 2's views: moved to the first stage it's read on
    char preset_name[PS_SAVED][NAME_LEN + 4]; // [0], User Custom, isn't named
    CamView view[VIEW_POOL];
    float skill[4]; // Auto Fade's hit rates, by cue kind
} PresetFile;
static float skill[4]; // the same, as they go (CUE_NUM used)
static CamView *View_Find(int v);
static CamView *View_New(int v);

// a name, cut to NAME_LEN letters
static void Name_Copy(char *to, const char *from)
{
    int i = 0;
    for (; i < NAME_LEN && from[i]; i++)
        to[i] = from[i];
    to[i] = 0;
}
static void Labels_Refresh(void);

typedef struct PresetMenu
{
    const char *tag;
    EventOption *opts;
    int num;
} PresetMenu;

// Every menu a player sets up: not Developer, and not Frame Advance or the
// chosen ledge route, which belong to the moment.
static const PresetMenu preset_menus[] = {
    {"Cues", Options_Cues, COPT_COUNT},
    {"Intensity", Options_Intensity, IOPT_COUNT},
    {"Rumble", Options_Rumble, RUOPT_COUNT},
    {"Timers", Options_Timers, TOPT_COUNT},
    {"Paths", Options_Paths, POPT_COUNT},
    {"HUD", Options_Hud, HOPT_COUNT},
    {"Sounds", Options_Sounds, SOPT_COUNT},
    {"Ledge", Options_Ledge, LOPT_COUNT},
    {"Jump", Options_Jump, JOPT_COUNT},
    {"Camera", Options_Camera, CAMOPT_COUNT},
    {"Speed", Options_Game, GOPT_COUNT},
};

static PresetFile *preset_file;    // what's on the card, and what gets written to it
static PresetSlot *preset_builtin; // [PS_COUNT - PS_SAVED]
static u8 preset_dirty;            // preset_file changed since it was last written
static u8 preset_was_paused;
static u8 preset_cam_pending; // a preset changed the camera mode
static u8 view_pending;       // ... or its view

static int Preset_Kept(EventOption *o)
{
    if (o == &Options_Ledge[LOPT_PICK] || o == &Options_Game[GOPT_FRAME_ADV])
        return 0;
    return o->kind == OPTKIND_STRING || o->kind == OPTKIND_INT || o->kind == OPTKIND_TOGGLE;
}

static u32 Preset_Hash(u32 h, const char *t)
{
    for (; *t; t++)
        h = (h ^ (u8)*t) * 16777619u;
    return h;
}

static u32 Preset_Key(const char *tag, EventOption *o)
{
    return Preset_Hash(Preset_Hash(2166136261u, tag) * 16777619u, o->name);
}

// The key of an option from any of the menus above, 0 if it's in none.
static u32 Preset_KeyOf(EventOption *o)
{
    for (int m = 0; m < (int)countof(preset_menus); m++)
        if (o >= preset_menus[m].opts && o < preset_menus[m].opts + preset_menus[m].num)
            return Preset_Key(preset_menus[m].tag, o);
    return 0;
}

static PresetSlot *Preset_Slot(int i)
{
    return i < PS_SAVED ? &preset_file->slot[i] : &preset_builtin[i - PS_SAVED];
}

static void Preset_Capture(PresetSlot *s)
{
    s->num = 0;
    for (int m = 0; m < (int)countof(preset_menus); m++)
        for (int i = 0; i < preset_menus[m].num; i++)
        {
            EventOption *o = &preset_menus[m].opts[i];
            if (!Preset_Kept(o) || s->num >= PRESET_OPTS)
                continue;
            PresetOpt *p = &s->opt[s->num++];
            p->key = Preset_Key(preset_menus[m].tag, o);
            p->val = o->val;
            p->pad = 0;
        }
    s->used = 1;
}

static PresetOpt *Preset_Find(PresetSlot *s, u32 key)
{
    for (int i = 0; i < s->num && i < PRESET_OPTS; i++)
        if (s->opt[i].key == key)
            return &s->opt[i];
    return 0;
}

// What the preset sets an option to: its own value when the preset doesn't
// have it.
static int Preset_Value(PresetSlot *s, EventOption *o)
{
    PresetOpt *p = Preset_Find(s, Preset_KeyOf(o));
    return p ? p->val : o->val;
}

static int Preset_Fits(EventOption *o, int v)
{
    if (o->kind == OPTKIND_TOGGLE)
        return v == 0 || v == 1;
    return v >= o->value_min && v - o->value_min < o->value_num;
}

static void Preset_Apply(PresetSlot *s)
{
    if (!s->used)
        return;
    int cam = Options_Camera[CAMOPT_MODE].val, view = Options_Camera[CAMOPT_VIEW].val, start = Options_Ledge[LOPT_START].val;
    for (int m = 0; m < (int)countof(preset_menus); m++)
        for (int i = 0; i < preset_menus[m].num; i++)
        {
            EventOption *o = &preset_menus[m].opts[i];
            if (!Preset_Kept(o))
                continue;
            PresetOpt *p = Preset_Find(s, Preset_Key(preset_menus[m].tag, o));
            if (p && Preset_Fits(o, p->val))
                o->val = p->val;
        }
    // the few settings that act when they change; the rest are read as they're
    // used. The camera waits for the next frame: this can run at start, before
    // the match's camera is ready.
    if (Options_Camera[CAMOPT_MODE].val != cam)
        preset_cam_pending = 1;
    if (Options_Camera[CAMOPT_VIEW].val != view && Options_Camera[CAMOPT_VIEW].val)
        view_pending = 1;
    if (Options_Ledge[LOPT_START].val != start)
        Event_ChangeLedgeStart(0, Options_Ledge[LOPT_START].val);
    Event_ChangeRoutes(0, 0);
}

static int Preset_Same(PresetSlot *a, PresetSlot *b)
{
    if (a->used != b->used || a->num != b->num)
        return 0;
    for (int i = 0; i < a->num && i < PRESET_OPTS; i++)
        if (a->opt[i].key != b->opt[i].key || a->opt[i].val != b->opt[i].val)
            return 0;
    return 1;
}

// The built-in presets, as changes to the defaults.
typedef struct PresetSet
{
    EventOption *o;
    s16 val;
} PresetSet;

// The AI cue and the corner strip, nothing else on screen
static const PresetSet preset_minimal[] = {
    {&Options_Cues[COPT_NIL], 0},
    {&Options_Cues[COPT_AI], 1},
    {&Options_Cues[COPT_WL], 0},
    {&Options_Cues[COPT_GLOW], 0},
    {&Options_Timers[TOPT_NEAR], NEAR_OFF},
    {&Options_Timers[TOPT_STRIP], STRIP_CELLS},
    {&Options_Timers[TOPT_SPOT], 0},
    {&Options_Timers[TOPT_WL], WLT_OFF},
    {&Options_Timers[TOPT_WD], WDT_OFF},
    {&Options_Paths[POPT_PATH], 0},
    {&Options_Paths[POPT_BODY], 0},
    {&Options_Paths[POPT_TICKS], 2},
    {&Options_Paths[POPT_SLIDEOFF], 0},
    {&Options_Paths[POPT_PREVIEW], PREVIEW_OFF},
    {&Options_Hud[HOPT_STICK], 3},
    {&Options_Hud[HOPT_PANEL], 0},
};

// Every cue, path, timer and sound
static const PresetSet preset_everything[] = {
    {&Options_Cues[COPT_NIL], 1},
    {&Options_Cues[COPT_AI], 1},
    {&Options_Cues[COPT_WL], 2},
    {&Options_Cues[COPT_FLASH], 1},
    {&Options_Cues[COPT_GLOW], 1},
    {&Options_Timers[TOPT_NEAR], NEAR_BUBBLE},
    {&Options_Timers[TOPT_STRIP], STRIP_CELLS},
    {&Options_Timers[TOPT_SPOT], 1},
    {&Options_Timers[TOPT_WL], WLT_TICKS},
    {&Options_Timers[TOPT_WD], WDT_CELLS},
    {&Options_Paths[POPT_PATH], 1},
    {&Options_Paths[POPT_BODY], 1},
    {&Options_Paths[POPT_ROUTE], 1},
    {&Options_Paths[POPT_INPUTS], 1},
    {&Options_Paths[POPT_TICKS], 1},
    {&Options_Paths[POPT_SLIDEOFF], 1},
    {&Options_Paths[POPT_PREVIEW], 0},
    {&Options_Hud[HOPT_STICK], 0},
    {&Options_Hud[HOPT_PANEL], 1},
    {&Options_Sounds[SOPT_SKIP], 1},
    {&Options_Sounds[SOPT_FF], 1},
    {&Options_Sounds[SOPT_JUMP], 1},
    {&Options_Sounds[SOPT_AERIAL], 1},
};

// Ledge routes from the ledge, both kinds, reset to the same ledge each time
static const PresetSet preset_ledge[] = {
    {&Options_Ledge[LOPT_ROUTES], 1},
    {&Options_Ledge[LOPT_KIND], ROUTES_BOTH},
    {&Options_Ledge[LOPT_START], 0},
    {&Options_Ledge[LOPT_RESET], 1},
    {&Options_Paths[POPT_ROUTE], 1},
    {&Options_Paths[POPT_INPUTS], 1},
    {&Options_Cues[COPT_NIL], 1},
    {&Options_Cues[COPT_AI], 1},
};

static void Preset_Build(PresetSlot *s, PresetSlot *base, const PresetSet *set, int n)
{
    memcpy(s, base, sizeof(*s));
    for (int i = 0; i < n; i++)
    {
        PresetOpt *p = Preset_Find(s, Preset_KeyOf(set[i].o));
        if (p)
            p->val = set[i].val;
    }
}

// The chosen preset's settings in three short lines, for the menu.
static void Preset_Describe(int which)
{
    PresetSlot *s = Preset_Slot(which);
    if (!s->used)
    {
        sprintf(preset_desc[0], "Empty: Save to Preset keeps the current");
        sprintf(preset_desc[1], "settings here.");
        preset_desc[2][0] = 0;
        return;
    }
    char *t = preset_desc[0];
    int nil = Preset_Value(s, &Options_Cues[COPT_NIL]), ai = Preset_Value(s, &Options_Cues[COPT_AI]);
    int wl = Preset_Value(s, &Options_Cues[COPT_WL]);
    int n = 0;
    t += sprintf(t, "Cues:");
    if (ai)
        t += sprintf(t, "%s AI", n++ ? "," : "");
    if (nil)
        t += sprintf(t, "%s NIL", n++ ? "," : "");
    if (wl)
        t += sprintf(t, "%s waveland", n++ ? "," : "");
    if (!n)
        sprintf(t, " none");

    int near = Preset_Value(s, &Options_Timers[TOPT_NEAR]), strip = Preset_Value(s, &Options_Timers[TOPT_STRIP]);
    t = preset_desc[1];
    t += sprintf(t, "Timers: %s strip", strip ? strip_names[strip] : "no");
    if (near != NEAR_OFF)
        sprintf(t, ", %s by Falcon", near_names[near]);

    int path = Preset_Value(s, &Options_Paths[POPT_PATH]), body = Preset_Value(s, &Options_Paths[POPT_BODY]);
    int route = Preset_Value(s, &Options_Paths[POPT_ROUTE]), pad = Preset_Value(s, &Options_Hud[HOPT_STICK]);
    t = preset_desc[2];
    n = 0;
    t += sprintf(t, "Paths:");
    if (path)
        t += sprintf(t, "%s landing", n++ ? "," : "");
    if (body)
        t += sprintf(t, "%s body", n++ ? "," : "");
    if (route)
        t += sprintf(t, "%s ledge", n++ ? "," : "");
    if (!n)
        t += sprintf(t, " none");
    sprintf(t, ". Pad %s", pad == 3 ? "off" : "on");
}

static void Card_Update(void);
static void Card_Load(void);

void Event_ChangePreset(GOBJ *menu, int value)
{
    Preset_Describe(value);
}

void Event_ChangePresetStart(GOBJ *menu, int value)
{
    preset_file->start = value;
    preset_dirty = 1;
}

void Event_PresetLoad(GOBJ *menu)
{
    int which = Options_Presets[PROPT_PICK].val;
    PresetSlot *s = Preset_Slot(which);
    if (!s->used)
    {
        SFX_PlayCommon(3);
        return;
    }
    Preset_Apply(s);
    OSReport("LLPRESET load %s\n", preset_names[which]);
    SFX_PlayCommon(1);
}

void Event_PresetSave(GOBJ *menu)
{
    int which = Options_Presets[PROPT_PICK].val;
    if (which >= PS_SAVED)
    {
        SFX_PlayCommon(3); // built in
        OSReport("LLPRESET save refused: %s is built in\n", preset_names[which]);
        return;
    }
    Preset_Capture(&preset_file->slot[which]);
    preset_dirty = 1;
    OSReport("LLPRESET save %s, %d settings\n", preset_names[which], preset_file->slot[which].num);
    Preset_Describe(which);
    SFX_PlayCommon(1);
}

// At start: the defaults and the built-in presets are made from the menus as
// the event sets them, then the card is read.
static void Presets_Init(void)
{
    preset_file = calloc(sizeof(PresetFile));
    preset_builtin = calloc(sizeof(PresetSlot) * (PS_COUNT - PS_SAVED));
    preset_file->magic = PRESET_MAGIC;
    preset_file->version = PRESET_VERSION;
    preset_file->start = PS_USER;
    PresetSlot *def = &preset_builtin[PS_DEFAULTS - PS_SAVED];
    Preset_Capture(def);
    Preset_Build(&preset_builtin[PS_MINIMAL - PS_SAVED], def, preset_minimal, countof(preset_minimal));
    Preset_Build(&preset_builtin[PS_EVERYTHING - PS_SAVED], def, preset_everything, countof(preset_everything));
    Preset_Build(&preset_builtin[PS_LEDGE - PS_SAVED], def, preset_ledge, countof(preset_ledge));
    Options_Presets[PROPT_START].val = PS_USER;
    Labels_Refresh();
    Preset_Describe(Options_Presets[PROPT_PICK].val);
    Card_Load();
}

// The card's file was read (or there is none): start with the preset it
// asks for.
static void Presets_Loaded(int ok)
{
    if (!ok || preset_file->magic != PRESET_MAGIC || preset_file->version != PRESET_VERSION)
    {
        memset(preset_file, 0, sizeof(*preset_file));
        preset_file->magic = PRESET_MAGIC;
        preset_file->version = PRESET_VERSION;
        preset_file->start = PS_USER;
    }
    // version 2's views, from before views were per stage, go to this one
    for (int i = 0; i < 4; i++)
    {
        OldView *o = &preset_file->old_view[i];
        CamView *w = o->used ? View_New(i + 1) : 0;
        if (w)
        {
            w->eye = o->eye;
            w->interest = o->interest;
            w->fov = o->fov;
            w->used = 1;
            preset_dirty = 1;
        }
        memset(o, 0, sizeof(*o));
    }
    for (int i = 0; i < PS_SAVED; i++)
        preset_file->preset_name[i][NAME_LEN] = 0;
    for (int i = 0; i < VIEW_POOL; i++)
        preset_file->view[i].name[NAME_LEN] = 0;
    for (int i = 0; i < 4; i++)
    {
        float k = preset_file->skill[i];
        skill[i] = k >= 0.f && k <= 1.f ? k : 0.f; // (a NaN fails both)
    }
    Labels_Refresh();
    int start = preset_file->start < PS_COUNT ? preset_file->start : PS_USER;
    Options_Presets[PROPT_START].val = start;
    Preset_Apply(Preset_Slot(start));
    OSReport("LLPRESET start %s (%s), card file %s\n", preset_names[start], Preset_Slot(start)->used ? "applied" : "empty", ok ? "read" : "not read");
    Preset_Describe(Options_Presets[PROPT_PICK].val);
}

///////////////////////
/// Memory card     ///
///////////////////////

// Landing Lab's own file on the memory card in slot A: one block with a
// comment for the card screen and the presets. It's read once at start (the
// event waits for it, as the lab does), written in the background one step
// a frame whenever a preset changes, and once more, waiting, on Exit.
#define CARD_SLOT 0
#define CARD_FILE "TMCE_LandingLab"
#define CARD_SIZE 8192 // one block
#define CARD_TIMEOUT_US 3000000

typedef struct CardImage
{
    char comment[CARD_COMMENT_SIZE]; // two lines of 32, at the file's start
    u32 sum;
    u32 size;
    PresetFile file;
} CardImage;
typedef char card_image_fits[sizeof(CardImage) <= 8192 ? 1 : -1]; // one block
#define CARD_READ_LEN ((sizeof(CardImage) + CARD_READ_SIZE - 1) & ~(CARD_READ_SIZE - 1))

enum card_step
{
    CARD_IDLE,
    CARD_MOUNT,
    CARD_CHECK,
    CARD_CREATE,
    CARD_STATUS,
    CARD_WRITE,
};

static u8 *card_buf; // CARD_SIZE, 32-byte aligned for the card's DMA
// frames left logging a probe line after a write, in test runs: in Dolphin,
// OSReport lines (EXI UART, which shares EXI channel 0 with slot A) have gone
// missing for a while after one
#define CARD_PROBE 180
static int card_probe;
static CARDFileInfo card_fi;
static CARDStat card_stat;
static volatile s32 card_result;
static volatile int card_done;
static int card_step;
static int card_tick;    // when the step under way started
static u8 card_mounted;
static u32 card_gen;     // goes up with every change to preset_file
static u32 card_gen_saved, card_gen_writing, card_gen_failed;

static void Card_Callback(s32 chan, s32 result)
{
    card_result = result;
    card_done = 1;
}

static void Card_Say(const char *a, const char *b, const char *c)
{
    strcpy(preset_card_desc[0], a);
    strcpy(preset_card_desc[1], b);
    strcpy(preset_card_desc[2], c);
    OSReport("LLCARD %s %s %s\n", a, b, c);
}

static u32 Card_Sum(const void *data, int n)
{
    const u8 *b = data;
    u32 h = 2166136261u;
    for (int i = 0; i < n; i++)
        h = (h ^ b[i]) * 16777619u;
    return h;
}

// Waits for the step under way: its result, or BUSY if the card never answered.
static s32 Card_Wait(void)
{
    int t0 = OSGetTick();
    while (!card_done)
        if (OSTicksToMicroseconds(OSGetTick() - t0) > CARD_TIMEOUT_US)
            return CARD_RESULT_BUSY;
    card_done = 0;
    return card_result;
}

static void Card_Error(s32 r)
{
    char line[52];
    sprintf(line, "or written (error %d), so settings last", (int)r);
    Card_Say("The memory card in slot A couldn't be read", line, "until you leave the event.");
}

// At start, waiting: the file's presets into preset_file, if it's there.
static void Card_Load(void)
{
    if (!card_buf)
    {
        void *raw = calloc(CARD_SIZE + 32);
        card_buf = (u8 *)(((u32)raw + 31) & ~31);
        Memcard_InitWorkArea();
    }
    int ok = 0;
    s32 mem, sec;
    s32 r = CARDProbeEx(CARD_SLOT, &mem, &sec);
    if (r != CARD_RESULT_READY)
    {
        Card_Say("No memory card in slot A: settings last until", "you leave the event.", "");
        Presets_Loaded(0);
        return;
    }
    card_done = 0;
    r = CARDMountAsync(CARD_SLOT, stc_memcard_work->work_area, 0, Card_Callback);
    if (r >= 0)
        r = Card_Wait();
    if (r == CARD_RESULT_READY || r == CARD_RESULT_BROKEN)
    {
        card_done = 0;
        r = CARDCheckAsync(CARD_SLOT, Card_Callback);
        if (r >= 0)
            r = Card_Wait();
        if (r == CARD_RESULT_READY)
        {
            r = CARDOpen(CARD_SLOT, CARD_FILE, &card_fi);
            if (r == CARD_RESULT_READY)
            {
                DCInvalidateRange(card_buf, CARD_READ_LEN);
                r = CARDRead(&card_fi, card_buf, CARD_READ_LEN, 0);
                CARDClose(&card_fi);
                CardImage *img = (CardImage *)card_buf;
                // older files are this one cut short: what they have
                // is kept
                u32 size = img->size;
                if (r == CARD_RESULT_READY && size >= __builtin_offsetof(PresetFile, old_view) && size <= sizeof(PresetFile) &&
                    img->sum == Card_Sum(&img->file, size))
                {
                    memset(preset_file, 0, sizeof(PresetFile));
                    memcpy(preset_file, &img->file, size);
                    if (preset_file->version < PRESET_VERSION)
                        preset_file->version = PRESET_VERSION;
                    ok = 1;
                    Card_Say("Kept in Landing Lab's own file on the", "memory card in slot A (1 block). Changes", "save when the menu closes.");
                }
                else
                    Card_Say("Landing Lab's file on the card in slot A", "couldn't be read, so it starts over. Changes", "save when the menu closes.");
            }
            else if (r == CARD_RESULT_NOFILE)
                Card_Say("Nothing saved on the card in slot A yet.", "Settings save to a file of 1 block there", "when the menu closes.");
            else
                Card_Error(r);
        }
        else
            Card_Error(r);
        CARDUnmount(CARD_SLOT);
    }
    else
        Card_Error(r);
    card_gen = card_gen_saved = card_gen_failed = 0;
    Presets_Loaded(ok);
}

static void Card_Stop(void)
{
    if (card_mounted)
        CARDUnmount(CARD_SLOT);
    card_mounted = 0;
    card_step = CARD_IDLE;
}

static void Card_Fail(s32 r)
{
    Card_Stop();
    card_gen_failed = card_gen_writing;
    if (r == CARD_RESULT_NOCARD || r == CARD_RESULT_WRONGDEVICE)
        Card_Say("No memory card in slot A: settings last until", "you leave the event.", "");
    else if (r == CARD_RESULT_INSSPACE || r == CARD_RESULT_NOENT)
        Card_Say("The card in slot A is full: Landing Lab", "needs 1 free block and 1 free file, so", "settings last until you leave the event.");
    else
        Card_Error(r);
}

static void Card_Write(void)
{
    card_done = 0;
    s32 r = CARDWriteAsync(&card_fi, card_buf, CARD_SIZE, 0, Card_Callback);
    if (r < 0)
    {
        CARDClose(&card_fi);
        Card_Fail(r);
        return;
    }
    card_step = CARD_WRITE;
}

// Starts writing preset_file as it is now.
static void Card_Begin(void)
{
    CardImage *img = (CardImage *)card_buf;
    memset(card_buf, 0, CARD_SIZE);
    strcpy(img->comment, "TM-CE Landing Lab");
    strcpy(img->comment + 32, "Settings presets");
    img->size = sizeof(PresetFile);
    memcpy(&img->file, preset_file, sizeof(PresetFile));
    img->sum = Card_Sum(&img->file, sizeof(PresetFile));
    DCFlushRange(card_buf, CARD_SIZE);
    card_gen_writing = card_gen;
    card_tick = OSGetTick();

    s32 mem, sec;
    s32 r = CARDProbeEx(CARD_SLOT, &mem, &sec);
    if (r == CARD_RESULT_READY)
    {
        card_done = 0;
        r = CARDMountAsync(CARD_SLOT, stc_memcard_work->work_area, 0, Card_Callback);
    }
    if (r < 0)
    {
        Card_Fail(r);
        return;
    }
    card_step = CARD_MOUNT;
}

// The next step, once the one under way is done.
static void Card_Step(void)
{
    if (card_step == CARD_IDLE)
        return;
    if (!card_done)
    {
        if (OSTicksToMicroseconds(OSGetTick() - card_tick) > CARD_TIMEOUT_US)
        {
            if (card_step == CARD_WRITE || card_step == CARD_STATUS)
                CARDClose(&card_fi);
            Card_Fail(CARD_RESULT_BUSY);
        }
        return;
    }
    s32 r = card_result;
    card_done = 0;
    card_tick = OSGetTick();
    switch (card_step)
    {
    case CARD_MOUNT:
        if (r != CARD_RESULT_READY && r != CARD_RESULT_BROKEN)
        {
            Card_Fail(r);
            return;
        }
        card_mounted = 1;
        r = CARDCheckAsync(CARD_SLOT, Card_Callback);
        if (r < 0)
            Card_Fail(r);
        else
            card_step = CARD_CHECK;
        return;
    case CARD_CHECK:
        if (r != CARD_RESULT_READY)
        {
            Card_Fail(r);
            return;
        }
        r = CARDOpen(CARD_SLOT, CARD_FILE, &card_fi);
        if (r == CARD_RESULT_READY)
            Card_Write();
        else if (r == CARD_RESULT_NOFILE)
        {
            r = CARDCreateAsync(CARD_SLOT, CARD_FILE, CARD_SIZE, &card_fi, Card_Callback);
            if (r < 0)
                Card_Fail(r);
            else
                card_step = CARD_CREATE;
        }
        else
            Card_Fail(r);
        return;
    case CARD_CREATE:
        if (r != CARD_RESULT_READY)
        {
            Card_Fail(r);
            return;
        }
        // the two comment lines at the file's start show on the card screen
        if (CARDGetStatus(CARD_SLOT, card_fi.fileNo, &card_stat) == CARD_RESULT_READY)
        {
            card_stat.commentAddr = 0;
            card_stat.iconAddr = 0xFFFFFFFF;
            card_stat.bannerFormat = 0;
            card_stat.iconFormat = 0;
            card_stat.iconSpeed = 0;
            if (CARDSetStatusAsync(CARD_SLOT, card_fi.fileNo, &card_stat, Card_Callback) >= 0)
            {
                card_step = CARD_STATUS;
                return;
            }
        }
        Card_Write();
        return;
    case CARD_STATUS:
        Card_Write(); // without the comment if it didn't take
        return;
    case CARD_WRITE:
        CARDClose(&card_fi);
        if (r != CARD_RESULT_READY)
        {
            Card_Fail(r);
            return;
        }
        Card_Stop();
        card_gen_saved = card_gen_writing;
        card_probe = CARD_PROBE;
        Card_Say("Saved in Landing Lab's own file on the", "memory card in slot A (1 block). Changes", "save when the menu closes.");
        return;
    }
}

static int Card_Wanted(void)
{
    return card_gen != card_gen_saved && card_gen != card_gen_failed;
}

// Each frame: start a write when the presets changed, and move the one
// under way along.
static void Card_Update(void)
{
    if (preset_dirty)
    {
        preset_dirty = 0;
        card_gen++;
    }
    if (card_step == CARD_IDLE && Card_Wanted())
        Card_Begin();
    Card_Step();
}

// Leaving the event: finish what's under way and write what's left, waiting.
static void Card_Flush(void)
{
    for (int pass = 0; pass < 2; pass++)
    {
        if (card_step == CARD_IDLE)
        {
            Card_Update();
            if (card_step == CARD_IDLE)
                break;
        }
        while (card_step != CARD_IDLE)
            Card_Step();
    }
}

// The settings as they are now into User Custom, if they changed.
static void Presets_KeepUser(void)
{
    PresetSlot now;
    Preset_Capture(&now);
    if (!Preset_Same(&now, &preset_file->slot[PS_USER]))
    {
        memcpy(&preset_file->slot[PS_USER], &now, sizeof(now));
        preset_dirty = 1;
    }
}

// Camera views: where the camera's eye is, what it looks at, and its field
// of view. A view is held in Advanced mode, the game's develop camera, which
// puts the camera each frame where its own state says (decomp
// CameraDebugMode at 0x80453004); a view is written there.
typedef struct DevCam
{
    int last_mode, ply_slot;
    Vec3 follow_int_offset, follow_eye_offset, follow_eye_pos, follow_int_pos;
    float follow_fov;
    Vec3 free_int_pos, free_eye_pos;
    float free_fov;
} DevCam;
#define dev_cam ((DevCam *)0x80453004)

// A view saved on this stage in slot v (1 to VIEW_SLOTS), or 0.
static CamView *View_Find(int v)
{
    int stage = Stage_GetExternalID();
    for (int i = 0; i < VIEW_POOL; i++)
    {
        CamView *w = &preset_file->view[i];
        if (w->used && w->stage == stage && w->slot == v)
            return w;
    }
    return 0;
}

// Slot v on this stage to save into: the one there, or a free one (0 when
// the file has no room left).
static CamView *View_New(int v)
{
    CamView *w = View_Find(v);
    if (w)
        return w;
    for (int i = 0; i < VIEW_POOL; i++)
    {
        w = &preset_file->view[i];
        if (!w->used)
        {
            memset(w, 0, sizeof(*w));
            w->stage = Stage_GetExternalID();
            w->slot = v;
            return w;
        }
    }
    return 0;
}

// The menus' names for the views on this stage and the presets.
static void Labels_Refresh(void)
{
    for (int i = 1; i < PS_SAVED; i++)
    {
        if (preset_file->preset_name[i][0])
            strcpy(preset_label[i], preset_file->preset_name[i]);
        else
            sprintf(preset_label[i], "Preset %d", i);
    }
    for (int v = 1; v <= VIEW_SLOTS; v++)
    {
        CamView *w = View_Find(v);
        if (w && w->name[0])
            strcpy(view_label[v - 1], w->name);
        else
            sprintf(view_label[v - 1], w ? "View %d" : "View %d (empty)", v);
    }
}

// The match camera's COBJ: Match_GetCObj gives its GOBJ, despite the name.
// It's the one the stage is drawn with in every camera mode; MexTK's
// stc_matchcam_cobj is a copy the game only keeps up in the normal and fixed
// modes, so it goes stale in Advanced (the develop camera).
static COBJ *View_CObj(void)
{
    GOBJ *g = (GOBJ *)Match_GetCObj();
    return g ? g->hsd_object : *stc_matchcam_cobj;
}

static void View_Apply(int v)
{
    CamView *w = View_Find(v);
    if (!w)
        return;
    Options_Camera[CAMOPT_MODE].val = CAM_ADVANCED;
    Event_ChangeCamera(0, CAM_ADVANCED);
    dev_cam->free_eye_pos = w->eye;
    dev_cam->free_int_pos = w->interest;
    dev_cam->free_fov = w->fov;
}

void Event_ChangeView(GOBJ *menu, int value)
{
    if (value > 0 && !View_Find(value))
    {
        SFX_PlayCommon(3); // nothing saved there yet
        return;
    }
    View_Apply(value);
}

void Event_SaveView(GOBJ *menu)
{
    int v = Options_Camera[CAMOPT_VIEW].val;
    CamView *w = v > 0 ? View_New(v) : 0;
    COBJ *cobj = View_CObj();
    if (!w || !cobj)
    {
        SFX_PlayCommon(3); // no view picked, or every view in the file is used
        return;
    }
    COBJ_GetEyePosition(cobj, &w->eye);
    COBJ_GetInterest(cobj, &w->interest);
    w->fov = cobj->projection_param.perspective.fov;
    w->used = 1;
    preset_dirty = 1;
    Labels_Refresh();
    OSReport("LLVIEW saved %d on stage %d: eye %.1f %.1f %.1f at %.1f %.1f %.1f fov %.1f\n", v, w->stage, w->eye.X, w->eye.Y,
             w->eye.Z, w->interest.X, w->interest.Y, w->interest.Z, w->fov);
    View_Apply(v);
    SFX_PlayCommon(1);
}

///////////////////////
/// Naming          ///
///////////////////////

// Views and presets are named on a letter grid drawn over the paused game,
// in place of the menu: the stick or D-pad picks a key, A types it, B
// deletes (or leaves, with nothing left to delete), Y types a space, X
// switches case and Start keeps the name.
enum { NAME_VIEW, NAME_PRESET };
#define NAMER_COLS 10
#define NAMER_KEYS 4 // the wide keys on the last row
#define NAMER_ROWS 5
#define NAMER_SUBTEXTS (4 * NAMER_COLS + NAMER_KEYS)
static const char *namer_rows[2][4] = {
    {"ABCDEFGHIJ", "KLMNOPQRST", "UVWXYZ-.'!", "1234567890"},
    {"abcdefghij", "klmnopqrst", "uvwxyz-.'!", "1234567890"},
};
static struct
{
    u8 on, what, slot, lower, dirty, leave_menu;
    int cx, cy, len;
    char buf[NAME_LEN + 1];
    char title[48];
    Text *text;
} namer;
static int namer_port; // the controller that opened the menu

static const char *Stage_Name(void)
{
    switch (Stage_GetExternalID())
    {
    case GRKINDEXT_BATTLE:
        return "Battlefield";
    case GRKINDEXT_FD:
        return "Final Destination";
    case GRKINDEXT_OLDPU:
        return "Dream Land";
    case GRKINDEXT_STORY:
        return "Yoshi's Story";
    case GRKINDEXT_IZUMI:
        return "Fountain of Dreams";
    case GRKINDEXT_PSTAD:
        return "Pokemon Stadium";
    }
    return "this stage";
}

// The grid is drawn on the HUD, which the game doesn't draw while paused:
// the pause menu closes for it, and the game stays frozen the way Frame
// Advance freezes it (Advance_CheckPause) until it's done, when the menu
// opens again.
static void Namer_Open(int what, int slot, const char *name)
{
    MenuData *md = event_vars->menu_gobj->userdata;
    namer_port = md->controller_index;
    HUDCamData *hud = event_vars->hudcam_gobj->userdata;
    memset(&namer, 0, sizeof(namer));
    namer.what = what;
    namer.slot = slot;
    Name_Copy(namer.buf, name);
    namer.len = strlen(namer.buf);
    namer.lower = namer.len > 0;
    if (what == NAME_VIEW)
        sprintf(namer.title, "Name view %d on %s", slot, Stage_Name());
    else
        sprintf(namer.title, "Name preset %d", slot);
    Text *t = Text_CreateText(2, hud->canvas);
    t->kerning = 1;
    t->align = 1;
    t->use_aspect = 0;
    t->is_depth_compare = 0;
    t->viewport_scale.X = 0.1f;
    t->viewport_scale.Y = 0.1f;
    for (int i = 0; i < NAMER_SUBTEXTS; i++)
        Text_AddSubtext(t, 0, 0, "");
    namer.text = t;
    namer.dirty = 1;
    namer.on = 1;
    namer.leave_menu = 1; // next frame: not from inside the menu's own call
    SFX_PlayCommon(1);
}

// reopen: back to the menu (Start opens it by itself)
static void Namer_Close(int keep, int reopen)
{
    MenuData *md = event_vars->menu_gobj->userdata;
    if (keep)
    {
        while (namer.len > 0 && namer.buf[namer.len - 1] == ' ')
            namer.buf[--namer.len] = 0;
        char *to = 0;
        if (namer.what == NAME_VIEW)
        {
            CamView *w = View_Find(namer.slot);
            to = w ? w->name : 0;
        }
        else
            to = preset_file->preset_name[namer.slot];
        if (to)
        {
            memset(to, 0, NAME_LEN + 4);
            strcpy(to, namer.buf);
            preset_dirty = 1;
            Labels_Refresh();
            OSReport("LLNAME %s %d \"%s\"\n", namer.what == NAME_VIEW ? "view" : "preset", namer.slot, namer.buf);
        }
    }
    if (namer.text)
        Text_Destroy(namer.text);
    namer.text = 0;
    namer.on = 0;
    if (reopen && Pause_CheckStatus(1) != 2)
    {
        event_vars->Menu_Enter(event_vars->menu_gobj);
        md->controller_index = namer_port;
    }
    SFX_PlayCommon(keep ? 1 : 0);
}

static void Namer_Type(char c)
{
    if (namer.len >= NAME_LEN)
    {
        SFX_PlayCommon(3);
        return;
    }
    namer.buf[namer.len++] = c;
    namer.buf[namer.len] = 0;
    // a capital to start with, then small letters, as names are written
    if (namer.len == 1 && c >= 'A' && c <= 'Z')
        namer.lower = 1;
    SFX_PlayCommon(1);
}

// Each frame while it's up, from Event_Update.
static void Namer_Think(void)
{
    HSD_Pad *pad = PadGetMaster(namer_port);
    int rep = pad->repeat, down = pad->down;
    int cols = namer.cy == NAMER_ROWS - 1 ? NAMER_KEYS : NAMER_COLS;
    int prev_cx = namer.cx, prev_cy = namer.cy;
    if (rep & (HSD_BUTTON_LEFT | HSD_BUTTON_DPAD_LEFT))
        namer.cx = (namer.cx + cols - 1) % cols;
    else if (rep & (HSD_BUTTON_RIGHT | HSD_BUTTON_DPAD_RIGHT))
        namer.cx = (namer.cx + 1) % cols;
    else if (rep & (HSD_BUTTON_UP | HSD_BUTTON_DPAD_UP | HSD_BUTTON_DOWN | HSD_BUTTON_DPAD_DOWN))
    {
        int dy = rep & (HSD_BUTTON_UP | HSD_BUTTON_DPAD_UP) ? NAMER_ROWS - 1 : 1;
        int from_keys = namer.cy == NAMER_ROWS - 1;
        namer.cy = (namer.cy + dy) % NAMER_ROWS;
        int to_keys = namer.cy == NAMER_ROWS - 1;
        // the wide keys sit under the columns they span
        if (to_keys && !from_keys)
            namer.cx = namer.cx * NAMER_KEYS / NAMER_COLS;
        else if (from_keys && !to_keys)
            namer.cx = namer.cx * NAMER_COLS / NAMER_KEYS + 1;
    }
    if (namer.cx != prev_cx || namer.cy != prev_cy)
    {
        namer.dirty = 1;
        SFX_PlayCommon(2);
    }

    if (down & HSD_BUTTON_START)
    {
        Namer_Close(1, 0);
        return;
    }
    if (down & HSD_BUTTON_A)
    {
        if (namer.cy < NAMER_ROWS - 1)
            Namer_Type(namer_rows[namer.lower][namer.cy][namer.cx]);
        else if (namer.cx == 0)
            Namer_Type(' ');
        else if (namer.cx == 1 && namer.len > 0)
        {
            namer.buf[--namer.len] = 0;
            SFX_PlayCommon(0);
        }
        else if (namer.cx == 2)
            namer.lower ^= 1;
        else if (namer.cx == 3)
        {
            Namer_Close(1, 1);
            return;
        }
        namer.dirty = 1;
    }
    else if (down & HSD_BUTTON_B)
    {
        if (namer.len == 0)
        {
            Namer_Close(0, 1);
            return;
        }
        namer.buf[--namer.len] = 0;
        SFX_PlayCommon(0);
    }
    else if (down & HSD_BUTTON_Y)
        Namer_Type(' ');
    else if (down & HSD_BUTTON_X)
    {
        namer.lower ^= 1;
        namer.dirty = 1;
    }
}

void Event_NameView(GOBJ *menu)
{
    int v = Options_Camera[CAMOPT_VIEW].val;
    CamView *w = v > 0 ? View_Find(v) : 0;
    if (!w)
    {
        SFX_PlayCommon(3); // pick a saved view first
        return;
    }
    Namer_Open(NAME_VIEW, v, w->name);
}

void Event_PresetName(GOBJ *menu)
{
    int p = Options_Presets[PROPT_PICK].val;
    if (p < PS_1 || p >= PS_SAVED)
    {
        SFX_PlayCommon(3); // User Custom and the built-in ones keep their names
        return;
    }
    Namer_Open(NAME_PRESET, p, preset_file->preset_name[p]);
}

// Each frame: closing the menu keeps the settings in User Custom, and the
// card is written when anything it holds changed.
static void Presets_Update(void)
{
    int paused = Pause_CheckStatus(1) == 2;
    if (preset_was_paused && !paused)
        Presets_KeepUser();
    // Auto Fade's hit rates go to the card when the menu opens, not after
    // every try
    if (paused && !preset_was_paused)
    {
        for (int i = 0; i < 4; i++)
        {
            if (preset_file->skill[i] != skill[i])
                preset_dirty = 1;
            preset_file->skill[i] = skill[i];
        }
    }
    preset_was_paused = paused;
    if (preset_cam_pending)
    {
        preset_cam_pending = 0;
        Event_ChangeCamera(0, Options_Camera[CAMOPT_MODE].val);
    }
    if (view_pending)
    {
        view_pending = 0;
        View_Apply(Options_Camera[CAMOPT_VIEW].val);
    }
    Card_Update();
}

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
static int body_learned;

// The timers: each counts down to its next window, lights up while a press
// works, then ends by how it went (a hit, a miss or a skip). Each kind is a
// row of the meter and has a spot timer on the floor (see "The timers'
// look" below). An ending plays out while the next countdown of its kind
// runs.
#define LL_COUNT_FRAMES 24 // a countdown starts at most this many frames before its press

enum cue_kind
{
    CUE_AI,
    CUE_WL,
    CUE_NIL,
    CUE_NUM
};

enum cue_phase
{
    PH_OFF,
    PH_COUNT,
    PH_WINDOW,
    PH_HIT,
    PH_FADE, // a miss or a skip folds in on itself
    PH_CUT,  // the prediction moved on before the window
};

enum cue_dim
{
    DIM_NONE,
    DIM_MISS,
    DIM_SKIP,
};

typedef struct Cue
{
    u8 phase;
    u8 width; // frames in the window
    u8 dim;   // a miss or skip already decided: it runs on, muted
    u8 held;  // pressed in the window, waiting for the touchdown
    u8 dirs;  // rails: DODGE_RIGHT / DODGE_LEFT that work; after a hit, the one slid
    u8 wd;    // rails: a wavedash out of the jumpsquat
    u8 soft;  // hit: a waveland that wasn't perfect
    u8 fresh; // set from this frame's prediction
    u8 has_body; // body is set (Cue_Body)
    int left; // frames until the press; 1 = press on the next frame
    int span; // frames the countdown had when it showed up
    int age;  // frames into the window, or into the ending
    int pulse; // frames since the window opened, -1 = none
    int press; // frames since a press that works (or the hit, with none), -1 = none
    int lag;  // hit: the landing lag the timer burns over
    Vec2 spot; // on the floor: where the AI or NIL lands, or the rails' middle
    Vec2 body; // where the middle of his body is as the press works (the touchdown, for a NIL)
    float x0, x1; // rails: where that floor ends
} Cue;

static Cue cue_live[CUE_NUM]; // counting down or in the window
static Cue cue_end[CUE_NUM];  // the last one's ending
static int squat_wd;          // in the jumpsquat: frames until a wavedash's airdodge
static int wd_late;           // that airdodge came a frame after takeoff
static int cue_log;           // write the timers' presses and endings to the log
static const char *cue_names[CUE_NUM] = {"AI", "WL", "NIL"};

static void Cue_Log(int kind, const char *what, int a, int b)
{
    if (!cue_log)
        return;
    char buf[80];
    sprintf(buf, "LLCUE %d %s %s %d %d\n", event_vars->game_timer, cue_names[kind], what, a, b);
    OSReport("%s", buf);
}
///////////////////////
/// Rumble          ///
///////////////////////

// A ramp up to a window on the controller's motor: short buzzes that come
// closer together, then a steady one, stopping dead as the window opens.
// While any cue's rumble is on, the game's own rumble is kept off that
// controller: its player's rumble setting is switched off, so the game
// queues none, and anything queued already is cleared every frame. A buzz
// is queued one frame at a time, so the motor can't be left running if the
// event stops calling this; between buzzes the port's direct setting is a
// stop, and a hard stop (which brakes the motor) as the window opens.
typedef struct PadRumble
{
    u8 last_status, status, direct_status; // HSD_RumbleData
    u16 nb_list;
    void *listdatap;
} PadRumble;
#define pad_rumble ((PadRumble *)0x804C22E0) // one per port
#define MOTOR_HARD_STOP 0 // as HSD reads direct_status
// Buzz on the frames this many frames before the press (bit n: n frames;
// 1 is the window): four on, gaps of four, three, two, then on for the last
// five. The motor needs a few frames to spin up, so shorter pulses barely
// register (Stephen found the first version, two-frame pulses, too weak).
#define RUMBLE_RAMP ((0xFu << 24) | (0xFu << 16) | (0xFu << 9) | (0x1Fu << 2))
#define MOTOR_STOP 1 // a soft stop: the motor coasts between pulses, which reads stronger

static s8 rumble_port = -1; // the port taken over, -1 none
static u8 rumble_ply;       // its player
static u8 rumble_was_on;    // and that player's rumble setting before
static u8 rumble_direct;    // and the port's direct setting
// HSD's rumble script for one frame on: run (op 1) for 1 frame, then end
static const u16 rumble_buzz[2] = {(1 << 13) | 1, 0};

static void Rumble_Release(void)
{
    if (rumble_port < 0)
        return;
    HSD_PadRumbleRemove(rumble_port);
    pad_rumble[rumble_port].direct_status = rumble_direct;
    Fighter_GetPlayerblock(rumble_ply)->flags.b0 = rumble_was_on;
    rumble_port = -1;
}

// Each frame, frozen or not (frozen: paused, or Frame Advance holding).
static void Rumble_Update(FighterData *fp, int frozen)
{
    static const u8 opt[CUE_NUM] = {RUOPT_AI, RUOPT_WL, RUOPT_NIL};
    int want = 0;
    for (int i = 0; i < RUOPT_NOTE; i++)
        want |= Options_Rumble[i].val;
    int port = fp->pad_index;
    if (!want || port > 3)
    {
        Rumble_Release();
        return;
    }
    if (rumble_port != port)
    {
        Rumble_Release();
        Playerblock *pb = Fighter_GetPlayerblock(fp->ply);
        rumble_ply = fp->ply;
        rumble_was_on = pb->flags.b0;
        pb->flags.b0 = 0;
        rumble_direct = pad_rumble[port].direct_status;
        pad_rumble[port].direct_status = MOTOR_HARD_STOP;
        rumble_port = port;
    }
    HSD_PadRumbleRemove(port);
    pad_rumble[port].direct_status = MOTOR_STOP;

    // the soonest window of a cue with its rumble on
    int soon = 99;
    for (int k = 0; k < CUE_NUM && !frozen; k++)
    {
        Cue *c = &cue_live[k];
        if (!Options_Rumble[opt[k]].val || c->dim || c->held)
            continue;
        if (c->phase == PH_WINDOW)
            soon = 0;
        else if (c->phase == PH_COUNT && c->left < soon)
            soon = c->left;
    }
    if (soon >= 2 && soon < 32 && ((RUMBLE_RAMP >> soon) & 1))
        HSD_PadRumbleAdd(port, 0, 1, 0, (void *)rumble_buzz);
    else if (soon <= 1)
        pad_rumble[port].direct_status = MOTOR_HARD_STOP; // it stops dead as the window opens
}

static int galint_now;        // ledge intangibility left once Falcon has let go of the ledge

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
static char text_last[48] = "-";
static u8 press_note; // text_last says why the last press stayed in the air: the landing keeps it
static char text_next[64] = "-"; // the panel's first line: what's coming up
static char text_steps[64];       // ... and a ledge route's inputs under it
static char text_frame[32]; // in Frame Advance: the state on screen and its frame
static int next_kind = -1;       // the cue (CUE_*) whose color the panel's lines start with, -1 none
static int last_kind = -1;
static char text_exact[32] = "-";

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
    sprintf(buf, "LandingLab common: shield drop stick %.4f window %d (raw %x), spotdodge stick %.4f window %d\n",
            common_drop_stick, common_drop_window, Common_Int(COMMON_DROP_WINDOW), common_spot_stick, common_spot_window);
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
    if (ts == TS_FALL || ts == TS_FALLAERIAL || ts == TS_FALLSPECIAL)
    {
        float weight;
        memcpy(&weight, &fp->state_var.state_var2, sizeof(weight));
        sprintf(buf, "LLLEAN %d sm %d w %.5f\n", event_vars->game_timer, fp->state_var.state_var1, weight);
        Log(buf);
    }

    // Falcon Dive: its own drift speed and flags, to check the learned
    // animation movement against
    if (Tracked_IsUpB(ts))
    {
        Vec2 drift = Upb_Drift(fp);
        sprintf(buf, "LLUPB %d drift %.5f %.5f flags %02x\n", event_vars->game_timer, drift.X, drift.Y,
                ((u8 *)&fp->state_var)[UPB_FLAGS]);
        Log(buf);
    }
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

// Each of the three things worth practicing has its own toggle, so one can
// be practiced alone.
static int Cues_Nil(void)
{
    return Options_Cues[COPT_NIL].val;
}

static int Cues_Ai(void)
{
    return Options_Cues[COPT_AI].val;
}

static int Cues_Waveland(void)
{
    return Options_Cues[COPT_WL].val != 0;
}

static int Cues_WavelandGround(void)
{
    return Options_Cues[COPT_WL].val == 2;
}

// Only NILs, aerial interrupts and perfect wavelands are worth practicing
// toward, and only the ones whose cues are on are highlighted.
static int Kind_Shown(int kind)
{
    return (kind == LAND_NIL && Cues_Nil()) || (kind == LAND_AI && Cues_Ai()) ||
           (kind == LAND_PERFECT_WL && Cues_Waveland());
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
    if (Kind_Shown(p->land_kind))
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

    char *t = text_next;
    if (preview_fh)
        t += sprintf(t, "FH %s%s%s", Kind_Short(pred_fh), fh[0] != '-' ? " " : "", fh[0] != '-' ? fh : "");
    if (preview_sh)
        sprintf(t, "%sSH %s%s%s", preview_fh ? ", " : "", Kind_Short(pred_sh), sh[0] != '-' ? " " : "", sh[0] != '-' ? sh : "");

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
    if (y > 0 || fabs(x) < common_dodge_deadzone.X || -y > fabs(x) * WL_LOW_MAX_TAN)
        return 0;
    return x > 0 ? DODGE_RIGHT : DODGE_LEFT;
}

// An airdodge's speed, sideways or at most WL_LOW_MAX_TAN below it.
static int Dodge_Low(Vec2 v)
{
    return v.X != 0 && v.Y <= 0 && -v.Y <= fabs(v.X) * WL_LOW_MAX_TAN;
}

static int dodge_start = -100; // game frame the last airdodge started
static int dodge_late_ok;      // ... on a late window's frame: landing a frame later is perfect
static int prev_wl_late;       // last frame's prediction: a late window on its next frame

static int Jump_Or_Fall(int ts)
{
    return ts >= 0 && ts <= TS_FALLAERIAL;
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
    if (!seg_valid || !Jump_Or_Fall(prev_ts) || !(Tracked_IsAerial(ts) || ts == TS_ESCAPEAIR))
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
        u8 others = dodge ? 0 : shown[k] & ~bit; // aerials that would have landed on this frame
        if (others)
        {
            // the frame was in the window, for another aerial: say which,
            // since the stick held in turns A into a fair
            char *t = text_last;
            t += sprintf(t, "No AI: %s here, ", tracked_state_names[ts]);
            int n = 0;
            for (int a = TS_AIRN; a <= TS_AIRLW && n < 2; a++)
                if (others & AERIAL_BIT(a))
                    t += sprintf(t, n++ ? "/%s" : "%s", tracked_state_names[a]);
            sprintf(t, n > 1 ? " land" : " lands");
        }
        else if (off == 0)
            return;
        else if (dodge)
            sprintf(text_last, "No %s, %df %s", name, off < 0 ? -off : off, off < 0 ? "early" : "late");
        else
            sprintf(text_last, "No AI: %s %df %s", tracked_state_names[ts], off < 0 ? -off : off, off < 0 ? "early" : "late");
        last_kind = -1;
        press_note = 1;
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
        last_kind = -1;
    }
    else
    {
        what = "aerial interrupt";
        sprintf(text_last, "No AI, predicted");
        last_kind = -1;
    }
    press_note = 1;

    stat_total++;
    Text_Exact();

    sprintf(buf, "LandingLab ai miss: %s at %d x %.4f y %.4f stayed in the air, predicted a %s (from %d)\n",
            tracked_state_names[ts], event_vars->game_timer, fp->phys.pos.X, fp->phys.pos.Y, what, seg_start_timer);
    Log(buf);
}

static void Cue_Missed(int kind);
static void Cue_Pressed(int kind);
static int route_active; // following a ledge route after letting go
static int jt_active;    // Jump Timing has an anchor for this fall

// Buzz when a window passes without its press, or an aerial or airdodge
// comes while the countdown runs but doesn't touch down. Called on tracked
// air frames, after pred_live is updated.
static void Window_Feedback(FighterData *fp, int ts)
{
    int miss = 0, skip = 0;
    if (Jump_Or_Fall(prev_ts) && Tracked_IsAerial(ts))
    {
        int timed = prev_ai_first && prev_ai_first <= LL_COUNT_FRAMES;
        miss = timed && !(prev_ai_now & AERIAL_BIT(ts));
        if (miss)
            Cue_Missed(CUE_AI);
        else if (timed)
            Cue_Pressed(CUE_AI);
    }
    else if (Jump_Or_Fall(prev_ts) && ts == TS_ESCAPEAIR)
    {
        int timed = prev_wl_first && prev_wl_first <= LL_COUNT_FRAMES && !cue_live[CUE_WL].held;
        miss = timed && !(prev_wl_now & Dodge_Dir(fp->input.lstick.X, fp->input.lstick.Y));
        if (miss)
            Cue_Missed(CUE_WL);
        else if (timed)
            Cue_Pressed(CUE_WL);
    }
    else if (Jump_Or_Fall(ts))
    {
        // this was the frame to press, and pressing on the next one is too late
        Prediction *p = pred_live;
        int ai_next = Cues_Ai() && p->ai_first == 1 && p->uncertain_from > 1;
        int wl_next = Cues_Waveland() && p->wl_first == 1 && p->uncertain_from > 1;
        skip = (prev_ai_first == 1 || prev_wl_first == 1) && !ai_next && !wl_next;
    }
    if ((miss && Options_Sounds[SOPT_WINDOW].val) || (skip && Options_Sounds[SOPT_SKIP].val))
        SFX_PlayCommon(3);
}

// What Window_Feedback needs from this frame's prediction on the next one.
static void Window_Remember(Prediction *p)
{
    prev_ai_first = Cues_Ai() && p->ai_first < p->uncertain_from ? p->ai_first : 0;
    prev_wl_first = Cues_Waveland() && p->wl_first < p->uncertain_from ? p->wl_first : 0;
    prev_ai_now = p->num >= 1 ? p->ai_mask[1] : 0;
    prev_wl_now = p->num >= 1 ? WL_Mask(p, 1) : 0;
}

static void Window_Forget(void)
{
    prev_ai_first = 0;
    prev_wl_first = 0;
    prev_ai_now = 0;
    prev_wl_now = 0;
}

///////////////////////
/// Timers          ///
///////////////////////

#define LL_GHOST 6 // frames a ghost takes to spread out and fade: snappy
#define LL_BURST 7 // a hit's burst
#define LL_FADE 10 // a miss or skip folding in
#define LL_CUT 4   // a countdown the prediction dropped

// The floor under (x, y), at most 20 below: its height at x and how far it
// runs each way, across floors joined end to end.
static void Floor_Ends(FloorLine *f, float *l, float *ly, float *r, float *ry)
{
    int flip = f->x0 > f->x1;
    *l = flip ? f->x1 : f->x0;
    *ly = flip ? f->y1 : f->y0;
    *r = flip ? f->x0 : f->x1;
    *ry = flip ? f->y0 : f->y1;
}

static int Floor_Under(float x, float y, Vec2 *spot, float *x0, float *x1)
{
    FloorLine *best = 0;
    float best_y = 0;
    float l, ly, r, ry;
    for (int i = 0; i < floor_num; i++)
    {
        FloorLine *f = &floor_cache[i];
        Floor_Ends(f, &l, &ly, &r, &ry);
        if (r - l < 0.001f || x < l || x > r)
            continue;
        float h = ly + (ry - ly) * (x - l) / (r - l);
        if (h <= y + 1.f && h >= y - 20.f && (!best || h > best_y))
        {
            best = f;
            best_y = h;
        }
    }
    if (!best)
        return 0;

    spot->X = x;
    spot->Y = best_y;
    Floor_Ends(best, &l, &ly, &r, &ry);
    for (int n = 0; n < 16; n++)
    {
        int grew = 0;
        for (int i = 0; i < floor_num; i++)
        {
            float fl, fly, fr, fry;
            Floor_Ends(&floor_cache[i], &fl, &fly, &fr, &fry);
            if (fl < l && fabs(fr - l) < 0.5f && fabs(fry - ly) < 0.5f)
            {
                l = fl;
                ly = fly;
                grew = 1;
            }
            else if (fr > r && fabs(fl - r) < 0.5f && fabs(fly - ry) < 0.5f)
            {
                r = fr;
                ry = fry;
                grew = 1;
            }
        }
        if (!grew)
            break;
    }
    *x0 = l;
    *x1 = r;
    return 1;
}

static void Cue_Place(Cue *c, float x, float y)
{
    if (!Floor_Under(x, y, &c->spot, &c->x0, &c->x1))
    {
        c->spot = (Vec2){x, y};
        c->x0 = x - 1000.f;
        c->x1 = x + 1000.f;
    }
}

static void Cue_Open(Cue *c)
{
    c->left = 1;
    c->phase = PH_WINDOW;
    c->age = 0;
    c->pulse = 0;
}

static void Skill_Add(int kind, int hit);

static void Cue_Finish(int kind, int phase, int dim)
{
    Cue *c = &cue_live[kind];
    if (!c->phase)
        return;
    Cue_Log(kind, "end", phase, dim);
    Cue *e = &cue_end[kind];
    if (phase == PH_FADE && dim == DIM_MISS)
        Skill_Add(kind, 0);
    *e = *c;
    e->phase = phase;
    e->dim = dim;
    e->age = 0;
    c->phase = PH_OFF;
}

// A countdown from this frame's prediction, its press left frames away.
static void Cue_Set(int kind, int left, int width, u8 dirs, int wd, float x, float y)
{
    Cue *c = &cue_live[kind];
    if (c->phase && (c->dim || c->held))
        return; // already decided: it runs on its own clock
    if (c->phase == PH_WINDOW && left > 1)
        Cue_Finish(kind, PH_FADE, DIM_SKIP); // the window went by
    if (!c->phase || left > c->left + 1)
    {
        // new, or a later window: start full and close over what's left
        memset(c, 0, sizeof(*c));
        c->span = left;
        c->pulse = -1;
        c->press = -1;
    }
    // a window's width is fixed before it opens; once open, what's left of
    // it shrinks
    if (c->phase != PH_WINDOW)
    {
        c->width = width;
        if (left <= 1)
            Cue_Open(c);
        else
            c->phase = PH_COUNT;
    }
    c->left = left;
    c->dirs = dirs;
    c->wd = wd;
    c->fresh = 1;
    Cue_Place(c, x, y);
}

// Where the middle of Falcon's body (halfway between his ECB's bottom and
// top) is on frame k of the prediction p, for the near-Falcon bubble: as he
// presses (a frame before the first the press works), or, for a NIL, as he
// touches down. Set with each countdown frame and on the frame the window
// opens, then kept: the ending copies it, so the bubble doesn't follow him
// through the window or the hit. A cue Cue_Set left alone (already decided)
// keeps its own.
static void Cue_Body(int kind, Prediction *p, int k)
{
    Cue *c = &cue_live[kind];
    if (!c->fresh || (c->phase == PH_WINDOW && c->age > 0) || k < 0 || k > p->num)
        return;
    c->body = (Vec2){p->pos[k].X, p->pos[k].Y + (p->bottom[k] + p->top[k]) / 2.f};
    c->has_body = 1;
}

// A press too early, or one that doesn't work: the timer still runs to its
// window, muted, so you see when it was.
static void Cue_Missed(int kind)
{
    Cue *c = &cue_live[kind];
    if (c->phase == PH_WINDOW)
        Cue_Finish(kind, PH_FADE, DIM_MISS);
    else if (c->phase == PH_COUNT)
    {
        c->dim = DIM_MISS;
        if (--c->left <= 1)
            Cue_Open(c);
    }
}

// A press that works, whose touchdown comes when the ECB lock runs out.
static void Cue_Pressed(int kind)
{
    Cue *c = &cue_live[kind];
    if (!c->phase || c->dim)
        return;
    Cue_Log(kind, "press", c->phase == PH_WINDOW ? c->age : -c->left, c->wd);
    if (c->phase != PH_WINDOW)
        Cue_Open(c);
    c->held = 1;
    c->press = 0;
}

// A hit plays until the burn over the landing lag and its flash are done,
// longer for a waveland's slide marker, and on while ledge intangibility is
// left to show.
static int Cue_HitLen(Cue *e, int kind)
{
    int len = e->lag + 5;
    if (len < LL_GHOST * 2)
        len = LL_GHOST * 2;
    if (kind == CUE_WL)
        len = e->lag + 24;
    return len;
}

// Done: the timer moves to where Falcon touched down and plays its hit, even
// when no countdown ran.
static void Cue_Hit(FighterData *fp, int kind, int lag, int soft, u8 dirs)
{
    Cue *c = &cue_live[kind];
    Cue *e = &cue_end[kind];
    Cue_Log(kind, "hit", lag, soft);
    Skill_Add(kind, 1);
    if (c->phase)
        *e = *c;
    else
    {
        memset(e, 0, sizeof(*e));
        e->pulse = -1;
        e->press = -1;
    }
    c->phase = PH_OFF;
    e->phase = PH_HIT;
    e->age = 0;
    e->dim = DIM_NONE;
    e->lag = lag;
    e->soft = soft;
    if (kind == CUE_WL)
        e->dirs = dirs;
    if (e->pulse < 0)
        e->pulse = 0;
    if (e->press < 0)
        e->press = 0; // no press to pulse on (a NIL): the hit pulses
    Cue_Place(e, fp->phys.pos.X, fp->phys.pos.Y + 1.f);
}

// Falcon touched down as kind. The timer that called for it is a hit; any
// other one still running fades out quickly, or as a miss if it was due.
static void Cue_Landed(FighterData *fp, int kind, int landing_air)
{
    // a wavedash timed out of the jumpsquat counts as perfect
    Cue *wl = &cue_live[CUE_WL];
    int wd_timed = wl->phase == PH_WINDOW && wl->wd && !wl->dim;

    // an airdodge that lands with no waveland timer behind it is only a landing
    int hit = -1;
    if (kind == LAND_NIL && Cues_Nil())
        hit = CUE_NIL;
    else if (kind == LAND_AI && !landing_air && Cues_Ai())
        hit = CUE_AI;
    else if ((kind == LAND_PERFECT_WL || (kind == LAND_WAVELAND && wl->phase)) && Cues_Waveland())
        hit = CUE_WL;

    for (int i = 0; i < CUE_NUM; i++)
    {
        Cue *c = &cue_live[i];
        if (i == hit || !c->phase || c->dim)
            continue;
        int due = c->phase == PH_WINDOW || c->held || c->left <= 2;
        Cue_Finish(i, hit < 0 && due ? PH_FADE : PH_CUT, DIM_MISS);
    }
    if (hit == CUE_WL)
    {
        float v = fp->phys.self_vel_ground.X != 0 ? fp->phys.self_vel_ground.X : prev_vel.X;
        int perfect = kind == LAND_PERFECT_WL || (wd_timed && !wd_late);
        Cue_Hit(fp, CUE_WL, (int)common_waveland_lag, !perfect, v >= 0 ? DODGE_RIGHT : DODGE_LEFT);
    }
    else if (hit >= 0)
        Cue_Hit(fp, hit, hit == CUE_AI ? (int)fp->attr.normal_landing_lag : 0, 0, 0);
}

// Before this frame's prediction: the clocks move on and endings play out.
static void Cues_Begin(void)
{
    for (int i = 0; i < CUE_NUM; i++)
    {
        Cue *c = &cue_live[i];
        c->fresh = 0;
        if (c->phase)
        {
            if (c->pulse >= 0)
                c->pulse++;
            if (c->press >= 0)
                c->press++;
            if (c->phase == PH_WINDOW)
                c->age++;
            if (c->held && c->age > 15)
                Cue_Finish(i, PH_FADE, DIM_MISS); // pressed, but it never touched down
            else if (c->dim && c->phase == PH_COUNT && --c->left <= 1)
                Cue_Open(c);
            else if (c->dim && c->phase == PH_WINDOW && c->age >= c->width)
                Cue_Finish(i, PH_FADE, c->dim);
        }

        Cue *e = &cue_end[i];
        if (e->phase)
        {
            if (e->pulse >= 0)
                e->pulse++;
            if (e->press >= 0)
                e->press++;
            e->age++;
            int len = e->phase == PH_HIT ? Cue_HitLen(e, i) : e->phase == PH_FADE ? LL_FADE : LL_CUT;
            if (e->age >= len && !(e->phase == PH_HIT && galint_now > 1))
                e->phase = PH_OFF;
        }
    }
}

// After it: a countdown the prediction no longer has ended without its
// press, skipped if its window was on, cut off if not.
static void Cues_End(void)
{
    for (int i = 0; i < CUE_NUM; i++)
    {
        Cue *c = &cue_live[i];
        if (c->phase && !c->fresh && !c->dim && !c->held)
            Cue_Finish(i, c->phase == PH_WINDOW ? PH_FADE : PH_CUT, DIM_SKIP);
    }
}

static void Cues_Clear(void)
{
    memset(cue_live, 0, sizeof(cue_live));
    memset(cue_end, 0, sizeof(cue_end));
}

#define LL_GHOST_FRAMES 90

// The fighter left the tracked air states this frame: judge the landing.
static void Landing_Judge(FighterData *fp);

// The landing's own text ("Lag") would replace why the press before it
// stayed in the air, which is the part worth reading, so that stays up
// unless the landing was a hit after all.
static void Landing_Resolve(FighterData *fp)
{
    char note[sizeof(text_last)] = "";
    int keep = press_note && !route_active;
    if (keep)
        strcpy(note, text_last);
    press_note = 0;
    Landing_Judge(fp);
    if (keep && last_kind < 0)
        strcpy(text_last, note);
}

static void Landing_Judge(FighterData *fp)
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
    else if (Tracked_IsHelpless(prev_ts))
        kind = LAND_NORMAL; // Falcon Dive or the helpless fall: their own lag
    else if (sid == ASID_LANDINGFALLSPECIAL)
    {
        // perfect: a dodge sideways or just below whose own ECB touched down
        // the first time it was used, now or as the lock ran out; or a
        // frame later, pressed in a late window (no sooner one was there)
        int low = prev_ts == TS_ESCAPEAIR && Dodge_Low(prev_vel);
        int perfect = dodge > 0 || (low && prev_lock == 1) ||
                      (low && dodge_late_ok && event_vars->game_timer - dodge_start == 1);
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
    ghost_visible = seg_valid && (Kind_Shown(kind) || Kind_Shown(pred_seg->land_kind));
    ghost_timer = LL_GHOST_FRAMES;

    // an AI into aerial lag (uair) is still an AI, but nothing to cheer
    int hit = kind == LAND_NIL || kind == LAND_PERFECT_WL || (kind == LAND_AI && !landing_air);
    last_kind = kind == LAND_NIL ? CUE_NIL : kind == LAND_AI ? CUE_AI : kind == LAND_PERFECT_WL ? CUE_WL : -1;
    Cue_Landed(fp, kind, landing_air);
    // only for the cues that are on; a ledge route chimes on its own
    if (Options_Sounds[SOPT_CHIME].val && hit && Kind_Shown(kind) && !route_active)
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
            predicted = in_range && (WL_Mask(pred_seg, k) & dodge);
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
            if (!predicted)
                sprintf(text_last, "%s, not predicted", name);
            else if (pressed >= 0)
                sprintf(text_last, "AI %s", tracked_state_names[pressed]);
            else
                sprintf(text_last, "%s", name);
            Text_Exact();
        }

        sprintf(buf, "LandingLab landing: %s with %s pressed at %d x %.4f y %.4f line %d, %s (from %d)%s\n",
                land_kind_names[kind], pressed >= 0 ? tracked_state_names[pressed] : (dodge == DODGE_RIGHT ? "airdodge right" : "airdodge left"),
                event_vars->game_timer, fp->phys.pos.X, fp->phys.pos.Y, fp->coll_data.ground_index,
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
            sprintf(text_last, "%s", land_kind_names[kind]);
        }
        else if (!Kind_Matches(kind, pred_seg->land_kind))
            sprintf(text_last, "%s, not %s", land_kind_names[kind], land_kind_names[pred_seg->land_kind]);
        else
            sprintf(text_last, "%s, %df %s", land_kind_names[kind], diff > 0 ? diff : -diff, diff > 0 ? "late" : "early");
        Text_Exact();
    }

    sprintf(buf, "LandingLab landing: %s at %d x %.4f y %.4f line %d, predicted %s at %d x %.4f (from %d)%s%s\n",
            land_kind_names[kind], event_vars->game_timer, fp->phys.pos.X, fp->phys.pos.Y, fp->coll_data.ground_index,
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

// World shapes sit at Falcon's depth (z 0) and are tested against what's
// already drawn, so his model and the stage in front of him hide them, as
// if the lines ran through his middle. Some stay on top of everything:
// ledge routes and the inputs along the paths, which his model would hide
// where they matter, and the collision display. Like the event's own
// GFX_Start, with the depth test left on.
static int world_on_top;
static int world_add; // glows: added to what's behind, so a faint one never darkens it

// Intensity: the cues, paths and timers are drawn fainter or bolder than
// the standard look (level 3); the controller and the panel never change.
// Fainter takes color and opacity down together. Bolder makes a shape more
// opaque, and brighter as far as its strongest channel allows, so no color
// shifts toward white and a hit can't be mistaken for a miss.
static const float vis_levels[] = {0.55f, 0.78f, 1.f, 1.3f, 1.65f};
static float vis_k = 1.f; // for what's being drawn now

static GXColor Vis(GXColor c)
{
    if (vis_k == 1.f)
        return c;
    float ka = vis_k, kc = vis_k;
    if (vis_k > 1.f)
    {
        int m = c.r > c.g ? c.r : c.g;
        if (c.b > m)
            m = c.b;
        if (c.a * ka > 255.f)
            ka = c.a ? 255.f / c.a : 1.f;
        kc = ka;
        if (m * kc > 255.f)
            kc = m ? 255.f / m : 1.f;
    }
    c.r = c.r * kc;
    c.g = c.g * kc;
    c.b = c.b * kc;
    c.a = c.a * ka;
    return c;
}

// Intensity by group (Options_Intensity), and Auto Fade: each cue kind
// keeps a running hit rate (about the last 10 tries), and in a group set
// to fade, each kind's drawing dims by its own rate (Fade_Factor).
enum { VG_CUES, VG_PATHS, VG_TIMERS, VG_LEDGE, VG_PAD };

static float Group_K(int g)
{
    if (g == VG_PAD)
        return vis_levels[Options_Intensity[IOPT_PAD].val];
    int lv = Options_Cues[COPT_INTENSITY].val + Options_Intensity[g].val - 2;
    lv = lv < 0 ? 0 : lv > 4 ? 4 : lv;
    return vis_levels[lv];
}

static void Skill_Add(int kind, int hit)
{
    skill[kind] += ((hit ? 1.f : 0.f) - skill[kind]) * 0.15f;
    if (cue_log)
        OSReport("LLFADE %d %s rate %.2f\n", kind, hit ? "hit" : "miss", skill[kind]);
}

// How far Auto Fade dims this frame: a kind's hit rate fades it from
// half its tries hit (nothing) to 9 in 10 (down to 35%).
static float Fade_Of(int kind)
{
    float t = (skill[kind] - 0.5f) / 0.4f;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return 1.f - 0.65f * t;
}

// Auto Fade's part of a group's level for one cue kind's drawing (1: none).
static float Fade_Factor(int g, int kind)
{
    int fade = Options_Intensity[IOPT_FADE].val;
    if (kind >= 0 && kind < CUE_NUM &&
        ((g == VG_TIMERS && (fade == FADE_TIMERS || fade == FADE_BOTH)) ||
         (g == VG_CUES && (fade == FADE_CUES || fade == FADE_BOTH))))
        return Fade_Of(kind);
    return 1.f;
}

static float Kind_K(int g, int kind)
{
    return Group_K(g) * Fade_Factor(g, kind);
}

void Event_FadeReset(GOBJ *menu)
{
    memset(skill, 0, sizeof(skill));
    SFX_PlayCommon(1);
}

static void World_Vtx(f32 x, f32 y, f32 z, GXColor c)
{
    GFX_AddVtx(x, y, z, Vis(c));
}

static void World_Start(int count, u8 shape, u8 size)
{
    Mtx mtx;
    HSD_ClearVtxDesc();
    GXSetCurrentMtx(0);
    COBJ_GetViewingMtx(COBJ_GetCurrent(), &mtx);
    GXLoadPosMtxImm(mtx, 0);
    HSD_StateSetLineWidth(size, 5);
    HSD_StateSetPointSize(size, 5);
    // blended, no depth writes, and the depth test off (on top) or less-equal
    HSD_SetupRenderMode(world_on_top ? 0x68000002 : 0x60000002);
    // the same depth mode again, straight to GX: something that set it
    // directly in between would leave the cached state stale
    GXSetZMode(1, world_on_top ? GX_ALWAYS : GX_LEQUAL, 0);
    if (world_add)
        HSD_StateSetBlendMode(GX_BM_BLEND, GX_BL_ONE, GX_BL_ONE, GX_LO_NOOP);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    HSD_StateSetCullMode(0);
    GXBegin(shape, GX_VTXFMT0, count);
}

// The paths flow: a brighter band keeps running from Falcon to the path's
// end over a dimmer line, so the lines move like the timers do.
#define LL_FLOW_LEN 6.f

static GXColor Path_Flow(GXColor c, int i, int from, int to)
{
    int len = to - from + (int)(LL_FLOW_LEN * 2);
    float head = (float)((event_vars->game_timer * 2) % len) - LL_FLOW_LEN;
    float d = fabs((i - from) - head);
    float k = 0.55f + (d < LL_FLOW_LEN ? 0.45f * (1.f - d / LL_FLOW_LEN) : 0);
    c.r = c.r * k;
    c.g = c.g * k;
    c.b = c.b * k;
    c.a = c.a * k;
    return c;
}

static void Draw_Path(Vec2 *pos, float *bottom, int from, int to, GXColor color, u8 size)
{
    int count = to - from + 1;
    if (count < 2)
        return;

    World_Start(count, GX_LINESTRIP, size);
    for (int i = from; i <= to; i++)
        World_Vtx(pos[i].X, pos[i].Y + bottom[i], 0, Path_Flow(color, i, from, to));
}

// The paths look different from each other: the landing path is solid, the
// short hop preview dashed (a dash every other frame, so it stays readable
// where it runs along the full hop's line) and the body path dotted.
#define LINE_SOLID 0
#define LINE_DASHED 1

static void Draw_Dashed(Vec2 *pos, float *bottom, int from, int to, GXColor color, u8 size)
{
    int dashes = (to - from + 1) / 2;
    if (dashes < 1)
        return;

    World_Start(dashes * 2, GX_LINES, size);
    for (int i = from; i + 1 <= to; i += 2)
    {
        World_Vtx(pos[i].X, pos[i].Y + bottom[i], 0, Path_Flow(color, i, from, to));
        World_Vtx(pos[i + 1].X, pos[i + 1].Y + bottom[i + 1], 0, Path_Flow(color, i + 1, from, to));
    }
}

// Frame Dots: a small ring on the path at every frame; their spacing shows
// the speed. Only during Frame Advance unless set to always, so the paths
// stay light at full speed.
#define LL_DOT_R 0.42f

static int Ticks_On(void)
{
    int opt = Options_Paths[POPT_TICKS].val;
    return opt == 1 || (opt == 0 && Options_Game[GOPT_FRAME_ADV].val);
}

static void Draw_Ticks(Vec2 *pos, float *bottom, int from, int to, GXColor color, u8 size)
{
    int count = to - from + 1;
    if (count < 2)
        return;

    World_Start(count * 8, GX_LINES, size);
    for (int i = from; i <= to; i++)
    {
        float x = pos[i].X;
        float y = pos[i].Y + bottom[i];
        float r = LL_DOT_R;
        World_Vtx(x, y - r, 0, color);
        World_Vtx(x + r, y, 0, color);
        World_Vtx(x + r, y, 0, color);
        World_Vtx(x, y + r, 0, color);
        World_Vtx(x, y + r, 0, color);
        World_Vtx(x - r, y, 0, color);
        World_Vtx(x - r, y, 0, color);
        World_Vtx(x, y - r, 0, color);
    }
}

static void Draw_Line(Prediction *p, int from, int to, GXColor color, u8 size, int style)
{
    if (style == LINE_DASHED)
        Draw_Dashed(p->pos, p->bottom, from, to, color, size);
    else
        Draw_Path(p->pos, p->bottom, from, to, color, size);
    if (Ticks_On())
        Draw_Ticks(p->pos, p->bottom, from, to, color, 9);
}

// The fighter's position raised by a fixed amount: a smooth arc through his
// body, unlike the ECB bottom, which moves with his legs. Drawn as a dot on
// every frame.
#define LL_DOT 0.4f

static void Draw_BodyPath(Prediction *p, int from, int last, GXColor c)
{
    int count = last - from + 1;
    if (count < 2)
        return;

    World_Start(count * 4, GX_QUADS, 0);
    for (int i = from; i <= last; i++)
    {
        float x = p->pos[i].X, y = p->pos[i].Y + body_offset;
        World_Vtx(x - LL_DOT, y - LL_DOT, 0, c);
        World_Vtx(x + LL_DOT, y - LL_DOT, 0, c);
        World_Vtx(x + LL_DOT, y + LL_DOT, 0, c);
        World_Vtx(x - LL_DOT, y + LL_DOT, 0, c);
    }
}

static void Draw_Ecb(Vec2 pos, float bottom, EcbSample *s, float facing, GXColor color)
{
    if (!s->has_shape)
    {
        // shape unknown: a small cross at the touchdown point
        float y = pos.Y + bottom;
        World_Start(4, GX_LINES, 24);
        World_Vtx(pos.X - 1.5f, y - 1.5f, 0, color);
        World_Vtx(pos.X + 1.5f, y + 1.5f, 0, color);
        World_Vtx(pos.X - 1.5f, y + 1.5f, 0, color);
        World_Vtx(pos.X + 1.5f, y - 1.5f, 0, color);
        return;
    }

    float right_x = facing > 0 ? s->front : -s->back;
    float left_x = facing > 0 ? s->back : -s->front;

    World_Start(5, GX_LINESTRIP, 24);
    World_Vtx(pos.X, pos.Y + s->top, 0, color);
    World_Vtx(pos.X + right_x, pos.Y + s->side_y, 0, color);
    World_Vtx(pos.X, pos.Y + bottom, 0, color);
    World_Vtx(pos.X + left_x, pos.Y + s->side_y, 0, color);
    World_Vtx(pos.X, pos.Y + s->top, 0, color);
}

// Falcon's ECB right now, for the collision view (his model is hidden).
static void Draw_CurrentEcb(FighterData *fp)
{
    CollData *cd = &fp->coll_data;
    float x = fp->phys.pos.X;
    float y = fp->phys.pos.Y;

    World_Start(5, GX_LINESTRIP, 24);
    World_Vtx(x + cd->ecbCurrCorrect_top.X, y + cd->ecbCurrCorrect_top.Y, 0, color_ecb);
    World_Vtx(x + cd->ecbCurrCorrect_right.X, y + cd->ecbCurrCorrect_right.Y, 0, color_ecb);
    World_Vtx(x + cd->ecbCurrCorrect_bot.X, y + cd->ecbCurrCorrect_bot.Y, 0, color_ecb);
    World_Vtx(x + cd->ecbCurrCorrect_left.X, y + cd->ecbCurrCorrect_left.Y, 0, color_ecb);
    World_Vtx(x + cd->ecbCurrCorrect_top.X, y + cd->ecbCurrCorrect_top.Y, 0, color_ecb);
}

// The aerial pickers (Compass_Draw), queued here while the stage is drawn
// and drawn on the HUD after it.
#define CP_MAX 4
typedef struct Compass
{
    float x, y;
    u8 mask;
    s8 facing;
    GXColor color;
} Compass;
static Compass compass[CP_MAX];
static int compass_num;

static void Compass_Queue(float x, float y, u8 mask, float facing, GXColor color)
{
    if (compass_num >= CP_MAX || !mask)
        return;
    compass[compass_num++] = (Compass){x, y, mask, facing > 0 ? 1 : -1, color};
}

// Windows after the next one of their kind: the same color at 40%
// (premultiplied).
static GXColor Color_Fill(GXColor c, float a);

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
    World_Start(e - k + 3, GX_LINESTRIP, size);
    World_Vtx((p->pos[before].X + p->pos[k].X) / 2,
               (p->pos[before].Y + p->bottom[before] + p->pos[k].Y + p->bottom[k]) / 2, 0, color);
    for (int i = k; i <= e; i++)
        World_Vtx(p->pos[i].X, p->pos[i].Y + p->bottom[i], 0, color);
    World_Vtx((p->pos[e].X + p->pos[after].X) / 2,
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
            if (!WL_Mask(p, k))
                continue;
            int e = k;
            while (e < last_wl && WL_Mask(p, e + 1))
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
            if (Options_Paths[POPT_INPUTS].val)
                Compass_Queue(p->pos[k].X, p->pos[k].Y + p->bottom[k], mask, p->facing, color);
            k = e;
        }
    }
}

static void Draw_Prediction(Prediction *p, int body, int style)
{
    int last = p->land_frame ? p->land_frame : p->num;
    int certain = p->land_frame && p->uncertain_from > p->land_frame;
    int highlight = certain && Kind_Shown(p->land_kind);
    int from = 0;

    int line = Options_Paths[POPT_PATH].val;

    // an aerial interrupt still ahead: past its window the path only shows
    // where Falcon goes if he skips it (usually on up through the platform
    // it lands on), so it's grayed until then
    int split = last;
    if (Cues_Ai() && p->ai_first && p->ai_first < p->uncertain_from && p->ai_first + p->ai_width - 1 < last)
        split = p->ai_first + p->ai_width - 1;
    GXColor skipped = Color_Fill(color_neutral, 0.45f);

    if (body && Options_Paths[POPT_BODY].val)
    {
        Draw_BodyPath(p, 0, split, color_body);
        if (split < last)
            Draw_BodyPath(p, split + 1, last, Color_Fill(color_body, 0.35f));
    }

    if (line && highlight)
    {
        Draw_Line(p, from, split, land_kind_colors[p->land_kind], 24, style);
        if (split < last)
            Draw_Line(p, split, last, skipped, 12, style);
    }
    else if (line)
    {
        int known = p->uncertain_from - 1;
        if (known > last)
            known = last;
        if (known < from)
            known = from;
        Draw_Line(p, from, known, color_neutral, 12, style);
        Draw_Line(p, known, last, color_learning, 12, style);
    }

    Draw_Windows(p);

    if (highlight)
    {
        int k = p->land_frame;
        Draw_Ecb(p->pos[k], p->bottom[k], &p->land_ecb, p->facing, land_kind_colors[p->land_kind]);
    }

    // short white tick where a pending fastfall kicks in
    if (line && p->fastfall_frame && p->fastfall_frame <= last)
    {
        int k = p->fastfall_frame;
        float y = p->pos[k].Y + p->bottom[k];
        World_Start(2, GX_LINES, 24);
        World_Vtx(p->pos[k].X - 2.f, y, 0, color_actual);
        World_Vtx(p->pos[k].X + 2.f, y, 0, color_actual);
    }
}

// The timers' look. Everything that times an input is drawn in screen
// space on the HUD camera, so it keeps its size whatever the zoom:
// - the meter, a frame strip above Falcon: one cell per frame, feeding
//   into the gate at its left end. Press as a solid cell reaches the gate.
//   A hollow cell with a floor bar is the touchdown, light dashes are
//   frames in the air, and the periwinkle tail is the ledge intangibility
//   left once Falcon can act. Cells already past are eaten by the gate.
// - the spot timers, on the floor where it happens: brackets close in for
//   an AI or NIL, ticks run in from both ends of the slide to the middle
//   for a waveland or wavedash.
// - the fixed strip (optional), a bigger meter at the bottom with labels.
// Every timer sends one ghost out: a strong one with a burst on a press
// that works, else a faint one on the window's last frame (a skip), and
// none for an early or late press. It ends in a burst (hit) or a muted
// implosion (miss or skip). Hits keep the cue's color, misses turn a cold slate and skips
// gray.
static const GXColor color_miss = {104, 114, 150, 255}; // cold slate
static const GXColor color_skip = {140, 149, 168, 255};
static const GXColor color_white = {255, 255, 255, 255};
static const GXColor color_galint = {128, 128, 255, 255};
static const GXColor color_plate = {6, 8, 16, 255};

// What each input is drawn in wherever it shows (the markers along the
// paths, the timer's glyphs, the controller display): the stick white, a
// jump yellow, an aerial pink like the AI cues, an airdodge cyan like the
// waveland cues.
static const GXColor color_in_jump = {255, 220, 40, 255};
#define color_in_aerial land_kind_colors[LAND_AI]
#define color_in_dodge land_kind_colors[LAND_PERFECT_WL]

// A color at a fraction of its strength (colors are premultiplied). The
// HUD blends by source alpha (HSD_SetupPEMode: SRCALPHA, INVSRCALPHA), so
// it shows at about a * a over what's behind it; it was all tuned that way.
static GXColor Color_Fill(GXColor c, float a)
{
    if (a < 0)
        a = 0;
    if (a > 1)
        a = 1;
    c.r = c.r * a;
    c.g = c.g * a;
    c.b = c.b * a;
    c.a = 255 * a;
    return c;
}

// The whole color laid over what's behind it at a, for a faint mark that
// still has to read over the dark plates.
static GXColor Color_Over(GXColor c, float a)
{
    if (a < 0)
        a = 0;
    if (a > 1)
        a = 1;
    c.a = 255 * a;
    return c;
}

static GXColor Color_Mix(GXColor a, GXColor b, float t)
{
    a.r = a.r + (b.r - a.r) * t;
    a.g = a.g + (b.g - a.g) * t;
    a.b = a.b + (b.b - a.b) * t;
    a.a = a.a + (b.a - a.a) * t;
    return a;
}

static float Ease_Out(float q)
{
    return 1.f - (1.f - q) * (1.f - q);
}

static float Clamp01(float v)
{
    return v < 0 ? 0 : v > 1 ? 1 : v;
}

static GXColor Cue_Color(int kind)
{
    static const u8 land[CUE_NUM] = {LAND_AI, LAND_PERFECT_WL, LAND_NIL};
    return land_kind_colors[land[kind]];
}

static GXColor Dim_Color(int dim)
{
    return dim == DIM_MISS ? color_miss : color_skip;
}

// How far a waveland slides from ground speed v until it stops, with the
// ground friction (ft_80084F3C, doubled above walk speed as in the
// jumpsquat). Approximate: assumes the landing keeps the dodge's sideways
// speed.
static float Slide_Distance(FighterData *fp, float v)
{
    float d = 0;
    for (int i = 0; i < 200 && v > 0; i++)
    {
        float friction = fp->attr.ground_friction;
        if (v > fp->attr.walk_maximum_velocity)
            friction *= common_run_friction;
        v = v > friction ? v - friction : 0;
        d += v;
    }
    return d;
}

// The frame a hit's timer burns down to: the first one Falcon can act on is
// the next.
static int Cue_FlashAge(Cue *e)
{
    return (e->lag > 1 ? e->lag : 1) - 1;
}

///////////////////////
/// HUD drawing     ///
///////////////////////

// HUD units are square on screen: x runs from about -29.4 to 29.4 and y
// from -24.1 (bottom) to 24.1. World points are projected onto it.
#define HUD_PX 10.85f // 640 x 480 viewport pixels per HUD unit, across
#define HUD_PY 9.96f  // ... and down
#define HUD_W 29.4f
#define HUD_H 24.1f
#define SAFE_W 27.4f // what stays on screen with any crop: TM-CE's own panels reach about 27.7
#define SAFE_H 22.f
#define PX 0.095f // about one pixel of the 640 x 480 picture

static int Hud_FromWorld(float x, float y, float *hx, float *hy)
{
    Vec3 in = {x, y, 0}, out;
    HSD_GXProject(View_CObj(), &in, &out, 1);
    *hx = (out.X - 320.f) / HUD_PX;
    *hy = (240.f - out.Y) / HUD_PY;
    return *hx > -HUD_W - 10.f && *hx < HUD_W + 10.f && *hy > -HUD_H - 10.f && *hy < HUD_H + 10.f;
}

// Shapes are queued as quads and drawn in one go, in the order queued.
#define LL_QUADS 2800
typedef struct Quad
{
    Vec2 v[4];
    GXColor c, c2; // c at the first two corners, c2 at the last two
} Quad;
static Quad *quads; // [LL_QUADS], allocated in Event_Init
static int quad_num;
static int quad_peak; // the most queued in one draw, for the log

// A quad shaded from c along its first edge to c2 along its last, as the
// soft edges use it.
static void Quad_Add2(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3, GXColor c, GXColor c2)
{
    if ((c.a == 0 && c2.a == 0) || quad_num >= LL_QUADS)
        return;
    Quad *q = &quads[quad_num++];
    q->v[0] = (Vec2){x0, y0};
    q->v[1] = (Vec2){x1, y1};
    q->v[2] = (Vec2){x2, y2};
    q->v[3] = (Vec2){x3, y3};
    q->c = Vis(c);
    q->c2 = Vis(c2);
}

static void Quad_Add(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3, GXColor c)
{
    Quad_Add2(x0, y0, x1, y1, x2, y2, x3, y3, c, c);
}

static void Quad_Flush(void)
{
    if (quad_num == 0)
        return;
    event_vars->GFX_Start(quad_num * 4, (GFX_Params){.shape = GX_QUADS});
    for (int i = 0; i < quad_num; i++)
    {
        Quad *q = &quads[i];
        for (int j = 0; j < 4; j++)
            GFX_AddVtx(q->v[j].X, q->v[j].Y, 0, j < 2 ? q->c : q->c2);
    }
    quad_num = 0;
}

static void Hud_Rect(float x0, float y0, float x1, float y1, GXColor c)
{
    Quad_Add(x0, y0, x1, y0, x1, y1, x0, y1, c);
}

// An outline just inside the box.
static void Hud_Frame(float x0, float y0, float x1, float y1, float w, GXColor c)
{
    Hud_Rect(x0, y0, x1, y0 + w, c);
    Hud_Rect(x0, y1 - w, x1, y1, c);
    Hud_Rect(x0, y0 + w, x0 + w, y1 - w, c);
    Hud_Rect(x1 - w, y0 + w, x1, y1 - w, c);
}

static void Hud_Seg(float ax, float ay, float bx, float by, float w, GXColor c)
{
    float dx = bx - ax, dy = by - ay;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.0001f)
        return;
    float nx = -dy / len * w / 2, ny = dx / len * w / 2;
    Quad_Add(ax + nx, ay + ny, bx + nx, by + ny, bx - nx, by - ny, ax - nx, ay - ny, c);
}

static void Hud_Tri(float ax, float ay, float bx, float by, float cx, float cy, GXColor c)
{
    Quad_Add(ax, ay, bx, by, cx, cy, cx, cy, c);
}

static void Hud_Diamond(float x, float y, float r, GXColor c)
{
    Quad_Add(x, y - r, x + r, y, x, y + r, x - r, y, c);
}

#define DISC_SEGS 16

// A filled circle: a fan of triangles.
static void Hud_Disc(float cx, float cy, float r, GXColor c)
{
    float px = cx + r, py = cy;
    for (int i = 1; i <= DISC_SEGS; i++)
    {
        float ang = i * (6.2831853f / DISC_SEGS);
        float nx = cx + cos(ang) * r, ny = cy + sin(ang) * r;
        Hud_Tri(cx, cy, px, py, nx, ny, c);
        px = nx;
        py = ny;
    }
}

// A circle's outline, w wide and centered on radius r.
static void Hud_Ring(float cx, float cy, float r, float w, GXColor c)
{
    float r0 = r - w / 2, r1 = r + w / 2;
    float px = 1.f, py = 0.f;
    for (int i = 1; i <= DISC_SEGS; i++)
    {
        float ang = i * (6.2831853f / DISC_SEGS);
        float nx = cos(ang), ny = sin(ang);
        Quad_Add(cx + px * r0, cy + py * r0, cx + px * r1, cy + py * r1, cx + nx * r1, cy + ny * r1, cx + nx * r0, cy + ny * r0, c);
        px = nx;
        py = ny;
    }
}

// A line along points, w wide, its sides fading out over f so it doesn't
// look jagged. Joints are mitered, so nothing overlaps (overlaps show as
// beads along a translucent line). At most LINE_MAX points.
#define LINE_MAX 64
static void Hud_Line(const float *px, const float *py, int n, int closed, float w, float f, GXColor c)
{
    if (n < 2 || n > LINE_MAX)
        return;
    float nx[LINE_MAX], ny[LINE_MAX];
    for (int i = 0; i < n; i++)
    {
        int a = i > 0 ? i - 1 : closed ? n - 1 : 0;
        int b = i < n - 1 ? i + 1 : closed ? 0 : n - 1;
        // the normals of the edges in and out, and their mean, stretched to
        // keep the line's width at the corner
        float ax = px[i] - px[a], ay = py[i] - py[a], bx = px[b] - px[i], by = py[b] - py[i];
        float la = sqrtf(ax * ax + ay * ay), lb = sqrtf(bx * bx + by * by);
        if (la < 0.0001f)
            ax = bx, ay = by, la = lb;
        if (lb < 0.0001f)
            bx = ax, by = ay, lb = la;
        if (la < 0.0001f)
            return;
        float n0x = -ay / la, n0y = ax / la, n1x = -by / lb, n1y = bx / lb;
        float mx = n0x + n1x, my = n0y + n1y, m = sqrtf(mx * mx + my * my);
        if (m < 0.0001f)
            mx = n0x, my = n0y, m = 1;
        mx /= m;
        my /= m;
        float d = mx * n0x + my * n0y;
        float k = d > 0.5f ? 1.f / d : 2.f;
        nx[i] = mx * k;
        ny[i] = my * k;
    }
    GXColor clear = {0, 0, 0, 0};
    float h = w / 2, o = h + f;
    int segs = closed ? n : n - 1;
    for (int i = 0; i < segs; i++)
    {
        int j = (i + 1) % n;
        if (w > 0)
            Quad_Add(px[i] + nx[i] * h, py[i] + ny[i] * h, px[j] + nx[j] * h, py[j] + ny[j] * h,
                     px[j] - nx[j] * h, py[j] - ny[j] * h, px[i] - nx[i] * h, py[i] - ny[i] * h, c);
        Quad_Add2(px[i] + nx[i] * h, py[i] + ny[i] * h, px[j] + nx[j] * h, py[j] + ny[j] * h,
                  px[j] + nx[j] * o, py[j] + ny[j] * o, px[i] + nx[i] * o, py[i] + ny[i] * o, c, clear);
        Quad_Add2(px[i] - nx[i] * h, py[i] - ny[i] * h, px[j] - nx[j] * h, py[j] - ny[j] * h,
                  px[j] - nx[j] * o, py[j] - ny[j] * o, px[i] - nx[i] * o, py[i] - ny[i] * o, c, clear);
    }
}

// A filled fan from (cx, cy) over the points of a closed outline.
static void Hud_Fan(float cx, float cy, const float *px, const float *py, int n, GXColor c)
{
    for (int i = 0; i < n; i++)
    {
        int j = (i + 1) % n;
        Hud_Tri(cx, cy, px[i], py[i], px[j], py[j], c);
    }
}

// A circle's points, counterclockwise from the right.
static void Circle_Points(float cx, float cy, float r, int n, float *px, float *py)
{
    for (int i = 0; i < n; i++)
    {
        float ang = i * (6.2831853f / n);
        px[i] = cx + cos(ang) * r;
        py[i] = cy + sin(ang) * r;
    }
}

// Text with its line's bottom at y, as the info panels use it (rows 2.5
// units tall), starting at x (align 0) or ending there (align 2). The HUD's
// texts are made with the alignment of their first use, so it's set again
// on every call.
static void Hud_TextAligned(const char *text, float x, float y, float size, GXColor color, int align)
{
    if (!event_vars->HUD_DrawTextEx)
        return; // a TM-CE menu from before it was set
    HUDCamData *hud = event_vars->hudcam_gobj->userdata;
    int slot = hud->text_cache_used;
    Rect r = {x, y, 0, 2.5f};
    event_vars->HUD_DrawTextEx(text, &r, size, color, (GXColor){0, 0, 0, 0}, 0, 0);
    if (slot < (int)countof(hud->text_cache) && hud->text_cache[slot])
    {
        Text *t = hud->text_cache[slot];
        t->align = align;
        t->use_aspect = 0;       // fitting to the HUD's 8-unit box squeezes long lines
        t->is_depth_compare = 0; // the stage's depth would hide it where the camera is close
    }
}

static void Hud_Text(const char *text, float x, float y, float size, GXColor color)
{
    Hud_TextAligned(text, x, y, size, color, 0);
}

// A line of text's width, for the dark plate behind it: Melee's font is
// proportional, so each glyph by its kind (about 6.5 px for an average
// glyph at size 0.45, as measured in game).
static float Text_Width(const char *text, float size)
{
    float w = 0;
    for (const char *c = text; *c; c++)
    {
        char ch = *c;
        float g;
        if (ch == ' ')
            g = 0.9f;
        else if (strchr("iIl1.,:;'!|", ch))
            g = 0.7f;
        else if (strchr("fjrt()[]-/", ch))
            g = 1.0f;
        else if (ch == 'm' || ch == 'w')
            g = 1.9f;
        else if (ch == 'M' || ch == 'W')
            g = 2.0f;
        else if (ch >= 'A' && ch <= 'Z')
            g = 1.6f;
        else if (ch >= '0' && ch <= '9')
            g = 1.35f;
        else if (ch >= 'a' && ch <= 'z')
            g = 1.3f;
        else
            g = 1.4f;
        w += g;
    }
    return w * size;
}

// The dark plate behind a line of text whose row starts at y (rows 2.5
// tall), from x0 to x1: solid enough to read over a light stage.
static void Text_Plate(float x0, float x1, float y)
{
    Hud_Rect(x0, y + 0.15f, x1, y + 2.35f, Color_Over(color_plate, 0.72f));
}

///////////////////////
/// The meter       ///
///////////////////////

#define MT_CELLS 40
#define MT_ROWS 5
#define MT_PITCH 0.622f // 35% bigger than the mockup's
#define MT_CW 0.498f
#define MT_CH 0.949f
#define MT_GAP 0.271f
#define MT_WIDE 0.074f   // Wide Cells: a fifth of the mockup's cell width
#define MT_FIXED 1.5f    // the fixed strip's scale
#define MT_FIXED_MAX 20  // ... and its most cells, leaving room for the stick

enum cell_kind
{
    CELL_NONE,
    CELL_PRESS, // an input: solid
    CELL_ACT,   // the first frame Falcon can act: solid, white cap
    CELL_AIR,   // in the air between inputs: a light dash
    CELL_LAND,  // touchdown: hollow, with a floor bar
    CELL_LAG,   // landing lag: a low bar
    CELL_TAIL,  // ledge intangibility left: periwinkle
};

enum cell_tone
{
    TONE_CUE,
    TONE_MISS,
    TONE_SKIP,
};

enum glyph_kind
{
    GLYPH_NONE,
    GLYPH_LEFT,   // stick left
    GLYPH_RIGHT,  // stick right
    GLYPH_DOWN,   // stick down
    GLYPH_JUMP,   // jump
    GLYPH_AERIAL, // an aerial
    GLYPH_DODGE,  // airdodge
    GLYPH_UP,     // stick (or C-stick) up
    GLYPH_DOWN_LEFT,
    GLYPH_DOWN_RIGHT,
    GLYPH_FF,            // fastfall: a double chevron, where a plain hold down is one solid arrow
    GLYPH_FF_DOWN_LEFT,
    GLYPH_FF_DOWN_RIGHT,
};

typedef struct MeterRow
{
    u8 cell[MT_CELLS];
    u8 tone[MT_CELLS];
    u8 alpha[MT_CELLS];
    u8 glyph[MT_CELLS];
    GXColor color;
    int len;      // cells used: the last one + 1
    u8 hot;       // a press is at the gate in its window: 1, 2 on the window's first or last frame
    u8 spent;     // frames of that window already gone: the cells show what's left, the dial the whole wedge
    u8 dim;       // a route that isn't the chosen one
    s8 ghost[4];  // ages of the ghosts spreading out of the gate, -1 none
    u8 ghost_bright[4];
    s8 burst;     // frames since a hit, -1 none
    s8 implode;   // frames since a miss or skip folded in, -1 none
    u8 implode_tone;
    s8 flash;     // frames since the gate reached the first frame Falcon can act, -1 none
    float fade;   // Auto Fade: its cells' opacity scaled by this
    char label[8];
    char info[24];
} MeterRow;

static MeterRow meter[MT_ROWS];
static int meter_rows;

static MeterRow *Meter_Add(GXColor color, const char *label)
{
    if (meter_rows >= MT_ROWS)
        return 0;
    MeterRow *r = &meter[meter_rows++];
    memset(r, 0, sizeof(*r));
    r->color = color;
    r->fade = 1.f;
    r->burst = -1;
    r->implode = -1;
    r->flash = -1;
    for (int i = 0; i < 4; i++)
        r->ghost[i] = -1;
    sprintf(r->label, "%s", label);
    return r;
}

static void Row_Set(MeterRow *r, int k, int kind, int tone, float alpha)
{
    if (k < 0 || k >= MT_CELLS)
        return;
    r->cell[k] = kind;
    r->tone[k] = tone;
    r->alpha[k] = 255 * Clamp01(alpha * r->fade);
    if (k + 1 > r->len)
        r->len = k + 1;
}

static void Row_Ghost(MeterRow *r, int age, int bright)
{
    for (int i = 0; i < 4; i++)
    {
        if (r->ghost[i] < 0)
        {
            r->ghost[i] = age;
            r->ghost_bright[i] = bright;
            return;
        }
    }
}

// The one ghost a timer sends out: a strong one on a press that works (or
// on the hit, when there was no press), else a faint one on the window's
// last frame, which a dry run can time against. An early or late press
// sends none, so it can't be mistaken for either. Returns its age, or -1.
static int Cue_Ghost(Cue *c, int *bright)
{
    if (c->press >= 0)
    {
        *bright = 1;
        return c->press < LL_GHOST ? c->press : -1;
    }
    *bright = 0;
    if (c->dim == DIM_MISS || c->pulse < 0)
        return -1;
    int g = c->pulse - (c->width - 1);
    return g >= 0 && g < LL_GHOST ? g : -1;
}

static void Row_Ghosts(MeterRow *r, Cue *c)
{
    int bright, g = Cue_Ghost(c, &bright);
    if (g >= 0)
        Row_Ghost(r, g, bright);
    if (c->press >= 0 && c->press < LL_BURST)
        r->burst = c->press; // the burst goes with the hit's ghost
}

// The ledge intangibility tail after the first frame Falcon can act (act,
// in cells from the gate).
static void Row_Tail(MeterRow *r, int act)
{
    for (int k = act + 1 > 0 ? act + 1 : 0; k < galint_now - 1; k++)
        Row_Set(r, k, CELL_TAIL, TONE_CUE, 0.85f);
}

static const char *cue_labels[CUE_NUM] = {"AI", "WL", "NIL"};

// One row per kind: its countdown, and the ending of the last one.
static void Meter_FromCue(int kind)
{
    Cue *c = &cue_live[kind], *e = &cue_end[kind];
    int live = c->phase != PH_OFF;
    if (!live && e->phase == PH_OFF)
        return;
    int wd = kind == CUE_WL && (live ? c->wd : e->wd);
    if (wd && Options_Timers[TOPT_WD].val != WDT_CELLS)
        return; // drawn by Falcon's feet, or not at all
    MeterRow *r = Meter_Add(Cue_Color(kind), wd ? "WD" : cue_labels[kind]);
    if (!r)
        return;
    r->fade = Fade_Factor(VG_TIMERS, kind);

    switch (e->phase)
    {
    case PH_HIT:
    {
        int act = Cue_FlashAge(e) - e->age;
        for (int k = 0; k < act; k++)
            Row_Set(r, k, CELL_LAG, TONE_CUE, 0.7f);
        if (act >= 0)
            Row_Set(r, act, CELL_ACT, TONE_CUE, 1);
        Row_Tail(r, act);
        if (act <= 0 && -act < 4)
            r->flash = -act;
        Row_Ghosts(r, e);
        if (galint_now > 1 && act < 0)
            sprintf(r->info, "%d GALINT", galint_now - 1);
        else if (act > 0)
            sprintf(r->info, "lag %d", act);
        else
            sprintf(r->info, "act");
        break;
    }
    case PH_FADE:
        r->implode = e->age;
        r->implode_tone = e->dim == DIM_MISS ? TONE_MISS : TONE_SKIP;
        Row_Ghosts(r, e); // a skip's faint ghost plays out
        sprintf(r->info, e->dim == DIM_MISS ? "miss" : "skip");
        break;
    case PH_CUT:
    {
        float a = 0.5f * (1.f - (float)e->age / LL_CUT);
        for (int j = 0; j < e->width; j++)
            Row_Set(r, e->left - 1 + j, CELL_PRESS, TONE_SKIP, a);
        break;
    }
    }

    if (!live)
        return;
    int tone = c->dim == DIM_MISS ? TONE_MISS : c->dim ? TONE_SKIP : TONE_CUE;
    float a = c->phase == PH_COUNT ? Clamp01((c->span - c->left + 1) / 2.f) : 1.f; // fades in over 2 frames
    if (kind == CUE_NIL)
    {
        Row_Set(r, c->left - 2, CELL_LAND, tone, a);
        Row_Set(r, c->left - 1, CELL_ACT, tone, a);
        sprintf(r->info, "%df", c->left - 1);
    }
    else if (c->phase == PH_COUNT)
    {
        for (int j = 0; j < c->width; j++)
            Row_Set(r, c->left - 1 + j, CELL_PRESS, tone, a);
        sprintf(r->info, "%df", c->left - 1);
    }
    else
    {
        for (int j = c->age; j < c->width; j++)
            Row_Set(r, j - c->age, CELL_PRESS, tone, 1);
        r->spent = c->age < c->width ? c->age : c->width;
        if (!c->dim)
            r->hot = c->age == 0 || c->age == c->width - 1 ? 2 : 1;
        Row_Ghosts(r, c);
        sprintf(r->info, c->dim ? "miss" : "now");
    }
}

static int route_rows_active; // ledge routes fill the meter; the cues' rows give way
static int drop_sid; // Falcon's state, for the drop drill's row
static void Meter_AddDrop(int sid);
static int route_dj_done;     // ... and its double jump happened
static void Meter_AddRoutes(void);
static void Meter_AddJump(void);

static void Meter_Build(void)
{
    meter_rows = 0;
    route_rows_active = 0;
    Meter_AddRoutes();
    Meter_AddJump();
    Meter_AddDrop(drop_sid);
    if (route_rows_active)
        return; // a ledge route has the meter to itself
    if (Cues_Ai())
        Meter_FromCue(CUE_AI);
    if (Cues_Waveland())
        Meter_FromCue(CUE_WL);
    if (Cues_Nil())
        Meter_FromCue(CUE_NIL);
}

static GXColor Tone_Color(MeterRow *r, int tone)
{
    return tone == TONE_MISS ? color_miss : tone == TONE_SKIP ? color_skip : r->color;
}

// Two chevrons stacked along (dx, dy), like speed lines: a fastfall.
static void Glyph_Chevrons(float x, float y, float r, float dx, float dy, float w, GXColor c)
{
    float px = -dy, py = dx;
    for (int i = 0; i < 2; i++)
    {
        float t = i ? r * 0.95f : r * 0.05f; // the tips, along the way down
        float tx = x + dx * t, ty = y + dy * t;
        float bx = tx - dx * r * 0.85f, by = ty - dy * r * 0.85f; // narrow, so turned 45 degrees it isn't an L
        Hud_Seg(bx + px * r * 0.62f, by + py * r * 0.62f, tx, ty, w, c);
        Hud_Seg(bx - px * r * 0.62f, by - py * r * 0.62f, tx, ty, w, c);
    }
}

static void Glyph_Draw(int glyph, float x, float y, float s, GXColor c)
{
    GXColor shade = Color_Fill(color_plate, c.a / 255.f * 0.9f);
    for (int pass = 0; pass < 2; pass++)
    {
        float r = pass ? s : s + 0.12f;
        GXColor col = pass ? c : shade;
        switch (glyph)
        {
        case GLYPH_LEFT:
            Hud_Tri(x + r * 0.8f, y + r, x + r * 0.8f, y - r, x - r, y, col);
            break;
        case GLYPH_RIGHT:
            Hud_Tri(x - r * 0.8f, y - r, x - r * 0.8f, y + r, x + r, y, col);
            break;
        case GLYPH_DOWN:
            Hud_Tri(x - r, y + r * 0.7f, x + r, y + r * 0.7f, x, y - r, col);
            break;
        case GLYPH_UP:
            Hud_Tri(x + r, y - r * 0.7f, x - r, y - r * 0.7f, x, y + r, col);
            break;
        case GLYPH_DOWN_LEFT:
        case GLYPH_DOWN_RIGHT:
        {
            // the down arrow turned 45 degrees
            float ux = (glyph == GLYPH_DOWN_RIGHT ? 0.707f : -0.707f), uy = -0.707f;
            float bx = x - ux * r * 0.7f, by = y - uy * r * 0.7f;
            Hud_Tri(bx + uy * r, by - ux * r, bx - uy * r, by + ux * r, x + ux * r, y + uy * r, col);
            break;
        }
        case GLYPH_JUMP:
            // a button with an up arrow cut into it
            Hud_Disc(x, y, r, col);
            if (pass)
                Hud_Tri(x + s * 0.55f, y - s * 0.3f, x - s * 0.55f, y - s * 0.3f, x, y + s * 0.5f, shade);
            break;
        case GLYPH_AERIAL:
            Hud_Disc(x, y, r, col);
            break;
        case GLYPH_DODGE:
            // a shield bubble
            Hud_Ring(x, y, s * 0.75f, (pass ? 0.45f : 0.45f + 0.24f / s) * s, col);
            break;
        case GLYPH_FF:
        case GLYPH_FF_DOWN_LEFT:
        case GLYPH_FF_DOWN_RIGHT:
        {
            float dx = glyph == GLYPH_FF ? 0 : glyph == GLYPH_FF_DOWN_RIGHT ? 0.707f : -0.707f;
            float dy = glyph == GLYPH_FF ? -1.f : -0.707f;
            float w = 0.3f * s + (pass ? 0 : 0.16f);
            Glyph_Chevrons(x - dx * s * 0.1f, y - dy * s * 0.1f, s, dx, dy, w, col); // centered
            break;
        }
        }
    }
}

// The meter with its gate's left edge at gx and its bottom at base. scale
// is 1 above Falcon and MT_FIXED for the fixed strip, which also gets
// labels. Returns nothing drawn when there are no rows.
static void Meter_Draw(float gx, float base, float scale, int max_cells, int labels)
{
    int wide = Options_Timers[TOPT_WIDE].val;
    float pitch = (MT_PITCH + (wide ? MT_WIDE : 0)) * scale;
    float cw = (MT_CW + (wide ? MT_WIDE : 0)) * scale;
    float ch = MT_CH * scale, gap = MT_GAP * scale;
    int rows = meter_rows;

    int len = 1, glyphs = 0;
    for (int i = 0; i < rows; i++)
    {
        if (meter[i].len > len)
            len = meter[i].len;
        for (int k = 0; k < meter[i].len; k++)
            glyphs |= meter[i].glyph[k];
    }
    if (len > max_cells)
        len = max_cells;

    float top = base + rows * (ch + gap) - gap;
    float glyph_h = glyphs ? 1.1f * scale : 0;
    float x1 = gx + (len - 1) * pitch + cw;

    // the plate, so it reads over any stage
    Hud_Rect(gx - 0.32f * scale, base - 0.22f * scale, x1 + 0.22f * scale, top + 0.22f * scale + glyph_h, Color_Fill(color_plate, 0.72f));

    for (int i = 0; i < rows; i++)
    {
        MeterRow *r = &meter[i];
        float y0 = base + (rows - 1 - i) * (ch + gap), y1 = y0 + ch;
        float dim = r->dim ? 0.5f : 1.f;

        // empty frames
        for (int k = 1; k < len; k++)
        {
            if (r->cell[k] == CELL_NONE)
                Hud_Rect(gx + k * pitch, y0, gx + k * pitch + cw, y1, Color_Fill(color_white, 0.1f * dim));
        }

        for (int k = 0; k < r->len && k < len; k++)
        {
            int kind = r->cell[k];
            if (kind == CELL_NONE)
                continue;
            float a = r->alpha[k] / 255.f * dim;
            GXColor col = Tone_Color(r, r->tone[k]);
            float cx0 = gx + k * pitch, cx1 = cx0 + cw;
            switch (kind)
            {
            case CELL_PRESS:
                Hud_Rect(cx0, y0, cx1, y1, Color_Fill(col, a));
                break;
            case CELL_ACT:
                Hud_Rect(cx0, y0, cx1, y1, Color_Fill(col, a));
                Hud_Rect(cx0, y1 - 0.2f * ch, cx1, y1, Color_Fill(color_white, a * 0.9f));
                break;
            case CELL_AIR:
                Hud_Rect(cx0, y0 + ch * 0.38f, cx1, y0 + ch * 0.62f, Color_Fill(col, a * 0.5f));
                break;
            case CELL_LAND:
            {
                float w = 1.4f * PX * scale;
                Hud_Rect(cx0, y0, cx1, y1, Color_Fill(color_plate, a * 0.6f));
                Hud_Frame(cx0, y0, cx1, y1, w, Color_Fill(col, a));
                Hud_Rect(cx0, y0, cx1, y0 + ch * 0.24f, Color_Fill(col, a));
                break;
            }
            case CELL_LAG:
                Hud_Rect(cx0, y0, cx1, y0 + ch * 0.55f, Color_Fill(col, a * 0.75f));
                break;
            case CELL_TAIL:
                Hud_Rect(cx0, y0, cx1, y1, Color_Fill(color_galint, a));
                break;
            }
            if (r->glyph[k] && i == 0)
                Glyph_Draw(r->glyph[k], (cx0 + cx1) / 2, top + 0.22f * scale + glyph_h / 2, 0.36f * scale, Color_Fill(color_white, 0.95f * dim));
        }
    }

    // the gate: two posts, lit while a press is due
    float gy0 = base - 0.3f * scale, gy1 = top + 0.3f * scale;
    float gcx = gx + cw / 2, gcy = (base + top) / 2;
    float hot = 0;
    GXColor hot_col = color_white;
    for (int i = 0; i < rows; i++)
    {
        MeterRow *r = &meter[i];
        if (r->hot && !r->dim && r->hot > hot)
        {
            hot = r->hot;
            hot_col = r->color;
        }
        if (r->flash >= 0 && !r->dim)
        {
            hot = 2;
            hot_col = r->color;
        }
    }
    if (hot > 0)
    {
        GXColor fill = Color_Mix(hot_col, color_white, hot >= 2 ? 1.f : 0.4f);
        Hud_Rect(gx - 0.35f * scale, gy0 - 0.2f * scale, gx + cw + 0.35f * scale, gy1 + 0.2f * scale, Color_Fill(hot_col, 0.3f));
        Hud_Rect(gx - 0.1f * scale, gy0, gx + cw + 0.1f * scale, gy1, fill);
        if (hot >= 2)
        {
            // the press frame: a white flash past the plate, and two ticks
            Hud_Rect(gx - 0.2f * scale, gy0 - 0.45f * scale, gx + cw + 0.2f * scale, gy1 + 0.45f * scale, color_white);
            Hud_Rect(gx - 1.0f * scale, gcy - 0.06f * scale, gx - 0.4f * scale, gcy + 0.06f * scale, Color_Fill(color_white, 0.9f));
            Hud_Rect(gx + cw + 0.4f * scale, gcy - 0.06f * scale, gx + cw + 1.0f * scale, gcy + 0.06f * scale, Color_Fill(color_white, 0.9f));
        }
    }
    else
    {
        GXColor post = Color_Fill(color_white, 0.55f);
        Hud_Rect(gx - 0.16f * scale, gy0, gx - 0.06f * scale, gy1, post);
        Hud_Rect(gx + cw + 0.06f * scale, gy0, gx + cw + 0.16f * scale, gy1, post);
    }

    // endings at the gate
    for (int i = 0; i < rows; i++)
    {
        MeterRow *r = &meter[i];
        if (r->dim)
            continue;
        for (int g = 0; g < 4; g++)
        {
            if (r->ghost[g] < 0)
                continue;
            // a filled copy of the gate's cell, in the row's color, that
            // swells and fades fast
            float q = (float)r->ghost[g] / LL_GHOST, fade = (1.f - q) * (1.f - q);
            int b = r->ghost_bright[g];
            float grow = (b ? 0.1f + 1.2f * Ease_Out(q) : 0.05f + 0.55f * Ease_Out(q)) * scale;
            GXColor tint = Color_Mix(r->color, color_white, b ? 0.35f : 0.2f);
            Hud_Rect(gx - grow, gy0 - grow, gx + cw + grow, gy1 + grow, Color_Over(tint, (b ? 0.55f : 0.22f) * fade));
            Hud_Frame(gx - grow, gy0 - grow, gx + cw + grow, gy1 + grow, 1.6f * PX * scale, Color_Over(tint, (b ? 0.95f : 0.45f) * fade));
        }
        if (r->burst >= 0)
        {
            float q = (float)r->burst / LL_BURST;
            float rad = (0.6f + 2.0f * Ease_Out(q)) * scale;
            GXColor bc = Color_Fill(r->color, 1.f - q);
            for (int ray = 0; ray < 6; ray++)
            {
                float ang = ray * 1.0471976f + 0.5235988f;
                float dx = cos(ang), dy = sin(ang);
                Hud_Seg(gcx + dx * rad * 0.55f, gcy + dy * rad * 0.55f, gcx + dx * rad, gcy + dy * rad, 0.2f * scale, bc);
            }
        }
        if (r->implode >= 0)
        {
            float q = (float)r->implode / LL_FADE;
            GXColor dc = r->implode_tone == TONE_MISS ? color_miss : color_skip;
            if (r->implode == 0)
                Hud_Rect(gx - 0.1f * scale, gy0, gx + cw + 0.1f * scale, gy1, dc); // the frame that was needed
            float k = 1.f - Ease_Out(q);
            float hw = (cw / 2 + 0.9f * scale) * k, hh = ((gy1 - gy0) / 2 + 0.6f * scale) * k;
            if (hw > 0.05f)
                Hud_Frame(gcx - hw, gcy - hh, gcx + hw, gcy + hh, 1.4f * PX * scale, Color_Fill(dc, 0.6f * (1.f - q)));
        }
    }

    if (!labels)
        return;
    for (int i = 0; i < rows; i++)
    {
        MeterRow *r = &meter[i];
        float y0 = base + (rows - 1 - i) * (ch + gap);
        float ty = y0 + ch / 2 - 1.25f;
        GXColor tc = Color_Mix(r->color, color_white, 0.35f);
        if (r->dim)
            tc = Color_Fill(tc, 0.6f);
        tc.a = 255;
        Text_Plate(gx - 3.4f, gx - 3.2f + Text_Width(r->label, 0.42f) + 0.2f, ty);
        Hud_Text(r->label, gx - 3.2f, ty, 0.42f, tc);
        if (r->info[0])
        {
            float ix = x1 + 0.5f * scale;
            Text_Plate(ix - 0.2f, ix + Text_Width(r->info, 0.42f) + 0.2f, ty);
            Hud_Text(r->info, ix, ty, 0.42f, (GXColor){220, 220, 220, 255});
        }
    }
}

// Near Falcon: the meter is pinned to one spot on the screen when it shows
// up, so the eye doesn't have to chase it: over his head if his path stays
// below it, else above the whole path, or beside it. It moves only when the
// path, the controller display, the info panel or the fixed strip would run
// into it, and then glides to the next clear spot. Between two timers close
// together it stays where it was.
typedef struct Box
{
    float x0, y0, x1, y1;
} Box;

// the ledge route's path (Ledge Practice, defined below)
static Vec2 *route_path;
static float *route_path_bottom;
static int route_path_num;
static int hang_ledge;
static int route_show_num;

#define PIN_BOXES 24
#define PIN_HOLD 30 // frames the spot is kept after the meter goes away
static Box pin_path[PIN_BOXES]; // Falcon now and along his path, on the screen
static int pin_path_num;
static float pin_x, pin_y;      // where the meter is: its gate's left and its bottom
static float pin_tx, pin_ty;    // where it's going
static int pin_valid;
static int pin_idle;            // frames without a meter

static int Box_Hit(Box *a, Box *b)
{
    return a->x0 < b->x1 && b->x0 < a->x1 && a->y0 < b->y1 && b->y0 < a->y1;
}

// Falcon's body from y0 to y1 at x, as a box on the screen, with a margin.
static void Pin_AddBody(float x, float y0, float y1)
{
    if (pin_path_num >= PIN_BOXES)
        return;
    float ax, ay, bx, by;
    if (!Hud_FromWorld(x - 5.f, y0, &ax, &ay) || !Hud_FromWorld(x + 5.f, y1, &bx, &by))
        return;
    Box *b = &pin_path[pin_path_num++];
    b->x0 = (ax < bx ? ax : bx) - 0.4f;
    b->x1 = (ax < bx ? bx : ax) + 0.4f;
    b->y0 = (ay < by ? ay : by) - 0.4f;
    b->y1 = (ay < by ? by : ay) + 0.4f;
}

static void Pin_Path(FighterData *fp)
{
    pin_path_num = 0;
    Pin_AddBody(fp->phys.pos.X, fp->phys.pos.Y - 1.f, fp->phys.pos.Y + 20.f);
    if (live_visible)
    {
        Prediction *p = pred_live;
        int last = p->land_frame ? p->land_frame : p->num;
        int step = last / (PIN_BOXES - 2) + 1;
        for (int k = 1; k <= last; k += step)
            Pin_AddBody(p->pos[k].X, p->pos[k].Y + p->bottom[k], p->pos[k].Y + p->top[k] + 2.f);
        if (last > 0)
            Pin_AddBody(p->pos[last].X, p->pos[last].Y + p->bottom[last], p->pos[last].Y + p->top[last] + 2.f);
    }
    else if (route_path_num >= 2 && ((hang_ledge >= 0 && route_show_num > 0) || route_active))
    {
        int step = route_path_num / (PIN_BOXES - 2) + 1;
        for (int k = 0; k < route_path_num; k += step)
            Pin_AddBody(route_path[k].X, route_path[k].Y + route_path_bottom[k], route_path[k].Y + route_path_bottom[k] + 18.f);
    }
}

static void Pad_Box(FighterData *fp, float *x0, float *y0, float *x1, float *y1); // with the controller display

// The fixed strip's other looks sit in a bottom corner, the one away from
// the controller display. The highway is a lane a row, HW_ROWS frames tall
// with the hit line at the bottom; the dial is a small dial a row, side by
// side. Both leave room under for the row's label, and the first lane for
// the glyphs of a route's inputs.
#define HW_ROWS 18       // frames of lead a lane shows
#define HW_PITCH 0.68f   // one frame's height
#define HW_LANE 2.1f
#define HW_LANE_GAP 0.5f
#define HW_GLYPH 1.5f
#define HW_LABEL 1.7f
#define DL_R 1.55f       // a dial's radius
#define DL_CELL 4.2f     // the room one takes, side to side
#define DL_LABEL 1.7f
#define STRIP_PAD 0.35f

static int Strip_Glyphs(void)
{
    if (meter_rows == 0)
        return 0;
    for (int k = 0; k < meter[0].len && k < HW_ROWS; k++)
    {
        if (meter[0].glyph[k])
            return 1;
    }
    return 0;
}

// Where the highway or the dial is: its left, bottom, width and height.
static void Strip_Place(FighterData *fp, int look, float *x, float *y, float *w, float *h)
{
    int n = meter_rows > 0 ? meter_rows : 1;
    if (look == STRIP_HIGHWAY)
    {
        *w = 2 * STRIP_PAD + (Strip_Glyphs() ? HW_GLYPH : 0) + n * HW_LANE + (n - 1) * HW_LANE_GAP;
        *h = HW_LABEL + HW_ROWS * HW_PITCH + 0.5f;
    }
    else
    {
        *w = 2 * STRIP_PAD + n * DL_CELL - 0.4f;
        *h = DL_LABEL + 2 * DL_R + 0.9f;
    }
    float x0, y0, x1, y1;
    Pad_Box(fp, &x0, &y0, &x1, &y1);
    *x = x0 + x1 < 0 ? SAFE_W - *w : -SAFE_W;
    *y = -SAFE_H + 0.4f;
}

// The fixed strip's box, so the near-Falcon strip keeps out of it.
static Box Strip_Box(FighterData *fp)
{
    int look = Options_Timers[TOPT_STRIP].val;
    if (look == STRIP_HIGHWAY || look == STRIP_DIAL)
    {
        float x, y, w, h;
        Strip_Place(fp, look, &x, &y, &w, &h);
        return (Box){x - 0.2f, -SAFE_H, x + w + 0.2f, y + h + 0.3f};
    }
    return (Box){-SAFE_W, -SAFE_H, SAFE_W, -SAFE_H + 0.4f + meter_rows * (MT_CH + MT_GAP) * MT_FIXED + 0.6f};
}

// Whether the meter at (gx, base), w by h, is clear of everything else.
static int Pin_Clear(FighterData *fp, float gx, float base, float w, float h)
{
    Box m = {gx - 0.4f, base - 0.3f, gx + w + 0.3f, base + h};
    if (m.x0 < -SAFE_W - 0.01f || m.x1 > SAFE_W + 0.01f || m.y0 < -SAFE_H - 0.01f || m.y1 > SAFE_H + 0.01f)
        return 0;
    for (int i = 0; i < pin_path_num; i++)
    {
        if (Box_Hit(&m, &pin_path[i]))
            return 0;
    }
    if (Options_Hud[HOPT_STICK].val != STICK_OFF)
    {
        Box pad;
        Pad_Box(fp, &pad.x0, &pad.y0, &pad.x1, &pad.y1);
        if (Box_Hit(&m, &pad))
            return 0;
    }
    if (Options_Hud[HOPT_PANEL].val)
    {
        // the info panel's lines in its top corner
        float low = SAFE_H - (Options_Game[GOPT_FRAME_ADV].val ? 10.5f : 8.f); // the frame line under it
        Box panel = {panel_left ? -SAFE_W : SAFE_W - 22.f, low, panel_left ? -SAFE_W + 22.f : SAFE_W, SAFE_H};
        if (Box_Hit(&m, &panel))
            return 0;
    }
    if (Options_Timers[TOPT_STRIP].val != STRIP_OFF)
    {
        // the fixed strip along the bottom, or in its corner
        Box strip = Strip_Box(fp);
        if (Box_Hit(&m, &strip))
            return 0;
    }
    return 1;
}

static void Pin_Clamp(float *gx, float *base, float w, float h)
{
    if (*gx > SAFE_W - w - 0.3f)
        *gx = SAFE_W - w - 0.3f;
    if (*gx < -SAFE_W + 0.4f)
        *gx = -SAFE_W + 0.4f;
    if (*base > SAFE_H - h)
        *base = SAFE_H - h;
    if (*base < -SAFE_H + 0.3f)
        *base = -SAFE_H + 0.3f;
}

// The first clear spot: over his head, over the whole path, then beside
// it on either side, nearest first. None clear: over the path anyway.
static void Pin_Choose(FighterData *fp, float head_x, float head_y, float cw, float w, float h, float *gx, float *base)
{
    Box u = pin_path[0];
    for (int i = 1; i < pin_path_num; i++)
    {
        Box *b = &pin_path[i];
        u.x0 = b->x0 < u.x0 ? b->x0 : u.x0;
        u.y0 = b->y0 < u.y0 ? b->y0 : u.y0;
        u.x1 = b->x1 > u.x1 ? b->x1 : u.x1;
        u.y1 = b->y1 > u.y1 ? b->y1 : u.y1;
    }
    float over = pin_path_num > 0 ? pin_path[0].y1 + 0.1f : head_y + 0.6f; // just over his head now
    float cand[6][2] = {
        {head_x - cw / 2, over},
        {head_x - cw / 2, u.y1 + 0.3f},
        {u.x0 - w - 0.6f, head_y - h / 2},
        {u.x1 + 0.8f, head_y - h / 2},
        {u.x0 - w - 0.6f, u.y1 - h},
        {u.x1 + 0.8f, u.y1 - h},
    };
    // the side nearer the middle of the screen first
    if (head_x < 0)
    {
        for (int i = 2; i < 6; i += 2)
        {
            float tx = cand[i][0], ty = cand[i][1];
            cand[i][0] = cand[i + 1][0];
            cand[i][1] = cand[i + 1][1];
            cand[i + 1][0] = tx;
            cand[i + 1][1] = ty;
        }
    }
    for (int i = 0; i < 6; i++)
    {
        float x = cand[i][0], y = cand[i][1];
        Pin_Clamp(&x, &y, w, h);
        if (Pin_Clear(fp, x, y, w, h))
        {
            *gx = x;
            *base = y;
            return;
        }
    }
    *gx = cand[1][0];
    *base = cand[1][1];
    Pin_Clamp(gx, base, w, h);
}

// Each frame without a meter.
static void Pin_Idle(void)
{
    if (pin_valid && ++pin_idle > PIN_HOLD)
        pin_valid = 0;
}

static void Meter_Above(FighterData *fp)
{
    float head_x, head_y;
    Hud_FromWorld(fp->phys.pos.X, fp->phys.pos.Y + 18.f, &head_x, &head_y);

    int wide = Options_Timers[TOPT_WIDE].val;
    float pitch = MT_PITCH + (wide ? MT_WIDE : 0);
    float cw = MT_CW + (wide ? MT_WIDE : 0);
    int len = 1;
    for (int i = 0; i < meter_rows; i++)
        if (meter[i].len > len)
            len = meter[i].len;
    if (len > 24)
        len = 24;
    float w = len * pitch, h = meter_rows * (MT_CH + MT_GAP) + 1.5f;

    Pin_Path(fp);
    pin_idle = 0;
    if (!pin_valid)
    {
        Pin_Choose(fp, head_x, head_y, cw, w, h, &pin_tx, &pin_ty);
        pin_x = pin_tx;
        pin_y = pin_ty;
        pin_valid = 1;
    }
    else
    {
        float x = pin_tx, y = pin_ty;
        Pin_Clamp(&x, &y, w, h);
        if (!Pin_Clear(fp, x, y, w, h))
            Pin_Choose(fp, head_x, head_y, cw, w, h, &pin_tx, &pin_ty);
        else
        {
            pin_tx = x;
            pin_ty = y;
        }
    }
    // glide to it
    pin_x += (pin_tx - pin_x) * 0.3f;
    pin_y += (pin_ty - pin_y) * 0.3f;
    Meter_Draw(pin_x, pin_y, 1.f, len, 0);
}

// A row's label centered at cx, its line's middle at y.
static void Strip_Label(MeterRow *r, float cx, float y)
{
    GXColor tc = Color_Mix(r->color, color_white, 0.35f);
    if (r->dim)
        tc = Color_Fill(tc, 0.6f);
    tc.a = 255;
    float w = Text_Width(r->label, 0.42f);
    Text_Plate(cx - w / 2 - 0.2f, cx + w / 2 + 0.2f, y - 1.25f);
    Hud_Text(r->label, cx - w / 2, y - 1.25f, 0.42f, tc);
}

// The dial's wedge, ring and hand are measured in degrees clockwise from
// twelve o'clock; a frame is DL_DEG of them.
#define DL_DEG 15.f

static void Dial_Pt(float cx, float cy, float r, float deg, float *x, float *y)
{
    float a = deg * 0.0174533f;
    *x = cx + sin(a) * r;
    *y = cy + cos(a) * r;
}

// A pie slice from one angle to the next (a whole disc for 360), in steps
// of at most 30 degrees.
static void Dial_Fan(float cx, float cy, float r, float from, float to, GXColor c)
{
    int n = (int)((to - from) / 30.f + 0.999f);
    if (n < 1)
        return;
    float step = (to - from) / n, px, py, nx, ny;
    Dial_Pt(cx, cy, r, from, &px, &py);
    for (int i = 1; i <= n; i++)
    {
        Dial_Pt(cx, cy, r, from + step * i, &nx, &ny);
        Hud_Tri(cx, cy, px, py, nx, ny, c);
        px = nx;
        py = ny;
    }
}

// An arc band between two radii.
static void Dial_Band(float cx, float cy, float r0, float r1, float from, float to, GXColor c)
{
    int n = (int)((to - from) / 30.f + 0.999f);
    if (n < 1)
        return;
    float step = (to - from) / n, ix, iy, ox, oy, jx, jy, kx, ky;
    Dial_Pt(cx, cy, r0, from, &ix, &iy);
    Dial_Pt(cx, cy, r1, from, &ox, &oy);
    for (int i = 1; i <= n; i++)
    {
        Dial_Pt(cx, cy, r0, from + step * i, &jx, &jy);
        Dial_Pt(cx, cy, r1, from + step * i, &kx, &ky);
        Quad_Add(ix, iy, ox, oy, kx, ky, jx, jy, c);
        ix = jx;
        iy = jy;
        ox = kx;
        oy = ky;
    }
}

// The ending a row plays at its gate, shared by the highway and the dial:
// the ghost's swell (a filled copy of the gate that grows and fades, strong
// on a press that worked), the hit's burst and the fold of a miss or skip.
// (cx, cy) is the gate's middle, hw by hh its half size (a round gate uses
// hw), s a size to scale the effects by.
static void Strip_Ending(MeterRow *r, float cx, float cy, float hw, float hh, float s, int round)
{
    for (int g = 0; g < 4; g++)
    {
        if (r->ghost[g] < 0)
            continue;
        float q = (float)r->ghost[g] / LL_GHOST, fade = (1.f - q) * (1.f - q);
        int b = r->ghost_bright[g];
        float grow = (b ? 0.1f + 1.0f * Ease_Out(q) : 0.05f + 0.45f * Ease_Out(q)) * s;
        GXColor tint = Color_Mix(r->color, color_white, b ? 0.35f : 0.2f);
        if (round)
            Dial_Fan(cx, cy, hw + grow, 0, 360, Color_Over(tint, (b ? 0.5f : 0.2f) * fade));
        else
        {
            Hud_Rect(cx - hw - grow, cy - hh - grow, cx + hw + grow, cy + hh + grow, Color_Over(tint, (b ? 0.55f : 0.22f) * fade));
            Hud_Frame(cx - hw - grow, cy - hh - grow, cx + hw + grow, cy + hh + grow, 1.6f * PX, Color_Over(tint, (b ? 0.95f : 0.45f) * fade));
        }
    }
    if (r->burst >= 0)
    {
        float q = (float)r->burst / LL_BURST;
        float rad = (round ? hw + 0.3f * s : 0.5f * s) + 1.6f * s * Ease_Out(q);
        float from = rad * 0.55f;
        if (round)
            from = rad - 0.8f * s > hw + 0.1f * s ? rad - 0.8f * s : hw + 0.1f * s; // a dial's rays start outside its rim
        GXColor bc = Color_Fill(r->color, 1.f - q);
        for (int ray = 0; ray < 6; ray++)
        {
            float ang = ray * 1.0471976f + 0.5235988f;
            float dx = cos(ang), dy = sin(ang);
            Hud_Seg(cx + dx * from, cy + dy * from, cx + dx * rad, cy + dy * rad, 0.18f * s, bc);
        }
    }
    if (r->implode >= 0)
    {
        float q = (float)r->implode / LL_FADE;
        GXColor dc = r->implode_tone == TONE_MISS ? color_miss : color_skip;
        float k = 1.f - Ease_Out(q);
        if (round)
        {
            float rr = (hw + 0.8f * s) * k;
            if (rr > 0.15f)
                Dial_Band(cx, cy, rr - 0.1f, rr + 0.1f, 0, 360, Color_Fill(dc, 0.6f * (1.f - q)));
        }
        else
        {
            if (r->implode == 0)
                Hud_Rect(cx - hw, cy - hh, cx + hw, cy + hh, dc); // the frame that was needed
            float fw = (hw + 0.8f * s) * k, fh = (hh + 0.6f * s) * k;
            if (fw > 0.05f)
                Hud_Frame(cx - fw, cy - fh, cx + fw, cy + fh, 1.4f * PX, Color_Fill(dc, 0.6f * (1.f - q)));
        }
    }
}

// One lane of the highway: its cells as notes, a cell a frame up from the
// hit line.
static void Hw_Lane(MeterRow *r, int i, float lx, float line_y)
{
    float rx = lx + HW_LANE, in = 0.15f;
    float dim = r->dim ? 0.5f : 1.f;
    int len = r->len < HW_ROWS ? r->len : HW_ROWS;

    for (int k = 0; k < len; k++)
    {
        int kind = r->cell[k];
        if (kind == CELL_NONE)
            continue;
        int tone = r->tone[k], al = r->alpha[k];
        int j = k;
        // a window two frames wide is one note two rows tall
        if (kind == CELL_PRESS || kind == CELL_LAG || kind == CELL_TAIL)
        {
            while (j + 1 < len && r->cell[j + 1] == kind && r->tone[j + 1] == tone && r->alpha[j + 1] == al)
                j++;
        }
        float a = al / 255.f * dim;
        GXColor col = Tone_Color(r, tone);
        float y0 = line_y + k * HW_PITCH + 0.04f, y1 = line_y + (j + 1) * HW_PITCH - 0.04f;
        float yc = (y0 + y1) / 2, mid = (lx + rx) / 2;
        switch (kind)
        {
        case CELL_PRESS:
            Hud_Rect(lx + in, y0, rx - in, y1, Color_Fill(col, a));
            break;
        case CELL_ACT:
            Hud_Rect(lx + in, y0, rx - in, y1, Color_Fill(col, a));
            Hud_Rect(lx + in, y0, rx - in, y0 + 0.2f * HW_PITCH + 0.04f, Color_Fill(color_white, a * 0.9f));
            break;
        case CELL_AIR:
            Hud_Rect(lx + 0.45f, yc - 0.07f, rx - 0.45f, yc + 0.07f, Color_Over(col, 0.45f * a));
            break;
        case CELL_LAND:
            Hud_Rect(lx + in, y0, rx - in, y1, Color_Fill(color_plate, a * 0.6f));
            Hud_Frame(lx + in, y0, rx - in, y1, 1.4f * PX, Color_Fill(col, a));
            Hud_Rect(lx + in, y0, rx - in, y0 + 0.24f * HW_PITCH + 0.04f, Color_Fill(col, a));
            break;
        case CELL_LAG:
            Hud_Rect(mid - 0.5f, y0, mid + 0.5f, y1, Color_Fill(col, a * 0.75f));
            break;
        case CELL_TAIL:
            Hud_Rect(lx + in, y0, rx - in, y1, Color_Fill(color_galint, a));
            break;
        }
        k = j;
    }
    if (i != 0)
        return;
    for (int k = 0; k < len; k++)
    {
        if (r->glyph[k])
            Glyph_Draw(r->glyph[k], lx - 0.8f, line_y + (k + 0.5f) * HW_PITCH, 0.3f, Color_Fill(color_white, 0.95f * dim));
    }
}

// The highway (Fixed Strip): one vertical lane per row, notes falling onto a
// hit line at the bottom, a row a frame with every fifth one heavier, the
// label under the lane. The hit line lights while a press is due, and the
// ghost, burst and fold are the cells' at the line.
static void Highway_Draw(FighterData *fp)
{
    int rows = meter_rows;
    float x0, y0, w, h;
    Strip_Place(fp, STRIP_HIGHWAY, &x0, &y0, &w, &h);
    float lane0 = x0 + STRIP_PAD + (Strip_Glyphs() ? HW_GLYPH : 0);
    float line_y = y0 + HW_LABEL;
    float lanes_r = lane0 + rows * HW_LANE + (rows - 1) * HW_LANE_GAP;

    Hud_Rect(x0, y0, x0 + w, y0 + h, Color_Fill(color_plate, 0.72f));
    for (int k = 1; k <= HW_ROWS; k++)
    {
        int five = k % 5 == 0;
        float y = line_y + k * HW_PITCH, e = five ? 0.1f : 0.05f;
        Hud_Rect(lane0 - 0.1f, y - e, lanes_r + 0.1f, y + e, Color_Over(color_white, five ? 0.3f : 0.12f));
    }

    for (int i = 0; i < rows; i++)
    {
        MeterRow *r = &meter[i];
        float lx = lane0 + i * (HW_LANE + HW_LANE_GAP), rx = lx + HW_LANE;
        Hud_Rect(lx, line_y, rx, line_y + HW_ROWS * HW_PITCH, Color_Over(color_white, 0.05f));
        Hw_Lane(r, i, lx, line_y);

        int hot = r->dim ? 0 : r->flash >= 0 ? 2 : r->hot;
        if (hot > 0)
        {
            GXColor fill = Color_Mix(r->color, color_white, hot >= 2 ? 1.f : 0.4f);
            Hud_Rect(lx - 0.3f, line_y - 0.2f, rx + 0.3f, line_y + HW_PITCH + 0.12f, Color_Fill(r->color, 0.3f));
            Hud_Rect(lx - 0.15f, line_y - 0.09f, rx + 0.15f, line_y + 0.09f, fill);
            if (hot >= 2)
                Hud_Rect(lx - 0.3f, line_y - 0.2f, rx + 0.3f, line_y + 0.2f, color_white); // the press frame
        }
        else
            Hud_Rect(lx - 0.2f, line_y - 0.07f, rx + 0.2f, line_y + 0.07f, Color_Over(color_white, r->dim ? 0.3f : 0.6f));

        if (!r->dim)
            Strip_Ending(r, (lx + rx) / 2, line_y + HW_PITCH / 2, HW_LANE / 2, HW_PITCH / 2, 1.f, 0);
        Strip_Label(r, (lx + rx) / 2, y0 + 0.75f);
    }
}

static int Dial_Target(int kind)
{
    return kind == CELL_PRESS || kind == CELL_LAND || kind == CELL_ACT;
}

// One dial for a row: the first run of cells that matter (a press, a
// touchdown, the first frame Falcon can act) is its wedge at twelve
// o'clock, and the hand sweeps clockwise a frame at a time into it, with
// ticks for the last six frames. A hand more than 21 frames out waits
// hidden. The ledge intangibility after the wedge is a band on the rim.
static void Dial_One(MeterRow *r, float cx, float cy)
{
    float R = DL_R, dim = r->dim ? 0.5f : 1.f;
    int a = -1, n = 0, tail = 0;
    for (int k = 0; k < r->len && k < MT_CELLS; k++)
    {
        if (Dial_Target(r->cell[k]))
        {
            a = k;
            break;
        }
    }
    if (a >= 0)
    {
        for (n = 1; a + n < r->len && a + n < MT_CELLS && n < 24 && Dial_Target(r->cell[a + n]); n++)
            ;
        while (a + n + tail < r->len && a + n + tail < MT_CELLS && n + tail < 24 && r->cell[a + n + tail] == CELL_TAIL)
            tail++;
    }

    Dial_Fan(cx, cy, R + 0.35f, 0, 360, Color_Fill(color_plate, 0.8f));
    Dial_Band(cx, cy, R - 0.05f, R + 0.05f, 0, 360, Color_Over(color_white, 0.2f * dim));

    if (a >= 0)
    {
        int tone = r->tone[a];
        float al = r->alpha[a] / 255.f * dim;
        GXColor col = Tone_Color(r, tone);
        int hot = r->flash >= 0 ? 2 : r->hot;
        float span = (n + r->spent) * DL_DEG;
        if (span > 360.f)
            span = 360.f;
        GXColor wc = hot >= 2 && tone == TONE_CUE ? Color_Mix(col, color_white, 0.6f) : col;
        Dial_Fan(cx, cy, R - 0.05f, 0, span, Color_Over(wc, (hot ? 1.f : 0.75f) * al));
        if (tail > 0 && span < 360.f)
        {
            float end = span + tail * DL_DEG;
            Dial_Band(cx, cy, R - 0.38f, R - 0.1f, span, end > 360.f ? 360.f : end, Color_Over(color_galint, 0.85f * al));
        }

        // ticks for the last six frames, where the hand will be (not on the
        // routes that aren't the chosen one)
        for (int d = 1; d <= 6 && !r->dim; d++)
        {
            float deg = (0.5f - d) * DL_DEG, x0, y0, x1, y1;
            Dial_Pt(cx, cy, R - 0.4f, deg, &x0, &y0);
            Dial_Pt(cx, cy, R, deg, &x1, &y1);
            Hud_Seg(x0, y0, x1, y1, 0.1f, Color_Over(color_white, 0.6f * al));
        }

        // the hand, in the middle of its frame's slice
        if (a <= 21)
        {
            float deg = (r->spent - a + 0.5f) * DL_DEG, hx, hy;
            Dial_Pt(cx, cy, R + 0.2f, deg, &hx, &hy);
            GXColor hc = tone == TONE_CUE ? Color_Mix(r->color, color_white, hot ? 1.f : 0.55f) : Tone_Color(r, tone);
            Hud_Seg(cx, cy, hx, hy, 0.34f, Color_Over(color_plate, 0.5f * al));
            Hud_Seg(cx, cy, hx, hy, 0.2f, Color_Over(hc, al));
        }
    }
    if (r->implode == 0)
        Dial_Fan(cx, cy, R - 0.05f, 0, DL_DEG, r->implode_tone == TONE_MISS ? color_miss : color_skip); // the frame that was needed
    if (!r->dim)
        Strip_Ending(r, cx, cy, R, R, 1.f, 1);
}

// The dial (Fixed Strip): a small dial for each row, side by side, the label
// under it.
static void Dial_Draw(FighterData *fp)
{
    float x0, y0, w, h;
    Strip_Place(fp, STRIP_DIAL, &x0, &y0, &w, &h);
    for (int i = 0; i < meter_rows; i++)
    {
        MeterRow *r = &meter[i];
        float cx = x0 + STRIP_PAD + DL_R + 0.35f + i * DL_CELL, cy = y0 + DL_LABEL + DL_R + 0.45f;
        Dial_One(r, cx, cy);
        Strip_Label(r, cx, y0 + 0.75f);
    }
}

// The fixed strip: bottom left, or bottom right when the controller display
// is on the left half of the screen. Cells, or the highway or dial in a
// corner.
static void Meter_Fixed(FighterData *fp)
{
    int look = Options_Timers[TOPT_STRIP].val;
    if (look == STRIP_HIGHWAY)
    {
        Highway_Draw(fp);
        return;
    }
    if (look == STRIP_DIAL)
    {
        Dial_Draw(fp);
        return;
    }
    float x0, y0, x1, y1;
    Pad_Box(fp, &x0, &y0, &x1, &y1);
    float gx = x0 + x1 < 0 ? 3.6f : -SAFE_W + 3.2f;
    Meter_Draw(gx, -SAFE_H + 0.4f, MT_FIXED, MT_FIXED_MAX, 1);
}

///////////////////////
/// Spot timers     ///
///////////////////////

#define SPOT_HW 4.f     // half the footprint, in world units
#define SPOT_STEM 1.5f  // bracket height, HUD units
#define SPOT_LINE 0.4f  // footprint and bracket thickness
#define SPOT_FOOT 0.7f

static float Meter_Pitch(void)
{
    return MT_PITCH + (Options_Timers[TOPT_WIDE].val ? MT_WIDE : 0);
}

static void Spot_Bracket(float x, float y, int side, float stem, GXColor c)
{
    Hud_Rect(x - SPOT_LINE / 2, y, x + SPOT_LINE / 2, y + stem, c);
    float fx = x - side * SPOT_FOOT;
    Hud_Rect(side > 0 ? fx : x, y, side > 0 ? x : fx, y + SPOT_LINE, c);
}

// A bracket on a dark edge, so it reads over any stage.
static void Spot_BracketEdged(float x, float y, int side, float stem, GXColor c, float a)
{
    float e = 0.12f;
    Hud_Rect(x - SPOT_LINE / 2 - e, y - e, x + SPOT_LINE / 2 + e, y + stem + e, Color_Over(color_plate, 0.55f * a));
    float fx = x - side * (SPOT_FOOT + e);
    Hud_Rect(side > 0 ? fx : x - e, y - e, side > 0 ? x + e : fx, y + SPOT_LINE + e, Color_Over(color_plate, 0.55f * a));
    Spot_Bracket(x, y, side, stem, c);
}

// The timer's one ghost (Cue_Ghost): brackets that spread out and fade.
static void Spot_BracketGhost(Cue *c, GXColor base, float lx, float ly, float rx, float ry)
{
    int bright, g = Cue_Ghost(c, &bright);
    if (g < 0)
        return;
    float q = (float)g / LL_GHOST, fade = (1.f - q) * (1.f - q);
    float o = (bright ? 3.f : 1.4f) * Ease_Out(q);
    GXColor gc = Color_Over(Color_Mix(base, color_white, bright ? 0.4f : 0.2f), (bright ? 0.95f : 0.45f) * fade);
    float stem = SPOT_STEM * (1.f + (bright ? 0.6f : 0.25f) * q);
    Spot_Bracket(lx - o, ly, -1, stem, gc);
    Spot_Bracket(rx + o, ry, 1, stem, gc);
    if (bright)
        Hud_Seg(lx - o, ly, rx + o, ry, SPOT_LINE, Color_Over(base, 0.5f * fade));
}

// AI and NIL: the footprint where Falcon touches down, and brackets that
// close in on it a meter cell per frame and meet it on the frame to press
// (the touchdown, for a NIL).
static void Spot_Brackets(int kind, Cue *c, int ended)
{
    float lx, ly, rx, ry, mx, my;
    if (!Hud_FromWorld(c->spot.X, c->spot.Y, &mx, &my))
        return;
    Hud_FromWorld(c->spot.X - SPOT_HW, c->spot.Y, &lx, &ly);
    Hud_FromWorld(c->spot.X + SPOT_HW, c->spot.Y, &rx, &ry);
    GXColor base = Cue_Color(kind);
    float pitch = Meter_Pitch();

    if (!ended && (c->phase == PH_COUNT || c->phase == PH_WINDOW))
    {
        int k = c->phase == PH_WINDOW ? 0 : kind == CUE_NIL ? c->left - 2 : c->left - 1;
        if (k < 0)
            k = 0;
        if (k > 10)
            return;
        GXColor col = c->dim ? Dim_Color(c->dim) : base;
        float am = c->dim ? 0.5f : 1.f;
        float charge = k >= 5 ? 0.75f : 1.f - k * 0.05f;
        int now = c->phase == PH_WINDOW && !c->dim;
        Hud_Seg(lx, ly, rx, ry, SPOT_LINE + 0.24f, Color_Over(color_plate, 0.5f * am));
        Hud_Seg(lx, ly, rx, ry, SPOT_LINE + 0.04f, Color_Fill(now ? color_white : col, charge * am));
        float off = k * pitch;
        float ba = (k == 0 ? 1.f : 0.7f + 0.3f * (1.f - k / 10.f)) * am;
        GXColor bc = Color_Fill(now ? Color_Mix(col, color_white, c->age == 0 ? 0.9f : 0.4f) : col, ba);
        Spot_BracketEdged(lx - off, ly, -1, SPOT_STEM, bc, ba);
        Spot_BracketEdged(rx + off, ry, 1, SPOT_STEM, bc, ba);
        if (c->phase == PH_WINDOW)
            Spot_BracketGhost(c, base, lx, ly, rx, ry);
        return;
    }

    if (!ended)
        return;
    switch (c->phase)
    {
    case PH_HIT:
    {
        int lag = c->lag > 0 ? c->lag : 1;
        if (c->age == 0)
        {
            Hud_Seg(lx - 0.3f, ly, rx + 0.3f, ry, SPOT_LINE + 0.25f, Color_Fill(base, 0.5f));
            Hud_Seg(lx, ly, rx, ry, SPOT_LINE + 0.1f, color_white);
        }
        else if (c->age < lag)
        {
            // burns down over the landing lag, gone when Falcon can act
            float k = 1.f - (float)c->age / lag;
            Hud_Seg(mx + (lx - mx) * k, my + (ly - my) * k, mx + (rx - mx) * k, my + (ry - my) * k, SPOT_LINE, Color_Fill(base, 0.95f));
        }
        if (c->age < 10)
        {
            // sparks running out along the floor
            float q = c->age / 10.f;
            float a = 2.f + 4.f * Ease_Out(q);
            GXColor sc = Color_Fill(base, 0.8f * (1.f - q));
            Hud_Rect(mx - a - 0.9f, my, mx - a, my + 0.12f, sc);
            Hud_Rect(mx + a, my, mx + a + 0.9f, my + 0.12f, sc);
        }
        Spot_BracketGhost(c, base, lx, ly, rx, ry);
        break;
    }
    case PH_FADE:
    {
        // the frame that was needed, then a slow fold inward
        float q = (float)c->age / LL_FADE;
        float k = 1.f - Ease_Out(q);
        GXColor dc = Color_Fill(Dim_Color(c->dim), (c->age == 0 ? 0.9f : 0.5f) * (1.f - q));
        Hud_Seg(mx + (lx - mx) * k, my + (ly - my) * k, mx + (rx - mx) * k, my + (ry - my) * k, SPOT_LINE, dc);
        float o = (rx - mx) * k;
        Spot_Bracket(mx - o, my, -1, SPOT_STEM * k, dc);
        Spot_Bracket(mx + o, my, 1, SPOT_STEM * k, dc);
        Spot_BracketGhost(c, base, lx, ly, rx, ry); // a skip's faint ghost
        break;
    }
    case PH_CUT:
        Hud_Seg(lx, ly, rx, ry, SPOT_LINE, Color_Fill(color_skip, 0.4f * (1.f - (float)c->age / LL_CUT)));
        break;
    }
}

// A small round dot as an octagon, in three quads.
static void Hud_Oct(float x, float y, float r, GXColor c)
{
    float s = r * 0.4142f;
    Hud_Rect(x - r, y - s, x + r, y + s, c);
    Hud_Rect(x - s, y + s, x + s, y + r, c);
    Hud_Rect(x - s, y - r, x + s, y - s, c);
}

// A flat disc on the floor: an ellipse rx wide and ry tall.
static void Hud_Ellipse(float cx, float cy, float rx, float ry, GXColor c)
{
    float px = cx + rx, py = cy;
    for (int i = 1; i <= DISC_SEGS; i++)
    {
        float ang = i * (6.2831853f / DISC_SEGS);
        float nx = cx + cos(ang) * rx, ny = cy + sin(ang) * ry;
        Hud_Tri(cx, cy, px, py, nx, ny, c);
        px = nx;
        py = ny;
    }
}

// An ellipse's outline, w thick all the way around (not thinner where it
// is flat), in enough steps that a flat one stays smooth.
#define ELL_SEGS 20
static void Hud_EllipseRing(float cx, float cy, float rx, float ry, float w, GXColor c)
{
    float ix[ELL_SEGS + 1], iy[ELL_SEGS + 1], ox[ELL_SEGS + 1], oy[ELL_SEGS + 1];
    if (rx < 0.05f)
        rx = 0.05f;
    if (ry < 0.05f)
        ry = 0.05f;
    if (w > ry * 1.6f)
        w = ry * 1.6f; // the inside of a very flat ring can't turn inside out
    for (int i = 0; i <= ELL_SEGS; i++)
    {
        float ang = i * (6.2831853f / ELL_SEGS);
        float ca = cos(ang), sa = sin(ang);
        // the outline's normal there points along (cos / rx, sin / ry)
        float nx = ca / rx, ny = sa / ry, nl = sqrtf(nx * nx + ny * ny);
        nx = nx / nl * w / 2;
        ny = ny / nl * w / 2;
        ix[i] = cx + ca * rx - nx;
        iy[i] = cy + sa * ry - ny;
        ox[i] = cx + ca * rx + nx;
        oy[i] = cy + sa * ry + ny;
    }
    for (int i = 0; i < ELL_SEGS; i++)
        Quad_Add(ix[i], iy[i], ox[i], oy[i], ox[i + 1], oy[i + 1], ix[i + 1], iy[i + 1], c);
}

// The wavedash timers that sit at Falcon's feet, drawn from the waveland
// cue of a wavedash (its jumpsquat, which the cue counts down to the
// airdodge): Pips and Ring. The countdown is the jumpsquat's own frames, so
// a pip or a notch of the ring is a frame; the press frame is the cue's
// window, which opens on the jumpsquat's last frame, as for any timer.
#define WD_PIPS 4      // frames in Falcon's jumpsquat
#define WD_SLOT 2.6f   // the pips' spacing, in world units
#define WD_RING 4.f    // the ring when closed, and
#define WD_STEP 3.2f   // how much wider it is for each frame left, in world units
#define WD_FLAT 0.22f  // how flat it lies

// Where the cue's spot is on the screen, and how many HUD units a world unit
// is there.
static int Wd_Spot(Cue *c, float *mx, float *my, float *sx)
{
    float hx, hy;
    if (!Hud_FromWorld(c->spot.X, c->spot.Y, mx, my) || !Hud_FromWorld(c->spot.X + 10.f, c->spot.Y, &hx, &hy))
        return 0;
    *sx = (hx - *mx) / 10.f;
    if (*sx < 0.05f)
        *sx = 0.05f;
    return 1;
}

// How a wavedash cue looks right now: how far along (k frames before the
// airdodge frame, 0 in the window), how strongly it shows, and the color it
// is drawn in. A press that worked fades it out over the ghost's frames; an
// ending folds it away muted.
typedef struct WdLook
{
    int k;
    int lit;       // jumpsquat frames gone, of WD_PIPS
    float alpha;
    float fold;    // 1 live, shrinking to 0 as a miss or skip folds
    GXColor color; // the cue's color, or the muted one
    int now;       // the frame to press, and it's not muted
} WdLook;

static void Wd_Look(Cue *c, int ended, WdLook *w)
{
    GXColor base = Cue_Color(CUE_WL);
    w->k = c->phase == PH_WINDOW ? 0 : c->left - 1;
    if (w->k < 0)
        w->k = 0;
    w->lit = c->phase == PH_WINDOW ? WD_PIPS : WD_PIPS + 1 - c->left;
    w->lit = w->lit < 0 ? 0 : w->lit > WD_PIPS ? WD_PIPS : w->lit;
    w->alpha = 1.f;
    w->fold = 1.f;
    w->color = c->dim ? Dim_Color(c->dim) : base;
    w->now = !ended && c->phase == PH_WINDOW && !c->dim;
    if (ended && c->phase == PH_FADE)
    {
        float q = (float)c->age / LL_FADE;
        w->alpha = (c->age == 0 ? 0.9f : 0.5f) * (1.f - q);
        w->fold = 1.f - Ease_Out(q);
        w->lit = WD_PIPS;
    }
    else if (ended && c->phase == PH_CUT)
    {
        w->alpha = 0.4f * (1.f - (float)c->age / LL_CUT);
        w->color = color_skip;
    }
    else if (!ended && c->press >= 0)
        w->alpha = 1.f - Clamp01((float)c->press / LL_GHOST); // gone with the ghost
}

// Squat pips: four small pips under Falcon's feet, one lit for each
// jumpsquat frame, then a diamond for the airdodge frame that turns white
// in place when it's due.
static void Wd_Pips(Cue *c, int ended)
{
    float mx, my, sx;
    if (!Wd_Spot(c, &mx, &my, &sx))
        return;
    float s = sx < 0.3f ? 0.3f : sx > 0.55f ? 0.55f : sx; // small shapes, kept readable at any zoom
    float step = WD_SLOT * s, pr = 0.8f * s, dr = 1.4f * s;
    float y = my - 2.f * s, dx = mx + 2.f * step;
    GXColor base = Cue_Color(CUE_WL);

    if (c->phase == PH_FADE || c->phase == PH_CUT || c->phase == PH_COUNT || c->phase == PH_WINDOW)
    {
        WdLook w;
        Wd_Look(c, ended, &w);
        if (w.alpha > 0.01f)
        {
            for (int i = 0; i < WD_PIPS; i++)
            {
                int on = i < w.lit;
                float x = mx + (i - 2) * step;
                GXColor pc = on ? (c->dim || c->phase == PH_FADE || c->phase == PH_CUT ? w.color : color_white) : color_skip;
                Hud_Oct(x, y, pr + 0.1f, Color_Over(color_plate, 0.6f * w.alpha * (on ? 1.f : 0.6f)));
                Hud_Oct(x, y, pr, Color_Over(pc, (on ? 0.95f : 0.4f) * w.alpha));
            }
            GXColor dc = w.now ? Color_Mix(base, color_white, c->age == 0 ? 0.9f : 0.4f) : c->dim || ended ? w.color : Color_Mix(base, color_plate, 0.4f);
            float r = dr * w.fold;
            if (r > 0.05f)
            {
                Hud_Diamond(dx, y, r + 0.14f, Color_Over(color_plate, 0.6f * w.alpha));
                Hud_Diamond(dx, y, r, Color_Over(dc, 0.95f * w.alpha));
            }
        }
    }

    // the one ghost: a disc swelling out of the diamond
    int bright, g = Cue_Ghost(c, &bright);
    if (g >= 0 && c->phase != PH_CUT)
    {
        float q = (float)g / LL_GHOST, fade = (1.f - q) * (1.f - q);
        float r = (1.4f + (bright ? 3.f : 1.2f) * Ease_Out(q)) * s;
        Hud_Disc(dx, y, r, Color_Over(Color_Mix(base, color_white, 0.35f), (bright ? 0.6f : 0.25f) * fade));
    }
}

// Ground ring: a flat ring on the floor around Falcon's feet, a notch wider
// for each jumpsquat frame left, closing on the airdodge frame.
static void Wd_Ring(Cue *c, int ended)
{
    float mx, my, sx;
    if (!Wd_Spot(c, &mx, &my, &sx))
        return;
    GXColor base = Cue_Color(CUE_WL);

    if (c->phase == PH_FADE || c->phase == PH_CUT || c->phase == PH_COUNT || c->phase == PH_WINDOW)
    {
        WdLook w;
        Wd_Look(c, ended, &w);
        float appear = c->phase == PH_COUNT ? Clamp01((c->span - c->left + 1) / 2.f) : 1.f;
        float rw = (WD_RING + WD_STEP * w.k) * w.fold, a = w.alpha * appear;
        GXColor rc = w.now ? color_white : w.color;
        if (rw * sx > 0.1f && a > 0.01f)
        {
            Hud_EllipseRing(mx, my, rw * sx, rw * sx * WD_FLAT, 0.42f, Color_Over(color_plate, 0.5f * a));
            Hud_EllipseRing(mx, my, rw * sx, rw * sx * WD_FLAT, 0.28f, Color_Over(rc, 0.95f * a));
        }
    }

    // the one ghost: a flat disc swelling out of the closed ring
    int bright, g = Cue_Ghost(c, &bright);
    if (g >= 0 && c->phase != PH_CUT)
    {
        float q = (float)g / LL_GHOST, fade = (1.f - q) * (1.f - q);
        float rx = (WD_RING + (bright ? 12.f : 4.f) * Ease_Out(q)) * sx;
        Hud_Ellipse(mx, my, rx, rx * (WD_FLAT + 0.03f), Color_Over(Color_Mix(base, color_white, 0.35f), (bright ? 0.55f : 0.22f) * fade));
    }
}

// Waveland and wavedash: a rail as long as the slide each way, and a mark on
// each end that counts down to the airdodge frame, in the look picked in the
// Timers menu: ticks that slide in and spike where they meet, rails that
// fill from the ends, or chevrons that hop a notch a frame. Every look
// crosses the rail at one speed, whatever the lead; a shorter one just
// starts closer. They meet in the middle on the frame to press, then the
// timer's one ghost plays in place. A white mark shows where the stick held
// now would stop the slide.
#define RAIL_FRAMES (LL_COUNT_FRAMES - 1) // frames a mark takes to cross a whole rail
#define RAIL_FILL 0.3f                    // how thick the filling rails are
#define CHEV_STEPS 8                      // notches a chevron hops over the last frames
#define CHEV_GAP 0.5f                     // the last notch's distance from the spot

// The rails on the screen: the spot and the slide's two ends, and which
// directions work.
typedef struct RailGeom
{
    float mx, my, lx, ly, rx, ry;
    int ok_l, ok_r;
} RailGeom;

// How far along its rail a mark k frames from the airdodge frame is: 0 at the
// spot, 1 at the slide's end.
static float Rail_Q(int k)
{
    return Clamp01((float)k / RAIL_FRAMES);
}

// A bump standing on y: a bell curve wb wide at its foot and h tall, the
// shape of the waveland ticks' wave.
#define BUMP_SLICES 8
static void Hud_Bump(float cx, float y, float wb, float h, GXColor c)
{
    float x0 = cx - wb / 2, dx = wb / BUMP_SLICES;
    float prev = 0;
    for (int i = 1; i <= BUMP_SLICES; i++)
    {
        float u = (float)i / BUMP_SLICES * 2.f - 1.f; // -1..1 across the foot
        float b = 1.f - u * u;
        float top = h * b * b * b;
        Quad_Add(x0 + dx * (i - 1), y, x0 + dx * i, y, x0 + dx * i, y + top, x0 + dx * (i - 1), y + prev, c);
        prev = top;
    }
}

// How tall the spike gets: about Falcon's height on the screen, kept between
// a height that shows and one that fills it.
static float Rail_Peak(Cue *c)
{
    float ax, ay, bx, by;
    if (!Hud_FromWorld(c->spot.X, c->spot.Y, &ax, &ay) || !Hud_FromWorld(c->spot.X, c->spot.Y + 17.f, &bx, &by))
        return 6.f;
    float h = fabs(by - ay);
    return h < 4.5f ? 4.5f : h > 12.f ? 12.f : h;
}

// One of the waveland ticks, q of the way from the spot (0) to the slide's
// end (1): wide and low out at the ends, where it first catches the eye. It
// keeps the same area as its foot narrows at a steady rate, so it shoots up
// taller and faster the closer it gets, to peak tall at the spot, while it
// slides in at a steady speed.
#define TICK_W0 0.6f // the foot at the spot
#define TICK_W1 4.0f // and at the slide's end
static void Rail_Tick(float x, float y, float q, float peak, GXColor c)
{
    float wb = TICK_W0 + (TICK_W1 - TICK_W0) * q;
    float h = peak * TICK_W0 / wb;
    Hud_Bump(x, y - 0.15f, wb + 0.3f, h + 0.25f, Color_Over(color_plate, 0.5f * c.a / 255.f));
    Hud_Bump(x, y - 0.15f, wb, h, c);
}

// The countdown, k frames from the airdodge frame (0 in the window), in
// the picked look, in col at am of its strength.
static void Rail_Count(int look, Cue *c, RailGeom *g, int k, GXColor col, float am)
{
    float q = Rail_Q(k), n = 1.f - q;
    int now = c->phase == PH_WINDOW && !c->dim;
    GXColor tc = now ? Color_Mix(col, color_white, c->age == 0 ? 0.9f : 0.4f) : Color_Mix(col, color_white, 0.35f * n * n);
    GXColor mark = Color_Over(tc, (0.85f + 0.15f * n) * am);

    switch (look)
    {
    case WLT_TICKS:
    {
        float peak = Rail_Peak(c);
        if (k == 0)
        {
            // they have met: one spike
            if (g->ok_l || g->ok_r)
                Rail_Tick(g->mx, g->my, 0, peak, mark);
            break;
        }
        if (g->ok_l)
            Rail_Tick(g->mx + (g->lx - g->mx) * q, g->my + (g->ly - g->my) * q, q, peak, mark);
        if (g->ok_r)
            Rail_Tick(g->mx + (g->rx - g->mx) * q, g->my + (g->ry - g->my) * q, q, peak, mark);
        break;
    }
    case WLT_RAILS:
        // the rail fills from its end toward the spot, the same share each
        // frame on both sides, and is full on the airdodge frame
        for (int side = -1; side <= 1; side += 2)
        {
            if (side < 0 ? !g->ok_l : !g->ok_r)
                continue;
            float ex = side < 0 ? g->lx : g->rx, ey = side < 0 ? g->ly : g->ry;
            if (n < 0.001f)
                continue;
            float tx = ex + (g->mx - ex) * n, ty = ey + (g->my - ey) * n;
            Hud_Seg(ex, ey, tx, ty, RAIL_FILL + 0.14f, Color_Over(color_plate, 0.5f * am));
            Hud_Seg(ex, ey, tx, ty, RAIL_FILL, mark);
            if (k > 0)
                Hud_Rect(tx - 0.08f, ty - 0.12f, tx + 0.08f, ty + 0.6f, Color_Over(Color_Mix(tc, color_white, 0.5f), 0.9f * am));
        }
        break;
    case WLT_CHEVRONS:
    {
        // arrowheads pointing at the spot, a notch closer a frame over the
        // last CHEV_STEPS; the notches are marked under the rail
        int step = k > CHEV_STEPS ? CHEV_STEPS : k;
        for (int side = -1; side <= 1; side += 2)
        {
            if (side < 0 ? !g->ok_l : !g->ok_r)
                continue;
            float ex = side < 0 ? g->lx : g->rx, ey = side < 0 ? g->ly : g->ry;
            float dx = ex - g->mx, dy = ey - g->my, len = sqrtf(dx * dx + dy * dy);
            if (len < 3.f * CHEV_GAP)
                continue;
            float ux = dx / len, uy = dy / len, notch = (len - CHEV_GAP) / (CHEV_STEPS + 1);
            for (int i = 1; i <= CHEV_STEPS; i++)
            {
                float p = CHEV_GAP + i * notch;
                Hud_Rect(g->mx + ux * p - 0.05f, g->my + uy * p - 0.45f, g->mx + ux * p + 0.05f, g->my + uy * p - 0.1f, Color_Over(color_white, 0.4f * am));
            }
            float p = CHEV_GAP + step * notch;
            float tx = g->mx + ux * p, ty = g->my + uy * p + 0.75f;
            float bx = tx + ux * 0.65f, by = ty + uy * 0.65f, wx = -uy * 0.7f, wy = ux * 0.7f;
            GXColor edge = Color_Over(color_plate, 0.5f * am);
            Hud_Seg(bx + wx, by + wy, tx, ty, 0.42f, edge);
            Hud_Seg(bx - wx, by - wy, tx, ty, 0.42f, edge);
            Hud_Seg(bx + wx, by + wy, tx, ty, 0.26f, mark);
            Hud_Seg(bx - wx, by - wy, tx, ty, 0.26f, mark);
        }
        break;
    }
    }
}

// The rails' one ghost (Cue_Ghost), in place like the look it belongs to,
// strong and growing for a press that worked, small and dim for a skip:
// the spike swells into a glow (Ticks), the rails swell (Rails), a disc
// rises at the spot (Chevrons). Nothing runs back out along the rail.
static void Spot_RailGhost(int look, Cue *c, GXColor base, RailGeom *g)
{
    int bright, gh = Cue_Ghost(c, &bright);
    if (gh < 0)
        return;
    float q = (float)gh / LL_GHOST, e = Ease_Out(q), fade = (1.f - q) * (1.f - q);
    GXColor gc = Color_Mix(base, color_white, bright ? 0.4f : 0.2f);
    switch (look)
    {
    case WLT_TICKS:
    {
        float wb = TICK_W0 + (bright ? 2.2f : 1.0f) * e;
        float h = Rail_Peak(c) * (bright ? 1.f + 0.25f * e : 0.5f);
        Hud_Bump(g->mx, g->my - 0.15f, wb, h, Color_Over(gc, (bright ? 0.95f : 0.45f) * fade));
        break;
    }
    case WLT_RAILS:
    {
        float ax = g->ok_l ? g->lx : g->mx, ay = g->ok_l ? g->ly : g->my;
        float bx = g->ok_r ? g->rx : g->mx, by = g->ok_r ? g->ry : g->my;
        Hud_Seg(ax, ay, bx, by, RAIL_FILL + 2.f * (bright ? 0.9f : 0.35f) * e, Color_Over(gc, (bright ? 0.6f : 0.25f) * fade));
        break;
    }
    case WLT_CHEVRONS:
        Hud_Disc(g->mx, g->my + 0.5f, bright ? 0.5f + 1.8f * e : 0.3f + 0.6f * e, Color_Over(gc, (bright ? 0.6f : 0.25f) * fade));
        break;
    }
}

static void Spot_Rails(Cue *c, FighterData *fp, int ended)
{
    float reach = Slide_Distance(fp, common_dodge_force);
    float L = c->spot.X - reach, R = c->spot.X + reach;
    int off_l = L < c->x0, off_r = R > c->x1;
    if (off_l)
        L = c->x0;
    if (off_r)
        R = c->x1;
    float mx, my, lx, ly, rx, ry;
    if (!Hud_FromWorld(c->spot.X, c->spot.Y, &mx, &my))
        return;
    Hud_FromWorld(L, c->spot.Y, &lx, &ly);
    Hud_FromWorld(R, c->spot.Y, &rx, &ry);
    GXColor base = Cue_Color(CUE_WL);
    int ok_l = c->wd || (c->dirs & DODGE_LEFT);
    int ok_r = c->wd || (c->dirs & DODGE_RIGHT);
    RailGeom g = {mx, my, lx, ly, rx, ry, ok_l, ok_r};
    int look = Options_Timers[TOPT_WL].val;
    // a wavedash out of the jumpsquat has a timer of its own (Wavedash in
    // the menu), so the rails only mark the slide then, unless that timer
    // is the strip's row
    int timed = !c->wd || Options_Timers[TOPT_WD].val == WDT_CELLS;

    if (!ended && (c->phase == PH_COUNT || c->phase == PH_WINDOW))
    {
        GXColor col = c->dim ? Dim_Color(c->dim) : base;
        float am = c->dim ? 0.5f : 1.f;

        Hud_Seg(lx, ly, mx, my, 0.12f, Color_Fill(col, (ok_l ? 0.4f : 0.15f) * am));
        Hud_Seg(mx, my, rx, ry, 0.12f, Color_Fill(col, (ok_r ? 0.4f : 0.15f) * am));
        Hud_Rect(mx - 0.08f, my - 0.15f, mx + 0.08f, my + 0.5f, Color_Fill(col, 0.8f * am));
        for (int side = -1; side <= 1; side += 2)
        {
            int ok = side < 0 ? ok_l : ok_r;
            int off = side < 0 ? off_l : off_r;
            float ex = side < 0 ? lx : rx, ey = side < 0 ? ly : ry;
            GXColor ec = Color_Fill(col, (ok ? 0.8f : 0.3f) * am);
            if (off)
            {
                // slides off here
                Hud_Tri(ex, ey + 0.3f, ex + side * 0.7f, ey - 0.2f, ex, ey - 0.7f, ec);
            }
            else
                Hud_Rect(ex - 0.1f, ey, ex + 0.1f, ey + 0.75f, ec);
        }

        // a countdown fades in over its first 2 frames
        int k = c->phase == PH_WINDOW ? 0 : c->left - 1;
        float appear = c->phase == PH_COUNT ? Clamp01((c->span - c->left + 1) / 2.f) : 1.f;
        if (timed)
            Rail_Count(look, c, &g, k < 0 ? 0 : k, col, am * appear);
        if (timed && c->phase == PH_WINDOW)
            Spot_RailGhost(look, c, base, &g);

        // where the stick held now would stop the slide: the dodge takes
        // its angle and keeps the sideways part of its speed
        float sx = fp->input.lstick.X, sy = fp->input.lstick.Y;
        float len = sqrtf(sx * sx + sy * sy);
        if (!c->dim && sx != 0 && len >= common_dodge_deadzone.X)
        {
            float d = Slide_Distance(fp, common_dodge_force * fabs(sx) / len);
            float x = sx > 0 ? c->spot.X + d : c->spot.X - d;
            float hx, hy;
            if (x < L)
                x = L;
            if (x > R)
                x = R;
            Hud_FromWorld(x, c->spot.Y, &hx, &hy);
            Hud_Rect(hx - 0.09f, hy - 0.35f, hx + 0.09f, hy + 0.85f, Color_Fill(color_white, 0.9f));
        }
        return;
    }

    if (!ended)
        return;
    switch (c->phase)
    {
    case PH_HIT:
    {
        int age = c->age, len = 24 + c->lag;
        float s = c->soft ? 0.6f : 1.f;
        if (age == 0 && !c->soft)
        {
            Hud_Seg(lx, ly, rx, ry, 0.45f, Color_Fill(base, 0.45f));
            Hud_Seg(lx, ly, rx, ry, 0.2f, color_white);
        }
        else
        {
            // how far you slid against how far it goes
            float fade = age > len - 6 ? (len - age) / 6.f : 1.f;
            GXColor rc = Color_Fill(base, 0.35f * fade * s);
            Hud_Seg(lx, ly, rx, ry, 0.12f, rc);
            Hud_Rect(lx - 0.1f, ly, lx + 0.1f, ly + 0.75f, Color_Fill(base, 0.6f * fade * s));
            Hud_Rect(rx - 0.1f, ry, rx + 0.1f, ry + 0.75f, Color_Fill(base, 0.6f * fade * s));
            float fx = fp->phys.pos.X < L ? L : fp->phys.pos.X > R ? R : fp->phys.pos.X;
            float hx, hy;
            Hud_FromWorld(fx, c->spot.Y, &hx, &hy);
            Hud_Rect(hx - 0.1f, hy - 0.2f, hx + 0.1f, hy + 1.0f, Color_Fill(color_white, 0.85f * fade));
        }
        // the ghost carries on from the press
        if (timed)
            Spot_RailGhost(look, c, base, &g);
        break;
    }
    case PH_FADE:
    {
        float q = (float)c->age / LL_FADE;
        float k = 1.f - Ease_Out(q);
        GXColor dc = Color_Fill(Dim_Color(c->dim), (c->age == 0 ? 0.9f : 0.5f) * (1.f - q));
        Hud_Seg(mx + (lx - mx) * k, my + (ly - my) * k, mx + (rx - mx) * k, my + (ry - my) * k, 0.14f, dc);
        Hud_Rect(mx + (lx - mx) * k - 0.1f, my, mx + (lx - mx) * k + 0.1f, my + 1.1f * k, dc);
        Hud_Rect(mx + (rx - mx) * k - 0.1f, my, mx + (rx - mx) * k + 0.1f, my + 1.1f * k, dc);
        if (timed)
            Spot_RailGhost(look, c, base, &g); // a skip's faint ghost
        break;
    }
    case PH_CUT:
        Hud_Seg(lx, ly, rx, ry, 0.12f, Color_Fill(color_skip, 0.35f * (1.f - (float)c->age / LL_CUT)));
        break;
    }
}

// The timers that ride with Falcon, in the looks of the Near Falcon option.
// Each is drawn on the HUD from the cues and his position alone, so a paused
// game draws the same picture again, and in world sizes that follow the
// camera's zoom, kept between a size that can be read and one that crowds the
// screen. They all share what the timers promise: a countdown closes at one
// fixed speed a frame whatever its lead (a shorter one starts closer), the
// press frame is the moving part arriving and turning white, a hit sends out
// the one strong ghost that grows, a skip a smaller, dimmer one, and a miss
// none: it folds away in slate (a skip in gray).
#define NEAR_PI 3.1415927f
#define NEAR_TAU 6.2831853f
#define NEAR_FILL 12       // the ECB look: frames it takes to fill
#define NEAR_LIGHTS_MAX 12 // the lights look: lights drawn, at most

enum near_mode
{
    NMODE_NONE,   // nothing but a ghost: a hit, or a press that worked waiting for its touchdown
    NMODE_COUNT,  // closing on the press
    NMODE_WINDOW, // the press works now
    NMODE_FOLD,   // a miss or a skip folding in
    NMODE_CUT,    // a countdown the prediction dropped
};

// What a look needs of one cue to draw it.
typedef struct NearCue
{
    Cue *c;
    int mode;
    int k;        // counting: frames until the press, 0 on its frame
    int w;        // frames in the window
    int age;      // frames into the window, or into the ending
    int dim;      // a miss or skip already decided
    int now;      // the press works on this frame: white
    int ghost;    // the ghost's age, -1 none
    int bright;   // ... of a press that worked
    float appear; // fading in over the countdown's first 2 frames
    GXColor base; // the cue's own color
    GXColor col;  // what its shapes are in: base, or slate or gray once a miss or skip is decided
} NearCue;

static int Near_Shown(int kind)
{
    return kind == CUE_AI ? Cues_Ai() : kind == CUE_WL ? Cues_Waveland() : Cues_Nil();
}

static float Near_Clamp(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

// HUD units a world unit takes up at (x, y): the camera's zoom there.
static float Near_Scale(float x, float y)
{
    float ax, ay, bx, by;
    Hud_FromWorld(x, y, &ax, &ay);
    Hud_FromWorld(x, y + 10.f, &bx, &by);
    float s = sqrtf((bx - ax) * (bx - ax) + (by - ay) * (by - ay)) / 10.f;
    return s > 0.02f ? s : 0.02f;
}

// The cue as a look draws it. Returns 0 when there is nothing to draw: no
// cue, a wavedash out of the jumpsquat (its own timers draw that), or an
// ending past its ghost.
static int Near_Cue(int kind, Cue *c, NearCue *n)
{
    memset(n, 0, sizeof(*n));
    if (!c->phase || (kind == CUE_WL && c->wd))
        return 0;
    n->c = c;
    n->base = Cue_Color(kind);
    n->dim = c->dim;
    n->col = c->dim ? Dim_Color(c->dim) : n->base;
    n->w = c->width > 0 ? c->width : 1;
    n->age = c->age;
    n->appear = 1.f;
    n->ghost = Cue_Ghost(c, &n->bright);
    switch (c->phase)
    {
    case PH_COUNT:
        // a NIL's press is its touchdown, a frame before the one it acts on
        n->mode = NMODE_COUNT;
        n->k = kind == CUE_NIL ? c->left - 2 : c->left - 1;
        if (n->k < 0)
            n->k = 0;
        // fades in over its first 2 frames (a countdown that grew a frame
        // stays as it was)
        n->appear = c->span - c->left + 1 < 1 ? 1.f : Clamp01((c->span - c->left + 1) / 2.f);
        n->now = n->k == 0 && !c->dim;
        break;
    case PH_WINDOW:
        // once the press worked, its ghost plays on alone
        n->mode = c->held || c->age >= c->width ? NMODE_NONE : NMODE_WINDOW;
        n->now = c->age == 0 && !c->dim;
        break;
    case PH_FADE:
        n->mode = NMODE_FOLD;
        break;
    case PH_CUT:
        n->mode = NMODE_CUT;
        break;
    }
    return n->mode != NMODE_NONE || n->ghost >= 0;
}

// Segments for a circle of radius r (HUD units) that keep its edge round to
// about a pixel, an even number so a disc is half as many quads.
static int Near_Segs(float r, int least)
{
    int n = (int)(12.f * sqrtf(r) + 0.99f);
    n += n & 1;
    return n < least ? least : n > 36 ? 36 : n;
}

// A circle's outline from angle a0 to a1 (radians, counterclockwise from
// the right), w wide and centered on radius r.
static void Near_Arc(float cx, float cy, float r, float w, float a0, float a1, GXColor c)
{
    float span = a1 - a0;
    if (span <= 0.f || r <= 0.f)
        return;
    int n = (int)(span / NEAR_TAU * Near_Segs(r, 10) + 0.99f);
    float r0 = r - w / 2, r1 = r + w / 2;
    if (r0 < 0.f)
        r0 = 0.f;
    float px = cos(a0), py = sin(a0);
    for (int i = 1; i <= n; i++)
    {
        float a = a0 + span * i / n;
        float nx = cos(a), ny = sin(a);
        Quad_Add(cx + px * r0, cy + py * r0, cx + px * r1, cy + py * r1, cx + nx * r1, cy + ny * r1, cx + nx * r0, cy + ny * r0, c);
        px = nx;
        py = ny;
    }
}

static void Near_Ring(float cx, float cy, float r, float w, GXColor c)
{
    Near_Arc(cx, cy, r, w, 0.f, NEAR_TAU, c);
}

// A filled slice of a circle from a0 to a1, with at least least segments
// around a whole one. Two slices go in each quad (a kite from the center).
static void Near_Fan(float cx, float cy, float r, float a0, float a1, int least, GXColor c)
{
    float span = a1 - a0;
    if (span <= 0.f || r <= 0.f)
        return;
    int n = (int)(span / NEAR_TAU * Near_Segs(r, least) + 0.99f);
    float px[40], py[40];
    if (n > 38)
        n = 38;
    for (int i = 0; i <= n; i++)
    {
        float a = a0 + span * i / n;
        px[i] = cx + r * cos(a);
        py[i] = cy + r * sin(a);
    }
    int i = 0;
    for (; i + 1 < n; i += 2)
        Quad_Add(cx, cy, px[i], py[i], px[i + 1], py[i + 1], px[i + 2], py[i + 2], c);
    if (i < n)
        Hud_Tri(cx, cy, px[i], py[i], px[i + 1], py[i + 1], c);
}

static void Near_Disc(float cx, float cy, float r, GXColor c)
{
    Near_Fan(cx, cy, r, 0.f, NEAR_TAU, 10, c);
}

// A small dot: an octagon, or a hexagon at the smallest.
static void Near_Dot(float cx, float cy, float r, GXColor c)
{
    Near_Fan(cx, cy, r, 0.f, NEAR_TAU, 6, c);
}

// The bubble: a ring closes on a bubble where Falcon's body will be on the
// first frame the press works (Cue_Body), at one fixed speed a frame, and
// touches it on that frame. In the window the bubble is a pie that drains a
// frame at a time, its edge white as the window opens. A hit's ghost fills
// the bubble, grows to about 2.3 times its size and bursts in six spokes; a
// skip's is smaller and dimmer; a miss's ring folds away in slate. The bubble
// never moves once its window has opened, so it never chases Falcon, and may
// cover the path under it.
static void Near_Bubble(int kind, Cue *c)
{
    NearCue n;
    if (!Near_Cue(kind, c, &n))
        return;
    // where his body will be; with no countdown behind a hit, where he is
    float bx = c->has_body ? c->body.X : c->spot.X;
    float by = c->has_body ? c->body.Y : c->spot.Y + body_offset;
    float hx, hy;
    if (!Hud_FromWorld(bx, by, &hx, &hy))
        return;
    float S = Near_Scale(bx, by);
    float rb = Near_Clamp(3.6f * S, 0.9f, 2.4f); // the bubble's radius
    float step = rb * 0.172f;                    // the ring's speed, a frame
    float wr = Near_Clamp(0.16f * rb, 0.12f, 0.3f);
    float ew = wr + 0.1f; // the dark under-stroke

    if (n.mode == NMODE_COUNT)
    {
        float a = n.appear;
        Near_Disc(hx, hy, rb, Color_Over(color_plate, 0.45f * a));
        Near_Ring(hx, hy, rb, ew, Color_Over(color_plate, 0.4f * a));
        Near_Ring(hx, hy, rb, wr, Color_Over(n.now ? color_white : n.col, 0.9f * a));
        Hud_Diamond(hx, hy, 0.4f * rb, Color_Over(color_plate, 0.5f * a));
        Hud_Diamond(hx, hy, 0.28f * rb, Color_Over(n.col, 0.9f * a));
        if (n.k > 0)
        {
            float ra = rb + n.k * step;
            if (n.k <= 6) // far out its dark edge isn't worth the quads
                Near_Ring(hx, hy, ra, ew, Color_Over(color_plate, 0.35f * a));
            Near_Ring(hx, hy, ra, wr, Color_Over(Color_Mix(n.col, color_white, 0.25f), 0.7f * a));
        }
    }
    else if (n.mode == NMODE_WINDOW)
    {
        float left = (float)(n.w - n.age) / n.w;
        Near_Disc(hx, hy, rb, Color_Over(color_plate, 0.5f));
        Near_Fan(hx, hy, rb, NEAR_PI / 2.f - NEAR_TAU * left, NEAR_PI / 2.f, 10, Color_Over(n.col, 0.9f));
        Near_Ring(hx, hy, rb, ew, Color_Over(color_plate, 0.4f));
        Near_Ring(hx, hy, rb, wr, Color_Over(n.now ? color_white : n.col, 1.f));
    }
    else if (n.mode == NMODE_FOLD)
    {
        float q = Clamp01((float)n.age / LL_FADE);
        float r = rb * (1.f - Ease_Out(q));
        if (r > 0.25f)
        {
            Near_Ring(hx, hy, r, ew, Color_Over(color_plate, 0.35f * (1.f - q)));
            Near_Ring(hx, hy, r, wr, Color_Over(n.col, 0.8f * (1.f - q)));
        }
    }
    else if (n.mode == NMODE_CUT)
        Near_Ring(hx, hy, rb, wr, Color_Over(color_skip, 0.35f * (1.f - (float)n.age / LL_CUT)));

    if (n.ghost >= 0)
    {
        float q = (float)n.ghost / LL_GHOST, fade = (1.f - q) * (1.f - q);
        float r = rb * (1.f + (n.bright ? 1.3f : 0.5f) * Ease_Out(q));
        Near_Disc(hx, hy, r, Color_Over(Color_Mix(n.base, color_white, n.bright ? 0.3f : 0.15f), (n.bright ? 0.6f : 0.22f) * fade));
        Near_Ring(hx, hy, r, wr * 1.2f, Color_Over(Color_Mix(n.base, color_white, 0.4f), (n.bright ? 0.95f : 0.45f) * fade));
        if (n.bright)
        {
            for (int i = 0; i < 6; i++)
            {
                float ang = i * NEAR_PI / 3.f + NEAR_PI / 6.f, cs = cos(ang), sn = sin(ang);
                Hud_Seg(hx + cs * r * 0.9f, hy + sn * r * 0.9f, hx + cs * r * 1.5f, hy + sn * r * 1.5f, wr, Color_Over(n.base, fade));
            }
        }
    }
}

// The halo: a faint ring around Falcon's body, which it follows, with a bead
// running clockwise at 15 degrees a frame into the window's notch at twelve
// o'clock (as wide as the window, 15 degrees a frame of it); the last three
// frames are dots on the ring. A hit lights the ring up a little and grows
// it; the ghost stays modest.
static void Near_Halo(FighterData *fp, NearCue *n)
{
    float x = fp->phys.pos.X, y = fp->phys.pos.Y + body_offset;
    float hx, hy;
    if (!Hud_FromWorld(x, y, &hx, &hy))
        return;
    float S = Near_Scale(x, y);
    float R = Near_Clamp(12.f * S, 2.6f, 7.5f);
    float wf = 0.12f;                             // the faint ring
    float wn = Near_Clamp(1.2f * S, 0.3f, 0.5f);   // the notch
    float step = NEAR_PI / 12.f;                   // 15 degrees
    float top = NEAR_PI / 2.f;                     // twelve o'clock; a frame before it is counterclockwise

    if (n->mode == NMODE_COUNT || n->mode == NMODE_WINDOW)
    {
        float a = n->mode == NMODE_COUNT ? n->appear : 1.f;
        Near_Ring(hx, hy, R, wf + 0.1f, Color_Over(color_plate, 0.25f * a));
        Near_Ring(hx, hy, R, wf, Color_Over(color_white, 0.3f * a));
        float w = n->w * step;
        if (w > NEAR_TAU - 2.f * step)
            w = NEAR_TAU - 2.f * step;
        Near_Arc(hx, hy, R, wn + 0.12f, top - w, top, Color_Over(color_plate, 0.5f));
        Near_Arc(hx, hy, R, wn, top - w, top, Color_Over(n->col, 0.9f));

        float dr = Near_Clamp(0.6f * S, 0.18f, 0.32f);
        for (int j = 1; j <= 3; j++)
        {
            float ang = top + j * step;
            Near_Dot(hx + R * cos(ang), hy + R * sin(ang), dr + 0.07f, Color_Over(color_plate, 0.5f * a));
            Near_Dot(hx + R * cos(ang), hy + R * sin(ang), dr, Color_Over(color_white, 0.7f * a));
        }

        if (n->mode == NMODE_COUNT)
        {
            // the bead and its tail, the tail trailing counterclockwise
            float br = Near_Clamp(1.4f * S, 0.4f, 0.75f);
            for (int pass = 0; pass < 2; pass++)
            {
                for (int tail = 3; tail >= 0; tail--)
                {
                    float ang = top + (n->k + tail * 0.35f) * step;
                    float r = br * (1.f - 0.19f * tail), al = (1.f - 0.25f * tail) * a;
                    if (pass == 0)
                        Near_Dot(hx + R * cos(ang), hy + R * sin(ang), r + 0.09f, Color_Over(color_plate, 0.4f * al));
                    else
                        Near_Dot(hx + R * cos(ang), hy + R * sin(ang), r, Color_Over(Color_Mix(n->col, color_white, 0.6f), al));
                }
            }
        }
        else
        {
            float ang = top - (n->age + 0.5f) * step;
            float br = Near_Clamp(1.5f * S, 0.42f, 0.8f);
            Near_Dot(hx + R * cos(ang), hy + R * sin(ang), br + 0.1f, Color_Over(color_plate, 0.55f));
            Near_Dot(hx + R * cos(ang), hy + R * sin(ang), br, Color_Over(n->dim ? n->col : color_white, 1.f));
        }
    }
    else if (n->mode == NMODE_FOLD)
    {
        float q = Clamp01((float)n->age / LL_FADE);
        float r = R * (1.f - 0.5f * Ease_Out(q));
        Near_Ring(hx, hy, r, wf + 0.12f, Color_Over(color_plate, 0.3f * (1.f - q)));
        Near_Ring(hx, hy, r, wf + 0.04f, Color_Over(n->col, 0.6f * (1.f - q)));
    }
    else if (n->mode == NMODE_CUT)
        Near_Ring(hx, hy, R, wf, Color_Over(color_skip, 0.3f * (1.f - (float)n->age / LL_CUT)));

    if (n->ghost >= 0)
    {
        float q = (float)n->ghost / LL_GHOST, fade = (1.f - q) * (1.f - q);
        float r = R * (1.f + (n->bright ? 0.22f : 0.1f) * Ease_Out(q));
        float gw = n->bright ? Near_Clamp(0.9f * S, 0.15f, 0.36f) : Near_Clamp(0.5f * S, 0.1f, 0.2f);
        if (n->bright)
            Near_Disc(hx, hy, r, Color_Over(n->base, 0.14f * fade));
        Near_Ring(hx, hy, r, gw, Color_Over(Color_Mix(n->base, color_white, 0.35f), (n->bright ? 0.9f : 0.4f) * fade));
    }
}

// Falcon's ECB as the game collides with it this frame, CollData's
// ecbCurrCorrect_* (offsets from his position): the top, right, bottom and
// left points, in v. One not built yet (all zero) gets a stand-in.
static void Near_Ecb(FighterData *fp, Vec2 *v)
{
    CollData *cd = &fp->coll_data;
    v[0] = cd->ecbCurrCorrect_top;
    v[1] = cd->ecbCurrCorrect_right;
    v[2] = cd->ecbCurrCorrect_bot;
    v[3] = cd->ecbCurrCorrect_left;
    if (v[0].Y - v[2].Y < 6.f || v[1].X - v[3].X < 2.f)
    {
        v[0] = (Vec2){0.f, 17.f};
        v[1] = (Vec2){4.5f, 9.5f};
        v[2] = (Vec2){0.f, 0.f};
        v[3] = (Vec2){-4.5f, 9.5f};
    }
}

// A bracket whose bar stands at x from y0 up to y1 with a foot at each end
// reaching toward the middle (side > 0: the right bracket, a ] shape); no
// part overlaps another, so a translucent one shows no darker corners.
static void Near_Bracket(float x, float y0, float y1, int side, float foot, float w, GXColor c)
{
    float out = x + side * w / 2, in = x - side * foot;
    float lo = out < in ? out : in, hi = out < in ? in : out;
    Hud_Rect(x - w / 2, y0 + w / 2, x + w / 2, y1 - w / 2, c);
    Hud_Rect(lo, y0 - w / 2, hi, y0 + w / 2, c);
    Hud_Rect(lo, y1 - w / 2, hi, y1 + w / 2, c);
}

// The pincers: two brackets as tall as Falcon's ECB close in from both sides
// at a fixed speed a frame and clamp onto his body width on the press frame,
// holding through the window, each on a dark under-stroke. A hit's ghost
// spreads them out again with a fill between; a miss folds them up to their
// middle in slate. Wide is the ECB's own width plus a little, kept near his
// body's.
static void Near_Pincers(FighterData *fp, NearCue *n)
{
    Vec2 v[4];
    Near_Ecb(fp, v);
    float x = fp->phys.pos.X, y = fp->phys.pos.Y;
    float hx, y0, y1, unused;
    if (!Hud_FromWorld(x, y + v[2].Y, &hx, &y0) || !Hud_FromWorld(x, y + v[0].Y, &unused, &y1))
        return;
    float S = Near_Scale(x, y + v[1].Y);
    float half = Near_Clamp((v[1].X - v[3].X) / 2.f + 0.5f, 3.6f, 7.f) * S; // body width, HUD units
    float step = Near_Clamp(0.6f * S, 0.1f, 0.22f);
    float foot = Near_Clamp(1.6f * S, 0.35f, 0.8f);
    float w = Near_Clamp(0.6f * S, 0.12f, 0.28f);
    float e = 0.1f; // the under-stroke's extra width

    if (n->mode == NMODE_COUNT || n->mode == NMODE_WINDOW)
    {
        int count = n->mode == NMODE_COUNT;
        float a = count ? n->appear : 1.f;
        float off = count ? n->k * step : 0.f;
        GXColor c = Color_Over(n->now ? color_white : n->col, count ? 0.9f * a : 1.f);
        for (int side = -1; side <= 1; side += 2)
            Near_Bracket(hx + side * (half + off), y0, y1, side, foot + e / 2, w + e, Color_Over(color_plate, 0.55f * a));
        for (int side = -1; side <= 1; side += 2)
            Near_Bracket(hx + side * (half + off), y0, y1, side, foot, w, c);
    }
    else if (n->mode == NMODE_FOLD)
    {
        float q = Clamp01((float)n->age / LL_FADE), k = 1.f - Ease_Out(q);
        float ym = (y0 + y1) / 2.f, hh = (y1 - y0) / 2.f * k;
        if (hh > 0.3f)
        {
            for (int side = -1; side <= 1; side += 2)
                Near_Bracket(hx + side * half, ym - hh, ym + hh, side, foot + e / 2, w * 0.85f + e, Color_Over(color_plate, 0.35f * (1.f - q)));
            for (int side = -1; side <= 1; side += 2)
                Near_Bracket(hx + side * half, ym - hh, ym + hh, side, foot, w * 0.85f, Color_Over(n->col, 0.7f * (1.f - q)));
        }
    }
    else if (n->mode == NMODE_CUT)
    {
        for (int side = -1; side <= 1; side += 2)
            Near_Bracket(hx + side * half, y0, y1, side, foot, w * 0.85f, Color_Over(color_skip, 0.3f * (1.f - (float)n->age / LL_CUT)));
    }

    if (n->ghost >= 0)
    {
        float q = (float)n->ghost / LL_GHOST, fade = (1.f - q) * (1.f - q);
        float o = Ease_Out(q) * (n->bright ? Near_Clamp(4.f * S, 0.6f, 1.8f) : Near_Clamp(1.6f * S, 0.3f, 0.8f));
        float gw = n->bright ? Near_Clamp(0.9f * S, 0.16f, 0.36f) : Near_Clamp(0.5f * S, 0.1f, 0.22f);
        GXColor gc = Color_Over(Color_Mix(n->base, color_white, 0.35f), (n->bright ? 0.95f : 0.45f) * fade);
        if (n->bright)
            Hud_Rect(hx - half - o, y0, hx + half + o, y1, Color_Over(n->base, 0.3f * fade));
        for (int side = -1; side <= 1; side += 2)
            Near_Bracket(hx + side * (half + o), y0, y1, side, foot, gw, gc);
    }
}

// A convex outline cut to what is at or below y (HUD y up), into ox and oy.
// Returns the points left: at most two more than there were.
static int Near_ClipBelow(const float *px, const float *py, int n, float y, float *ox, float *oy)
{
    int m = 0;
    for (int i = 0; i < n; i++)
    {
        int j = (i + 1) % n;
        int in_i = py[i] <= y, in_j = py[j] <= y;
        if (in_i)
        {
            ox[m] = px[i];
            oy[m] = py[i];
            m++;
        }
        if (in_i != in_j)
        {
            float t = (y - py[i]) / (py[j] - py[i]);
            ox[m] = px[i] + (px[j] - px[i]) * t;
            oy[m] = y;
            m++;
        }
    }
    return m;
}

// The ECB fill: Falcon's ECB (see Near_Ecb), the diamond the game lands with,
// drawn on him. It fills from its bottom point at a fixed rate and is full on
// the press frame, 12 frames after it started; ticks beside it mark the last
// three frames' levels. In the window its bottom point drops a spike to the
// floor where he will land (the cue's spot), which is what the interrupt is.
// A hit's ghost is a bigger, filled diamond; a miss shrinks it away in slate.
static void Near_EcbFill(FighterData *fp, NearCue *n)
{
    Vec2 v[4];
    Near_Ecb(fp, v);
    float x = fp->phys.pos.X, y = fp->phys.pos.Y;
    float px[4], py[4];
    for (int i = 0; i < 4; i++)
    {
        if (!Hud_FromWorld(x + v[i].X, y + v[i].Y, &px[i], &py[i]))
            return;
    }
    float S = Near_Scale(x, y + v[1].Y);
    float lw = Near_Clamp(0.6f * S, 0.18f, 0.32f);
    float mx = (px[0] + px[1] + px[2] + px[3]) / 4.f, my = (py[0] + py[1] + py[2] + py[3]) / 4.f;

    if (n->mode == NMODE_COUNT || n->mode == NMODE_WINDOW)
    {
        int window = n->mode == NMODE_WINDOW;
        float a = window ? 1.f : n->appear;
        float level = window ? 1.f : Clamp01(1.f - (float)n->k / NEAR_FILL);
        if (level > 0.f)
        {
            float ox[8], oy[8];
            int m = Near_ClipBelow(px, py, 4, py[2] + (py[0] - py[2]) * level, ox, oy);
            if (m >= 3)
            {
                float cx = 0.f, cy = 0.f;
                for (int i = 0; i < m; i++)
                {
                    cx += ox[i] / m;
                    cy += oy[i] / m;
                }
                Hud_Fan(cx, cy, ox, oy, m, Color_Over(window ? Color_Mix(n->col, color_white, n->now ? 0.5f : 0.f) : n->col, 0.72f * a));
            }
        }
        // a soft glow around the outline: the diamond is small and Falcon's
        // own colors run through it, so it needs to stand off him
        Hud_Line(px, py, 4, 1, lw + 0.55f, PX, Color_Over(n->col, 0.22f * a));
        Hud_Line(px, py, 4, 1, lw + 0.12f, PX, Color_Over(color_plate, 0.45f * a));
        Hud_Line(px, py, 4, 1, lw, PX, Color_Over(n->col, 0.8f * a));

        // ticks for the last three frames' levels, lit once the fill has reached them
        float xr = px[1] > px[3] ? px[1] : px[3];
        for (int j = 1; j <= 3; j++)
        {
            float ty = py[2] + (py[0] - py[2]) * (1.f - (float)j / NEAR_FILL);
            int lit = window || n->k <= j;
            Hud_Rect(xr + 0.25f, ty - 0.1f, xr + 0.95f, ty + 0.1f, Color_Over(color_plate, 0.5f * a));
            Hud_Rect(xr + 0.3f, ty - 0.05f, xr + 0.9f, ty + 0.05f, Color_Over(color_white, (lit ? 0.95f : 0.4f) * a));
        }

        if (window)
        {
            float sx, sy;
            if (Hud_FromWorld(n->c->spot.X, n->c->spot.Y, &sx, &sy) && sy < py[2] - 0.3f)
            {
                float bw = Near_Clamp(0.45f * S, 0.12f, 0.26f);
                Hud_Tri(px[2] - bw - 0.06f, py[2] + 0.06f, px[2] + bw + 0.06f, py[2] + 0.06f, sx, sy - 0.1f, Color_Over(color_plate, 0.5f));
                Hud_Tri(px[2] - bw, py[2], px[2] + bw, py[2], sx, sy, Color_Over(n->col, 1.f));
                Hud_Rect(sx - 0.5f, sy - 0.05f, sx + 0.5f, sy + 0.1f, Color_Over(n->now ? color_white : n->col, 1.f));
            }
        }
    }
    else if (n->mode == NMODE_FOLD || n->mode == NMODE_CUT)
    {
        float q = n->mode == NMODE_FOLD ? Clamp01((float)n->age / LL_FADE) : (float)n->age / LL_CUT;
        float k = n->mode == NMODE_FOLD ? 1.f - Ease_Out(q) : 1.f;
        float sx[4], sy[4];
        for (int i = 0; i < 4; i++)
        {
            sx[i] = mx + (px[i] - mx) * k;
            sy[i] = my + (py[i] - my) * k;
        }
        GXColor fc = n->mode == NMODE_FOLD ? Color_Over(n->col, 0.8f * (1.f - q)) : Color_Over(color_skip, 0.3f * (1.f - q));
        Hud_Line(sx, sy, 4, 1, lw + 0.12f, PX, Color_Over(color_plate, 0.3f * (1.f - q)));
        Hud_Line(sx, sy, 4, 1, lw, PX, fc);
    }

    if (n->ghost >= 0)
    {
        float q = (float)n->ghost / LL_GHOST, fade = (1.f - q) * (1.f - q);
        float g = 1.f + (n->bright ? 0.7f : 0.25f) * Ease_Out(q);
        float gx[4], gy[4];
        for (int i = 0; i < 4; i++)
        {
            gx[i] = mx + (px[i] - mx) * g;
            gy[i] = my + (py[i] - my) * g;
        }
        Quad_Add(gx[0], gy[0], gx[1], gy[1], gx[2], gy[2], gx[3], gy[3],
                 Color_Over(Color_Mix(n->base, color_white, 0.3f), (n->bright ? 0.5f : 0.18f) * fade));
        Hud_Line(gx, gy, 4, 1, lw, PX, Color_Over(Color_Mix(n->base, color_white, 0.4f), (n->bright ? 0.95f : 0.45f) * fade));
    }
}

// The count-in lights: 4 plus the window's width small lights in an arc over
// Falcon's head, on a dark track. A thin fuse burns down along the arc until
// 4 frames are left, then one white light comes on a frame; the window's
// lights, in the cue's color, come on as its frames go by. A light that is
// off is a dim fill in an outline of its color, so it still shows over a dark
// stage. A hit's ghost swells the window's lights; a miss shrinks every light
// away in slate. A window of more than 8 frames shows only its first 8.
static void Near_Lights(FighterData *fp, NearCue *n)
{
    float x = fp->phys.pos.X, y = fp->phys.pos.Y + body_offset + 14.5f;
    float hx, hy;
    if (!Hud_FromWorld(x, y, &hx, &hy))
        return;
    float S = Near_Scale(x, y);
    int lights = 4 + n->w;
    if (lights > NEAR_LIGHTS_MAX)
        lights = NEAR_LIGHTS_MAX;
    float R = Near_Clamp(13.f * S, 3.f, 9.f);    // the arc's radius
    float r = Near_Clamp(1.25f * S, 0.34f, 0.75f); // a light's
    float da = 2.9f * r / R;                     // the angle between two
    if ((lights - 1) * da > 2.8f)
        da = 2.8f / (lights - 1);
    float amax = (lights - 1) * da / 2.f;
    if (hy > SAFE_H - 1.2f - r)
        hy = SAFE_H - 1.2f - r; // keep it on the screen
    float cyc = hy - R;         // the arc's center, below

    float a = n->mode == NMODE_COUNT ? n->appear : 1.f;
    int live = n->mode == NMODE_COUNT || n->mode == NMODE_WINDOW;
    float q = n->mode == NMODE_FOLD ? Clamp01((float)n->age / LL_FADE) : n->mode == NMODE_CUT ? (float)n->age / LL_CUT : 0.f;
    if (live || n->mode == NMODE_FOLD || n->mode == NMODE_CUT)
        Near_Arc(hx, cyc, R, 3.f * r, NEAR_PI / 2.f - amax - da * 0.6f, NEAR_PI / 2.f + amax + da * 0.6f,
                 Color_Over(color_plate, (live ? 0.5f : 0.35f) * (1.f - q) * a));

    for (int i = 0; i < lights; i++)
    {
        float ang = (i - (lights - 1) / 2.f) * da;
        float lx = hx + R * sin(ang), ly = cyc + R * cos(ang);
        GXColor c = i < 4 ? color_white : n->col;
        if (live)
        {
            int on = n->mode == NMODE_WINDOW ? i <= 4 + n->age : (i < 4 && i <= 4 - n->k);
            if (on)
                Near_Dot(lx, ly, r, Color_Over(c, 1.f));
            else
            {
                Near_Dot(lx, ly, r * 1.1f, Color_Over(c, 0.6f * a));
                Near_Dot(lx, ly, r * 0.62f, Color_Over(Color_Mix(c, color_plate, 0.75f), 0.95f * a));
            }
        }
        else if (n->mode == NMODE_FOLD)
            Near_Dot(lx, ly, r * (1.f - Ease_Out(q)), Color_Over(n->col, 0.8f * (1.f - q)));
        else if (n->mode == NMODE_CUT)
            Near_Dot(lx, ly, r, Color_Over(color_skip, 0.3f * (1.f - q)));
    }

    // the fuse, along the arc just inside the lights, burning down from the right
    if (n->mode == NMODE_COUNT && n->k > 4)
    {
        float frac = Clamp01((n->k - 4) / 20.f);
        float rf = R - 2.2f * r;
        float a1 = NEAR_PI / 2.f + amax + da * 0.6f, a0 = a1 - frac * (2.f * amax + da * 1.2f);
        Near_Arc(hx, cyc, rf, 0.2f + 0.12f, a0, a1, Color_Over(color_plate, 0.45f * a));
        Near_Arc(hx, cyc, rf, 0.2f, a0, a1, Color_Over(color_white, 0.75f * a));
    }

    if (n->ghost >= 0)
    {
        float gq = (float)n->ghost / LL_GHOST, fade = (1.f - gq) * (1.f - gq);
        for (int i = 4; i < lights; i++)
        {
            float ang = (i - (lights - 1) / 2.f) * da;
            Near_Dot(hx + R * sin(ang), cyc + R * cos(ang), r * (1.f + (n->bright ? 1.6f : 0.6f) * Ease_Out(gq)),
                     Color_Over(Color_Mix(n->base, color_white, 0.3f), (n->bright ? 0.7f : 0.3f) * fade));
        }
    }
}

// The near-Falcon timers other than the old strip, in the look picked in the
// Timers menu. The bubble draws every cue that is live (and each kind's last
// ending) at its own spot; the other looks draw only the soonest cue (the
// smallest left; a tie goes to the AI, then the NIL, then the waveland) and
// its ending, or with none counting down, whichever ending is freshest. A
// ledge route that has the timer rows gets none of them.
static void Near_Draw(FighterData *fp, int look)
{
    if (route_rows_active)
        return;

    float base = vis_k;
    if (look == NEAR_BUBBLE)
    {
        for (int pass = 0; pass < 2; pass++)
        {
            for (int i = 0; i < CUE_NUM; i++)
            {
                vis_k = base * Fade_Factor(VG_TIMERS, i);
                if (Near_Shown(i))
                    Near_Bubble(i, pass ? &cue_live[i] : &cue_end[i]);
            }
        }
        vis_k = base;
        return;
    }

    // (a cue with nothing left to draw, like a press waiting for its touchdown
    // once its ghost is over, doesn't hold the others back)
    static const u8 order[CUE_NUM] = {CUE_AI, CUE_NIL, CUE_WL};
    NearCue n;
    int kind = -1, left = 0;
    for (int i = 0; i < CUE_NUM; i++)
    {
        Cue *c = &cue_live[order[i]];
        if (Near_Shown(order[i]) && Near_Cue(order[i], c, &n) && (kind < 0 || c->left < left))
        {
            kind = order[i];
            left = c->left;
        }
    }
    if (kind < 0)
    {
        int age = 0;
        for (int i = 0; i < CUE_NUM; i++)
        {
            Cue *e = &cue_end[order[i]];
            if (Near_Shown(order[i]) && Near_Cue(order[i], e, &n) && (kind < 0 || e->age < age))
            {
                kind = order[i];
                age = e->age;
            }
        }
    }
    if (kind < 0)
        return;

    vis_k = base * Fade_Factor(VG_TIMERS, kind);
    for (int pass = 0; pass < 2; pass++)
    {
        if (!Near_Cue(kind, pass ? &cue_live[kind] : &cue_end[kind], &n))
            continue;
        switch (look)
        {
        case NEAR_HALO:
            Near_Halo(fp, &n);
            break;
        case NEAR_PINCERS:
            Near_Pincers(fp, &n);
            break;
        case NEAR_ECB:
            Near_EcbFill(fp, &n);
            break;
        case NEAR_LIGHTS:
            Near_Lights(fp, &n);
            break;
        }
    }
    vis_k = base;
}

// The wavedash timers drawn at Falcon's feet (Pips, Ring); Cells is a row in
// the strips.
static void Wd_Draw(FighterData *fp)
{
    int look = Options_Timers[TOPT_WD].val;
    if ((look != WDT_PIPS && look != WDT_RING) || !Cues_Waveland())
        return;
    float base = vis_k;
    vis_k = base * Fade_Factor(VG_TIMERS, CUE_WL);
    // the last one's ending first, the live one over it
    for (int pass = 0; pass < 2; pass++)
    {
        Cue *c = pass ? &cue_live[CUE_WL] : &cue_end[CUE_WL];
        if (!c->phase || !c->wd)
            continue;
        if (look == WDT_PIPS)
            Wd_Pips(c, !pass);
        else
            Wd_Ring(c, !pass);
    }
    vis_k = base;
}

static void Spot_Draw(FighterData *fp)
{
    float base = vis_k;
    for (int pass = 0; pass < 2; pass++)
    {
        for (int i = 0; i < CUE_NUM; i++)
        {
            Cue *c = pass ? &cue_live[i] : &cue_end[i];
            if (!c->phase)
                continue;
            if (i == CUE_AI ? !Cues_Ai() : i == CUE_WL ? !Cues_Waveland() : !Cues_Nil())
                continue;
            vis_k = base * Fade_Factor(VG_TIMERS, i);
            if (i == CUE_WL)
            {
                if (Options_Timers[TOPT_WL].val != WLT_OFF)
                    Spot_Rails(c, fp, !pass);
            }
            else if (Options_Timers[TOPT_SPOT].val)
                Spot_Brackets(i, c, !pass);
        }
    }
    vis_k = base;
}

///////////////////////
/// Controller      ///
///////////////////////

// A compact picture of the whole controller, from the raw pad (what the
// player's hands did, before Melee's deadzones): the stick with its
// deadzone cross, trail and fastfall line, the C-stick, the face buttons
// and both triggers. A button stays lit for a few frames after it's let go,
// so a one-frame tap can still be seen.
//
// Laid out from the block's bottom left corner, in HUD units: 12.7 wide and
// 5.3 tall, or just the stick (5.2 square) with Buttons and Triggers off.
#define PAD_W 12.7f
#define PAD_H 5.3f
#define PAD_STICK_W 5.2f
#define PAD_STICK_H 5.2f
#define PAD_STICK_X 3.5f // the stick's center in the full block
#define PAD_R 2.6f       // the stick gate's radius
#define PCT_HALF_W 6.4f  // half a percent display's width, "%" and stock icon included
#define PCT_TOP 7.f      // its top above its HUD position

#define PAD_TRAIL 8
#define PAD_CTRAIL 4

// The Ring look: the whole controller in an ellipse 9.1 by 6.4 HUD units at
// Small, the room TM-CE's own controller model takes (lab.dat). L and R are
// the ellipse's halves, each a band from the gap at the top to the gap at
// the bottom that swells into a deep, pointed nub at the side. A press fills
// both ends of the band to the middle, then floods the nub; a click lights
// the whole half and keeps it lit while held. The stick's gate is in the
// middle with the buttons in the four bays around it, filled in the
// controller's own colors and lettered like the real ones: X and Y top left,
// Z top right, B bottom left, A bottom right, each in the middle of its bay
// (the widest circle that fits there). A press leaves a ghost of the button's
// own fill that grows a little and fades in a few frames. Below the gate the
// C-stick: four arrows for the direction the game reads as a smash or aerial,
// around a small live gate whose trail shows smash DI. Every edge is soft, so
// the small shapes don't look jagged.
#define RING_A 4.55f    // the ellipse's half width
#define RING_B 3.2f     // and half height
#define RING_BAND 0.3f  // the triggers' band, where a light press shows
#define RING_NUB 2.2f   // how far the nub reaches in past the band
#define RING_NUB_W 34.f // the nub's half width, in degrees around the ellipse
#define RING_GAP 7.f    // degrees left open at the top and bottom
#define RING_CH 0.5f    // the share of a trigger's travel that fills the band; the rest floods the nub
#define RING_LIGHT (43.f / 140.f) // a light press, where the game starts counting a trigger
#define RING_LEG_SEGS 4
#define RING_NUB_SEGS 12
#define RING_SEGS (2 * RING_LEG_SEGS + RING_NUB_SEGS)
#define RING_STICK_Y 0.35f // the stick gate's center, above the ellipse's
#define RING_STICK_R 1.45f
#define RING_C_Y -2.16f // the C-stick's center, clear of the gate above it
#define RING_C_R 0.4f

// The Crest look's size, in units of its drawing at size 1 (y up from its
// middle), and how much of a HUD unit that is: about the Ring's height.
#define CREST_K 0.92f
#define CREST_W 4.72f   // half its width, to the top blades' tips
#define CREST_TOP 3.4f  // its top above its middle
#define CREST_BOT 3.72f // its bottom below it

// The inputs that glow.
enum pad_input
{
    PIN_A,
    PIN_B,
    PIN_X,
    PIN_Y,
    PIN_Z,
    PIN_L,
    PIN_R,
    PIN_C, // the C-stick past the aerial threshold

    PIN_COUNT
};

// Where each input was drawn this frame, for the controller cues: its
// middle and about how big it is (r 0: not drawn).
static struct
{
    float x, y, r;
} pin_spot[PIN_COUNT];

static void Pin_Spot(int in, float x, float y, float r)
{
    pin_spot[in].x = x;
    pin_spot[in].y = y;
    pin_spot[in].r = r;
}

static Vec2 pad_trail[PAD_TRAIL];   // the last frames' raw stick, newest at pad_trail_pos
static int pad_trail_pos;
static Vec2 pad_ctrail[PAD_CTRAIL]; // and the C-stick's
static int pad_ctrail_pos;
static float pad_glow[PIN_COUNT];      // 1 while held, then fading
static float pad_glow_prev[PIN_COUNT]; // ... as it was a record ago
static float pad_flash[PIN_COUNT];     // 1 on the record it went down, then fading fast
static float pad_cdir_glow[4];         // the C-stick's arrows: right, up, left, down

static int assist_frozen; // Assist holds the game on the frame before an input

// The GameCube controller's own colors.
static const GXColor color_btn_a = {40, 200, 110, 255};
static const GXColor color_btn_b = {235, 55, 55, 255};
static const GXColor color_btn_xy = {205, 208, 220, 255};
static const GXColor color_btn_z = {125, 95, 255, 255};
static const GXColor color_btn_c = {255, 210, 40, 255};

static int pad_soft; // draw the gates with soft edges (the Ring look)

// Which inputs are down on a pad.
static void Pad_Held(HSD_Pad *pad, int *held)
{
    held[PIN_A] = (pad->held & HSD_BUTTON_A) != 0;
    held[PIN_B] = (pad->held & HSD_BUTTON_B) != 0;
    held[PIN_X] = (pad->held & HSD_BUTTON_X) != 0;
    held[PIN_Y] = (pad->held & HSD_BUTTON_Y) != 0;
    held[PIN_Z] = (pad->held & HSD_TRIGGER_Z) != 0;
    held[PIN_L] = (pad->held & HSD_TRIGGER_L) != 0;
    held[PIN_R] = (pad->held & HSD_TRIGGER_R) != 0;
    held[PIN_C] = fabs(pad->fsubstickX) >= common_aerial_stick_x || fabs(pad->fsubstickY) >= common_aerial_stick_y;
}

// The way the game reads a C-stick at (x, y) for a smash or an aerial: 0
// right, 1 up, 2 left, 3 down, or -1 short of it (the aerial thresholds and
// angle, as Aerial_Pressed uses them).
static int CStick_Dir(float x, float y)
{
    if (fabs(x) < common_aerial_stick_x && fabs(y) < common_aerial_stick_y)
        return -1;
    float angle = atan2(y, fabs(x));
    if (angle > common_aerial_angle)
        return 1;
    if (angle < -common_aerial_angle)
        return 3;
    return x >= 0 ? 0 : 2;
}

// Once per game frame, and per real frame while Assist holds the game: the
// pad's sticks into the trails, and its inputs into their glows.
static void Pad_Record(HSD_Pad *pad)
{
    pad_trail_pos = (pad_trail_pos + 1) % PAD_TRAIL;
    pad_trail[pad_trail_pos] = (Vec2){pad->fstickX, pad->fstickY};
    pad_ctrail_pos = (pad_ctrail_pos + 1) % PAD_CTRAIL;
    pad_ctrail[pad_ctrail_pos] = (Vec2){pad->fsubstickX, pad->fsubstickY};

    int held[PIN_COUNT];
    Pad_Held(pad, held);
    for (int i = 0; i < PIN_COUNT; i++)
    {
        pad_glow_prev[i] = pad_glow[i];
        float g = held[i] ? 1.f : pad_glow[i] * 0.55f;
        pad_glow[i] = g < 0.05f ? 0.f : g;
        float f = held[i] && pad_glow_prev[i] < 1.f ? 1.f : pad_flash[i] * 0.6f;
        pad_flash[i] = f < 0.04f ? 0.f : f;
    }
    int dir = CStick_Dir(pad->fsubstickX, pad->fsubstickY);
    for (int i = 0; i < 4; i++)
    {
        float g = i == dir ? 1.f : pad_cdir_glow[i] * 0.5f;
        pad_cdir_glow[i] = g < 0.04f ? 0.f : g;
    }
}

// The pad the display shows: the one the game reads, or while the game is
// paused the master pad, which still moves.
static HSD_Pad *Pad_Live(FighterData *fp)
{
    int paused = (stc_hsd_update->pause_kind & 1) || assist_frozen;
    return paused ? PadGetMaster(fp->pad_index) : PadGetEngine(fp->pad_index);
}

// Where the display goes: the bottom of the screen, in a corner or beside
// the player's percent. Other drawing keeps clear of this box.
static void Pad_Box(FighterData *fp, float *x0, float *y0, float *x1, float *y1)
{
    int buttons = Options_Hud[HOPT_BUTTONS].val;
    int look = buttons ? Options_Hud[HOPT_LOOK].val : -1;
    float rs = ring_sizes[Options_Hud[HOPT_PAD_SIZE].val];
    float w = look == LOOK_RING ? 2 * RING_A * rs : look == LOOK_CREST ? 2 * CREST_W * CREST_K * rs : buttons ? PAD_W : PAD_STICK_W;
    float h = look == LOOK_RING ? 2 * RING_B * rs : look == LOOK_CREST ? (CREST_TOP + CREST_BOT) * CREST_K * rs : buttons ? PAD_H : PAD_STICK_H;
    float left = SAFE_W - w, bottom = -SAFE_H + 0.4f;
    int place = Options_Hud[HOPT_STICK].val;
    if (place == STICK_LEFT)
        left = -SAFE_W;
    else if (place == STICK_PERCENT)
    {
        Vec3 *hp = Match_GetPlayerHUDPos(fp->ply);
        if (hp)
        {
            // every player's percent, so a CPU's isn't covered either
            Box pct[4];
            int n = 0;
            for (int i = 0; i < 4; i++)
            {
                Vec3 *o = Fighter_GetSlotType(i) != 3 ? Match_GetPlayerHUDPos(i) : 0;
                if (o)
                    pct[n++] = (Box){o->X - PCT_HALF_W, -SAFE_H - 1.f, o->X + PCT_HALF_W, o->Y + PCT_TOP};
            }
            // beside his percent, the side toward the middle first, then
            // above it; the first spot on screen that covers no percent
            float side = hp->X > 0.5f ? -1.f : 1.f;
            float cand[3][2] = {
                {side > 0 ? hp->X + PCT_HALF_W + 0.2f : hp->X - PCT_HALF_W - 0.2f - w, bottom},
                {side > 0 ? hp->X - PCT_HALF_W - 0.2f - w : hp->X + PCT_HALF_W + 0.2f, bottom},
                {hp->X - w / 2, hp->Y + PCT_TOP + 0.3f},
            };
            int pick = 2;
            for (int c = 0; c < 2 && pick == 2; c++)
            {
                Box b = {cand[c][0], cand[c][1], cand[c][0] + w, cand[c][1] + h};
                int clear = b.x0 >= -SAFE_W - 0.01f && b.x1 <= SAFE_W + 0.01f;
                for (int i = 0; i < n && clear; i++)
                    clear = !Box_Hit(&b, &pct[i]);
                if (clear)
                    pick = c;
            }
            left = cand[pick][0];
            bottom = cand[pick][1];
        }
    }
    // TM-CE's version text sits in the bottom right corner (TM_CreateWatermark:
    // right edge at x 615, top at y 446 of the 640 x 480 picture); a display
    // over it goes up above it
    Box wm = {(470 - 320) * PX, -SAFE_H - 1.f, (620 - 320) * PX, -(443 - 240) * PX};
    Box b = {left, bottom, left + w, bottom + h};
    if (Box_Hit(&b, &wm))
        bottom = wm.y1;
    *x0 = left;
    *y0 = bottom;
    *x1 = left + w;
    *y1 = *y0 + h;
}

// A stick's gate: an octagon with corners at the notches.
static void Pad_Gate(float cx, float cy, float R, float rim_w, GXColor rim)
{
    float vx[9], vy[9];
    for (int i = 0; i <= 8; i++)
    {
        float ang = (i % 8) * 0.7853982f;
        vx[i] = cx + cos(ang) * R;
        vy[i] = cy + sin(ang) * R;
    }
    GXColor plate = Color_Fill(color_plate, 0.6f);
    for (int i = 0; i < 8; i++)
        Hud_Tri(cx, cy, vx[i], vy[i], vx[i + 1], vy[i + 1], plate);
    if (pad_soft)
    {
        Hud_Line(vx, vy, 8, 1, rim_w, PX, rim);
        return;
    }
    for (int i = 0; i < 8; i++)
        Hud_Seg(vx[i], vy[i], vx[i + 1], vy[i + 1], rim_w, rim);
}

// A pill's ten corners: half circles joined by straight edges, its length
// turned rot radians from level.
static void Pill_Points(float cx, float cy, float w, float h, float rot, float *px, float *py)
{
    float r = h / 2, d = w / 2 - r;
    float cr = cos(rot), sr = sin(rot);
    for (int i = 0; i < 5; i++)
    {
        float ang = (-90 + 45 * i) * 0.01745329f;
        float x = d + cos(ang) * r, y = sin(ang) * r;
        px[i] = cx + x * cr - y * sr;
        py[i] = cy + x * sr + y * cr;
        px[5 + i] = cx - x * cr + y * sr;
        py[5 + i] = cy - x * sr - y * cr;
    }
}

static void Hud_Pill(float cx, float cy, float w, float h, float rot, GXColor c)
{
    float px[10], py[10];
    Pill_Points(cx, cy, w, h, rot, px, py);
    for (int i = 0; i < 10; i++)
        Hud_Tri(cx, cy, px[i], py[i], px[(i + 1) % 10], py[(i + 1) % 10], c);
}

static void Hud_PillRing(float cx, float cy, float w, float h, float rot, float lw, GXColor c)
{
    float px[10], py[10];
    Pill_Points(cx, cy, w, h, rot, px, py);
    for (int i = 0; i < 10; i++)
        Hud_Seg(px[i], py[i], px[(i + 1) % 10], py[(i + 1) % 10], lw, c);
}

// A round button: a faint outline, filled by its glow, and a bigger bright
// ring on the frame it goes down.
static void Pad_Button(float cx, float cy, float r, GXColor c, int in, float glow)
{
    Pin_Spot(in, cx, cy, r);
    Hud_Ring(cx, cy, r - 0.035f, 0.07f, Color_Fill(c, 0.5f));
    if (glow > 0)
        Hud_Disc(cx, cy, r - 0.07f, Color_Fill(c, glow));
    if (pad_glow[in] >= 1.f && pad_glow_prev[in] < 1.f)
        Hud_Ring(cx, cy, r + 0.2f, 0.1f, Color_Mix(c, color_white, 0.5f));
}

// A trigger: a bar filled from the bottom by how far it's pressed, and a cap
// above it that lights while the click is down. by is the block's bottom.
static void Pad_Trigger(float x, float by, float analog, float glow)
{
    GXColor c = land_kind_colors[LAND_PERFECT_WL];
    GXColor plate = Color_Fill(color_plate, 0.6f);
    float x1 = x + 0.5f;
    Hud_Rect(x, by + 0.2f, x1, by + 4.6f, plate);
    Hud_Frame(x, by + 0.2f, x1, by + 4.6f, 0.06f, Color_Fill(c, 0.4f));
    analog = Clamp01(analog);
    if (analog > 0)
        Hud_Rect(x + 0.06f, by + 0.26f, x1 - 0.06f, by + 0.26f + 4.28f * analog, Color_Fill(c, 0.7f));
    Hud_Rect(x, by + 4.8f, x1, by + 5.3f, plate);
    Hud_Frame(x, by + 4.8f, x1, by + 5.3f, 0.06f, Color_Fill(c, 0.4f));
    if (glow > 0)
        Hud_Rect(x + 0.06f, by + 4.86f, x1 - 0.06f, by + 5.24f, Color_Fill(c, glow));
}

// The shield drop zone: the stick angles that drop Falcon through a platform
// out of his shield, shaded in the gate while he stands or shields on one.
//
// Checked in the decomp:
// - The drop is ftCo_80099F1C (ftCo_Pass.c), reached through ftCo_8009A080:
//   L or R held, stick y <= -PlCo 0x464, the y timer (active_timer.lstick.y,
//   the frames since the stick left the tilt zone; timer_lstick_tilt_y here)
//   below PlCo 0x468, and mpColl_IsOnPlatform on the floor under him (the
//   line's platform flag, desc->is_unk). It never looks at x, so everything
//   below the line drops: the zone is a cap of the gate.
// - It's the last check in GuardOn's and Guard's IASA (ftCo_Guard.c). The
//   spotdodge is ahead of it (ftCo_8009980C, ftCo_Escape.c): stick y <=
//   PlCo 0x314 with that timer below PlCo 0x318, or the C-stick down as far.
//   A flick that gets that far down fast enough spotdodges and never drops.
//   The sideways roll check (ftCo_8009917C) is ahead of it too, but goes by
//   the x flick, so it isn't drawn.
// Inferred, not checked on a console:
// - The signs: 0x314 is compared as it is (so negative), 0x464 negated (so
//   positive). The decomp types 0x468 as a float where the other windows are
//   ints, so Common_Frames reads it either way. Event_Init logs all four.
// - Standing (Wait) and the two shield states are where to show it.
static int Pad_DropShown(FighterData *fp)
{
    int sid = fp->state_id;
    int id = fp->coll_data.ground_index;
    RawCollLine *lines = (RawCollLine *)*stc_collline;
    return Options_Hud[HOPT_SHIELD_DROP].val && fp->phys.air_state == 0 &&
           (sid == ASID_WAIT || sid == ASID_GUARDON || sid == ASID_GUARD) &&
           common_drop_stick > 0.3f && common_drop_stick < 1.f && // as read from the game
           lines && id >= 0 && lines[id].desc->is_unk;            // is_unk is the platform flag
}

// The part of the gate (an octagon) at or below the stick height lim (-1 to
// 1), tinted c at a, with a line along its top edge at la (none if 0). Cut
// like any convex polygon: the points that are below it, and where the edges
// that cross it do.
static void Pad_Cap(float cx, float cy, float R, float k, float lim, GXColor c, float a, float la)
{
    float vx[8], vy[8], px[10], py[10], ex[2];
    float y = cy + lim * R;
    Circle_Points(cx, cy, R, 8, vx, vy);
    int n = 0, e = 0;
    for (int i = 0; i < 8; i++)
    {
        int j = (i + 1) % 8;
        int in0 = vy[i] <= y, in1 = vy[j] <= y;
        if (in0)
        {
            px[n] = vx[i];
            py[n++] = vy[i];
        }
        if (in0 != in1 && e < 2)
        {
            ex[e] = vx[i] + (vx[j] - vx[i]) * (y - vy[i]) / (vy[j] - vy[i]);
            px[n] = ex[e++];
            py[n++] = y;
        }
    }
    if (e < 2 || n < 3)
        return;
    float mx = 0, my = 0;
    for (int i = 0; i < n; i++)
    {
        mx += px[i] / n;
        my += py[i] / n;
    }
    Hud_Fan(mx, my, px, py, n, Color_Over(c, a));
    if (la > 0)
        Hud_Seg(ex[0], y, ex[1], y, 0.06f * k, Color_Over(c, la));
}

// The zone in the stick's gate: blue where a press drops, with amber over
// where a flick down fast enough spotdodges instead (a slow one still drops
// there). Each brightens while the stick is doing it now, as the fastfall
// line does.
static void Pad_DropZone(FighterData *fp, float cx, float cy, float R, float k)
{
    if (!Pad_DropShown(fp))
        return;
    float y = fp->input.lstick.Y;
    int flick = (u8)fp->input.timer_lstick_tilt_y;
    int dodges = y <= common_spot_stick && flick < common_spot_window;
    int drops = y <= -common_drop_stick && flick < common_drop_window && !dodges;
    GXColor blue = {100, 170, 255, 255}, amber = {255, 175, 60, 255};
    Pad_Cap(cx, cy, R, k, -common_drop_stick, blue, drops ? 0.36f : 0.2f, drops ? 0.9f : 0.5f);
    if (common_spot_stick > -1.f && common_spot_stick < -0.3f)
    {
        // the two lines would sit on each other if the thresholds are close
        int apart = fabs(common_spot_stick + common_drop_stick) > 0.08f;
        Pad_Cap(cx, cy, R, k, common_spot_stick, amber, dodges ? 0.34f : 0.14f, !apart ? 0 : dodges ? 0.9f : 0.5f);
    }
}

// The stick: its gate, the band Melee reads as zero, the last frames as a
// trail, where the stick is, while falling the line where pulling down
// starts a fastfall (lit while a fastfall flick is live), and on a platform
// the shield drop zone. R is the gate's radius, k scales the marks inside it.
static void Pad_Stick(FighterData *fp, HSD_Pad *pad, float cx, float cy, float R, float k)
{
    Pad_Gate(cx, cy, R, 0.12f * k, Color_Fill(color_white, 0.45f));

    // the deadzone cross, where an axis reads as zero; fainter in the Ring
    // look, so the gate and the stick read first
    float rx = pad->fstickX, ry = pad->fstickY;
    float dzx = Common_Float(0x0), dzy = Common_Float(0x4);
    GXColor band = Color_Over(color_white, pad_soft ? 0.1f : 0.25f);
    float len = R * 0.88f;
    Hud_Rect(cx - dzx * R, cy - len, cx + dzx * R, cy + len, band);
    Hud_Rect(cx - len, cy - dzy * R, cx - dzx * R, cy + dzy * R, band);
    Hud_Rect(cx + dzx * R, cy - dzy * R, cx + len, cy + dzy * R, band);

    Pad_DropZone(fp, cx, cy, R, k);

    // fastfall line: lit on the frames the game takes a flick past it (it
    // has to be falling, not fastfalling yet, and not in an airdodge or a
    // Falcon Dive, ft_80084DB0), half lit while a fastfall is on
    if (fp->phys.air_state == 1)
    {
        int ts = Tracked_Index(fp->state_id);
        int can = fp->phys.self_vel.Y < 0 && !fp->flags.is_fastfall && ts != TS_ESCAPEAIR && !Tracked_IsUpB(ts);
        int ff = can && fp->input.lstick.Y <= -common_fastfall_stick && (u8)fp->input.timer_lstick_tilt_y < common_fastfall_window;
        GXColor c = ff ? color_white : Color_Over(color_white, fp->flags.is_fastfall ? 0.6f : 0.35f);
        float fy = cy - common_fastfall_stick * R;
        float fw = R * 0.62f;
        Hud_Rect(cx - fw, fy - 0.05f * k, cx + fw, fy + 0.05f * k, c);
        Glyph_Draw(GLYPH_FF, cx + fw + 0.3f * k, fy, 0.2f * k, c);
    }

    // trail, oldest first
    for (int n = PAD_TRAIL - 1; n >= 1; n--)
    {
        Vec2 *p = &pad_trail[(pad_trail_pos - n + PAD_TRAIL) % PAD_TRAIL];
        float a = 0.5f * (1.f - (float)n / PAD_TRAIL);
        float r = (0.14f + 0.12f * (1.f - (float)n / PAD_TRAIL)) * k;
        Hud_Rect(cx + p->X * R - r, cy + p->Y * R - r, cx + p->X * R + r, cy + p->Y * R + r, Color_Fill(color_white, a));
    }
    float dx = cx + rx * R, dy = cy + ry * R;
    Hud_Seg(cx, cy, dx, dy, 0.14f * k, Color_Fill(color_white, 0.8f));
    Hud_Rect(dx - 0.28f * k, dy - 0.28f * k, dx + 0.28f * k, dy + 0.28f * k, color_white);

    // where the game reads it, when that isn't where it is
    float gx = fabs(rx) <= dzx ? 0.f : rx;
    float gy = fabs(ry) <= dzy ? 0.f : ry;
    if (gx != rx || gy != ry)
        Hud_Ring(cx + gx * R, cy + gy * R, 0.4f * k, 0.09f * k, Color_Fill(color_white, 0.85f));
}

// The C-stick: a small gate with a dot. Dot and rim turn yellow past where
// it throws an aerial (aerial is how much, 1 while it does and fading after).
static void Pad_CStick(HSD_Pad *pad, float cx, float cy, float aerial)
{
    float R = 1.15f;
    GXColor pink = color_btn_c;
    GXColor c = Color_Mix(color_white, pink, aerial);
    Pad_Gate(cx, cy, R, 0.09f, Color_Mix(Color_Fill(color_white, 0.45f), pink, aerial));
    for (int n = PAD_CTRAIL - 1; n >= 1; n--)
    {
        Vec2 *p = &pad_ctrail[(pad_ctrail_pos - n + PAD_CTRAIL) % PAD_CTRAIL];
        float a = 0.5f * (1.f - (float)n / PAD_CTRAIL);
        float r = 0.07f + 0.07f * (1.f - (float)n / PAD_CTRAIL);
        Hud_Rect(cx + p->X * R - r, cy + p->Y * R - r, cx + p->X * R + r, cy + p->Y * R + r, Color_Fill(color_white, a));
    }
    float dx = cx + pad->fsubstickX * R, dy = cy + pad->fsubstickY * R;
    Hud_Rect(dx - 0.17f, dy - 0.17f, dx + 0.17f, dy + 0.17f, c);
}

// The nub's depth past the band, t degrees from its middle: a smooth bell,
// since a point flares into a spike where the soft outline turns.
static float Ring_Swell(float t)
{
    float u = fabs(t) / RING_NUB_W;
    if (u >= 1)
        return 0;
    float sh = 1 - u * u;
    sh *= sh;
    return RING_NUB * sh * sh;
}

// A point of the ring's left half at size s: t degrees around the ellipse
// (180 is the middle of the left side), off units in from its edge. The
// right half is its mirror.
static void Ring_Point(float t, float off, float s, float *x, float *y)
{
    float a = t * 0.01745329f;
    float c = cos(a), sn = sin(a);
    float nx = c / RING_A, ny = sn / RING_B, m = sqrtf(nx * nx + ny * ny);
    *x = (RING_A * c - nx / m * off) * s;
    *y = (RING_B * sn - ny / m * off) * s;
}

// A point on the inner edge of the left half, pushed d units further in
// toward the middle (the nub): straight across rather than along the
// ellipse's normal, which folds over itself once it's deeper than the
// ellipse's curve at the side is round.
static void Ring_Inner(float t, float d, float s, float *x, float *y)
{
    Ring_Point(t, RING_BAND, s, x, y);
    *x += d * s;
}

// The angle of the k-th sample along a half, from its top end (k = 0) to its
// bottom end, closer together across the nub.
static float Ring_Angle(int k)
{
    float top = 90 + RING_GAP, n0 = 180 - RING_NUB_W, n1 = 180 + RING_NUB_W, bot = 270 - RING_GAP;
    if (k <= RING_LEG_SEGS)
        return top + (n0 - top) * k / RING_LEG_SEGS;
    k -= RING_LEG_SEGS;
    if (k <= RING_NUB_SEGS)
        return n0 + (n1 - n0) * k / RING_NUB_SEGS;
    k -= RING_NUB_SEGS;
    return n1 + (bot - n1) * k / RING_LEG_SEGS;
}

// A quad of a half between the angles t0 and t1, from the offset o0 in to the
// band's inner edge pushed d further in (each given at both angles). m is 1
// for the left half and -1 for the right one.
static void Ring_Quad(float cx, float cy, float s, float m, float t0, float t1, float o0a, float o0b, float d1a, float d1b, GXColor c)
{
    float x[4], y[4];
    Ring_Point(t0, o0a, s, &x[0], &y[0]);
    Ring_Point(t1, o0b, s, &x[1], &y[1]);
    Ring_Inner(t1, d1b, s, &x[2], &y[2]);
    Ring_Inner(t0, d1a, s, &x[3], &y[3]);
    Quad_Add(cx + m * x[0], cy + y[0], cx + m * x[1], cy + y[1], cx + m * x[2], cy + y[2], cx + m * x[3], cy + y[3], c);
}

// One trigger as a half of the ring around (cx, cy): side -1 is L, 1 is R.
// analog is how far it's pressed, click whether it's clicked, lit how lit the
// click leaves it (1 while it's held, fading after), flash the press's flash.
static void Ring_Trigger(float cx, float cy, float s, int side, float analog, int click, float lit, float flash)
{
    GXColor c = color_in_dodge;
    float m = -side;
    float px[2 * RING_SEGS + 2], py[2 * RING_SEGS + 2]; // its outline: down the outside, up the inside
    int n = 2 * RING_SEGS + 2;
    for (int k = 0; k <= RING_SEGS; k++)
    {
        float t = Ring_Angle(k);
        int o = k, i = n - 1 - k;
        Ring_Point(t, 0, s, &px[o], &py[o]);
        Ring_Inner(t, Ring_Swell(t - 180), s, &px[i], &py[i]);
        px[o] = cx + m * px[o];
        py[o] += cy;
        px[i] = cx + m * px[i];
        py[i] += cy;
    }
    GXColor plate = Color_Fill(color_plate, 0.55f);
    for (int k = 0; k < RING_SEGS; k++)
        Quad_Add(px[k], py[k], px[k + 1], py[k + 1], px[n - 2 - k], py[n - 2 - k], px[n - 1 - k], py[n - 1 - k], plate);
    {
        float mx = 0, my = 0;
        for (int k = 0; k < n; k++)
        {
            mx += px[k];
            my += py[k];
        }
        Pin_Spot(side > 0 ? PIN_R : PIN_L, mx / n, my / n, 0.8f * s);
    }
    // the outline, under the fill so a light press isn't hidden by it; a
    // lit half has a bright, heavier one
    Hud_Line(px, py, n, 1, (0.05f + 0.04f * lit) * s, PX, Color_Mix(Color_Fill(c, 0.8f), color_white, lit));

    // the fill: both ends of the band run in to the middle, then the nub
    // floods from its base to its tip
    float a = click ? 1.f : Clamp01(analog);
    float ac = Clamp01(a / RING_CH), as = Clamp01((a - RING_CH) / (1 - RING_CH));
    GXColor fc = Color_Fill(c, 0.6f + 0.4f * a);
    if (ac > 0.002f)
    {
        float top = 90 + RING_GAP, bot = 270 - RING_GAP;
        float f0 = top + (180 - top) * ac, f1 = bot - (bot - 180) * ac;
        for (int k = 0; k < RING_SEGS; k++)
        {
            float t0 = Ring_Angle(k), t1 = Ring_Angle(k + 1);
            if (t0 < f0)
                Ring_Quad(cx, cy, s, m, t0, t1 < f0 ? t1 : f0, 0, 0, 0, 0, fc);
            if (t1 > f1)
                Ring_Quad(cx, cy, s, m, t0 > f1 ? t0 : f1, t1, 0, 0, 0, 0, fc);
        }
    }
    if (as > 0.002f)
    {
        float level = as * RING_NUB;
        for (int k = RING_LEG_SEGS; k < RING_LEG_SEGS + RING_NUB_SEGS; k++)
        {
            float t0 = Ring_Angle(k), t1 = Ring_Angle(k + 1);
            float d0 = Ring_Swell(t0 - 180), d1 = Ring_Swell(t1 - 180);
            Ring_Quad(cx, cy, s, m, t0, t1, RING_BAND * 0.5f, RING_BAND * 0.5f, d0 < level ? d0 : level, d1 < level ? d1 : level, fc);
        }
    }

    // a mark on each way in where a light press counts: a short bar across
    // the band with a pointer under it
    for (int e = 0; e < 2; e++)
    {
        float end = e ? 270 - RING_GAP : 90 + RING_GAP;
        float t = end + (180 - end) * (RING_LIGHT / RING_CH);
        float tx[2], ty[2];
        Ring_Point(t, 0.03f, s, &tx[0], &ty[0]);
        Ring_Point(t, RING_BAND - 0.03f, s, &tx[1], &ty[1]);
        tx[0] = cx + m * tx[0];
        tx[1] = cx + m * tx[1];
        ty[0] += cy;
        ty[1] += cy;
        Hud_Line(tx, ty, 2, 0, 0.07f * s, PX, Color_Over(color_white, 0.95f));
    }

    // a click lights the whole half, as bright as the flash it starts with,
    // and keeps it so while it's held (the flash is the same light, brighter,
    // settling into it)
    float w = 0.68f * lit + (1.f - 0.68f * lit) * flash;
    if (w > 0)
    {
        GXColor fl = Color_Over(color_white, w);
        for (int k = 0; k < RING_SEGS; k++)
            Quad_Add(px[k], py[k], px[k + 1], py[k + 1], px[n - 2 - k], py[n - 2 - k], px[n - 1 - k], py[n - 1 - k], fl);
    }
}

// The dark plate behind the whole ring, so its outlines read over any stage.
static void Ring_Backing(float cx, float cy, float s)
{
    float px[32], py[32];
    for (int i = 0; i < 32; i++)
    {
        float ang = i * (6.2831853f / 32);
        px[i] = cx + cos(ang) * RING_A * s;
        py[i] = cy + sin(ang) * RING_B * s;
    }
    Hud_Fan(cx, cy, px, py, 32, Color_Fill(color_plate, 0.45f)); // the triggers' outlines soften its edge
}

// A button's fill: its own color at rest, lighter and fully opaque the more
// it's pressed.
static GXColor Ring_Lit(GXColor c, float glow)
{
    // up: a dark well in the button's color, its rim and letter in the
    // color; down: filled solid and bright, the way a trigger fills
    return Color_Mix(Color_Over(Color_Mix(c, color_plate, 0.72f), 0.88f), Color_Over(Color_Mix(c, color_white, 0.3f), 1.f), glow);
}

// A button's outline, lighter than its fill.
static GXColor Ring_Edge(GXColor c)
{
    return Color_Over(Color_Mix(c, color_white, 0.35f), 0.9f);
}

// The ghost a press leaves, a copy of the button's fill, from the button's
// flash (1 on the frame it went down, times 0.6 on each after, so a few
// frames in all): how far it has grown (1 and up) ...
static float Ghost_Grow(float flash)
{
    return 1.15f + 0.2f * (1.f - flash);
}

#define GHOST_MIN 0.1f

// ... and fading to nothing as the flash reaches GHOST_MIN
static GXColor Ghost_Color(GXColor c, float flash)
{
    return Color_Over(Color_Mix(c, color_white, 0.45f), 0.9f * sqrtf((flash - GHOST_MIN) / (1.f - GHOST_MIN)));
}

// One stroke of a button's letter: n points in units of the letter's height h,
// from -0.5 to 0.5 each way, around (cx, cy).
static void Ring_Stroke(float cx, float cy, float h, GXColor c, int n, const float *xy)
{
    float px[7], py[7];
    for (int i = 0; i < n; i++)
    {
        px[i] = cx + xy[2 * i] * h;
        py[i] = cy + xy[2 * i + 1] * h;
    }
    Hud_Line(px, py, n, 0, (0.17f * h > 0.095f ? 0.17f * h : 0.095f), PX * 0.6f, c);
}

// The letter printed on a button, in strokes since there's no text engine.
static void Ring_Letter(int in, float cx, float cy, float h, GXColor bc, float glow)
{
    Pin_Spot(in, cx, cy, h);
    // light in the button's color on the dark well, dark on the lit fill
    GXColor c = Color_Mix(Color_Over(Color_Mix(bc, color_white, 0.5f), 0.95f), Color_Over(color_plate, 0.92f), glow);
    switch (in)
    {
    case PIN_A:
    {
        float legs[] = {-0.34f, -0.5f, 0, 0.5f, 0.34f, -0.5f};
        float bar[] = {-0.2f, -0.14f, 0.2f, -0.14f};
        Ring_Stroke(cx, cy, h, c, 3, legs);
        Ring_Stroke(cx, cy, h, c, 2, bar);
        break;
    }
    case PIN_B:
    {
        float top[] = {-0.3f, -0.5f, -0.3f, 0.5f, 0.08f, 0.5f, 0.3f, 0.36f, 0.3f, 0.16f, 0.08f, 0, -0.3f, 0};
        float low[] = {0.08f, 0, 0.34f, -0.14f, 0.34f, -0.36f, 0.08f, -0.5f, -0.3f, -0.5f};
        Ring_Stroke(cx, cy, h, c, 7, top);
        Ring_Stroke(cx, cy, h, c, 5, low);
        break;
    }
    case PIN_X:
    {
        float down[] = {-0.32f, 0.5f, 0.32f, -0.5f};
        float up[] = {-0.32f, -0.5f, 0.32f, 0.5f};
        Ring_Stroke(cx, cy, h, c, 2, down);
        Ring_Stroke(cx, cy, h, c, 2, up);
        break;
    }
    case PIN_Y:
    {
        float arms[] = {-0.34f, 0.5f, 0, 0.02f, 0.34f, 0.5f};
        float stem[] = {0, 0.02f, 0, -0.5f};
        Ring_Stroke(cx, cy, h, c, 3, arms);
        Ring_Stroke(cx, cy, h, c, 2, stem);
        break;
    }
    case PIN_Z:
    {
        float z[] = {-0.32f, 0.5f, 0.32f, 0.5f, -0.32f, -0.5f, 0.32f, -0.5f};
        Ring_Stroke(cx, cy, h, c, 4, z);
        break;
    }
    }
}

// A round button: filled in its color, lettered, with a soft outline, and on
// a press a ghost of its fill that grows a little and fades in a few frames.
static void Ring_Button(float cx, float cy, float r, float s, GXColor c, int in, float glow)
{
    float px[18], py[18];
    float flash = pad_flash[in];
    if (flash > GHOST_MIN)
    {
        Circle_Points(cx, cy, r * Ghost_Grow(flash), 18, px, py);
        Hud_Fan(cx, cy, px, py, 18, Ghost_Color(c, flash));
    }
    Circle_Points(cx, cy, r, 18, px, py);
    Hud_Fan(cx, cy, px, py, 18, Ring_Lit(c, glow));
    Hud_Line(px, py, 18, 1, 0.07f * s, PX, Ring_Edge(c));
    Ring_Letter(in, cx, cy, 0.95f * r, c, glow);
}

// A kidney, the shape of the X and Y buttons (and here Z): an arc rho from
// the middle of its line (mx, my) bulging toward dir degrees, span degrees
// long and w thick with round ends.
#define KIDNEY_ARC 6
#define KIDNEY_CAP 4
#define KIDNEY_POINTS (2 * KIDNEY_ARC + 2 * KIDNEY_CAP)

// Its outline into px and py, and the centers of its round ends into ends.
static void Kidney_Points(float mx, float my, float dir, float rho, float span, float w, float *px, float *py, float *ends)
{
    float d2r = 0.01745329f;
    float cx = mx - cos(dir * d2r) * rho, cy = my - sin(dir * d2r) * rho; // the arc's center
    float a0 = dir - span / 2, a1 = dir + span / 2, h = w / 2;
    int n = 0;
    for (int i = 0; i <= KIDNEY_ARC; i++) // the outside, a0 to a1
    {
        float a = (a0 + (a1 - a0) * i / KIDNEY_ARC) * d2r;
        px[n] = cx + cos(a) * (rho + h);
        py[n++] = cy + sin(a) * (rho + h);
    }
    float e = a1 * d2r;
    ends[0] = cx + cos(e) * rho;
    ends[1] = cy + sin(e) * rho;
    for (int i = 1; i < KIDNEY_CAP; i++) // round end at a1
    {
        float a = e + 3.1415927f * i / KIDNEY_CAP;
        px[n] = ends[0] + cos(a) * h;
        py[n++] = ends[1] + sin(a) * h;
    }
    for (int i = 0; i <= KIDNEY_ARC; i++) // the inside, a1 back to a0
    {
        float a = (a1 - (a1 - a0) * i / KIDNEY_ARC) * d2r;
        px[n] = cx + cos(a) * (rho - h);
        py[n++] = cy + sin(a) * (rho - h);
    }
    float b = a0 * d2r;
    ends[2] = cx + cos(b) * rho;
    ends[3] = cy + sin(b) * rho;
    for (int i = 1; i < KIDNEY_CAP; i++) // round end at a0
    {
        float a = b + 3.1415927f + 3.1415927f * i / KIDNEY_CAP;
        px[n] = ends[2] + cos(a) * h;
        py[n++] = ends[3] + sin(a) * h;
    }
}

static void Kidney_Fill(const float *px, const float *py, const float *ends, GXColor f)
{
    for (int i = 0; i < KIDNEY_ARC; i++)
    {
        int o = i, in0 = 2 * KIDNEY_ARC + KIDNEY_CAP - i; // the inside point at the same angle
        Quad_Add(px[o], py[o], px[o + 1], py[o + 1], px[in0 - 1], py[in0 - 1], px[in0], py[in0], f);
    }
    for (int i = 0; i < KIDNEY_CAP; i++)
    {
        int j = KIDNEY_ARC + i;
        Hud_Tri(ends[0], ends[1], px[j], py[j], px[j + 1], py[j + 1], f);
        j = 2 * KIDNEY_ARC + KIDNEY_CAP + i;
        Hud_Tri(ends[2], ends[3], px[j], py[j], px[(j + 1) % KIDNEY_POINTS], py[(j + 1) % KIDNEY_POINTS], f);
    }
}

// Filled in its color, lettered, with a soft outline and the press ghost.
static void Ring_Kidney(float mx, float my, float dir, float rho, float span, float w, float s, GXColor c, int in, float glow)
{
    float px[KIDNEY_POINTS], py[KIDNEY_POINTS], ends[4];
    float flash = pad_flash[in];
    if (flash > GHOST_MIN)
    {
        // the same shape grown about its middle
        Kidney_Points(mx, my, dir, rho * Ghost_Grow(flash), span, w * Ghost_Grow(flash), px, py, ends);
        Kidney_Fill(px, py, ends, Ghost_Color(c, flash));
    }
    Kidney_Points(mx, my, dir, rho, span, w, px, py, ends);
    Kidney_Fill(px, py, ends, Ring_Lit(c, glow));
    Hud_Line(px, py, KIDNEY_POINTS, 1, 0.07f * s, PX, Ring_Edge(c));
    // Z's bean runs diagonally under its letter, so the letter's corners
    // reach the edges sooner
    Ring_Letter(in, mx, my, (in == PIN_Z ? 0.64f : 0.85f) * w, c, glow);
}

// The Ring look's C-stick around (cx, cy): the four arrows, lit the way the
// game reads it and fading after, and the live gate with its trail.
static void Ring_CStick(HSD_Pad *pad, float cx, float cy, float s)
{
    GXColor yel = color_btn_c;
    int dir = CStick_Dir(pad->fsubstickX, pad->fsubstickY);
    for (int d = 0; d < 4; d++)
    {
        float g = d == dir ? 1.f : pad_cdir_glow[d];
        int level = d % 2 == 0; // left and right have more room
        float base = (level ? 0.58f : 0.52f) * s;
        float len = (level ? 0.26f : 0.22f) * s, half = (level ? 0.23f : 0.2f) * s;
        float ux = d == 0 ? 1 : d == 2 ? -1 : 0, uy = d == 1 ? 1 : d == 3 ? -1 : 0;
        float ax[3] = {cx + ux * base - uy * half, cx + ux * (base + len), cx + ux * base + uy * half};
        float ay[3] = {cy + uy * base + ux * half, cy + uy * (base + len), cy + uy * base - ux * half};
        Hud_Tri(ax[0], ay[0], ax[1], ay[1], ax[2], ay[2], Ring_Lit(yel, g));
        Hud_Line(ax, ay, 3, 1, 0.06f * s, PX, Ring_Edge(yel));
    }
    float R = RING_C_R * s;
    float vx[8], vy[8];
    Circle_Points(cx, cy, R, 8, vx, vy);
    Hud_Fan(cx, cy, vx, vy, 8, Color_Over(yel, 0.3f)); // the stick is yellow too
    Pad_Gate(cx, cy, R, 0.06f * s, Color_Over(yel, 0.8f));
    for (int n = PAD_CTRAIL - 1; n >= 1; n--)
    {
        Vec2 *p = &pad_ctrail[(pad_ctrail_pos - n + PAD_CTRAIL) % PAD_CTRAIL];
        float a = 0.85f * (1.f - (float)n / PAD_CTRAIL);
        float r = (0.04f + 0.05f * (1.f - (float)n / PAD_CTRAIL)) * s;
        Hud_Rect(cx + p->X * R - r, cy + p->Y * R - r, cx + p->X * R + r, cy + p->Y * R + r, Color_Fill(yel, a));
    }
    float dx = cx + pad->fsubstickX * R, dy = cy + pad->fsubstickY * R, h = 0.13f * s;
    Hud_Rect(dx - h, dy - h, dx + h, dy + h, color_white);
}

// The Ring look, from the box's bottom left corner.
static void Ring_Draw(FighterData *fp, HSD_Pad *pad, float bx, float by)
{
    float s = ring_sizes[Options_Hud[HOPT_PAD_SIZE].val];
    float cx = bx + RING_A * s, cy = by + RING_B * s;

    int held[PIN_COUNT];
    Pad_Held(pad, held);
    float glow[PIN_COUNT];
    for (int i = 0; i < PIN_COUNT; i++)
        glow[i] = held[i] ? 1.f : pad_glow[i];

    pad_soft = 1;
    Ring_Backing(cx, cy, s);
    Ring_Trigger(cx, cy, s, -1, pad->ftriggerLeft, held[PIN_L], glow[PIN_L], pad_flash[PIN_L]);
    Ring_Trigger(cx, cy, s, 1, pad->ftriggerRight, held[PIN_R], glow[PIN_R], pad_flash[PIN_R]);

    // the stick in the middle, the buttons in the bays around it, each in
    // the middle of its bay (where its outline keeps the most room)
    float sx = cx, sy = cy + RING_STICK_Y * s;
    Pad_Stick(fp, pad, sx, sy, RING_STICK_R * s, 0.72f * s);
    float tx = 2.22f * s, ty = cy + 1.26f * s, lx = 1.92f * s, ly = cy - 1.38f * s;
    // Y left of X and a little above it, the same bean mirrored
    Ring_Kidney(cx - tx - 0.55f * s, ty + 0.1f * s, 180, 0.8f * s, 60, 0.5f * s, s, color_btn_xy, PIN_Y, glow[PIN_Y]);
    Ring_Kidney(cx - tx + 0.55f * s, ty - 0.1f * s, 0, 0.8f * s, 60, 0.5f * s, s, color_btn_xy, PIN_X, glow[PIN_X]);
    // Z well above the pair's middle: its bean hangs down to the right, so
    // its weight lines up with theirs only when its top is a little above Y's
    Ring_Kidney(cx + tx, ty + 0.4f * s, 45, 0.9f * s, 65, 0.55f * s, s, color_btn_z, PIN_Z, glow[PIN_Z]);
    Ring_Button(cx + lx, ly, 0.68f * s, s, color_btn_a, PIN_A, glow[PIN_A]);
    Ring_Button(cx - lx, ly, 0.5f * s, s, color_btn_b, PIN_B, glow[PIN_B]);

    Ring_CStick(pad, cx, cy + RING_C_Y * s, s);
    pad_soft = 0;
}


// The Crest look: the controller as a crest, a shield with a wing off each
// shoulder and the C-stick for a tail. Each wing is a trigger: four swept
// blades, their upper edges curved like the trigger's finger groove. A press
// fills every blade from its tip in toward the shoulder, the top blade
// leading: gray until the game counts the press, cyan after, with a notch
// across each blade where that happens. The click lights the spine at the
// wing's root and, as in the Ring, the whole wing. The buttons are cut gems
// in the controller's colors: long pointed X and Y stacked on the left, Z on
// the right, round B and A at the bottom, each with a lighter table. Units
// are the drawing's (see CREST_K), the left wing built and the right one its
// mirror.
#define CREST_ROOT 1.95f   // the wings' roots, either side of the middle
#define CREST_BLADE_H 0.5f // a blade's height at its root
#define CREST_SEGS 6       // segments along a blade
#define CREST_LEAD 1.24f   // the top blade's fill runs this far ahead of the press
#define CREST_LAG 0.08f    // and each blade under it this much behind the one above

static const float crest_blades[4][3] = {
    // the root's top, the sweep in degrees, the length
    {2.05f, 150, 3.2f},
    {1.4f, 158, 2.8f},
    {0.75f, 166, 2.4f},
    {0.1f, 174, 2.f},
};

// The shield behind it all, clockwise from its top left; a fan from its
// middle covers it.
static const float crest_plate[][2] = {
    {-1.9f, 2.45f}, {1.9f, 2.45f}, {2.05f, -0.35f}, {3.75f, -0.5f}, {3.1f, -2.62f}, {1.15f, -3.3f},
    {0, -3.72f}, {-1.15f, -3.3f}, {-3.1f, -2.62f}, {-3.75f, -0.5f}, {-2.05f, -0.35f},
};

// The spine at the left wing's root, with a talon at its foot.
static const float crest_spine[6][2] = {
    {-1.93f, 2.2f}, {-1.65f, 2.05f}, {-1.65f, -0.25f}, {-1.9f, -0.85f}, {-2.07f, -0.2f}, {-2.07f, 2.05f},
};

// A point of blade i on its top edge (or its bottom one), u of the way from
// its root (0) to its tip (1), in HUD units: m is -1 for the left wing and 1
// for the right one, k the drawing's scale.
static void Crest_BladePoint(int i, float u, int top, float m, float k, float cx, float cy, float *x, float *y)
{
    float yt = crest_blades[i][0], ang = crest_blades[i][1] * 0.01745329f, len = crest_blades[i][2];
    float dx = cos(ang), dy = sin(ang);
    float tx = -CREST_ROOT + dx * len, ty = yt - CREST_BLADE_H / 2 + dy * len;
    float rx = -CREST_ROOT, ry = top ? yt : yt - CREST_BLADE_H;
    float px = rx + (tx - rx) * u, py = ry + (ty - ry) * u;
    // both edges swell a little between root and tip, the top one more and
    // nearer the root, like the trigger's dish
    if (top)
    {
        float d = 0.16f * sin(3.1415927f * sqrtf(u) * sqrtf(sqrtf(u)));
        px += dy * d;
        py -= dx * d;
    }
    else
    {
        float d = 0.07f * sin(3.1415927f * u);
        px += dy * d;
        py += dx * d;
    }
    *x = cx - m * px * k;
    *y = cy + py * k;
}

// The blade between u0 and u1 along it, as quads between its edges.
static void Crest_BladeQuads(int i, float u0, float u1, float m, float k, float cx, float cy, GXColor c)
{
    float tx0, ty0, bx0, by0;
    Crest_BladePoint(i, u0, 1, m, k, cx, cy, &tx0, &ty0);
    Crest_BladePoint(i, u0, 0, m, k, cx, cy, &bx0, &by0);
    for (int j = 1; j <= CREST_SEGS; j++)
    {
        float u = u0 + (u1 - u0) * j / CREST_SEGS, tx1, ty1, bx1, by1;
        Crest_BladePoint(i, u, 1, m, k, cx, cy, &tx1, &ty1);
        Crest_BladePoint(i, u, 0, m, k, cx, cy, &bx1, &by1);
        Quad_Add(tx0, ty0, tx1, ty1, bx1, by1, bx0, by0, c);
        tx0 = tx1, ty0 = ty1, bx0 = bx1, by0 = by1;
    }
}

// One wing: side -1 is L, 1 is R; analog, click, lit and flash as for the Ring.
static void Crest_Wing(float cx, float cy, float k, int side, float analog, int click, float lit, float flash)
{
    {
        float mx, my;
        Crest_BladePoint(1, 0.5f, 1, side, k, cx, cy, &mx, &my);
        Pin_Spot(side > 0 ? PIN_R : PIN_L, mx, my, 0.9f * k);
    }
    float m = side;
    float a = click ? 1.f : Clamp01(analog);
    int counted = a >= RING_LIGHT;
    GXColor cyan = color_in_dodge;
    GXColor fc = click ? Color_Over(Color_Mix(cyan, color_white, 0.5f), 1.f) : counted ? Color_Fill(cyan, 0.6f + 0.35f * a) : Color_Over(color_skip, 0.55f);
    float w = 0.68f * lit + (1.f - 0.68f * lit) * flash;
    for (int i = 0; i < 4; i++)
    {
        // its outline: out along the top edge, back along the bottom one
        float px[2 * CREST_SEGS + 2], py[2 * CREST_SEGS + 2];
        int n = 2 * CREST_SEGS + 2;
        for (int j = 0; j <= CREST_SEGS; j++)
        {
            float u = (float)j / CREST_SEGS;
            Crest_BladePoint(i, u, 1, m, k, cx, cy, &px[j], &py[j]);
            Crest_BladePoint(i, u, 0, m, k, cx, cy, &px[n - 1 - j], &py[n - 1 - j]);
        }
        Crest_BladeQuads(i, 0, 1, m, k, cx, cy, Color_Fill(color_plate, 0.62f));
        // a lighter facet along the top edge, an edge that catches the light
        for (int j = 0; j < CREST_SEGS; j++)
        {
            int b0 = n - 1 - j, b1 = n - 2 - j;
            float mx0 = px[j] * 0.62f + px[b0] * 0.38f, my0 = py[j] * 0.62f + py[b0] * 0.38f;
            float mx1 = px[j + 1] * 0.62f + px[b1] * 0.38f, my1 = py[j + 1] * 0.62f + py[b1] * 0.38f;
            Quad_Add(px[j], py[j], px[j + 1], py[j + 1], mx1, my1, mx0, my0, Color_Over(color_white, 0.07f));
        }
        // the press, from the tip in
        float f = Clamp01(a * CREST_LEAD - i * CREST_LAG);
        if (f > 0.003f)
            Crest_BladeQuads(i, 1.f - f, 1.f, m, k, cx, cy, fc);
        // the notch where the fill stands when a light press counts
        float un = 1.f - Clamp01(RING_LIGHT * CREST_LEAD - i * CREST_LAG);
        float nx[2], ny[2];
        Crest_BladePoint(i, un, 1, m, k, cx, cy, &nx[0], &ny[0]);
        Crest_BladePoint(i, un, 0, m, k, cx, cy, &nx[1], &ny[1]);
        Hud_Line(nx, ny, 2, 0, 0.06f * k, PX, Color_Over(color_white, 0.85f));
        Hud_Line(px, py, n, 1, (0.05f + 0.04f * lit) * k, PX, Color_Mix(Color_Fill(cyan, 0.8f), color_white, lit));
        // the click lights the whole wing, as bright as its flash, while held
        if (w > 0.01f)
            Crest_BladeQuads(i, 0, 1, m, k, cx, cy, Color_Over(color_white, w));
    }
    float sx[6], sy[6], mx = 0, my = 0;
    for (int i = 0; i < 6; i++)
    {
        sx[i] = cx - m * crest_spine[i][0] * k;
        sy[i] = cy + crest_spine[i][1] * k;
        mx += sx[i] / 6;
        my += sy[i] / 6;
    }
    Hud_Fan(mx, my, sx, sy, 6, click ? color_white : Color_Over(Color_Mix(color_plate, cyan, 0.25f), 0.9f));
    Hud_Line(sx, sy, 6, 1, 0.05f * k, PX, Color_Over(Color_Mix(cyan, color_white, lit), 0.85f));
}

#define GEM_MAX 18

// A long pointed gem's outline (a marquise) around (gx, gy): l long, w wide,
// turned ang degrees. 2 * GEM_SIDE points.
#define GEM_SIDE 8
static void Gem_Marquise(float gx, float gy, float l, float w, float ang, float *px, float *py)
{
    float c = cos(ang * 0.01745329f), sn = sin(ang * 0.01745329f);
    for (int i = 0; i < 2 * GEM_SIDE; i++)
    {
        // along the top from the left point, then back along the bottom
        float u = i <= GEM_SIDE ? (float)i / GEM_SIDE : (float)(2 * GEM_SIDE - i) / GEM_SIDE;
        float v = sin(3.1415927f * u);
        float h = w / 2 * sqrtf(v) * sqrtf(sqrtf(v)) * (i <= GEM_SIDE ? 1 : -1);
        float x = -l / 2 + l * u;
        px[i] = gx + x * c - h * sn;
        py[i] = gy + x * sn + h * c;
    }
}

// A gem as a button: the press ghost, the fill, the outline, the table and
// the letter. Its outline (n points) and table (tn points) go round (gx, gy).
static void Gem_Button(const float *px, const float *py, int n, const float *tx, const float *ty, int tn, float gx, float gy,
                       float k, float letter, GXColor c, int in, float glow)
{
    float flash = pad_flash[in];
    if (flash > GHOST_MIN)
    {
        float g = Ghost_Grow(flash), qx[GEM_MAX], qy[GEM_MAX];
        for (int i = 0; i < n; i++)
        {
            qx[i] = gx + (px[i] - gx) * g;
            qy[i] = gy + (py[i] - gy) * g;
        }
        Hud_Fan(gx, gy, qx, qy, n, Ghost_Color(c, flash));
    }
    Hud_Fan(gx, gy, px, py, n, Ring_Lit(c, glow));
    Hud_Line(px, py, n, 1, 0.07f * k, PX, Ring_Edge(c));
    Hud_Fan(gx, gy, tx, ty, tn, Color_Over(color_white, 0.16f + 0.1f * glow));
    Ring_Letter(in, gx, gy, letter, c, glow);
}

static void Crest_Marquise(float cx, float cy, float k, float gx, float gy, float l, float w, float ang, float letter,
                           GXColor c, int in, float glow)
{
    float px[2 * GEM_SIDE], py[2 * GEM_SIDE], tx[2 * GEM_SIDE], ty[2 * GEM_SIDE];
    gx = cx + gx * k;
    gy = cy + gy * k;
    Gem_Marquise(gx, gy, l * k, w * k, ang, px, py);
    Gem_Marquise(gx, gy, 0.59f * l * k, 0.43f * w * k, ang, tx, ty);
    Gem_Button(px, py, 2 * GEM_SIDE, tx, ty, 2 * GEM_SIDE, gx, gy, k, letter * k, c, in, glow);
}

// A round gem with an eight-sided table.
static void Crest_Round(float cx, float cy, float k, float gx, float gy, float r, float letter, GXColor c, int in, float glow)
{
    float px[GEM_MAX], py[GEM_MAX], tx[8], ty[8];
    gx = cx + gx * k;
    gy = cy + gy * k;
    Circle_Points(gx, gy, r * k, GEM_MAX, px, py);
    for (int i = 0; i < 8; i++)
    {
        float ang = (i + 0.5f) * 0.7853982f;
        tx[i] = gx + cos(ang) * 0.6f * r * k;
        ty[i] = gy + sin(ang) * 0.6f * r * k;
    }
    Gem_Button(px, py, GEM_MAX, tx, ty, 8, gx, gy, k, letter * k, c, in, glow);
}

// The Crest look, from the box's bottom left corner.
static void Crest_Draw(FighterData *fp, HSD_Pad *pad, float bx, float by)
{
    float k = CREST_K * ring_sizes[Options_Hud[HOPT_PAD_SIZE].val];
    float cx = bx + CREST_W * k, cy = by + CREST_BOT * k;

    int held[PIN_COUNT];
    Pad_Held(pad, held);
    float glow[PIN_COUNT];
    for (int i = 0; i < PIN_COUNT; i++)
        glow[i] = held[i] ? 1.f : pad_glow[i];

    pad_soft = 1;
    int n = countof(crest_plate);
    float px[countof(crest_plate)], py[countof(crest_plate)];
    for (int i = 0; i < n; i++)
    {
        px[i] = cx + crest_plate[i][0] * k;
        py[i] = cy + crest_plate[i][1] * k;
    }
    Hud_Fan(cx, cy - 0.4f * k, px, py, n, Color_Fill(color_plate, 0.5f));
    Hud_Line(px, py, n, 1, 0.05f * k, PX, Color_Over(color_white, 0.22f));
    Crest_Wing(cx, cy, k, -1, pad->ftriggerLeft, held[PIN_L], glow[PIN_L], pad_flash[PIN_L]);
    Crest_Wing(cx, cy, k, 1, pad->ftriggerRight, held[PIN_R], glow[PIN_R], pad_flash[PIN_R]);

    Pad_Stick(fp, pad, cx, cy + 0.6f * k, 1.35f * k, 0.7f * k);
    // X and Y stacked on the left, Y above, clear of each other and of B
    Crest_Marquise(cx, cy, k, -2.88f, -0.92f, 1.25f, 0.56f, -25, 0.3f, color_btn_xy, PIN_Y, glow[PIN_Y]);
    Crest_Marquise(cx, cy, k, -2.62f, -1.74f, 1.25f, 0.56f, -25, 0.3f, color_btn_xy, PIN_X, glow[PIN_X]);
    Crest_Marquise(cx, cy, k, 2.62f, -1.12f, 1.6f, 0.64f, 25, 0.42f, color_btn_z, PIN_Z, glow[PIN_Z]);
    Crest_Round(cx, cy, k, -1.47f, -2.42f, 0.48f, 0.44f, color_btn_b, PIN_B, glow[PIN_B]);
    Crest_Round(cx, cy, k, 1.55f, -2.32f, 0.62f, 0.56f, color_btn_a, PIN_A, glow[PIN_A]);
    Ring_CStick(pad, cx, cy - 2.35f * k, 0.9f * k);
    pad_soft = 0;
}

// The controller display. Reads the pad live; the trails and glows come
// from what Pad_Record saw.
// Controller Cues: the input a live AI or waveland window wants, timed
// around where it's drawn. The Closing Ring shrinks onto it from twice its
// size over the last frames before the window, and sits snug and thick
// while the window is open; the Gauge is a ring going round it, closed as
// the window opens. Either is only an outline in the cue's color, never a
// fill, so it can't be taken for a press.
#define PADCUE_LEAD 20 // frames ahead it shows
static void Pad_CueOne(int in, int kind)
{
    Cue *c = &cue_live[kind];
    if (!pin_spot[in].r || !c->phase || c->dim || c->held)
        return;
    float x = pin_spot[in].x, y = pin_spot[in].y, r = pin_spot[in].r;
    int open = c->phase == PH_WINDOW;
    int ahead = open ? 0 : c->left - 1;
    if (ahead > PADCUE_LEAD)
        return;
    float t = open ? 0 : (float)ahead / PADCUE_LEAD; // 1 far, 0 at the window
    if (cue_log)
        OSReport("LLPADCUE %d pin %d kind %d ahead %d open %d at %.2f %.2f r %.2f\n", event_vars->game_timer, in, kind, ahead,
                 open, x, y, r);
    GXColor col = Cue_Color(kind);
    vis_k = Kind_K(VG_TIMERS, kind);
    float w = open ? 0.16f : 0.09f;
    if (Options_Hud[HOPT_PAD_CUES].val == PADCUE_RING)
    {
        float rr = r * (1.25f + 1.1f * t);
        Hud_Ring(x, y, rr, w, Color_Over(col, open ? 1.f : 0.55f + 0.45f * (1.f - t)));
        return;
    }
    // the gauge: an arc from the top, clockwise, as much of the way round
    // as the window is near
    float rr = r * 1.35f, frac = open ? 1.f : 1.f - t;
    int n = 2 + (int)(frac * 22);
    float px[24], py[24];
    for (int i = 0; i < n; i++)
    {
        float a = 1.5707963f - 6.2831853f * frac * i / (n - 1);
        px[i] = x + cos(a) * rr;
        py[i] = y + sin(a) * rr;
    }
    Hud_Ring(x, y, rr, 0.05f, Color_Over(col, 0.25f)); // the track
    Hud_Line(px, py, n, 0, w, PX, Color_Over(col, open ? 1.f : 0.8f));
}

static void Pad_Cues(void)
{
    if (Options_Hud[HOPT_PAD_CUES].val == PADCUE_OFF || route_rows_active)
        return;
    float k = vis_k;
    if (Cues_Ai())
        Pad_CueOne(PIN_A, CUE_AI);
    if (Cues_Waveland())
    {
        Pad_CueOne(PIN_L, CUE_WL);
        Pad_CueOne(PIN_R, CUE_WL);
    }
    vis_k = k;
}

static void Pad_Draw(FighterData *fp)
{
    if (Options_Hud[HOPT_STICK].val == STICK_OFF)
        return;
    float bx, by, x1, y1;
    Pad_Box(fp, &bx, &by, &x1, &y1);
    HSD_Pad *pad = Pad_Live(fp);
    int buttons = Options_Hud[HOPT_BUTTONS].val;
    memset(pin_spot, 0, sizeof(pin_spot));

    if (buttons && Options_Hud[HOPT_LOOK].val == LOOK_RING)
    {
        Ring_Draw(fp, pad, bx, by);
        Pad_Cues();
        return;
    }
    if (buttons && Options_Hud[HOPT_LOOK].val == LOOK_CREST)
    {
        Crest_Draw(fp, pad, bx, by);
        Pad_Cues();
        return;
    }
    Pad_Stick(fp, pad, buttons ? bx + PAD_STICK_X : bx + PAD_R, by + PAD_R, PAD_R, 1.f);
    if (!buttons)
        return;

    // what's down now counts as lit too, so it also shows while paused
    int held[PIN_COUNT];
    Pad_Held(pad, held);
    float glow[PIN_COUNT];
    for (int i = 0; i < PIN_COUNT; i++)
        glow[i] = held[i] ? 1.f : pad_glow[i];

    Pad_Trigger(bx, by, pad->ftriggerLeft, glow[PIN_L]);
    Pad_Trigger(bx + PAD_W - 0.5f, by, pad->ftriggerRight, glow[PIN_R]);
    Pin_Spot(PIN_L, bx + 0.25f, by + 2.4f, 0.7f);
    Pin_Spot(PIN_R, bx + PAD_W - 0.25f, by + 2.4f, 0.7f);

    Pad_Button(bx + 8.3f, by + 2.6f, 0.8f, color_btn_a, PIN_A, glow[PIN_A]);
    Pad_Button(bx + 7.0f, by + 1.5f, 0.48f, color_btn_b, PIN_B, glow[PIN_B]);
    Pad_Button(bx + 9.6f, by + 3.1f, 0.48f, color_btn_xy, PIN_X, glow[PIN_X]);
    Pad_Button(bx + 8.0f, by + 4.0f, 0.48f, color_btn_xy, PIN_Y, glow[PIN_Y]);

    // Z, a pill
    float zx = bx + 9.6f, zy = by + 4.35f;
    Hud_PillRing(zx, zy, 1.0f, 0.36f, 0, 0.07f, Color_Fill(color_btn_z, 0.5f));
    if (glow[PIN_Z] > 0)
        Hud_Pill(zx, zy, 0.93f, 0.29f, 0, Color_Fill(color_btn_z, glow[PIN_Z]));
    if (pad_glow[PIN_Z] >= 1.f && pad_glow_prev[PIN_Z] < 1.f)
        Hud_PillRing(zx, zy, 1.3f, 0.66f, 0, 0.1f, Color_Mix(color_btn_z, color_white, 0.5f));

    Pad_CStick(pad, bx + 10.6f, by + 1.3f, glow[PIN_C]);
    Pad_Cues();
}

///////////////////////
/// Corner panel    ///
///////////////////////

// Two lines in a top corner: what's coming up, and how the last attempt
// went (a third, the accuracy count, in Developer). Each has a square in
// its cue's color at the screen's edge, and its text runs from there toward
// the middle, so a long line can't run off the screen.

static void Panel_Line(float x, float y, int right, const char *text, int kind)
{
    float size = 0.45f;
    float w = 1.7f + Text_Width(text, size);
    float x0 = right ? x - w : x;
    Text_Plate(x0, x0 + w, y);
    GXColor sq = kind >= 0 ? Cue_Color(kind) : Color_Fill(color_white, kind == -2 ? 0 : 0.3f); // -2: no square
    GXColor tc = {235, 235, 235, 255};
    if (right)
    {
        Hud_Rect(x - 1.05f, y + 0.8f, x - 0.35f, y + 1.5f, sq);
        Hud_TextAligned(text, x - 1.4f, y, size, tc, 2);
    }
    else
    {
        Hud_Rect(x + 0.35f, y + 0.8f, x + 1.05f, y + 1.5f, sq);
        Hud_TextAligned(text, x + 1.4f, y, size, tc, 0);
    }
}

// A quick toggle's new setting, shown for a moment at the top of the panel
// (even with the panel off).
static char toast_text[40];
static int toast_timer;
static u8 hide_all; // everything the event draws hidden but the toast (L or R, Z and D-pad up or down)
#define TOAST_FRAMES 90

static void Toast(const char *t)
{
    strcpy(toast_text, t);
    toast_timer = TOAST_FRAMES;
    OSReport("LLTOAST %s\n", t);
}

static void Panel_Draw(void)
{
    int right = !panel_left;
    float x = right ? SAFE_W : -SAFE_W;
    float y = SAFE_H - 3.0f;
    if (toast_timer > 0)
    {
        Panel_Line(x, y, right, toast_text, -2);
        y -= 2.4f;
    }
    if (!Options_Hud[HOPT_PANEL].val || hide_all)
        return;
    Panel_Line(x, y, right, text_next, next_kind);
    if (text_steps[0])
    {
        y -= 2.4f;
        Panel_Line(x, y, right, text_steps, -2);
    }
    Panel_Line(x, y - 2.4f, right, text_last, last_kind);
    if (Options_Dev[DOPT_EXACT].val)
    {
        y -= 2.4f;
        Panel_Line(x, y - 2.4f, right, text_exact, -1);
    }
    // stepping frame by frame: which frame of which state is on screen, so
    // windows can be counted (a press now comes out on the next one)
    if (Options_Game[GOPT_FRAME_ADV].val && text_frame[0])
        Panel_Line(x, y - 4.8f, right, text_frame, -2);
}

///////////////////////
/// Debug camera    ///
///////////////////////

// For mockups drawn on real game frames: where world points land on screen
// on the frame being drawn (the stage plane is z = 0, so four points give the
// exact mapping), and what the timers and the prediction hold. Logged from
// the draw so the camera is the one this frame is rendered with.
static int script_cur; // defined with the scripts below
// test scripts log everything
static int Log_Level(void) { return script_cur >= 0 ? LOG_ALL : Options_Dev[DOPT_LOG].val; }
static int capture_clean; // hide everything the event draws (D-pad up in Frame Advance, with the Debug Log on)

static void Log_Camera(FighterData *fp)
{
    static int last = -1;
    static const Vec2 ref[4] = {{-100.f, 0}, {100.f, 0}, {-100.f, 100.f}, {100.f, 100.f}};
    if (event_vars->game_timer == last)
        return;
    last = event_vars->game_timer;

    COBJ *cobj = View_CObj();
    char buf[512];
    int n = sprintf(buf, "LLCAM %d vp %.1f %.1f %.1f %.1f pts", event_vars->game_timer,
                    cobj->viewport_left, cobj->viewport_right, cobj->viewport_top, cobj->viewport_bottom);
    for (int i = 0; i < 4; i++)
    {
        Vec3 in = {ref[i].X, ref[i].Y, 0}, out;
        HSD_GXProject(cobj, &in, &out, 1);
        n += sprintf(buf + n, " %.2f %.2f", out.X, out.Y);
    }
    n += sprintf(buf + n, " falcon %.4f %.4f sid %d face %d stick %.3f %.3f", fp->phys.pos.X, fp->phys.pos.Y, fp->state_id,
                 fp->facing_direction > 0 ? 1 : -1, fp->input.lstick.X, fp->input.lstick.Y);
    for (int i = 0; i < CUE_NUM; i++)
    {
        Cue *c = &cue_live[i], *e = &cue_end[i];
        n += sprintf(buf + n, " cue%d %d %d %d %d %.3f %.3f end %d %d %d", i, c->phase, c->left, c->span, c->width,
                     c->spot.X, c->spot.Y, e->phase, e->age, e->dim);
    }
    sprintf(buf + n, " intang %d\n", fp->hurt.intang_frames.ledge);
    Log(buf);

    // where the percent is and where the controller display went, to check
    // the guess at its width (when either moves)
    static float last_hud[3];
    Vec3 *hp = Match_GetPlayerHUDPos(fp->ply);
    float px0, py0, px1, py1;
    Pad_Box(fp, &px0, &py0, &px1, &py1);
    float hx = hp ? hp->X : -999.f, hy = hp ? hp->Y : -999.f;
    if (hx != last_hud[0] || hy != last_hud[1] || px0 != last_hud[2])
    {
        last_hud[0] = hx;
        last_hud[1] = hy;
        last_hud[2] = px0;
        sprintf(buf, "LLHUD ply %d pct %.2f %.2f pad %.2f %.2f %.2f %.2f dz %.4f %.4f\n", fp->ply, hx, hy, px0, py0, px1, py1,
                Common_Float(0x0), Common_Float(0x4));
        Log(buf);
    }

    Prediction *p = live_visible ? pred_live : 0;
    if (p && p->num > 0)
    {
        // the path's first frames: position and ECB top
        int n = sprintf(buf, "LLPATH %d", event_vars->game_timer);
        for (int k = 0; k <= p->num && k <= 12; k++)
            n += sprintf(buf + n, " %.2f,%.2f,%.2f", p->pos[k].X, p->pos[k].Y, p->top[k]);
        sprintf(buf + n, "%s\n", p->land_frame ? "" : p->uncertain_from <= p->num ? " stop" : " none");
        Log(buf);

        // on a double jump's first frame, the whole path with the ECB
        // bottom, to check against the frames that follow
        static int last_sid = -1;
        int ts = Tracked_Index(fp->state_id);
        if (fp->state_id != last_sid && (ts == TS_JUMPAERIALF || ts == TS_JUMPAERIALB))
        {
            int last = p->land_frame ? p->land_frame : p->num;
            for (int k0 = 0; k0 <= last; k0 += 10)
            {
                n = sprintf(buf, "LLPATHD %d %d", event_vars->game_timer, k0);
                for (int k = k0; k < k0 + 10 && k <= last; k++)
                    n += sprintf(buf + n, " %.3f,%.3f,%.3f", p->pos[k].X, p->pos[k].Y, p->bottom[k]);
                sprintf(buf + n, "\n");
                Log(buf);
            }
        }
        last_sid = fp->state_id;
    }
    if (p && p->land_frame)
    {
        int k = p->land_frame;
        // the first aerial interrupt before any filter, and its touchdown
        int raw = 0;
        for (int j = 1; j < k && !raw; j++)
        {
            if (p->ai_mask[j] & ~p->ai_lag_mask[j])
                raw = j;
        }
        u8 raw_mask = raw ? p->ai_mask[raw] & ~p->ai_lag_mask[raw] : 0;
        sprintf(buf, "LLPRED %d land %d kind %d at %.3f %.3f lag %d ai %d w%d wl %d w%d raw %d t%d m%x\n",
                event_vars->game_timer, k, p->land_kind, p->pos[k].X, p->pos[k].Y + p->bottom[k], p->lag, p->ai_first,
                p->ai_width, p->wl_first, p->wl_width, raw, raw ? raw + Ai_FirstDelay(p, raw, raw_mask) : 0, raw_mask);
        Log(buf);
    }
}

static void Draw_SlideOff(void);
static void Draw_RoutePath(void);
static void Draw_JumpPath(void);
static int Jump_Showing(void);
static void Markers_Draw(void);
static void Compass_Draw(void);

// Platform Glow: the floor a waveland or wavedash slides along lights up
// in the stage, at Falcon's depth. A faint glow marks the slide while the
// timer counts down and fills in from its ends toward the landing spot,
// growing brighter and taller, steeply at the end; on each frame of the
// window the whole slide flares white-hot (hottest on the first and last).
// Only the directions that work light up.
// A glow standing on the floor from xa to xb, just behind Falcon's middle:
// a bright line along the surface and light falling off fast above it.
#define GLOW_Z -1.f

static void Glow_Span(float xa, float xb, float y, float h, GXColor c)
{
    if (xb - xa < 0.05f || c.a == 0)
        return;
    GXColor mid = Color_Fill(c, 0.35f);
    GXColor top = {0, 0, 0, 0};
    float ym = y + h * 0.3f;
    World_Start(8, GX_QUADS, 0);
    World_Vtx(xa, y, GLOW_Z, c);
    World_Vtx(xb, y, GLOW_Z, c);
    World_Vtx(xb, ym, GLOW_Z, mid);
    World_Vtx(xa, ym, GLOW_Z, mid);
    World_Vtx(xa, ym, GLOW_Z, mid);
    World_Vtx(xb, ym, GLOW_Z, mid);
    World_Vtx(xb, y + h, GLOW_Z, top);
    World_Vtx(xa, y + h, GLOW_Z, top);
    GXColor line = Color_Mix(c, Color_Fill(color_white, c.a / 255.f), 0.3f);
    World_Start(2, GX_LINES, 42);
    World_Vtx(xa, y + 0.1f, GLOW_Z, line);
    World_Vtx(xb, y + 0.1f, GLOW_Z, line);
}

// How the glow ends: a hit bursts white-cyan and rises off the floor, a
// miss sinks in violet toward the spot, and a skipped or interrupted one
// just goes out. All of it is over in a few frames.
#define GLOW_HIT 10
#define GLOW_MISS 10
#define GLOW_SKIP 5
static const GXColor color_glow_miss = {150, 80, 255, 255};

static void Plat_Glow(FighterData *fp)
{
    if (!Options_Cues[COPT_GLOW].val || !Cues_Waveland())
        return;
    GXColor base = Cue_Color(CUE_WL);
    float reach = Slide_Distance(fp, common_dodge_force);
    for (int pass = 0; pass < 2; pass++)
    {
        Cue *c = pass ? &cue_live[CUE_WL] : &cue_end[CUE_WL];
        if (!c->phase)
            continue;
        float m = c->spot.X, y = c->spot.Y;
        float L = m - reach < c->x0 ? c->x0 : m - reach;
        float R = m + reach > c->x1 ? c->x1 : m + reach;
        int ok_l = c->wd || (c->dirs & DODGE_LEFT);
        int ok_r = c->wd || (c->dirs & DODGE_RIGHT);

        if (pass && c->phase == PH_COUNT && !c->dim)
        {
            // builds up: fills in from the ends toward the spot, brighter
            // and taller the closer the press, steeply at the end
            int k = c->left - 1;
            float p = 1.f - (c->span > 1 ? Clamp01((float)k / (c->span - 1)) : 0);
            float p2 = p * p;
            GXColor faint = Color_Fill(base, 0.1f + 0.1f * p);
            GXColor lit = Color_Fill(Color_Mix(base, color_white, 0.2f * p2), 0.2f + 0.6f * p2);
            float h = 0.9f + 2.4f * p2;
            if (ok_l)
            {
                Glow_Span(L, m, y, 0.8f, faint);
                Glow_Span(L, L + (m - L) * p, y, h, lit);
            }
            if (ok_r)
            {
                Glow_Span(m, R, y, 0.8f, faint);
                Glow_Span(R - (R - m) * p, R, y, h, lit);
            }
        }
        else if (pass && c->phase == PH_WINDOW && !c->dim)
        {
            // the window: the whole slide flares white-hot, the first and
            // last frames hottest, with a tall haze over it
            int edge = c->age == 0 || c->age == c->width - 1;
            GXColor core = Color_Fill(Color_Mix(base, color_white, edge ? 0.7f : 0.45f), edge ? 1.f : 0.85f);
            GXColor haze = Color_Fill(base, edge ? 0.45f : 0.3f);
            float h = edge ? 4.5f : 3.5f;
            if (ok_l)
            {
                Glow_Span(L, m, y, h * 2.2f, haze);
                Glow_Span(L, m, y, h, core);
            }
            if (ok_r)
            {
                Glow_Span(m, R, y, h * 2.2f, haze);
                Glow_Span(m, R, y, h, core);
            }
        }
        else if (!pass && c->phase == PH_HIT && c->age < GLOW_HIT)
        {
            float q = (float)c->age / GLOW_HIT;
            float a = (1.f - q) * (1.f - q) * (c->soft ? 0.55f : 1.f);
            GXColor flash = Color_Fill(Color_Mix(base, color_white, 0.7f * (1.f - q)), a);
            Glow_Span(L, R, y, 3.f + 5.f * Ease_Out(q), flash);
        }
        else if (!pass && c->phase == PH_FADE && c->dim == DIM_MISS && c->age < GLOW_MISS)
        {
            // folds in toward the spot
            float q = (float)c->age / GLOW_MISS;
            float a = 0.7f * (1.f - q);
            float l = L + (m - L) * Ease_Out(q), r = R - (R - m) * Ease_Out(q);
            Glow_Span(l, r, y, 2.2f * (1.f - q) + 0.4f, Color_Fill(color_glow_miss, a));
        }
        else if (!pass && (c->phase == PH_FADE || c->phase == PH_CUT) && c->age < GLOW_SKIP)
        {
            float q = (float)c->age / GLOW_SKIP;
            Glow_Span(L, R, y, 0.8f, Color_Fill(base, 0.12f * (1.f - q)));
        }
    }
}

static void World_GX(GOBJ *gobj, int pass)
{
    if (pass != 2)
        return;

    FighterData *fp = Fighter_GetGObj(0)->userdata;
    compass_num = 0;
    if (Log_Level() >= LOG_ALL)
        Log_Camera(fp);
    if (capture_clean || hide_all)
        return;
    world_on_top = 1;
    vis_k = 1.f;
    if (Options_Dev[DOPT_COLL].val)
        Draw_CurrentEcb(fp);
    vis_k = Group_K(VG_PATHS);
    // a ledge route stays on top all the way down
    world_on_top = route_active;

    if (live_visible && route_active && !route_dj_done)
        ; // the route's own path shows where it goes
    else if (live_visible)
        Draw_Prediction(pred_live, 1, LINE_SOLID);
    else if (ghost_visible)
    {
        Draw_Prediction(pred_seg, 0, LINE_SOLID);
        if (Options_Paths[POPT_PATH].val)
            Draw_Path(actual_pos, actual_bottom, 0, actual_num - 1, color_actual, 12);
    }
    else
    {
        // the short hop is dashed, and only has a body line of its own
        // when the full hop isn't shown
        if (preview_fh)
            Draw_Prediction(pred_fh, 1, LINE_SOLID);
        if (preview_sh)
            Draw_Prediction(pred_sh, !preview_fh, LINE_DASHED);
    }
    world_on_top = 0;
    world_add = 1;
    vis_k = Kind_K(VG_CUES, CUE_WL);
    Plat_Glow(fp);
    world_add = 0;
    world_on_top = 1;
    vis_k = Group_K(VG_LEDGE);
    Draw_RoutePath();
    Draw_JumpPath();
    // along the floor, where the depth test can't tell the line from it
    vis_k = Group_K(VG_PATHS);
    Draw_SlideOff();
    world_on_top = 0;
    vis_k = 1.f;
}

// The name picker's grid (Naming above), over everything else.
#define NAMER_CELL_W 3.2f
#define NAMER_CELL_H 3.0f
#define NAMER_TOP 4.6f
static void Namer_Key(int r, int c, float *x0, float *y0, float *x1, float *y1)
{
    float left = -NAMER_COLS * NAMER_CELL_W / 2;
    float w = r == NAMER_ROWS - 1 ? NAMER_COLS * NAMER_CELL_W / NAMER_KEYS : NAMER_CELL_W;
    *x0 = left + c * w + 0.15f;
    *x1 = left + (c + 1) * w - 0.15f;
    *y1 = NAMER_TOP - r * NAMER_CELL_H;
    *y0 = *y1 - NAMER_CELL_H + 0.3f;
}

static void Namer_Draw(void)
{
    static const char *keys[NAMER_KEYS] = {"Space", "Delete", 0, "Done"};
    GXColor ink = {235, 235, 235, 255}, dark = {10, 12, 24, 255}, lit = {120, 150, 255, 255};
    Hud_Rect(-40.f, -30.f, 40.f, 30.f, Color_Over(color_plate, 0.6f));
    float px = NAMER_COLS * NAMER_CELL_W / 2 + 1.2f;
    Hud_Rect(-px, -13.6f, px, 13.f, Color_Over(color_plate, 0.92f));
    Hud_Frame(-px, -13.6f, px, 13.f, 0.12f, Color_Over(lit, 0.6f));
    Hud_TextAligned(namer.title, -px + 1.f, 10.f, 0.5f, ink, 0);
    // the name so far, with the cursor after it
    Hud_Rect(-px + 1.f, 6.6f, px - 1.f, 9.4f, Color_Over(color_white, 0.08f));
    Hud_Rect(-px + 1.f, 6.6f, px - 1.f, 6.75f, Color_Over(lit, 0.8f));
    char line[NAME_LEN + 2];
    sprintf(line, "%s_", namer.buf);
    Hud_TextAligned(line, -px + 1.6f, 6.75f, 0.6f, color_white, 0);

    for (int r = 0; r < NAMER_ROWS; r++)
    {
        int cols = r == NAMER_ROWS - 1 ? NAMER_KEYS : NAMER_COLS;
        for (int c = 0; c < cols; c++)
        {
            float x0, y0, x1, y1;
            Namer_Key(r, c, &x0, &y0, &x1, &y1);
            int on = r == namer.cy && c == namer.cx;
            Hud_Rect(x0, y0, x1, y1, on ? Color_Over(lit, 1.f) : Color_Over(color_white, 0.1f));
            if (on)
                Hud_Frame(x0 - 0.1f, y0 - 0.1f, x1 + 0.1f, y1 + 0.1f, 0.15f, color_white);
            if (!namer.dirty || !namer.text)
                continue;
            int i = r * NAMER_COLS + c;
            char glyph[2] = {0, 0};
            const char *label;
            if (r < NAMER_ROWS - 1)
            {
                glyph[0] = namer_rows[namer.lower][r][c];
                label = glyph;
            }
            else
                label = keys[c] ? keys[c] : namer.lower ? "ABC" : "abc";
            float size = r < NAMER_ROWS - 1 ? 0.55f : 0.45f;
            // as Hud_TextAligned places a row 2.5 tall whose bottom is y
            float y = (y0 + y1) / 2 - 1.25f;
            Text_SetText(namer.text, i, label);
            Text_SetScale(namer.text, i, size, size);
            Text_SetPosition(namer.text, i, (x0 + x1) / 2 * 10.f, y * -10.f - 37.5f);
            Text_SetColor(namer.text, i, on ? &dark : &ink);
        }
    }
    namer.dirty = 0;
    Hud_TextAligned("A type   B delete   Y space   X case   Start done", -px + 1.f, -12.9f, 0.4f,
                    (GXColor){180, 185, 200, 255}, 0);
}

static void Hud_GX(GOBJ *gobj, int pass)
{
    if (pass != 2 || capture_clean)
        return;
    HUDCamData *hud = event_vars->hudcam_gobj->userdata;
    if (hud->hide)
        return;

    FighterData *fp = Fighter_GetGObj(0)->userdata;
    COBJ *prev = COBJ_GetCurrent();
    CObj_SetCurrent(event_vars->hudcam_gobj->hsd_object);
    quad_num = 0;
    if (namer.on)
    {
        Namer_Draw();
        Quad_Flush();
        CObj_SetCurrent(prev);
        return;
    }
    if (hide_all)
    {
        Panel_Draw(); // the toast that says so
        Quad_Flush();
        CObj_SetCurrent(prev);
        return;
    }
    Meter_Build();
    vis_k = Group_K(route_rows_active ? VG_LEDGE : VG_TIMERS);
    Spot_Draw(fp);
    int near = Options_Timers[TOPT_NEAR].val;
    if (meter_rows == 0)
        Pin_Idle();
    else
    {
        if (near == NEAR_STRIP)
            Meter_Above(fp);
        if (Options_Timers[TOPT_STRIP].val != STRIP_OFF)
            Meter_Fixed(fp);
    }
    if (near != NEAR_OFF && near != NEAR_STRIP)
        Near_Draw(fp, near);
    Wd_Draw(fp);
    int route_marks = (hang_ledge >= 0 && route_show_num > 0) || (route_active && !route_dj_done) || Jump_Showing();
    vis_k = Group_K(route_marks ? VG_LEDGE : VG_PATHS);
    Markers_Draw();
    vis_k = Group_K(VG_PATHS);
    Compass_Draw();
    vis_k = Group_K(VG_PAD);
    Pad_Draw(fp);
    vis_k = 1.f;
    Panel_Draw();
    if (quad_num > quad_peak)
        quad_peak = quad_num;
    Quad_Flush();

    CObj_SetCurrent(prev);
}

///////////////////////
/// Frame advance   ///
///////////////////////

// The game's debug pause, as the lab uses it: while Frame Advance is on the
// game stays paused, and each press of the advance button runs one frame
// (held for half a second, it steps 10 times a second). The pause and
// step checks run every frame, paused or not; the scene resets them when the
// event ends.
#define LL_ADVANCE_HOLD 30   // frames held before it repeats
#define LL_ADVANCE_REPEAT 6  // then one step every this many frames

static int Advance_Port(void)
{
    return Fighter_GetControllerPort(0);
}

static int assist_advance; // Assist runs one frame further into the window

static int Advance_CheckPause(void)
{
    HSD_Update *update = stc_hsd_update;
    int paused = update->pause_kind & 1;
    return paused != (Options_Game[GOPT_FRAME_ADV].val || assist_frozen || namer.on);
}

static int Advance_CheckStep(void)
{
    static int timer;
    int port = Advance_Port();
    HSD_Pad *pad = PadGetMaster(port);
    HSD_Pad *engine = PadGetEngine(port);
    int button = adv_button_masks[Options_Game[GOPT_ADV_BUTTON].val];

    if (namer.on)
        return 0; // the name grid has the buttons
    if (assist_advance)
    {
        assist_advance = 0;
        return 1;
    }
    // with Frame Advance off the button is the game's (L airdodges)
    if (Pause_CheckStatus(1) == 2 || !(Options_Game[GOPT_FRAME_ADV].val || assist_frozen) || !(pad->held & button))
    {
        timer = 0;
        return 0;
    }
    timer++;
    if (timer != 1 && (timer < LL_ADVANCE_HOLD || (timer - LL_ADVANCE_HOLD) % LL_ADVANCE_REPEAT))
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
//   ledge <left|right>         hang Falcon on the main stage's left or right
//                              ledge, freshly grabbed (full intangibility)
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
// Steps that do what the menu would, so a test needs no menuing:
//   set <Menu>/<Option> = <v>  an option's value, its place in the list
//                              (Menu: Cues, Timers, Paths, HUD, Sounds, Ledge,
//                              Jump, Camera, Speed or Dev)
//   get <Menu>/<Option>        write an option's value to the log
//   closemenu                  what closing the pause menu does
//   preset load|save|start <name>  the Presets menu's actions on that preset
//   card flush                 finish writing the memory card, waiting
//   card restart               as if the event started over: every option
//                              back to its default, then the card read again
//   chord lr|z|lrz up|down|left|right  a quick toggle
//   view save|load|show <1-4>  Camera > Save View, picking that View, or
//                              logging where the camera is against it
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
    SOP_LEDGE,
    SOP_WL,
    SOP_AI,
    SOP_LAND,
    SOP_SHOT,
    SOP_MARK,
    SOP_CMD, // count: which (SCMD_*), label: the rest of the line
};

enum script_cmd
{
    SCMD_SET,
    SCMD_CLOSEMENU,
    SCMD_PRESET,
    SCMD_CARD,
    SCMD_CHORD,
    SCMD_GET,
    SCMD_VIEW,
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
static char shot_label[48];  // the last shot's label, for the heartbeat
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
        else if (Script_Is(w, "ledge"))
        {
            char *side = Script_Word(&rest);
            op->kind = SOP_LEDGE;
            ok = side && (Script_Is(side, "left") || Script_Is(side, "right"));
            if (ok)
                op->facing = side[0] == 'l' ? 1 : -1; // facing the stage
        }
        else if (Script_Is(w, "shot") || Script_Is(w, "mark"))
        {
            op->kind = w[0] == 's' ? SOP_SHOT : SOP_MARK;
            op->label = Script_Trim(rest);
        }
        else if (Script_Is(w, "set") || Script_Is(w, "get") || Script_Is(w, "closemenu") || Script_Is(w, "preset") ||
                 Script_Is(w, "card") || Script_Is(w, "chord") || Script_Is(w, "view"))
        {
            op->kind = SOP_CMD;
            op->count = w[0] == 's' ? SCMD_SET : w[0] == 'g' ? SCMD_GET : w[0] == 'v' ? SCMD_VIEW : w[1] == 'l' ? SCMD_CLOSEMENU : w[0] == 'p' ? SCMD_PRESET : w[1] == 'a' ? SCMD_CARD : SCMD_CHORD;
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
    Options_Dev[DOPT_SCRIPT].value_num = 1;

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
    Options_Dev[DOPT_SCRIPT].value_num = script_shown + 1 + (script_num > 1);
    if (script_autorun && script_num > 0)
    {
        Options_Dev[DOPT_SCRIPT].val = script_num > 1 ? script_shown + 1 : 1;
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
    jt_active = 0;
    Cues_Clear();
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

// Hang Falcon on a ledge of the main stage as if he'd just grabbed it:
// CliffWait with the full ledge intangibility and his double jump back, the
// way ledgedash.c places a player (Fighter_PlaceOnLedge).
static int Script_PlaceLedge(GOBJ *ft, ScriptOp *op)
{
    FighterData *fp = ft->userdata;
    CollLine *lines = *stc_collline;
    int best = -1;
    float best_x = 0;
    for (CollGroup *group = *stc_firstcollgroup; group != 0; group = group->next)
    {
        int first = group->desc->floor_start;
        for (int i = first; i < first + group->desc->floor_num; i++)
        {
            if (!lines[i].desc->is_ledge)
                continue;
            Vec3 end;
            if (op->facing > 0)
                GrColl_GetGroundLineEndLeft(i, &end);
            else
                GrColl_GetGroundLineEndRight(i, &end);
            if (best < 0 || (op->facing > 0 ? end.X < best_x : end.X > best_x))
            {
                best = i;
                best_x = end.X;
            }
        }
    }
    if (best < 0)
        return 0;

    fp->phys.self_vel.X = 0;
    fp->phys.self_vel.Y = 0;
    Fighter_EnterSleep(ft, 0);
    Fighter_EnterRebirth(ft);
    fp->facing_direction = op->facing;
    FtCliffCatch *cliff = (void *)&fp->state_var;
    cliff->ledge_index = best;
    Fighter_EnterCliffWait(ft);
    cliff->timer = 0;
    Fighter_SetAirborne(fp);
    Fighter_EnableCollUpdate(fp);
    Coll_CheckLedge(&fp->coll_data);
    Fighter_MoveToCliff(ft);
    Script_UpdatePosition(ft);
    Fighter_ApplyIntang(ft, (*stc_ftcommon)->cliff_invuln_time + 1);
    fp->jump.jumps_used = 1;
    Script_ResetTimers(fp);
    Script_FixCamera(ft);
    Script_ResetTracking();
    return 1;
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
    int all = script_num > 1 && Options_Dev[DOPT_SCRIPT].val == script_shown + 1;
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

static void Chord(int lr, int z, int down);

// An option by "Menu/Option name", or 0.
static EventOption *Script_FindOption(char *spec)
{
    char menu[24];
    int n = 0;
    while (spec[n] && spec[n] != '/' && n < (int)sizeof(menu) - 1)
    {
        menu[n] = spec[n];
        n++;
    }
    if (spec[n] != '/')
        return 0;
    menu[n] = 0;
    char *name = spec + n + 1;
    if (Script_Is(menu, "Dev"))
    {
        for (int i = 0; i < DOPT_COUNT; i++)
            if (Script_Is(Options_Dev[i].name, name))
                return &Options_Dev[i];
        return 0;
    }
    for (int m = 0; m < (int)countof(preset_menus); m++)
        if (Script_Is(preset_menus[m].tag, menu))
            for (int i = 0; i < preset_menus[m].num; i++)
                if (Script_Is(preset_menus[m].opts[i].name, name))
                    return &preset_menus[m].opts[i];
    return 0;
}

static int Script_Preset(const char *name)
{
    for (int i = 0; i < PS_COUNT; i++)
        if (Script_Is(preset_names[i], name))
            return i;
    return -1;
}

// A step that does what the menu would. The line is copied first: the
// script can run again.
static void Script_Cmd(ScriptOp *op)
{
    char line[96], buf[160];
    int len = strlen(op->label);
    if (len > (int)sizeof(line) - 1)
        len = sizeof(line) - 1;
    memcpy(line, op->label, len);
    line[len] = 0;
    char *rest = line;
    int ok = 1;
    switch (op->count)
    {
    case SCMD_SET:
    {
        char *eq = line;
        while (*eq && *eq != '=')
            eq++;
        ok = *eq == '=';
        if (!ok)
            break;
        *eq = 0;
        char *v = Script_Trim(eq + 1);
        EventOption *o = Script_FindOption(Script_Trim(line));
        int val = (int)Script_Number(&v);
        ok = o != 0 && *v == 0 && Preset_Fits(o, val);
        if (!ok)
            break;
        int cam = o == &Options_Camera[CAMOPT_MODE] && o->val != val;
        int start = o == &Options_Ledge[LOPT_START] && o->val != val;
        o->val = val;
        if (cam)
            preset_cam_pending = 1;
        if (start)
            Event_ChangeLedgeStart(0, val);
        Event_ChangeRoutes(0, 0);
        sprintf(buf, "LLSET %s = %d\n", line, val);
        Log(buf);
        return;
    }
    case SCMD_GET:
    {
        EventOption *o = Script_FindOption(Script_Trim(line));
        ok = o != 0;
        if (!ok)
            break;
        if (o->kind == OPTKIND_STRING && o->val >= 0 && o->val < o->value_num)
            sprintf(buf, "LLGET %s = %d (%s)\n", line, o->val, o->values[o->val]);
        else
            sprintf(buf, "LLGET %s = %d\n", line, o->val);
        Log(buf);
        return;
    }
    case SCMD_CLOSEMENU:
        Presets_KeepUser();
        return;
    case SCMD_VIEW:
    {
        char *what = Script_Word(&rest), *n = Script_Word(&rest);
        int v = n ? (int)Script_Number(&n) : 0;
        ok = what && v >= 1 && v <= VIEW_SLOTS;
        if (!ok)
            break;
        if (Script_Is(what, "show"))
        {
            // where the camera is now, to compare with view v as saved
            COBJ *cobj = View_CObj();
            CamView *w = View_Find(v);
            Vec3 eye = {0}, at = {0};
            if (cobj)
            {
                COBJ_GetEyePosition(cobj, &eye);
                COBJ_GetInterest(cobj, &at);
            }
            sprintf(buf, "LLVIEW show: eye %.1f %.1f %.1f at %.1f %.1f %.1f fov %.1f; view %d \"%s\" on stage %d %s\n", eye.X,
                    eye.Y, eye.Z, at.X, at.Y, at.Z, cobj ? cobj->projection_param.perspective.fov : 0.f, v, view_label[v - 1],
                    Stage_GetExternalID(),
                    !w ? "unsaved"
                    : fabs(eye.X - w->eye.X) + fabs(eye.Y - w->eye.Y) + fabs(eye.Z - w->eye.Z) < 0.5f ? "matches" : "differs");
            Log(buf);
            break;
        }
        if (Script_Is(what, "shift"))
        {
            // view shift 1 dx dy: slides the Advanced camera (eye and what
            // it looks at) by dx dy, to check effects follow it; the number
            // after shift is ignored
            char *a = Script_Word(&rest), *b = Script_Word(&rest);
            ok = a && b;
            if (ok)
            {
                float dx = Script_Number(&a), dy = Script_Number(&b);
                Options_Camera[CAMOPT_MODE].val = CAM_ADVANCED;
                Event_ChangeCamera(0, CAM_ADVANCED);
                COBJ *cobj = View_CObj();
                if (cobj)
                {
                    COBJ_GetEyePosition(cobj, &dev_cam->free_eye_pos);
                    COBJ_GetInterest(cobj, &dev_cam->free_int_pos);
                    dev_cam->free_fov = cobj->projection_param.perspective.fov;
                }
                dev_cam->free_eye_pos.X += dx;
                dev_cam->free_eye_pos.Y += dy;
                dev_cam->free_int_pos.X += dx;
                dev_cam->free_int_pos.Y += dy;
            }
            break;
        }
        if (Script_Is(what, "name"))
        {
            // view name N Word: names a saved view without the letter grid
            CamView *w = View_Find(v);
            char *t = Script_Word(&rest);
            ok = w && t;
            if (ok)
            {
                Name_Copy(w->name, t);
                preset_dirty = 1;
                Labels_Refresh();
            }
            break;
        }
        Options_Camera[CAMOPT_VIEW].val = v;
        if (Script_Is(what, "save"))
            Event_SaveView(0);
        else if (Script_Is(what, "load"))
            Event_ChangeView(0, v);
        else
            ok = 0;
        break;
    }
    case SCMD_PRESET:
    {
        char *what = Script_Word(&rest);
        int which = Script_Preset(Script_Trim(rest));
        ok = what && which >= 0;
        if (!ok)
            break;
        if (Script_Is(what, "load") || Script_Is(what, "save"))
        {
            Options_Presets[PROPT_PICK].val = which;
            if (what[0] == 'l')
                Event_PresetLoad(0);
            else
                Event_PresetSave(0);
        }
        else if (Script_Is(what, "start"))
        {
            Options_Presets[PROPT_START].val = which;
            Event_ChangePresetStart(0, which);
        }
        else
            ok = 0;
        break;
    }
    case SCMD_CARD:
        if (Script_Is(rest, "flush"))
            Card_Flush();
        else if (Script_Is(rest, "restart"))
        {
            Card_Flush();
            Preset_Apply(&preset_builtin[PS_DEFAULTS - PS_SAVED]);
            Card_Load();
        }
        else
            ok = 0;
        break;
    case SCMD_CHORD:
    {
        char *mods = Script_Word(&rest), *dir = Script_Word(&rest);
        ok = mods && dir;
        if (!ok)
            break;
        int lr = mods[0] == 'l', z = mods[0] == 'z' || mods[2] == 'z';
        int down = Script_Is(dir, "up") ? HSD_BUTTON_DPAD_UP : Script_Is(dir, "down") ? HSD_BUTTON_DPAD_DOWN : Script_Is(dir, "left") ? HSD_BUTTON_DPAD_LEFT : Script_Is(dir, "right") ? HSD_BUTTON_DPAD_RIGHT : 0;
        ok = down != 0 && (lr || z);
        if (ok)
            Chord(lr, z, down);
        break;
    }
    }
    if (!ok)
    {
        sprintf(buf, "LLSCRIPT can't do this step: %s\n", op->label);
        Log(buf);
    }
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
        else if (op->kind == SOP_LEDGE)
        {
            if (!pre)
                return 1;
            if (!Script_PlaceLedge(ft, op))
            {
                sprintf(buf, "LLSCRIPT no ledge, skipping %s\n", scripts[script_cur].name);
                Log(buf);
                Script_End();
                return 0;
            }
        }
        else if (op->kind == SOP_SHOT)
        {
            Options_Game[GOPT_FRAME_ADV].val = 1;
            int n = strlen(op->label);
            if (n > (int)sizeof(shot_label) - 1)
                n = sizeof(shot_label) - 1;
            memcpy(shot_label, op->label, n);
            shot_label[n] = 0;
            sprintf(buf, "LLSHOT %s %s at %d\n", scripts[script_cur].name, op->label, event_vars->game_timer);
            Log(buf);
        }
        else if (op->kind == SOP_MARK)
        {
            sprintf(buf, "LLMARK %s %s at %d\n", scripts[script_cur].name, op->label, event_vars->game_timer);
            Log(buf);
        }
        else if (op->kind == SOP_CMD)
            Script_Cmd(op);
        else
            return 1;
        Script_Next();
        // the steps after a shot wait for the next frame, so the frozen
        // frame shows what the script set up for it
        if (op->kind == SOP_SHOT && !pre)
            return 1;
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

    // the windows the cues show: for a waveland, a late one when that's all
    // there is
    for (int k = 1; k <= last && k < p->uncertain_from; k++)
    {
        if ((wl ? WL_Mask(p, k) : p->ai_mask[k]) & bit)
        {
            *width = 0;
            while (k + *width <= last && k + *width < p->uncertain_from &&
                   ((wl ? WL_Mask(p, k + *width) : p->ai_mask[k + *width]) & bit))
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
        int sel = Options_Dev[DOPT_SCRIPT].val;
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

#define JT_D 31             // frames to jump on: d = 0 to 30 frames of fall first
#define JT_STICKS 5
#define JT_CANDIDATES (JT_D * JT_STICKS * 2) // frame, stick, and then holding its x or not
#define JT_SIM 80           // frames simulated after the jump
#define JT_BEST 8           // routes kept per kind
#define JT_BOTTOM 30.f      // how far under the lowest floor a fall is given up on
#define JT_REANCHOR (JT_D - 5) // frames after which the anchor has too few jump frames left
#define JT_STICK_TOL 0.05f  // how far the stick may move before the anchor's fall is wrong
#define JT_DRIFT_TOL 0.05f  // ... and how far Falcon may be from the simulated fall
#define JT_BUDGET LR_BUDGET // simulated frames per game frame (the time limit LR_TIME_US holds it too)
#define JT_TRY (1 + 5 * LR_WINDOWS)
#define JT_PATH (JT_D + 1 + JT_SIM + 2)

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
static JumpRoute jt_best[2][JT_BEST]; // per kind (NIL, AI), best first
static int jt_best_n[2];
static JumpRoute jt_route;      // the one shown
static int jt_have;             // ... there is one
static Vec2 *jt_path;           // [JT_PATH], allocated in Event_Init: where it goes, from the anchor
static float *jt_path_bottom;
static int jt_path_num;
static JumpRoute jt_path_of;    // the route the path is for
static int jt_path_anchor = -1;

static int Jump_Showing(void)
{
    return jt_active && jt_have && Options_Jump[JOPT_SHOW].val && !route_active && hang_ledge < 0;
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
    return a->kind == b->kind && a->d == b->d && a->stick == b->stick && a->aerial == b->aerial && a->press == b->press;
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
static void Jump_Try(FighterData *fp, int d, int stick, int hold, JumpRoute *out, int *n_out, Vec2 *path, float *bottom,
                     int *num)
{
    SimStart st = jt_start;
    SimState s = jt_cache[d];
    SimStep step;
    int n = 0;
    *n_out = 0;
    if (path)
    {
        *num = 0;
        for (int i = 0; i <= d; i++)
        {
            path[n] = (Vec2){jt_cache[i].x, jt_cache[i].y};
            bottom[n++] = jt_cache[i].bottom;
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
    SimStart ps;
    Sim_ToStart(&s, &st, &ps);
    sim_limit = JT_SIM;
    sim_bottom_y = jt_bottom;
    Predict(fp, &ps, pred_route, BR_AI);
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
    base.d = d;
    base.dj = d + 1;
    base.press = -1;
    base.changes = Jump_Changes(stick, hold);

    int nr = 0;
    if (p->land_frame && p->land_kind == LAND_NIL && p->uncertain_from > p->land_frame)
    {
        JumpRoute *r = &out[nr++];
        *r = base;
        r->valid = 1;
        r->kind = LAND_NIL;
        r->land = base.dj + p->land_frame;
        r->act = r->land;
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
                r->press = base.dj + k;
                r->press_w = w;
                r->land = base.dj + touch;
                r->act = r->land + normal_lag;
                windows++;
            }
            k += w - 1;
        }
    }
    *n_out = nr;
}

// The jumps still ahead, and the best of them of the kind Kind asks for.
static void Jump_Prune(int e)
{
    for (int k = 0; k < 2; k++)
    {
        int n = 0;
        for (int i = 0; i < jt_best_n[k]; i++)
        {
            if (jt_best[k][i].dj > e)
                jt_best[k][n++] = jt_best[k][i];
        }
        jt_best_n[k] = n;
    }
}

static JumpRoute *Jump_Pick(void)
{
    int kinds = Options_Jump[JOPT_KIND].val;
    JumpRoute *best = 0;
    for (int k = 0; k < 2; k++)
    {
        if ((k == 0 && kinds == ROUTES_AI) || (k == 1 && kinds == ROUTES_NIL) || jt_best_n[k] == 0)
            continue;
        if (!best || Jump_Better(&jt_best[k][0], best))
            best = &jt_best[k][0];
    }
    return best;
}

// Falcon with the fall as the anchor: the fall before the jump, stepped
// once with the stick held, and every frame of it kept. Starts the search
// over.
static void Jump_Anchor(FighterData *fp, int ts)
{
    jt_active = 1;
    jt_anchor = event_vars->game_timer;
    Sim_FromFighter(fp, ts, frame_in_state, &jt_start);
    jt_stick_x = fp->input.lstick.X;
    jt_stick_down = Stick_Down(fp->input.lstick.Y);
    jt_stick_drop = Stick_Drop(fp->input.lstick.Y);

    // how far down is lost: under the lowest floor
    float low = 100000.f;
    for (int i = 0; i < floor_num; i++)
    {
        FloorLine *f = &floor_cache[i];
        float y = f->y0 < f->y1 ? f->y0 : f->y1;
        if (y < low)
            low = y;
    }
    jt_bottom = floor_num > 0 ? low - JT_BOTTOM : -100000.f;

    SimState s;
    SimStep step;
    Sim_Init(&jt_start, &s);
    jt_cache[0] = s;
    jt_cache_n = 1;
    for (int d = 1; d < JT_D; d++)
    {
        Sim_Step(fp, &jt_start, &s, -1, 0, &step);
        if (step.landed || step.ceiling)
            break;
        jt_cache[d] = s;
        jt_cache_n = d + 1;
    }

    jt_next = 0;
    jt_done = 0;
    jt_found = 0;
    jt_best_n[0] = 0;
    jt_best_n[1] = 0;
    jt_have = 0;
}

// Is Falcon in a fall the search is for: airborne with the double jump
// left, in a fall or the first jump, not hit, on a ledge route or in
// Assist.
static int Jump_Eligible(FighterData *fp, int ts, int tracked_air)
{
    if (!Options_Jump[JOPT_SHOW].val || !tracked_air)
        return 0;
    if (ts != TS_FALL && ts != TS_JUMPF && ts != TS_JUMPB)
        return 0;
    if (fp->jump.jumps_used >= fp->attr.max_jumps)
        return 0;
    return !route_active && hang_ledge < 0 && !Options_Ledge[LOPT_ASSIST].val;
}

// The player did something the anchor's fall didn't assume (Segment_
// InputChanged's checks, with a little give on the stick's x so a hand at
// rest doesn't keep starting the search over).
static int Jump_InputChanged(FighterData *fp, int ts)
{
    if (fabs(fp->input.lstick.X - jt_stick_x) > JT_STICK_TOL)
        return 1;
    if (Stick_Down(fp->input.lstick.Y) != jt_stick_down)
        return 1;
    if (Stick_Drop(fp->input.lstick.Y) != jt_stick_drop)
        return 1;
    if ((u8)fp->input.timer_lstick_tilt_y < jt_prev_tilt) // a new flick
        return 1;
    if (ts != jt_prev_ts && !Tracked_EndedInto(jt_prev_ts, ts))
        return 1;
    return 0;
}

// Falcon is not where the anchor's fall has him (Segment_CheckDrift's
// check): wind, a push, a model gap.
static int Jump_Drifted(FighterData *fp, int e)
{
    if (e < 1 || e >= jt_cache_n)
        return 0;
    return fabs(fp->phys.pos.X - jt_cache[e].x) > JT_DRIFT_TOL || fabs(fp->phys.pos.Y - jt_cache[e].y) > JT_DRIFT_TOL;
}

// Each frame, before the search: is Falcon in a fall to search, and is the
// anchor still right for it. It is taken again when he comes back to a fall
// after leaving one, when the stick or his path leaves what it assumed, and
// when too few of its jump frames are left.
static void Jump_Update(FighterData *fp, int ts, int tracked_air)
{
    int e = event_vars->game_timer - jt_anchor;
    if (!Jump_Eligible(fp, ts, tracked_air))
    {
        jt_active = 0;
        jt_have = 0;
    }
    else if (!jt_active || e < 0 || e >= JT_REANCHOR || Jump_InputChanged(fp, ts) || Jump_Drifted(fp, e))
        Jump_Anchor(fp, ts);
    jt_prev_ts = ts;
    jt_prev_tilt = (u8)fp->input.timer_lstick_tilt_y;
}

// The facing Falcon has as he presses the aerial: a jump backwards turns
// him around.
static float Jump_Facing(JumpRoute *r)
{
    float face = jt_start.facing > 0 ? 1.f : -1.f;
    return jt_stick_xy[r->stick][0] * face > -common_jump_back_stick ? face : -face;
}

// The search's one line in the log: the best jump found for an anchor,
// frames counted from the anchor.
static void Jump_Log(void)
{
    JumpRoute *b = Jump_Pick();
    char buf[200];
    int n = sprintf(buf, "LLJUMP anchor %d %.3f %.3f found %d", jt_anchor, jt_start.pos.X, jt_start.pos.Y, jt_found);
    if (!b)
        sprintf(buf + n, " best none\n");
    else if (b->kind == LAND_AI)
        sprintf(buf + n, " best d %d stick %d aerial %d press %d-%d land %d kind AI\n", b->d, b->stick, b->aerial, b->press,
                b->press + b->press_w - 1, b->land);
    else
        sprintf(buf + n, " best d %d stick %d aerial none press none land %d kind NIL\n", b->d, b->stick, b->land);
    OSReport("%s", buf);
}

// Search on: the candidates in the order of the jump frame, the soonest
// first, until the budget of simulated frames or of real time is spent. A
// jump whose frame has gone by is not tried.
static void Jump_Solve(FighterData *fp)
{
    if (!jt_active || jt_done)
        return;
    int e = event_vars->game_timer - jt_anchor;
    int start = sim_steps;
    int t0 = OSGetTick();
    while (jt_next < JT_CANDIDATES && sim_steps - start < JT_BUDGET)
    {
        if (jt_next < e * JT_STICKS * 2)
            jt_next = e * JT_STICKS * 2;
        if (jt_next >= JT_CANDIDATES)
            break;
        int c = jt_next++;
        int d = c / (JT_STICKS * 2), stick = (c / 2) % JT_STICKS, hold = c % 2;
        if (d >= jt_cache_n)
        {
            jt_next = JT_CANDIDATES; // he lands before this frame
            break;
        }
        if (stick == JT_UP && hold)
            continue; // up has no x to keep
        JumpRoute found[JT_TRY];
        int found_n;
        Jump_Try(fp, d, stick, hold, found, &found_n, 0, 0, 0);
        for (int i = 0; i < found_n; i++)
        {
            jt_found++;
            Jump_Insert(jt_best[found[i].kind == LAND_AI], &jt_best_n[found[i].kind == LAND_AI], &found[i]);
        }
        if (OSTicksToMicroseconds(OSGetTick() - t0) >= LR_TIME_US)
            break;
    }
    if (jt_next >= JT_CANDIDATES)
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
    if (jt_path_anchor == jt_anchor && jt_path_num > 0 && o->d == r->d && o->stick == r->stick && o->hold == r->hold)
        return;
    JumpRoute found[JT_TRY];
    int found_n;
    Jump_Try(fp, r->d, r->stick, r->hold, found, &found_n, jt_path, jt_path_bottom, &jt_path_num);
    jt_path_of = *r;
    jt_path_anchor = jt_anchor;
}

// "DJ up in 6f, nair f7-9: AI": the frames are counted from now, the same
// as the row's cells.
static void Jump_Text(JumpRoute *r, int e)
{
    char aerial[8];
    int until = r->dj - e - 1;
    char *t = text_next;
    if (until > 0)
        t += sprintf(t, "DJ %s in %df", jt_stick_names[r->stick], until);
    else
        t += sprintf(t, "DJ %s now", jt_stick_names[r->stick]);
    if (r->kind == LAND_AI)
    {
        sprintf(aerial, "%s", tracked_state_names[r->aerial]);
        aerial[0] |= 0x20; // lower case
        int first = r->press - e - 1;
        if (r->press_w > 1)
            t += sprintf(t, ", %s f%d-%d: AI", aerial, first, first + r->press_w - 1);
        else
            t += sprintf(t, ", %s f%d: AI", aerial, first);
    }
    else
        sprintf(t, ": NIL");
    if (r->hold && r->stick != JT_UP)
        sprintf(text_steps, "then hold %s", r->stick == JT_LEFT || r->stick == JT_UPLEFT ? "left" : "right");
    else
        sprintf(text_steps, "then let the stick go");
    next_kind = r->kind == LAND_AI ? CUE_AI : CUE_NIL;
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
    int e = event_vars->game_timer - jt_anchor;
    Jump_Prune(e);
    JumpRoute *b = Jump_Pick();
    jt_have = b != 0;
    if (!b)
        return;
    jt_route = *b;
    Jump_Path(fp);
    Jump_Text(&jt_route, e);
}

static void Draw_JumpPath(void)
{
    if (!Jump_Showing() || !Options_Paths[POPT_ROUTE].val || jt_path_num < 2)
        return;
    int from = event_vars->game_timer - jt_anchor;
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
        return;
    JumpRoute *r = &jt_route;
    int e = event_vars->game_timer - jt_anchor;
    int until = r->dj - e - 1; // cells to the jump: 0 is the next frame

    MeterRow *row = Meter_Add(color_in_jump, "JUMP");
    if (!row)
        return;
    for (int n = e + 1; n < r->dj; n++)
        Route_Cell(row, -e - 1, n, CELL_AIR, 0);
    Route_Cell(row, -e - 1, r->dj, CELL_PRESS, GLYPH_JUMP);
    if (until == 0)
        row->hot = 2;
    if (until > 0)
        sprintf(row->info, "%df", until);
    else
        sprintf(row->info, "now");
    route_rows_active = 1;

    row = Meter_Add(land_kind_colors[r->kind], r->kind == LAND_AI ? "AI" : "NIL");
    if (!row)
        return;
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

// The inputs along the paths, where each is due: a chip beside the line
// with the stick's direction, the jump, the aerial or the airdodge, joined
// to its frame by a thin leader. Chips that would overlap stack downward in
// order, so the quick inputs at the ledge stay apart. On the HUD, so they
// stay on top of everything and keep their size.
#define MK_MAX 6
#define MK_H 1.2f   // chip height, at Marker Size 1
#define MK_GW 1.0f  // width per glyph
#define MK_GAP 0.25f

static float Marker_Scale(void)
{
    return mark_sizes[Options_Paths[POPT_MARK_SIZE].val];
}

typedef struct Marker
{
    float px, py; // the frame's point
    float cx, cy; // the chip's middle
    float w;
    int n;
    u8 glyph[3];
    GXColor color[3];
} Marker;

static Marker mk[MK_MAX];
static int mk_num;

static Marker *Marker_Add(float wx, float wy)
{
    if (mk_num >= MK_MAX)
        return 0;
    Marker *m = &mk[mk_num];
    if (!Hud_FromWorld(wx, wy, &m->px, &m->py))
        return 0;
    m->n = 0;
    mk_num++;
    return m;
}

static void Marker_Glyph(Marker *m, int glyph, GXColor c)
{
    if (m && glyph && m->n < 3)
    {
        m->glyph[m->n] = glyph;
        m->color[m->n++] = c;
    }
}

// The chip placed before this one that it would overlap, -1 none.
static int Marker_Hit(int i, float h, float gap)
{
    Marker *m = &mk[i];
    for (int j = 0; j < i; j++)
    {
        Marker *o = &mk[j];
        if (fabs(o->cx - m->cx) < (o->w + m->w) / 2 + gap && fabs(o->cy - m->cy) < h + gap)
            return j;
    }
    return -1;
}

// side: -1 puts the chips left of the line, 1 right.
static void Markers_Flush(int side)
{
    float S = Marker_Scale();
    float h = MK_H * S, gw = MK_GW * S, gap = MK_GAP * S;
    for (int i = 0; i < mk_num; i++)
    {
        Marker *m = &mk[i];
        m->w = m->n * gw + 0.3f * S;
        m->cx = m->px + side * (1.3f * S + m->w / 2);
        float lim = SAFE_W - m->w / 2;
        if (m->cx > lim)
            m->cx = lim;
        if (m->cx < -lim)
            m->cx = -lim;
        // stacked down out of each other's way, or up once that would
        // leave the screen, and kept on it
        float low = -SAFE_H + h / 2, high = SAFE_H - h / 2;
        int dir = -1;
        m->cy = m->py;
        for (int tries = 0, j; tries < 2 * MK_MAX && (j = Marker_Hit(i, h, gap)) >= 0; tries++)
        {
            m->cy = mk[j].cy + dir * (h + gap);
            if (dir < 0 && m->cy < low)
            {
                dir = 1;
                m->cy = m->py;
            }
        }
        if (m->cy < low)
            m->cy = low;
        if (m->cy > high)
            m->cy = high;
    }
    for (int i = 0; i < mk_num; i++)
    {
        Marker *m = &mk[i];
        float ex = m->cx - side * m->w / 2; // the chip's edge facing the line
        Hud_Seg(m->px, m->py, ex, m->cy, 0.1f, Color_Fill(color_plate, 0.7f));
        Hud_Seg(m->px, m->py, ex, m->cy, 0.05f, Color_Fill(color_white, 0.6f));
        Hud_Diamond(m->px, m->py, 0.3f, Color_Fill(color_plate, 0.8f));
        Hud_Diamond(m->px, m->py, 0.2f, m->color[0]);
    }
    for (int i = 0; i < mk_num; i++)
    {
        Marker *m = &mk[i];
        float x0 = m->cx - m->w / 2, x1 = m->cx + m->w / 2;
        Hud_Rect(x0, m->cy - h / 2, x1, m->cy + h / 2, Color_Fill(color_plate, 0.85f));
        Hud_Frame(x0, m->cy - h / 2, x1, m->cy + h / 2, 0.07f * S, Color_Fill(m->color[0], 0.7f));
        for (int g = 0; g < m->n; g++)
            Glyph_Draw(m->glyph[g], x0 + 0.15f * S + gw * (g + 0.5f), m->cy, 0.36f * S, m->color[g]);
    }
    mk_num = 0;
}

// The aerial picker: at the start of an aerial interrupt's window, a small
// C-stick face with the directions that interrupt there lit (the middle for
// a nair), forward turned the way Falcon faces. Queued from the stage's
// drawing and drawn on the HUD, above its frame.
static void Compass_Draw(void)
{
    float S = Marker_Scale();
    float r = 0.95f * S;
    for (int i = 0; i < compass_num; i++)
    {
        Compass *c = &compass[i];
        float px, py;
        if (!Hud_FromWorld(c->x, c->y, &px, &py))
            continue;
        float cx = px, cy = py + 1.2f * S + r;
        Hud_Seg(px, py, cx, cy - r, 0.1f, Color_Fill(color_plate, 0.7f));
        Hud_Seg(px, py, cx, cy - r, 0.05f, Color_Fill(c->color, 0.7f));
        Hud_Disc(cx, cy, r + 0.15f * S, Color_Fill(color_plate, 0.85f));
        Hud_Ring(cx, cy, r, 0.08f * S, Color_Fill(c->color, 0.6f));
        GXColor off = Color_Fill(color_white, 0.12f);
        // N F B U D, as the aerial bits
        static const float dirs[5][2] = {{0, 0}, {1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (int a = 0; a < 5; a++)
        {
            GXColor col = (c->mask & (1 << a)) ? c->color : off;
            if (a == 0)
            {
                Hud_Disc(cx, cy, 0.22f * r, col);
                continue;
            }
            float dx = dirs[a][0] * c->facing, dy = dirs[a][1];
            float tx = cx + dx * 0.9f * r, ty = cy + dy * 0.9f * r;     // tip
            float bx = cx + dx * 0.38f * r, by = cy + dy * 0.38f * r;   // base
            float wx = -dy * 0.36f * r, wy = dx * 0.36f * r;
            Hud_Tri(tx, ty, bx + wx, by + wy, bx - wx, by - wy, col);
        }
    }
    compass_num = 0;
}

// The C-stick direction of an aerial, nothing for nair.
static int Aerial_Glyph(int aerial, int facing)
{
    switch (aerial)
    {
    case TS_AIRF:
        return facing > 0 ? GLYPH_RIGHT : GLYPH_LEFT;
    case TS_AIRB:
        return facing > 0 ? GLYPH_LEFT : GLYPH_RIGHT;
    case TS_AIRHI:
        return GLYPH_UP;
    case TS_AIRLW:
        return GLYPH_DOWN;
    }
    return 0;
}

static void Route_Marker(int frame, int g0, GXColor c0, int g1, GXColor c1)
{
    int i = frame < route_path_num ? frame : route_path_num - 1;
    Marker *m = Marker_Add(route_path[i].X, route_path[i].Y + route_path_bottom[i]);
    Marker_Glyph(m, g0, c0);
    Marker_Glyph(m, g1, c1);
}

static void Markers_Draw(void)
{
    if (!Options_Paths[POPT_INPUTS].val || capture_clean)
        return;
    mk_num = 0;

    // a ledge route: the inputs still to come
    int route = (hang_ledge >= 0 && route_show_num > 0) || (route_active && !route_dj_done);
    if (route && Routes_On() && route_path_num >= 2 && route_path_ledge >= 0)
    {
        LedgeRoute *r = &route_path_of;
        int facing = ledges[route_path_ledge].facing;
        int from = route_active ? route_e + 1 : 0;
        int away = facing > 0 ? GLYPH_LEFT : GLYPH_RIGHT;
        int in = facing > 0 ? GLYPH_RIGHT : GLYPH_LEFT;
        int ff_away = facing > 0 ? GLYPH_FF_DOWN_LEFT : GLYPH_FF_DOWN_RIGHT;
        if (from <= 0)
            Route_Marker(0, r->drop == DROP_AWAY ? away : GLYPH_DOWN, color_white,
                         r->fall_away && r->drop != DROP_AWAY ? away : 0, color_white);
        if (r->ff > 0 && Route_FF(r) >= from)
            Route_Marker(Route_FF(r), r->fall_away ? ff_away : GLYPH_FF, color_white, 0, color_white);
        if (r->dj >= from)
        {
            int dj_glyph = r->dj_x == LR_DJ_IN     ? in
                           : r->dj_x == LR_DJ_DIAG ? (facing > 0 ? GLYPH_DOWN_RIGHT : GLYPH_DOWN_LEFT)
                                                   : 0;
            Route_Marker(r->dj, GLYPH_JUMP, color_in_jump, dj_glyph, color_white);
        }
        if (r->kind == LAND_AI && r->press >= from)
            Route_Marker(r->press, GLYPH_AERIAL, color_in_aerial, Aerial_Glyph(r->aerial, facing), color_in_aerial);
        // beside the line on the stage's side, clear of Falcon hanging
        // and dropping off it
        Markers_Flush(facing > 0 ? 1 : -1);
        return;
    }

    // a jump: the stick and the jump where it is due, then the aerial
    if (Jump_Showing() && jt_path_num >= 2)
    {
        JumpRoute *r = &jt_route;
        int e = event_vars->game_timer - jt_anchor;
        int i = r->dj < jt_path_num ? r->dj : jt_path_num - 1;
        Marker *m = Marker_Add(jt_path[i].X, jt_path[i].Y + jt_path_bottom[i]);
        Marker_Glyph(m, GLYPH_JUMP, color_in_jump);
        if (r->stick <= JT_UPRIGHT)
            Marker_Glyph(m, GLYPH_UP, color_white);
        if (r->stick == JT_LEFT || r->stick == JT_UPLEFT)
            Marker_Glyph(m, GLYPH_LEFT, color_white);
        if (r->stick == JT_RIGHT || r->stick == JT_UPRIGHT)
            Marker_Glyph(m, GLYPH_RIGHT, color_white);
        if (r->kind == LAND_AI && r->press > e)
        {
            i = r->press < jt_path_num ? r->press : jt_path_num - 1;
            m = Marker_Add(jt_path[i].X, jt_path[i].Y + jt_path_bottom[i]);
            Marker_Glyph(m, GLYPH_AERIAL, color_in_aerial);
            Marker_Glyph(m, Aerial_Glyph(r->aerial, Jump_Facing(r) > 0 ? 1 : -1), color_in_aerial);
        }
        // on the side the path came from, out of its way
        Markers_Flush(jt_path[jt_path_num - 1].X >= jt_path[0].X ? -1 : 1);
    }

    // the next waveland's airdodge, while its timer runs
    Cue *c = &cue_live[CUE_WL];
    Prediction *p = pred_live;
    if (live_visible && Cues_Waveland() && (c->phase == PH_COUNT || c->phase == PH_WINDOW) && !c->dim && !c->wd &&
        p->wl_first > 0 && p->wl_first < p->uncertain_from)
    {
        int k = p->wl_first - 1; // where Falcon is as he presses it
        Marker *m = Marker_Add(p->pos[k].X, p->pos[k].Y + p->bottom[k]);
        Marker_Glyph(m, GLYPH_DODGE, color_in_dodge);
        if (p->wl_dirs & DODGE_LEFT)
            Marker_Glyph(m, GLYPH_LEFT, color_white);
        if (p->wl_dirs & DODGE_RIGHT)
            Marker_Glyph(m, GLYPH_RIGHT, color_white);
        // on the side the path came from, out of its way
        int last = p->land_frame ? p->land_frame : p->num;
        Markers_Flush(p->pos[last].X >= p->pos[0].X ? -1 : 1);
    }
}

// A route's kind and GALINT, like "NIL  11 GALINT" or "AI Bair  15 GALINT".
static void Route_Name(char *t, LedgeRoute *r, int galint)
{
    t += sprintf(t, "%s", r->kind == LAND_AI ? "AI " : "NIL");
    if (r->kind == LAND_AI)
        t += sprintf(t, "%s", tracked_state_names[r->aerial]);
    if (galint > 0)
        sprintf(t, "  %d GALINT", galint);
}

// Its inputs, like "away, wait 1, FF 2, DJ in".
static void Route_Steps(char *t, LedgeRoute *r)
{
    t += sprintf(t, "%s", r->drop == DROP_AWAY ? "away" : "down");
    if (r->fall_away)
        t += sprintf(t, ", drift away");
    if (r->wait > 0)
        t += sprintf(t, ", wait %d", r->wait);
    if (r->ff > 0)
        t += sprintf(t, ", FF %d", r->ff);
    static const char *dj[LR_DJ_X] = {"DJ in", "DJ down-in", "DJ"};
    sprintf(t, ", %s%s", dj[r->dj_x], r->hold ? ", hold in" : "");
}

// The panel's lines for a route: which of the routes it is, its kind and
// GALINT, and its inputs under it.
static void Route_Text(LedgeRoute *r, int galint, int num, int total)
{
    char *t = text_next;
    if (num > 0)
        t += sprintf(t, "Route %d of %d: ", num, total);
    Route_Name(t, r, galint);
    Route_Steps(text_steps, r);
}

// The Route option, in the menu: its value ("4 of 23") and the chosen
// route's GALINT and inputs in the lines of its description. The HUD isn't
// drawn while the menu is open, so this is where browsing shows what each
// route is.
static void Route_MenuText(void)
{
    EventOption *o = &Options_Ledge[LOPT_PICK];
    int n = route_list_num;
    route_desc[0][0] = route_desc[1][0] = route_desc[2][0] = 0;
    strcpy(route_pick_fmt, "%d");
    if (route_list_ledge < 0)
        sprintf(route_desc[0], "Hang from a ledge to browse its routes.");
    else if (!ledges[route_list_ledge].have)
        sprintf(route_desc[0], "%s", Routes_On() ? "Still searching this ledge's routes..." : "Turn on Show Routes to find routes.");
    else if (n == 0)
    {
        strcpy(route_pick_fmt, "none");
        sprintf(route_desc[0], "No route of this kind keeps GALINT.");
    }
    else
    {
        int pick = o->val - 1;
        if (pick >= n)
            pick = n - 1;
        if (pick < 0)
            pick = 0;
        LedgeRoute *r = &route_list[pick];
        sprintf(route_pick_fmt, "%%d of %d", n);
        sprintf(route_desc[0], "Route %d of %d: ", pick + 1, n);
        Route_Name(route_desc[0] + strlen(route_desc[0]), r, Route_Galint(r, -1, (*stc_ftcommon)->cliff_invuln_time));
        Route_Steps(route_desc[1], r);
        if (r->kind == LAND_NIL)
            sprintf(route_desc[2], "No aerial: it lands with no lag.");
        else if (r->press_w == 1)
            sprintf(route_desc[2], "%s on frame %d after the jump.", tracked_state_names[r->aerial], r->press - r->dj);
        else
            sprintf(route_desc[2], "%s on frames %d to %d after the jump.", tracked_state_names[r->aerial],
                    r->press - r->dj, r->press - r->dj + r->press_w - 1);
    }
}

void Event_ChangeRoutes(GOBJ *menu, int value)
{
    // a new kind or order rebuilds the list and keeps the chosen route; only
    // a new Route number is a new choice
    if (!Routes_Update(route_browse))
        Route_RememberPick();
    if (hang_ledge >= 0 && ledges[hang_ledge].have)
        Routes_Show(); // the rows follow the choice as soon as the game goes on
    Route_MenuText();
}

// The route was lost, or a step whose timing buzzes went wrong: say which
// and by how much, and drop it.
static void Route_Fail(const char *step, int off, int buzz)
{
    char buf[96];
    if (off == LR_PLAIN)
        sprintf(text_last, "%s", step);
    else if (off == 0)
        sprintf(text_last, "%s missed", step);
    else
        sprintf(text_last, "%s %df %s", step, off < 0 ? -off : off, off < 0 ? "early" : "late");
    last_kind = -1;
    route_active = 0;
    route_fail = event_vars->game_timer;
    if (buzz)
        SFX_PlayCommon(3);
    sprintf(buf, "LandingLab route: %s at %d after the drop\n", text_last, route_e);
    Log(buf);
    Drill_Finish(0);
}

// A step off its frame. When its timing is checked under Sounds, or Assist
// is drilling the route, it's a miss (returns 1, the route is dropped);
// otherwise it's only noted, and the attempt goes on to be judged by how it
// lands. opt -1: a slip no Sounds option checks (a bonk).
static int Route_Slip(int opt, const char *step, int off)
{
    int checked = opt >= 0 && Options_Sounds[opt].val;
    if (checked || Options_Ledge[LOPT_ASSIST].val)
    {
        Route_Fail(step, off, checked || Options_Sounds[SOPT_LOST].val);
        return 1;
    }
    char note[24], buf[80];
    if (off == LR_PLAIN)
        sprintf(note, "%s", step);
    else if (off == 0)
        sprintf(note, "%s missed", step);
    else
        sprintf(note, "%s %df %s", step, off < 0 ? -off : off, off < 0 ? "early" : "late");
    if (!route_note[0])
        strcpy(route_note, note);
    sprintf(buf, "LandingLab route: slip %s at %d after the drop\n", note, route_e);
    Log(buf);
    return 0;
}

// How the route was lost, with the slip that likely did it.
static const char *Route_Why(const char *what)
{
    static char why[48];
    if (route_note[0])
        sprintf(why, "%s, %s", what, route_note);
    else
        sprintf(why, "%s", what);
    return why;
}

static void Route_Log(const char *what, LedgeEntry *L, LedgeRoute *r, int galint)
{
    char buf[200];
    sprintf(buf, "LLROUTE %s ledge %.4f %.4f facing %d kind %s drop %s wait %d ff %d hold %d dj %d dj_x %d away %d ff_at %d press %d w %d aerial %d land %d act %d galint %d\n",
            what, L->x, L->y, L->facing, r->kind == LAND_AI ? "AI" : "NIL", r->drop == DROP_AWAY ? "away" : "down", r->wait,
            r->ff, r->hold, r->dj, r->dj_x, r->fall_away, r->ff_at, r->press, r->press_w, r->aerial, r->land, r->act, galint);
    Log(buf);
}

static void Route_Step(void)
{
    route_hit = event_vars->game_timer;
}

// Each frame: hanging, letting go, and every step of the route after it.
// A step is judged a frame after it was due, so one frame late reads as
// late rather than missed.
// The ledge drop drill. The game lets go of the ledge only after a frame
// of the hang with the stick at rest (CliffWait clears the flag as it
// starts and sets it on a frame the stick is in its deadzone), so holding
// down through the catch drops nothing: the stick rests on the hang's
// first frame and goes down or away on the second, the first frame a drop
// can come out. A drop on that frame or the next is a hit.
#define DROP_HIST 10
static int drop_w0 = -1;     // game_timer of the hang's first frame, -1 not hanging
static int drop_catch_left;  // frames of the catch left
static int drop_rest;        // the stick rested on the hang's first frame
static u8 drop_hist[DROP_HIST];
static int drop_hist_n, drop_hist_pos;
static int drop_age = 99;    // frames since the last graded drop
static int drop_last;        // its frame: 1 = the first possible

static int Drop_On(void)
{
    return Options_Ledge[LOPT_DROP].val;
}

static void Drop_Think(FighterData *fp, int sid, int prev_sid)
{
    drop_sid = sid;
    if (prev_sid < 0)
        drop_w0 = -1; // a test script just put Falcon somewhere
    if (drop_age < 99)
        drop_age++;
    if (sid == ASID_CLIFFCATCH)
    {
        Figatree *anim = fp->figatree_curr;
        float rate = fp->state.rate > 0 ? fp->state.rate : 1.f;
        drop_catch_left = anim ? (int)((anim->frame_num - fp->state.frame) / rate + 0.999f) : 0;
        if (drop_w0 != -2 && Drop_On() && cue_log)
            OSReport("LLDROP catch at %d, %d left\n", event_vars->game_timer, drop_catch_left);
        drop_w0 = -2; // (-2: logged)
        return;
    }
    if (sid == ASID_CLIFFWAIT)
    {
        if (drop_w0 < 0)
        {
            drop_w0 = event_vars->game_timer;
            FtCliffCatch *cliff = (void *)&fp->state_var;
            drop_rest = cliff->timer != 0;
            if (Drop_On() && cue_log)
                OSReport("LLDROP hang at %d rest %d\n", drop_w0, drop_rest);
        }
        return;
    }
    if (prev_sid == ASID_CLIFFWAIT && drop_w0 >= 0 && sid == ASID_FALL && Drop_On())
    {
        int k = event_vars->game_timer - drop_w0;
        int hit = k >= 1 && k <= 2;
        drop_hist[drop_hist_pos] = hit;
        drop_hist_pos = (drop_hist_pos + 1) % DROP_HIST;
        if (drop_hist_n < DROP_HIST)
            drop_hist_n++;
        int n = 0;
        for (int i = 0; i < drop_hist_n; i++)
            n += drop_hist[i];
        drop_last = k;
        drop_age = 0;
        if (hit)
            sprintf(text_last, "Drop frame %d, %d of last %d on 1-2", k, n, drop_hist_n);
        else if (!drop_rest)
            sprintf(text_last, "Drop f%d: rest stick as the hang starts", k);
        else
            sprintf(text_last, "Drop frame %d, %d late", k, k - 2);
        last_kind = -1;
        if (hit && Options_Sounds[SOPT_CHIME].val)
            SFX_PlayRaw(303, 255, 128, 20, 3);
        else if (!hit && Options_Sounds[SOPT_WINDOW].val)
            SFX_PlayCommon(3);
        char buf[96];
        sprintf(buf, "LLDROP frame %d rest %d hits %d of %d\n", k, drop_rest, n, drop_hist_n);
        Log(buf);
    }
    drop_w0 = -1;
}

// Its row in the timers: the hang's first frame (stick at rest) and the two
// drop frames, counting down through the catch, then the result.
static void Meter_AddDrop(int sid)
{
    if (!Drop_On())
        return;
    int hanging = sid == ASID_CLIFFWAIT && drop_w0 >= 0;
    if (sid != ASID_CLIFFCATCH && !hanging && drop_age > 20)
        return;
    MeterRow *r = Meter_Add(color_galint, "DRP");
    if (!r)
        return;
    if (sid == ASID_CLIFFCATCH || hanging)
    {
        // index 0 is now; the hang's first frame is drop_catch_left away
        int rest = sid == ASID_CLIFFCATCH ? drop_catch_left : -(event_vars->game_timer - drop_w0);
        if (rest >= 0)
            Row_Set(r, rest, CELL_LAND, TONE_CUE, 1); // hollow: nothing held
        for (int j = 1; j <= 2; j++)
            if (rest + j >= 0)
                Row_Set(r, rest + j, CELL_PRESS, TONE_CUE, 1);
        if (rest + 1 <= 0 && rest + 2 >= 0)
        {
            r->hot = rest + 1 == 0 || rest + 2 == 0 ? 2 : 1;
            r->spent = -(rest + 1);
        }
        if (rest > 0)
            sprintf(r->info, "%df", rest + 1);
        else if (rest + 2 >= 0)
            sprintf(r->info, "now");
        else
            sprintf(r->info, "late");
    }
    else
    {
        int hit = drop_last >= 1 && drop_last <= 2;
        if (hit)
            r->burst = drop_age;
        else
        {
            r->implode = drop_age;
            r->implode_tone = TONE_MISS;
        }
        sprintf(r->info, "f%d", drop_last);
    }
}

static void Ledge_Think(FighterData *fp, int sid)
{
    if (!ledges_found)
    {
        Ledge_FindMain();
        ledges_found = 1;
    }

    int hanging = sid == ASID_CLIFFWAIT;
    if (hanging && Options_Ledge[LOPT_INV].val)
    {
        fp->hurt.intang_frames.ledge = (*stc_ftcommon)->cliff_invuln_time + 1;
        FtCliffCatch *cliff = (void *)&fp->state_var;
        cliff->fall_timer = 2;
    }
    int intang = fp->hurt.intang_frames.ledge;
    galint_now = hanging ? 0 : intang;
    if (!hanging)
        Routes_Update(route_browse); // the Route option's text, before the first ledge too

    if (hanging)
    {
        // learn where Falcon really hangs, and keep the ledge's routes ready
        int facing = fp->facing_direction > 0 ? 1 : -1;
        hang_ledge = Ledge_Add(fp->phys.pos.X, fp->phys.pos.Y, facing);
        attempt_active = 0;
        route_active = 0;
        route_show_num = 0;
        if (hang_ledge >= 0)
            route_browse = hang_ledge;
        Routes_Update(hang_ledge);
        // a ledge's routes show once its first search is done, and the
        // ones of the search before keep showing while it's searched again
        if (hang_ledge >= 0 && ledges[hang_ledge].have)
        {
            Routes_Show();
            if (Log_Level())
            {
                LedgeEntry *L = &ledges[hang_ledge];
                if (!L->logged)
                {
                    char buf[96];
                    sprintf(buf, "LLROUTES ledge %.4f %.4f facing %d found %d nil %d ai %d\n", L->x, L->y, L->facing,
                            route_list_num, L->list.n[ROUTES_SORT_GALINT][0], L->list.n[ROUTES_SORT_GALINT][1]);
                    Log(buf);
                    for (int i = 0; i < LR_KEEP && i < route_list_num; i++)
                        Route_Log(Ordinal(i + 1), L, &route_list[i], Route_Galint(&route_list[i], -1, intang));
                }
                L->logged = 1;
            }
            if (route_show_num > 0 && Routes_On())
            {
                Route_Path(fp, hang_ledge, &route_show[0]);
                Route_Text(&route_show[0], Route_Galint(&route_show[0], -1, intang), route_show_rank + 1, route_list_num);
                next_kind = route_show[0].kind == LAND_AI ? CUE_AI : CUE_NIL;
            }
            else if (Routes_On())
            {
                int kind = Options_Ledge[LOPT_KIND].val;
                sprintf(text_next, "No %s route with GALINT", kind == ROUTES_BOTH ? "NIL or AI" : route_kind_names[kind]);
                next_kind = -1;
            }
        }
        else if (Routes_On())
        {
            sprintf(text_next, "Finding ledge routes...");
            next_kind = -1;
        }
    }

    // letting go
    if (prev_state_id == ASID_CLIFFWAIT && !hanging)
    {
        int ledge = hang_ledge;
        hang_ledge = -1;
        attempt_active = 1;
        attempt_facing = fp->facing_direction > 0 ? 1 : -1;
        route_active = 0;
        if (sid == ASID_FALL && ledge >= 0 && route_show_num > 0 && Routes_On())
        {
            // follow the chosen route, or the best one let go the same way
            int drop = fp->input.lstick.Y <= -Common_Float(0x494) && fabs(fp->input.lstick.X) < -fp->input.lstick.Y ? DROP_DOWN : DROP_AWAY;
            LedgeRoute *pick = 0;
            route_cur_num = 0;
            if (route_show[0].drop == drop)
            {
                pick = &route_show[0];
                route_cur_num = route_show_rank + 1;
            }
            for (int i = 0; i < route_list_num && !pick; i++)
            {
                if (route_list[i].drop == drop)
                {
                    pick = &route_list[i];
                    route_cur_num = i + 1;
                }
            }
            if (pick)
            {
                route_cur = *pick;
                route_cur_total = route_list_num;
                route_ledge = ledge;
                route_active = 1;
                route_drop = event_vars->game_timer;
                route_dj_done = 0;
                route_aerial_done = 0;
                route_ff_done = 0;
                route_off = 0;
                route_bonked = 0;
                route_any_aerial = 0;
                route_note[0] = 0;
                route_landed = -1;
                route_galint = -1;
                route_pred = Route_Galint(&route_cur, 0, intang);
                if (Log_Level())
                    Route_Log("follow", &ledges[ledge], &route_cur, route_pred);
                route_hit = event_vars->game_timer;
                prev_fastfall = 0;
                Route_Path(fp, ledge, &route_cur);
            }
        }
        route_show_num = 0;
    }

    if (route_active)
    {
        LedgeRoute *r = &route_cur;
        int e = event_vars->game_timer - route_drop;
        route_e = e;
        int ff_now = fp->flags.is_fastfall && !prev_fastfall;
        prev_fastfall = fp->flags.is_fastfall;
        int jumped = (sid == ASID_JUMPAERIALF || sid == ASID_JUMPAERIALB) && prev_state_id != sid;
        int in_aerial = sid >= ASID_ATTACKAIRN && sid <= ASID_ATTACKAIRLW;
        int aerial = in_aerial && prev_state_id != sid;
        int landed = fp->phys.air_state == 0;
        // an aerial whose ECB touched down on its first frame never shows
        if (!aerial && landed && sid == ASID_LANDING && !route_any_aerial && !route_aerial_done)
            aerial = Aerial_Pressed(fp) >= 0;

        if (fp->flags.hitlag || fp->flags.hitstun || sid == ASID_CLIFFCATCH || hanging)
        {
            route_active = 0; // hit, or back on a ledge: no longer the route
            return;
        }
        Route_Text(r, route_galint >= 0 ? route_galint : Route_Galint(r, e, intang), route_cur_num, route_cur_total);
        next_kind = r->kind == LAND_AI ? CUE_AI : CUE_NIL;

        if (route_landed < 0)
        {
            // the routes never touch a ceiling: one that does went another way
            if ((fp->coll_data.envFlags & COLL_CEILING) && !route_bonked)
            {
                route_bonked = 1;
                route_off = 1;
                if (Route_Slip(-1, "Bonked", LR_PLAIN))
                    return;
            }
            int ff = Route_FF(r);
            if (ff_now && route_dj_done)
                ff_now = 0; // after the jump: the route is past its inputs
            if (ff_now && !route_ff_done)
            {
                route_ff_done = 1;
                if (r->ff == 0 ? Route_Slip(SOPT_FF, "FF not in route", LR_PLAIN)
                               : e != ff ? Route_Slip(SOPT_FF, "FF", e - ff) : (Route_Step(), 0))
                    return;
            }
            else if (r->ff > 0 && e > ff && !route_ff_done)
            {
                route_ff_done = 1;
                if (Route_Slip(SOPT_FF, "FF", 0))
                    return;
            }

            if (jumped && !route_dj_done)
            {
                route_dj_done = 1;
                if (e != r->dj)
                {
                    route_off = 1;
                    if (Route_Slip(SOPT_JUMP, "Jump", e - r->dj))
                        return;
                }
                else
                    Route_Step();
            }
            else if (e > r->dj && !route_dj_done)
            {
                route_dj_done = 1;
                route_off = 1;
                if (Route_Slip(SOPT_JUMP, "Jump", 0))
                    return;
            }

            int last = r->press + r->press_w - 1;
            if (r->kind == LAND_AI && aerial && !route_aerial_done)
            {
                route_aerial_done = 1;
                if (!route_off && (e < r->press || e > last))
                {
                    route_off = 1;
                    if (Route_Slip(SOPT_AERIAL, tracked_state_names[r->aerial], e < r->press ? e - r->press : e - last))
                        return;
                }
                else if (!route_off)
                    Route_Step();
            }
            else if (r->kind == LAND_AI && !route_aerial_done && !route_off && e > last && !landed)
            {
                route_off = 1;
                if (Route_Slip(SOPT_AERIAL, tracked_state_names[r->aerial], 0))
                    return;
            }

            if (aerial)
                route_any_aerial = 1;
            if (landed)
            {
                // judged by how it landed, whatever the route: on the stage
                // with no lag (a NIL), or out of an aerial with only the
                // normal landing's lag (an AI)
                if (sid != ASID_WAIT && !(sid == ASID_LANDING && route_any_aerial))
                {
                    Route_Fail(Route_Why("Landed with lag"), LR_PLAIN, Options_Sounds[SOPT_LOST].val);
                    return;
                }
                route_landed = e;
                route_act = sid == ASID_WAIT ? e : e + (int)fp->attr.normal_landing_lag;
            }
            else if (fp->phys.pos.Y < ledges[route_ledge].y - 40.f)
            {
                Route_Fail(Route_Why("Missed the stage"), LR_PLAIN, Options_Sounds[SOPT_LOST].val);
                return;
            }
        }
        if (route_landed >= 0 && route_galint < 0 && e >= route_act)
        {
            // GALINT, read where ledgedash training reads it
            char buf[96];
            route_galint = intang;
            if (intang <= 0)
            {
                Route_Fail(Route_Why("No GALINT left"), LR_PLAIN, Options_Sounds[SOPT_LOST].val);
                return;
            }
            int t = sprintf(text_last, "Ledge %s  %d GALINT", route_act == route_landed ? "NIL" : "AI", intang);
            if (route_note[0])
                sprintf(text_last + t, ", %s", route_note);
            last_kind = route_act == route_landed ? CUE_NIL : CUE_AI;
            if (Options_Sounds[SOPT_ROUTE_CHIME].val)
                SFX_PlayRaw(303, 255, 128, 20, 3);
            sprintf(buf, "LandingLab route: %s, predicted %d on the drop\n", text_last, route_pred);
            Log(buf);
            Drill_Finish(1);
        }
        if (route_galint >= 0)
        {
            // the row runs on until the tail and the ACT flash are gone
            int end = Route_TailEnd(r, route_galint);
            if (end < Route_ActCell(r) + 3)
                end = Route_ActCell(r) + 3;
            if (end - e - 1 < 0)
                route_active = 0;
        }
        return;
    }

    // an attempt without a route ends at the landing, or far below
    if (attempt_active)
    {
        if (fp->phys.air_state == 0 && !hanging)
        {
            attempt_active = 0;
            Drill_Finish(last_kind == CUE_NIL || last_kind == CUE_AI); // as Landing_Resolve judged it
        }
        else if (fp->phys.pos.Y < -150.f || fp->flags.dead)
        {
            attempt_active = 0;
            Drill_Finish(0);
        }
    }
}

///////////////////////
/// Reset and drill ///
///////////////////////

static int Reset_Mode(void)
{
    int mode = Options_Ledge[LOPT_RESET].val;
    if (mode == RESET_NONE && Options_Ledge[LOPT_ASSIST].val)
        mode = RESET_SAME;
    return mode;
}

static void Drill_Finish(int success)
{
    attempt_active = 0;
    int mode = Reset_Mode();
    if (mode == RESET_NONE)
        return;
    int delay = Options_Ledge[LOPT_DELAY].val;
    drill_timer = success ? reset_delay_hit[delay] : reset_delay_miss[delay];
    drill_success = success;
    int facing = attempt_facing ? attempt_facing : drill_side;
    switch (mode)
    {
    case RESET_SAME:
        drill_side = facing;
        break;
    case RESET_SWAP:
        drill_side = -facing;
        break;
    case RESET_SWAP_HIT:
        drill_side = success ? -facing : facing;
        break;
    case RESET_RANDOM:
        drill_side = HSD_Randi(2) ? 1 : -1;
        break;
    }
}

static void Assist_Clear(void);

// Start over: on a ledge, or from the saved position.
static void Drill_Reset(void)
{
    GOBJ *ft = Fighter_GetGObj(0);
    drill_timer = -1;
    route_active = 0;
    Assist_Clear();
    if (Options_Ledge[LOPT_START].val == START_SAVED && event_vars->savestate->is_exist)
    {
        event_vars->Savestate_Load_v1(event_vars->savestate, 0);
        Script_ResetTracking();
        prev_state_id = ((FighterData *)ft->userdata)->state_id;
        return;
    }
    ScriptOp op = {0};
    op.facing = drill_side;
    Script_PlaceLedge(ft, &op);
    prev_state_id = ((FighterData *)ft->userdata)->state_id;
}

static void Drill_Think(void)
{
    if (drill_timer < 0)
        return;
    if (Reset_Mode() == RESET_NONE)
    {
        drill_timer = -1;
        return;
    }
    if (--drill_timer <= 0)
        Drill_Reset();
}

void Event_ChangeLedgeStart(GOBJ *menu, int value)
{
    drill_timer = -1;
}

void Event_ChangeCamera(GOBJ *menu, int value)
{
    MatchCamera *cam = stc_matchcam;
    if (menu)
        Options_Camera[CAMOPT_VIEW].val = 0; // a mode picked by hand leaves the view
    if (value == 0)
        Match_SetNormalCamera();
    else if (value == 1)
    {
        Match_SetFreeCamera(0, 3);
        cam->freecam_fov.X = 140;
        cam->freecam_rotate.Y = 10;
    }
    else if (value == 2)
        Match_SetFixedCamera();
    else
        Match_SetDevelopCamera();
    Match_CorrectCamera();
}

///////////////////////
/// Assist          ///
///////////////////////

// Quicktime practice of the chosen route: the game freezes on the frame
// before each of its inputs and waits for it. The input comes: the game
// plays on at full speed to the next one. It doesn't come within the wait
// set in the menu: a window longer than a frame steps one frame further
// into it, and after its last frame Falcon starts over. While frozen, the
// stick is fed into Falcon's input timers every real frame, as the game
// would, so a slow press reads as a slow press when the game goes on.

enum assist_step
{
    ASSIST_NONE,
    ASSIST_DROP,
    ASSIST_FF,
    ASSIST_JUMP,
    ASSIST_AERIAL,
};

static int assist_step;      // the input it waits for
static int assist_left;      // frames of the window left, counting this one
static int assist_hang;      // hang frames the drop has been waited for
static int assist_timer = -1; // game frame it froze on

static void Assist_Clear(void)
{
    assist_frozen = 0;
    assist_step = ASSIST_NONE;
    assist_wait = 0;
    assist_advance = 0;
    assist_hang = 0;
    assist_timer = -1;
}

static int Assist_On(void)
{
    return Options_Ledge[LOPT_ASSIST].val && Pause_CheckStatus(1) != 2;
}

static void Assist_Freeze(int step, int left)
{
    if (assist_timer == event_vars->game_timer)
        return;
    assist_frozen = 1;
    assist_step = step;
    assist_wait = 0;
    assist_left = left;
    assist_timer = event_vars->game_timer;
}

// After the game frame: is the next one an input of the route?
static void Assist_Think(FighterData *fp, int sid)
{
    if (!Options_Ledge[LOPT_ASSIST].val)
    {
        Assist_Clear();
        return;
    }
    if (sid == ASID_CLIFFWAIT && route_show_num > 0)
    {
        FtCliffCatch *cliff = (void *)&fp->state_var;
        if (cliff->timer) // a drop works on the next frame
        {
            if (assist_hang >= 3)
                return;
            assist_hang++;
            Assist_Freeze(ASSIST_DROP, 4 - assist_hang);
        }
        return;
    }
    assist_hang = 0;
    if (!route_active || route_landed >= 0 || route_off)
        return;
    LedgeRoute *r = &route_cur;
    int next = route_e + 1;
    if (r->ff > 0 && next == Route_FF(r))
        Assist_Freeze(ASSIST_FF, 1);
    else if (next == r->dj)
        Assist_Freeze(ASSIST_JUMP, 1);
    else if (r->kind == LAND_AI && next >= r->press && next < r->press + r->press_w)
        Assist_Freeze(ASSIST_AERIAL, r->press + r->press_w - next);
}

// One axis of the stick's timers, as the game updates them each frame
// (Fighter_procInput): tilt (0x670), smash (0x673), any (0x676) and the
// activity count (0x679).
static void Timer_Axis(float cur, float prev, float dz, u8 *tilt, u8 *smash, u8 *any, u8 *activity)
{
    if (*any < 254)
        (*any)++;
    int same = (cur >= dz && prev >= dz) || (cur <= -dz && prev <= -dz);
    if (cur >= dz || cur <= -dz)
    {
        if (same)
        {
            if (*tilt < 254)
                (*tilt)++;
            if (*smash < 254)
                (*smash)++;
            if (*activity < 254)
                (*activity)++;
        }
        else
        {
            *any = 0;
            *smash = 0;
            *tilt = 0;
        }
    }
    else
    {
        *activity = 254;
        *smash = 254;
        *tilt = 254;
    }
}

static void Assist_Mirror(FighterData *fp, HSD_Pad *pad)
{
    float x = pad->fstickX, y = pad->fstickY;
    if (fabs(x) <= Common_Float(0x0))
        x = 0;
    if (fabs(y) <= Common_Float(0x4))
        y = 0;
    Vec2 prev = fp->input.lstick;
    fp->input.lstick_prev = prev;
    fp->input.lstick = (Vec2){x, y};
    u8 *t = (u8 *)&fp->input.timer_lstick_tilt_x;
    Timer_Axis(x, prev.X, Common_Float(0x8), &t[0], &t[3], &t[6], &t[9]);
    Timer_Axis(y, prev.Y, Common_Float(0xC), &t[1], &t[4], &t[7], &t[10]);
    Pad_Record(pad);
}

// Whether the pad now holds the input the route needs, as the game will
// read it when it goes on: a fastfall or tap jump only counts while the
// stick's flick is fresh (the game adds a frame to its timer first), so a
// slow roll down while frozen is no fastfall, just as at full speed.
static int Assist_Input(FighterData *fp, HSD_Pad *pad)
{
    float x = fp->input.lstick.X, y = fp->input.lstick.Y;
    float thresh = Common_Float(0x494);
    float facing = fp->facing_direction;
    int flick = (u8)fp->input.timer_lstick_tilt_y + 1;
    switch (assist_step)
    {
    case ASSIST_DROP:
        if (route_show[0].drop == DROP_DOWN)
            return y <= -thresh && fabs(x) < -y;
        return x * facing <= -thresh && y < thresh;
    case ASSIST_FF:
        return y <= -common_fastfall_stick && flick < common_fastfall_window;
    case ASSIST_JUMP:
        return (pad->down & (HSD_BUTTON_X | HSD_BUTTON_Y)) ||
               (y >= Common_Float(0x70) && flick < Common_Int(0x74)); // tap jump
    case ASSIST_AERIAL:
        return (pad->down & HSD_BUTTON_A) || fabs(pad->fsubstickX) >= common_aerial_stick_x ||
               fabs(pad->fsubstickY) >= common_aerial_stick_y;
    }
    return 1;
}

static const char *assist_names[] = {"", "drop", "fastfall", "jump", "aerial"};

// Every real frame, frozen or not.
static void Assist_Update(void)
{
    if (!assist_frozen)
        return;
    if (!Assist_On())
    {
        Assist_Clear();
        return;
    }
    FighterData *fp = Fighter_GetGObj(0)->userdata;
    HSD_Pad *pad = PadGetMaster(Advance_Port());
    Assist_Mirror(fp, pad);
    if (Assist_Input(fp, pad))
    {
        assist_frozen = 0;
        return;
    }
    int wait = wait_frames[Options_Ledge[LOPT_WAIT].val];
    sprintf(text_next, "Assist: %s  %.1f s", assist_names[assist_step], (wait - assist_wait) / 60.f);
    if (++assist_wait < wait)
        return;
    if (assist_left > 1)
    {
        // a frame further into the window
        assist_advance = 1;
        assist_wait = 0;
        assist_left--;
        assist_timer = -1;
        return;
    }
    // too late: start over
    sprintf(text_last, "Assist: no %s in time", assist_names[assist_step]);
    last_kind = -1;
    if (Options_Sounds[SOPT_LOST].val)
        SFX_PlayCommon(3);
    attempt_facing = fp->facing_direction > 0 ? 1 : -1;
    Drill_Finish(0);
    Drill_Reset();
}

///////////////////////
/// Slide-off       ///
///////////////////////

// Where a waveland or wavedash goes when its slide runs off the floor's
// end: friction along the floor to the edge, then falling. Before the
// press it assumes full stick that way; while sliding, the stick held now.
#define SLIDE_MAX 80
static Vec2 *slide_pos;     // [2][SLIDE_MAX], left side first
static float *slide_bottom; // [2][SLIDE_MAX]
static int slide_num[2];

static void Slide_Path(FighterData *fp, int side, float x, float y, float v, float edge, float stick_x, float stick_y)
{
    Vec2 *pos = &slide_pos[side * SLIDE_MAX];
    float *bottom = &slide_bottom[side * SLIDE_MAX];
    int n = 0;
    slide_num[side] = 0;
    pos[n] = (Vec2){x, y};
    bottom[n++] = 0;

    int off = 0;
    for (int i = 0; i < 120 && v != 0 && n < SLIDE_MAX; i++)
    {
        float friction = fp->attr.ground_friction;
        if (fabs(v) > fp->attr.walk_maximum_velocity)
            friction *= common_run_friction;
        if (fabs(v) <= friction)
            v = 0;
        else
            v += v > 0 ? -friction : friction;
        x += v;
        if ((v > 0 && x > edge) || (v < 0 && x < edge))
        {
            off = 1;
            break;
        }
        pos[n] = (Vec2){x, y};
        bottom[n++] = 0;
    }
    if (!off)
        return;

    // off the edge: falling with the slide's speed
    EcbSample *e = Ecb_Get(TS_FALL, 0);
    SimStart st;
    memset(&st, 0, sizeof(st));
    st.pos = (Vec2){edge, y};
    st.vel = (Vec2){v, 0};
    st.stick_x = stick_x;
    st.stick_y = stick_y;
    st.facing = fp->facing_direction;
    st.bottom = e->has_bottom ? e->bottom : 2.f;
    st.locked_bottom = st.bottom;
    st.ecb = (SimEcb){st.bottom, st.bottom + 16.f, st.bottom + 8.f, -3.f, 3.f};
    if (e->has_shape)
        Ecb_FromSample(e, st.facing, &st.ecb);
    Ecb_Fix(&st.ecb);
    st.ts = TS_FALL;
    st.frame = -1;
    st.tilt_timer = LL_TIMER_MAX;
    st.trigger_timer = LL_TIMER_MAX;
    st.skip_line = -1;
    SimState s;
    SimStep step;
    Sim_Init(&st, &s);
    pos[n] = (Vec2){edge, y};
    bottom[n++] = 0;
    for (int i = 0; i < 60 && n < SLIDE_MAX; i++)
    {
        Sim_Step(fp, &st, &s, -1, 0, &step);
        pos[n] = (Vec2){s.x, s.y};
        bottom[n++] = s.bottom;
        if (step.landed || s.y < y - 80.f)
            break;
    }
    slide_num[side] = n;
}

static void Slide_Update(FighterData *fp)
{
    slide_num[0] = 0;
    slide_num[1] = 0;
    if (!Options_Paths[POPT_SLIDEOFF].val || !Cues_Waveland())
        return;
    Cue *c = &cue_live[CUE_WL], *e = &cue_end[CUE_WL];
    float reach = Slide_Distance(fp, common_dodge_force);
    if ((c->phase == PH_COUNT || c->phase == PH_WINDOW) && !c->dim)
    {
        Floor_BuildCache();
        if ((c->wd || (c->dirs & DODGE_LEFT)) && c->spot.X - reach < c->x0)
            Slide_Path(fp, 0, c->spot.X, c->spot.Y, -common_dodge_force, c->x0, -1.f, 0);
        if ((c->wd || (c->dirs & DODGE_RIGHT)) && c->spot.X + reach > c->x1)
            Slide_Path(fp, 1, c->spot.X, c->spot.Y, common_dodge_force, c->x1, 1.f, 0);
    }
    else if (e->phase == PH_HIT && fp->phys.air_state == 0 && fp->state_id == ASID_LANDINGFALLSPECIAL)
    {
        float v = fp->phys.self_vel_ground.X;
        if (v != 0)
        {
            Floor_BuildCache();
            Slide_Path(fp, v > 0, fp->phys.pos.X, fp->phys.pos.Y, v, v > 0 ? e->x1 : e->x0, fp->input.lstick.X, fp->input.lstick.Y);
        }
    }
}

static void Draw_SlideOff(void)
{
    if (!Options_Paths[POPT_SLIDEOFF].val)
        return;
    GXColor c = Color_Fill(Cue_Color(CUE_WL), 0.85f);
    for (int side = 0; side < 2; side++)
    {
        if (slide_num[side] >= 2)
            Draw_Dashed(&slide_pos[side * SLIDE_MAX], &slide_bottom[side * SLIDE_MAX], 0, slide_num[side] - 1, c, 18);
    }
}

///////////////////////
/// Body flash      ///
///////////////////////

// On the first frame Falcon can act after a hit, he flashes the cue's
// color, which dust can't hide; after a ledge route he's tinted periwinkle
// while ledge intangibility lasts (blinking over its last frames).
static int flash_kind = -1;
static int flash_age = 100;
static int tint_on;

static void Body_Flash(FighterData *fp, int sid)
{
    static const float flash_a[4] = {0.75f, 0.55f, 0.35f, 0.18f};
    for (int i = 0; i < CUE_NUM; i++)
    {
        Cue *e = &cue_end[i];
        if (e->phase == PH_HIT && e->age == Cue_FlashAge(e))
        {
            flash_kind = i;
            flash_age = 0;
        }
    }
    if (route_active && route_landed >= 0)
    {
        LedgeRoute *r = &route_cur;
        if (Route_ActCell(r) - route_e - 1 == 0)
        {
            flash_kind = r->kind == LAND_AI ? CUE_AI : CUE_NIL;
            flash_age = 0;
        }
    }

    float a = 0;
    GXColor c = color_galint;
    int landing = sid == ASID_LANDING || sid == ASID_LANDINGFALLSPECIAL || (sid >= ASID_LANDINGAIRN && sid <= ASID_LANDINGAIRLW);
    if (Options_Cues[COPT_FLASH].val)
    {
        if (flash_age < 4 && flash_kind >= 0)
        {
            c = Cue_Color(flash_kind);
            a = flash_a[flash_age];
        }
        else if (galint_now > 0 && fp->phys.air_state == 0 && !landing)
            a = galint_now <= 3 && (event_vars->game_timer & 1) ? 0.2f : 0.45f;
    }
    if (flash_age < 100)
        flash_age++;
    a *= hide_all ? 0 : Kind_K(VG_CUES, flash_age < 4 ? flash_kind : -1);
    if (a > 0.9f)
        a = 0.9f;

    if (a > 0)
    {
        memset(&fp->color[1], 0, sizeof(ColorOverlay));
        c.a = 255 * a;
        fp->color[1].hex = c;
        fp->color[1].color_enable = 1;
        tint_on = 1;
    }
    else if (tint_on)
    {
        memset(&fp->color[1], 0, sizeof(ColorOverlay));
        tint_on = 0;
    }
}

///////////////////////
/// Panel text      ///
///////////////////////

// The panel's first line in the air: the next windows, like "AI NFD 2f,
// WL 1f", or the NIL, or else what the landing will be.
static void Text_Next(Prediction *p)
{
    static const char letters[] = "NFBUD";
    int ai = Cues_Ai() && p->ai_first && p->ai_first < p->uncertain_from;
    int wl = Cues_Waveland() && p->wl_first && p->wl_first < p->uncertain_from;
    char *t = text_next;
    next_kind = -1;
    *t = 0;
    if (ai)
    {
        t += sprintf(t, "AI ");
        for (int a = 0; a < 5; a++)
        {
            if (p->ai_aerials & (1 << a))
                *t++ = letters[a];
        }
        t += sprintf(t, " %df", p->ai_width);
        next_kind = CUE_AI;
    }
    if (wl)
    {
        t += sprintf(t, "%sWL %df", ai ? ", " : "", p->wl_width);
        if (next_kind < 0)
            next_kind = CUE_WL;
    }
    if (ai || wl)
        return;
    if (Cues_Nil() && p->land_frame && p->land_kind == LAND_NIL && p->uncertain_from > p->land_frame)
    {
        sprintf(text_next, "NIL");
        next_kind = CUE_NIL;
        return;
    }
    sprintf(text_next, "%s", text_ai[0] == 'L' ? text_ai : text_predict);
}

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
    int side = Options_Hud[HOPT_PANEL_SIDE].val;
    if (side != PANEL_AUTO)
    {
        panel_left = side == PANEL_LEFT;
        return;
    }

    // where Falcon is on screen: the camera turns as well as moves, so
    // comparing with its eye position isn't enough
    COBJ *cobj = View_CObj();
    Vec3 pos = {fp->phys.pos.X, fp->phys.pos.Y + body_offset, 0};
    Vec3 screen;
    HSD_GXProject(cobj, &pos, &screen, 1);
    float dx = screen.X - (cobj->viewport_left + cobj->viewport_right) / 2;
    if (dx > 24.f) // a little slack so it doesn't flicker near the middle
        panel_left = 1;
    else if (dx < -24.f)
        panel_left = 0;
}

// The timers count down to the next AI, perfect waveland and NIL windows.
// p starts lead frames from now (the rest of a jumpsquat, or 0 in the air).
// In the jumpsquat the waveland timer counts down to the wavedash instead.
static void Timing_Update(FighterData *fp, Prediction *p, int lead)
{
    int ai = Cues_Ai() && p->ai_first && p->ai_first < p->uncertain_from ? lead + p->ai_first : 0;
    int wl = Cues_Waveland() && p->wl_first && p->wl_first < p->uncertain_from ? lead + p->wl_first : 0;
    int nil = Cues_Nil() && p->land_frame && p->land_kind == LAND_NIL && p->uncertain_from > p->land_frame
                  ? lead + p->land_frame + 1 // the first frame Falcon can act
                  : 0;

    // each timer sits where its touchdown is: the floor under the frame
    // before it
    if (ai && ai <= LL_COUNT_FRAMES)
    {
        int k = p->ai_first;
        int t = k + Ai_FirstDelay(p, k, p->ai_show[k]);
        if (t > p->num)
            t = p->num;
        Cue_Set(CUE_AI, ai, p->ai_width, 0, 0, p->pos[t].X, p->pos[k - 1].Y + p->bottom[k - 1]);
        Cue_Body(CUE_AI, p, k - 1);
    }
    if (wl && wl <= LL_COUNT_FRAMES && !squat_wd)
    {
        int k = p->wl_first;
        Cue_Set(CUE_WL, wl, p->wl_width, p->wl_dirs, 0, p->pos[k].X, p->pos[k - 1].Y + p->bottom[k - 1]);
        Cue_Body(CUE_WL, p, k - 1);
    }
    if (nil && nil <= LL_COUNT_FRAMES)
    {
        int k = p->land_frame;
        Cue_Set(CUE_NIL, nil, 1, 0, 0, p->pos[k].X, p->pos[k - 1].Y + p->bottom[k - 1]);
        Cue_Body(CUE_NIL, p, k);
    }
}

static void Ground_Preview(FighterData *fp)
{
    int opt = Options_Paths[POPT_PREVIEW].val;
    SimStart start;

    preview_fh = opt == PREVIEW_BOTH || opt == PREVIEW_FULL;
    preview_sh = opt == PREVIEW_BOTH || opt == PREVIEW_SHORT;
    if (!preview_fh && !preview_sh && fp->state_id != ASID_KNEEBEND)
        return;

    // in the jumpsquat, count down to the hop's windows: a short hop once
    // the jump is let go (ftCo_KneeBend_Check_ShortHop sets the first state
    // variable), a full hop until then
    int squat = fp->state_id == ASID_KNEEBEND;
    int short_hop = squat && fp->state_var.state_var1;
    int need_fh = preview_fh || (squat && !short_hop);
    int need_sh = preview_sh || (squat && short_hop);
    int lead_fh = 0, lead_sh = 0;

    Floor_BuildCache();
    if (need_fh)
    {
        lead_fh = Sim_GroundJump(fp, 0, &start);
        Predict(fp, &start, pred_fh, BR_ALL);
    }
    if (need_sh)
    {
        lead_sh = Sim_GroundJump(fp, 1, &start);
        Predict(fp, &start, pred_sh, BR_ALL);
    }
    if (squat)
        Timing_Update(fp, short_hop ? pred_sh : pred_fh, short_hop ? lead_sh : lead_fh);
    if (preview_fh || preview_sh)
        Text_Preview();
}

void Event_Init(GOBJ *gobj)
{
    int init_tick = OSGetTick(); // the start-up work, timed for the log
    Cues_Clear();
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
    common_air_friction_oob = Common_Float(COMMON_AIR_FRICTION_OOB);
    common_upb_drift_stick = Common_Float(COMMON_UPB_DRIFT_STICK);
    common_drop_stick = fabs(Common_Float(COMMON_DROP_STICK));
    common_drop_window = Common_Frames(COMMON_DROP_WINDOW);
    common_spot_stick = -fabs(Common_Float(COMMON_SPOT_STICK));
    common_spot_window = Common_Frames(COMMON_SPOT_WINDOW);

    ecb_table = calloc(sizeof(EcbSample) * TS_COUNT * LL_STATE_FRAMES);
    upb_table = calloc(sizeof(UpbFrame) * 2 * LL_STATE_FRAMES);
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
    ceil_cache = calloc(sizeof(FloorLine) * LL_MAX_CEILS);
    quads = calloc(sizeof(Quad) * LL_QUADS);
    ledges = calloc(sizeof(LedgeEntry) * LR_LEDGES);
    pred_route = calloc(sizeof(Prediction));
    route_list = calloc(sizeof(LedgeRoute) * 2 * LR_ROUTES);
    route_build = calloc(sizeof(RouteList));
    route_path = calloc(sizeof(Vec2) * LR_PATH);
    route_path_bottom = calloc(sizeof(float) * LR_PATH);
    jt_cache = calloc(sizeof(SimState) * JT_D);
    jt_path = calloc(sizeof(Vec2) * JT_PATH);
    jt_path_bottom = calloc(sizeof(float) * JT_PATH);
    slide_pos = calloc(sizeof(Vec2) * 2 * SLIDE_MAX);
    slide_bottom = calloc(sizeof(float) * 2 * SLIDE_MAX);
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
    Presets_Init();
    GOBJ *script_gobj = GObj_Create(0, 7, 0);
    GObj_AddProc(script_gobj, Script_Think, 3);

    char buf[64];
    sprintf(buf, "LLPERF init %.2f ms\n", OSTicksToMicroseconds(OSGetTick() - init_tick) / 1000.f);
    Log(buf);
}

static void Event_ThinkFrame(GOBJ *event);

// The event's work each frame, timed for the log: the most it took over
// each second, all of it and the ledge route search in it.
static float perf_think, perf_solve;
static int perf_frames;

void Event_Think(GOBJ *event)
{
    int t0 = OSGetTick();
    Event_ThinkFrame(event);
    float ms = OSTicksToMicroseconds(OSGetTick() - t0) / 1000.f;
    if (ms > perf_think)
        perf_think = ms;
    if (++perf_frames >= 60)
    {
        if (Log_Level())
        {
            char buf[96];
            sprintf(buf, "LLPERF %d think %.2f ms solve %.2f ms quads %d\n", event_vars->game_timer, perf_think, perf_solve, quad_peak);
            Log(buf);
        }
        perf_frames = 0;
        perf_think = 0;
        perf_solve = 0;
        quad_peak = 0;
    }
}

static void Event_ThinkFrame(GOBJ *event)
{
    GOBJ *ft = Fighter_GetGObj(0);
    FighterData *fp = ft->userdata;
    if (card_probe > 0 && script_cur >= 0)
    {
        char buf[48];
        sprintf(buf, "LLPROBE think %d at %d\n", CARD_PROBE - card_probe, event_vars->game_timer);
        Log(buf);
    }

    if (!attributes_logged)
    {
        Log_Attributes(fp);
        attributes_logged = 1;
    }

    ai_show_all = Options_Cues[COPT_AI_FILTER].val == 1;
    ai_only = Options_Cues[COPT_AI_AERIAL].val ? AERIAL_BIT(TS_AIRN + Options_Cues[COPT_AI_AERIAL].val - 1) : 0;
    int logging = Log_Level() >= LOG_FRAMES;
    cue_log = Log_Level() >= LOG_LANDINGS;

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
    {
        const char *name = ts >= 0                     ? tracked_state_names[ts]
                           : sid == ASID_KNEEBEND      ? "Jumpsquat"
                           : sid == ASID_CLIFFWAIT     ? "Ledge"
                           : sid == ASID_LANDING       ? "Landing"
                           : sid == ASID_WAIT          ? "Standing"
                                                       : "State";
        sprintf(text_frame, "%s frame %d", name, frame_in_state + 1);
    }

    int tracked_air = ts >= 0 && airborne && !disturbed;
    if (sid == ASID_ESCAPEAIR && prev_state_id != sid)
    {
        dodge_start = event_vars->game_timer;
        dodge_late_ok = prev_wl_late;
    }

    // a wavedash's airdodge works on the takeoff frame itself (KneeBend goes
    // straight into the airdodge): in the jumpsquat, count down to it
    // (KneeBend takes off once its frame reaches the startup time). Its
    // window is the frame before, as for any press; on the takeoff frame a
    // jump with no airdodge yet is a frame late
    squat_wd = 0;
    if (Cues_Waveland() && !disturbed)
    {
        if (sid == ASID_KNEEBEND)
        {
            float left = fp->attr.jump_startup_time - fp->state.frame;
            int until = (int)left;
            if (until < left)
                until++;
            squat_wd = until < 1 ? 1 : until;
        }
        else if (airborne && prev_state_id == ASID_KNEEBEND)
            squat_wd = 1;
    }

    galint_now = sid == ASID_CLIFFWAIT ? 0 : fp->hurt.intang_frames.ledge;
    sprintf(text_next, "-");
    text_steps[0] = 0;
    next_kind = -1;

    Cues_Begin();

    // an airdodge on the takeoff frame or the one after is the wavedash,
    // whatever the jump's prediction made of it
    Cue *wdc = &cue_live[CUE_WL];
    if (wdc->wd && wdc->phase == PH_WINDOW && !wdc->held && !wdc->dim && prev_state_id != sid &&
        (sid == ASID_ESCAPEAIR || sid == ASID_LANDINGFALLSPECIAL))
    {
        wd_late = prev_state_id != ASID_KNEEBEND;
        Cue_Pressed(CUE_WL);
    }
    // landing on the takeoff frame shows no air frame at all
    int wd_landed = prev_state_id == ASID_KNEEBEND && sid == ASID_LANDINGFALLSPECIAL;
    if (wd_landed)
        seg_valid = 0;
    if ((prev_tracked_air || wd_landed) && !tracked_air)
        Landing_Resolve(fp);
    else
        Ghost_Update(sid);

    Panel_UpdateSide(fp);
    // once, from a moment of the idle animation that's always the same:
    // following it every frame would bob the paths with his breathing
    if (sid == ASID_WAIT && frame_in_state == 10 && !body_learned && fp->coll_data.ecbCurr_right.Y > 1.f)
    {
        body_offset = fp->coll_data.ecbCurr_right.Y;
        body_learned = 1;
    }
    preview_fh = 0;
    preview_sh = 0;

    if (tracked_air)
    {
        if (Lean_IsPlain(fp, ts, prev_vel.X))
            Ecb_Record(fp, ts, frame_in_state);
        if (Tracked_IsUpB(ts))
            Upb_Record(fp, ts, frame_in_state);
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
        start.dodge_late = ts == TS_ESCAPEAIR && frame_in_state == 0 && dodge_late_ok;
        Predict(fp, &start, pred_live, BR_ALL);
        live_timer = event_vars->game_timer;
        Text_Prediction(pred_live);
        Text_Windows(pred_live);
        Text_Next(pred_live);
        Timing_Update(fp, pred_live, 0);

        if (prev_tracked_air)
            Window_Feedback(fp, ts);
        Window_Remember(pred_live);
        if (!prev_tracked_air || (ts != prev_ts && !Tracked_EndedInto(prev_ts, ts)))
            press_note = 0; // a new jump or press: the old note is stale
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
        // in the air but in a state we don't predict (hitstun, side-B, ...)
        live_visible = 0;
        seg_valid = 0;
        sprintf(text_ai, "-");
    }
    else if (Ground_CanJump(sid) && !disturbed && !ghost_visible)
        Ground_Preview(fp);
    if (!tracked_air)
        Window_Forget();

    // nothing can be pressed from an airdodge or a helpless fall: a muted
    // waveland timer stops now, not at the touchdown
    if ((ts == TS_ESCAPEAIR || Tracked_IsHelpless(ts)) && cue_live[CUE_WL].dim)
        Cue_Finish(CUE_WL, PH_CUT, cue_live[CUE_WL].dim);

    if (squat_wd)
        Cue_Set(CUE_WL, squat_wd, 2, DODGE_RIGHT | DODGE_LEFT, 1, fp->phys.pos.X, fp->phys.pos.Y + 1.f);
    Cues_End();

    Ledge_Think(fp, sid);
    Drop_Think(fp, sid, prev_state_id);
    Assist_Think(fp, sid);
    Jump_Update(fp, ts, tracked_air);
    Body_Flash(fp, sid);
    Slide_Update(fp);
    Pad_Record(PadGetEngine(fp->pad_index));
    int t_solve = OSGetTick();
    Jump_Solve(fp);
    Jump_Publish(fp);
    Ledge_Solve(fp, tracked_air ? LR_BUDGET_AIR : LR_BUDGET);
    float solve_ms = OSTicksToMicroseconds(OSGetTick() - t_solve) / 1000.f;
    if (solve_ms > perf_solve)
        perf_solve = solve_ms;

    // the jumpsquat and takeoff, to check the ground previews against
    if (sid == ASID_KNEEBEND && logging)
        Log_Squat(fp);

    prev_state_id = sid;
    prev_ts = ts;
    prev_tracked_air = tracked_air;
    prev_tilt_timer = (u8)fp->input.timer_lstick_tilt_y;
    prev_lock = fp->coll_data.u.ecb_bot_lock_frames;
    prev_wl_late = live_visible && Cues_Waveland() && pred_live->wl_late && pred_live->wl_first == 1;
    prev_vel = (Vec2){fp->phys.self_vel.X, fp->phys.self_vel.Y};

    Drill_Think();
}

// Save and load position like the lab: hold D-pad right to save, press
// D-pad left to load. The event's own count of frames in Falcon's state
// goes with the save, since the predictor reads the state's ECB by it.
#define LL_SAVE_HOLD 10
static int save_hold;
static int saved_frame_in_state;

static void Position_Update(int held, int down)
{
    if (held & HSD_BUTTON_DPAD_RIGHT)
    {
        if (++save_hold == LL_SAVE_HOLD &&
            event_vars->Savestate_Save_v1(event_vars->savestate, 0))
            saved_frame_in_state = frame_in_state;
    }
    else
        save_hold = 0;

    if ((down & HSD_BUTTON_DPAD_LEFT) && event_vars->savestate->is_exist)
    {
        event_vars->Savestate_Load_v1(event_vars->savestate, 0);
        Script_ResetTracking();
        prev_state_id = ((FighterData *)Fighter_GetGObj(0)->userdata)->state_id;
        frame_in_state = saved_frame_in_state;
    }
}

// The quick toggles' steps: each list goes round, up or right forward.
typedef struct CueSet
{
    u8 ai, nil, wl;
    const char *name;
} CueSet;
static const CueSet cue_sets[] = {
    {1, 0, 1, "AI and waveland cues"},
    {1, 0, 0, "AI cues"},
    {0, 1, 0, "NIL cues"},
    {0, 0, 1, "Waveland cues"},
    {1, 1, 1, "All cues"},
    {0, 0, 0, "Cues off"},
};
// the paths in the order the toggle steps through them: body, landing, then
// both, first with the frame dots only in Frame Advance, then always
static const char *path_set_names[] = {"Paths off", "Body path", "Landing path", "Body and landing paths",
                                       "Body path with dots", "Landing path with dots", "Both paths with dots"};
static const u8 path_sets[][3] = {{0, 0, 0}, {0, 1, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 1}, {1, 0, 1}, {1, 1, 1}}; // landing, body, dots
static const char *pad_set_names[] = {"Controller off", "Ring, small", "Ring, medium", "Ring, large",
                                      "Crest, small", "Crest, medium", "Crest, large",
                                      "Classic, small", "Classic, medium", "Classic, large"};
static int chord_pad_place; // where the controller goes when it comes back on

static int Chord_Step(int i, int n, int dir)
{
    return i < 0 ? (dir > 0 ? 0 : n - 1) : (i + dir + n) % n;
}

static void Chord(int lr, int z, int down)
{
    int dir = down & (HSD_BUTTON_DPAD_UP | HSD_BUTTON_DPAD_RIGHT) ? 1 : down & (HSD_BUTTON_DPAD_DOWN | HSD_BUTTON_DPAD_LEFT) ? -1 : 0;
    if (!dir)
        return;
    int vert = (down & (HSD_BUTTON_DPAD_UP | HSD_BUTTON_DPAD_DOWN)) != 0;
    char buf[40];
    if (lr && z && vert)
    {
        // everything the event draws, hidden or back
        hide_all ^= 1;
        Toast(hide_all ? "Landing Lab hidden" : "Landing Lab shown");
    }
    else if (lr && z)
    {
        // the next preset that has settings in it
        int cur = Options_Presets[PROPT_PICK].val;
        for (int n = 0; n < PS_COUNT; n++)
        {
            cur = Chord_Step(cur, PS_COUNT, dir);
            if (Preset_Slot(cur)->used)
                break;
        }
        Options_Presets[PROPT_PICK].val = cur;
        Preset_Apply(Preset_Slot(cur));
        Preset_Describe(cur);
        sprintf(buf, "Preset: %s", preset_names[cur]);
        Toast(buf);
    }
    else if (lr && vert)
    {
        // the cue set
        int wl = Options_Cues[COPT_WL].val, cur = -1;
        for (int i = 0; i < (int)countof(cue_sets); i++)
            if (cue_sets[i].ai == Options_Cues[COPT_AI].val && cue_sets[i].nil == Options_Cues[COPT_NIL].val && cue_sets[i].wl == (wl != 0))
                cur = i;
        const CueSet *c = &cue_sets[Chord_Step(cur, countof(cue_sets), dir)];
        Options_Cues[COPT_AI].val = c->ai;
        Options_Cues[COPT_NIL].val = c->nil;
        Options_Cues[COPT_WL].val = c->wl ? (wl ? wl : 2) : 0;
        Toast(c->name);
    }
    else if (lr)
    {
        // the landing and body paths, and the frame dots on them
        int dots = Options_Paths[POPT_TICKS].val == 1, cur = -1;
        for (int i = 0; i < (int)countof(path_sets); i++)
            if (path_sets[i][0] == Options_Paths[POPT_PATH].val && path_sets[i][1] == Options_Paths[POPT_BODY].val &&
                (path_sets[i][2] == dots || i == 0))
                cur = i;
        int next = Chord_Step(cur, countof(path_sets), dir);
        Options_Paths[POPT_PATH].val = path_sets[next][0];
        Options_Paths[POPT_BODY].val = path_sets[next][1];
        if (next != 0)
            Options_Paths[POPT_TICKS].val = path_sets[next][2] ? 1 : 0;
        Toast(path_set_names[next]);
    }
    else if (vert)
    {
        // the controller: off, then each look at each size
        int place = Options_Hud[HOPT_STICK].val;
        int cur = place == 3 ? 0 : 1 + 3 * Options_Hud[HOPT_LOOK].val + Options_Hud[HOPT_PAD_SIZE].val;
        if (place != 3)
            chord_pad_place = place;
        int next = Chord_Step(cur, countof(pad_set_names), dir);
        if (next == 0)
            Options_Hud[HOPT_STICK].val = 3;
        else
        {
            Options_Hud[HOPT_STICK].val = chord_pad_place;
            Options_Hud[HOPT_LOOK].val = (next - 1) / 3;
            Options_Hud[HOPT_PAD_SIZE].val = (next - 1) % 3;
        }
        Toast(pad_set_names[next]);
    }
    else
    {
        // how strongly everything shows
        EventOption *o = &Options_Cues[COPT_INTENSITY];
        o->val = o->val + dir < 0 ? 0 : o->val + dir >= o->value_num ? o->value_num - 1 : o->val + dir;
        sprintf(buf, "Intensity %s", intensity_names[o->val]);
        Toast(buf);
    }
    SFX_PlayCommon(2);
}

void Event_Update(void)
{
    if (Pause_CheckStatus(1) != 2)
        HSD_SetSpeedEasy(speed_values[Options_Game[GOPT_SPEED].val]);
    else
        HSD_SetSpeedEasy(1.0);

    // runs every frame, frozen or not: Assist waits for its input, D-pad
    // down toggles frame advance, D-pad left plays the chosen script again,
    // or else the D-pad saves and loads position (or puts Falcon back on
    // the ledge, when Reset starts from one)
    Presets_Update();
    if (toast_timer > 0)
        toast_timer--;
    Rumble_Update(Fighter_GetGObj(0)->userdata,
                  Pause_CheckStatus(1) == 2 || Options_Game[GOPT_FRAME_ADV].val || assist_frozen);
    // while a script runs, a line in the log every 5 seconds, frozen or not:
    // whoever watches the log tells a hang (no lines) from a shot waiting,
    // and which shot, should its own line have gone missing
    static int script_beat;
    if (script_cur >= 0 && ++script_beat >= 300)
    {
        script_beat = 0;
        char buf[128];
        if (Options_Game[GOPT_FRAME_ADV].val)
            sprintf(buf, "LLBEAT %d frozen at shot \"%s\", waiting for D-pad down\n", event_vars->game_timer, shot_label);
        else
            sprintf(buf, "LLBEAT %d running\n", event_vars->game_timer);
        Log(buf);
    }
    if (card_probe > 0)
    {
        if (script_cur >= 0)
        {
            char buf[48];
            sprintf(buf, "LLPROBE update %d at %d\n", CARD_PROBE - card_probe, event_vars->game_timer);
            Log(buf);
        }
        card_probe--;
    }
    if (namer.on)
    {
        if (namer.leave_menu)
        {
            namer.leave_menu = 0;
            if (Pause_CheckStatus(1) == 2)
                event_vars->Menu_Exit(event_vars->menu_gobj);
            return;
        }
        Namer_Think();
        return;
    }
    if (Pause_CheckStatus(1) == 2)
        return;
    Assist_Update();
    HSD_Pad *pad = PadGetMaster(Advance_Port());
    int down = pad->down;
    // quick toggles: L or R clicked all the way (a light press doesn't
    // count, so the D-pad while shielding does nothing) or Z held, with the
    // D-pad, which does nothing else meanwhile
    int lr = (pad->held & (HSD_TRIGGER_L | HSD_TRIGGER_R)) != 0;
    int z = (pad->held & HSD_TRIGGER_Z) != 0;
    if (lr || z)
    {
        Chord(lr, z, down);
        save_hold = 0;
        return;
    }
    if (down & HSD_BUTTON_DPAD_DOWN)
        Options_Game[GOPT_FRAME_ADV].val ^= 1;
    // a clean frame for mockups, the same frame as the one with cues. Only
    // for the Developer's captures, and never left on past Frame Advance:
    // pressed by chance, everything the event draws would stay hidden.
    if ((down & HSD_BUTTON_DPAD_UP) && Options_Game[GOPT_FRAME_ADV].val && Log_Level())
        capture_clean ^= 1;
    else if ((down & HSD_BUTTON_DPAD_UP) && hang_ledge >= 0 && route_list_num > 1 && Routes_On())
    {
        // hanging: the next route in the list, back to the first after the last
        EventOption *o = &Options_Ledge[LOPT_PICK];
        o->val = o->val >= route_list_num ? 1 : o->val + 1;
        Route_RememberPick();
        Route_MenuText();
        SFX_PlayCommon(2);
    }
    if (!Options_Game[GOPT_FRAME_ADV].val)
        capture_clean = 0;
    if ((down & HSD_BUTTON_DPAD_LEFT) && Options_Dev[DOPT_SCRIPT].val)
    {
        script_cur = -1;
        script_pending = 1;
    }
    else if ((down & HSD_BUTTON_DPAD_LEFT) && Reset_Mode() != RESET_NONE &&
             Options_Ledge[LOPT_START].val == START_LEDGE)
        Drill_Reset();
    else if (script_cur < 0)
        Position_Update(pad->held, down);
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

static void Presets_KeepUser(void);
static void Card_Flush(void);

void Event_Exit(GOBJ *menu)
{
    // leaving from the menu: it never closes, so keep the settings now
    Presets_KeepUser();
    Card_Flush();
    Rumble_Release();
    stc_match->state = 3;
    Match_EndVS();
}

