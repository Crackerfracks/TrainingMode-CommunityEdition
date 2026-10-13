/* landinglab/gx_draw.c
 *
 * Holds Platform Glow, the stage and HUD draw callbacks (World_GX, Hud_GX),
 * and the naming grid's drawing.
 *
 * Part of Landing Gear: src/landinglab.c includes it, in order, into one
 * translation unit, so it sees everything included before it (and mex.h,
 * events.h, landinglab_sim.h).
 *
 * Uses 16 of the 28 modules before it.
 * Refers ahead, through forward declarations, to: jump_timing, ground_guide,
 * jump_search, markers, assist, slide_off, preset_deck.
 */

// Platform Glow: the floor a waveland or wavedash slides along lights up
// in the stage, at Falcon's depth. A faint glow marks the slide while the
// timer counts down and fills in from its ends toward the landing spot,
// growing brighter and taller, steeply at the end; on each frame of the
// window the whole slide flares white-hot (hottest on the first and last).
// Only the directions that work light up.
// A glow standing on the floor from xa to xb, just behind Falcon's middle:
// a bright line along the surface and light falling off fast above it.
#define GLOW_Z -1.f

static void Glow_Span(float xa, float xb, float y, float h, GXColor c)
{
    if (xb - xa < 0.05f || c.a == 0)
        return;
    GXColor mid = Color_Fill(c, 0.35f);
    GXColor top = {0, 0, 0, 0};
    float ym = y + h * 0.3f;
    World_Start(8, GX_QUADS, 0);
    World_Vtx(xa, y, GLOW_Z, c);
    World_Vtx(xb, y, GLOW_Z, c);
    World_Vtx(xb, ym, GLOW_Z, mid);
    World_Vtx(xa, ym, GLOW_Z, mid);
    World_Vtx(xa, ym, GLOW_Z, mid);
    World_Vtx(xb, ym, GLOW_Z, mid);
    World_Vtx(xb, y + h, GLOW_Z, top);
    World_Vtx(xa, y + h, GLOW_Z, top);
    GXColor line = Color_Mix(c, Color_Fill(color_white, c.a / 255.f), 0.3f);
    World_Start(2, GX_LINES, 42);
    World_Vtx(xa, y + 0.1f, GLOW_Z, line);
    World_Vtx(xb, y + 0.1f, GLOW_Z, line);
}

// How the glow ends: a hit bursts white-cyan and rises off the floor, a
// miss sinks in violet toward the spot, and a skipped or interrupted one
// just goes out. All of it is over in a few frames.
#define GLOW_HIT 10
#define GLOW_MISS 10
#define GLOW_SKIP 5
static const GXColor color_glow_miss = {150, 80, 255, 255};

