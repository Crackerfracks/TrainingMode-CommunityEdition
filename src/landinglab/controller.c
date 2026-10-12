/* landinglab/controller.c
 *
 * Holds the controller display: pad input and trails, the Classic look's
 * gate, sticks and buttons.
 *
 * Part of Landing Lab: src/landinglab.c includes it, in order, into one
 * translation unit, so it sees everything included before it (and mex.h,
 * events.h, landinglab_sim.h).
 *
 * Uses: option_values, menus_display, drawing, hud_drawing, meter,
 * meter_placement.
 */

///////////////////////
/// Controller      ///
///////////////////////

// A compact picture of the whole controller, from the raw pad (what the
// player's hands did, before Melee's deadzones): the stick with its
// deadzone cross, trail and fastfall line, the C-stick, the face buttons
// and both triggers. A button stays lit for a few frames after it's let go,
// so a one-frame tap can still be seen.
//
// Laid out from the block's bottom left corner, in HUD units: 12.7 wide and
// 5.3 tall, or just the stick (5.2 square) with Buttons and Triggers off.
#define PAD_W 12.7f
#define PAD_H 5.3f
#define PAD_STICK_W 5.2f
#define PAD_STICK_H 5.2f
#define PAD_STICK_X 3.5f // the stick's center in the full block
#define PAD_R 2.6f       // the stick gate's radius
#define PCT_HALF_W 6.4f  // half a percent display's width, "%" and stock icon included
#define PCT_TOP 7.f      // its top above its HUD position

#define PAD_TRAIL 8
#define PAD_CTRAIL 4

// The Ring look: the whole controller in an ellipse 9.1 by 6.4 HUD units at
// Small, the room TM-CE's own controller model takes (lab.dat). L and R are
// the ellipse's halves, each a band from the gap at the top to the gap at
// the bottom that swells into a deep, pointed nub at the side. A press fills
// both ends of the band to the middle, then floods the nub; a click lights
// the whole half and keeps it lit while held. The stick's gate is in the
// middle with the buttons in the four bays around it, filled in the
// controller's own colors and lettered like the real ones: X and Y top left,
// Z top right, B bottom left, A bottom right, each in the middle of its bay
// (the widest circle that fits there). A press leaves a ghost of the button's
// own fill that grows a little and fades in a few frames. Below the gate the
// C-stick: four arrows for the direction the game reads as a smash or aerial,
// around a small live gate whose trail shows smash DI. Every edge is soft, so
// the small shapes don't look jagged.
#define RING_A 4.55f    // the ellipse's half width
#define RING_B 3.2f     // and half height
#define RING_BAND 0.3f  // the triggers' band, where a light press shows
#define RING_NUB 2.2f   // how far the nub reaches in past the band
#define RING_NUB_W 34.f // the nub's half width, in degrees around the ellipse
#define RING_GAP 7.f    // degrees left open at the top and bottom
#define RING_CH 0.5f    // the share of a trigger's travel that fills the band; the rest floods the nub
#define RING_LIGHT (43.f / 140.f) // a light press, where the game starts counting a trigger
#define RING_LEG_SEGS 4
#define RING_NUB_SEGS 12
#define RING_SEGS (2 * RING_LEG_SEGS + RING_NUB_SEGS)
#define RING_STICK_Y 0.35f // the stick gate's center, above the ellipse's
#define RING_STICK_R 1.45f
#define RING_C_Y -2.16f // the C-stick's center, clear of the gate above it
#define RING_C_R 0.4f

// The Crest look's size, in units of its drawing at size 1 (y up from its
// middle), and how much of a HUD unit that is: about the Ring's height.
#define CREST_K 0.92f
#define CREST_W 4.72f   // half its width, to the top blades' tips
#define CREST_TOP 3.4f  // its top above its middle
#define CREST_BOT 3.72f // its bottom below it

// The inputs that glow.
enum pad_input
{
    PIN_A,
    PIN_B,
    PIN_X,
    PIN_Y,
    PIN_Z,
    PIN_L,
    PIN_R,
    PIN_C, // the C-stick past the aerial threshold

    PIN_COUNT
};

// Where each input was drawn this frame, for the controller cues: its
// middle and about how big it is (r 0: not drawn).
static struct
{
    float x, y, r;
} pin_spot[PIN_COUNT];

