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
 * Refers ahead, through forward declarations, to: conveyor, spot_timers,
 * controller.
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
// 0: off the screen.
static int Body_Box(float x, float y0, float y1, Box *b)
{
    float ax, ay, bx, by;
    if (!Hud_FromWorld(x - 5.f, y0, &ax, &ay) || !Hud_FromWorld(x + 5.f, y1, &bx, &by))
        return 0;
    b->x0 = (ax < bx ? ax : bx) - 0.4f;
    b->x1 = (ax < bx ? bx : ax) + 0.4f;
    b->y0 = (ay < by ? ay : by) - 0.4f;
    b->y1 = (ay < by ? by : ay) + 0.4f;
    return 1;
}

static void Pin_AddBody(float x, float y0, float y1)
{
    if (pin_path_num < PIN_BOXES && Body_Box(x, y0, y1, &pin_path[pin_path_num]))
        pin_path_num++;
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
static void Cv_Note(MeterRow *r, int kind, int tone, float a, float x0, float y0, float x1, float y1, int open_lo,
                    int open_hi); // with the conveyor

// The fixed strip's other looks sit in a bottom corner, the one away from
// the controller display. The highway is a lane a slot, HW_ROWS frames tall
// with the hit line at the bottom; the dial is a small dial a slot, side by
// side. Both leave room under for the slot's swatch, and the highway and
// conveyor a column for the glyphs of a route's inputs. All of it a fixed
// size, so nothing in it moves (visual audit, 2026-10-15: the highway was
// 135 to 180 px wide, the dials 3 or 4).
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
#define HW_LANES (MT_SLOTS_SHOWN * HW_LANE + (MT_SLOTS_SHOWN - 1) * HW_LANE_GAP)
#define HW_OTHER (HW_LANE / 2) // the rest's lane, half wide, past the glyphs' column while it's used
#define HW_OTHER_GAP 0.15f
#define HW_GLYPH 1.5f
#define HW_LABEL 1.7f
#define DL_R 1.7f        // a dial's radius (1.55 was too small to read the face)
#define DL_CELL 4.0f     // the room one takes, side to side: its face plate and the clock's ticks
#define DL_LABEL 1.7f
#define STRIP_PAD 0.35f
#define CV_W (2 * HW_LANE + HW_LANE_GAP) // the conveyor's belt
#define CV_BASE 1.5f // under the window: the key's swatches
#define SLOT_DIM 0.6f // an empty slot's swatch (vis_k squares it: about a third as strong)

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

// Where the highway, conveyor or dial is: its left, bottom, width and
// height. The four slots always; the rest's only widens it toward the
// screen's middle, so the others stay put.
static void Strip_Place(FighterData *fp, int look, float *x, float *y, float *w, float *h)
{
    int other = Slot_Count() > MT_SLOTS_SHOWN;
    if (look == STRIP_HIGHWAY)
    {
        *w = 2 * STRIP_PAD + HW_GLYPH + HW_LANES + (other ? HW_OTHER_GAP + HW_OTHER : 0);
        *h = HW_LABEL + HW_ROWS * HW_PITCH + 0.5f;
    }
    else if (look == STRIP_CONVEYOR)
    {
        // a fixed width, however many cues are up (Stephen, 0.9.10), the
        // glyphs' column kept whether or not there are any, and the key in
        // swatches under the belt (visual audit, 2026-10-15: its words made
        // it 108 to 125 px wide and ran into the belt)
        *w = 2 * STRIP_PAD + HW_GLYPH + CV_W;
        *h = CV_BASE + CV_ROWS * CV_PITCH + 0.5f + 1.3f; // and the entry's waiting room
    }
    else
    {
        *w = 2 * STRIP_PAD + (other ? MT_SLOTS : MT_SLOTS_SHOWN) * DL_CELL;
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

// The players' percent displays, as the controller display keeps clear of
// them (the same as controller.c's).
#define PCT_HALF_W 6.4f  // half a percent display's width, "%" and stock icon included
#define PCT_TOP 7.f      // its top above its HUD position

// The left edge of the nearest percent display right of x that reaches up
// past y, or the screen's edge.
static float Pct_Left(float x, float y)
{
    float left = SAFE_W;
    for (int i = 0; i < 4; i++)
    {
        Vec3 *o = Fighter_GetSlotType(i) != 3 ? Match_GetPlayerHUDPos(i) : 0;
        if (o && o->X - PCT_HALF_W > x && o->Y + PCT_TOP > y && o->X - PCT_HALF_W < left)
            left = o->X - PCT_HALF_W;
    }
    return left;
}

// The most cells the cell strip shows from gx: MT_FIXED_MAX, or fewer to
// end short of the next percent display (visual audit, 2026-10-15: a long
// row ran into the stock icon and "0%").
static int Strip_CellsMax(float gx, float base)
{
    float cw = (MT_CW + (Options_Timers[TOPT_WIDE].val ? MT_WIDE : 0)) * MT_FIXED;
    int n = (int)((Pct_Left(gx, base) - 0.5f - gx - cw) / (Meter_Pitch() * MT_FIXED)) + 1;
    return n > MT_FIXED_MAX ? MT_FIXED_MAX : n < 6 ? 6 : n;
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
    return (Box){-SAFE_W, -SAFE_H, SAFE_W, base + Slot_Shown() * (MT_CH + MT_GAP) * MT_FIXED + 0.6f};
}

// The highway's and conveyor's plate and belt fade back while Falcon is
// over them, easing a sixth of the way a frame, so his legs show through
// and the notes, window and endings still read (visual audit, 2026-10-15:
// standing in the corner, the plate hid his legs).
#define STRIP_DIM_OVER 0.35f
static float strip_dim = 1.f;

static float Strip_Dim(FighterData *fp)
{
    Box body, strip = Strip_Box(fp);
    int over = Body_Box(fp->phys.pos.X, fp->phys.pos.Y - 1.f, fp->phys.pos.Y + 20.f, &body) && Box_Hit(&body, &strip);
    strip_dim += ((over ? STRIP_DIM_OVER : 1.f) - strip_dim) / 6.f;
    return strip_dim;
}

// The key's swatch for slot s, in place of words under a lane (visual
// audit, 2026-10-15: "JUMPWL" ran together): its kind's note in its shape
// and color, a jump's tab, an AI solid, a NIL notched, a waveland hollow,
// w by h at (cx, cy), lit (k 1) or dim (SLOT_DIM). The rest's slot shows
// its row's own (r).
static void Slot_Swatch(int s, MeterRow *r, float cx, float cy, float w, float h, float k)
{
    MeterRow sw = {0};
    sw.color = Slot_Color(s);
    strcpy(sw.label, s == SLOT_WAVE ? "WL" : slot_labels[s]);
    if (s == SLOT_OTHER && r)
        sw = *r;
    float base_k = vis_k;
    vis_k = base_k * k;
    Cv_Note(&sw, CELL_PRESS, TONE_CUE, sw.dim ? 0.6f : 1.f, cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2, 0, 0);
    vis_k = base_k;
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
    float w = len * pitch, h = Slot_Count() * (MT_CH + MT_GAP) + 1.5f;

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
