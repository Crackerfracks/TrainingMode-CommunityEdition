/* landinglab/near_falcon.c
 *
 * Holds the timers that ride with Falcon (Bubble, Halo, Pincers, ECB Fill,
 * Lights), and Spot_Draw.
 *
 * Part of Landing Gear: src/landinglab.c includes it, in order, into one
 * translation unit, so it sees everything included before it (and mex.h,
 * events.h, landinglab_sim.h).
 *
 * Uses: menus_display, tracking_state, hud_text, timers, drawing,
 * hud_drawing, meter, spot_timers.
 */

// The timers that ride with Falcon, in the looks of the Near Falcon option.
// Each is drawn on the HUD from the cues and his position alone, so a paused
// game draws the same picture again, and in world sizes that follow the
// camera's zoom, kept between a size that can be read and one that crowds the
// screen. They all share what the timers promise: a countdown closes at one
// fixed speed a frame whatever its lead (a shorter one starts closer), the
// press frame is the moving part arriving and turning white, a hit sends out
// the one strong ghost that grows, a skip a smaller, dimmer one, and a miss
// none: it folds away in slate (a skip in gray).
#define NEAR_PI 3.1415927f
#define NEAR_TAU 6.2831853f
#define NEAR_FILL 12       // the ECB look: frames it takes to fill
#define NEAR_LIGHTS_MAX 12 // the lights look: lights drawn, at most

enum near_mode
{
    NMODE_NONE,   // nothing but a ghost: a hit, or a press that worked waiting for its touchdown
    NMODE_COUNT,  // closing on the press
    NMODE_WINDOW, // the press works now
    NMODE_FOLD,   // a miss or a skip folding in
    NMODE_CUT,    // a countdown the prediction dropped
};

// What a look needs of one cue to draw it.
typedef struct NearCue
{
    Cue *c;
    int mode;
    int k;        // counting: frames until the press, 0 on its frame
    int w;        // frames in the window
    int age;      // frames into the window, or into the ending
    int dim;      // a miss or skip already decided
    int now;      // the press works on this frame: white
    int ghost;    // the ghost's age, -1 none
    int bright;   // ... of a press that worked
    float appear; // fading in over the countdown's first 2 frames
    GXColor base; // the cue's own color
    GXColor col;  // what its shapes are in: base, or slate or gray once a miss or skip is decided
} NearCue;

static int Near_Shown(int kind, Cue *c)
{
    return Cue_KindOn(kind, c);
}

