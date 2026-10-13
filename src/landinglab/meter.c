/* landinglab/meter.c
 *
 * Holds the meter: rows built from the cues, settled, and drawn as cells.
 *
 * Part of Landing Gear: src/landinglab.c includes it, in order, into one
 * translation unit, so it sees everything included before it (and mex.h,
 * events.h, landinglab_sim.h).
 *
 * Uses: option_values, menus_display, tracking_state, rumble, hud_text,
 * timers, drawing, hud_drawing.
 * Refers ahead, through forward declarations, to: ledge_routes, jump_timing,
 * jump_search, ledge_practice.
 */

///////////////////////
/// The meter       ///
///////////////////////

#define MT_CELLS 40
#define MT_ROWS 5
#define MT_PITCH 0.622f // 35% bigger than the mockup's
#define MT_CW 0.498f
#define MT_CH 0.949f
#define MT_GAP 0.271f
#define MT_WIDE 0.074f   // Wide Cells: a fifth of the mockup's cell width
#define MT_FIXED 1.5f    // the fixed strip's scale
#define MT_FIXED_MAX 20  // ... and its most cells, leaving room for the stick
#define ST_TEXT 0.36f      // every fixed strip label's and note's text size (Stephen, 0.9.9: they should match)
#define MT_LABEL_W 2.6f    // the cells' label column: the longest label ("Route", "JUMP") at ST_TEXT
#define MT_LABEL_GAP 0.95f // from the labels' right edge to the gate, before the scale: past the lit ticks

enum cell_kind
{
    CELL_NONE,
    CELL_PRESS, // an input: solid
    CELL_ACT,   // the first frame Falcon can act: solid, white cap
    CELL_AIR,   // in the air between inputs: a light dash
    CELL_LAND,  // touchdown: hollow, with a floor bar
    CELL_LAG,   // landing lag: a low bar
    CELL_TAIL,  // ledge intangibility left: periwinkle
};

enum cell_tone
{
    TONE_CUE,
    TONE_MISS,
    TONE_SKIP,
    TONE_JUMP, // a jump inside another cue's row (a wavedash's): jump yellow
};

enum glyph_kind
{
    GLYPH_NONE,
    GLYPH_LEFT,   // stick left
    GLYPH_RIGHT,  // stick right
    GLYPH_DOWN,   // stick down
    GLYPH_JUMP,   // jump
    GLYPH_AERIAL, // an aerial
    GLYPH_DODGE,  // airdodge
    GLYPH_UP,     // stick (or C-stick) up
    GLYPH_DOWN_LEFT,
    GLYPH_DOWN_RIGHT,
    GLYPH_FF,            // fastfall: a double chevron, where a plain hold down is one solid arrow
    GLYPH_FF_DOWN_LEFT,
    GLYPH_FF_DOWN_RIGHT,
};

typedef struct MeterRow
{
    u8 cell[MT_CELLS];
    u8 tone[MT_CELLS];
    u8 alpha[MT_CELLS];
    u8 glyph[MT_CELLS];
    GXColor color;
    int len;      // cells used: the last one + 1
    u8 hot;       // a press is at the gate in its window: 1, 2 on the window's first or last frame
    u8 spent;     // frames of that window already gone: the cells show what's left, the dial the whole wedge
    u8 dim;       // a route that isn't the chosen one
    s8 ghost[4];  // ages of the ghosts spreading out of the gate, -1 none
    u8 ghost_bright[4];
    s8 burst;     // frames since a hit, -1 none
    s8 implode;   // frames since a miss or skip folded in, -1 none
    u8 implode_tone;
    s8 flash;     // frames since the gate reached the first frame Falcon can act, -1 none
    float fade;   // Auto Fade (and a lane fading out): the whole row's opacity scaled by this
    u8 gap;       // a lane kept free for a row that went, so the others stay put
    u8 span;      // cells from the gate its first window was at when it came up (Note Travel)
    u8 parked;    // a preload: laid out from a press that hasn't happened, not moving yet
    u8 settle;    // frames since it stopped being parked and set down, + 1 (0: none)
    char label[8];
    char info[24];
} MeterRow;

static MeterRow meter[MT_ROWS];
static int meter_rows;