static void Plat_Glow(FighterData *fp)
{
    if (!Options_Cues[COPT_GLOW].val || !Cues_Waveland())
        return;
    GXColor base = Cue_Color(CUE_WL);
    float reach = Slide_Distance(fp, common_dodge_force);
    for (int pass = 0; pass < 2; pass++)
    {
        Cue *c = pass ? &cue_live[CUE_WL] : &cue_end[CUE_WL];
        if (!c->phase)
            continue;
        float m = c->spot.X, y = c->spot.Y;
        float L = m - reach < c->x0 ? c->x0 : m - reach;
        float R = m + reach > c->x1 ? c->x1 : m + reach;
        int ok_l = c->wd || (c->dirs & DODGE_LEFT);
        int ok_r = c->wd || (c->dirs & DODGE_RIGHT);

        if (pass && c->phase == PH_COUNT && !c->dim)
        {
            // builds up: fills in from the ends toward the spot, brighter
            // and taller the closer the press, steeply at the end
            int k = c->left - 1;
            float p = 1.f - (c->span > 1 ? Clamp01((float)k / (c->span - 1)) : 0);
            float p2 = p * p;
            GXColor faint = Color_Fill(base, 0.1f + 0.1f * p);
            GXColor lit = Color_Fill(Color_Mix(base, color_white, 0.2f * p2), 0.2f + 0.6f * p2);
            float h = 0.9f + 2.4f * p2;
            if (ok_l)
            {
                Glow_Span(L, m, y, 0.8f, faint);
                Glow_Span(L, L + (m - L) * p, y, h, lit);
            }
            if (ok_r)
            {
                Glow_Span(m, R, y, 0.8f, faint);
                Glow_Span(R - (R - m) * p, R, y, h, lit);
            }
        }
        else if (pass && c->phase == PH_WINDOW && !c->dim)
        {
            // the window: the whole slide flares white-hot, the first and
            // last frames hottest, with a tall haze over it
            int edge = c->age == 0 || c->age == c->width - 1;
            GXColor core = Color_Fill(Color_Mix(base, color_white, edge ? 0.7f : 0.45f), edge ? 1.f : 0.85f);
            GXColor haze = Color_Fill(base, edge ? 0.45f : 0.3f);
            float h = edge ? 4.5f : 3.5f;
            if (ok_l)
            {
                Glow_Span(L, m, y, h * 2.2f, haze);
                Glow_Span(L, m, y, h, core);
            }
            if (ok_r)
            {
                Glow_Span(m, R, y, h * 2.2f, haze);
                Glow_Span(m, R, y, h, core);
            }
        }
        else if (!pass && c->phase == PH_HIT && c->age < GLOW_HIT)
        {
            float q = (float)c->age / GLOW_HIT;
            float a = (1.f - q) * (1.f - q) * (c->soft ? 0.55f : 1.f);
            GXColor flash = Color_Fill(Color_Mix(base, color_white, 0.7f * (1.f - q)), a);
            Glow_Span(L, R, y, 3.f + 5.f * Ease_Out(q), flash);
        }
        else if (!pass && c->phase == PH_FADE && c->dim == DIM_MISS && c->age < GLOW_MISS)
        {
            // folds in toward the spot
            float q = (float)c->age / GLOW_MISS;
            float a = 0.7f * (1.f - q);
            float l = L + (m - L) * Ease_Out(q), r = R - (R - m) * Ease_Out(q);
            Glow_Span(l, r, y, 2.2f * (1.f - q) + 0.4f, Color_Fill(color_glow_miss, a));
        }
        else if (!pass && (c->phase == PH_FADE || c->phase == PH_CUT) && c->age < GLOW_SKIP)
        {
            float q = (float)c->age / GLOW_SKIP;
            Glow_Span(L, R, y, 0.8f, Color_Fill(base, 0.12f * (1.f - q)));
        }
    }
}

static void World_GX(GOBJ *gobj, int pass)
{
    if (pass != 2)
        return;

    FighterData *fp = Fighter_GetGObj(0)->userdata;
    compass_num = 0;
    if (Log_Level() >= LOG_ALL)
        Log_Camera(fp);
    if (capture_clean || hide_all)
        return;
    world_on_top = 1;
    vis_k = 1.f;
    if (Options_Dev[DOPT_COLL].val)
        Draw_CurrentEcb(fp);
    vis_k = Group_K(VG_PATHS);
    // a ledge route stays on top all the way down
    world_on_top = route_active;

    if (live_visible && route_active && !route_dj_done)
        ; // the route's own path shows where it goes
    else if (live_visible)
        Draw_Prediction(pred_live, 1, LINE_SOLID);
    else if (ghost_visible)
    {
        Draw_Prediction(pred_seg, 0, LINE_SOLID);
        if (Options_Paths[POPT_PATH].val)
            Draw_Path(actual_pos, actual_bottom, 0, actual_num - 1, color_actual, 12);
    }
    else
    {
        // the short hop is dashed, and only has a body line of its own
        // when the full hop isn't shown
        if (preview_fh)
            Draw_Prediction(pred_fh, 1, LINE_SOLID);
        if (preview_sh)
            Draw_Prediction(pred_sh, !preview_fh, LINE_DASHED);
    }
    world_on_top = 0;
    world_add = 1;
    vis_k = Kind_K(VG_CUES, CUE_WL);
    Plat_Glow(fp);
    world_add = 0;
    world_on_top = 1;
#ifdef LL_GROUND_GUIDE
    Guide_Draw(fp); // sets each kind's own level
#endif
    vis_k = Group_K(VG_LEDGE);
    vis_k = Group_K(VG_LEDGE);
    Draw_RoutePath();
    Draw_JumpPath();
    // along the floor, where the depth test can't tell the line from it
    vis_k = Group_K(VG_PATHS);
    Draw_SlideOff();
    world_on_top = 0;
    vis_k = 1.f;
}

