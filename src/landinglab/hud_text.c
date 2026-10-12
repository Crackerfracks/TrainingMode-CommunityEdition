/* landinglab/hud_text.c
 *
 * Holds which cues are on, the prediction as text, and press feedback (missed
 * windows).
 *
 * Part of Landing Lab: src/landinglab.c includes it, in order, into one
 * translation unit, so it sees everything included before it (and mex.h,
 * events.h, landinglab_sim.h).
 *
 * Uses: menus_display, tracking_state, rumble, logging.
 * Refers ahead, through forward declarations, to: timers.
 */

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

// The waveland cue also counts down to a wavedash out of the jumpsquat,
// which has its own switch, Timers > Wavedash, so it can be on with the
// waveland cues off (Stephen, 0.8.4).
static int Cues_Wd(void)
{
    return Options_Timers[TOPT_WD].val != WDT_OFF;
}

// Whether cue c, of this kind, is drawn at all.
static int Cue_KindOn(int kind, Cue *c)
{
    if (kind == CUE_AI)
        return Cues_Ai();
    if (kind == CUE_NIL)
        return Cues_Nil();
    return c->wd ? Cues_Wd() : Cues_Waveland();
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
