/* landinglab/spot_timers.c
 *
 * Holds the spot timers on the floor: brackets, wavedash pips and ring,
 * waveland rails.
 *
 * Part of Landing Lab: src/landinglab.c includes it, in order, into one
 * translation unit, so it sees everything included before it (and mex.h,
 * events.h, landinglab_sim.h).
 *
 * Uses: menus_display, tracking_state, timers, drawing, hud_drawing, meter.
 */

///////////////////////
/// Spot timers     ///
///////////////////////

#define SPOT_HW 4.f     // half the footprint, in world units
#define SPOT_STEM 1.5f  // bracket height, HUD units
#define SPOT_LINE 0.4f  // footprint and bracket thickness
#define SPOT_FOOT 0.7f

static float Meter_Pitch(void)
{
    return MT_PITCH + (Options_Timers[TOPT_WIDE].val ? MT_WIDE : 0);
}

static void Spot_Bracket(float x, float y, int side, float stem, GXColor c)
{
    Hud_Rect(x - SPOT_LINE / 2, y, x + SPOT_LINE / 2, y + stem, c);
    float fx = x - side * SPOT_FOOT;
    Hud_Rect(side > 0 ? fx : x, y, side > 0 ? x : fx, y + SPOT_LINE, c);
}

// A bracket on a dark edge, so it reads over any stage.
static void Spot_BracketEdged(float x, float y, int side, float stem, GXColor c, float a)
{
    float e = 0.12f;
    Hud_Rect(x - SPOT_LINE / 2 - e, y - e, x + SPOT_LINE / 2 + e, y + stem + e, Color_Over(color_plate, 0.55f * a));
    float fx = x - side * (SPOT_FOOT + e);
    Hud_Rect(side > 0 ? fx : x - e, y - e, side > 0 ? x + e : fx, y + SPOT_LINE + e, Color_Over(color_plate, 0.55f * a));
    Spot_Bracket(x, y, side, stem, c);
}

// The timer's one ghost (Cue_Ghost): brackets that spread out and fade.
static void Spot_BracketGhost(Cue *c, GXColor base, float lx, float ly, float rx, float ry)
{
    int bright, g = Cue_Ghost(c, &bright);
    if (g < 0)
        return;
    float q = (float)g / LL_GHOST, fade = (1.f - q) * (1.f - q);
    float o = (bright ? 3.f : 1.4f) * Ease_Out(q);
    GXColor gc = Color_Over(Color_Mix(base, color_white, bright ? 0.4f : 0.2f), (bright ? 0.95f : 0.45f) * fade);
    float stem = SPOT_STEM * (1.f + (bright ? 0.6f : 0.25f) * q);
    Spot_Bracket(lx - o, ly, -1, stem, gc);
    Spot_Bracket(rx + o, ry, 1, stem, gc);
    if (bright)
        Hud_Seg(lx - o, ly, rx + o, ry, SPOT_LINE, Color_Over(base, 0.5f * fade));
}

