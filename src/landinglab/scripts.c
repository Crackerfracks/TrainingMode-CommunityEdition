/* landinglab/scripts.c
 *
 * Holds test scripts: reference playback, parsing and loading, placing
 * Falcon, running steps.
 *
 * Part of Landing Gear: src/landinglab.c includes it, in order, into one
 * translation unit, so it sees everything included before it (and mex.h,
 * events.h, landinglab_sim.h).
 *
 * Uses: option_values, menus_practice, menus_display, presets,
 * tracking_state, rumble, logging, hud_text, timers, debug_camera.
 * Refers ahead, through forward declarations, to: position_chords.
 */

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
//   follow <n> [inputs]        hold the inputs and press what Jump Timing
//                              shows, on its frames, for n frames or until
//                              back on the ground after leaving it
//   playroute [<n>]            while hanging: wait (stick at rest) until the
//                              ledge's routes are found, then play the nth
//                              row of the timers (1, the chosen route, if
//                              none; a later one is chosen first, as the
//                              Route option would) with the inputs the search
//                              simulated it with: the drop, the wait, the
//                              fastfall, the jump and its stick, the hold and
//                              an AI's aerial on its window's first frame.
//                              Over at the touchdown. Logs LLPLAY and
//                              LLROUTE play (the row), to compare with
//                              LLROUTE follow and the "LandingLab route:"
//                              lines
//   shot [label]               freeze the game after the frame before; D-pad
//                              down goes on, the advance button steps
//   trace on|off               opt-in LLST1 engine-state records (all states)
//   mark [label]               only write a line to the log
//   sync [label]               the whole screen magenta for two drawn frames
//                              (LLSYNC in the log): where a GIF's frame dump
//                              lines up with the log
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
//   view save|load|show <1-8>  Camera > Save View, picking that View, or
//                              logging where the camera is against it
//   view name <1-8> <word>     name a saved view
//   view shift 1 <dx> <dy>     slide the Advanced camera
//   fadereset                  Intensity > Reset Fade
//   notfor <kind> ...          leave the steps after it out for these
//                              fighters (by kind, e.g. 7 19 for Sheik and
//                              Zelda), up to the next notfor line; a bare
//                              notfor ends it. Logs LLSKIP for each step left
//                              out
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
    SOP_FOLLOW, // count frames of doing what Jump Timing shows, on its frames
    SOP_PLAY,   // play the count-th row of the ledge's routes (playroute)
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
    SCMD_FADE,
    SCMD_GET,
    SCMD_VIEW,
    SCMD_BAKE,
    SCMD_TRACE,
    SCMD_DUMP,
    SCMD_HOVER,
    SCMD_CAPTURE,
    SCMD_DUMPECB,
    SCMD_REFILL,
    SCMD_SYNC,
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
    u32 not_for; // fighter kinds (bits) that leave this step out (notfor)
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

static int script_trace_last = -1;
static int script_trace_end_pending;
static char script_trace_id[65]; // whitespace-safe, bounded trace identity

static int script_cur = -1; // running script, -1 = none
static char shot_label[48];  // the last shot's label, for the heartbeat
static int script_pc;       // its current step
static int script_left;     // frames left in that step
static int script_wait;     // frames a wl/ai/land step has waited
static int script_air;      // Falcon has been in the air during a land or follow step
static int script_window;   // the frame a wl/ai step's window opened, -1 = not yet
static int script_width;    // how many frames the prediction gave it
static int script_pending;  // start the chosen script on the next frame
static ScriptInput script_last; // the last frame's inputs
static int live_timer;      // the frame pred_live was made on

