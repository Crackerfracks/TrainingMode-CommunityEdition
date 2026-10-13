/* landinglab_sim.h
 *
 * Landing Lab's simulation: tracked states, the learned and baked ECB
 * tables, stage floors and walls, and the prediction itself. Only
 * landinglab.c includes it (hmex builds an event from one file); it is
 * apart so tools/llsim can build the same code on a PC.
 */

#define LL_SIM_FRAMES 240   // how far ahead to simulate
#define LL_STATE_FRAMES 128 // frames learned per state
#define LL_FALL_LOOP 8       // Falcon's Fall and FallAerial animations loop every 8 frames (fall_loop: another fighter's)
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
#define COMMON_RUN_STICK 0x58       // float: stick x * facing >= this turns a dash into a run
#define COMMON_RUN_TAPER 0x5C       // float: how much a run's speed-up eases off toward its top speed
#define COMMON_DASHRUN_FRICTION 0x60 // float: friction multiplier while a dash or run turns or slows
#define COMMON_TAP_JUMP_STICK 0x70  // float: stick y >= this jumps (tap_jump_threshold)
#define COMMON_JUMP_BACK_STICK 0x78 // float: stick x * facing <= -this jumps backwards
#define COMMON_DODGE_DEADZONE 0x32C // Vec2: stick inside both is a neutral airdodge
#define COMMON_DODGE_FORCE 0x338    // float: airdodge starting speed
#define COMMON_DODGE_DECAY 0x33C    // float: airdodge speed multiplier per frame
#define COMMON_WAVELAND_LAG 0x344   // float: landing lag out of an airdodge
#define COMMON_FALL_LEAN_DEADZONE 0x444 // float: |x speed / drift max| above this leans the fall pose
#define COMMON_FALL_LEAN_RATE 0x448     // float: how fast the lean moves to its target each frame
#define COMMON_AIR_FRICTION_OOB 0x1FC   // float: air friction while faster than the drift max
#define COMMON_UPB_DRIFT_STICK 0x258    // float: |stick x| below this stops a Falcon Dive's drift (and an air jump's of Kirby's)
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
    TS_MJ1,         // Kirby's and Jigglypuff's air jumps, 1 to 5 (their own states)
    TS_MJ2,
    TS_MJ3,
    TS_MJ4,
    TS_MJ5,

    TS_COUNT
};

#define ASID_CAPTAIN_SPECIALHI 353    // Falcon's own states start at 341
#define ASID_CAPTAIN_SPECIALAIRHI 354
#define ASID_MULTIJUMP1 341 // Kirby's and Jigglypuff's JumpAerialF1 (ftKb_MS_, ftPr_MS_), F2 .. F5 after it

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
    ASID_MULTIJUMP1,
    ASID_MULTIJUMP1 + 1,
    ASID_MULTIJUMP1 + 2,
    ASID_MULTIJUMP1 + 3,
    ASID_MULTIJUMP1 + 4,
};

static const char *tracked_state_names[TS_COUNT] = {
    "JumpF", "JumpB", "DJumpF", "DJumpB", "Fall", "FallAerial",
    "Nair", "Fair", "Bair", "Uair", "Dair", "Airdodge",
    "FallSpecial", "UpB", "UpBAir", "AirJump1", "AirJump2", "AirJump3", "AirJump4", "AirJump5",
};

// The panel's name for a state the event doesn't track, or 0 for its number.
typedef struct StateName
{
    short sid;
    const char *name;
} StateName;
static const StateName state_names[] = {
    {ASID_WAIT, "Standing"},       {ASID_WALKSLOW, "Walk"},         {ASID_WALKMIDDLE, "Walk"},
    {ASID_WALKFAST, "Walk"},       {ASID_TURN, "Turn"},             {ASID_TURNRUN, "Run turn"},
    {ASID_DASH, "Dash"},           {ASID_RUN, "Run"},               {ASID_RUNDIRECT, "Run"},
    {ASID_RUNBRAKE, "Run brake"},  {ASID_KNEEBEND, "Jumpsquat"},    {ASID_FALLSPECIAL, "Helpless"},
    {ASID_FALLSPECIALF, "Helpless"}, {ASID_FALLSPECIALB, "Helpless"}, {ASID_DAMAGEFALL, "Tumble"},
    {ASID_SQUAT, "Crouch"},        {ASID_SQUATWAIT, "Crouch"},      {ASID_SQUATRV, "Stand up"},
    {ASID_LANDING, "Landing"},     {ASID_LANDINGFALLSPECIAL, "Special landing"},
    {ASID_LANDINGAIRN, "Nair lag"}, {ASID_LANDINGAIRF, "Fair lag"}, {ASID_LANDINGAIRB, "Bair lag"},
    {ASID_LANDINGAIRHI, "Uair lag"}, {ASID_LANDINGAIRLW, "Dair lag"}, {ASID_GUARDON, "Shield on"},
    {ASID_GUARD, "Shield"},        {ASID_GUARDOFF, "Shield off"},   {ASID_GUARDSETOFF, "Shield stun"},
    {ASID_GUARDREFLECT, "Powershield"}, {ASID_PASS, "Platform drop"}, {ASID_OTTOTTO, "Teeter"},
    {ASID_OTTOTTOWAIT, "Teeter"},  {ASID_ESCAPE, "Spotdodge"},     {ASID_ESCAPEF, "Roll"},
    {ASID_ESCAPEB, "Roll"},        {ASID_ESCAPEAIR, "Airdodge"},    {ASID_CLIFFCATCH, "Ledge grab"},
    {ASID_CLIFFWAIT, "Ledge"},     {ASID_CLIFFCLIMBQUICK, "Getup"}, {ASID_CLIFFCLIMBSLOW, "Getup"},
    {ASID_CLIFFATTACKQUICK, "Ledge attack"}, {ASID_CLIFFATTACKSLOW, "Ledge attack"},
    {ASID_CLIFFESCAPEQUICK, "Ledge roll"}, {ASID_CLIFFESCAPESLOW, "Ledge roll"},
    {ASID_CLIFFJUMPQUICK1, "Ledge jump"}, {ASID_CLIFFJUMPQUICK2, "Ledge jump"},
    {ASID_CLIFFJUMPSLOW1, "Ledge jump"}, {ASID_CLIFFJUMPSLOW2, "Ledge jump"},
    {ASID_ATTACKAIRN, "Nair"},     {ASID_ATTACKAIRF, "Fair"},       {ASID_ATTACKAIRB, "Bair"},
    {ASID_ATTACKAIRHI, "Uair"},    {ASID_ATTACKAIRLW, "Dair"},      {ASID_ATTACK11, "Jab"},
    {ASID_ATTACKDASH, "Dash attack"}, {ASID_DOWNWAITU, "Down"},     {ASID_DOWNWAITD, "Down"},
    {ASID_DOWNBOUNDU, "Missed tech"}, {ASID_DOWNBOUNDD, "Missed tech"}, {ASID_PASSIVE, "Tech"},
    {ASID_REBIRTHWAIT, "Platform"},
};