// AI and NIL: the footprint where Falcon touches down, and brackets that
// close in on it a meter cell per frame and meet it on the frame to press
// (the touchdown, for a NIL).
static void Spot_Brackets(int kind, Cue *c, int ended)
{
    float lx, ly, rx, ry, mx, my;
    if (!Hud_FromWorld(c->spot.X, c->spot.Y, &mx, &my))
        return;
    Hud_FromWorld(c->spot.X - SPOT_HW, c->spot.Y, &lx, &ly);
    Hud_FromWorld(c->spot.X + SPOT_HW, c->spot.Y, &rx, &ry);
    GXColor base = Cue_Color(kind);
    float pitch = Meter_Pitch();

    if (!ended && (c->phase == PH_COUNT || c->phase == PH_WINDOW))
    {
        int k = c->phase == PH_WINDOW ? 0 : kind == CUE_NIL ? c->left - 2 : c->left - 1;
        if (k < 0)
            k = 0;
        if (k > 10)
            return;
        GXColor col = c->dim ? Dim_Color(c->dim) : base;
        float am = c->dim ? 0.5f : 1.f;
        float charge = k >= 5 ? 0.75f : 1.f - k * 0.05f;
        int now = c->phase == PH_WINDOW && !c->dim;
        Hud_Seg(lx, ly, rx, ry, SPOT_LINE + 0.24f, Color_Over(color_plate, 0.5f * am));
        Hud_Seg(lx, ly, rx, ry, SPOT_LINE + 0.04f, Color_Fill(now ? color_white : col, charge * am));
        float off = k * pitch;
        float ba = (k == 0 ? 1.f : 0.7f + 0.3f * (1.f - k / 10.f)) * am;
        GXColor bc = Color_Fill(now ? Color_Mix(col, color_white, c->age == 0 ? 0.9f : 0.4f) : col, ba);
        Spot_BracketEdged(lx - off, ly, -1, SPOT_STEM, bc, ba);
        Spot_BracketEdged(rx + off, ry, 1, SPOT_STEM, bc, ba);
        if (c->phase == PH_WINDOW)
            Spot_BracketGhost(c, base, lx, ly, rx, ry);
        return;
    }

    if (!ended)
        return;
    switch (c->phase)
    {
    case PH_HIT:
    {
        int lag = c->lag > 0 ? c->lag : 1;
        if (c->age == 0)
        {
            Hud_Seg(lx - 0.3f, ly, rx + 0.3f, ry, SPOT_LINE + 0.25f, Color_Fill(base, 0.5f));
            Hud_Seg(lx, ly, rx, ry, SPOT_LINE + 0.1f, color_white);
        }
        else if (c->age < lag)
        {
            // burns down over the landing lag, gone when Falcon can act
            float k = 1.f - (float)c->age / lag;
            Hud_Seg(mx + (lx - mx) * k, my + (ly - my) * k, mx + (rx - mx) * k, my + (ry - my) * k, SPOT_LINE, Color_Fill(base, 0.95f));
        }
        if (c->age < 10)
        {
            // sparks running out along the floor
            float q = c->age / 10.f;
            float a = 2.f + 4.f * Ease_Out(q);
            GXColor sc = Color_Fill(base, 0.8f * (1.f - q));
            Hud_Rect(mx - a - 0.9f, my, mx - a, my + 0.12f, sc);
            Hud_Rect(mx + a, my, mx + a + 0.9f, my + 0.12f, sc);
        }
        Spot_BracketGhost(c, base, lx, ly, rx, ry);
        break;
    }
    case PH_FADE:
    {
        // the frame that was needed, then a slow fold inward
        float q = (float)c->age / LL_FADE;
        float k = 1.f - Ease_Out(q);
        GXColor dc = Color_Fill(Dim_Color(c->dim), (c->age == 0 ? 0.9f : 0.5f) * (1.f - q));
        Hud_Seg(mx + (lx - mx) * k, my + (ly - my) * k, mx + (rx - mx) * k, my + (ry - my) * k, SPOT_LINE, dc);
        float o = (rx - mx) * k;
        Spot_Bracket(mx - o, my, -1, SPOT_STEM * k, dc);
        Spot_Bracket(mx + o, my, 1, SPOT_STEM * k, dc);
        Spot_BracketGhost(c, base, lx, ly, rx, ry); // a skip's faint ghost
        break;
    }
    case PH_CUT:
        Hud_Seg(lx, ly, rx, ry, SPOT_LINE, Color_Fill(color_skip, 0.4f * (1.f - (float)c->age / LL_CUT)));
        break;
    }
}

// A small round dot as an octagon, in three quads.
static void Hud_Oct(float x, float y, float r, GXColor c)
{
    float s = r * 0.4142f;
    Hud_Rect(x - r, y - s, x + r, y + s, c);
    Hud_Rect(x - s, y + s, x + s, y + r, c);
    Hud_Rect(x - s, y - r, x + s, y - s, c);
}

// A flat disc on the floor: an ellipse rx wide and ry tall.
static void Hud_Ellipse(float cx, float cy, float rx, float ry, GXColor c)
{
    float px = cx + rx, py = cy;
    for (int i = 1; i <= DISC_SEGS; i++)
    {
        float ang = i * (6.2831853f / DISC_SEGS);
        float nx = cx + cos(ang) * rx, ny = cy + sin(ang) * ry;
        Hud_Tri(cx, cy, px, py, nx, ny, c);
        px = nx;
        py = ny;
    }
}

