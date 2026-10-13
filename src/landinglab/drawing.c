/* landinglab/drawing.c
 *
 * Holds drawing in the stage: intensity and fade, paths, ECBs, window bars,
 * colors.
 *
 * Part of Landing Gear: src/landinglab.c includes it, in order, into one
 * translation unit, so it sees everything included before it (and mex.h,
 * events.h, landinglab_sim.h).
 *
 * Uses: menus_display, presets, tracking_state, hud_text.
 */

///////////////////////
/// Drawing         ///
///////////////////////

static const GXColor color_neutral = {150, 150, 150, 255};
static const GXColor color_body = {130, 150, 200, 255};
static const GXColor color_ecb = {255, 230, 0, 255};

// World shapes sit at Falcon's depth (z 0) and are tested against what's
// already drawn, so his model and the stage in front of him hide them, as
// if the lines ran through his middle. Some stay on top of everything:
// ledge routes and the inputs along the paths, which his model would hide
// where they matter, and the collision display. Like the event's own
// GFX_Start, with the depth test left on.
static int world_on_top;
static int world_add; // glows: added to what's behind, so a faint one never darkens it

// Intensity: the cues, paths and timers are drawn fainter or bolder than
// the standard look (level 3); the controller and the panel never change.
// Fainter takes color and opacity down together. Bolder makes a shape more
// opaque, and brighter as far as its strongest channel allows, so no color
// shifts toward white and a hit can't be mistaken for a miss.
static const float vis_levels[] = {0.55f, 0.78f, 1.f, 1.3f, 1.65f};
static float vis_k = 1.f; // for what's being drawn now

static GXColor Vis(GXColor c)
{
    if (vis_k == 1.f)
        return c;
    // dimmer: only the alpha goes down (by k squared, as bright as scaling
    // both had looked), so a faded shape fades into what's behind it instead
    // of leaving a dark shadow of itself
    float ka = vis_k < 1.f ? vis_k * vis_k : vis_k, kc = vis_k < 1.f ? 1.f : vis_k;
    if (vis_k > 1.f)
    {
        int m = c.r > c.g ? c.r : c.g;
        if (c.b > m)
            m = c.b;
        if (c.a * ka > 255.f)
            ka = c.a ? 255.f / c.a : 1.f;
        kc = ka;
        if (m * kc > 255.f)
            kc = m ? 255.f / m : 1.f;
    }
    c.r = c.r * kc;
    c.g = c.g * kc;
    c.b = c.b * kc;
    c.a = c.a * ka;
    return c;
}

// Intensity by group (Options_Intensity), and Auto Fade: each cue kind
// keeps a running hit rate (about the last 10 tries), and in a group set
// to fade, each kind's drawing dims by its own rate (Fade_Factor).
enum { VG_CUES, VG_PATHS, VG_TIMERS, VG_LEDGE, VG_PAD };

static float Group_K(int g)
{
    if (g == VG_PAD)
        return vis_levels[Options_Intensity[IOPT_PAD].val];
    int lv = Options_Cues[COPT_INTENSITY].val + Options_Intensity[g].val - 2;
    lv = lv < 0 ? 0 : lv > 4 ? 4 : lv;
    return vis_levels[lv];
}

static void Skill_Add(int kind, int hit)
{
    skill[kind] += ((hit ? 1.f : 0.f) - skill[kind]) * 0.15f;
    if (cue_log)
        OSReport("LLFADE %d %s rate %.2f\n", kind, hit ? "hit" : "miss", skill[kind]);
}

// How far Auto Fade dims this frame: a kind's hit rate fades it from
// half its tries hit (nothing) to 9 in 10 (down to 35%).
static float Fade_Of(int kind)
{
    float t = (skill[kind] - 0.5f) / 0.4f;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return 1.f - 0.65f * t;
}

// Auto Fade's part of a group's level for one cue kind's drawing (1: none).
static float Fade_Factor(int g, int kind)
{
    int fade = Options_Intensity[IOPT_FADE].val;
    if (kind >= 0 && kind < CUE_NUM &&
        ((g == VG_TIMERS && (fade == FADE_TIMERS || fade == FADE_BOTH)) ||
         (g == VG_CUES && (fade == FADE_CUES || fade == FADE_BOTH))))
        return Fade_Of(kind);
    return 1.f;
}