// Is it Mr. Game & Watch? His nair, bair and uair are his own states
// (ftGw_AttackAirN_DecideAction; fair and dair are the common ones), tracked
// in the aerial slots, and they land in his own landing states.
static int sim_gw;

#define ASID_GW_ATTACKAIRN 347  // ftGw_MS_AttackAirN, then AttackAirB, AttackAirHi
#define ASID_GW_LANDINGAIRN 350 // ftGw_MS_LandingAirN, then LandingAirB, LandingAirHi
static const s8 gw_aerials[3] = {TS_AIRN, TS_AIRB, TS_AIRHI};
static const char *gw_landing_names[3] = {"Nair lag", "Bair lag", "Uair lag"};

// Which of his three (from first, the attack's or the landing's), or -1.
static int Gw_Own(int sid, int first)
{
    return sim_gw && sid >= first && sid < first + 3 ? sid - first : -1;
}

// An aerial's state: the common ones, or his own.
static int Is_AerialState(int sid)
{
    return (sid >= ASID_ATTACKAIRN && sid <= ASID_ATTACKAIRLW) || Gw_Own(sid, ASID_GW_ATTACKAIRN) >= 0;
}

// An aerial's landing lag (ftCo_LandingAir_EnterWithMsidLag): the common
// states, or his own.
static int Is_LandingAirState(int sid)
{
    return (sid >= ASID_LANDINGAIRN && sid <= ASID_LANDINGAIRLW) || Gw_Own(sid, ASID_GW_LANDINGAIRN) >= 0;
}

static const char *State_Name(int sid)
{
    int gw = Gw_Own(sid, ASID_GW_LANDINGAIRN);
    if (gw >= 0)
        return gw_landing_names[gw];
    for (int i = 0; i < (int)countof(state_names); i++)
        if (state_names[i].sid == sid)
            return state_names[i].name;
    return 0;
}


static int Tracked_IsUpB(int ts)
{
    return ts == TS_UPB || ts == TS_UPBA;
}

// Is the fighter Falcon? Another fighter's own states (341 on) aren't
// Falcon Dive, and its table isn't his (Sim_ForFighter).
static int sim_falcon = 1;

// Is it Kirby or Jigglypuff, with their five air jumps (fp->multi_jump_desc,
// their special attributes)? Only theirs are 341 .. 345.
static int sim_mj;

static int Is_MultiJump(int ts)
{
    return ts >= TS_MJ1 && ts <= TS_MJ5;
}

static int Tracked_Index(int state_id)
{
    if (state_id == ASID_FALLSPECIALF || state_id == ASID_FALLSPECIALB)
        return TS_FALLSPECIAL;
    int gw = Gw_Own(state_id, ASID_GW_ATTACKAIRN);
    if (gw >= 0)
        return gw_aerials[gw];
    for (int i = 0; i < TS_COUNT; i++)
    {
        if (tracked_state_ids[i] == state_id)
            return (Tracked_IsUpB(i) && !sim_falcon) || (Is_MultiJump(i) && !sim_mj) ? -1 : i;
    }
    return -1;
}

static int Tracked_IsAerial(int ts)
{
    return ts >= TS_AIRN && ts <= TS_AIRLW;
}

// Nothing can be pressed out of a Falcon Dive or the helpless fall after
// it: no aerial, airdodge or jump.
static int Tracked_IsHelpless(int ts)
{
    return ts == TS_FALLSPECIAL || Tracked_IsUpB(ts);
}

// Jump, double jump, fall and helpless fall collision pass the drop-through
// callback (ftCo_80096CC8); aerials, the airdodge (ft_80081D0C), Falcon
// Dive and Kirby's and Jigglypuff's air jumps (ft_CheckGroundAndLedge,
// ft_80082F28) collide without it and always land on platforms.
static int Tracked_UsesPlatformDrop(int ts)
{
    return ts <= TS_FALLAERIAL || ts == TS_FALLSPECIAL;
}

// The state the game enters when this one's animation runs out, or -1 if it
// loops or isn't tracked. Jump and aerials go to Fall (ftCo_Fall_Enter),
// double jump goes to FallAerial (ftCo_FallAerial_Enter), the airdodge and
// Falcon Dive go to FallSpecial (ftCo_80096900). Kirby's and Jigglypuff's
// air jumps go to Fall with jumps left, FallAerial after the last
// (ftCo_JumpAerialF1_Anim; max_jumps 6: jump 5). Once a state has been seen
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
    case TS_MJ1:
    case TS_MJ2:
    case TS_MJ3:
    case TS_MJ4:
        return TS_FALL;
    case TS_JUMPAERIALF:
    case TS_JUMPAERIALB:
    case TS_MJ5:
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

// A pose's ECB, for a fall's blend-in (fall_blend).
typedef struct PoseEcb
{
    float bottom, top, side_y, front, back;
    u8 has_bottom;
} PoseEcb;

static void Pose_From(const EcbSample *e, PoseEcb *p)
{
    p->bottom = e->bottom;
    p->top = e->top;
    p->side_y = e->side_y;
    p->front = e->front;
    p->back = e->back;
    p->has_bottom = e->has_bottom;
}

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
static void Dj_Clear(void);

static void Learned_Clear(void)
{
    Upb_Clear();
    Dj_Clear();
    memset(ecb_table, 0, sizeof(EcbSample) * TS_COUNT * LL_STATE_FRAMES);
    memset(state_len, 0, sizeof(state_len));
    memset(state_next, -1, sizeof(state_next));
}

// How many frames each fall's animation takes to loop (Fall, FallAerial,
// FallSpecial): Falcon's are 8, another fighter's its own (Fall_Learn,
// or its packed table).
static u8 fall_loop[3] = {LL_FALL_LOOP, LL_FALL_LOOP, LL_FALL_LOOP};

// How many frames Fall's and FallAerial's animations move on a frame after
// one that leant (lean > 0): 2 for Peach and DK, 1 for the rest (their
// LLFALLANIM lines). Unleaning, they move 1. From the packed table.
static u8 fall_step = 1;

// How many frames Fall and FallAerial blend in from the pose before them
// (state.blend; LLFALLANIM's last field): 10 for Peach, 16 for DK, 0 for
// the rest. Frame k of the fall eases the pose so far toward the fall's own
// by 1/(blend - k) (fit to the bf9 captures). From the packed table.
static u8 fall_blend = 0;

