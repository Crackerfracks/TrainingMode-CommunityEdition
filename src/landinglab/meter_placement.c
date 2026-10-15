/* landinglab/meter_placement.c
 *
 * Holds where the meter goes: pinned near Falcon clear of his path, or a
 * fixed strip.
 *
 * Part of Landing Gear: src/landinglab.c includes it, in order, into one
 * translation unit, so it sees everything included before it (and mex.h,
 * events.h, landinglab_sim.h).
 *
 * Uses: menus_display, tracking_state, rumble, hud_text, drawing,
 * hud_drawing, meter.
 * Refers ahead, through forward declarations, to: spot_timers, controller.
 */

// Near Falcon: the meter is pinned to one spot on the screen when it shows
// up, so the eye doesn't have to chase it: over his head if his path stays
// below it, else above the whole path, or beside it. It moves only when the
// path, the controller display, the info panel or the fixed strip would run
// into it, and then glides to the next clear spot. Between two timers close
// together it stays where it was.
typedef struct Box
{
    float x0, y0, x1, y1;
} Box;

// the ledge route's path (Ledge Practice, defined below)
static Vec2 *route_path;
static float *route_path_bottom;
static int route_path_num;
static int hang_ledge;
static int route_show_num;

#define PIN_BOXES 24
#define PIN_HOLD 30 // frames the spot is kept after the meter goes away
static Box pin_path[PIN_BOXES]; // Falcon now and along his path, on the screen
static int pin_path_num;
static float pin_x, pin_y;      // where the meter is: its gate's left and its bottom
static float pin_tx, pin_ty;    // where it's going
static int pin_valid;
static int pin_idle;            // frames without a meter

static int Box_Hit(Box *a, Box *b)
{
    return a->x0 < b->x1 && b->x0 < a->x1 && a->y0 < b->y1 && b->y0 < a->y1;
}

// TM-CE's version text in the bottom right corner (TM_CreateWatermark:
// right edge at x 615, top at y 446 of the 640 x 480 picture). Displays
// that would cover it go up above it.
static Box Wm_Box(void)
{
    return (Box){(470 - 320) * PX, -SAFE_H - 1.f, (620 - 320) * PX, -(443 - 240) * PX};
}

// Falcon's body from y0 to y1 at x, as a box on the screen, with a margin.
static void Pin_AddBody(float x, float y0, float y1)
{
    if (pin_path_num >= PIN_BOXES)
        return;
    float ax, ay, bx, by;
    if (!Hud_FromWorld(x - 5.f, y0, &ax, &ay) || !Hud_FromWorld(x + 5.f, y1, &bx, &by))
        return;
    Box *b = &pin_path[pin_path_num++];
    b->x0 = (ax < bx ? ax : bx) - 0.4f;
    b->x1 = (ax < bx ? bx : ax) + 0.4f;
    b->y0 = (ay < by ? ay : by) - 0.4f;
    b->y1 = (ay < by ? by : ay) + 0.4f;
}

static void Pin_Path(FighterData *fp)
{
    pin_path_num = 0;
    Pin_AddBody(fp->phys.pos.X, fp->phys.pos.Y - 1.f, fp->phys.pos.Y + 20.f);
    if (live_visible)
    {
        Prediction *p = pred_live;
        int last = p->land_frame ? p->land_frame : p->num;
        int step = last / (PIN_BOXES - 2) + 1;
        for (int k = 1; k <= last; k += step)
            Pin_AddBody(p->pos[k].X, p->pos[k].Y + p->bottom[k], p->pos[k].Y + p->top[k] + 2.f);
        if (last > 0)
            Pin_AddBody(p->pos[last].X, p->pos[last].Y + p->bottom[last], p->pos[last].Y + p->top[last] + 2.f);
    }
    else if (route_path_num >= 2 && ((hang_ledge >= 0 && route_show_num > 0) || route_active))
    {
        int step = route_path_num / (PIN_BOXES - 2) + 1;
        for (int k = 0; k < route_path_num; k += step)
            Pin_AddBody(route_path[k].X, route_path[k].Y + route_path_bottom[k], route_path[k].Y + route_path_bottom[k] + 18.f);
    }
}