// An ellipse's outline, w thick all the way around (not thinner where it
// is flat), in enough steps that a flat one stays smooth.
#define ELL_SEGS 20
static void Hud_EllipseRing(float cx, float cy, float rx, float ry, float w, GXColor c)
{
    float ix[ELL_SEGS + 1], iy[ELL_SEGS + 1], ox[ELL_SEGS + 1], oy[ELL_SEGS + 1];
    if (rx < 0.05f)
        rx = 0.05f;
    if (ry < 0.05f)
        ry = 0.05f;
    if (w > ry * 1.6f)
        w = ry * 1.6f; // the inside of a very flat ring can't turn inside out
    for (int i = 0; i <= ELL_SEGS; i++)
    {
        float ang = i * (6.2831853f / ELL_SEGS);
        float ca = cos(ang), sa = sin(ang);
        // the outline's normal there points along (cos / rx, sin / ry)
        float nx = ca / rx, ny = sa / ry, nl = sqrtf(nx * nx + ny * ny);
        nx = nx / nl * w / 2;
        ny = ny / nl * w / 2;
        ix[i] = cx + ca * rx - nx;
        iy[i] = cy + sa * ry - ny;
        ox[i] = cx + ca * rx + nx;
        oy[i] = cy + sa * ry + ny;
    }
    for (int i = 0; i < ELL_SEGS; i++)
        Quad_Add(ix[i], iy[i], ox[i], oy[i], ox[i + 1], oy[i + 1], ix[i + 1], iy[i + 1], c);
}

// The wavedash timers that sit at Falcon's feet, drawn from the waveland
// cue of a wavedash (its jumpsquat, which the cue counts down to the
// airdodge): Pips and Ring. The countdown is the jumpsquat's own frames, so
// a pip or a notch of the ring is a frame; the press frame is the cue's
// window, which opens on the jumpsquat's last frame, as for any timer.
#define WD_PIPS 4      // frames in Falcon's jumpsquat
#define WD_SLOT 2.6f   // the pips' spacing, in world units
#define WD_RING 4.f    // the ring when closed, and
#define WD_STEP 3.2f   // how much wider it is for each frame left, in world units
#define WD_FLAT 0.22f  // how flat it lies

// Where the cue's spot is on the screen, and how many HUD units a world unit
// is there.
static int Wd_Spot(Cue *c, float *mx, float *my, float *sx)
{
    float hx, hy;
    if (!Hud_FromWorld(c->spot.X, c->spot.Y, mx, my) || !Hud_FromWorld(c->spot.X + 10.f, c->spot.Y, &hx, &hy))
        return 0;
    *sx = (hx - *mx) / 10.f;
    if (*sx < 0.05f)
        *sx = 0.05f;
    return 1;
}

// How a wavedash cue looks right now: how far along (k frames before the
// airdodge frame, 0 in the window), how strongly it shows, and the color it
// is drawn in. A press that worked fades it out over the ghost's frames; an
// ending folds it away muted.
typedef struct WdLook
{
    int k;
    int lit;       // jumpsquat frames gone, of WD_PIPS
    float alpha;
    float fold;    // 1 live, shrinking to 0 as a miss or skip folds
    GXColor color; // the cue's color, or the muted one
    int now;       // the frame to press, and it's not muted
} WdLook;

static void Wd_Look(Cue *c, int ended, WdLook *w)
{
    GXColor base = Cue_Color(CUE_WL);
    w->k = c->phase == PH_WINDOW ? 0 : c->left - 1;
    if (w->k < 0)
        w->k = 0;
    w->lit = c->phase == PH_WINDOW ? WD_PIPS : WD_PIPS + 1 - c->left;
    w->lit = w->lit < 0 ? 0 : w->lit > WD_PIPS ? WD_PIPS : w->lit;
    w->alpha = 1.f;
    w->fold = 1.f;
    w->color = c->dim ? Dim_Color(c->dim) : base;
    w->now = !ended && c->phase == PH_WINDOW && !c->dim;
    if (ended && c->phase == PH_FADE)
    {
        float q = (float)c->age / LL_FADE;
        w->alpha = (c->age == 0 ? 0.9f : 0.5f) * (1.f - q);
        w->fold = 1.f - Ease_Out(q);
        w->lit = WD_PIPS;
    }
    else if (ended && c->phase == PH_CUT)
    {
        w->alpha = 0.4f * (1.f - (float)c->age / LL_CUT);
        w->color = color_skip;
    }
    else if (!ended && c->press >= 0)
        w->alpha = 1.f - Clamp01((float)c->press / LL_GHOST); // gone with the ghost
}

