/* landinglab/hud_drawing.c
 *
 * Holds drawing on the screen: HUD quads, shapes, rings, lines and text.
 *
 * Part of Landing Gear: src/landinglab.c includes it, in order, into one
 * translation unit, so it sees everything included before it (and mex.h,
 * events.h, landinglab_sim.h).
 *
 * Uses: camera_views, drawing.
 */

///////////////////////
/// HUD drawing     ///
///////////////////////

// HUD units are square on screen: x runs from about -29.4 to 29.4 and y
// from -24.1 (bottom) to 24.1. World points are projected onto it.
#define HUD_PX 10.85f // 640 x 480 viewport pixels per HUD unit, across
#define HUD_PY 9.96f  // ... and down
#define HUD_W 29.4f
#define HUD_H 24.1f
#define SAFE_W 27.4f // what stays on screen with any crop: TM-CE's own panels reach about 27.7
#define SAFE_H 22.f
#define PX 0.095f // about one pixel of the 640 x 480 picture

static int Hud_FromWorld(float x, float y, float *hx, float *hy)
{
    Vec3 in = {x, y, 0}, out;
    HSD_GXProject(View_CObj(), &in, &out, 1);
    *hx = (out.X - 320.f) / HUD_PX;
    *hy = (240.f - out.Y) / HUD_PY;
    return *hx > -HUD_W - 10.f && *hx < HUD_W + 10.f && *hy > -HUD_H - 10.f && *hy < HUD_H + 10.f;
}

// Shapes are queued as quads and drawn in one go, in the order queued.
#define LL_QUADS 2800
typedef struct Quad
{
    Vec2 v[4];
    GXColor c, c2; // c at the first two corners, c2 at the last two
} Quad;
static Quad *quads; // [LL_QUADS], allocated in Event_Init
static int quad_num;
static int quad_peak; // the most queued in one draw, for the log

// A quad shaded from c along its first edge to c2 along its last, as the
// soft edges use it.
static void Quad_Add2(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3, GXColor c, GXColor c2)
{
    if ((c.a == 0 && c2.a == 0) || quad_num >= LL_QUADS)
        return;
    Quad *q = &quads[quad_num++];
    q->v[0] = (Vec2){x0, y0};
    q->v[1] = (Vec2){x1, y1};
    q->v[2] = (Vec2){x2, y2};
    q->v[3] = (Vec2){x3, y3};
    q->c = Vis(c);
    q->c2 = Vis(c2);
}

static void Quad_Add(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3, GXColor c)
{
    Quad_Add2(x0, y0, x1, y1, x2, y2, x3, y3, c, c);
}

static void Quad_Flush(void)
{
    if (quad_num == 0)
        return;
    event_vars->GFX_Start(quad_num * 4, (GFX_Params){.shape = GX_QUADS});
    for (int i = 0; i < quad_num; i++)
    {
        Quad *q = &quads[i];
        for (int j = 0; j < 4; j++)
            GFX_AddVtx(q->v[j].X, q->v[j].Y, 0, j < 2 ? q->c : q->c2);
    }
    quad_num = 0;
}

static void Hud_Rect(float x0, float y0, float x1, float y1, GXColor c)
{
    Quad_Add(x0, y0, x1, y0, x1, y1, x0, y1, c);
}

// An outline just inside the box.
static void Hud_Frame(float x0, float y0, float x1, float y1, float w, GXColor c)
{
    Hud_Rect(x0, y0, x1, y0 + w, c);
    Hud_Rect(x0, y1 - w, x1, y1, c);
    Hud_Rect(x0, y0 + w, x0 + w, y1 - w, c);
    Hud_Rect(x1 - w, y0 + w, x1, y1 - w, c);
}

static void Hud_Seg(float ax, float ay, float bx, float by, float w, GXColor c)
{
    float dx = bx - ax, dy = by - ay;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.0001f)
        return;
    float nx = -dy / len * w / 2, ny = dx / len * w / 2;
    Quad_Add(ax + nx, ay + ny, bx + nx, by + ny, bx - nx, by - ny, ax - nx, ay - ny, c);
}

