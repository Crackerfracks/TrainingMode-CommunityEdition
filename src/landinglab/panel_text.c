/* landinglab/panel_text.c
 *
 * Holds the panel's text each frame, the ghost, and the ground preview.
 *
 * Part of Landing Lab: src/landinglab.c includes it, in order, into one
 * translation unit, so it sees everything included before it (and mex.h,
 * events.h, landinglab_sim.h).
 *
 * Uses: menus_display, camera_views, tracking_state, rumble, hud_text,
 * timers.
 */

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
    int resting = sid == ASID_WAIT || sid == ASID_LANDING || sid == ASID_LANDINGFALLSPECIAL || Is_LandingAirState(sid);
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
