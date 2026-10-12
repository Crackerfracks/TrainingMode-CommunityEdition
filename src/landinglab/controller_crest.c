/* landinglab/controller_crest.c
 *
 * Holds the controller display's Crest look.
 *
 * Part of Landing Lab: src/landinglab.c includes it, in order, into one
 * translation unit, so it sees everything included before it (and mex.h,
 * events.h, landinglab_sim.h).
 *
 * Uses: option_values, menus_display, drawing, hud_drawing, controller,
 * controller_ring.
 */

// The Crest look: the controller as a crest, a shield with a wing off each
// shoulder and the C-stick for a tail. Each wing is a trigger: four swept
// blades, their upper edges curved like the trigger's finger groove. A press
// fills every blade from its tip in toward the shoulder, the top blade
// leading: gray until the game counts the press, cyan after, with a notch
// across each blade where that happens. The click lights the spine at the
// wing's root and, as in the Ring, the whole wing. The buttons are cut gems
// in the controller's colors: long pointed X and Y stacked on the left, Z on
// the right, round B and A at the bottom, each with a lighter table. Units
// are the drawing's (see CREST_K), the left wing built and the right one its
// mirror.
#define CREST_ROOT 1.95f   // the wings' roots, either side of the middle
#define CREST_BLADE_H 0.5f // a blade's height at its root
#define CREST_SEGS 6       // segments along a blade
#define CREST_LEAD 1.24f   // the top blade's fill runs this far ahead of the press
#define CREST_LAG 0.08f    // and each blade under it this much behind the one above

static const float crest_blades[4][3] = {
    // the root's top, the sweep in degrees, the length
    {2.05f, 150, 3.2f},
    {1.4f, 158, 2.8f},
    {0.75f, 166, 2.4f},
    {0.1f, 174, 2.f},
};

// The shield behind it all, clockwise from its top left; a fan from its
// middle covers it.
static const float crest_plate[][2] = {
    {-1.9f, 2.45f}, {1.9f, 2.45f}, {2.05f, -0.35f}, {3.75f, -0.5f}, {3.1f, -2.62f}, {1.15f, -3.3f},
    {0, -3.72f}, {-1.15f, -3.3f}, {-3.1f, -2.62f}, {-3.75f, -0.5f}, {-2.05f, -0.35f},
};

// The spine at the left wing's root, with a talon at its foot.
static const float crest_spine[6][2] = {
    {-1.93f, 2.2f}, {-1.65f, 2.05f}, {-1.65f, -0.25f}, {-1.9f, -0.85f}, {-2.07f, -0.2f}, {-2.07f, 2.05f},
};

// A point of blade i on its top edge (or its bottom one), u of the way from
// its root (0) to its tip (1), in HUD units: m is -1 for the left wing and 1
// for the right one, k the drawing's scale.
static void Crest_BladePoint(int i, float u, int top, float m, float k, float cx, float cy, float *x, float *y)
{
    float yt = crest_blades[i][0], ang = crest_blades[i][1] * 0.01745329f, len = crest_blades[i][2];
    float dx = cos(ang), dy = sin(ang);
    float tx = -CREST_ROOT + dx * len, ty = yt - CREST_BLADE_H / 2 + dy * len;
    float rx = -CREST_ROOT, ry = top ? yt : yt - CREST_BLADE_H;
    float px = rx + (tx - rx) * u, py = ry + (ty - ry) * u;
    // both edges swell a little between root and tip, the top one more and
    // nearer the root, like the trigger's dish
    if (top)
    {
        float d = 0.16f * sin(3.1415927f * sqrtf(u) * sqrtf(sqrtf(u)));
        px += dy * d;
        py -= dx * d;
    }
    else
    {
        float d = 0.07f * sin(3.1415927f * u);
        px += dy * d;
        py += dx * d;
    }
    *x = cx - m * px * k;
    *y = cy + py * k;
}

// The blade between u0 and u1 along it, as quads between its edges.
static void Crest_BladeQuads(int i, float u0, float u1, float m, float k, float cx, float cy, GXColor c)
{
    float tx0, ty0, bx0, by0;
    Crest_BladePoint(i, u0, 1, m, k, cx, cy, &tx0, &ty0);
    Crest_BladePoint(i, u0, 0, m, k, cx, cy, &bx0, &by0);
    for (int j = 1; j <= CREST_SEGS; j++)
    {
        float u = u0 + (u1 - u0) * j / CREST_SEGS, tx1, ty1, bx1, by1;
        Crest_BladePoint(i, u, 1, m, k, cx, cy, &tx1, &ty1);
        Crest_BladePoint(i, u, 0, m, k, cx, cy, &bx1, &by1);
        Quad_Add(tx0, ty0, tx1, ty1, bx1, by1, bx0, by0, c);
        tx0 = tx1, ty0 = ty1, bx0 = bx1, by0 = by1;
    }
}