static float Near_Clamp(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

// HUD units a world unit takes up at (x, y): the camera's zoom there.
static float Near_Scale(float x, float y)
{
    float ax, ay, bx, by;
    Hud_FromWorld(x, y, &ax, &ay);
    Hud_FromWorld(x, y + 10.f, &bx, &by);
    float s = sqrtf((bx - ax) * (bx - ax) + (by - ay) * (by - ay)) / 10.f;
    return s > 0.02f ? s : 0.02f;
}

// The cue as a look draws it. Returns 0 when there is nothing to draw: no
// cue, a wavedash out of the jumpsquat (its own timers draw that), or an
// ending past its ghost.
static int Near_Cue(int kind, Cue *c, NearCue *n)
{
    memset(n, 0, sizeof(*n));
    if (!c->phase || (kind == CUE_WL && c->wd))
        return 0;
    n->c = c;
    n->base = Cue_Color(kind);
    n->dim = c->dim;
    n->col = c->dim ? Dim_Color(c->dim) : n->base;
    n->w = c->width > 0 ? c->width : 1;
    n->age = c->age;
    n->appear = 1.f;
    n->ghost = Cue_Ghost(c, &n->bright);
    switch (c->phase)
    {
    case PH_COUNT:
        // a NIL's press is its touchdown, a frame before the one it acts on
        n->mode = NMODE_COUNT;
        n->k = kind == CUE_NIL ? c->left - 2 : c->left - 1;
        if (n->k < 0)
            n->k = 0;
        // fades in over its first 2 frames (a countdown that grew a frame
        // stays as it was)
        n->appear = c->span - c->left + 1 < 1 ? 1.f : Clamp01((c->span - c->left + 1) / 2.f);
        n->now = n->k == 0 && !c->dim;
        break;
    case PH_WINDOW:
        // once the press worked, its ghost plays on alone
        n->mode = c->held || c->age >= c->width ? NMODE_NONE : NMODE_WINDOW;
        n->now = c->age == 0 && !c->dim;
        break;
    case PH_FADE:
        n->mode = NMODE_FOLD;
        break;
    case PH_CUT:
        n->mode = NMODE_CUT;
        break;
    }
    return n->mode != NMODE_NONE || n->ghost >= 0;
}

// Segments for a circle of radius r (HUD units) that keep its edge round to
// about a pixel, an even number so a disc is half as many quads.
static int Near_Segs(float r, int least)
{
    int n = (int)(12.f * sqrtf(r) + 0.99f);
    n += n & 1;
    return n < least ? least : n > 36 ? 36 : n;
}

// A circle's outline from angle a0 to a1 (radians, counterclockwise from
// the right), w wide and centered on radius r.
static void Near_Arc(float cx, float cy, float r, float w, float a0, float a1, GXColor c)
{
    float span = a1 - a0;
    if (span <= 0.f || r <= 0.f)
        return;
    int n = (int)(span / NEAR_TAU * Near_Segs(r, 10) + 0.99f);
    float r0 = r - w / 2, r1 = r + w / 2;
    if (r0 < 0.f)
        r0 = 0.f;
    float px = cos(a0), py = sin(a0);
    for (int i = 1; i <= n; i++)
    {
        float a = a0 + span * i / n;
        float nx = cos(a), ny = sin(a);
        Quad_Add(cx + px * r0, cy + py * r0, cx + px * r1, cy + py * r1, cx + nx * r1, cy + ny * r1, cx + nx * r0, cy + ny * r0, c);
        px = nx;
        py = ny;
    }
}

static void Near_Ring(float cx, float cy, float r, float w, GXColor c)
{
    Near_Arc(cx, cy, r, w, 0.f, NEAR_TAU, c);
}

// A filled slice of a circle from a0 to a1, with at least least segments
// around a whole one. Two slices go in each quad (a kite from the center).
static void Near_Fan(float cx, float cy, float r, float a0, float a1, int least, GXColor c)
{
    float span = a1 - a0;
    if (span <= 0.f || r <= 0.f)
        return;
    int n = (int)(span / NEAR_TAU * Near_Segs(r, least) + 0.99f);
    float px[40], py[40];
    if (n > 38)
        n = 38;
    for (int i = 0; i <= n; i++)
    {
        float a = a0 + span * i / n;
        px[i] = cx + r * cos(a);
        py[i] = cy + r * sin(a);
    }
    int i = 0;
    for (; i + 1 < n; i += 2)
        Quad_Add(cx, cy, px[i], py[i], px[i + 1], py[i + 1], px[i + 2], py[i + 2], c);
    if (i < n)
        Hud_Tri(cx, cy, px[i], py[i], px[i + 1], py[i + 1], c);
}

static void Near_Disc(float cx, float cy, float r, GXColor c)
{
    Near_Fan(cx, cy, r, 0.f, NEAR_TAU, 10, c);
}

// A small dot: an octagon, or a hexagon at the smallest.
static void Near_Dot(float cx, float cy, float r, GXColor c)
{
    Near_Fan(cx, cy, r, 0.f, NEAR_TAU, 6, c);
}

// The bubble: a ring closes on a bubble where Falcon's body will be on the
// first frame the press works (Cue_Body), at one fixed speed a frame, and
// touches it on that frame. In the window the bubble is a pie that drains a
// frame at a time, its edge white as the window opens. A hit's ghost fills
// the bubble, grows to about 2.3 times its size and bursts in six spokes; a
// skip's is smaller and dimmer; a miss's ring folds away in slate. The bubble
// never moves once its window has opened, so it never chases Falcon, and may
// cover the path under it.
static void Near_Bubble(int kind, Cue *c)
{
    NearCue n;
    if (!Near_Cue(kind, c, &n))
        return;
    // where his body will be; with no countdown behind a hit, where he is
    float bx = c->has_body ? c->body.X : c->spot.X;
    float by = c->has_body ? c->body.Y : c->spot.Y + body_offset;
    float hx, hy;
    if (!Hud_FromWorld(bx, by, &hx, &hy))
        return;
    float S = Near_Scale(bx, by);
    float rb = Near_Clamp(3.6f * S, 0.9f, 2.4f); // the bubble's radius
    float step = rb * 0.172f;                    // the ring's speed, a frame
    float wr = Near_Clamp(0.16f * rb, 0.12f, 0.3f);
    float ew = wr + 0.1f; // the dark under-stroke

    if (n.mode == NMODE_COUNT)
    {
        float a = n.appear;
        Near_Disc(hx, hy, rb, Color_Over(color_plate, 0.45f * a));
        Near_Ring(hx, hy, rb, ew, Color_Over(color_plate, 0.4f * a));
        Near_Ring(hx, hy, rb, wr, Color_Over(n.now ? color_white : n.col, 0.9f * a));
        Hud_Diamond(hx, hy, 0.4f * rb, Color_Over(color_plate, 0.5f * a));
        Hud_Diamond(hx, hy, 0.28f * rb, Color_Over(n.col, 0.9f * a));
        if (n.k > 0)
        {
            float ra = rb + n.k * step;
            if (n.k <= 6) // far out its dark edge isn't worth the quads
                Near_Ring(hx, hy, ra, ew, Color_Over(color_plate, 0.35f * a));
            Near_Ring(hx, hy, ra, wr, Color_Over(Color_Mix(n.col, color_white, 0.25f), 0.7f * a));
        }
    }
    else if (n.mode == NMODE_WINDOW)
    {
        float left = (float)(n.w - n.age) / n.w;
        Near_Disc(hx, hy, rb, Color_Over(color_plate, 0.5f));
        Near_Fan(hx, hy, rb, NEAR_PI / 2.f - NEAR_TAU * left, NEAR_PI / 2.f, 10, Color_Over(n.col, 0.9f));
        Near_Ring(hx, hy, rb, ew, Color_Over(color_plate, 0.4f));
        Near_Ring(hx, hy, rb, wr, Color_Over(n.now ? color_white : n.col, 1.f));
    }
    else if (n.mode == NMODE_FOLD)
    {
        float q = Clamp01((float)n.age / LL_FADE);
        float r = rb * (1.f - Ease_Out(q));
        if (r > 0.25f)
        {
            Near_Ring(hx, hy, r, ew, Color_Over(color_plate, 0.35f * (1.f - q)));
            Near_Ring(hx, hy, r, wr, Color_Over(n.col, 0.8f * (1.f - q)));
        }
    }
    else if (n.mode == NMODE_CUT)
        Near_Ring(hx, hy, rb, wr, Color_Over(color_skip, 0.35f * (1.f - (float)n.age / LL_CUT)));

    if (n.ghost >= 0)
    {
        float q = (float)n.ghost / LL_GHOST, fade = (1.f - q) * (1.f - q);
        float r = rb * (1.f + (n.bright ? 1.3f : 0.5f) * Ease_Out(q));
        Near_Disc(hx, hy, r, Color_Over(Color_Mix(n.base, color_white, n.bright ? 0.3f : 0.15f), (n.bright ? 0.6f : 0.22f) * fade));
        Near_Ring(hx, hy, r, wr * 1.2f, Color_Over(Color_Mix(n.base, color_white, 0.4f), (n.bright ? 0.95f : 0.45f) * fade));
        if (n.bright)
        {
            for (int i = 0; i < 6; i++)
            {
                float ang = i * NEAR_PI / 3.f + NEAR_PI / 6.f, cs = cos(ang), sn = sin(ang);
                Hud_Seg(hx + cs * r * 0.9f, hy + sn * r * 0.9f, hx + cs * r * 1.5f, hy + sn * r * 1.5f, wr, Color_Over(n.base, fade));
            }
        }
    }
}

// The halo: a faint ring around Falcon's body, which it follows, with a bead
// running clockwise at 15 degrees a frame into the window's notch at twelve
// o'clock (as wide as the window, 15 degrees a frame of it); the last three
// frames are dots on the ring. A hit lights the ring up a little and grows
// it; the ghost stays modest.
static void Near_Halo(FighterData *fp, NearCue *n)
{
    float x = fp->phys.pos.X, y = fp->phys.pos.Y + body_offset;
    float hx, hy;
    if (!Hud_FromWorld(x, y, &hx, &hy))
        return;
    float S = Near_Scale(x, y);
    float R = Near_Clamp(12.f * S, 2.6f, 7.5f);
    float wf = 0.12f;                             // the faint ring
    float wn = Near_Clamp(1.2f * S, 0.3f, 0.5f);   // the notch
    float step = NEAR_PI / 12.f;                   // 15 degrees
    float top = NEAR_PI / 2.f;                     // twelve o'clock; a frame before it is counterclockwise

    if (n->mode == NMODE_COUNT || n->mode == NMODE_WINDOW)
    {
        float a = n->mode == NMODE_COUNT ? n->appear : 1.f;
        Near_Ring(hx, hy, R, wf + 0.1f, Color_Over(color_plate, 0.25f * a));
        Near_Ring(hx, hy, R, wf, Color_Over(color_white, 0.3f * a));
        float w = n->w * step;
        if (w > NEAR_TAU - 2.f * step)
            w = NEAR_TAU - 2.f * step;
        Near_Arc(hx, hy, R, wn + 0.12f, top - w, top, Color_Over(color_plate, 0.5f));
        Near_Arc(hx, hy, R, wn, top - w, top, Color_Over(n->col, 0.9f));

        float dr = Near_Clamp(0.6f * S, 0.18f, 0.32f);
        for (int j = 1; j <= 3; j++)
        {
            float ang = top + j * step;
            Near_Dot(hx + R * cos(ang), hy + R * sin(ang), dr + 0.07f, Color_Over(color_plate, 0.5f * a));
            Near_Dot(hx + R * cos(ang), hy + R * sin(ang), dr, Color_Over(color_white, 0.7f * a));
        }

        if (n->mode == NMODE_COUNT)
        {
            // the bead and its tail, the tail trailing counterclockwise
            float br = Near_Clamp(1.4f * S, 0.4f, 0.75f);
            for (int pass = 0; pass < 2; pass++)
            {
                for (int tail = 3; tail >= 0; tail--)
                {
                    float ang = top + (n->k + tail * 0.35f) * step;
                    float r = br * (1.f - 0.19f * tail), al = (1.f - 0.25f * tail) * a;
                    if (pass == 0)
                        Near_Dot(hx + R * cos(ang), hy + R * sin(ang), r + 0.09f, Color_Over(color_plate, 0.4f * al));
                    else
                        Near_Dot(hx + R * cos(ang), hy + R * sin(ang), r, Color_Over(Color_Mix(n->col, color_white, 0.6f), al));
                }
            }
        }
        else
        {
            float ang = top - (n->age + 0.5f) * step;
            float br = Near_Clamp(1.5f * S, 0.42f, 0.8f);
            Near_Dot(hx + R * cos(ang), hy + R * sin(ang), br + 0.1f, Color_Over(color_plate, 0.55f));
            Near_Dot(hx + R * cos(ang), hy + R * sin(ang), br, Color_Over(n->dim ? n->col : color_white, 1.f));
        }
    }
    else if (n->mode == NMODE_FOLD)
    {
        float q = Clamp01((float)n->age / LL_FADE);
        float r = R * (1.f - 0.5f * Ease_Out(q));
        Near_Ring(hx, hy, r, wf + 0.12f, Color_Over(color_plate, 0.3f * (1.f - q)));
        Near_Ring(hx, hy, r, wf + 0.04f, Color_Over(n->col, 0.6f * (1.f - q)));
    }
    else if (n->mode == NMODE_CUT)
        Near_Ring(hx, hy, R, wf, Color_Over(color_skip, 0.3f * (1.f - (float)n->age / LL_CUT)));

    if (n->ghost >= 0)
    {
        float q = (float)n->ghost / LL_GHOST, fade = (1.f - q) * (1.f - q);
        float r = R * (1.f + (n->bright ? 0.22f : 0.1f) * Ease_Out(q));
        float gw = n->bright ? Near_Clamp(0.9f * S, 0.15f, 0.36f) : Near_Clamp(0.5f * S, 0.1f, 0.2f);
        if (n->bright)
            Near_Disc(hx, hy, r, Color_Over(n->base, 0.14f * fade));
        Near_Ring(hx, hy, r, gw, Color_Over(Color_Mix(n->base, color_white, 0.35f), (n->bright ? 0.9f : 0.4f) * fade));
    }
}

// Falcon's ECB as the game collides with it this frame, CollData's
// ecbCurrCorrect_* (offsets from his position): the top, right, bottom and
// left points, in v. One not built yet (all zero) gets a stand-in.
static void Near_Ecb(FighterData *fp, Vec2 *v)
{
    CollData *cd = &fp->coll_data;
    v[0] = cd->ecbCurrCorrect_top;
    v[1] = cd->ecbCurrCorrect_right;
    v[2] = cd->ecbCurrCorrect_bot;
    v[3] = cd->ecbCurrCorrect_left;
    if (v[0].Y - v[2].Y < 6.f || v[1].X - v[3].X < 2.f)
    {
        v[0] = (Vec2){0.f, 17.f};
        v[1] = (Vec2){4.5f, 9.5f};
        v[2] = (Vec2){0.f, 0.f};
        v[3] = (Vec2){-4.5f, 9.5f};
    }
}

// A bracket whose bar stands at x from y0 up to y1 with a foot at each end
// reaching toward the middle (side > 0: the right bracket, a ] shape); no
// part overlaps another, so a translucent one shows no darker corners.
static void Near_Bracket(float x, float y0, float y1, int side, float foot, float w, GXColor c)
{
    float out = x + side * w / 2, in = x - side * foot;
    float lo = out < in ? out : in, hi = out < in ? in : out;
    Hud_Rect(x - w / 2, y0 + w / 2, x + w / 2, y1 - w / 2, c);
    Hud_Rect(lo, y0 - w / 2, hi, y0 + w / 2, c);
    Hud_Rect(lo, y1 - w / 2, hi, y1 + w / 2, c);
}

// The pincers: two brackets as tall as Falcon's ECB close in from both sides
// at a fixed speed a frame and clamp onto his body width on the press frame,
// holding through the window, each on a dark under-stroke. A hit's ghost
// spreads them out again with a fill between; a miss folds them up to their
// middle in slate. Wide is the ECB's own width plus a little, kept near his
// body's.
static void Near_Pincers(FighterData *fp, NearCue *n)
{
    Vec2 v[4];
    Near_Ecb(fp, v);
    float x = fp->phys.pos.X, y = fp->phys.pos.Y;
    float hx, y0, y1, unused;
    if (!Hud_FromWorld(x, y + v[2].Y, &hx, &y0) || !Hud_FromWorld(x, y + v[0].Y, &unused, &y1))
        return;
    float S = Near_Scale(x, y + v[1].Y);
    float half = Near_Clamp((v[1].X - v[3].X) / 2.f + 0.5f, 3.6f, 7.f) * S; // body width, HUD units
    float step = Near_Clamp(0.6f * S, 0.1f, 0.22f);
    float foot = Near_Clamp(1.6f * S, 0.35f, 0.8f);
    float w = Near_Clamp(0.6f * S, 0.12f, 0.28f);
    float e = 0.1f; // the under-stroke's extra width

    if (n->mode == NMODE_COUNT || n->mode == NMODE_WINDOW)
    {
        int count = n->mode == NMODE_COUNT;
        float a = count ? n->appear : 1.f;
        float off = count ? n->k * step : 0.f;
        GXColor c = Color_Over(n->now ? color_white : n->col, count ? 0.9f * a : 1.f);
        for (int side = -1; side <= 1; side += 2)
            Near_Bracket(hx + side * (half + off), y0, y1, side, foot + e / 2, w + e, Color_Over(color_plate, 0.55f * a));
        for (int side = -1; side <= 1; side += 2)
            Near_Bracket(hx + side * (half + off), y0, y1, side, foot, w, c);
    }
    else if (n->mode == NMODE_FOLD)
    {
        float q = Clamp01((float)n->age / LL_FADE), k = 1.f - Ease_Out(q);
        float ym = (y0 + y1) / 2.f, hh = (y1 - y0) / 2.f * k;
        if (hh > 0.3f)
        {
            for (int side = -1; side <= 1; side += 2)
                Near_Bracket(hx + side * half, ym - hh, ym + hh, side, foot + e / 2, w * 0.85f + e, Color_Over(color_plate, 0.35f * (1.f - q)));
            for (int side = -1; side <= 1; side += 2)
                Near_Bracket(hx + side * half, ym - hh, ym + hh, side, foot, w * 0.85f, Color_Over(n->col, 0.7f * (1.f - q)));
        }
    }
    else if (n->mode == NMODE_CUT)
    {
        for (int side = -1; side <= 1; side += 2)
            Near_Bracket(hx + side * half, y0, y1, side, foot, w * 0.85f, Color_Over(color_skip, 0.3f * (1.f - (float)n->age / LL_CUT)));
    }

    if (n->ghost >= 0)
    {
        float q = (float)n->ghost / LL_GHOST, fade = (1.f - q) * (1.f - q);
        float o = Ease_Out(q) * (n->bright ? Near_Clamp(4.f * S, 0.6f, 1.8f) : Near_Clamp(1.6f * S, 0.3f, 0.8f));
        float gw = n->bright ? Near_Clamp(0.9f * S, 0.16f, 0.36f) : Near_Clamp(0.5f * S, 0.1f, 0.22f);
        GXColor gc = Color_Over(Color_Mix(n->base, color_white, 0.35f), (n->bright ? 0.95f : 0.45f) * fade);
        if (n->bright)
            Hud_Rect(hx - half - o, y0, hx + half + o, y1, Color_Over(n->base, 0.3f * fade));
        for (int side = -1; side <= 1; side += 2)
            Near_Bracket(hx + side * (half + o), y0, y1, side, foot, gw, gc);
    }
}

// A convex outline cut to what is at or below y (HUD y up), into ox and oy.
// Returns the points left: at most two more than there were.
static int Near_ClipBelow(const float *px, const float *py, int n, float y, float *ox, float *oy)
{
    int m = 0;
    for (int i = 0; i < n; i++)
    {
        int j = (i + 1) % n;
        int in_i = py[i] <= y, in_j = py[j] <= y;
        if (in_i)
        {
            ox[m] = px[i];
            oy[m] = py[i];
            m++;
        }
        if (in_i != in_j)
        {
            float t = (y - py[i]) / (py[j] - py[i]);
            ox[m] = px[i] + (px[j] - px[i]) * t;
            oy[m] = y;
            m++;
        }
    }
    return m;
}

// The ECB fill: Falcon's ECB (see Near_Ecb), the diamond the game lands with,
// drawn on him. It fills from its bottom point at a fixed rate and is full on
// the press frame, 12 frames after it started; ticks beside it mark the last
// three frames' levels. In the window its bottom point drops a spike to the
// floor where he will land (the cue's spot), which is what the interrupt is.
// A hit's ghost is a bigger, filled diamond; a miss shrinks it away in slate.
static void Near_EcbFill(FighterData *fp, NearCue *n)
{
    Vec2 v[4];
    Near_Ecb(fp, v);
    float x = fp->phys.pos.X, y = fp->phys.pos.Y;
    float px[4], py[4];
    for (int i = 0; i < 4; i++)
    {
        if (!Hud_FromWorld(x + v[i].X, y + v[i].Y, &px[i], &py[i]))
            return;
    }
    float S = Near_Scale(x, y + v[1].Y);
    float lw = Near_Clamp(0.6f * S, 0.18f, 0.32f);
    float mx = (px[0] + px[1] + px[2] + px[3]) / 4.f, my = (py[0] + py[1] + py[2] + py[3]) / 4.f;

    if (n->mode == NMODE_COUNT || n->mode == NMODE_WINDOW)
    {
        int window = n->mode == NMODE_WINDOW;
        float a = window ? 1.f : n->appear;
        float level = window ? 1.f : Clamp01(1.f - (float)n->k / NEAR_FILL);
        if (level > 0.f)
        {
            float ox[8], oy[8];
            int m = Near_ClipBelow(px, py, 4, py[2] + (py[0] - py[2]) * level, ox, oy);
            if (m >= 3)
            {
                float cx = 0.f, cy = 0.f;
                for (int i = 0; i < m; i++)
                {
                    cx += ox[i] / m;
                    cy += oy[i] / m;
                }
                Hud_Fan(cx, cy, ox, oy, m, Color_Over(window ? Color_Mix(n->col, color_white, n->now ? 0.5f : 0.f) : n->col, 0.72f * a));
            }
        }
        // a soft glow around the outline: the diamond is small and Falcon's
        // own colors run through it, so it needs to stand off him
        Hud_Line(px, py, 4, 1, lw + 0.55f, PX, Color_Over(n->col, 0.22f * a));
        Hud_Line(px, py, 4, 1, lw + 0.12f, PX, Color_Over(color_plate, 0.45f * a));
        Hud_Line(px, py, 4, 1, lw, PX, Color_Over(n->col, 0.8f * a));

        // ticks for the last three frames' levels, lit once the fill has reached them
        float xr = px[1] > px[3] ? px[1] : px[3];
        for (int j = 1; j <= 3; j++)
        {
            float ty = py[2] + (py[0] - py[2]) * (1.f - (float)j / NEAR_FILL);
            int lit = window || n->k <= j;
            Hud_Rect(xr + 0.25f, ty - 0.1f, xr + 0.95f, ty + 0.1f, Color_Over(color_plate, 0.5f * a));
            Hud_Rect(xr + 0.3f, ty - 0.05f, xr + 0.9f, ty + 0.05f, Color_Over(color_white, (lit ? 0.95f : 0.4f) * a));
        }

        if (window)
        {
            float sx, sy;
            if (Hud_FromWorld(n->c->spot.X, n->c->spot.Y, &sx, &sy) && sy < py[2] - 0.3f)
            {
                float bw = Near_Clamp(0.45f * S, 0.12f, 0.26f);
                Hud_Tri(px[2] - bw - 0.06f, py[2] + 0.06f, px[2] + bw + 0.06f, py[2] + 0.06f, sx, sy - 0.1f, Color_Over(color_plate, 0.5f));
                Hud_Tri(px[2] - bw, py[2], px[2] + bw, py[2], sx, sy, Color_Over(n->col, 1.f));
                Hud_Rect(sx - 0.5f, sy - 0.05f, sx + 0.5f, sy + 0.1f, Color_Over(n->now ? color_white : n->col, 1.f));
            }
        }
    }
    else if (n->mode == NMODE_FOLD || n->mode == NMODE_CUT)
    {
        float q = n->mode == NMODE_FOLD ? Clamp01((float)n->age / LL_FADE) : (float)n->age / LL_CUT;
        float k = n->mode == NMODE_FOLD ? 1.f - Ease_Out(q) : 1.f;
        float sx[4], sy[4];
        for (int i = 0; i < 4; i++)
        {
            sx[i] = mx + (px[i] - mx) * k;
            sy[i] = my + (py[i] - my) * k;
        }
        GXColor fc = n->mode == NMODE_FOLD ? Color_Over(n->col, 0.8f * (1.f - q)) : Color_Over(color_skip, 0.3f * (1.f - q));
        Hud_Line(sx, sy, 4, 1, lw + 0.12f, PX, Color_Over(color_plate, 0.3f * (1.f - q)));
        Hud_Line(sx, sy, 4, 1, lw, PX, fc);
    }

    if (n->ghost >= 0)
    {
        float q = (float)n->ghost / LL_GHOST, fade = (1.f - q) * (1.f - q);
        float g = 1.f + (n->bright ? 0.7f : 0.25f) * Ease_Out(q);
        float gx[4], gy[4];
        for (int i = 0; i < 4; i++)
        {
            gx[i] = mx + (px[i] - mx) * g;
            gy[i] = my + (py[i] - my) * g;
        }
        Quad_Add(gx[0], gy[0], gx[1], gy[1], gx[2], gy[2], gx[3], gy[3],
                 Color_Over(Color_Mix(n->base, color_white, 0.3f), (n->bright ? 0.5f : 0.18f) * fade));
        Hud_Line(gx, gy, 4, 1, lw, PX, Color_Over(Color_Mix(n->base, color_white, 0.4f), (n->bright ? 0.95f : 0.45f) * fade));
    }
}

// The count-in lights: 4 plus the window's width small lights in an arc over
// Falcon's head, on a dark track. A thin fuse burns down along the arc until
// 4 frames are left, then one white light comes on a frame; the window's
// lights, in the cue's color, come on as its frames go by. A light that is
// off is a dim fill in an outline of its color, so it still shows over a dark
// stage. A hit's ghost swells the window's lights; a miss shrinks every light
// away in slate. A window of more than 8 frames shows only its first 8.
static void Near_Lights(FighterData *fp, NearCue *n)
{
    float x = fp->phys.pos.X, y = fp->phys.pos.Y + body_offset + 14.5f;
    float hx, hy;
    if (!Hud_FromWorld(x, y, &hx, &hy))
        return;
    float S = Near_Scale(x, y);
    int lights = 4 + n->w;
    if (lights > NEAR_LIGHTS_MAX)
        lights = NEAR_LIGHTS_MAX;
    float R = Near_Clamp(13.f * S, 3.f, 9.f);    // the arc's radius
    float r = Near_Clamp(1.25f * S, 0.34f, 0.75f); // a light's
    float da = 2.9f * r / R;                     // the angle between two
    if ((lights - 1) * da > 2.8f)
        da = 2.8f / (lights - 1);
    float amax = (lights - 1) * da / 2.f;
    if (hy > SAFE_H - 1.2f - r)
        hy = SAFE_H - 1.2f - r; // keep it on the screen
    float cyc = hy - R;         // the arc's center, below

    float a = n->mode == NMODE_COUNT ? n->appear : 1.f;
    int live = n->mode == NMODE_COUNT || n->mode == NMODE_WINDOW;
    float q = n->mode == NMODE_FOLD ? Clamp01((float)n->age / LL_FADE) : n->mode == NMODE_CUT ? (float)n->age / LL_CUT : 0.f;
    if (live || n->mode == NMODE_FOLD || n->mode == NMODE_CUT)
        Near_Arc(hx, cyc, R, 3.f * r, NEAR_PI / 2.f - amax - da * 0.6f, NEAR_PI / 2.f + amax + da * 0.6f,
                 Color_Over(color_plate, (live ? 0.5f : 0.35f) * (1.f - q) * a));

    for (int i = 0; i < lights; i++)
    {
        float ang = (i - (lights - 1) / 2.f) * da;
        float lx = hx + R * sin(ang), ly = cyc + R * cos(ang);
        GXColor c = i < 4 ? color_white : n->col;
        if (live)
        {
            int on = n->mode == NMODE_WINDOW ? i <= 4 + n->age : (i < 4 && i <= 4 - n->k);
            if (on)
                Near_Dot(lx, ly, r, Color_Over(c, 1.f));
            else
            {
                Near_Dot(lx, ly, r * 1.1f, Color_Over(c, 0.6f * a));
                Near_Dot(lx, ly, r * 0.62f, Color_Over(Color_Mix(c, color_plate, 0.75f), 0.95f * a));
            }
        }
        else if (n->mode == NMODE_FOLD)
            Near_Dot(lx, ly, r * (1.f - Ease_Out(q)), Color_Over(n->col, 0.8f * (1.f - q)));
        else if (n->mode == NMODE_CUT)
            Near_Dot(lx, ly, r, Color_Over(color_skip, 0.3f * (1.f - q)));
    }

    // the fuse, along the arc just inside the lights, burning down from the right
    if (n->mode == NMODE_COUNT && n->k > 4)
    {
        float frac = Clamp01((n->k - 4) / 20.f);
        float rf = R - 2.2f * r;
        float a1 = NEAR_PI / 2.f + amax + da * 0.6f, a0 = a1 - frac * (2.f * amax + da * 1.2f);
        Near_Arc(hx, cyc, rf, 0.2f + 0.12f, a0, a1, Color_Over(color_plate, 0.45f * a));
        Near_Arc(hx, cyc, rf, 0.2f, a0, a1, Color_Over(color_white, 0.75f * a));
    }

    if (n->ghost >= 0)
    {
        float gq = (float)n->ghost / LL_GHOST, fade = (1.f - gq) * (1.f - gq);
        for (int i = 4; i < lights; i++)
        {
            float ang = (i - (lights - 1) / 2.f) * da;
            Near_Dot(hx + R * sin(ang), cyc + R * cos(ang), r * (1.f + (n->bright ? 1.6f : 0.6f) * Ease_Out(gq)),
                     Color_Over(Color_Mix(n->base, color_white, 0.3f), (n->bright ? 0.7f : 0.3f) * fade));
        }
    }
}

// The near-Falcon timers other than the old strip, in the look picked in the
// Timers menu. The bubble draws every cue that is live (and each kind's last
// ending) at its own spot; the other looks draw only the soonest cue (the
// smallest left; a tie goes to the AI, then the NIL, then the waveland) and
// its ending, or with none counting down, whichever ending is freshest. A
// ledge route that has the timer rows gets none of them.
static void Near_Draw(FighterData *fp, int look)
{
    if (route_rows_active)
        return;

    float base = vis_k;
    if (look == NEAR_BUBBLE)
    {
        for (int pass = 0; pass < 2; pass++)
        {
            for (int i = 0; i < CUE_NUM; i++)
            {
                vis_k = base * Fade_Factor(VG_TIMERS, i);
                if (Near_Shown(i, pass ? &cue_live[i] : &cue_end[i]))
                    Near_Bubble(i, pass ? &cue_live[i] : &cue_end[i]);
            }
        }
        vis_k = base;
        return;
    }

    // (a cue with nothing left to draw, like a press waiting for its touchdown
    // once its ghost is over, doesn't hold the others back)
    static const u8 order[CUE_NUM] = {CUE_AI, CUE_NIL, CUE_WL};
    NearCue n;
    int kind = -1, left = 0;
    for (int i = 0; i < CUE_NUM; i++)
    {
        Cue *c = &cue_live[order[i]];
        if (Near_Shown(order[i], c) && Near_Cue(order[i], c, &n) && (kind < 0 || c->left < left))
        {
            kind = order[i];
            left = c->left;
        }
    }
    if (kind < 0)
    {
        int age = 0;
        for (int i = 0; i < CUE_NUM; i++)
        {
            Cue *e = &cue_end[order[i]];
            if (Near_Shown(order[i], e) && Near_Cue(order[i], e, &n) && (kind < 0 || e->age < age))
            {
                kind = order[i];
                age = e->age;
            }
        }
    }
    if (kind < 0)
        return;

    vis_k = base * Fade_Factor(VG_TIMERS, kind);
    for (int pass = 0; pass < 2; pass++)
    {
        if (!Near_Cue(kind, pass ? &cue_live[kind] : &cue_end[kind], &n))
            continue;
        switch (look)
        {
        case NEAR_HALO:
            Near_Halo(fp, &n);
            break;
        case NEAR_PINCERS:
            Near_Pincers(fp, &n);
            break;
        case NEAR_ECB:
            Near_EcbFill(fp, &n);
            break;
        case NEAR_LIGHTS:
            Near_Lights(fp, &n);
            break;
        }
    }
    vis_k = base;
}

// The wavedash timers drawn at Falcon's feet (Pips, Ring); Cells is a row in
// the strips.
static void Wd_Draw(FighterData *fp)
{
    int look = Options_Timers[TOPT_WD].val;
    if (look != WDT_PIPS && look != WDT_RING)
        return;
    float base = vis_k;
    vis_k = base * Fade_Factor(VG_TIMERS, CUE_WL);
    // the last one's ending first, the live one over it
    for (int pass = 0; pass < 2; pass++)
    {
        Cue *c = pass ? &cue_live[CUE_WL] : &cue_end[CUE_WL];
        if (!c->phase || !c->wd)
            continue;
        if (look == WDT_PIPS)
            Wd_Pips(c, !pass);
        else
            Wd_Ring(c, !pass);
    }
    vis_k = base;
}

static void Spot_Draw(FighterData *fp)
{
    float base = vis_k;
    for (int pass = 0; pass < 2; pass++)
    {
        for (int i = 0; i < CUE_NUM; i++)
        {
            Cue *c = pass ? &cue_live[i] : &cue_end[i];
            if (!c->phase)
                continue;
            if (!Cue_KindOn(i, c))
                continue;
            vis_k = base * Fade_Factor(VG_TIMERS, i);
            if (i == CUE_WL)
            {
                if (Options_Timers[TOPT_WL].val != WLT_OFF)
                    Spot_Rails(c, fp, !pass);
            }
            else if (Options_Timers[TOPT_SPOT].val)
                Spot_Brackets(i, c, !pass);
        }
    }
    vis_k = base;
}
