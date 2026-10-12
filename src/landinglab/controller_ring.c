/* landinglab/controller_ring.c
 *
 * Holds the controller display's Ring look.
 *
 * Part of Landing Lab: src/landinglab.c includes it, in order, into one
 * translation unit, so it sees everything included before it (and mex.h,
 * events.h, landinglab_sim.h).
 *
 * Uses: option_values, menus_display, drawing, hud_drawing, controller.
 */

// The nub's depth past the band, t degrees from its middle: a smooth bell,
// since a point flares into a spike where the soft outline turns.
static float Ring_Swell(float t)
{
    float u = fabs(t) / RING_NUB_W;
    if (u >= 1)
        return 0;
    float sh = 1 - u * u;
    sh *= sh;
    return RING_NUB * sh * sh;
}

// A point of the ring's left half at size s: t degrees around the ellipse
// (180 is the middle of the left side), off units in from its edge. The
// right half is its mirror.
static void Ring_Point(float t, float off, float s, float *x, float *y)
{
    float a = t * 0.01745329f;
    float c = cos(a), sn = sin(a);
    float nx = c / RING_A, ny = sn / RING_B, m = sqrtf(nx * nx + ny * ny);
    *x = (RING_A * c - nx / m * off) * s;
    *y = (RING_B * sn - ny / m * off) * s;
}

// A point on the inner edge of the left half, pushed d units further in
// toward the middle (the nub): straight across rather than along the
// ellipse's normal, which folds over itself once it's deeper than the
// ellipse's curve at the side is round.
static void Ring_Inner(float t, float d, float s, float *x, float *y)
{
    Ring_Point(t, RING_BAND, s, x, y);
    *x += d * s;
}

// The angle of the k-th sample along a half, from its top end (k = 0) to its
// bottom end, closer together across the nub.
static float Ring_Angle(int k)
{
    float top = 90 + RING_GAP, n0 = 180 - RING_NUB_W, n1 = 180 + RING_NUB_W, bot = 270 - RING_GAP;
    if (k <= RING_LEG_SEGS)
        return top + (n0 - top) * k / RING_LEG_SEGS;
    k -= RING_LEG_SEGS;
    if (k <= RING_NUB_SEGS)
        return n0 + (n1 - n0) * k / RING_NUB_SEGS;
    k -= RING_NUB_SEGS;
    return n1 + (bot - n1) * k / RING_LEG_SEGS;
}

// A quad of a half between the angles t0 and t1, from the offset o0 in to the
// band's inner edge pushed d further in (each given at both angles). m is 1
// for the left half and -1 for the right one.
static void Ring_Quad(float cx, float cy, float s, float m, float t0, float t1, float o0a, float o0b, float d1a, float d1b, GXColor c)
{
    float x[4], y[4];
    Ring_Point(t0, o0a, s, &x[0], &y[0]);
    Ring_Point(t1, o0b, s, &x[1], &y[1]);
    Ring_Inner(t1, d1b, s, &x[2], &y[2]);
    Ring_Inner(t0, d1a, s, &x[3], &y[3]);
    Quad_Add(cx + m * x[0], cy + y[0], cx + m * x[1], cy + y[1], cx + m * x[2], cy + y[2], cx + m * x[3], cy + y[3], c);
}