// One wing: side -1 is L, 1 is R; analog, click, lit and flash as for the Ring.
static void Crest_Wing(float cx, float cy, float k, int side, float analog, int click, float lit, float flash)
{
    {
        float mx, my;
        Crest_BladePoint(1, 0.5f, 1, side, k, cx, cy, &mx, &my);
        Pin_Spot(side > 0 ? PIN_R : PIN_L, mx, my, 0.9f * k);
    }
    float m = side;
    float a = click ? 1.f : Clamp01(analog);
    int counted = a >= RING_LIGHT;
    GXColor cyan = color_in_dodge;
    GXColor fc = click ? Color_Over(Color_Mix(cyan, color_white, 0.5f), 1.f) : counted ? Color_Fill(cyan, 0.6f + 0.35f * a) : Color_Over(color_skip, 0.55f);
    float w = 0.68f * lit + (1.f - 0.68f * lit) * flash;
    for (int i = 0; i < 4; i++)
    {
        // its outline: out along the top edge, back along the bottom one
        float px[2 * CREST_SEGS + 2], py[2 * CREST_SEGS + 2];
        int n = 2 * CREST_SEGS + 2;
        for (int j = 0; j <= CREST_SEGS; j++)
        {
            float u = (float)j / CREST_SEGS;
            Crest_BladePoint(i, u, 1, m, k, cx, cy, &px[j], &py[j]);
            Crest_BladePoint(i, u, 0, m, k, cx, cy, &px[n - 1 - j], &py[n - 1 - j]);
        }
        Crest_BladeQuads(i, 0, 1, m, k, cx, cy, Color_Fill(color_plate, 0.62f));
        // a lighter facet along the top edge, an edge that catches the light
        for (int j = 0; j < CREST_SEGS; j++)
        {
            int b0 = n - 1 - j, b1 = n - 2 - j;
            float mx0 = px[j] * 0.62f + px[b0] * 0.38f, my0 = py[j] * 0.62f + py[b0] * 0.38f;
            float mx1 = px[j + 1] * 0.62f + px[b1] * 0.38f, my1 = py[j + 1] * 0.62f + py[b1] * 0.38f;
            Quad_Add(px[j], py[j], px[j + 1], py[j + 1], mx1, my1, mx0, my0, Color_Over(color_white, 0.07f));
        }
        // the press, from the tip in
        float f = Clamp01(a * CREST_LEAD - i * CREST_LAG);
        if (f > 0.003f)
            Crest_BladeQuads(i, 1.f - f, 1.f, m, k, cx, cy, fc);
        // the notch where the fill stands when a light press counts
        float un = 1.f - Clamp01(RING_LIGHT * CREST_LEAD - i * CREST_LAG);
        float nx[2], ny[2];
        Crest_BladePoint(i, un, 1, m, k, cx, cy, &nx[0], &ny[0]);
        Crest_BladePoint(i, un, 0, m, k, cx, cy, &nx[1], &ny[1]);
        Hud_Line(nx, ny, 2, 0, 0.06f * k, PX, Color_Over(color_white, 0.85f));
        Hud_Line(px, py, n, 1, (0.05f + 0.04f * lit) * k, PX, Color_Mix(Color_Fill(cyan, 0.8f), color_white, lit));
        // the click lights the whole wing, as bright as its flash, while held
        if (w > 0.01f)
            Crest_BladeQuads(i, 0, 1, m, k, cx, cy, Color_Over(color_white, w));
    }
    float sx[6], sy[6], mx = 0, my = 0;
    for (int i = 0; i < 6; i++)
    {
        sx[i] = cx - m * crest_spine[i][0] * k;
        sy[i] = cy + crest_spine[i][1] * k;
        mx += sx[i] / 6;
        my += sy[i] / 6;
    }
    Hud_Fan(mx, my, sx, sy, 6, click ? color_white : Color_Over(Color_Mix(color_plate, cyan, 0.25f), 0.9f));
    Hud_Line(sx, sy, 6, 1, 0.05f * k, PX, Color_Over(Color_Mix(cyan, color_white, lit), 0.85f));
}

#define GEM_MAX 18

// A long pointed gem's outline (a marquise) around (gx, gy): l long, w wide,
// turned ang degrees. 2 * GEM_SIDE points.
#define GEM_SIDE 8
static void Gem_Marquise(float gx, float gy, float l, float w, float ang, float *px, float *py)
{
    float c = cos(ang * 0.01745329f), sn = sin(ang * 0.01745329f);
    for (int i = 0; i < 2 * GEM_SIDE; i++)
    {
        // along the top from the left point, then back along the bottom
        float u = i <= GEM_SIDE ? (float)i / GEM_SIDE : (float)(2 * GEM_SIDE - i) / GEM_SIDE;
        float v = sin(3.1415927f * u);
        float h = w / 2 * sqrtf(v) * sqrtf(sqrtf(v)) * (i <= GEM_SIDE ? 1 : -1);
        float x = -l / 2 + l * u;
        px[i] = gx + x * c - h * sn;
        py[i] = gy + x * sn + h * c;
    }
}