static void Hud_Tri(float ax, float ay, float bx, float by, float cx, float cy, GXColor c)
{
    Quad_Add(ax, ay, bx, by, cx, cy, cx, cy, c);
}

static void Hud_Diamond(float x, float y, float r, GXColor c)
{
    Quad_Add(x, y - r, x + r, y, x, y + r, x - r, y, c);
}

#define DISC_SEGS 16

// A filled circle: a fan of triangles.
static void Hud_Disc(float cx, float cy, float r, GXColor c)
{
    float px = cx + r, py = cy;
    for (int i = 1; i <= DISC_SEGS; i++)
    {
        float ang = i * (6.2831853f / DISC_SEGS);
        float nx = cx + cos(ang) * r, ny = cy + sin(ang) * r;
        Hud_Tri(cx, cy, px, py, nx, ny, c);
        px = nx;
        py = ny;
    }
}

// A circle's outline, w wide and centered on radius r.
static void Hud_Ring(float cx, float cy, float r, float w, GXColor c)
{
    float r0 = r - w / 2, r1 = r + w / 2;
    float px = 1.f, py = 0.f;
    for (int i = 1; i <= DISC_SEGS; i++)
    {
        float ang = i * (6.2831853f / DISC_SEGS);
        float nx = cos(ang), ny = sin(ang);
        Quad_Add(cx + px * r0, cy + py * r0, cx + px * r1, cy + py * r1, cx + nx * r1, cy + ny * r1, cx + nx * r0, cy + ny * r0, c);
        px = nx;
        py = ny;
    }
}

// A line along points, w wide, its sides fading out over f so it doesn't
// look jagged. Joints are mitered, so nothing overlaps (overlaps show as
// beads along a translucent line). At most LINE_MAX points.
#define LINE_MAX 64
static void Hud_Line(const float *px, const float *py, int n, int closed, float w, float f, GXColor c)
{
    if (n < 2 || n > LINE_MAX)
        return;
    float nx[LINE_MAX], ny[LINE_MAX];
    for (int i = 0; i < n; i++)
    {
        int a = i > 0 ? i - 1 : closed ? n - 1 : 0;
        int b = i < n - 1 ? i + 1 : closed ? 0 : n - 1;
        // the normals of the edges in and out, and their mean, stretched to
        // keep the line's width at the corner
        float ax = px[i] - px[a], ay = py[i] - py[a], bx = px[b] - px[i], by = py[b] - py[i];
        float la = sqrtf(ax * ax + ay * ay), lb = sqrtf(bx * bx + by * by);
        if (la < 0.0001f)
            ax = bx, ay = by, la = lb;
        if (lb < 0.0001f)
            bx = ax, by = ay, lb = la;
        if (la < 0.0001f)
            return;
        float n0x = -ay / la, n0y = ax / la, n1x = -by / lb, n1y = bx / lb;
        float mx = n0x + n1x, my = n0y + n1y, m = sqrtf(mx * mx + my * my);
        if (m < 0.0001f)
            mx = n0x, my = n0y, m = 1;
        mx /= m;
        my /= m;
        float d = mx * n0x + my * n0y;
        float k = d > 0.5f ? 1.f / d : 2.f;
        nx[i] = mx * k;
        ny[i] = my * k;
    }
    GXColor clear = {0, 0, 0, 0};
    float h = w / 2, o = h + f;
    int segs = closed ? n : n - 1;
    for (int i = 0; i < segs; i++)
    {
        int j = (i + 1) % n;
        if (w > 0)
            Quad_Add(px[i] + nx[i] * h, py[i] + ny[i] * h, px[j] + nx[j] * h, py[j] + ny[j] * h,
                     px[j] - nx[j] * h, py[j] - ny[j] * h, px[i] - nx[i] * h, py[i] - ny[i] * h, c);
        Quad_Add2(px[i] + nx[i] * h, py[i] + ny[i] * h, px[j] + nx[j] * h, py[j] + ny[j] * h,
                  px[j] + nx[j] * o, py[j] + ny[j] * o, px[i] + nx[i] * o, py[i] + ny[i] * o, c, clear);
        Quad_Add2(px[i] - nx[i] * h, py[i] - ny[i] * h, px[j] - nx[j] * h, py[j] - ny[j] * h,
                  px[j] - nx[j] * o, py[j] - ny[j] * o, px[i] - nx[i] * o, py[i] - ny[i] * o, c, clear);
    }
}