// One trigger as a half of the ring around (cx, cy): side -1 is L, 1 is R.
// analog is how far it's pressed, click whether it's clicked, lit how lit the
// click leaves it (1 while it's held, fading after), flash the press's flash.
static void Ring_Trigger(float cx, float cy, float s, int side, float analog, int click, float lit, float flash)
{
    GXColor c = color_in_dodge;
    float m = -side;
    float px[2 * RING_SEGS + 2], py[2 * RING_SEGS + 2]; // its outline: down the outside, up the inside
    int n = 2 * RING_SEGS + 2;
    for (int k = 0; k <= RING_SEGS; k++)
    {
        float t = Ring_Angle(k);
        int o = k, i = n - 1 - k;
        Ring_Point(t, 0, s, &px[o], &py[o]);
        Ring_Inner(t, Ring_Swell(t - 180), s, &px[i], &py[i]);
        px[o] = cx + m * px[o];
        py[o] += cy;
        px[i] = cx + m * px[i];
        py[i] += cy;
    }
    GXColor plate = Color_Fill(color_plate, 0.55f);
    for (int k = 0; k < RING_SEGS; k++)
        Quad_Add(px[k], py[k], px[k + 1], py[k + 1], px[n - 2 - k], py[n - 2 - k], px[n - 1 - k], py[n - 1 - k], plate);
    {
        float mx = 0, my = 0;
        for (int k = 0; k < n; k++)
        {
            mx += px[k];
            my += py[k];
        }
        Pin_Spot(side > 0 ? PIN_R : PIN_L, mx / n, my / n, 0.8f * s);
    }
    // the outline, under the fill so a light press isn't hidden by it; a
    // lit half has a bright, heavier one
    Hud_Line(px, py, n, 1, (0.05f + 0.04f * lit) * s, PX, Color_Mix(Color_Fill(c, 0.8f), color_white, lit));

    // the fill: both ends of the band run in to the middle, then the nub
    // floods from its base to its tip
    float a = click ? 1.f : Clamp01(analog);
    float ac = Clamp01(a / RING_CH), as = Clamp01((a - RING_CH) / (1 - RING_CH));
    GXColor fc = Color_Fill(c, 0.6f + 0.4f * a);
    if (ac > 0.002f)
    {
        float top = 90 + RING_GAP, bot = 270 - RING_GAP;
        float f0 = top + (180 - top) * ac, f1 = bot - (bot - 180) * ac;
        for (int k = 0; k < RING_SEGS; k++)
        {
            float t0 = Ring_Angle(k), t1 = Ring_Angle(k + 1);
            if (t0 < f0)
                Ring_Quad(cx, cy, s, m, t0, t1 < f0 ? t1 : f0, 0, 0, 0, 0, fc);
            if (t1 > f1)
                Ring_Quad(cx, cy, s, m, t0 > f1 ? t0 : f1, t1, 0, 0, 0, 0, fc);
        }
    }
    if (as > 0.002f)
    {
        float level = as * RING_NUB;
        for (int k = RING_LEG_SEGS; k < RING_LEG_SEGS + RING_NUB_SEGS; k++)
        {
            float t0 = Ring_Angle(k), t1 = Ring_Angle(k + 1);
            float d0 = Ring_Swell(t0 - 180), d1 = Ring_Swell(t1 - 180);
            Ring_Quad(cx, cy, s, m, t0, t1, RING_BAND * 0.5f, RING_BAND * 0.5f, d0 < level ? d0 : level, d1 < level ? d1 : level, fc);
        }
    }

    // a mark on each way in where a light press counts: a short bar across
    // the band with a pointer under it
    for (int e = 0; e < 2; e++)
    {
        float end = e ? 270 - RING_GAP : 90 + RING_GAP;
        float t = end + (180 - end) * (RING_LIGHT / RING_CH);
        float tx[2], ty[2];
        Ring_Point(t, 0.03f, s, &tx[0], &ty[0]);
        Ring_Point(t, RING_BAND - 0.03f, s, &tx[1], &ty[1]);
        tx[0] = cx + m * tx[0];
        tx[1] = cx + m * tx[1];
        ty[0] += cy;
        ty[1] += cy;
        Hud_Line(tx, ty, 2, 0, 0.07f * s, PX, Color_Over(color_white, 0.95f));
    }

    // a click lights the whole half, as bright as the flash it starts with,
    // and keeps it so while it's held (the flash is the same light, brighter,
    // settling into it)
    float w = 0.68f * lit + (1.f - 0.68f * lit) * flash;
    if (w > 0)
    {
        GXColor fl = Color_Over(color_white, w);
        for (int k = 0; k < RING_SEGS; k++)
            Quad_Add(px[k], py[k], px[k + 1], py[k + 1], px[n - 2 - k], py[n - 2 - k], px[n - 1 - k], py[n - 1 - k], fl);
    }
}

// The dark plate behind the whole ring, so its outlines read over any stage.
static void Ring_Backing(float cx, float cy, float s)
{
    float px[32], py[32];
    for (int i = 0; i < 32; i++)
    {
        float ang = i * (6.2831853f / 32);
        px[i] = cx + cos(ang) * RING_A * s;
        py[i] = cy + sin(ang) * RING_B * s;
    }
    Hud_Fan(cx, cy, px, py, 32, Color_Fill(color_plate, 0.45f)); // the triggers' outlines soften its edge
}

// A button's fill: its own color at rest, lighter and fully opaque the more
// it's pressed.
static GXColor Ring_Lit(GXColor c, float glow)
{
    // up: a dark well in the button's color, its rim and letter in the
    // color; down: filled solid and bright, the way a trigger fills
    return Color_Mix(Color_Over(Color_Mix(c, color_plate, 0.72f), 0.88f), Color_Over(Color_Mix(c, color_white, 0.3f), 1.f), glow);
}

// A button's outline, lighter than its fill.
static GXColor Ring_Edge(GXColor c)
{
    return Color_Over(Color_Mix(c, color_white, 0.35f), 0.9f);
}

