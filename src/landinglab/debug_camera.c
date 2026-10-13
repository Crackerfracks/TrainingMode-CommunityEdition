/* landinglab/debug_camera.c
 *
 * Holds camera logging, and forward declarations for the draw callbacks.
 *
 * Part of Landing Gear: src/landinglab.c includes it, in order, into one
 * translation unit, so it sees everything included before it (and mex.h,
 * events.h, landinglab_sim.h).
 *
 * Uses: option_values, menus_practice, menus_display, camera_views,
 * tracking_state, logging, controller.
 * Refers ahead, through forward declarations, to: jump_timing, ground_guide,
 * jump_search, markers, assist, slide_off.
 */

///////////////////////
/// Debug camera    ///
///////////////////////

// For mockups drawn on real game frames: where world points land on screen
// on the frame being drawn (the stage plane is z = 0, so four points give the
// exact mapping), and what the timers and the prediction hold. Logged from
// the draw so the camera is the one this frame is rendered with.
static int script_cur; // defined with the scripts below
static int script_trace; // defined with the scripts below
// Legacy scripts log everything; state traces respect the requested log level.
static int Log_Level(void) { return script_cur >= 0 && !script_trace && !reference_mode ? LOG_ALL : Options_Dev[DOPT_LOG].val; }
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
        if (fp->state_id != last_sid && (Is_DoubleJump(ts) || Is_MultiJump(ts)))
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
static void Assist_Draw(void);
static void Compass_Draw(void);
#ifdef LL_GROUND_GUIDE
static void Guide_Draw(FighterData *fp);
static int gd_bake = -1; // the ground guide height being baked, -1 none, -2 asked for by the script
#endif