static MeterRow *Meter_Add(GXColor color, const char *label)
{
    if (meter_rows >= MT_ROWS)
        return 0;
    MeterRow *r = &meter[meter_rows++];
    memset(r, 0, sizeof(*r));
    r->color = color;
    r->fade = 1.f;
    r->burst = -1;
    r->implode = -1;
    r->flash = -1;
    for (int i = 0; i < 4; i++)
        r->ghost[i] = -1;
    sprintf(r->label, "%s", label);
    return r;
}

static void Row_Set(MeterRow *r, int k, int kind, int tone, float alpha)
{
    if (k < 0 || k >= MT_CELLS)
        return;
    r->cell[k] = kind;
    r->tone[k] = tone;
    r->alpha[k] = 255 * Clamp01(alpha);
    if (k + 1 > r->len)
        r->len = k + 1;
}

static void Row_Ghost(MeterRow *r, int age, int bright)
{
    for (int i = 0; i < 4; i++)
    {
        if (r->ghost[i] < 0)
        {
            r->ghost[i] = age;
            r->ghost_bright[i] = bright;
            return;
        }
    }
}

// The one ghost a timer sends out: a strong one on a press that works (or
// on the hit, when there was no press), else a faint one on the window's
// last frame, which a dry run can time against. An early or late press
// sends none, so it can't be mistaken for either. Returns its age, or -1.
static int Cue_Ghost(Cue *c, int *bright)
{
    if (c->press >= 0)
    {
        *bright = 1;
        return c->press < LL_GHOST ? c->press : -1;
    }
    *bright = 0;
    if (c->dim == DIM_MISS || c->pulse < 0)
        return -1;
    int g = c->pulse - (c->width - 1);
    return g >= 0 && g < LL_GHOST ? g : -1;
}

static void Row_Ghosts(MeterRow *r, Cue *c)
{
    int bright, g = Cue_Ghost(c, &bright);
    if (g >= 0)
        Row_Ghost(r, g, bright);
    if (c->press >= 0 && c->press < LL_BURST)
        r->burst = c->press; // the burst goes with the hit's ghost
}

// The ledge intangibility tail after the first frame Falcon can act (act,
// in cells from the gate).
static void Row_Tail(MeterRow *r, int act)
{
    for (int k = act + 1 > 0 ? act + 1 : 0; k < galint_now - 1; k++)
        Row_Set(r, k, CELL_TAIL, TONE_CUE, 0.85f);
}

static const char *cue_labels[CUE_NUM] = {"AI", "WL", "NIL"};

// One row per kind: its countdown, and the ending of the last one.
static void Meter_FromCue(int kind)
{
    Cue *c = &cue_live[kind], *e = &cue_end[kind];
    int live = c->phase != PH_OFF;
    if (!live && e->phase == PH_OFF)
        return;
    int wd = kind == CUE_WL && (live ? c->wd : e->wd);
    if (wd && Options_Timers[TOPT_WD].val != WDT_CELLS)
        return; // drawn by Falcon's feet, or not at all
    if (kind == CUE_WL && !wd && !Cues_Waveland())
        return;
    MeterRow *r = Meter_Add(Cue_Color(kind), wd ? "WD" : cue_labels[kind]);
    if (!r)
        return;
    r->fade = Fade_Factor(VG_TIMERS, kind);

    switch (e->phase)
    {
    case PH_HIT:
    {
        int act = Cue_FlashAge(e) - e->age;
        for (int k = 0; k < act; k++)
            Row_Set(r, k, CELL_LAG, TONE_CUE, 0.7f);
        if (act >= 0)
            Row_Set(r, act, CELL_ACT, TONE_CUE, 1);
        Row_Tail(r, act);
        if (act <= 0 && -act < 4)
            r->flash = -act;
        Row_Ghosts(r, e);
        if (galint_now > 1 && act < 0)
            sprintf(r->info, "%d GALINT", galint_now - 1);
        else if (act > 0)
            sprintf(r->info, "lag %d", act);
        else
            sprintf(r->info, "act");
        break;
    }
    case PH_FADE:
        r->implode = e->age;
        r->implode_tone = e->dim == DIM_MISS ? TONE_MISS : TONE_SKIP;
        Row_Ghosts(r, e); // a skip's faint ghost plays out
        sprintf(r->info, e->dim == DIM_MISS ? "miss" : "skip");
        break;
    case PH_CUT:
    {
        float a = 0.5f * (1.f - (float)e->age / LL_CUT);
        for (int j = 0; j < e->width; j++)
            Row_Set(r, e->left - 1 + j, CELL_PRESS, TONE_SKIP, a);
        break;
    }
    }

    if (!live)
        return;
    int tone = c->dim == DIM_MISS ? TONE_MISS : c->dim ? TONE_SKIP : TONE_CUE;
    // fades in over 2 frames, but a wavedash's countdown takes over from its
    // preload in place (0.9.15 test: it dimmed on the jumpsquat's first frame)
    float a = c->phase == PH_COUNT && !c->wd ? Clamp01((c->span - c->left + 1) / 2.f) : 1.f;
    if (kind == CUE_NIL)
    {
        Row_Set(r, c->left - 2, CELL_LAND, tone, a);
        Row_Set(r, c->left - 1, CELL_ACT, tone, a);
        sprintf(r->info, "%df", c->left - 1);
    }
    else if (c->phase == PH_COUNT)
    {
        for (int j = 0; j < c->width; j++)
            Row_Set(r, c->left - 1 + j, CELL_PRESS, tone, a);
        sprintf(r->info, "%df", c->left - 1);
    }
    else
    {
        for (int j = c->age; j < c->width; j++)
            Row_Set(r, j - c->age, CELL_PRESS, tone, 1);
        r->spent = c->age < c->width ? c->age : c->width;
        if (!c->dim)
            r->hot = c->age == 0 || c->age == c->width - 1 ? 2 : 1;
        Row_Ghosts(r, c);
        sprintf(r->info, c->dim ? "miss" : "now");
    }
}