// The ghost a press leaves, a copy of the button's fill, from the button's
// flash (1 on the frame it went down, times 0.6 on each after, so a few
// frames in all): how far it has grown (1 and up) ...
static float Ghost_Grow(float flash)
{
    return 1.15f + 0.2f * (1.f - flash);
}

#define GHOST_MIN 0.1f

// ... and fading to nothing as the flash reaches GHOST_MIN
static GXColor Ghost_Color(GXColor c, float flash)
{
    return Color_Over(Color_Mix(c, color_white, 0.45f), 0.9f * sqrtf((flash - GHOST_MIN) / (1.f - GHOST_MIN)));
}

// One stroke of a button's letter: n points in units of the letter's height h,
// from -0.5 to 0.5 each way, around (cx, cy).
static void Ring_Stroke(float cx, float cy, float h, GXColor c, int n, const float *xy)
{
    float px[7], py[7];
    for (int i = 0; i < n; i++)
    {
        px[i] = cx + xy[2 * i] * h;
        py[i] = cy + xy[2 * i + 1] * h;
    }
    Hud_Line(px, py, n, 0, (0.17f * h > 0.095f ? 0.17f * h : 0.095f), PX * 0.6f, c);
}

// The letter printed on a button, in strokes since there's no text engine.
static void Ring_Letter(int in, float cx, float cy, float h, GXColor bc, float glow)
{
    Pin_Spot(in, cx, cy, h);
    // light in the button's color on the dark well, dark on the lit fill
    GXColor c = Color_Mix(Color_Over(Color_Mix(bc, color_white, 0.5f), 0.95f), Color_Over(color_plate, 0.92f), glow);
    switch (in)
    {
    case PIN_A:
    {
        float legs[] = {-0.34f, -0.5f, 0, 0.5f, 0.34f, -0.5f};
        float bar[] = {-0.2f, -0.14f, 0.2f, -0.14f};
        Ring_Stroke(cx, cy, h, c, 3, legs);
        Ring_Stroke(cx, cy, h, c, 2, bar);
        break;
    }
    case PIN_B:
    {
        float top[] = {-0.3f, -0.5f, -0.3f, 0.5f, 0.08f, 0.5f, 0.3f, 0.36f, 0.3f, 0.16f, 0.08f, 0, -0.3f, 0};
        float low[] = {0.08f, 0, 0.34f, -0.14f, 0.34f, -0.36f, 0.08f, -0.5f, -0.3f, -0.5f};
        Ring_Stroke(cx, cy, h, c, 7, top);
        Ring_Stroke(cx, cy, h, c, 5, low);
        break;
    }
    case PIN_X:
    {
        float down[] = {-0.32f, 0.5f, 0.32f, -0.5f};
        float up[] = {-0.32f, -0.5f, 0.32f, 0.5f};
        Ring_Stroke(cx, cy, h, c, 2, down);
        Ring_Stroke(cx, cy, h, c, 2, up);
        break;
    }
    case PIN_Y:
    {
        float arms[] = {-0.34f, 0.5f, 0, 0.02f, 0.34f, 0.5f};
        float stem[] = {0, 0.02f, 0, -0.5f};
        Ring_Stroke(cx, cy, h, c, 3, arms);
        Ring_Stroke(cx, cy, h, c, 2, stem);
        break;
    }
    case PIN_Z:
    {
        float z[] = {-0.32f, 0.5f, 0.32f, 0.5f, -0.32f, -0.5f, 0.32f, -0.5f};
        Ring_Stroke(cx, cy, h, c, 4, z);
        break;
    }
    }
}

// A round button: filled in its color, lettered, with a soft outline, and on
// a press a ghost of its fill that grows a little and fades in a few frames.
static void Ring_Button(float cx, float cy, float r, float s, GXColor c, int in, float glow)
{
    float px[18], py[18];
    float flash = pad_flash[in];
    if (flash > GHOST_MIN)
    {
        Circle_Points(cx, cy, r * Ghost_Grow(flash), 18, px, py);
        Hud_Fan(cx, cy, px, py, 18, Ghost_Color(c, flash));
    }
    Circle_Points(cx, cy, r, 18, px, py);
    Hud_Fan(cx, cy, px, py, 18, Ring_Lit(c, glow));
    Hud_Line(px, py, 18, 1, 0.07f * s, PX, Ring_Edge(c));
    Ring_Letter(in, cx, cy, 0.95f * r, c, glow);
}