static void Pin_Spot(int in, float x, float y, float r)
{
    pin_spot[in].x = x;
    pin_spot[in].y = y;
    pin_spot[in].r = r;
}

static Vec2 pad_trail[PAD_TRAIL];   // the last frames' raw stick, newest at pad_trail_pos
static int pad_trail_pos;
static Vec2 pad_ctrail[PAD_CTRAIL]; // and the C-stick's
static int pad_ctrail_pos;
static float pad_glow[PIN_COUNT];      // 1 while held, then fading
static float pad_glow_prev[PIN_COUNT]; // ... as it was a record ago
static float pad_flash[PIN_COUNT];     // 1 on the record it went down, then fading fast
static float pad_cdir_glow[4];         // the C-stick's arrows: right, up, left, down

static int assist_frozen; // Assist holds the game on the frame before an input

// The GameCube controller's own colors.
static const GXColor color_btn_a = {40, 200, 110, 255};
static const GXColor color_btn_b = {235, 55, 55, 255};
static const GXColor color_btn_xy = {205, 208, 220, 255};
static const GXColor color_btn_z = {125, 95, 255, 255};
static const GXColor color_btn_c = {255, 210, 40, 255};

static int pad_soft; // draw the gates with soft edges (the Ring look)

// Which inputs are down on a pad.
static void Pad_Held(HSD_Pad *pad, int *held)
{
    held[PIN_A] = (pad->held & HSD_BUTTON_A) != 0;
    held[PIN_B] = (pad->held & HSD_BUTTON_B) != 0;
    held[PIN_X] = (pad->held & HSD_BUTTON_X) != 0;
    held[PIN_Y] = (pad->held & HSD_BUTTON_Y) != 0;
    held[PIN_Z] = (pad->held & HSD_TRIGGER_Z) != 0;
    held[PIN_L] = (pad->held & HSD_TRIGGER_L) != 0;
    held[PIN_R] = (pad->held & HSD_TRIGGER_R) != 0;
    held[PIN_C] = fabs(pad->fsubstickX) >= common_aerial_stick_x || fabs(pad->fsubstickY) >= common_aerial_stick_y;
}

// The way the game reads a C-stick at (x, y) for a smash or an aerial: 0
// right, 1 up, 2 left, 3 down, or -1 short of it (the aerial thresholds and
// angle, as Aerial_Pressed uses them).
static int CStick_Dir(float x, float y)
{
    if (fabs(x) < common_aerial_stick_x && fabs(y) < common_aerial_stick_y)
        return -1;
    float angle = atan2(y, fabs(x));
    if (angle > common_aerial_angle)
        return 1;
    if (angle < -common_aerial_angle)
        return 3;
    return x >= 0 ? 0 : 2;
}

// Once per game frame, and per real frame while Assist holds the game: the
// pad's sticks into the trails, and its inputs into their glows.
static void Pad_Record(HSD_Pad *pad)
{
    pad_trail_pos = (pad_trail_pos + 1) % PAD_TRAIL;
    pad_trail[pad_trail_pos] = (Vec2){pad->fstickX, pad->fstickY};
    pad_ctrail_pos = (pad_ctrail_pos + 1) % PAD_CTRAIL;
    pad_ctrail[pad_ctrail_pos] = (Vec2){pad->fsubstickX, pad->fsubstickY};

    int held[PIN_COUNT];
    Pad_Held(pad, held);
    for (int i = 0; i < PIN_COUNT; i++)
    {
        pad_glow_prev[i] = pad_glow[i];
        float g = held[i] ? 1.f : pad_glow[i] * 0.55f;
        pad_glow[i] = g < 0.05f ? 0.f : g;
        float f = held[i] && pad_glow_prev[i] < 1.f ? 1.f : pad_flash[i] * 0.6f;
        pad_flash[i] = f < 0.04f ? 0.f : f;
    }
    int dir = CStick_Dir(pad->fsubstickX, pad->fsubstickY);
    for (int i = 0; i < 4; i++)
    {
        float g = i == dir ? 1.f : pad_cdir_glow[i] * 0.5f;
        pad_cdir_glow[i] = g < 0.04f ? 0.f : g;
    }
}

// The pad the display shows: the one the game reads, or while the game is
// paused the master pad, which still moves.
static HSD_Pad *Pad_Live(FighterData *fp)
{
    int paused = (stc_hsd_update->pause_kind & 1) || assist_frozen;
    return paused ? PadGetMaster(fp->pad_index) : PadGetEngine(fp->pad_index);
}