// The name picker's grid (Naming above), over everything else.
#define NAMER_CELL_W 3.2f
#define NAMER_CELL_H 3.0f
#define NAMER_TOP 4.6f
static void Namer_Key(int r, int c, float *x0, float *y0, float *x1, float *y1)
{
    float left = -NAMER_COLS * NAMER_CELL_W / 2;
    float w = r == NAMER_ROWS - 1 ? NAMER_COLS * NAMER_CELL_W / NAMER_KEYS : NAMER_CELL_W;
    *x0 = left + c * w + 0.15f;
    *x1 = left + (c + 1) * w - 0.15f;
    *y1 = NAMER_TOP - r * NAMER_CELL_H;
    *y0 = *y1 - NAMER_CELL_H + 0.3f;
}

// The quick menu, Preset Deck (Deck_* below): up while L or R is held with
// the game frozen.
static u8 deck_on;
static u8 deck_frozen; // ... and the game frozen under it (not while it plays out)
static void Deck_Draw(void);
static void Deck_Open(void);

static void Namer_Draw(void)
{
    static const char *keys[NAMER_KEYS] = {"Space", "Delete", 0, "Done"};
    GXColor ink = {235, 235, 235, 255}, dark = {10, 12, 24, 255}, lit = {120, 150, 255, 255};
    Hud_Rect(-40.f, -30.f, 40.f, 30.f, Color_Over(color_plate, 0.6f));
    float px = NAMER_COLS * NAMER_CELL_W / 2 + 1.2f;
    Hud_Rect(-px, -13.6f, px, 13.f, Color_Over(color_plate, 0.92f));
    Hud_Frame(-px, -13.6f, px, 13.f, 0.12f, Color_Over(lit, 0.6f));
    Hud_TextAligned(namer.title, -px + 1.f, 10.f, 0.5f, ink, 0);
    // the name so far, with the cursor after it
    Hud_Rect(-px + 1.f, 6.6f, px - 1.f, 9.4f, Color_Over(color_white, 0.08f));
    Hud_Rect(-px + 1.f, 6.6f, px - 1.f, 6.75f, Color_Over(lit, 0.8f));
    char line[NAME_LEN + 2];
    sprintf(line, "%s_", namer.buf);
    Hud_TextAligned(line, -px + 1.6f, 6.75f, 0.6f, color_white, 0);

    for (int r = 0; r < NAMER_ROWS; r++)
    {
        int cols = r == NAMER_ROWS - 1 ? NAMER_KEYS : NAMER_COLS;
        for (int c = 0; c < cols; c++)
        {
            float x0, y0, x1, y1;
            Namer_Key(r, c, &x0, &y0, &x1, &y1);
            int on = r == namer.cy && c == namer.cx;
            Hud_Rect(x0, y0, x1, y1, on ? Color_Over(lit, 1.f) : Color_Over(color_white, 0.1f));
            if (on)
                Hud_Frame(x0 - 0.1f, y0 - 0.1f, x1 + 0.1f, y1 + 0.1f, 0.15f, color_white);
            if (!namer.dirty || !namer.text)
                continue;
            int i = r * NAMER_COLS + c;
            char glyph[2] = {0, 0};
            const char *label;
            if (r < NAMER_ROWS - 1)
            {
                glyph[0] = namer_rows[namer.lower][r][c];
                label = glyph;
            }
            else
                label = keys[c] ? keys[c] : namer.lower ? "ABC" : "abc";
            float size = r < NAMER_ROWS - 1 ? 0.55f : 0.45f;
            // as Hud_TextAligned places a row 2.5 tall whose bottom is y
            float y = (y0 + y1) / 2 - 1.25f;
            Text_SetText(namer.text, i, label);
            Text_SetScale(namer.text, i, size, size);
            Text_SetPosition(namer.text, i, (x0 + x1) / 2 * 10.f, y * -10.f - 37.5f);
            Text_SetColor(namer.text, i, on ? &dark : &ink);
        }
    }
    namer.dirty = 0;
    Hud_TextAligned("A type   B delete   Y space   X case   Start done", -px + 1.f, -12.9f, 0.4f,
                    (GXColor){180, 185, 200, 255}, 0);
}

