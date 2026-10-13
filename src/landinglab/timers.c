/* landinglab/timers.c
 *
 * Holds the timers' life: floor under a cue, countdowns, hits, misses,
 * judging a landing.
 *
 * Part of Landing Lab: src/landinglab.c includes it, in order, into one
 * translation unit, so it sees everything included before it (and mex.h,
 * events.h, landinglab_sim.h).
 *
 * Uses: menus_display, tracking_state, rumble, logging, segments, hud_text.
 * Refers ahead, through forward declarations, to: drawing.
 */

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
    if (c->phase)
        Skill_Add(kind, 1); // only a timer that was shown counts toward its fade
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
    else if ((kind == LAND_PERFECT_WL || (kind == LAND_WAVELAND && wl->phase)) &&
             (Cues_Waveland() || (wl->phase && wl->wd && Cues_Wd())))
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
    int landing_air = Is_LandingAirState(sid);
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
    else if (Gw_Own(sid, ASID_GW_LANDINGAIRN) >= 0)
        kind = LAND_AERIAL; // Game & Watch's own: never L-cancelled (Aerial_LCancels)
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