// Squat pips: four small pips under Falcon's feet, one lit for each
// jumpsquat frame, then a diamond for the airdodge frame that turns white
// in place when it's due.
static void Wd_Pips(Cue *c, int ended)
{
    float mx, my, sx;
    if (!Wd_Spot(c, &mx, &my, &sx))
        return;
    float s = sx < 0.3f ? 0.3f : sx > 0.55f ? 0.55f : sx; // small shapes, kept readable at any zoom
    float step = WD_SLOT * s, pr = 0.8f * s, dr = 1.4f * s;
    float y = my - 2.f * s, dx = mx + 2.f * step;
    GXColor base = Cue_Color(CUE_WL);

    if (c->phase == PH_FADE || c->phase == PH_CUT || c->phase == PH_COUNT || c->phase == PH_WINDOW)
    {
        WdLook w;
        Wd_Look(c, ended, &w);
        if (w.alpha > 0.01f)
        {
            for (int i = 0; i < WD_PIPS; i++)
            {
                int on = i < w.lit;
                float x = mx + (i - 2) * step;
                GXColor pc = on ? (c->dim || c->phase == PH_FADE || c->phase == PH_CUT ? w.color : color_white) : color_skip;
                Hud_Oct(x, y, pr + 0.1f, Color_Over(color_plate, 0.6f * w.alpha * (on ? 1.f : 0.6f)));
                Hud_Oct(x, y, pr, Color_Over(pc, (on ? 0.95f : 0.4f) * w.alpha));
            }
            GXColor dc = w.now ? Color_Mix(base, color_white, c->age == 0 ? 0.9f : 0.4f) : c->dim || ended ? w.color : Color_Mix(base, color_plate, 0.4f);
            float r = dr * w.fold;
            if (r > 0.05f)
            {
                Hud_Diamond(dx, y, r + 0.14f, Color_Over(color_plate, 0.6f * w.alpha));
                Hud_Diamond(dx, y, r, Color_Over(dc, 0.95f * w.alpha));
            }
        }
    }

    // the one ghost: a disc swelling out of the diamond
    int bright, g = Cue_Ghost(c, &bright);
    if (g >= 0 && c->phase != PH_CUT)
    {
        float q = (float)g / LL_GHOST, fade = (1.f - q) * (1.f - q);
        float r = (1.4f + (bright ? 3.f : 1.2f) * Ease_Out(q)) * s;
        Hud_Disc(dx, y, r, Color_Over(Color_Mix(base, color_white, 0.35f), (bright ? 0.6f : 0.25f) * fade));
    }
}

// Ground ring: a flat ring on the floor around Falcon's feet, a notch wider
// for each jumpsquat frame left, closing on the airdodge frame.
static void Wd_Ring(Cue *c, int ended)
{
    float mx, my, sx;
    if (!Wd_Spot(c, &mx, &my, &sx))
        return;
    GXColor base = Cue_Color(CUE_WL);

    if (c->phase == PH_FADE || c->phase == PH_CUT || c->phase == PH_COUNT || c->phase == PH_WINDOW)
    {
        WdLook w;
        Wd_Look(c, ended, &w);
        float appear = c->phase == PH_COUNT ? Clamp01((c->span - c->left + 1) / 2.f) : 1.f;
        float rw = (WD_RING + WD_STEP * w.k) * w.fold, a = w.alpha * appear;
        GXColor rc = w.now ? color_white : w.color;
        if (rw * sx > 0.1f && a > 0.01f)
        {
            Hud_EllipseRing(mx, my, rw * sx, rw * sx * WD_FLAT, 0.42f, Color_Over(color_plate, 0.5f * a));
            Hud_EllipseRing(mx, my, rw * sx, rw * sx * WD_FLAT, 0.28f, Color_Over(rc, 0.95f * a));
        }
    }

    // the one ghost: a flat disc swelling out of the closed ring
    int bright, g = Cue_Ghost(c, &bright);
    if (g >= 0 && c->phase != PH_CUT)
    {
        float q = (float)g / LL_GHOST, fade = (1.f - q) * (1.f - q);
        float rx = (WD_RING + (bright ? 12.f : 4.f) * Ease_Out(q)) * sx;
        Hud_Ellipse(mx, my, rx, rx * (WD_FLAT + 0.03f), Color_Over(Color_Mix(base, color_white, 0.35f), (bright ? 0.55f : 0.22f) * fade));
    }
}