// Where the display goes: the bottom of the screen, in a corner or beside
// the player's percent. Other drawing keeps clear of this box.
static void Pad_Box(FighterData *fp, float *x0, float *y0, float *x1, float *y1)
{
    int buttons = Options_Hud[HOPT_BUTTONS].val;
    int look = buttons ? Options_Hud[HOPT_LOOK].val : -1;
    float rs = ring_sizes[Options_Hud[HOPT_PAD_SIZE].val];
    float w = look == LOOK_RING ? 2 * RING_A * rs : look == LOOK_CREST ? 2 * CREST_W * CREST_K * rs : buttons ? PAD_W : PAD_STICK_W;
    float h = look == LOOK_RING ? 2 * RING_B * rs : look == LOOK_CREST ? (CREST_TOP + CREST_BOT) * CREST_K * rs : buttons ? PAD_H : PAD_STICK_H;
    float left = SAFE_W - w, bottom = -SAFE_H + 0.4f;
    int place = Options_Hud[HOPT_STICK].val;
    if (place == STICK_LEFT)
        left = -SAFE_W;
    else if (place == STICK_PERCENT)
    {
        Vec3 *hp = Match_GetPlayerHUDPos(fp->ply);
        if (hp)
        {
            // every player's percent, so a CPU's isn't covered either
            Box pct[4];
            int n = 0;
            for (int i = 0; i < 4; i++)
            {
                Vec3 *o = Fighter_GetSlotType(i) != 3 ? Match_GetPlayerHUDPos(i) : 0;
                if (o)
                    pct[n++] = (Box){o->X - PCT_HALF_W, -SAFE_H - 1.f, o->X + PCT_HALF_W, o->Y + PCT_TOP};
            }
            // beside his percent, the side toward the middle first, then
            // above it; the first spot on screen that covers no percent
            float side = hp->X > 0.5f ? -1.f : 1.f;
            float cand[3][2] = {
                {side > 0 ? hp->X + PCT_HALF_W + 0.2f : hp->X - PCT_HALF_W - 0.2f - w, bottom},
                {side > 0 ? hp->X - PCT_HALF_W - 0.2f - w : hp->X + PCT_HALF_W + 0.2f, bottom},
                {hp->X - w / 2, hp->Y + PCT_TOP + 0.3f},
            };
            int pick = 2;
            for (int c = 0; c < 2 && pick == 2; c++)
            {
                Box b = {cand[c][0], cand[c][1], cand[c][0] + w, cand[c][1] + h};
                int clear = b.x0 >= -SAFE_W - 0.01f && b.x1 <= SAFE_W + 0.01f;
                for (int i = 0; i < n && clear; i++)
                    clear = !Box_Hit(&b, &pct[i]);
                if (clear)
                    pick = c;
            }
            left = cand[pick][0];
            bottom = cand[pick][1];
        }
    }
    // clear of TM-CE's version text
    Box wm = Wm_Box();
    Box b = {left, bottom, left + w, bottom + h};
    if (Box_Hit(&b, &wm))
        bottom = wm.y1;
    *x0 = left;
    *y0 = bottom;
    *x1 = left + w;
    *y1 = *y0 + h;
}

// A stick's gate: an octagon with corners at the notches.
static void Pad_Gate(float cx, float cy, float R, float rim_w, GXColor rim)
{
    float vx[9], vy[9];
    for (int i = 0; i <= 8; i++)
    {
        float ang = (i % 8) * 0.7853982f;
        vx[i] = cx + cos(ang) * R;
        vy[i] = cy + sin(ang) * R;
    }
    GXColor plate = Color_Fill(color_plate, 0.6f);
    for (int i = 0; i < 8; i++)
        Hud_Tri(cx, cy, vx[i], vy[i], vx[i + 1], vy[i + 1], plate);
    if (pad_soft)
    {
        Hud_Line(vx, vy, 8, 1, rim_w, PX, rim);
        return;
    }
    for (int i = 0; i < 8; i++)
        Hud_Seg(vx[i], vy[i], vx[i + 1], vy[i + 1], rim_w, rim);
}