static void Reference_Restore(int keep_advance)
{
    if (!reference_mode)
        return;
    int mode = reference_mode;
    int advance = Options_Game[GOPT_FRAME_ADV].val;
    for (int i = 0; i < (int)countof(reference_options); i++)
        reference_options[i]->val = reference_saved[i];
    // Taking over a practice shot must not undo the player's pause choice.
    if (keep_advance)
        Options_Game[GOPT_FRAME_ADV].val = advance;
    reference_mode = 0;
    OSReport("LLREF restore mode=%d assist=%d reset=%d inv=%d drop=%d routes=%d advance=%d\n",
             mode, reference_saved[0], reference_saved[1], reference_saved[2],
             reference_saved[3], reference_saved[4], Options_Game[GOPT_FRAME_ADV].val);
}

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
    u32 not_for = 0;
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
            not_for = 0;
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
        if (Script_Is(w, "notfor"))
        {
            not_for = 0;
            for (char *k; (k = Script_Word(&rest));)
                not_for |= 1u << ((int)Script_Number(&k) & 31);
            continue;
        }
        ScriptOp *op = &script_ops[script_op_num];
        memset(op, 0, sizeof(*op));
        op->not_for = not_for;
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
                 Script_Is(w, "card") || Script_Is(w, "chord") || Script_Is(w, "view") || Script_Is(w, "fadereset") ||
                 Script_Is(w, "bake") || Script_Is(w, "trace") || Script_Is(w, "dump") || Script_Is(w, "hover") ||
                 Script_Is(w, "capture") || Script_Is(w, "dumpecb") || Script_Is(w, "refill") || Script_Is(w, "sync"))
        {
            op->kind = SOP_CMD;
            op->count = Script_Is(w, "sync") ? SCMD_SYNC : Script_Is(w, "dumpecb") ? SCMD_DUMPECB : Script_Is(w, "refill") ? SCMD_REFILL : w[0] == 'h' ? SCMD_HOVER : Script_Is(w, "capture") ? SCMD_CAPTURE
                      : w[0] == 'd' ? SCMD_DUMP : w[0] == 't' ? SCMD_TRACE : w[0] == 'b' ? SCMD_BAKE : w[0] == 'f' ? SCMD_FADE : w[0] == 's' ? SCMD_SET : w[0] == 'g' ? SCMD_GET : w[0] == 'v' ? SCMD_VIEW : w[1] == 'l' ? SCMD_CLOSEMENU : w[0] == 'p' ? SCMD_PRESET : w[1] == 'a' ? SCMD_CARD : SCMD_CHORD;
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
        else if (Script_Is(w, "follow"))
        {
            char *n = Script_Word(&rest);
            op->kind = SOP_FOLLOW;
            ok = n != 0;
            if (ok)
                op->count = (int)Script_Number(&n);
            ok = ok && op->count > 0;
            inputs = 1;
        }
        else if (Script_Is(w, "playroute"))
        {
            char *n = Script_Word(&rest);
            op->kind = SOP_PLAY;
            op->count = n ? (int)Script_Number(&n) : 1;
            ok = op->count > 0 && Script_Word(&rest) == 0;
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
static void Ledge_Forget(void);

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
    Ledge_Forget();
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

// The ledge Script_PlaceLedge hung the fighter on, watched for a few
// frames (Script_LedgeCheck).
static int ledge_check_line = -1, ledge_check_facing, ledge_check_left;

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
    ledge_check_line = best;
    ledge_check_facing = op->facing;
    ledge_check_left = 3;
    fp->jump.jumps_used = 1;
    Script_ResetTimers(fp);
    Script_FixCamera(ft);
    Script_ResetTracking();
    return 1;
}

// Pichu, put straight into CliffWait, stays on the ledge's corner (his
// TransN isn't there, ftCo_CliffCatch_Phys) and on the next frame stands
// up on the stage. Then drop him from above the ledge, just off it, and
// let the game catch it as a player would.
static void Script_LedgeCheck(GOBJ *ft)
{
    FighterData *fp = ft->userdata;
    if (ledge_check_line < 0)
        return;
    Vec3 corner;
    if (ledge_check_facing > 0)
        GrColl_GetGroundLineEndLeft(ledge_check_line, &corner);
    else
        GrColl_GetGroundLineEndRight(ledge_check_line, &corner);
    if (fp->phys.air_state == 0 && fabs(fp->phys.pos.X - corner.X) < 1.f && fabs(fp->phys.pos.Y - corner.Y) < 1.f)
    {
        fp->facing_direction = ledge_check_facing;
        fp->phys.self_vel.X = 0;
        fp->phys.self_vel.Y = 0;
        // where ledgedash training's "falling" start puts a fighter (5 out,
        // 20 above): falling from there it sweeps past the corner
        fp->phys.pos.X = corner.X - ledge_check_facing * 5.f;
        fp->phys.pos.Y = corner.Y + 20.f;
        Fighter_SetAirborne(fp);
        Fighter_EnterFall(ft);
        Script_UpdatePosition(ft);
        fp->jump.jumps_used = 1;
        fp->ledge_cooldown = 0; // set as he stood up out of CliffWait: it would keep him from the catch
        Log("LLSCRIPT ledge: falling into the ledge's catch\n");
        ledge_check_line = -1;
        return;
    }
    if (--ledge_check_left <= 0)
        ledge_check_line = -1;
}

static void Script_Next(void);

static void Script_End(void);
// Capturing a character's ECB (script commands hover, capture, dumpecb).
static int hover_on, capture_on;
static float hover_x, hover_y;
static float hover_lean; // a fall's lean target to hold there (signed: < 0 leans back), 0 = still

// ... and every state of the character's own (its specials, Kirby's and
// Jigglypuff's air jumps: state ids from 341 on) seen in the air while
// capturing, in slots taken as they come (Stephen, 0.9.16: specials too)
#define SPEC_SLOTS 40
#define ASID_CHAR_FIRST 341
static EcbSample *spec_table;       // [SPEC_SLOTS][LL_STATE_FRAMES]
static s16 spec_sid[SPEC_SLOTS];    // the state in each slot
static s16 spec_len[SPEC_SLOTS];    // frames it last ran before changing state, 0 = still on
static s16 spec_next[SPEC_SLOTS];   // the state it changed to, -1 = not seen
static int spec_n, spec_cur = -1;   // slots used, the slot of the state now

static void Spec_Clear(void)
{
    if (!spec_table)
        spec_table = calloc(sizeof(EcbSample) * SPEC_SLOTS * LL_STATE_FRAMES);
    memset(spec_table, 0, sizeof(EcbSample) * SPEC_SLOTS * LL_STATE_FRAMES);
    spec_n = 0;
    spec_cur = -1;
}

// Each frame while capturing: the character's own states, in the air.
static void Spec_Think(FighterData *fp, int sid, int frame)
{
    if (!capture_on || !spec_table)
        return;
    int slot = -1;
    if (sid >= ASID_CHAR_FIRST && fp->phys.air_state == 1)
    {
        for (int i = 0; i < spec_n; i++)
            if (spec_sid[i] == sid)
                slot = i;
        if (slot < 0 && spec_n < SPEC_SLOTS)
        {
            slot = spec_n++;
            spec_sid[slot] = sid;
            spec_len[slot] = 0;
            spec_next[slot] = -1;
        }
    }
    if (spec_cur >= 0 && spec_sid[spec_cur] != sid)
    {
        spec_next[spec_cur] = sid; // what it went into, and after how long
        spec_cur = -1;
    }
    if (slot < 0)
        return;
    spec_cur = slot;
    if (frame + 1 > spec_len[slot])
        spec_len[slot] = frame + 1;
    if (frame < LL_STATE_FRAMES)
        Ecb_Fill(fp, &spec_table[slot * LL_STATE_FRAMES + frame]);
}

// Post-collision snapshot at event priority 15, before event overrides. Three
// bounded physical records form one logical FRAME. Never indexes tracked data.
static void Script_TraceFrame(FighterData *fp)
{
    int timer = event_vars->game_timer;
    if (!script_trace || script_cur < 0 || script_trace_last == timer)
        return;
    script_trace_last = timer;
    CollData *cd = &fp->coll_data;
    HSD_Pad *pad = PadGetEngine(fp->pad_index);
    int sid = fp->state_id;
    // At most 64 ID bytes; %.9g bounds each finite or nonfinite float. Each
    // format below plus worst-case 32-bit integers fits this 768-byte buffer.
    char buf[768];
    sprintf(buf, "LLST1 FRAME id=%s timer=%d part=0 state_id=%d animation_frame=%.9g posx=%.9g posy=%.9g velocityx=%.9g velocityy=%.9g ground_velocityx=%.9g air_state=%d facing=%.9g fastfall=%d jumps_used=%d intang=%d wait=%d normal_landing=%d aerial=%d cliff_catch=%d cliff_wait=%d keep_inv=%d\n",
            script_trace_id, timer, sid, fp->state.frame,
            fp->phys.pos.X, fp->phys.pos.Y, fp->phys.self_vel.X, fp->phys.self_vel.Y,
            fp->phys.self_vel_ground.X, fp->phys.air_state, fp->facing_direction,
            fp->flags.is_fastfall, fp->jump.jumps_used, fp->hurt.intang_frames.ledge,
            sid == ASID_WAIT, sid == ASID_LANDING,
            Is_AerialState(sid),
            sid == ASID_CLIFFCATCH, sid == ASID_CLIFFWAIT, Options_Ledge[LOPT_INV].val);
    Log(buf);
    sprintf(buf, "LLST1 FRAME id=%s timer=%d part=1 ecb_lock=%d bottom_desired=%.9g bottom_corrected=%.9g ecb_top=%.9g ecb_left=%.9g ecb_right=%.9g ecb_side_y=%.9g floor_line=%d ignore_line=%d\n",
            script_trace_id, timer, cd->u.ecb_bot_lock_frames,
            cd->ecbCurr_bot.Y, cd->ecbCurrCorrect_bot.Y, cd->ecbCurr_top.Y,
            cd->ecbCurr_left.X, cd->ecbCurr_right.X, cd->ecbCurr_right.Y,
            cd->ground_index, cd->ignore_line);
    Log(buf);
    sprintf(buf, "LLST1 FRAME id=%s timer=%d part=2 stickx=%.9g sticky=%.9g cstickx=%.9g csticky=%.9g held=%u trigger=%.9g trigger_left=%.9g trigger_right=%.9g tilt_y=%u smash_y=%u any_y=%u trigger_analog_timer=%u trigger_digital_timer=%u trigger_any_timer=%u trigger_nohitlag_timer=%u\n",
            script_trace_id, timer, fp->input.lstick.X, fp->input.lstick.Y,
            fp->input.cstick.X, fp->input.cstick.Y, (unsigned int)fp->input.held,
            fp->input.trigger, pad->ftriggerLeft, pad->ftriggerRight,
            (u8)fp->input.timer_lstick_tilt_y, (u8)fp->input.timer_lstick_smash_y,
            (u8)fp->input.timer_lstick_any_y, (u8)fp->input.timer_trigger_analog,
            (u8)fp->input.timer_trigger_digital, (u8)fp->input.timer_trigger_any,
            (u8)fp->input.timer_trigger_any_ignore_hitlag);
    Log(buf);
}

static void Script_Begin(int index)
{
    char buf[120];
    script_trace = 0;
    script_trace_last = -1;
    script_trace_end_pending = 0;
    script_cur = index;
    script_pc = -1;
    Script_Next();
    memset(&script_last, 0, sizeof(script_last));
    Learned_Reset();
    sprintf(text_last, "-");
    sprintf(buf, "LLRUN %d %s at %d\n", index + 1, scripts[index].name, event_vars->game_timer);
    Log(buf);
}

static void Script_End(void)
{
    if (script_trace && script_trace_last != event_vars->game_timer)
    {
        script_trace_end_pending = 1;
        return;
    }
    char buf[192];
    if (script_trace)
    {
        sprintf(buf, "LLST1 END id=%s frame=%d\n", script_trace_id, event_vars->game_timer);
        Log(buf);
    }
    script_trace = 0;
    script_trace_end_pending = 0;
    hover_on = 0;
    hover_lean = 0;
    capture_on = 0;
    Script *sc = &scripts[script_cur];
    sprintf(buf, "LLSCRIPT end %s at %d, last landing: %s\n", sc->name, event_vars->game_timer, text_last);
    Log(buf);

    if (reference_mode)
    {
        OSReport("LLREF end mode=%d frame=%d\n", reference_mode, event_vars->game_timer);
        if (reference_mode == 1)
            Reference_Restore(0);
    }

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
    FighterData *fp = Fighter_GetGObj(0)->userdata;
    u32 kind = 1u << (fp->kind & 31);
    while (script_pc < sc->num && script_ops[sc->first + script_pc].not_for & kind)
    {
        char buf[64];
        sprintf(buf, "LLSKIP step %d for kind %d\n", script_pc + 1, fp->kind);
        Log(buf);
        script_pc++;
    }
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