// Waveland and wavedash: a rail as long as the slide each way, and a mark on
// each end that counts down to the airdodge frame, in the look picked in the
// Timers menu: ticks that slide in and spike where they meet, rails that
// fill from the ends, or chevrons that hop a notch a frame. Every look
// crosses the rail at one speed, whatever the lead; a shorter one just
// starts closer. They meet in the middle on the frame to press, then the
// timer's one ghost plays in place. A white mark shows where the stick held
// now would stop the slide.
#define RAIL_FRAMES (LL_COUNT_FRAMES - 1) // frames a mark takes to cross a whole rail
#define RAIL_FILL 0.3f                    // how thick the filling rails are
#define CHEV_STEPS 8                      // notches a chevron hops over the last frames
#define CHEV_GAP 0.5f                     // the last notch's distance from the spot

// The rails on the screen: the spot and the slide's two ends, and which
// directions work.
typedef struct RailGeom
{
    float mx, my, lx, ly, rx, ry;
    int ok_l, ok_r;
} RailGeom;

// How far along its rail a mark k frames from the airdodge frame is: 0 at the
// spot, 1 at the slide's end.
static float Rail_Q(int k)
{
    return Clamp01((float)k / RAIL_FRAMES);
}

// A bump standing on y: a bell curve wb wide at its foot and h tall, the
// shape of the waveland ticks' wave.
#define BUMP_SLICES 8
static void Hud_Bump(float cx, float y, float wb, float h, GXColor c)
{
    float x0 = cx - wb / 2, dx = wb / BUMP_SLICES;
    float prev = 0;
    for (int i = 1; i <= BUMP_SLICES; i++)
    {
        float u = (float)i / BUMP_SLICES * 2.f - 1.f; // -1..1 across the foot
        float b = 1.f - u * u;
        float top = h * b * b * b;
        Quad_Add(x0 + dx * (i - 1), y, x0 + dx * i, y, x0 + dx * i, y + top, x0 + dx * (i - 1), y + prev, c);
        prev = top;
    }
}

// How tall the spike gets: about Falcon's height on the screen, kept between
// a height that shows and one that fills it.
static float Rail_Peak(Cue *c)
{
    float ax, ay, bx, by;
    if (!Hud_FromWorld(c->spot.X, c->spot.Y, &ax, &ay) || !Hud_FromWorld(c->spot.X, c->spot.Y + 17.f, &bx, &by))
        return 6.f;
    float h = fabs(by - ay);
    return h < 4.5f ? 4.5f : h > 12.f ? 12.f : h;
}

// One of the waveland ticks, q of the way from the spot (0) to the slide's
// end (1): wide and low out at the ends, where it first catches the eye. It
// keeps the same area as its foot narrows at a steady rate, so it shoots up
// taller and faster the closer it gets, to peak tall at the spot, while it
// slides in at a steady speed.
#define TICK_W0 0.6f // the foot at the spot
#define TICK_W1 4.0f // and at the slide's end
static void Rail_Tick(float x, float y, float q, float peak, GXColor c)
{
    float wb = TICK_W0 + (TICK_W1 - TICK_W0) * q;
    float h = peak * TICK_W0 / wb;
    Hud_Bump(x, y - 0.15f, wb + 0.3f, h + 0.25f, Color_Over(color_plate, 0.5f * c.a / 255.f));
    Hud_Bump(x, y - 0.15f, wb, h, c);
}