static void Pad_Box(FighterData *fp, float *x0, float *y0, float *x1, float *y1); // with the controller display
static float Meter_Pitch(void);

// The fixed strip's other looks sit in a bottom corner, the one away from
// the controller display. The highway is a lane a row, HW_ROWS frames tall
// with the hit line at the bottom; the dial is a small dial a row, side by
// side. Both leave room under for the row's label, and the first lane for
// the glyphs of a route's inputs.
#define HW_ROWS 24       // frames of lead a lane shows (18 was a little short, Stephen 0.9.19)
#define HW_PITCH_BASE 0.68f   // one frame's height
#define CV_ROWS 36       // ... the conveyor's: twice (18 was way too short for a preloaded wavedash, Stephen 0.9.19)
#define CV_PITCH_BASE 0.46f   // ... in about a third more height than it had
// Wide Cells stretches the highway's and conveyor's frames as it does the
// cells' (Stephen, 0.9.29: it should work on every timer)
#define WIDE_K (Options_Timers[TOPT_WIDE].val ? (MT_PITCH + MT_WIDE) / MT_PITCH : 1.f)
#define HW_PITCH (HW_PITCH_BASE * WIDE_K)
#define CV_PITCH (CV_PITCH_BASE * WIDE_K)
#define HW_LANE 2.1f
#define HW_LANE_GAP 0.5f
#define HW_GLYPH 1.5f
#define HW_LABEL 1.7f
#define DL_R 1.55f       // a dial's radius
#define DL_CELL 4.2f     // the room one takes, side to side
#define DL_LABEL 1.7f
#define STRIP_PAD 0.35f
#define CV_KEY 4.9f // the conveyor key's column: a swatch, then "JUMP" or "WAVE" (about 3.5 wide at ST_TEXT in game, more than Text_Width says), clear of the rail studs

static float hw_pitch = HW_PITCH_BASE; // a frame's height in the window the highway and conveyor share

static int Strip_Glyphs(void)
{
    for (int i = 0; i < meter_rows; i++)
        for (int k = 0; k < meter[i].len && k < CV_ROWS; k++)
            if (meter[i].glyph[k])
                return 1;
    return 0;
}

// The fade the strip's plate takes: its brightest row's.
static float Strip_PlateK(void)
{
    float k = 0;
    for (int i = 0; i < meter_rows; i++)
        if (!meter[i].gap && meter[i].fade > k)
            k = meter[i].fade;
    return k;
}

// Where the highway or the dial is: its left, bottom, width and height.
static void Strip_Place(FighterData *fp, int look, float *x, float *y, float *w, float *h)
{
    int n = meter_rows > 0 ? meter_rows : 1;
    if (look == STRIP_HIGHWAY)
    {
        *w = 2 * STRIP_PAD + (Strip_Glyphs() ? HW_GLYPH : 0) + n * HW_LANE + (n - 1) * HW_LANE_GAP;
        *h = HW_LABEL + HW_ROWS * HW_PITCH + 0.5f;
    }
    else if (look == STRIP_CONVEYOR)
    {
        // a fixed width, however many cues are up (Stephen, 0.9.10)
        *w = 2 * STRIP_PAD + (Strip_Glyphs() ? HW_GLYPH : 0) + 2 * HW_LANE + HW_LANE_GAP + CV_KEY;
        *h = 0.8f + CV_ROWS * CV_PITCH + 0.5f + 1.3f; // and the entry's waiting room
    }
    else
    {
        *w = 2 * STRIP_PAD + n * DL_CELL - 0.4f;
        *h = DL_LABEL + 2 * DL_R + 0.9f;
    }
    float x0, y0, x1, y1;
    Pad_Box(fp, &x0, &y0, &x1, &y1);
    *x = x0 + x1 < 0 ? SAFE_W - *w : -SAFE_W;
    *y = -SAFE_H + 0.4f;
    Box b = {*x, *y, *x + *w, *y + *h}, wm = Wm_Box();
    if (Box_Hit(&b, &wm))
        *y = wm.y1 + 0.2f;
}