// A kidney, the shape of the X and Y buttons (and here Z): an arc rho from
// the middle of its line (mx, my) bulging toward dir degrees, span degrees
// long and w thick with round ends.
#define KIDNEY_ARC 6
#define KIDNEY_CAP 4
#define KIDNEY_POINTS (2 * KIDNEY_ARC + 2 * KIDNEY_CAP)

// Its outline into px and py, and the centers of its round ends into ends.
static void Kidney_Points(float mx, float my, float dir, float rho, float span, float w, float *px, float *py, float *ends)
{
    float d2r = 0.01745329f;
    float cx = mx - cos(dir * d2r) * rho, cy = my - sin(dir * d2r) * rho; // the arc's center
    float a0 = dir - span / 2, a1 = dir + span / 2, h = w / 2;
    int n = 0;
    for (int i = 0; i <= KIDNEY_ARC; i++) // the outside, a0 to a1
    {
        float a = (a0 + (a1 - a0) * i / KIDNEY_ARC) * d2r;
        px[n] = cx + cos(a) * (rho + h);
        py[n++] = cy + sin(a) * (rho + h);
    }
    float e = a1 * d2r;
    ends[0] = cx + cos(e) * rho;
    ends[1] = cy + sin(e) * rho;
    for (int i = 1; i < KIDNEY_CAP; i++) // round end at a1
    {
        float a = e + 3.1415927f * i / KIDNEY_CAP;
        px[n] = ends[0] + cos(a) * h;
        py[n++] = ends[1] + sin(a) * h;
    }
    for (int i = 0; i <= KIDNEY_ARC; i++) // the inside, a1 back to a0
    {
        float a = (a1 - (a1 - a0) * i / KIDNEY_ARC) * d2r;
        px[n] = cx + cos(a) * (rho - h);
        py[n++] = cy + sin(a) * (rho - h);
    }
    float b = a0 * d2r;
    ends[2] = cx + cos(b) * rho;
    ends[3] = cy + sin(b) * rho;
    for (int i = 1; i < KIDNEY_CAP; i++) // round end at a0
    {
        float a = b + 3.1415927f + 3.1415927f * i / KIDNEY_CAP;
        px[n] = ends[2] + cos(a) * h;
        py[n++] = ends[3] + sin(a) * h;
    }
}

static void Kidney_Fill(const float *px, const float *py, const float *ends, GXColor f)
{
    for (int i = 0; i < KIDNEY_ARC; i++)
    {
        int o = i, in0 = 2 * KIDNEY_ARC + KIDNEY_CAP - i; // the inside point at the same angle
        Quad_Add(px[o], py[o], px[o + 1], py[o + 1], px[in0 - 1], py[in0 - 1], px[in0], py[in0], f);
    }
    for (int i = 0; i < KIDNEY_CAP; i++)
    {
        int j = KIDNEY_ARC + i;
        Hud_Tri(ends[0], ends[1], px[j], py[j], px[j + 1], py[j + 1], f);
        j = 2 * KIDNEY_ARC + KIDNEY_CAP + i;
        Hud_Tri(ends[2], ends[3], px[j], py[j], px[(j + 1) % KIDNEY_POINTS], py[(j + 1) % KIDNEY_POINTS], f);
    }
}

// Filled in its color, lettered, with a soft outline and the press ghost.
static void Ring_Kidney(float mx, float my, float dir, float rho, float span, float w, float s, GXColor c, int in, float glow)
{
    float px[KIDNEY_POINTS], py[KIDNEY_POINTS], ends[4];
    float flash = pad_flash[in];
    if (flash > GHOST_MIN)
    {
        // the same shape grown about its middle
        Kidney_Points(mx, my, dir, rho * Ghost_Grow(flash), span, w * Ghost_Grow(flash), px, py, ends);
        Kidney_Fill(px, py, ends, Ghost_Color(c, flash));
    }
    Kidney_Points(mx, my, dir, rho, span, w, px, py, ends);
    Kidney_Fill(px, py, ends, Ring_Lit(c, glow));
    Hud_Line(px, py, KIDNEY_POINTS, 1, 0.07f * s, PX, Ring_Edge(c));
    // Z's bean runs diagonally under its letter, so the letter's corners
    // reach the edges sooner
    Ring_Letter(in, mx, my, (in == PIN_Z ? 0.64f : 0.85f) * w, c, glow);
}