// A pill's ten corners: half circles joined by straight edges, its length
// turned rot radians from level.
static void Pill_Points(float cx, float cy, float w, float h, float rot, float *px, float *py)
{
    float r = h / 2, d = w / 2 - r;
    float cr = cos(rot), sr = sin(rot);
    for (int i = 0; i < 5; i++)
    {
        float ang = (-90 + 45 * i) * 0.01745329f;
        float x = d + cos(ang) * r, y = sin(ang) * r;
        px[i] = cx + x * cr - y * sr;
        py[i] = cy + x * sr + y * cr;
        px[5 + i] = cx - x * cr + y * sr;
        py[5 + i] = cy - x * sr - y * cr;
    }
}

static void Hud_Pill(float cx, float cy, float w, float h, float rot, GXColor c)
{
    float px[10], py[10];
    Pill_Points(cx, cy, w, h, rot, px, py);
    for (int i = 0; i < 10; i++)
        Hud_Tri(cx, cy, px[i], py[i], px[(i + 1) % 10], py[(i + 1) % 10], c);
}

static void Hud_PillRing(float cx, float cy, float w, float h, float rot, float lw, GXColor c)
{
    float px[10], py[10];
    Pill_Points(cx, cy, w, h, rot, px, py);
    for (int i = 0; i < 10; i++)
        Hud_Seg(px[i], py[i], px[(i + 1) % 10], py[(i + 1) % 10], lw, c);
}

// A round button: a faint outline, filled by its glow, and a bigger bright
// ring on the frame it goes down.
static void Pad_Button(float cx, float cy, float r, GXColor c, int in, float glow)
{
    Pin_Spot(in, cx, cy, r);
    Hud_Ring(cx, cy, r - 0.035f, 0.07f, Color_Fill(c, 0.5f));
    if (glow > 0)
        Hud_Disc(cx, cy, r - 0.07f, Color_Fill(c, glow));
    if (pad_glow[in] >= 1.f && pad_glow_prev[in] < 1.f)
        Hud_Ring(cx, cy, r + 0.2f, 0.1f, Color_Mix(c, color_white, 0.5f));
}

// A trigger: a bar filled from the bottom by how far it's pressed, and a cap
// above it that lights while the click is down. by is the block's bottom.
static void Pad_Trigger(float x, float by, float analog, float glow)
{
    GXColor c = land_kind_colors[LAND_PERFECT_WL];
    GXColor plate = Color_Fill(color_plate, 0.6f);
    float x1 = x + 0.5f;
    Hud_Rect(x, by + 0.2f, x1, by + 4.6f, plate);
    Hud_Frame(x, by + 0.2f, x1, by + 4.6f, 0.06f, Color_Fill(c, 0.4f));
    analog = Clamp01(analog);
    if (analog > 0)
        Hud_Rect(x + 0.06f, by + 0.26f, x1 - 0.06f, by + 0.26f + 4.28f * analog, Color_Fill(c, 0.7f));
    Hud_Rect(x, by + 4.8f, x1, by + 5.3f, plate);
    Hud_Frame(x, by + 4.8f, x1, by + 5.3f, 0.06f, Color_Fill(c, 0.4f));
    if (glow > 0)
        Hud_Rect(x + 0.06f, by + 4.86f, x1 - 0.06f, by + 5.24f, Color_Fill(c, glow));
}

// The shield drop zone: the stick angles that drop Falcon through a platform
// out of his shield, shaded in the gate while he stands or shields on one.
//
// Checked in the decomp:
// - The drop is ftCo_80099F1C (ftCo_Pass.c), reached through ftCo_8009A080:
//   L or R held, stick y <= -PlCo 0x464, the y timer (active_timer.lstick.y,
//   the frames since the stick left the tilt zone; timer_lstick_tilt_y here)
//   below PlCo 0x468, and mpColl_IsOnPlatform on the floor under him (the
//   line's platform flag, desc->is_unk). It never looks at x, so everything
//   below the line drops: the zone is a cap of the gate.
// - It's the last check in GuardOn's and Guard's IASA (ftCo_Guard.c). The
//   spotdodge is ahead of it (ftCo_8009980C, ftCo_Escape.c): stick y <=
//   PlCo 0x314 with that timer below PlCo 0x318, or the C-stick down as far.
//   A flick that gets that far down fast enough spotdodges and never drops.
//   The sideways roll check (ftCo_8009917C) is ahead of it too, but goes by
//   the x flick, so it isn't drawn.
// Inferred, not checked on a console:
// - The signs: 0x314 is compared as it is (so negative), 0x464 negated (so
//   positive). The decomp types 0x468 as a float where the other windows are
//   ints, so Common_Frames reads it either way. Event_Init logs all four.
// - Standing (Wait) and the two shield states are where to show it.
static int Pad_DropShown(FighterData *fp)
{
    int sid = fp->state_id;
    int id = fp->coll_data.ground_index;
    RawCollLine *lines = (RawCollLine *)*stc_collline;
    return Options_Hud[HOPT_SHIELD_DROP].val && fp->phys.air_state == 0 &&
           (sid == ASID_WAIT || sid == ASID_GUARDON || sid == ASID_GUARD) &&
           common_drop_stick > 0.3f && common_drop_stick < 1.f && // as read from the game
           lines && id >= 0 && lines[id].desc->is_unk;            // is_unk is the platform flag
}