static void Hud_GX(GOBJ *gobj, int pass)
{
    if (pass != 2 || capture_clean)
        return;
    HUDCamData *hud = event_vars->hudcam_gobj->userdata;
    if (hud->hide)
        return;

    FighterData *fp = Fighter_GetGObj(0)->userdata;
    COBJ *prev = COBJ_GetCurrent();
    CObj_SetCurrent(event_vars->hudcam_gobj->hsd_object);
    quad_num = 0;
    if (namer.on)
    {
        Namer_Draw();
        Quad_Flush();
        CObj_SetCurrent(prev);
        return;
    }
    if (hide_all)
    {
        Panel_Draw(); // the toast that says so
        // a quiet reminder that it's only hidden, and how to bring it back
        Hud_TextAligned("Effects hidden  (L+R + D-pad up)", -SAFE_W, SAFE_H - 2.5f, 0.3f,
                        (GXColor){200, 205, 220, 110}, 0);
        if (deck_on)
            Deck_Draw();
        Quad_Flush();
        CObj_SetCurrent(prev);
        return;
    }
    Meter_Build();
    float strip_k = Group_K(route_rows_active ? VG_LEDGE : VG_TIMERS);
    vis_k = strip_k;
    Spot_Draw(fp);
    int near = Options_Timers[TOPT_NEAR].val;
    int strip = Options_Timers[TOPT_STRIP].val;
    if (meter_rows == 0)
        Pin_Idle();
    else if (near == NEAR_STRIP)
        Meter_Above(fp);
    if (near != NEAR_OFF && near != NEAR_STRIP)
        Near_Draw(fp, near);
    Wd_Draw(fp);
    int route_marks = (hang_ledge >= 0 && route_show_num > 0) || (route_active && !route_dj_done) || Jump_Showing();
    vis_k = Group_K(route_marks ? VG_LEDGE : VG_PATHS);
    Markers_Draw();
    vis_k = Group_K(VG_LEDGE);
    Assist_Draw();
    vis_k = Group_K(VG_PATHS);
    Compass_Draw();
    // the fixed strip over what's placed in the world, so an input chip or
    // a spot's rails never cross its belt (0.9.13 test: an airdodge chip
    // lay over it). The conveyor runs with nothing on it too, its belt
    // always moving (Stephen, 0.9.10)
    vis_k = strip_k;
    if (strip != STRIP_OFF && (meter_rows > 0 || strip == STRIP_CONVEYOR))
        Meter_Fixed(fp);
    vis_k = Group_K(VG_PAD);
    Pad_Draw(fp);
    vis_k = 1.f;
    Panel_Draw();
    if (deck_on)
        Deck_Draw();
    if (quad_num > quad_peak)
        quad_peak = quad_num;
    Quad_Flush();

    CObj_SetCurrent(prev);
}