static float Kind_K(int g, int kind)
{
    return Group_K(g) * Fade_Factor(g, kind);
}

void Event_FadeReset(GOBJ *menu)
{
    memset(skill, 0, sizeof(skill));
    SFX_PlayCommon(1);
}

static void World_Vtx(f32 x, f32 y, f32 z, GXColor c)
{
    GFX_AddVtx(x, y, z, Vis(c));
}

static void World_Start(int count, u8 shape, u8 size)
{
    Mtx mtx;
    HSD_ClearVtxDesc();
    GXSetCurrentMtx(0);
    COBJ_GetViewingMtx(COBJ_GetCurrent(), &mtx);
    GXLoadPosMtxImm(mtx, 0);
    HSD_StateSetLineWidth(size, 5);
    HSD_StateSetPointSize(size, 5);
    // blended, no depth writes, and the depth test off (on top) or less-equal
    HSD_SetupRenderMode(world_on_top ? 0x68000002 : 0x60000002);
    // the same depth mode again, straight to GX: something that set it
    // directly in between would leave the cached state stale
    GXSetZMode(1, world_on_top ? GX_ALWAYS : GX_LEQUAL, 0);
    if (world_add)
        HSD_StateSetBlendMode(GX_BM_BLEND, GX_BL_ONE, GX_BL_ONE, GX_LO_NOOP);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    HSD_StateSetCullMode(0);
    GXBegin(shape, GX_VTXFMT0, count);
}

// The paths flow: a brighter band keeps running from Falcon to the path's
// end over a dimmer line, so the lines move like the timers do.
#define LL_FLOW_LEN 6.f

static GXColor Path_Flow(GXColor c, int i, int from, int to)
{
    int len = to - from + (int)(LL_FLOW_LEN * 2);
    float head = (float)((event_vars->game_timer * 2) % len) - LL_FLOW_LEN;
    float d = fabs((i - from) - head);
    float k = 0.55f + (d < LL_FLOW_LEN ? 0.45f * (1.f - d / LL_FLOW_LEN) : 0);
    c.r = c.r * k;
    c.g = c.g * k;
    c.b = c.b * k;
    c.a = c.a * k;
    return c;
}

static void Draw_Path(Vec2 *pos, float *bottom, int from, int to, GXColor color, u8 size)
{
    int count = to - from + 1;
    if (count < 2)
        return;

    World_Start(count, GX_LINESTRIP, size);
    for (int i = from; i <= to; i++)
        World_Vtx(pos[i].X, pos[i].Y + bottom[i], 0, Path_Flow(color, i, from, to));
}

// The paths look different from each other: the landing path is solid, the
// short hop preview dashed (a dash every other frame, so it stays readable
// where it runs along the full hop's line) and the body path dotted.
#define LINE_SOLID 0
#define LINE_DASHED 1

static void Draw_Dashed(Vec2 *pos, float *bottom, int from, int to, GXColor color, u8 size)
{
    int dashes = (to - from + 1) / 2;
    if (dashes < 1)
        return;

    World_Start(dashes * 2, GX_LINES, size);
    for (int i = from; i + 1 <= to; i += 2)
    {
        World_Vtx(pos[i].X, pos[i].Y + bottom[i], 0, Path_Flow(color, i, from, to));
        World_Vtx(pos[i + 1].X, pos[i + 1].Y + bottom[i + 1], 0, Path_Flow(color, i + 1, from, to));
    }
}

// Frame Dots: a small ring on the path at every frame; their spacing shows
// the speed. Only during Frame Advance unless set to always, so the paths
// stay light at full speed.
#define LL_DOT_R 0.42f

static int Ticks_On(void)
{
    int opt = Options_Paths[POPT_TICKS].val;
    return opt == 1 || (opt == 0 && Options_Game[GOPT_FRAME_ADV].val);
}

