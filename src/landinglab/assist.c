/* landinglab/assist.c
 *
 * Holds the quicktime assist for ledge routes.
 *
 * Part of Landing Lab: src/landinglab.c includes it, in order, into one
 * translation unit, so it sees everything included before it (and mex.h,
 * events.h, landinglab_sim.h).
 *
 * Uses 16 of the 42 modules before it.
 */

///////////////////////
/// Assist          ///
///////////////////////

// Quicktime practice of the chosen route: the game freezes on the frame
// before each of its inputs and waits for it. The input comes: the game
// plays on at full speed to the next one. It doesn't come within the wait
// set in the menu: a window longer than a frame steps one frame further
// into it, and after its last frame the fighter starts over. While frozen,
// the stick is fed into the fighter's input timers every real frame, as the
// game would, so a slow press reads as a slow press when the game goes on.

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
static int assist_fx = -1;    // real frames since the last wait ended, -1 none
static u8 assist_fx_hit;      // ... with the input (1) or out of time (0)
static u8 assist_fx_step;     // ... waiting for this

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
        assist_fx = 0;
        assist_fx_hit = 1;
        assist_fx_step = assist_step;
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
    assist_fx = 0;
    assist_fx_hit = 0;
    assist_fx_step = assist_step;
    sprintf(text_last, "Assist: no %s in time", assist_names[assist_step]);
    last_kind = -1;
    if (Options_Sounds[SOPT_LOST].val)
        SFX_PlayCommon(3);
    attempt_facing = fp->facing_direction > 0 ? 1 : -1;
    Drill_Finish(0);
    Drill_Reset();
}

// The input Assist waits for, over the fighter's head: its glyphs in a ring
// that drains clockwise as the wait runs out, the window's frames left as dots
// under it. The input arriving bursts the ring out in white; running out of
// time folds it in, in the miss color. (Codex's note of Stephen's report:
// the old timer, a line in the info panel, didn't say an input was timed,
// how long was left, or whether it reset.)
#define AS_R 1.7f

// How high over the fighter's feet the ring goes: 24 units for Falcon, and
// for another in proportion to how tall its fall's ECB is against his
// (10.7793, baked_ecb).
static float Assist_Over(void)
{
    EcbSample *e = Ecb_Get(TS_FALL, 0);
    return e->has_shape && e->top > 1.f ? 24.f * (e->top / 10.7793f) : 24.f;
}

static void Assist_Glyphs(FighterData *fp, int step, int *g0, int *g1, GXColor *c)
{
    LedgeRoute *r = route_active ? &route_cur : &route_show[0];
    int facing = fp->facing_direction > 0 ? 1 : -1;
    *g1 = 0;
    *c = color_white;
    switch (step)
    {
    case ASSIST_DROP:
        *g0 = route_show[0].drop == DROP_DOWN ? GLYPH_DOWN : facing > 0 ? GLYPH_LEFT : GLYPH_RIGHT;
        break;
    case ASSIST_FF:
        *g0 = GLYPH_FF;
        break;
    case ASSIST_JUMP:
        *g0 = GLYPH_JUMP;
        *c = color_in_jump;
        *g1 = r->dj_x == LR_DJ_IN ? (facing > 0 ? GLYPH_RIGHT : GLYPH_LEFT) : r->dj_x == LR_DJ_UP ? GLYPH_UP : 0;
        break;
    default:
        *g0 = GLYPH_AERIAL;
        *c = color_in_aerial;
        *g1 = Aerial_Glyph(r->aerial, facing);
        break;
    }
}

static void Assist_Draw(void)
{
    if (!Options_Ledge[LOPT_ASSIST].val || capture_clean)
    {
        assist_fx = -1;
        return;
    }
    int live = assist_frozen && assist_step != ASSIST_NONE;
    if (!live && assist_fx < 0)
        return;
    FighterData *fp = Fighter_GetGObj(0)->userdata;
    float cx, cy;
    if (!Hud_FromWorld(fp->phys.pos.X, fp->phys.pos.Y + Assist_Over(), &cx, &cy))
        return;
    float lim_x = SAFE_W - AS_R - 0.5f, lim_y = SAFE_H - AS_R - 0.5f;
    cx = cx > lim_x ? lim_x : cx < -lim_x ? -lim_x : cx;
    cy = cy > lim_y ? lim_y : cy < -lim_y + 1.5f ? -lim_y + 1.5f : cy;
    int g0, g1;
    GXColor c;
    if (live)
    {
        Assist_Glyphs(fp, assist_step, &g0, &g1, &c);
        int wait = wait_frames[Options_Ledge[LOPT_WAIT].val];
        float left = 1.f - (float)assist_wait / wait;
        if (left < 0)
            left = 0;
        // it comes in a little big and settles, so a new wait is noticed
        float pop = assist_wait < 6 ? 1.f + 0.06f * (6 - assist_wait) : 1.f;
        float R = AS_R * pop;
        // the last third: the ring beats faster as time runs out
        float beat = 1.f;
        if (left < 0.34f)
        {
            int period = left < 0.15f ? 8 : 16;
            beat = (assist_wait % period) < period / 2 ? 1.f : 0.55f;
        }
        Hud_Disc(cx, cy, R + 0.3f, Color_Fill(color_plate, 0.85f));
        Hud_Ring(cx, cy, R, 0.1f, Color_Over(color_white, 0.25f));
        Dial_Band(cx, cy, R - 0.28f, R + 0.06f, 0, 360.f * left, Color_Over(c, beat));
        float gs = 0.5f, gw = 1.1f;
        if (g1)
        {
            Glyph_Draw(g0, cx - gw / 2, cy, gs, c);
            Glyph_Draw(g1, cx + gw / 2, cy, gs, color_white);
        }
        else
            Glyph_Draw(g0, cx, cy, gs, c);
        // a window longer than a frame: a dot for each frame left in it
        for (int i = 0; i < assist_left && assist_left > 1 && i < 8; i++)
        {
            float dx = (i - (assist_left - 1) / 2.f) * 0.55f;
            Hud_Disc(cx + dx, cy - R - 0.75f, 0.18f, c);
        }
        return;
    }
    // the ending
    Assist_Glyphs(fp, assist_fx_step, &g0, &g1, &c);
    if (assist_fx_hit)
    {
        float q = assist_fx / 8.f, R = AS_R * (1.f + q);
        Hud_Ring(cx, cy, R, 0.3f * (1.f - q), Color_Over(color_white, 1.f - q));
        Hud_Disc(cx, cy, AS_R * 0.7f * (1.f - q), Color_Over(c, 0.8f * (1.f - q)));
        if (++assist_fx >= 8)
            assist_fx = -1;
    }
    else
    {
        float q = assist_fx / 12.f, R = AS_R * (1.f - q);
        Hud_Ring(cx, cy, R, 0.25f, Color_Over(color_miss, 1.f - q * 0.5f));
        Glyph_Draw(g0, cx, cy, 0.5f * (1.f - q), Color_Over(color_miss, 1.f - q));
        if (++assist_fx >= 12)
            assist_fx = -1;
    }
}