// The Ring look's C-stick around (cx, cy): the four arrows, lit the way the
// game reads it and fading after, and the live gate with its trail.
static void Ring_CStick(HSD_Pad *pad, float cx, float cy, float s)
{
    GXColor yel = color_btn_c;
    int dir = CStick_Dir(pad->fsubstickX, pad->fsubstickY);
    for (int d = 0; d < 4; d++)
    {
        float g = d == dir ? 1.f : pad_cdir_glow[d];
        int level = d % 2 == 0; // left and right have more room
        float base = (level ? 0.58f : 0.52f) * s;
        float len = (level ? 0.26f : 0.22f) * s, half = (level ? 0.23f : 0.2f) * s;
        float ux = d == 0 ? 1 : d == 2 ? -1 : 0, uy = d == 1 ? 1 : d == 3 ? -1 : 0;
        float ax[3] = {cx + ux * base - uy * half, cx + ux * (base + len), cx + ux * base + uy * half};
        float ay[3] = {cy + uy * base + ux * half, cy + uy * (base + len), cy + uy * base - ux * half};
        Hud_Tri(ax[0], ay[0], ax[1], ay[1], ax[2], ay[2], Ring_Lit(yel, g));
        Hud_Line(ax, ay, 3, 1, 0.06f * s, PX, Ring_Edge(yel));
    }
    float R = RING_C_R * s;
    float vx[8], vy[8];
    Circle_Points(cx, cy, R, 8, vx, vy);
    Hud_Fan(cx, cy, vx, vy, 8, Color_Over(yel, 0.3f)); // the stick is yellow too
    Pad_Gate(cx, cy, R, 0.06f * s, Color_Over(yel, 0.8f));
    for (int n = PAD_CTRAIL - 1; n >= 1; n--)
    {
        Vec2 *p = &pad_ctrail[(pad_ctrail_pos - n + PAD_CTRAIL) % PAD_CTRAIL];
        float a = 0.85f * (1.f - (float)n / PAD_CTRAIL);
        float r = (0.04f + 0.05f * (1.f - (float)n / PAD_CTRAIL)) * s;
        Hud_Rect(cx + p->X * R - r, cy + p->Y * R - r, cx + p->X * R + r, cy + p->Y * R + r, Color_Fill(yel, a));
    }
    float dx = cx + pad->fsubstickX * R, dy = cy + pad->fsubstickY * R, h = 0.13f * s;
    Hud_Rect(dx - h, dy - h, dx + h, dy + h, color_white);
}

// The Ring look, from the box's bottom left corner.
static void Ring_Draw(FighterData *fp, HSD_Pad *pad, float bx, float by)
{
    float s = ring_sizes[Options_Hud[HOPT_PAD_SIZE].val];
    float cx = bx + RING_A * s, cy = by + RING_B * s;

    int held[PIN_COUNT];
    Pad_Held(pad, held);
    float glow[PIN_COUNT];
    for (int i = 0; i < PIN_COUNT; i++)
        glow[i] = held[i] ? 1.f : pad_glow[i];

    pad_soft = 1;
    Ring_Backing(cx, cy, s);
    Ring_Trigger(cx, cy, s, -1, pad->ftriggerLeft, held[PIN_L], glow[PIN_L], pad_flash[PIN_L]);
    Ring_Trigger(cx, cy, s, 1, pad->ftriggerRight, held[PIN_R], glow[PIN_R], pad_flash[PIN_R]);

    // the stick in the middle, the buttons in the bays around it, each in
    // the middle of its bay (where its outline keeps the most room)
    float sx = cx, sy = cy + RING_STICK_Y * s;
    Pad_Stick(fp, pad, sx, sy, RING_STICK_R * s, 0.72f * s);
    float tx = 2.22f * s, ty = cy + 1.26f * s, lx = 1.92f * s, ly = cy - 1.38f * s;
    // Y left of X and a little above it, the same bean mirrored
    Ring_Kidney(cx - tx - 0.55f * s, ty + 0.1f * s, 180, 0.8f * s, 60, 0.5f * s, s, color_btn_xy, PIN_Y, glow[PIN_Y]);
    Ring_Kidney(cx - tx + 0.55f * s, ty - 0.1f * s, 0, 0.8f * s, 60, 0.5f * s, s, color_btn_xy, PIN_X, glow[PIN_X]);
    // Z well above the pair's middle: its bean hangs down to the right, so
    // its weight lines up with theirs only when its top is a little above Y's
    Ring_Kidney(cx + tx, ty + 0.4f * s, 45, 0.9f * s, 65, 0.55f * s, s, color_btn_z, PIN_Z, glow[PIN_Z]);
    Ring_Button(cx + lx, ly, 0.68f * s, s, color_btn_a, PIN_A, glow[PIN_A]);
    Ring_Button(cx - lx, ly, 0.5f * s, s, color_btn_b, PIN_B, glow[PIN_B]);

    Ring_CStick(pad, cx, cy + RING_C_Y * s, s);
    pad_soft = 0;
}
