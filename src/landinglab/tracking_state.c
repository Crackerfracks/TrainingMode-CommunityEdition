/* landinglab/tracking_state.c
 *
 * Holds the live tracking state, the prediction buffers, and the timers' cue
 * state.
 *
 * Part of Landing Lab: src/landinglab.c includes it, in order, into one
 * translation unit, so it sees everything included before it (and mex.h,
 * events.h, landinglab_sim.h).
 */

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
static int wd_preload;        // on the ground, able to jump: where a wavedash's airdodge would be, 0 none
static int wd_last;           // ... last frame (the log checks it against the jumpsquat's)
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