static void Draw_Ticks(Vec2 *pos, float *bottom, int from, int to, GXColor color, u8 size)
{
    int count = to - from + 1;
    if (count < 2)
        return;

    World_Start(count * 8, GX_LINES, size);
    for (int i = from; i <= to; i++)
    {
        float x = pos[i].X;
        float y = pos[i].Y + bottom[i];
        float r = LL_DOT_R;
        World_Vtx(x, y - r, 0, color);
        World_Vtx(x + r, y, 0, color);
        World_Vtx(x + r, y, 0, color);
        World_Vtx(x, y + r, 0, color);
        World_Vtx(x, y + r, 0, color);
        World_Vtx(x - r, y, 0, color);
        World_Vtx(x - r, y, 0, color);
        World_Vtx(x, y - r, 0, color);
    }
}

static void Draw_Line(Prediction *p, int from, int to, GXColor color, u8 size, int style)
{
    if (style == LINE_DASHED)
        Draw_Dashed(p->pos, p->bottom, from, to, color, size);
    else
        Draw_Path(p->pos, p->bottom, from, to, color, size);
    if (Ticks_On())
        Draw_Ticks(p->pos, p->bottom, from, to, color, 9);
}

// The fighter's position raised by a fixed amount: a smooth arc through his
// body, unlike the ECB bottom, which moves with his legs. Drawn as a dot on
// every frame.
#define LL_DOT 0.4f

static void Draw_BodyPath(Prediction *p, int from, int last, GXColor c)
{
    int count = last - from + 1;
    if (count < 2)
        return;

    World_Start(count * 4, GX_QUADS, 0);
    for (int i = from; i <= last; i++)
    {
        float x = p->pos[i].X, y = p->pos[i].Y + body_offset;
        World_Vtx(x - LL_DOT, y - LL_DOT, 0, c);
        World_Vtx(x + LL_DOT, y - LL_DOT, 0, c);
        World_Vtx(x + LL_DOT, y + LL_DOT, 0, c);
        World_Vtx(x - LL_DOT, y + LL_DOT, 0, c);
    }
}

static void Draw_Ecb(Vec2 pos, float bottom, EcbSample *s, float facing, GXColor color)
{
    if (!s->has_shape)
    {
        // shape unknown: a small cross at the touchdown point
        float y = pos.Y + bottom;
        World_Start(4, GX_LINES, 24);
        World_Vtx(pos.X - 1.5f, y - 1.5f, 0, color);
        World_Vtx(pos.X + 1.5f, y + 1.5f, 0, color);
        World_Vtx(pos.X - 1.5f, y + 1.5f, 0, color);
        World_Vtx(pos.X + 1.5f, y - 1.5f, 0, color);
        return;
    }

    float right_x = facing > 0 ? s->front : -s->back;
    float left_x = facing > 0 ? s->back : -s->front;

    World_Start(5, GX_LINESTRIP, 24);
    World_Vtx(pos.X, pos.Y + s->top, 0, color);
    World_Vtx(pos.X + right_x, pos.Y + s->side_y, 0, color);
    World_Vtx(pos.X, pos.Y + bottom, 0, color);
    World_Vtx(pos.X + left_x, pos.Y + s->side_y, 0, color);
    World_Vtx(pos.X, pos.Y + s->top, 0, color);
}

// Falcon's ECB right now, for the collision view (his model is hidden).
static void Draw_CurrentEcb(FighterData *fp)
{
    CollData *cd = &fp->coll_data;
    float x = fp->phys.pos.X;
    float y = fp->phys.pos.Y;

    World_Start(5, GX_LINESTRIP, 24);
    World_Vtx(x + cd->ecbCurrCorrect_top.X, y + cd->ecbCurrCorrect_top.Y, 0, color_ecb);
    World_Vtx(x + cd->ecbCurrCorrect_right.X, y + cd->ecbCurrCorrect_right.Y, 0, color_ecb);
    World_Vtx(x + cd->ecbCurrCorrect_bot.X, y + cd->ecbCurrCorrect_bot.Y, 0, color_ecb);
    World_Vtx(x + cd->ecbCurrCorrect_left.X, y + cd->ecbCurrCorrect_left.Y, 0, color_ecb);
    World_Vtx(x + cd->ecbCurrCorrect_top.X, y + cd->ecbCurrCorrect_top.Y, 0, color_ecb);
}