// The part of the gate (an octagon) at or below the stick height lim (-1 to
// 1), tinted c at a, with a line along its top edge at la (none if 0). Cut
// like any convex polygon: the points that are below it, and where the edges
// that cross it do.
static void Pad_Cap(float cx, float cy, float R, float k, float lim, GXColor c, float a, float la)
{
    float vx[8], vy[8], px[10], py[10], ex[2];
    float y = cy + lim * R;
    Circle_Points(cx, cy, R, 8, vx, vy);
    int n = 0, e = 0;
    for (int i = 0; i < 8; i++)
    {
        int j = (i + 1) % 8;
        int in0 = vy[i] <= y, in1 = vy[j] <= y;
        if (in0)
        {
            px[n] = vx[i];
            py[n++] = vy[i];
        }
        if (in0 != in1 && e < 2)
        {
            ex[e] = vx[i] + (vx[j] - vx[i]) * (y - vy[i]) / (vy[j] - vy[i]);
            px[n] = ex[e++];
            py[n++] = y;
        }
    }
    if (e < 2 || n < 3)
        return;
    float mx = 0, my = 0;
    for (int i = 0; i < n; i++)
    {
        mx += px[i] / n;
        my += py[i] / n;
    }
    Hud_Fan(mx, my, px, py, n, Color_Over(c, a));
    if (la > 0)
        Hud_Seg(ex[0], y, ex[1], y, 0.06f * k, Color_Over(c, la));
}

// The zone in the stick's gate: blue where a press drops, with amber over
// where a flick down fast enough spotdodges instead (a slow one still drops
// there). Each brightens while the stick is doing it now, as the fastfall
// line does.
static void Pad_DropZone(FighterData *fp, float cx, float cy, float R, float k)
{
    if (!Pad_DropShown(fp))
        return;
    float y = fp->input.lstick.Y;
    int flick = (u8)fp->input.timer_lstick_tilt_y;
    int dodges = y <= common_spot_stick && flick < common_spot_window;
    int drops = y <= -common_drop_stick && flick < common_drop_window && !dodges;
    GXColor blue = {100, 170, 255, 255}, amber = {255, 175, 60, 255};
    Pad_Cap(cx, cy, R, k, -common_drop_stick, blue, drops ? 0.36f : 0.2f, drops ? 0.9f : 0.5f);
    if (common_spot_stick > -1.f && common_spot_stick < -0.3f)
    {
        // the two lines would sit on each other if the thresholds are close
        int apart = fabs(common_spot_stick + common_drop_stick) > 0.08f;
        Pad_Cap(cx, cy, R, k, common_spot_stick, amber, dodges ? 0.34f : 0.14f, !apart ? 0 : dodges ? 0.9f : 0.5f);
    }
}