// The countdown, k frames from the airdodge frame (0 in the window), in
// the picked look, in col at am of its strength.
static void Rail_Count(int look, Cue *c, RailGeom *g, int k, GXColor col, float am)
{
    float q = Rail_Q(k), n = 1.f - q;
    int now = c->phase == PH_WINDOW && !c->dim;
    GXColor tc = now ? Color_Mix(col, color_white, c->age == 0 ? 0.9f : 0.4f) : Color_Mix(col, color_white, 0.35f * n * n);
    GXColor mark = Color_Over(tc, (0.85f + 0.15f * n) * am);

    switch (look)
    {
    case WLT_TICKS:
    {
        float peak = Rail_Peak(c);
        if (k == 0)
        {
            // they have met: one spike
            if (g->ok_l || g->ok_r)
                Rail_Tick(g->mx, g->my, 0, peak, mark);
            break;
        }
        if (g->ok_l)
            Rail_Tick(g->mx + (g->lx - g->mx) * q, g->my + (g->ly - g->my) * q, q, peak, mark);
        if (g->ok_r)
            Rail_Tick(g->mx + (g->rx - g->mx) * q, g->my + (g->ry - g->my) * q, q, peak, mark);
        break;
    }
    case WLT_RAILS:
        // the rail fills from its end toward the spot, the same share each
        // frame on both sides, and is full on the airdodge frame
        for (int side = -1; side <= 1; side += 2)
        {
            if (side < 0 ? !g->ok_l : !g->ok_r)
                continue;
            float ex = side < 0 ? g->lx : g->rx, ey = side < 0 ? g->ly : g->ry;
            if (n < 0.001f)
                continue;
            float tx = ex + (g->mx - ex) * n, ty = ey + (g->my - ey) * n;
            Hud_Seg(ex, ey, tx, ty, RAIL_FILL + 0.14f, Color_Over(color_plate, 0.5f * am));
            Hud_Seg(ex, ey, tx, ty, RAIL_FILL, mark);
            if (k > 0)
                Hud_Rect(tx - 0.08f, ty - 0.12f, tx + 0.08f, ty + 0.6f, Color_Over(Color_Mix(tc, color_white, 0.5f), 0.9f * am));
        }
        break;
    case WLT_CHEVRONS:
    {
        // arrowheads pointing at the spot, a notch closer a frame over the
        // last CHEV_STEPS; the notches are marked under the rail
        int step = k > CHEV_STEPS ? CHEV_STEPS : k;
        for (int side = -1; side <= 1; side += 2)
        {
            if (side < 0 ? !g->ok_l : !g->ok_r)
                continue;
            float ex = side < 0 ? g->lx : g->rx, ey = side < 0 ? g->ly : g->ry;
            float dx = ex - g->mx, dy = ey - g->my, len = sqrtf(dx * dx + dy * dy);
            if (len < 3.f * CHEV_GAP)
                continue;
            float ux = dx / len, uy = dy / len, notch = (len - CHEV_GAP) / (CHEV_STEPS + 1);
            for (int i = 1; i <= CHEV_STEPS; i++)
            {
                float p = CHEV_GAP + i * notch;
                Hud_Rect(g->mx + ux * p - 0.05f, g->my + uy * p - 0.45f, g->mx + ux * p + 0.05f, g->my + uy * p - 0.1f, Color_Over(color_white, 0.4f * am));
            }
            float p = CHEV_GAP + step * notch;
            float tx = g->mx + ux * p, ty = g->my + uy * p + 0.75f;
            float bx = tx + ux * 0.65f, by = ty + uy * 0.65f, wx = -uy * 0.7f, wy = ux * 0.7f;
            GXColor edge = Color_Over(color_plate, 0.5f * am);
            Hud_Seg(bx + wx, by + wy, tx, ty, 0.42f, edge);
            Hud_Seg(bx - wx, by - wy, tx, ty, 0.42f, edge);
            Hud_Seg(bx + wx, by + wy, tx, ty, 0.26f, mark);
            Hud_Seg(bx - wx, by - wy, tx, ty, 0.26f, mark);
        }
        break;
    }
    }
}