// The cell strip's left and bottom: above the version text when its
// longest rows would reach it, so it doesn't move as rows grow.
static void Strip_CellsAt(FighterData *fp, float *gx, float *base)
{
    float x0, y0, x1, y1;
    Pad_Box(fp, &x0, &y0, &x1, &y1);
    // room on the left for the longest label ("Route", "JUMP"), which ends
    // clear of the gate's lit ticks (Stephen, 0.9.8: JUMP ran into them)
    *gx = x0 + x1 < 0 ? 3.6f : -SAFE_W + 0.3f + MT_LABEL_W + MT_LABEL_GAP * MT_FIXED;
    *base = -SAFE_H + 0.4f;
    float far = *gx + (MT_FIXED_MAX - 1) * Meter_Pitch() * MT_FIXED + (MT_CW + MT_WIDE) * MT_FIXED + 0.3f;
    Box b = {*gx, *base, far, *base + 2.f}, wm = Wm_Box();
    if (Box_Hit(&b, &wm))
        *base = wm.y1 + 0.4f;
}

// The fixed strip's box, so the near-Falcon strip keeps out of it.
static Box Strip_Box(FighterData *fp)
{
    int look = Options_Timers[TOPT_STRIP].val;
    if (look == STRIP_HIGHWAY || look == STRIP_DIAL || look == STRIP_CONVEYOR)
    {
        float x, y, w, h;
        Strip_Place(fp, look, &x, &y, &w, &h);
        return (Box){x - 0.2f, -SAFE_H, x + w + 0.2f, y + h + 0.3f};
    }
    float gx, base;
    Strip_CellsAt(fp, &gx, &base);
    return (Box){-SAFE_W, -SAFE_H, SAFE_W, base + meter_rows * (MT_CH + MT_GAP) * MT_FIXED + 0.6f};
}

// Whether the meter at (gx, base), w by h, is clear of everything else.
static int Pin_Clear(FighterData *fp, float gx, float base, float w, float h)
{
    Box m = {gx - 0.4f, base - 0.3f, gx + w + 0.3f, base + h};
    if (m.x0 < -SAFE_W - 0.01f || m.x1 > SAFE_W + 0.01f || m.y0 < -SAFE_H - 0.01f || m.y1 > SAFE_H + 0.01f)
        return 0;
    for (int i = 0; i < pin_path_num; i++)
    {
        if (Box_Hit(&m, &pin_path[i]))
            return 0;
    }
    if (Options_Hud[HOPT_STICK].val != STICK_OFF)
    {
        Box pad;
        Pad_Box(fp, &pad.x0, &pad.y0, &pad.x1, &pad.y1);
        if (Box_Hit(&m, &pad))
            return 0;
    }
    if (Options_Hud[HOPT_PANEL].val)
    {
        // the info panel's lines in its top corner
        float low = SAFE_H - (Options_Game[GOPT_FRAME_ADV].val ? 10.5f : 8.f); // the frame line under it
        Box panel = {panel_left ? -SAFE_W : SAFE_W - 22.f, low, panel_left ? -SAFE_W + 22.f : SAFE_W, SAFE_H};
        if (Box_Hit(&m, &panel))
            return 0;
    }
    if (Options_Timers[TOPT_STRIP].val != STRIP_OFF)
    {
        // the fixed strip along the bottom, or in its corner
        Box strip = Strip_Box(fp);
        if (Box_Hit(&m, &strip))
            return 0;
    }
    return 1;
}

static void Pin_Clamp(float *gx, float *base, float w, float h)
{
    if (*gx > SAFE_W - w - 0.3f)
        *gx = SAFE_W - w - 0.3f;
    if (*gx < -SAFE_W + 0.4f)
        *gx = -SAFE_W + 0.4f;
    if (*base > SAFE_H - h)
        *base = SAFE_H - h;
    if (*base < -SAFE_H + 0.3f)
        *base = -SAFE_H + 0.3f;
}