// The stick: its gate, the band Melee reads as zero, the last frames as a
// trail, where the stick is, while falling the line where pulling down
// starts a fastfall (lit while a fastfall flick is live), and on a platform
// the shield drop zone. R is the gate's radius, k scales the marks inside it.
static void Pad_Stick(FighterData *fp, HSD_Pad *pad, float cx, float cy, float R, float k)
{
    Pad_Gate(cx, cy, R, 0.12f * k, Color_Fill(color_white, 0.45f));

    // the deadzone cross, where an axis reads as zero; fainter in the Ring
    // look, so the gate and the stick read first
    float rx = pad->fstickX, ry = pad->fstickY;
    float dzx = Common_Float(0x0), dzy = Common_Float(0x4);
    GXColor band = Color_Over(color_white, pad_soft ? 0.1f : 0.25f);
    float len = R * 0.88f;
    Hud_Rect(cx - dzx * R, cy - len, cx + dzx * R, cy + len, band);
    Hud_Rect(cx - len, cy - dzy * R, cx - dzx * R, cy + dzy * R, band);
    Hud_Rect(cx + dzx * R, cy - dzy * R, cx + len, cy + dzy * R, band);

    Pad_DropZone(fp, cx, cy, R, k);

    // fastfall line: lit on the frames the game takes a flick past it (it
    // has to be falling, not fastfalling yet, and not in an airdodge or a
    // Falcon Dive, ft_80084DB0), half lit while a fastfall is on
    if (fp->phys.air_state == 1)
    {
        int ts = Tracked_Index(fp->state_id);
        int can = fp->phys.self_vel.Y < 0 && !fp->flags.is_fastfall && ts != TS_ESCAPEAIR && !Tracked_IsUpB(ts);
        int ff = can && fp->input.lstick.Y <= -common_fastfall_stick && (u8)fp->input.timer_lstick_tilt_y < common_fastfall_window;
        GXColor c = ff ? color_white : Color_Over(color_white, fp->flags.is_fastfall ? 0.6f : 0.35f);
        float fy = cy - common_fastfall_stick * R;
        float fw = R * 0.62f;
        Hud_Rect(cx - fw, fy - 0.05f * k, cx + fw, fy + 0.05f * k, c);
        Glyph_Draw(GLYPH_FF, cx + fw + 0.3f * k, fy, 0.2f * k, c);
    }

    // trail, oldest first
    for (int n = PAD_TRAIL - 1; n >= 1; n--)
    {
        Vec2 *p = &pad_trail[(pad_trail_pos - n + PAD_TRAIL) % PAD_TRAIL];
        float a = 0.5f * (1.f - (float)n / PAD_TRAIL);
        float r = (0.14f + 0.12f * (1.f - (float)n / PAD_TRAIL)) * k;
        Hud_Rect(cx + p->X * R - r, cy + p->Y * R - r, cx + p->X * R + r, cy + p->Y * R + r, Color_Fill(color_white, a));
    }
    float dx = cx + rx * R, dy = cy + ry * R;
    Hud_Seg(cx, cy, dx, dy, 0.14f * k, Color_Fill(color_white, 0.8f));
    Hud_Rect(dx - 0.28f * k, dy - 0.28f * k, dx + 0.28f * k, dy + 0.28f * k, color_white);

    // where the game reads it, when that isn't where it is
    float gx = fabs(rx) <= dzx ? 0.f : rx;
    float gy = fabs(ry) <= dzy ? 0.f : ry;
    if (gx != rx || gy != ry)
        Hud_Ring(cx + gx * R, cy + gy * R, 0.4f * k, 0.09f * k, Color_Fill(color_white, 0.85f));
}

// The C-stick: a small gate with a dot. Dot and rim turn yellow past where
// it throws an aerial (aerial is how much, 1 while it does and fading after).
static void Pad_CStick(HSD_Pad *pad, float cx, float cy, float aerial)
{
    float R = 1.15f;
    GXColor pink = color_btn_c;
    GXColor c = Color_Mix(color_white, pink, aerial);
    Pad_Gate(cx, cy, R, 0.09f, Color_Mix(Color_Fill(color_white, 0.45f), pink, aerial));
    for (int n = PAD_CTRAIL - 1; n >= 1; n--)
    {
        Vec2 *p = &pad_ctrail[(pad_ctrail_pos - n + PAD_CTRAIL) % PAD_CTRAIL];
        float a = 0.5f * (1.f - (float)n / PAD_CTRAIL);
        float r = 0.07f + 0.07f * (1.f - (float)n / PAD_CTRAIL);
        Hud_Rect(cx + p->X * R - r, cy + p->Y * R - r, cx + p->X * R + r, cy + p->Y * R + r, Color_Fill(color_white, a));
    }
    float dx = cx + pad->fsubstickX * R, dy = cy + pad->fsubstickY * R;
    Hud_Rect(dx - 0.17f, dy - 0.17f, dx + 0.17f, dy + 0.17f, c);
}