// A gem as a button: the press ghost, the fill, the outline, the table and
// the letter. Its outline (n points) and table (tn points) go round (gx, gy).
static void Gem_Button(const float *px, const float *py, int n, const float *tx, const float *ty, int tn, float gx, float gy,
                       float k, float letter, GXColor c, int in, float glow)
{
    float flash = pad_flash[in];
    if (flash > GHOST_MIN)
    {
        float g = Ghost_Grow(flash), qx[GEM_MAX], qy[GEM_MAX];
        for (int i = 0; i < n; i++)
        {
            qx[i] = gx + (px[i] - gx) * g;
            qy[i] = gy + (py[i] - gy) * g;
        }
        Hud_Fan(gx, gy, qx, qy, n, Ghost_Color(c, flash));
    }
    Hud_Fan(gx, gy, px, py, n, Ring_Lit(c, glow));
    Hud_Line(px, py, n, 1, 0.07f * k, PX, Ring_Edge(c));
    Hud_Fan(gx, gy, tx, ty, tn, Color_Over(color_white, 0.16f + 0.1f * glow));
    Ring_Letter(in, gx, gy, letter, c, glow);
}

static void Crest_Marquise(float cx, float cy, float k, float gx, float gy, float l, float w, float ang, float letter,
                           GXColor c, int in, float glow)
{
    float px[2 * GEM_SIDE], py[2 * GEM_SIDE], tx[2 * GEM_SIDE], ty[2 * GEM_SIDE];
    gx = cx + gx * k;
    gy = cy + gy * k;
    Gem_Marquise(gx, gy, l * k, w * k, ang, px, py);
    Gem_Marquise(gx, gy, 0.59f * l * k, 0.43f * w * k, ang, tx, ty);
    Gem_Button(px, py, 2 * GEM_SIDE, tx, ty, 2 * GEM_SIDE, gx, gy, k, letter * k, c, in, glow);
}

// A round gem with an eight-sided table.
static void Crest_Round(float cx, float cy, float k, float gx, float gy, float r, float letter, GXColor c, int in, float glow)
{
    float px[GEM_MAX], py[GEM_MAX], tx[8], ty[8];
    gx = cx + gx * k;
    gy = cy + gy * k;
    Circle_Points(gx, gy, r * k, GEM_MAX, px, py);
    for (int i = 0; i < 8; i++)
    {
        float ang = (i + 0.5f) * 0.7853982f;
        tx[i] = gx + cos(ang) * 0.6f * r * k;
        ty[i] = gy + sin(ang) * 0.6f * r * k;
    }
    Gem_Button(px, py, GEM_MAX, tx, ty, 8, gx, gy, k, letter * k, c, in, glow);
}

// The Crest look, from the box's bottom left corner.
static void Crest_Draw(FighterData *fp, HSD_Pad *pad, float bx, float by)
{
    float k = CREST_K * ring_sizes[Options_Hud[HOPT_PAD_SIZE].val];
    float cx = bx + CREST_W * k, cy = by + CREST_BOT * k;

    int held[PIN_COUNT];
    Pad_Held(pad, held);
    float glow[PIN_COUNT];
    for (int i = 0; i < PIN_COUNT; i++)
        glow[i] = held[i] ? 1.f : pad_glow[i];

    pad_soft = 1;
    int n = countof(crest_plate);
    float px[countof(crest_plate)], py[countof(crest_plate)];
    for (int i = 0; i < n; i++)
    {
        px[i] = cx + crest_plate[i][0] * k;
        py[i] = cy + crest_plate[i][1] * k;
    }
    Hud_Fan(cx, cy - 0.4f * k, px, py, n, Color_Fill(color_plate, 0.5f));
    Hud_Line(px, py, n, 1, 0.05f * k, PX, Color_Over(color_white, 0.22f));
    Crest_Wing(cx, cy, k, -1, pad->ftriggerLeft, held[PIN_L], glow[PIN_L], pad_flash[PIN_L]);
    Crest_Wing(cx, cy, k, 1, pad->ftriggerRight, held[PIN_R], glow[PIN_R], pad_flash[PIN_R]);

    Pad_Stick(fp, pad, cx, cy + 0.6f * k, 1.35f * k, 0.7f * k);
    // X and Y stacked on the left, Y above, clear of each other and of B
    Crest_Marquise(cx, cy, k, -2.88f, -0.92f, 1.25f, 0.56f, -25, 0.3f, color_btn_xy, PIN_Y, glow[PIN_Y]);
    Crest_Marquise(cx, cy, k, -2.62f, -1.74f, 1.25f, 0.56f, -25, 0.3f, color_btn_xy, PIN_X, glow[PIN_X]);
    Crest_Marquise(cx, cy, k, 2.62f, -1.12f, 1.6f, 0.64f, 25, 0.42f, color_btn_z, PIN_Z, glow[PIN_Z]);
    Crest_Round(cx, cy, k, -1.47f, -2.42f, 0.48f, 0.44f, color_btn_b, PIN_B, glow[PIN_B]);
    Crest_Round(cx, cy, k, 1.55f, -2.32f, 0.62f, 0.56f, color_btn_a, PIN_A, glow[PIN_A]);
    Ring_CStick(pad, cx, cy - 2.35f * k, 0.9f * k);
    pad_soft = 0;
}