// The first clear spot: over his head, over the whole path, then beside
// it on either side, nearest first. None clear: over the path anyway.
static void Pin_Choose(FighterData *fp, float head_x, float head_y, float cw, float w, float h, float *gx, float *base)
{
    Box u = pin_path[0];
    for (int i = 1; i < pin_path_num; i++)
    {
        Box *b = &pin_path[i];
        u.x0 = b->x0 < u.x0 ? b->x0 : u.x0;
        u.y0 = b->y0 < u.y0 ? b->y0 : u.y0;
        u.x1 = b->x1 > u.x1 ? b->x1 : u.x1;
        u.y1 = b->y1 > u.y1 ? b->y1 : u.y1;
    }
    float over = pin_path_num > 0 ? pin_path[0].y1 + 0.1f : head_y + 0.6f; // just over his head now
    float cand[6][2] = {
        {head_x - cw / 2, over},
        {head_x - cw / 2, u.y1 + 0.3f},
        {u.x0 - w - 0.6f, head_y - h / 2},
        {u.x1 + 0.8f, head_y - h / 2},
        {u.x0 - w - 0.6f, u.y1 - h},
        {u.x1 + 0.8f, u.y1 - h},
    };
    // the side nearer the middle of the screen first
    if (head_x < 0)
    {
        for (int i = 2; i < 6; i += 2)
        {
            float tx = cand[i][0], ty = cand[i][1];
            cand[i][0] = cand[i + 1][0];
            cand[i][1] = cand[i + 1][1];
            cand[i + 1][0] = tx;
            cand[i + 1][1] = ty;
        }
    }
    for (int i = 0; i < 6; i++)
    {
        float x = cand[i][0], y = cand[i][1];
        Pin_Clamp(&x, &y, w, h);
        if (Pin_Clear(fp, x, y, w, h))
        {
            *gx = x;
            *base = y;
            return;
        }
    }
    *gx = cand[1][0];
    *base = cand[1][1];
    Pin_Clamp(gx, base, w, h);
}

// Each frame without a meter.
static void Pin_Idle(void)
{
    if (pin_valid && ++pin_idle > PIN_HOLD)
        pin_valid = 0;
}

static void Meter_Above(FighterData *fp)
{
    float head_x, head_y;
    Hud_FromWorld(fp->phys.pos.X, fp->phys.pos.Y + 18.f, &head_x, &head_y);

    int wide = Options_Timers[TOPT_WIDE].val;
    float pitch = MT_PITCH + (wide ? MT_WIDE : 0);
    float cw = MT_CW + (wide ? MT_WIDE : 0);
    int len = 1;
    for (int i = 0; i < meter_rows; i++)
        if (meter[i].len > len)
            len = meter[i].len;
    if (len > 24)
        len = 24;
    float w = len * pitch, h = meter_rows * (MT_CH + MT_GAP) + 1.5f;

    Pin_Path(fp);
    pin_idle = 0;
    if (!pin_valid)
    {
        Pin_Choose(fp, head_x, head_y, cw, w, h, &pin_tx, &pin_ty);
        pin_x = pin_tx;
        pin_y = pin_ty;
        pin_valid = 1;
    }
    else
    {
        float x = pin_tx, y = pin_ty;
        Pin_Clamp(&x, &y, w, h);
        if (!Pin_Clear(fp, x, y, w, h))
            Pin_Choose(fp, head_x, head_y, cw, w, h, &pin_tx, &pin_ty);
        else
        {
            pin_tx = x;
            pin_ty = y;
        }
    }
    // glide to it
    pin_x += (pin_tx - pin_x) * 0.3f;
    pin_y += (pin_ty - pin_y) * 0.3f;
    Meter_Draw(pin_x, pin_y, 1.f, len, 0);
}

// A row's label centered at cx, its line's middle at y, made smaller to fit
// in room (so a long one can't run into its neighbor's).
static void Strip_Label(MeterRow *r, float cx, float y, float room)
{
    GXColor tc = Color_Mix(r->color, color_white, 0.35f);
    if (r->dim)
        tc = Color_Fill(tc, 0.6f);
    tc.a = 255;
    // one size for every label, centered by the game on its lane (Stephen,
    // 0.9.9: JUMP came out smaller and off its lane when it was shrunk to fit)
    float w = Text_Width(r->label, ST_TEXT);
    Text_PlateAt(cx - w / 2 - 0.2f, cx + w / 2 + 0.2f, y, 0.85f);
    Hud_TextAligned(r->label, cx, Text_MidY(y, ST_TEXT), ST_TEXT, tc, 1);
}