static int Fall_Index(int ts)
{
    return ts == TS_FALL ? 0 : ts == TS_FALLAERIAL ? 1 : ts == TS_FALLSPECIAL ? 2 : -1;
}

static int Is_Fall(int ts)
{
    return ts == TS_FALL || ts == TS_FALLAERIAL;
}

// The live fall's animation frame in its loop: where its pose is, which
// is the frame in the state only while it moves 1 a frame (fall_step).
static int Fall_Anim(FighterData *fp, int ts)
{
    int anim = (int)(fp->state.frame + 0.5f);
    return anim < 0 ? 0 : anim % fall_loop[Fall_Index(ts)];
}

// Where a frame is learned.
static EcbSample *Ecb_Slot(int ts, int frame)
{
    int fi = Fall_Index(ts);
    if (fi >= 0)
        frame %= fall_loop[fi]; // the fall animations loop
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
        // Game & Watch's takes his bair's (ftGw_LandingAirHi_Init)
        return sim_gw ? fp->attr.b_air_landing_lag : fp->attr.u_air_landing_lag;
    case TS_AIRLW:
        return fp->attr.d_air_landing_lag;
    default:
        return fp->attr.normal_landing_lag;
    }
}

// Does an L-cancel halve this aerial's lag (ftCo_LandingAir_EnterWithLag)?
// Game & Watch's own nair, bair and uair land straight into their lag
// (ftGw_LandingAirN_Enter and the others: ftCo_LandingAir_EnterWithMsidLag
// with the full lag), so not theirs.
static int Aerial_LCancels(int ts)
{
    return !(sim_gw && (ts == TS_AIRN || ts == TS_AIRB || ts == TS_AIRHI));
}