// The rails' one ghost (Cue_Ghost), in place like the look it belongs to,
// strong and growing for a press that worked, small and dim for a skip:
// the spike swells into a glow (Ticks), the rails swell (Rails), a disc
// rises at the spot (Chevrons). Nothing runs back out along the rail.
static void Spot_RailGhost(int look, Cue *c, GXColor base, RailGeom *g)
{
    int bright, gh = Cue_Ghost(c, &bright);
    if (gh < 0)
        return;
    float q = (float)gh / LL_GHOST, e = Ease_Out(q), fade = (1.f - q) * (1.f - q);
    GXColor gc = Color_Mix(base, color_white, bright ? 0.4f : 0.2f);
    switch (look)
    {
    case WLT_TICKS:
    {
        float wb = TICK_W0 + (bright ? 2.2f : 1.0f) * e;
        float h = Rail_Peak(c) * (bright ? 1.f + 0.25f * e : 0.5f);
        Hud_Bump(g->mx, g->my - 0.15f, wb, h, Color_Over(gc, (bright ? 0.95f : 0.45f) * fade));
        break;
    }
    case WLT_RAILS:
    {
        float ax = g->ok_l ? g->lx : g->mx, ay = g->ok_l ? g->ly : g->my;
        float bx = g->ok_r ? g->rx : g->mx, by = g->ok_r ? g->ry : g->my;
        Hud_Seg(ax, ay, bx, by, RAIL_FILL + 2.f * (bright ? 0.9f : 0.35f) * e, Color_Over(gc, (bright ? 0.6f : 0.25f) * fade));
        break;
    }
    case WLT_CHEVRONS:
        Hud_Disc(g->mx, g->my + 0.5f, bright ? 0.5f + 1.8f * e : 0.3f + 0.6f * e, Color_Over(gc, (bright ? 0.6f : 0.25f) * fade));
        break;
    }
}