// A filled fan from (cx, cy) over the points of a closed outline.
static void Hud_Fan(float cx, float cy, const float *px, const float *py, int n, GXColor c)
{
    for (int i = 0; i < n; i++)
    {
        int j = (i + 1) % n;
        Hud_Tri(cx, cy, px[i], py[i], px[j], py[j], c);
    }
}

// A circle's points, counterclockwise from the right.
static void Circle_Points(float cx, float cy, float r, int n, float *px, float *py)
{
    for (int i = 0; i < n; i++)
    {
        float ang = i * (6.2831853f / n);
        px[i] = cx + cos(ang) * r;
        py[i] = cy + sin(ang) * r;
    }
}

// Text with its line's bottom at y, as the info panels use it (rows 2.5
// units tall), starting at x (align 0) or ending there (align 2). The HUD's
// texts are made with the alignment of their first use, so it's set again
// on every call.
static void Hud_TextAligned(const char *text, float x, float y, float size, GXColor color, int align)
{
    if (!event_vars->HUD_DrawTextEx)
        return; // a TM-CE menu from before it was set
    HUDCamData *hud = event_vars->hudcam_gobj->userdata;
    int slot = hud->text_cache_used;
    Rect r = {x, y, 0, 2.5f};
    color = Vis(color); // text fades with what it's part of, like the shapes
    event_vars->HUD_DrawTextEx(text, &r, size, color, (GXColor){0, 0, 0, 0}, 0, 0);
    if (slot < (int)countof(hud->text_cache) && hud->text_cache[slot])
    {
        Text *t = hud->text_cache[slot];
        t->align = align;
        t->use_aspect = 0;       // fitting to the HUD's 8-unit box squeezes long lines
        t->is_depth_compare = 0; // the stage's depth would hide it where the camera is close
    }
}

// (unused since the cell strip lost its numbers; kept for the next text)
__attribute__((unused)) static void Hud_Text(const char *text, float x, float y, float size, GXColor color)
{
    Hud_TextAligned(text, x, y, size, color, 0);
}

// A line of text's width, for the dark plate behind it: Melee's font is
// proportional, so each glyph by its kind (about 6.5 px for an average
// glyph at size 0.45, as measured in game).
static float Text_Width(const char *text, float size)
{
    float w = 0;
    for (const char *c = text; *c; c++)
    {
        char ch = *c;
        float g;
        if (ch == ' ')
            g = 0.9f;
        else if (strchr("iIl1.,:;'!|", ch))
            g = 0.7f;
        else if (strchr("fjrt()[]-/", ch))
            g = 1.0f;
        else if (ch == 'm' || ch == 'w')
            g = 1.9f;
        else if (ch == 'M' || ch == 'W')
            g = 2.0f;
        else if (ch >= 'A' && ch <= 'Z')
            g = 1.6f;
        else if (ch >= '0' && ch <= '9')
            g = 1.35f;
        else if (ch >= 'a' && ch <= 'z')
            g = 1.3f;
        else
            g = 1.4f;
        w += g;
    }
    return w * size;
}

// The dark plate behind a line of text whose row starts at y (rows 2.5
// tall), from x0 to x1: solid enough to read over a light stage.
static void Text_Plate(float x0, float x1, float y)
{
    Hud_Rect(x0, y + 0.15f, x1, y + 2.35f, Color_Over(color_plate, 0.72f));
}

// The y to draw a line of text at so its middle is at mid: a row is 2.5
// tall at size 0.42, and smaller text sits lower in it.
static float Text_MidY(float mid, float size)
{
    return mid - 1.25f + (0.42f - size) * 2.f;
}

// A plate from x0 to x1, half tall above and below mid.
__attribute__((unused)) static void Text_PlateAt(float x0, float x1, float mid, float half)
{
    Hud_Rect(x0, mid - half, x1, mid + half, Color_Over(color_plate, 0.72f));
}