static int route_rows_active; // ledge routes fill the meter; the cues' rows give way
static int jump_rows_active;  // ... or it's a jump's route (Jump Timing), which keeps the controller cues
static int Jump_PressAhead(void);
static int drop_sid; // Falcon's state, for the drop drill's row
static void Meter_AddDrop(int sid);
static int route_dj_done;     // ... and its double jump happened
static void Meter_AddRoutes(void);
static void Meter_AddJump(void);

// On the ground and able to jump, the wavedash out of a jump now, as a
// preload: the jump, then the airdodge one jumpsquat later, hovering until
// the jump comes, when the WD row's own countdown takes over in its place
// (Stephen, 0.9.10: Fox's 3 frames to Bowser's 8).
static void Meter_AddWdPreload(void)
{
    if (!wd_preload || Options_Timers[TOPT_WD].val != WDT_CELLS)
        return;
    if (cue_live[CUE_WL].phase != PH_OFF || cue_end[CUE_WL].phase != PH_OFF)
        return; // a waveland or wavedash row is up already
    MeterRow *r = Meter_Add(Cue_Color(CUE_WL), "WD");
    if (!r)
        return;
    r->parked = 1;
    r->fade = 0.85f * Fade_Factor(VG_TIMERS, CUE_WL);
    Row_Set(r, 0, CELL_PRESS, TONE_JUMP, 1);
    r->glyph[0] = GLYPH_JUMP;
    for (int j = 0; j < 2; j++)
        Row_Set(r, wd_preload - 1 + j, CELL_PRESS, TONE_CUE, 1);
    r->glyph[wd_preload - 1] = GLYPH_DODGE;
}

static void Meter_Collect(void)
{
    meter_rows = 0;
    route_rows_active = 0;
    jump_rows_active = 0;
    Meter_AddRoutes();
    Meter_AddJump();
    Meter_AddDrop(drop_sid);
    if (route_rows_active)
    {
        // a ledge route has the meter to itself. A jump's plan keeps the
        // wavedash out of a jump beside it (0.9.13 test: the standing hop
        // plan hid the WD preload, and the jumpsquat's countdown with it)
        Cue *c = &cue_live[CUE_WL], *e = &cue_end[CUE_WL];
        if (jump_rows_active && Cues_Wd() && (c->phase != PH_OFF ? c->wd : e->phase != PH_OFF && e->wd))
            Meter_FromCue(CUE_WL);
        if (jump_rows_active)
            Meter_AddWdPreload();
        return;
    }
    if (Cues_Ai())
        Meter_FromCue(CUE_AI);
    if (Cues_Waveland() || Cues_Wd())
        Meter_FromCue(CUE_WL);
    Meter_AddWdPreload();
    if (Cues_Nil())
        Meter_FromCue(CUE_NIL);
}