static void Spot_Rails(Cue *c, FighterData *fp, int ended)
{
    float reach = Slide_Distance(fp, common_dodge_force);
    float L = c->spot.X - reach, R = c->spot.X + reach;
    int off_l = L < c->x0, off_r = R > c->x1;
    if (off_l)
        L = c->x0;
    if (off_r)
        R = c->x1;
    float mx, my, lx, ly, rx, ry;
    if (!Hud_FromWorld(c->spot.X, c->spot.Y, &mx, &my))
        return;
    Hud_FromWorld(L, c->spot.Y, &lx, &ly);
    Hud_FromWorld(R, c->spot.Y, &rx, &ry);
    GXColor base = Cue_Color(CUE_WL);
    int ok_l = c->wd || (c->dirs & DODGE_LEFT);
    int ok_r = c->wd || (c->dirs & DODGE_RIGHT);
    RailGeom g = {mx, my, lx, ly, rx, ry, ok_l, ok_r};
    int look = Options_Timers[TOPT_WL].val;
    // a wavedash out of the jumpsquat has a timer of its own (Wavedash in
    // the menu), so the rails only mark the slide then, unless that timer
    // is the strip's row
    int timed = !c->wd || Options_Timers[TOPT_WD].val == WDT_CELLS;

    if (!ended && (c->phase == PH_COUNT || c->phase == PH_WINDOW))
    {
        GXColor col = c->dim ? Dim_Color(c->dim) : base;
        float am = c->dim ? 0.5f : 1.f;

        Hud_Seg(lx, ly, mx, my, 0.12f, Color_Fill(col, (ok_l ? 0.4f : 0.15f) * am));
        Hud_Seg(mx, my, rx, ry, 0.12f, Color_Fill(col, (ok_r ? 0.4f : 0.15f) * am));
        Hud_Rect(mx - 0.08f, my - 0.15f, mx + 0.08f, my + 0.5f, Color_Fill(col, 0.8f * am));
        for (int side = -1; side <= 1; side += 2)
        {
            int ok = side < 0 ? ok_l : ok_r;
            int off = side < 0 ? off_l : off_r;
            float ex = side < 0 ? lx : rx, ey = side < 0 ? ly : ry;
            GXColor ec = Color_Fill(col, (ok ? 0.8f : 0.3f) * am);
            if (off)
            {
                // slides off here
                Hud_Tri(ex, ey + 0.3f, ex + side * 0.7f, ey - 0.2f, ex, ey - 0.7f, ec);
            }
            else
                Hud_Rect(ex - 0.1f, ey, ex + 0.1f, ey + 0.75f, ec);
        }

        // a countdown fades in over its first 2 frames
        int k = c->phase == PH_WINDOW ? 0 : c->left - 1;
        float appear = c->phase == PH_COUNT ? Clamp01((c->span - c->left + 1) / 2.f) : 1.f;
        if (timed)
            Rail_Count(look, c, &g, k < 0 ? 0 : k, col, am * appear);
        if (timed && c->phase == PH_WINDOW)
            Spot_RailGhost(look, c, base, &g);

        // where the stick held now would stop the slide: the dodge takes
        // its angle and keeps the sideways part of its speed
        float sx = fp->input.lstick.X, sy = fp->input.lstick.Y;
        float len = sqrtf(sx * sx + sy * sy);
        if (!c->dim && sx != 0 && len >= common_dodge_deadzone.X)
        {
            float d = Slide_Distance(fp, common_dodge_force * fabs(sx) / len);
            float x = sx > 0 ? c->spot.X + d : c->spot.X - d;
            float hx, hy;
            if (x < L)
                x = L;
            if (x > R)
                x = R;
            Hud_FromWorld(x, c->spot.Y, &hx, &hy);
            Hud_Rect(hx - 0.09f, hy - 0.35f, hx + 0.09f, hy + 0.85f, Color_Fill(color_white, 0.9f));
        }
        return;
    }

    if (!ended)
        return;
    switch (c->phase)
    {
    case PH_HIT:
    {
        int age = c->age, len = 24 + c->lag;
        float s = c->soft ? 0.6f : 1.f;
        if (age == 0 && !c->soft)
        {
            Hud_Seg(lx, ly, rx, ry, 0.45f, Color_Fill(base, 0.45f));
            Hud_Seg(lx, ly, rx, ry, 0.2f, color_white);
        }
        else
        {
            // how far you slid against how far it goes
            float fade = age > len - 6 ? (len - age) / 6.f : 1.f;
            GXColor rc = Color_Fill(base, 0.35f * fade * s);
            Hud_Seg(lx, ly, rx, ry, 0.12f, rc);
            Hud_Rect(lx - 0.1f, ly, lx + 0.1f, ly + 0.75f, Color_Fill(base, 0.6f * fade * s));
            Hud_Rect(rx - 0.1f, ry, rx + 0.1f, ry + 0.75f, Color_Fill(base, 0.6f * fade * s));
            float fx = fp->phys.pos.X < L ? L : fp->phys.pos.X > R ? R : fp->phys.pos.X;
            float hx, hy;
            Hud_FromWorld(fx, c->spot.Y, &hx, &hy);
            Hud_Rect(hx - 0.1f, hy - 0.2f, hx + 0.1f, hy + 1.0f, Color_Fill(color_white, 0.85f * fade));
        }
        // the ghost carries on from the press
        if (timed)
            Spot_RailGhost(look, c, base, &g);
        break;
    }
    case PH_FADE:
    {
        float q = (float)c->age / LL_FADE;
        float k = 1.f - Ease_Out(q);
        GXColor dc = Color_Fill(Dim_Color(c->dim), (c->age == 0 ? 0.9f : 0.5f) * (1.f - q));
        Hud_Seg(mx + (lx - mx) * k, my + (ly - my) * k, mx + (rx - mx) * k, my + (ry - my) * k, 0.14f, dc);
        Hud_Rect(mx + (lx - mx) * k - 0.1f, my, mx + (lx - mx) * k + 0.1f, my + 1.1f * k, dc);
        Hud_Rect(mx + (rx - mx) * k - 0.1f, my, mx + (rx - mx) * k + 0.1f, my + 1.1f * k, dc);
        if (timed)
            Spot_RailGhost(look, c, base, &g); // a skip's faint ghost
        break;
    }
    case PH_CUT:
        Hud_Seg(lx, ly, rx, ry, 0.12f, Color_Fill(color_skip, 0.35f * (1.f - (float)c->age / LL_CUT)));
        break;
    }
}