// The ECB this frame into s (the capture's character states use it too).
static void Ecb_Fill(FighterData *fp, EcbSample *s)
{
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

// Another fighter's fall loops as long as its animation (from its first
// frame in each, before anything's learned there).
static void Fall_Learn(FighterData *fp, int ts)
{
    static u8 logged[3];
    int fi = Fall_Index(ts);
    if (fi < 0 || !fp->figatree_curr)
        return;
    int n = (int)(fp->figatree_curr->frame_num + 0.5f);
    if (!logged[fi])
        OSReport("LLFALL %d anim %d\n", ts, n); // Falcon's should say 8
    logged[fi] = 1;
    if (!sim_falcon)
        fall_loop[fi] = n < 1 ? 1 : n > LL_STATE_FRAMES ? LL_STATE_FRAMES : n;
}

// A fall is learned at its animation frame (Fall_Anim).
static void Ecb_Record(FighterData *fp, int ts, int frame)
{
    if (frame >= LL_STATE_FRAMES)
        return;
    if (Is_Fall(ts) && frame < fall_blend)
        return; // still blending in from the pose before: not the fall's own
    Ecb_Fill(fp, Ecb_Slot(ts, Is_Fall(ts) ? Fall_Anim(fp, ts) : frame));
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

// The double-jump-cancel fighters (Ness, Peach, Yoshi, Mewtwo): their
// double jump starts with no vertical speed, and its animation sets their
// vertical speed frame by frame, with no gravity (ft_800851D0, the
// ftCo_JumpAerial_Phys_Cb they get). An aerial or airdodge cuts it short
// with the speed of the frame before, then gravity as usual. Ness's
// sideways speed is the animation's too, plus a drift speed of its own
// that starts at the jump's (ftNs_JumpAerial_Phys_Cb). Yoshi has no
// backward double jump: held back, his forward one turns him around,
// switching sides halfway through the turn (ftYs_JumpAerial_Enter,
// ft_800CB6EC). The movement comes from the fighter's packed table.
#define DJC_NONE 0
#define DJC_PLAIN 1 // Peach, Mewtwo
#define DJC_NESS 2
#define DJC_YOSHI 3
#define YS_TURN_FRAMES 0x0 // int: the turn's length (ftYoshiAttributes x0)
#define YS_TURN_STICK 0x4  // float: stick x back past this turns him

typedef struct DjMove
{
    float x, y; // the animation's speed, as if facing right
    u8 seen;
} DjMove;

static DjMove *dj_move; // [2][LL_STATE_FRAMES], forward then backward
static int sim_djc;

static int Djc_Kind(int kind)
{
    switch (kind)
    {
    case FTKIND_NESS:
        return DJC_NESS;
    case FTKIND_YOSHI:
        return DJC_YOSHI;
    case FTKIND_PEACH:
    case FTKIND_MEWTWO:
        return DJC_PLAIN;
    }
    return DJC_NONE;
}

static int Is_DoubleJump(int ts)
{
    return ts == TS_JUMPAERIALF || ts == TS_JUMPAERIALB;
}

static DjMove *Dj_Get(int ts, int frame)
{
    if (frame >= LL_STATE_FRAMES)
        frame = LL_STATE_FRAMES - 1;
    return &dj_move[(ts - TS_JUMPAERIALF) * LL_STATE_FRAMES + frame];
}

// Whether the double jump moves by its animation here: a double-jump-cancel
// fighter whose movement is known (without it, the usual physics stand in).
static int Dj_Animated(int ts)
{
    return sim_djc && Is_DoubleJump(ts) && Dj_Get(ts, 0)->seen;
}

static void Dj_Clear(void)
{
    memset(dj_move, 0, sizeof(DjMove) * 2 * LL_STATE_FRAMES);
}

// Yoshi's turn, counted down each frame of the jump; he switches sides
// when it's halfway done. The jump's first frame counts too. Kirby's and
// Jigglypuff's air jumps turn the same way (ft_800CB6EC), their turn frames
// first in their attributes too (Fighter_x2D0_t x0).
static void Dj_Turn(FighterData *fp, int *turn, float *face)
{
    if (*turn <= 0)
        return;
    int frames = *(int *)((u8 *)fp->special_attributes + YS_TURN_FRAMES);
    (*turn)--;
    if (*turn == frames / 2)
        *face = -*face;
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
    if (id < 0)
        return 0; // the hop library's endless floor
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
    int anim;            // a fall's animation frame in its loop (Fall_Anim)
    PoseEcb pose;        // the pose's ECB on the starting frame (a fall's blend-in)
    int pose_ok;         // pose is known
    Vec2 drift;          // Falcon Dive's own drift speed (mv.ca.specialhi.vel)
    float special_lag;   // the helpless fall's landing lag (mv.co.fallspecial.landing_lag)
    int dodge_late;      // an airdodge's first frame, pressed in a late window
    int dj_turn;         // Yoshi's double jump: turn frames left (mv.co.jumpaerial.x0), Kirby's and Jigglypuff's air jumps too
    int jumps;           // jumps used (fp->jump.jumps_used): which of Kirby's and Jigglypuff's air jumps is next
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
    int anim;        // a fall's animation frame in its loop: the pose
    PoseEcb pose[2]; // the pose's ECB on the last two frames (fall_blend)
    int pose_ok;     // how many of them are known
    PoseEcb blend;   // a fall's pose while it blends in
    float locked_bottom; // bottom kept while the lock lasts
    float face;          // facing: Falcon Dive can turn him around
    Vec2 drift;          // Falcon Dive's drift speed (x: Ness's double jump's)
    float special_lag;   // landing lag once helpless
    int dodge_late;      // landing on the airdodge's second frame is perfect too
    int dj_turn;         // Yoshi's double jump: turn frames left
    int jumps;           // jumps used
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
    EcbSample blend_ecb;  // ... when a fall blends in from the pose before
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
static float common_run_stick;
static float common_run_taper;
static float common_dashrun_friction;
static float common_tap_jump_stick;
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

// ftCommon_CalcSelfAccel_AccelToVelClampedFrom: accel toward target, air
// friction past it, never past the air speed max
static float Drift_AccelTo(FighterData *fp, float vel, float accel, float target)
{
    float friction = fp->attr.aerial_friction;
    float max_vel = fp->attr.horizontal_air_mobility_constant;

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

// ftCommon_CalcSelfAccel_DriftFrom with the stick held at stick_x
static float Drift_Accel(FighterData *fp, float vel, float stick_x)
{
    float target = stick_x * fp->attr.aerial_drift_max;
    float accel = stick_x * fp->attr.aerial_drift_stick_mult;
    if (stick_x > 0)
        accel += fp->attr.aerial_drift_base;
    else
        accel -= fp->attr.aerial_drift_base;
    return Drift_AccelTo(fp, vel, accel, target);
}

// Kirby's and Jigglypuff's air jumps drift by the stick alone, no base
// accel, both scaled by their own multipliers, and not at all near the
// middle (ftCo_JumpAerialF1_Phys, ft_80084E1C)
static float Mj_DriftAccel(FighterData *fp, float vel, float stick_x)
{
    FtMultiJumpDesc *mj = fp->multi_jump_desc;
    if (fabs(stick_x) < common_upb_drift_stick)
        return Drift_AccelTo(fp, vel, 0, 0);
    return Drift_AccelTo(fp, vel, stick_x * (fp->attr.aerial_drift_stick_mult * mj->aerial_drift_stick_mult_mult),
                         stick_x * (fp->attr.aerial_drift_max * mj->aerial_drift_max_mult));
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

// The steady grid in use: Falcon's above, or another fighter's own from
// TM/llecb.bin (lean_own, as many blends but a frame per frame of its own
// fall's loop). Each fall and direction's rows run blend by blend, a row
// per loop frame in each.
#define LL_LEAN_MAX 2048
static const LeanEcb *lean_rows = lean_ecb;
static int lean_count = countof(lean_ecb);
static LeanEcb *lean_own; // [LL_LEAN_MAX]

static int lean_first[2][2]; // first row of each fall and direction
static int lean_levels[2][2]; // blends measured for it
static int onset_first[2][2];
static int onset_levels[2][2];

static void Lean_Index(void)
{
    memset(lean_levels, 0, sizeof(lean_levels));
    memset(onset_levels, 0, sizeof(onset_levels));
    for (int i = lean_count - 1; i >= 0; i--)
    {
        const LeanEcb *l = &lean_rows[i];
        lean_first[l->fall][l->back] = i;
        if (l->k == 0)
            lean_levels[l->fall][l->back]++;
    }
    if (lean_rows != lean_ecb)
        return; // the first frame's own measurements are Falcon's only
    for (int i = (int)countof(lean_onset) - 1; i >= 0; i--)
    {
        const LeanEcb *l = &lean_onset[i];
        onset_first[l->fall][l->back] = i;
        onset_levels[l->fall][l->back]++;
    }
}

// No leans at all: falls keep their plain pose.
static void Lean_Drop(void)
{
    lean_count = 0;
    Lean_Index();
}

// The fighter's own packed table, kept to start over from (Learned_Reset).
static const u8 *own_table;
static int own_size;

static int Ecb_Unpack(const u8 *b, int size);

// What's known before anything's learned: Falcon's baked table, another
// fighter's packed one, or nothing.
static void Learned_Reset(void)
{
    if (sim_falcon)
    {
        Learned_Clear();
        Learned_Bake();
    }
    else if (!own_table || !Ecb_Unpack(own_table, own_size))
        Learned_Clear();
}

// Another fighter than Falcon: none of his baked table, Falcon Dive or
// fall leans. Its own table and leans come packed (Ecb_Unpack), or the
// table is learned in play (and its falls don't lean).
static int sim_kind = FTKIND_FALCON; // the fighter the tables are for

static void Sim_ForFighter(int kind)
{
    int falcon = kind == FTKIND_FALCON;
    sim_djc = Djc_Kind(kind);
    sim_mj = kind == FTKIND_KIRBY || kind == FTKIND_JIGGLYPUFF;
    sim_gw = kind == FTKIND_GAW;
    if (falcon == sim_falcon && kind == sim_kind)
        return;
    sim_kind = kind; // another fighter: a new one, or Zelda and Sheik changing
    sim_falcon = falcon;
    fall_loop[0] = fall_loop[1] = fall_loop[2] = LL_FALL_LOOP;
    fall_step = 1;
    fall_blend = 0;
    Learned_Clear();
    lean_rows = falcon ? lean_ecb : lean_own;
    lean_count = 0;
    if (falcon)
    {
        Learned_Bake();
        lean_count = countof(lean_ecb);
    }
    Lean_Index();
}

// The fighters' tables as tools/llsim/ecbpack.py packs them (TM/llecb.bin),
// big-endian: "LLE1", the number of fighters, and from byte 32 a directory
// of LLE_FIGHTERS entries (internal kind, offset, size, a pad word). A
// fighter's block: u16 rows, u16 states, each state's s16 frames and s16
// next state (as LLLEN), the falls' loops (u8 each for Fall, FallAerial,
// FallSpecial; 0 = Falcon's) and their lean step (u8 fall_step; 0 = 1, the
// pad byte of packs from before it), then each row: u8 state, u8 frame
// (a fall's: its animation frame), u8 flags (BAKED_*), a pad byte and five
// floats (as BakedEcb). Then sections, each a u32 tag, its u32 size and its
// data, up to a zero tag: "LEAN", the falls' leans: u16 rows, a pad u16,
// each row u8 fall, u8 leaning back, u8 animation frame in the loop, a pad
// byte and six floats (as LeanEcb, in its order). "DJMV", a
// double-jump-cancel fighter's double jump movement: u16 frames forward,
// u16 backward, then each frame's two floats (DjMove x and y), forward ones
// first. "MJMP", Kirby's and Jigglypuff's air jumps (TS_MJ1 on): u16
// states, u16 rows, each state's s16 frames and s16 next state (as LLLEN),
// then rows as above with the jump's index (0 for TS_MJ1) for the state.
// Blocks start and end on 32 bytes, as the disc reads them.
#define LLE_FIGHTERS 32
#define LLE_DIR_BYTES (32 + LLE_FIGHTERS * 16)
#define LLE_ROW 24
#define LLE_LEAN_ROW 28
#define LLE_TAG_LEAN 0x4C45414E // "LEAN"
#define LLE_TAG_DJMV 0x444A4D56 // "DJMV"
#define LLE_TAG_MJMP 0x4D4A4D50 // "MJMP"

static u32 Pack_U32(const u8 *b)
{
    return (u32)b[0] << 24 | (u32)b[1] << 16 | (u32)b[2] << 8 | b[3];
}

static int Pack_S16(const u8 *b)
{
    return (s16)(b[0] << 8 | b[1]);
}

static float Pack_F32(const u8 *b)
{
    u32 u = Pack_U32(b);
    float f;
    memcpy(&f, &u, sizeof(f));
    return f;
}

// Where kind's block is in the file, from its first LLE_DIR_BYTES; 0 if
// it has none.
static int Ecb_Find(const u8 *dir, int kind, int *offset, int *size)
{
    if (Pack_U32(dir) != 0x4C4C4531) // "LLE1"
        return 0;
    int n = Pack_U32(dir + 4);
    for (int i = 0; i < n && i < LLE_FIGHTERS; i++)
    {
        const u8 *e = dir + 32 + i * 16;
        if ((int)Pack_U32(e) == kind)
        {
            *offset = Pack_U32(e + 4);
            *size = Pack_U32(e + 8);
            return *size > 0;
        }
    }
    return 0;
}

// A packed row's ECB (u8 state, u8 frame, u8 flags, pad, five floats).
static void Pack_Ecb(const u8 *r, EcbSample *e)
{
    int flags = r[2];
    e->bottom = Pack_F32(r + 4);
    e->top = Pack_F32(r + 8);
    e->side_y = Pack_F32(r + 12);
    e->front = Pack_F32(r + 16);
    e->back = Pack_F32(r + 20);
    e->has_bottom = (flags & BAKED_BOTTOM) != 0;
    e->has_shape = (flags & BAKED_SHAPE) != 0;
    e->aerial_lag = (flags & BAKED_LAG) != 0;
    e->seen = 1;
}

// A fighter's block in place of what's learned; the rows it took.
static int Ecb_Unpack(const u8 *b, int size)
{
    if (size < 4)
        return 0;
    int rows = Pack_S16(b) & 0xFFFF, states = Pack_S16(b + 2), at = 4;
    if (states > TS_COUNT || at + states * 4 + 4 + rows * LLE_ROW > size)
        return 0;
    Learned_Clear();
    for (int ts = 0; ts < states; ts++, at += 4)
    {
        state_len[ts] = Pack_S16(b + at);
        state_next[ts] = Pack_S16(b + at + 2);
    }
    for (int i = 0; i < 3; i++)
        fall_loop[i] = b[at + i] ? b[at + i] : LL_FALL_LOOP;
    fall_step = (b[at + 3] & 3) > 1 ? b[at + 3] & 3 : 1;
    fall_blend = b[at + 3] >> 2;
    at += 4;
    for (int i = 0; i < rows; i++, at += LLE_ROW)
    {
        int ts = b[at], frame = b[at + 1];
        if (ts >= TS_COUNT || frame >= LL_STATE_FRAMES)
            continue;
        Pack_Ecb(b + at, &ecb_table[ts * LL_STATE_FRAMES + frame]);
    }
    int leans = 0;
    while (at + 8 <= size && Pack_U32(b + at) != 0)
    {
        u32 tag = Pack_U32(b + at);
        int len = Pack_U32(b + at + 4), data = at + 8;
        at = data + len;
        if (len < 0 || at > size)
            break;
        // the leans, whole or not at all (each blend a full loop of its fall)
        if (tag == LLE_TAG_LEAN && len >= 4)
        {
            leans = Pack_S16(b + data) & 0xFFFF;
            if (sim_falcon || !lean_own || leans > LL_LEAN_MAX || 4 + leans * LLE_LEAN_ROW > len)
                leans = 0;
            for (int i = 0, r = data + 4; i < leans; i++, r += LLE_LEAN_ROW)
            {
                LeanEcb *l = &lean_own[i];
                l->fall = b[r] != 0;
                l->back = b[r + 1] != 0;
                l->k = b[r + 2];
                l->lean = Pack_F32(b + r + 4);
                l->bottom = Pack_F32(b + r + 8);
                l->top = Pack_F32(b + r + 12);
                l->side = Pack_F32(b + r + 16);
                l->front = Pack_F32(b + r + 20);
                l->back_x = Pack_F32(b + r + 24);
            }
        }
        else if (tag == LLE_TAG_MJMP && len >= 4)
        {
            int n = Pack_S16(b + data), mj_rows = Pack_S16(b + data + 2) & 0xFFFF, r = data + 4;
            if (n < 0 || n > TS_MJ5 - TS_MJ1 + 1 || 4 + n * 4 + mj_rows * LLE_ROW > len)
                continue;
            for (int j = 0; j < n; j++, r += 4)
            {
                state_len[TS_MJ1 + j] = Pack_S16(b + r);
                state_next[TS_MJ1 + j] = Pack_S16(b + r + 2);
            }
            for (int i = 0; i < mj_rows; i++, r += LLE_ROW)
            {
                int j = b[r], frame = b[r + 1];
                if (j < n && frame < LL_STATE_FRAMES)
                    Pack_Ecb(b + r, &ecb_table[(TS_MJ1 + j) * LL_STATE_FRAMES + frame]);
            }
            rows += mj_rows;
        }
        else if (tag == LLE_TAG_DJMV && len >= 4)
        {
            int n[2] = {Pack_S16(b + data) & 0xFFFF, Pack_S16(b + data + 2) & 0xFFFF};
            if (4 + (n[0] + n[1]) * 8 > len)
                continue;
            for (int d = 0, r = data + 4; d < 2; d++)
                for (int f = 0; f < n[d]; f++, r += 8)
                {
                    if (f >= LL_STATE_FRAMES)
                        continue;
                    DjMove *m = Dj_Get(TS_JUMPAERIALF + d, f);
                    m->x = Pack_F32(b + r);
                    m->y = Pack_F32(b + r + 4);
                    m->seen = 1;
                }
        }
    }
    if (!sim_falcon)
    {
        lean_count = leans;
        Lean_Index();
    }
    return rows;
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

// The fall's ECB on this frame (in the state) and animation frame with the
// given lean.
static EcbSample *Lean_Ecb(EcbSample *plain, int ts, int side, int frame, int anim, float lean, EcbSample *out)
{
    int fall = ts == TS_FALLAERIAL, back = side == LEAN_BACK;
    if (side == LEAN_NONE || lean <= 0.f || !plain->has_bottom || !plain->has_shape)
        return plain;
    if (frame == 1 && onset_levels[fall][back] > 0)
        return Lean_Mix(plain, &lean_onset[onset_first[fall][back]], onset_levels[fall][back], 1, lean, out);
    if (lean_levels[fall][back] == 0)
        return plain;
    int loop = fall_loop[fall];
    return Lean_Mix(plain, &lean_rows[lean_first[fall][back] + anim % loop], lean_levels[fall][back], loop, lean, out);
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

// A fall's first frames ease from the last pose into the fall's
// (state.blend, 10 for Peach and 16 for DK): each frame closes 1/(blend-k)
// of the gap, starting from where the last pose was heading. The pose
// history is kept for every state so a fall knows where it starts.
static float Blend_Field(float *at, float to, int left)
{
    *at += (to - *at) / left;
    return *at;
}

static EcbSample *Fall_Blend(SimState *s, EcbSample *e, EcbSample *out)
{
    EcbSample *res = e;
    if (Is_Fall(s->ts) && s->frame < fall_blend && e->has_bottom && e->has_shape)
    {
        PoseEcb *b = &s->blend;
        if (s->frame == 0)
        {
            PoseEcb *p = &s->pose[0], *q = &s->pose[1];
            if (s->pose_ok >= 2)
            {
                b->top = 2 * p->top - q->top;
                b->side_y = 2 * p->side_y - q->side_y;
                b->front = 2 * p->front - q->front;
                b->back = 2 * p->back - q->back;
            }
            else if (s->pose_ok == 1)
                *b = *p;
            else
                Pose_From(e, b);
            if (s->pose_ok >= 2 && p->has_bottom && q->has_bottom)
                b->bottom = 2 * p->bottom - q->bottom;
            else if (s->pose_ok >= 1 && p->has_bottom)
                b->bottom = p->bottom;
            else
                b->bottom = e->bottom;
            b->has_bottom = 1;
        }
        int left = fall_blend - s->frame;
        *out = *e;
        out->bottom = Blend_Field(&b->bottom, e->bottom, left);
        out->top = Blend_Field(&b->top, e->top, left);
        out->side_y = Blend_Field(&b->side_y, e->side_y, left);
        out->front = Blend_Field(&b->front, e->front, left);
        out->back = Blend_Field(&b->back, e->back, left);
        res = out;
    }
    s->pose[1] = s->pose[0];
    Pose_From(res, &s->pose[0]);
    if (s->pose_ok < 2)
        s->pose_ok++;
    return res;
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
        // a state the fighter's table has but never saw end (Link's and
        // Young Link's dair) lasts until it lands
        if (s->len == 0 && !(own_table && Ecb_Slot(ts, 0)->seen))
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
    s->anim = Is_Fall(ts) ? Fall_Anim(fp, ts) : 0;
    EcbSample live = {0};
    Ecb_Fill(fp, &live);
    Pose_From(&live, &s->pose);
    s->pose_ok = 1;
    s->drift = (Vec2){0, 0};
    s->special_lag = 0;
    s->dodge_late = 0;
    s->dj_turn = 0;
    s->jumps = fp->jump.jumps_used;
    if (Tracked_IsUpB(ts))
        s->drift = Upb_Drift(fp);
    else if (Is_MultiJump(ts))
        s->dj_turn = fp->state_var.state_var1; // mv.co.jumpaerial.x0, as Yoshi's
    else if (Dj_Animated(ts))
    {
        // mv.co.jumpaerial: Yoshi's turn count, then Ness's drift speed
        if (sim_djc == DJC_YOSHI)
            s->dj_turn = fp->state_var.state_var1;
        else if (sim_djc == DJC_NESS)
            memcpy(&s->drift.X, &fp->state_var.state_var2, sizeof(float));
    }
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
    s->anim = start->anim;
    s->pose[0] = s->pose[1] = s->blend = start->pose;
    s->pose_ok = start->pose_ok;
    s->locked_bottom = start->locked_bottom;
    s->face = start->facing;
    s->drift = start->drift;
    s->special_lag = start->special_lag;
    s->dodge_late = start->dodge_late;
    s->dj_turn = start->dj_turn;
    s->jumps = start->jumps;
}

// One frame, in the game's order: animation (the state can end), interrupt
// (press, an aerial or airdodge replaces the state), input timers, physics,
// ECB, floor test. press is the aerial or TS_ESCAPEAIR pressed this frame,
// or -1; dodge_x is the airdodge's direction (+1 right, -1 left),
// horizontal unless sim_dodge_y is set (then both are the unit direction's
// parts).
static int sim_steps; // simulated frames, to spread the ledge route search over game frames
static float sim_dodge_y;

// Kirby's and Jigglypuff's air jumps after their first come out with the
// stick just held up, no flick (ftCo_800D730C), from a jump or fall, or
// from one of those jumps once its animation sets cmd_vars[0] (its table
// keeps that in aerial_lag, as Ecb_Fill reads it; on its first frame the
// check was the state's before). X or Y held would too, but the prediction
// presses nothing.
static int Mj_Held(FighterData *fp, SimStart *start, SimState *s, SimStep *out)
{
    if (!sim_mj || s->jumps < 2 || s->jumps >= fp->attr.max_jumps || start->stick_y < common_tap_jump_stick)
        return 0;
    if (Is_MultiJump(s->ts))
    {
        if (s->frame == 0)
            return 0;
        EcbSample *e = Ecb_Get(s->ts, s->frame);
        if (!e->seen)
            out->unlearned = 1;
        return e->aerial_lag;
    }
    return s->ts == TS_JUMPF || s->ts == TS_JUMPB || Is_Fall(s->ts);
}

static void Mj_Enter(FighterData *fp, SimState *s, float stick_x);

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
    if (s->len > 0 && s->frame >= s->len && Tracked_Next(s->ts) < 0)
    {
        // a state that doesn't end by itself (a fall) given a length: it
        // goes on, never into "state -1"
        s->len = 0;
        s->len_estimated = 0;
    }
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

    // Yoshi's double jump turning him around (its animation callback, so
    // before an interrupt), and Kirby's and Jigglypuff's air jumps
    if (((sim_djc == DJC_YOSHI && Dj_Animated(s->ts)) || Is_MultiJump(s->ts)) && s->frame > 0)
        Dj_Turn(fp, &s->dj_turn, &s->face);

    // their next air jump, the stick held up (ftCo_JumpAerial_IASA and the
    // falls' check it after aerials and the airdodge)
    if (press < 0 && Mj_Held(fp, start, s, out))
        Mj_Enter(fp, s, start->stick_x);

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

    // a fall's lean: back to plain on entering, then toward the drift speed;
    // its animation moves on first, fall_step frames if it leant last frame
    if (Is_Fall(s->ts))
    {
        if (s->frame == 0)
        {
            s->lean = 0;
            s->lean_side = LEAN_NONE;
            s->anim = 0;
        }
        else
        {
            s->anim = (s->anim + (s->lean > 0.f ? fall_step : 1)) % fall_loop[Fall_Index(s->ts)];
            s->lean_side = Lean_Update(fp, s->vx, s->face, &s->lean);
        }
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
    EcbSample *e = Ecb_Get(s->ts, Is_Fall(s->ts) ? s->anim : s->frame);
    if (Is_Fall(s->ts))
        e = Lean_Ecb(e, s->ts, s->lean_side, s->frame, s->anim, s->lean, &out->lean_ecb);
    if (fall_blend)
        e = Fall_Blend(s, e, &out->blend_ecb);
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
    else if (Dj_Animated(s->ts))
    {
        // the double jump's own physics: drift, and the animation's speed
        DjMove *a = Dj_Get(s->ts, s->frame);
        if (!a->seen)
            out->unlearned = 1;
        if (sim_djc == DJC_NESS)
        {
            // ftNs_JumpAerial_Phys_Cb: his drift speed drifts from itself,
            // but with the stick at 0 its air friction slows his whole speed
            // last frame, the animation's included (Deaccel reads self_vel)
            s->drift.X += Drift_Accel(fp, start->stick_x == 0 ? s->vx : s->drift.X, start->stick_x);
            s->vx = a->x * s->face + s->drift.X;
        }
        else
            s->vx += Drift_Accel(fp, s->vx, start->stick_x);
        s->vy = a->y;
    }
    else
    {
        // physics (ft_80084DB0; Kirby's and Jigglypuff's air jumps,
        // ft_80084E1C, differ only in the drift)
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
        if (Is_MultiJump(s->ts))
            s->vx += Mj_DriftAccel(fp, s->vx, start->stick_x);
        else
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
// ftCo_LandingAir_EnterWithLag for aerials (Game & Watch's own nair, bair
// and uair: Aerial_LCancels), ftCo_LandingFallSpecial_Enter
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
    if (!Aerial_LCancels(s->ts))
        halved = (int)full;
    *lcancel_lag = halved;

    int lcancel = Aerial_LCancels(s->ts) && s->trigger_timer < common_lcancel_window;
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
// Only branch actions off frames where the fighter rises: only rising
// aerial interrupts are worth showing (Windows_Summarize), so a search
// that keeps nothing else skips the rest, most of the work.
static int sim_rising_only;
// ... and stop once falling too fast to land as a NIL (Landing_Kind): no
// NIL or rising AI can come after that.
static float sim_stop_vy = -100000.f;

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

        if (branches && (!sim_rising_only || s.vy > 0 || before.vy > 0) &&
            (((branches & BR_LOCK) && before.lock > 0) || Floor_Near(s.x, s.y + s.bottom)))
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
        if (s.vy < sim_stop_vy)
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
// Sim_GroundJumpAt does it from a given spot and ground speed, as if the
// jump were pressed on the frame after (Hop Timing's hops further on); with
// slide_x, also gives where the squat frames slide to.
static int Sim_GroundJumpAt(FighterData *fp, int short_hop, float x, float y, float gr_vel, float stick_x, int fresh,
                            SimStart *s, float *slide_x);

static int Sim_GroundJump(FighterData *fp, int short_hop, SimStart *s)
{
    return Sim_GroundJumpAt(fp, short_hop, fp->phys.pos.X, fp->phys.pos.Y, fp->phys.self_vel_ground.X,
                            fp->input.lstick.X, fp->state_id != ASID_KNEEBEND, s, 0);
}

static int Sim_GroundJumpAt(FighterData *fp, int short_hop, float x, float y, float gr_vel, float stick_x, int fresh,
                            SimStart *s, float *slide_x)
{
    float startup = fp->attr.jump_startup_time;

    // KneeBend_Anim takes off once the animation frame reaches the startup
    // time; a new press starts the squat at frame 0 on the next frame
    float squat_left = startup;
    int until = 1;
    if (!fresh)
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
        if (slide_x)
            slide_x[i] = x;
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
    s->anim = 0;
    s->pose_ok = 0; // the grounded pose isn't the jump's
    s->drift = (Vec2){0, 0};
    s->special_lag = 0;
    s->dodge_late = 0;
    s->dj_turn = 0;
    s->jumps = 1; // ftCommon_8007D5D4
    return until;
}

// The double jump's state for the stick (times facing). A fighter whose
// table has nothing for the backward one (Yoshi: the game plays his
// forward one either way) gets the forward one.
static int Dj_State(float stick_x)
{
    if (stick_x > -common_jump_back_stick)
        return TS_JUMPAERIALF;
    if (!sim_falcon && !state_len[TS_JUMPAERIALB] && !Ecb_Slot(TS_JUMPAERIALB, 0)->seen)
        return TS_JUMPAERIALF;
    return TS_JUMPAERIALB;
}

// Kirby's or Jigglypuff's air jump n, n the jumps used so far
// (ftCo_800D74A4): its own state (x2C + n - 1), the stick's x speed and its
// own rise (x14[n - 1]); with the stick back past x4 it turns them around.
// No fastfall, and the stick timer goes on (ftCo_800CBAC4). Only the first
// locks the ECB bottom for 10 frames (ftCommon_8007D5D4 before it,
// ftCo_800D730C). s is on the new state's first frame.
static void Mj_Enter(FighterData *fp, SimState *s, float stick_x)
{
    FtMultiJumpDesc *mj = fp->multi_jump_desc;
    int n = s->jumps < 1 ? 1 : s->jumps > 5 ? 5 : s->jumps;
    int ts = TS_MJ1 + n - 1;
    s->ts = ts;
    s->frame = 0;
    s->len = state_len[ts];
    s->len_estimated = 0;
    if (s->len == 0)
    {
        s->len = LL_GUESS_JUMP_LEN;
        s->len_estimated = 1;
    }
    s->vx = stick_x * mj->vel_stick_x_mult;
    s->vy = mj->jump_vel_y[n - 1];
    s->fastfall = 0;
    s->ecb_pending = 0;
    if (n == 1)
    {
        s->lock = 10;
        s->locked_bottom = s->bottom;
    }
    s->dj_turn = 0;
    if (stick_x * s->face < -mj->turn_stick_x_min)
    {
        s->dj_turn = mj->turn_frames;
        Dj_Turn(fp, &s->dj_turn, &s->face); // counted on entry too
    }
    s->jumps = n + 1;
}

// A double jump pressed on this frame from a jump or fall
// (ftCo_JumpAerial_Enter_Basic): its speed replaces the old one, the ECB
// bottom stays where it was for 10 frames (ftCommon_8007D5D4) and the stick
// timer resets, then the frame plays out with the usual air physics.
// start's stick is the one held on this frame. Kirby and Jigglypuff do
// their next air jump instead (Mj_Enter).
static void Sim_DoubleJump(FighterData *fp, SimStart *start, SimState *s, SimStep *out)
{
    if (sim_mj)
    {
        Mj_Enter(fp, s, start->stick_x);
        s->frame = -1;
        Sim_Step(fp, start, s, -1, 0, out);
        return;
    }
    int ts = Dj_State(start->stick_x * start->facing);
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
    s->dj_turn = 0;
    if (Dj_Animated(ts))
    {
        s->vy = 0;
        if (sim_djc == DJC_NESS)
        {
            s->drift.X = s->vx;
            s->vx = 0;
        }
        else if (sim_djc == DJC_YOSHI && start->stick_x * start->facing < -Captain_Attr(fp, YS_TURN_STICK))
        {
            s->dj_turn = *(int *)((u8 *)fp->special_attributes + YS_TURN_FRAMES);
            Dj_Turn(fp, &s->dj_turn, &s->face); // ftYs_JumpAerial_Enter counts the first frame
        }
    }
    s->fastfall = 0;
    s->tilt_timer = LL_TIMER_MAX;
    s->lock = 10;
    s->locked_bottom = s->bottom;
    s->ecb_pending = 0;
    s->jumps = 2; // ftCommon_8007D5D4, then ftCo_800CBAC4
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
    out->anim = s->anim;
    out->pose = Is_Fall(s->ts) && s->frame < fall_blend ? s->blend : s->pose[0];
    out->pose_ok = s->pose_ok > 0;
    out->facing = s->face;
    out->drift = s->drift;
    out->special_lag = s->special_lag;
    out->dodge_late = 0;
    out->dj_turn = s->dj_turn;
    out->jumps = s->jumps;
}

// The common numbers, the tables and the stage caches' room, once at the
// start (Event_Init, or tools/llsim with the game's data loaded).
static void Sim_Setup(void)
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
    common_run_stick = Common_Float(COMMON_RUN_STICK);
    common_run_taper = Common_Float(COMMON_RUN_TAPER);
    common_dashrun_friction = Common_Float(COMMON_DASHRUN_FRICTION);
    common_tap_jump_stick = Common_Float(COMMON_TAP_JUMP_STICK);
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
    lean_own = calloc(sizeof(LeanEcb) * LL_LEAN_MAX);
    dj_move = calloc(sizeof(DjMove) * 2 * LL_STATE_FRAMES);
    Learned_Clear();
    Learned_Bake();
    Lean_Index();
    floor_cache = calloc(sizeof(FloorLine) * LL_MAX_FLOORS);
    ceil_cache = calloc(sizeof(FloorLine) * LL_MAX_CEILS);
    for (int side = 0; side < 2; side++)
    {
        wall_ids[side] = calloc(sizeof(s16) * LL_MAX_WALLS);
        wall_box[side] = calloc(sizeof(WallBox) * LL_MAX_WALLS);
    }
}

// Script command "dump" ends with this, and tools/llsim runs the same: hops
// from fixed spots on Battlefield's floors predicted in full, one line each
// with what they land as and a hash of every frame, so the PC build can be
// checked against the game line for line.
static u32 Check_Hash(u32 h, u32 v)
{
    for (int i = 0; i < 4; i++)
    {
        h ^= (v >> (i * 8)) & 0xFF;
        h *= 16777619u;
    }
    return h;
}

static u32 Check_Bits(float f)
{
    union
    {
        float f;
        u32 u;
    } v = {f};
    return v.u;
}

static void Sim_Check(FighterData *live, Prediction *p)
{
    static FighterData *copy; // a copy, facing right with no stick or trigger history
    if (!copy)
        copy = calloc(sizeof(FighterData)); // ~9 KB: too big a static for hmex at -O2
    FighterData *fp = copy;
    static const float spots[][2] = {{-60, 0}, {-40, 0}, {-20, 0}, {0, 0}, {20, 0}, {40, 0}, {60, 0}, {-40, 27.2f}, {0, 54.4f}};
    static const float sticks[] = {-1, 0, 1};
    static const float speeds[] = {0, 1.5f};
    *fp = *live;
    fp->facing_direction = 1;
    fp->input.lstick.Y = 0;
    fp->input.timer_trigger_any_ignore_hitlag = LL_TIMER_MAX;
    OSReport("LLCHECK begin wlground %d\n", Cues_WavelandGround());
    for (int i = 0; i < (int)countof(spots); i++)
        for (int sh = 0; sh < 2; sh++)
            for (int k = 0; k < (int)countof(sticks); k++)
                for (int v = 0; v < (int)countof(speeds); v++)
                {
                    SimStart s;
                    Sim_GroundJumpAt(fp, sh, spots[i][0], spots[i][1], speeds[v], sticks[k], 1, &s, 0);
                    Predict(fp, &s, p, BR_ALL);
                    u32 h = 2166136261u;
                    for (int f = 0; f <= p->num; f++)
                    {
                        h = Check_Hash(h, Check_Bits(p->pos[f].X));
                        h = Check_Hash(h, Check_Bits(p->pos[f].Y));
                        h = Check_Hash(h, Check_Bits(p->bottom[f]));
                        h = Check_Hash(h, (u32)p->ts[f] | p->ai_mask[f] << 8 | p->ai_show[f] << 16 | p->wl_mask[f] << 24);
                    }
                    OSReport("LLCHECK %d %d %d %d num %d land %d kind %d lag %d ai %d %d %d wl %d %d %d hash %08x\n", i, sh, k, v,
                             p->num, p->land_frame, p->land_kind, p->lag, p->ai_first, p->ai_width, p->ai_aerials,
                             p->wl_first, p->wl_width, p->wl_dirs, h);
                }
    OSReport("LLCHECK end\n");
}
