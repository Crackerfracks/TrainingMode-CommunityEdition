/* landinglab/rumble.c
 *
 * Holds the rumble ramp up to a window, and keeping the game's own rumble
 * off.
 *
 * Part of Landing Lab: src/landinglab.c includes it, in order, into one
 * translation unit, so it sees everything included before it (and mex.h,
 * events.h, landinglab_sim.h).
 *
 * Uses: menus_practice, tracking_state.
 */

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
static char text_predict[48] = "-";
static char text_ai[32] = "-";
static char text_last[48] = "-";
static u8 press_note; // text_last says why the last press stayed in the air: the landing keeps it
static char text_next[64] = "-"; // the panel's first line: what's coming up
static char text_steps[64];       // ... and a ledge route's inputs under it
static char text_frame[32]; // in Frame Advance: the state on screen and its frame
static int next_kind = -1;       // the cue (CUE_*) whose color the panel's lines start with, -1 none
static int last_kind = -1;
static char text_exact[32] = "-";