// Lanes keep their places: a row keeps the lane it came up in (by its
// label) while it's up and for MT_LINGER frames after, fading out, so the
// others never shift or swap. One that went with a window still ahead
// leaves that window in gray as it fades, rather than just vanishing. A new
// row takes the first free lane. Each lane also remembers how far out its
// window was when it came up, for Note Travel.
#define MT_LINGER 8
typedef struct Lane
{
    char label[8];
    int seen;    // the frame it was last up
    int k_prev;  // its first window's cell last frame, -1 none
    int k0;      // ... and when that window came up
    u8 used;
    u8 parked;   // its row was a preload last frame
    u8 settle;   // ... and the row's settle
    MeterRow last;
} Lane;
static Lane lanes[MT_ROWS];

static int Str_Same(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

// The first cell that is a window to press in, a touchdown or the first
// frame to act, or -1.
static int Row_FirstTarget(MeterRow *r)
{
    for (int k = 0; k < r->len && k < MT_CELLS; k++)
        if (r->cell[k] == CELL_PRESS || r->cell[k] == CELL_LAND || r->cell[k] == CELL_ACT)
            return k;
    return -1;
}

static void Meter_Settle(void)
{
    static MeterRow built[MT_ROWS];
    int now = event_vars->game_timer, n = meter_rows;
    int taken[MT_ROWS], at[MT_ROWS];
    memcpy(built, meter, n * sizeof(MeterRow));
    for (int l = 0; l < MT_ROWS; l++)
    {
        taken[l] = -1;
        if (lanes[l].used && (now < lanes[l].seen || now - lanes[l].seen > MT_LINGER))
        {
            lanes[l].used = 0; // gone long enough, or the clock went back (a save state)
            if (cue_log)
                OSReport("LLLANE %d %s lane %d gone\n", now, lanes[l].label, l);
        }
    }
    // the lanes the rows already had, then the first free one for the rest
    for (int i = 0; i < n; i++)
    {
        at[i] = -1;
        for (int l = 0; l < MT_ROWS && at[i] < 0; l++)
            if (lanes[l].used && taken[l] < 0 && Str_Same(lanes[l].label, built[i].label))
                at[i] = l;
        if (at[i] >= 0)
            taken[at[i]] = i;
    }
    for (int i = 0; i < n; i++)
    {
        if (at[i] >= 0)
            continue;
        int best = -1;
        for (int l = 0; l < MT_ROWS && best < 0; l++)
            if (!lanes[l].used && taken[l] < 0)
                best = l;
        // all taken: the one gone longest
        for (int l = 0; l < MT_ROWS && best < 0; l++)
            if (taken[l] < 0 && (best < 0 || lanes[l].seen < lanes[best].seen))
                best = l;
        if (best < 0)
            continue;
        Lane *L = &lanes[best];
        memset(L, 0, sizeof(*L));
        strcpy(L->label, built[i].label);
        L->k_prev = -1;
        if (cue_log)
            OSReport("LLLANE %d %s lane %d new\n", now, L->label, best);
        at[i] = best;
        taken[best] = i;
    }
    for (int i = 0; i < n; i++)
    {
        if (at[i] < 0)
            continue;
        Lane *L = &lanes[at[i]];
        int t = Row_FirstTarget(&built[i]);
        if (t >= 0 && (L->k_prev < 0 || t > L->k_prev))
            L->k0 = t; // a new window (or one further out than the last)
        L->k_prev = t;
        built[i].span = t >= 0 && L->k0 < 255 ? L->k0 : 0;
        // a preload that just set down (its press came) eases onto the belt
        if (L->parked && !built[i].parked)
            L->settle = 1;
        else if (L->settle && L->settle < 8)
            L->settle++;
        else
            L->settle = 0;
        L->parked = built[i].parked;
        built[i].settle = L->settle;
        L->last = built[i];
        L->seen = now;
        L->used = 1;
    }

    int top = -1;
    for (int l = 0; l < MT_ROWS; l++)
        if (lanes[l].used)
            top = l;
    meter_rows = top + 1;
    for (int l = 0; l <= top; l++)
    {
        MeterRow *r = &meter[l];
        Lane *L = &lanes[l];
        if (taken[l] >= 0)
        {
            *r = built[taken[l]];
            continue;
        }
        if (!L->used)
        {
            memset(r, 0, sizeof(*r));
            r->gap = 1;
            r->fade = 1.f;
            r->burst = r->implode = r->flash = -1;
            for (int g = 0; g < 4; g++)
                r->ghost[g] = -1;
            continue;
        }
        // going: fades out over MT_LINGER frames
        *r = L->last;
        r->fade *= 1.f - (float)(now - L->seen) / (MT_LINGER + 1);
        r->hot = 0;
        r->flash = r->burst = r->implode = -1;
        r->info[0] = 0;
        int t = Row_FirstTarget(r);
        if (t >= 2)
        {
            for (int k = 0; k < r->len; k++)
                r->tone[k] = TONE_SKIP; // a window it no longer has, in gray
        }
        else
            r->len = 0;
    }
}

static void Meter_Build(void)
{
    Meter_Collect();
    Meter_Settle();
}

// Where cell k of a row is along its track, in frames from the gate, with
// track frames in all. Catch Up brings a window that came up nearer than the
// track's end in from the end, settling to the fixed speed halfway to the
// gate (a smoothstep), so every window travels the whole track.
static float Note_At(MeterRow *r, float k, int track)
{
    int k0 = r->span;
    if (Options_Timers[TOPT_TRAVEL].val != TRAVEL_CATCH || k0 < 2 || k0 >= track)
        return k;
    float m = k0 * 0.5f, extra = track - k0;
    if (k <= m)
        return k;
    if (k >= k0)
        return k + extra; // past it (a window's later frames): off the far end
    float t = (k - m) / (k0 - m);
    return k + extra * t * t * (3.f - 2.f * t);
}

static GXColor Tone_Color(MeterRow *r, int tone)
{
    if (tone == TONE_JUMP)
        return color_in_jump;
    return tone == TONE_MISS ? color_miss : tone == TONE_SKIP ? color_skip : r->color;
}

// Two chevrons stacked along (dx, dy), like speed lines: a fastfall.
static void Glyph_Chevrons(float x, float y, float r, float dx, float dy, float w, GXColor c)
{
    float px = -dy, py = dx;
    for (int i = 0; i < 2; i++)
    {
        float t = i ? r * 0.95f : r * 0.05f; // the tips, along the way down
        float tx = x + dx * t, ty = y + dy * t;
        float bx = tx - dx * r * 0.85f, by = ty - dy * r * 0.85f; // narrow, so turned 45 degrees it isn't an L
        Hud_Seg(bx + px * r * 0.62f, by + py * r * 0.62f, tx, ty, w, c);
        Hud_Seg(bx - px * r * 0.62f, by - py * r * 0.62f, tx, ty, w, c);
    }
}

static void Glyph_Draw(int glyph, float x, float y, float s, GXColor c)
{
    GXColor shade = Color_Fill(color_plate, c.a / 255.f * 0.9f);
    for (int pass = 0; pass < 2; pass++)
    {
        float r = pass ? s : s + 0.12f;
        GXColor col = pass ? c : shade;
        switch (glyph)
        {
        case GLYPH_LEFT:
            Hud_Tri(x + r * 0.8f, y + r, x + r * 0.8f, y - r, x - r, y, col);
            break;
        case GLYPH_RIGHT:
            Hud_Tri(x - r * 0.8f, y - r, x - r * 0.8f, y + r, x + r, y, col);
            break;
        case GLYPH_DOWN:
            Hud_Tri(x - r, y + r * 0.7f, x + r, y + r * 0.7f, x, y - r, col);
            break;
        case GLYPH_UP:
            Hud_Tri(x + r, y - r * 0.7f, x - r, y - r * 0.7f, x, y + r, col);
            break;
        case GLYPH_DOWN_LEFT:
        case GLYPH_DOWN_RIGHT:
        {
            // the down arrow turned 45 degrees
            float ux = (glyph == GLYPH_DOWN_RIGHT ? 0.707f : -0.707f), uy = -0.707f;
            float bx = x - ux * r * 0.7f, by = y - uy * r * 0.7f;
            Hud_Tri(bx + uy * r, by - ux * r, bx - uy * r, by + ux * r, x + ux * r, y + uy * r, col);
            break;
        }
        case GLYPH_JUMP:
            // a button with an up arrow cut into it
            Hud_Disc(x, y, r, col);
            if (pass)
                Hud_Tri(x + s * 0.55f, y - s * 0.3f, x - s * 0.55f, y - s * 0.3f, x, y + s * 0.5f, shade);
            break;
        case GLYPH_AERIAL:
            Hud_Disc(x, y, r, col);
            break;
        case GLYPH_DODGE:
            // a shield bubble
            Hud_Ring(x, y, s * 0.75f, (pass ? 0.45f : 0.45f + 0.24f / s) * s, col);
            break;
        case GLYPH_FF:
        case GLYPH_FF_DOWN_LEFT:
        case GLYPH_FF_DOWN_RIGHT:
        {
            float dx = glyph == GLYPH_FF ? 0 : glyph == GLYPH_FF_DOWN_RIGHT ? 0.707f : -0.707f;
            float dy = glyph == GLYPH_FF ? -1.f : -0.707f;
            float w = 0.3f * s + (pass ? 0 : 0.16f);
            Glyph_Chevrons(x - dx * s * 0.1f, y - dy * s * 0.1f, s, dx, dy, w, col); // centered
            break;
        }
        }
    }
}

// The meter with its gate's left edge at gx and its bottom at base. scale
// is 1 above Falcon and MT_FIXED for the fixed strip, which also gets
// labels. Returns nothing drawn when there are no rows.
static void Meter_Draw(float gx, float base, float scale, int max_cells, int labels)
{
    int wide = Options_Timers[TOPT_WIDE].val;
    float pitch = (MT_PITCH + (wide ? MT_WIDE : 0)) * scale;
    float cw = (MT_CW + (wide ? MT_WIDE : 0)) * scale;
    float ch = MT_CH * scale, gap = MT_GAP * scale;
    int rows = meter_rows;

    int len = 1, glyphs = 0;
    for (int i = 0; i < rows; i++)
    {
        if (meter[i].len > len)
            len = meter[i].len;
        for (int k = 0; k < meter[i].len; k++)
            glyphs |= meter[i].glyph[k];
    }
    if (len > max_cells)
        len = max_cells;

    float top = base + rows * (ch + gap) - gap;
    float glyph_h = glyphs ? 1.1f * scale : 0;
    float x1 = gx + (len - 1) * pitch + cw;

    // the plate, so it reads over any stage; it fades with the brightest row
    float base_k = vis_k, plate_k = 0;
    for (int i = 0; i < rows; i++)
        if (!meter[i].gap && meter[i].fade > plate_k)
            plate_k = meter[i].fade;
    vis_k = base_k * plate_k;
    // (with labels, it reaches over their column too: one panel, no plate
    // per label to overlap the next row's)
    float plate_x0 = labels ? gx - MT_LABEL_GAP * scale - MT_LABEL_W - 0.25f : gx - 0.32f * scale;
    // (deeper under the rows than over them: the time ticks go there)
    Hud_Rect(plate_x0, base - 0.5f * scale, x1 + 0.22f * scale, top + 0.22f * scale + glyph_h, Color_Fill(color_plate, 0.72f));

    // the game's time going by under the rows: a tick a frame, longer and
    // brighter on every fifth frame of the clock, moving a cell toward the
    // gate each frame, like the highway's and the conveyor's lines (0.9.17's
    // were under a pixel and didn't show)
    int now = event_vars->game_timer;
    for (int k = 1; k < len; k++)
    {
        int five = (now + k) % 5 == 0;
        float tx = gx + k * pitch + cw / 2, e = (five ? 1.4f : 0.8f) * PX;
        Hud_Rect(tx - e, base - (five ? 0.44f : 0.34f) * scale, tx + e, base - (five ? 0.08f : 0.14f) * scale,
                 Color_Over(color_white, five ? 0.6f : 0.25f));
    }

    // rows from the bottom up, each in its own lane (Meter_Settle)
    for (int i = 0; i < rows; i++)
    {
        MeterRow *r = &meter[i];
        if (r->gap)
            continue;
        vis_k = base_k * r->fade;
        float y0 = base + i * (ch + gap), y1 = y0 + ch;
        float dim = r->dim ? 0.5f : 1.f;

        // empty frames
        for (int k = 1; k < len; k++)
        {
            if (r->cell[k] == CELL_NONE)
                Hud_Rect(gx + k * pitch, y0, gx + k * pitch + cw, y1, (now + k) % 5 == 0 ? Color_Over(color_white, 0.16f * dim)
                                                                                    : Color_Fill(color_white, 0.1f * dim));
        }

        for (int k = 0; k < r->len && k < len; k++)
        {
            int kind = r->cell[k];
            if (kind == CELL_NONE)
                continue;
            float a = r->alpha[k] / 255.f * dim;
            GXColor col = Tone_Color(r, r->tone[k]);
            float at = Note_At(r, k, len);
            if (at > len - 0.5f)
                continue; // still off the far end
            float cx0 = gx + at * pitch, cx1 = cx0 + cw;
            switch (kind)
            {
            case CELL_PRESS:
                Hud_Rect(cx0, y0, cx1, y1, Color_Fill(col, a));
                break;
            case CELL_ACT:
                Hud_Rect(cx0, y0, cx1, y1, Color_Fill(col, a));
                Hud_Rect(cx0, y1 - 0.2f * ch, cx1, y1, Color_Fill(color_white, a * 0.9f));
                break;
            case CELL_AIR:
                Hud_Rect(cx0, y0 + ch * 0.38f, cx1, y0 + ch * 0.62f, Color_Fill(col, a * 0.5f));
                break;
            case CELL_LAND:
            {
                float w = 1.4f * PX * scale;
                Hud_Rect(cx0, y0, cx1, y1, Color_Fill(color_plate, a * 0.6f));
                Hud_Frame(cx0, y0, cx1, y1, w, Color_Fill(col, a));
                Hud_Rect(cx0, y0, cx1, y0 + ch * 0.24f, Color_Fill(col, a));
                break;
            }
            case CELL_LAG:
                Hud_Rect(cx0, y0, cx1, y0 + ch * 0.55f, Color_Fill(col, a * 0.75f));
                break;
            case CELL_TAIL:
                Hud_Rect(cx0, y0, cx1, y1, Color_Fill(color_galint, a));
                break;
            }
            if (r->glyph[k])
                Glyph_Draw(r->glyph[k], (cx0 + cx1) / 2, top + 0.22f * scale + glyph_h / 2, 0.36f * scale, Color_Fill(color_white, 0.95f * dim));
        }
    }

    vis_k = base_k * plate_k;
    // the gate: two posts, lit while a press is due
    float gy0 = base - 0.3f * scale, gy1 = top + 0.3f * scale;
    float gcx = gx + cw / 2, gcy = (base + top) / 2;
    float hot = 0;
    GXColor hot_col = color_white;
    for (int i = 0; i < rows; i++)
    {
        MeterRow *r = &meter[i];
        if (r->hot && !r->dim && r->hot > hot)
        {
            hot = r->hot;
            hot_col = r->color;
        }
        if (r->flash >= 0 && !r->dim)
        {
            hot = 2;
            hot_col = r->color;
        }
    }
    if (hot > 0)
    {
        GXColor fill = Color_Mix(hot_col, color_white, hot >= 2 ? 1.f : 0.4f);
        Hud_Rect(gx - 0.35f * scale, gy0 - 0.2f * scale, gx + cw + 0.35f * scale, gy1 + 0.2f * scale, Color_Fill(hot_col, 0.3f));
        Hud_Rect(gx - 0.1f * scale, gy0, gx + cw + 0.1f * scale, gy1, fill);
        if (hot >= 2)
        {
            // the press frame: a white flash past the plate, and two ticks
            Hud_Rect(gx - 0.2f * scale, gy0 - 0.45f * scale, gx + cw + 0.2f * scale, gy1 + 0.45f * scale, color_white);
            Hud_Rect(gx - 0.75f * scale, gcy - 0.06f * scale, gx - 0.4f * scale, gcy + 0.06f * scale, Color_Fill(color_white, 0.9f)); // short of the labels
            Hud_Rect(gx + cw + 0.4f * scale, gcy - 0.06f * scale, gx + cw + 1.0f * scale, gcy + 0.06f * scale, Color_Fill(color_white, 0.9f));
        }
    }
    else
    {
        GXColor post = Color_Fill(color_white, 0.55f);
        Hud_Rect(gx - 0.16f * scale, gy0, gx - 0.06f * scale, gy1, post);
        Hud_Rect(gx + cw + 0.06f * scale, gy0, gx + cw + 0.16f * scale, gy1, post);
    }

    // endings at the gate
    for (int i = 0; i < rows; i++)
    {
        MeterRow *r = &meter[i];
        if (r->dim || r->gap)
            continue;
        vis_k = base_k * r->fade;
        for (int g = 0; g < 4; g++)
        {
            if (r->ghost[g] < 0)
                continue;
            // a filled copy of the gate's cell, in the row's color, that
            // swells and fades fast
            float q = (float)r->ghost[g] / LL_GHOST, fade = (1.f - q) * (1.f - q);
            int b = r->ghost_bright[g];
            float grow = (b ? 0.1f + 1.2f * Ease_Out(q) : 0.05f + 0.55f * Ease_Out(q)) * scale;
            GXColor tint = Color_Mix(r->color, color_white, b ? 0.35f : 0.2f);
            Hud_Rect(gx - grow, gy0 - grow, gx + cw + grow, gy1 + grow, Color_Over(tint, (b ? 0.55f : 0.22f) * fade));
            Hud_Frame(gx - grow, gy0 - grow, gx + cw + grow, gy1 + grow, 1.6f * PX * scale, Color_Over(tint, (b ? 0.95f : 0.45f) * fade));
        }
        if (r->burst >= 0)
        {
            float q = (float)r->burst / LL_BURST;
            float rad = (0.6f + 2.0f * Ease_Out(q)) * scale;
            GXColor bc = Color_Fill(r->color, 1.f - q);
            for (int ray = 0; ray < 6; ray++)
            {
                float ang = ray * 1.0471976f + 0.5235988f;
                float dx = cos(ang), dy = sin(ang);
                Hud_Seg(gcx + dx * rad * 0.55f, gcy + dy * rad * 0.55f, gcx + dx * rad, gcy + dy * rad, 0.2f * scale, bc);
            }
        }
        if (r->implode >= 0)
        {
            float q = (float)r->implode / LL_FADE;
            GXColor dc = r->implode_tone == TONE_MISS ? color_miss : color_skip;
            // the cell that was due stays in the gate on its own row,
            // fading: whole and gray for a skip, cracked in two for a miss
            // (Stephen, 0.9.8: a skip barely showed)
            float ry0 = base + i * (ch + gap), ry1 = ry0 + ch, rcy = (ry0 + ry1) / 2;
            float a = 0.9f * (1.f - q);
            if (r->implode_tone == TONE_MISS)
            {
                float d = 0.25f * scale * Ease_Out(q), h = ch / 2 - 0.05f * scale;
                Hud_Rect(gx, rcy + 0.05f * scale + d, gx + cw, rcy + 0.05f * scale + h + d, Color_Fill(dc, a));
                Hud_Rect(gx, rcy - 0.05f * scale - h - d, gx + cw, rcy - 0.05f * scale - d, Color_Fill(dc, a));
            }
            else
                Hud_Rect(gx, ry0, gx + cw, ry1, Color_Fill(dc, a * 0.8f));
            float k = 1.f - Ease_Out(q);
            float hw = (cw / 2 + 0.7f * scale) * k, hh = (ch / 2 + 0.4f * scale) * k;
            if (hw > 0.05f)
                Hud_Frame(gcx - hw, rcy - hh, gcx + hw, rcy + hh, 1.4f * PX * scale, Color_Fill(dc, 0.6f * (1.f - q)));
        }
    }

    vis_k = base_k;
    if (!labels)
        return;
    for (int i = 0; i < rows; i++)
    {
        MeterRow *r = &meter[i];
        if (r->gap)
            continue;
        vis_k = base_k * r->fade;
        float y0 = base + i * (ch + gap);
        float ty = Text_MidY(y0 + ch / 2, ST_TEXT);
        GXColor tc = Color_Mix(r->color, color_white, 0.35f);
        if (r->dim)
            tc = Color_Fill(tc, 0.6f);
        tc.a = 255;
        // right-aligned on its row, just short of the gate's ticks (the
        // game aligns it, so it can't run long)
        Hud_TextAligned(r->label, gx - MT_LABEL_GAP * scale, ty, ST_TEXT, tc, 2);
        if (r->info[0])
        {
            float ix = x1 + 0.5f * scale;
            Text_PlateAt(ix - 0.2f, ix + Text_Width(r->info, ST_TEXT) + 0.2f, y0 + ch / 2, (ch + gap) / 2);
            Hud_Text(r->info, ix, ty, ST_TEXT, (GXColor){220, 220, 220, 255});
        }
    }
    vis_k = base_k;
}