// The aerial pickers (Compass_Draw), queued here while the stage is drawn
// and drawn on the HUD after it.
#define CP_MAX 4
typedef struct Compass
{
    float x, y;
    u8 mask;
    s8 facing;
    GXColor color;
} Compass;
static Compass compass[CP_MAX];
static int compass_num;

static void Compass_Queue(float x, float y, u8 mask, float facing, GXColor color)
{
    if (compass_num >= CP_MAX || !mask)
        return;
    compass[compass_num++] = (Compass){x, y, mask, facing > 0 ? 1 : -1, color};
}

// Windows after the next one of their kind: the same color at 40%
// (premultiplied).
static GXColor Color_Fill(GXColor c, float a);

static GXColor Color_Dim(GXColor c)
{
    c.r = c.r * 2 / 5;
    c.g = c.g * 2 / 5;
    c.b = c.b * 2 / 5;
    c.a = 102;
    return c;
}

// A thick stretch of the path over frames k to e, from half a frame before
// the window to half a frame after it.
// A window's stretch of the path as a ribbon half wide on each side, square
// to the path everywhere. (A wide GX line only widens along x or y, picking
// one by its slope, so a drifting path's window used to come out as an odd
// slab that changed shape with the drift. Stephen, 0.8.4.)
#define BAR_PTS 32
static void Draw_Bar(Prediction *p, int k, int e, GXColor color, float half)
{
    float x[BAR_PTS], y[BAR_PTS], nx[BAR_PTS], ny[BAR_PTS];
    if (e - k + 3 > BAR_PTS)
        e = k + BAR_PTS - 3;
    int before = k - 1;
    int after = e < p->num ? e + 1 : e;
    int n = 0;
    x[n] = (p->pos[before].X + p->pos[k].X) / 2;
    y[n++] = (p->pos[before].Y + p->bottom[before] + p->pos[k].Y + p->bottom[k]) / 2;
    for (int i = k; i <= e; i++)
    {
        x[n] = p->pos[i].X;
        y[n++] = p->pos[i].Y + p->bottom[i];
    }
    x[n] = (p->pos[e].X + p->pos[after].X) / 2;
    y[n++] = (p->pos[e].Y + p->bottom[e] + p->pos[after].Y + p->bottom[after]) / 2;

    for (int i = 0; i < n; i++)
    {
        int a = i > 0 ? i - 1 : i, b = i < n - 1 ? i + 1 : i;
        float tx = x[b] - x[a], ty = y[b] - y[a];
        float len = sqrtf(tx * tx + ty * ty);
        if (len < 0.05f)
        {
            // standing still (the top of a straight jump): across, as the
            // last one was
            nx[i] = i > 0 ? nx[i - 1] : half;
            ny[i] = i > 0 ? ny[i - 1] : 0;
            continue;
        }
        nx[i] = -ty / len * half;
        ny[i] = tx / len * half;
        // the same side as the last one, so the ribbon never twists where
        // the path turns back (up, then down the same line)
        if (i > 0 && nx[i] * nx[i - 1] + ny[i] * ny[i - 1] < 0)
        {
            nx[i] = -nx[i];
            ny[i] = -ny[i];
        }
    }

    World_Start((n - 1) * 4, GX_QUADS, 6);
    for (int i = 0; i + 1 < n; i++)
    {
        World_Vtx(x[i] - nx[i], y[i] - ny[i], 0, color);
        World_Vtx(x[i] + nx[i], y[i] + ny[i], 0, color);
        World_Vtx(x[i + 1] + nx[i + 1], y[i + 1] + ny[i + 1], 0, color);
        World_Vtx(x[i + 1] - nx[i + 1], y[i + 1] - ny[i + 1], 0, color);
    }
}

