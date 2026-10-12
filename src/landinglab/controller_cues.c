/* landinglab/controller_cues.c
 *
 * Holds Controller Cues (Closing Ring, Gauge) and Pad_Draw.
 *
 * Part of Landing Lab: src/landinglab.c includes it, in order, into one
 * translation unit, so it sees everything included before it (and mex.h,
 * events.h, landinglab_sim.h).
 *
 * Uses: option_values, menus_display, tracking_state, hud_text, drawing,
 * hud_drawing, meter, controller, controller_ring, controller_crest.
 * Refers ahead, through forward declarations, to: jump_timing.
 */

// The controller display. Reads the pad live; the trails and glows come
// from what Pad_Record saw.
// Controller Cues: the input a live AI or waveland window wants, timed
// around where it's drawn. The Closing Ring shrinks onto it from twice its
// size over the last frames before the window, and sits snug and thick
// while the window is open; the Gauge is a ring going round it, closed as
// the window opens. Either is only an outline in the cue's color, never a
// fill, so it can't be taken for a press.
#define PADCUE_LEAD 20 // frames ahead it shows
static void Pad_CueAt(int in, GXColor col, int ahead, int open);
static void Pad_CueOne(int in, int kind)
{
    Cue *c = &cue_live[kind];
    if (cue_log && c->phase && (!pin_spot[in].r || c->dim || c->held))
        OSReport("LLPADCUE %d pin %d kind %d skipped: r %.2f phase %d dim %d held %d left %d\n", event_vars->game_timer, in,
                 kind, pin_spot[in].r, c->phase, c->dim, c->held, c->left);
    if (!pin_spot[in].r || !c->phase || c->dim || c->held)
        return;
    int open = c->phase == PH_WINDOW;
    vis_k = Kind_K(VG_TIMERS, kind);
    Pad_CueAt(in, Cue_Color(kind), open ? 0 : c->left - 1, open);
}

// The cue itself, on input in, ahead frames before its window (0 and open
// while it's open).
static void Pad_CueAt(int in, GXColor col, int ahead, int open)
{
    if (!pin_spot[in].r || ahead > PADCUE_LEAD || ahead < 0)
        return;
    float x = pin_spot[in].x, y = pin_spot[in].y, r = pin_spot[in].r;
    float t = open ? 0 : (float)ahead / PADCUE_LEAD; // 1 far, 0 at the window
    if (cue_log)
        OSReport("LLPADCUE %d pin %d ahead %d open %d at %.2f %.2f r %.2f\n", event_vars->game_timer, in, ahead, open, x, y,
                 r);
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
    // a ledge route has its own steps; a jump's route (Jump Timing) cues
    // the jump buttons as well as the AI and waveland ones
    if (Options_Hud[HOPT_PAD_CUES].val == PADCUE_OFF || (route_rows_active && !jump_rows_active))
        return;
    float k = vis_k;
    int jump = Jump_PressAhead();
    if (jump >= 0)
    {
        vis_k = Group_K(VG_TIMERS);
        Pad_CueAt(PIN_X, color_in_jump, jump, jump == 0);
        Pad_CueAt(PIN_Y, color_in_jump, jump, jump == 0);
    }
    if (Cues_Ai())
        Pad_CueOne(PIN_A, CUE_AI);
    if (Cue_KindOn(CUE_WL, &cue_live[CUE_WL]))
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