// The windows on the path, where the bottom of Falcon's ECB (about his
// feet) is on the frames that work. A perfect waveland window is a wide cyan
// ribbon; an aerial interrupt window is a narrower pink bar drawn over it, with
// the aerials that work at its start. Where both work, the pink sits inside
// the white. The next window of each kind is bright, later ones are dim.
static void Draw_Windows(Prediction *p)
{
    int last_ai = p->land_frame ? p->land_frame - 1 : p->num;
    int last_wl = p->land_frame ? p->land_frame : p->num;

    if (Cues_Waveland())
    {
        int index = 0;
        for (int k = 1; k <= last_wl; k++)
        {
            if (!WL_Mask(p, k))
                continue;
            int e = k;
            while (e < last_wl && WL_Mask(p, e + 1))
                e++;

            GXColor color = k >= p->uncertain_from ? color_learning : land_kind_colors[LAND_PERFECT_WL];
            if (index++ > 0)
                color = Color_Dim(color);
            if (Options_Paths[POPT_WINDOWS].val)
                Draw_Bar(p, k, e, color, 1.8f);
            k = e;
        }
    }

    if (Cues_Ai())
    {
        int index = 0;
        for (int k = 1; k <= last_ai; k++)
        {
            if (!p->ai_show[k])
                continue;
            int e = k;
            u8 mask = p->ai_show[k];
            while (e < last_ai && p->ai_show[e + 1])
                mask |= p->ai_show[++e];

            GXColor color = k >= p->uncertain_from ? color_learning : land_kind_colors[LAND_AI];
            if (index++ > 0)
                color = Color_Dim(color);
            if (Options_Paths[POPT_WINDOWS].val)
                Draw_Bar(p, k, e, color, 1.f);
            if (Options_Paths[POPT_INPUTS].val)
                Compass_Queue(p->pos[k].X, p->pos[k].Y + p->bottom[k], mask, p->facing, color);
            k = e;
        }
    }
}

static void Draw_Prediction(Prediction *p, int body, int style)
{
    int last = p->land_frame ? p->land_frame : p->num;
    int certain = p->land_frame && p->uncertain_from > p->land_frame;
    int highlight = certain && Kind_Shown(p->land_kind);
    int from = 0;

    int line = Options_Paths[POPT_PATH].val;

    // an aerial interrupt still ahead: past its window the path only shows
    // where Falcon goes if he skips it (usually on up through the platform
    // it lands on), so it's grayed until then
    int split = last;
    if (Cues_Ai() && p->ai_first && p->ai_first < p->uncertain_from && p->ai_first + p->ai_width - 1 < last)
        split = p->ai_first + p->ai_width - 1;
    GXColor skipped = Color_Fill(color_neutral, 0.45f);

    if (body && Options_Paths[POPT_BODY].val)
    {
        Draw_BodyPath(p, 0, split, color_body);
        if (split < last)
            Draw_BodyPath(p, split + 1, last, Color_Fill(color_body, 0.35f));
    }

    if (line && highlight)
    {
        Draw_Line(p, from, split, land_kind_colors[p->land_kind], 24, style);
        if (split < last)
            Draw_Line(p, split, last, skipped, 12, style);
    }
    else if (line)
    {
        int known = p->uncertain_from - 1;
        if (known > last)
            known = last;
        if (known < from)
            known = from;
        Draw_Line(p, from, known, color_neutral, 12, style);
        Draw_Line(p, known, last, color_learning, 12, style);
    }

    Draw_Windows(p);

    if (highlight)
    {
        int k = p->land_frame;
        Draw_Ecb(p->pos[k], p->bottom[k], &p->land_ecb, p->facing, land_kind_colors[p->land_kind]);
    }

    // short white tick where a pending fastfall kicks in
    if (line && p->fastfall_frame && p->fastfall_frame <= last)
    {
        int k = p->fastfall_frame;
        float y = p->pos[k].Y + p->bottom[k];
        World_Start(2, GX_LINES, 24);
        World_Vtx(p->pos[k].X - 2.f, y, 0, color_actual);
        World_Vtx(p->pos[k].X + 2.f, y, 0, color_actual);
    }
}

// The timers' look. Everything that times an input is drawn in screen
// space on the HUD camera, so it keeps its size whatever the zoom:
// - the meter, a frame strip above Falcon: one cell per frame, feeding
//   into the gate at its left end. Press as a solid cell reaches the gate.
//   A hollow cell with a floor bar is the touchdown, light dashes are
//   frames in the air, and the periwinkle tail is the ledge intangibility
//   left once Falcon can act. Cells already past are eaten by the gate.
// - the spot timers, on the floor where it happens: brackets close in for
//   an AI or NIL, ticks run in from both ends of the slide to the middle
//   for a waveland or wavedash.
// - the fixed strip (optional), a bigger meter at the bottom with labels.
// Every timer sends one ghost out: a strong one with a burst on a press
// that works, else a faint one on the window's last frame (a skip), and
// none for an early or late press. It ends in a burst (hit) or a muted
// implosion (miss or skip). Hits keep the cue's color, misses turn a cold slate and skips
// gray.
static const GXColor color_miss = {104, 114, 150, 255}; // cold slate
static const GXColor color_skip = {140, 149, 168, 255};
static const GXColor color_white = {255, 255, 255, 255};
static const GXColor color_galint = {128, 128, 255, 255};
static const GXColor color_plate = {6, 8, 16, 255};

// What each input is drawn in wherever it shows (the markers along the
// paths, the timer's glyphs, the controller display): the stick white, a
// jump yellow, an aerial pink like the AI cues, an airdodge cyan like the
// waveland cues.
static const GXColor color_in_jump = {255, 220, 40, 255};
#define color_in_aerial land_kind_colors[LAND_AI]
#define color_in_dodge land_kind_colors[LAND_PERFECT_WL]

// A color at a fraction of its strength (colors are premultiplied). The
// HUD blends by source alpha (HSD_SetupPEMode: SRCALPHA, INVSRCALPHA), so
// it shows at about a * a over what's behind it; it was all tuned that way.
static GXColor Color_Fill(GXColor c, float a)
{
    if (a < 0)
        a = 0;
    if (a > 1)
        a = 1;
    c.r = c.r * a;
    c.g = c.g * a;
    c.b = c.b * a;
    c.a = 255 * a;
    return c;
}

// The whole color laid over what's behind it at a, for a faint mark that
// still has to read over the dark plates.
static GXColor Color_Over(GXColor c, float a)
{
    if (a < 0)
        a = 0;
    if (a > 1)
        a = 1;
    c.a = 255 * a;
    return c;
}

static GXColor Color_Mix(GXColor a, GXColor b, float t)
{
    a.r = a.r + (b.r - a.r) * t;
    a.g = a.g + (b.g - a.g) * t;
    a.b = a.b + (b.b - a.b) * t;
    a.a = a.a + (b.a - a.a) * t;
    return a;
}

static float Ease_Out(float q)
{
    return 1.f - (1.f - q) * (1.f - q);
}

static float Clamp01(float v)
{
    return v < 0 ? 0 : v > 1 ? 1 : v;
}

static GXColor Cue_Color(int kind)
{
    static const u8 land[CUE_NUM] = {LAND_AI, LAND_PERFECT_WL, LAND_NIL};
    return land_kind_colors[land[kind]];
}

static GXColor Dim_Color(int dim)
{
    return dim == DIM_MISS ? color_miss : color_skip;
}

// How far a waveland slides from ground speed v until it stops, with the
// ground friction (ft_80084F3C, doubled above walk speed as in the
// jumpsquat). Approximate: assumes the landing keeps the dodge's sideways
// speed.
static float Slide_Distance(FighterData *fp, float v)
{
    float d = 0;
    for (int i = 0; i < 200 && v > 0; i++)
    {
        float friction = fp->attr.ground_friction;
        if (v > fp->attr.walk_maximum_velocity)
            friction *= common_run_friction;
        v = v > friction ? v - friction : 0;
        d += v;
    }
    return d;
}

// The frame a hit's timer burns down to: the first one Falcon can act on is
// the next.
static int Cue_FlashAge(Cue *e)
{
    return (e->lag > 1 ? e->lag : 1) - 1;
}
