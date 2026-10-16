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
// Every strip keeps a slot for each kind of row, left to right and bottom
// up in the order inputs come: the jump, then the landing (AI, NIL), then
// the waveland or wavedash, and one for the rest (the drop drill, a ledge
// route's alternate) while it's used. A slot stays where it is, empty or
// not, so a hit, miss or skip always lands in the same place (visual audit,
// 2026-10-15: rows, lanes and dials changed order and count between
// moments; lanes never reorder, as in a rhythm game).
enum lane_slot
{
    SLOT_JUMP, // a hop or a jump: any yellow row
    SLOT_AI,
    SLOT_NIL,
    SLOT_WAVE, // a waveland or wavedash
    SLOT_OTHER,
    MT_SLOTS,
};
#define MT_SLOTS_SHOWN 4 // the slots a strip always has room for
// A slot holds two rows (lanes s and s + MT_SLOTS) when two of a kind are
// up at once: a hop and its double jump, or the chosen route and the miss
// still folding where the last one was.
#define MT_LANES (2 * MT_SLOTS)
#define MT_PITCH 0.622f // 35% bigger than the mockup's
#define MT_CW 0.498f
#define MT_CH 0.949f
#define MT_GAP 0.271f
#define MT_WIDE 0.074f   // Cell Size: a fifth of the mockup's cell width a step
#define MT_WIDE_STEPS 3  // Normal, Wide, Wider, Widest (Stephen, 0.9.30: a size, not a toggle)
#define CELL_WIDE (Options_Timers[TOPT_WIDE].val * MT_WIDE)
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
    GLYPH_JUMP_UL, // a double jump with the stick up-left: the jump button's arrow turned
    GLYPH_JUMP_UR,
    GLYPH_JUMP_L,
    GLYPH_JUMP_R,
};
#define GLYPH_HELD 0x40 // or'd in: the stick's drift kept after the jump, a bar under the glyph

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
    u8 held;      // the same plan held a frame on (a hop offered "now" while walking): no re-timing to slide
    float slide;  // rows added to every cell's place while a re-timed plan eases to its true row (0: none)
    float arrive; // the notes' opacity still to come in after a plan swap (0: none)
    char label[8];
    char info[24];
} MeterRow;

static MeterRow meter[MT_LANES]; // built: MT_ROWS at most; settled: a lane each
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

// Lanes keep their places: a row takes the slot of its kind (Lane_Slot)
// and keeps that lane (by its label) while it's up and for MT_LINGER frames
// after, fading out, so the others never shift or swap. One that went with
// a window still ahead leaves that window in gray as it fades, rather than
// just vanishing. Each lane also remembers how far out its window was when
// it came up, for Note Travel.
#define MT_LINGER 8
typedef struct Lane
{
    char label[8];
    int seen;    // the frame it was last up
    int k_prev;  // its first window's cell last frame, -1 none
    int k0;      // ... and when that window came up
    u8 used;
    u8 dim;      // an alternate route's
    u8 parked;   // its row was a preload last frame
    u8 settle;   // ... and the row's settle
    u8 leave_age; // frames since its plan was swapped out (meter_leave), 0 none
    float slide; // rows its notes are drawn off their true place, easing to 0
    float drop;  // ... and how high a swapped-in plan still hangs over its own
    MeterRow last;
} Lane;
static Lane lanes[MT_LANES];
// Each lane's plan that was swapped out, drawn fading as it steps down
// (calloc'd in Event_Init).
static MeterRow *meter_leave;

// A plan re-timed by a few frames slides to its new place; one that moves
// further is a swap: the old plan leaves, fading as it steps down, and the
// new one drops in from up the belt (conveyor measurements, 2026-10-16:
// walking, the notes hopped a row each time the plan's timing shifted a
// frame, and a later plan teleported 19-27 rows up the lane).
#define MT_SLIDE_MAX 3
#define MT_LEAVE 4     // frames a swapped-out plan fades over
#define MT_DROP_MAX 17 // rows a swapped-in plan drops from at most: its first step stays under 8 rows
static int Strip_Track(void); // the fixed strip's rows (meter_placement)

static int Str_Same(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

static int Color_Same(GXColor a, GXColor b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

// The slot of a row's kind. A ledge route's rows ("1st", "2nd", "Route")
// go by their color, AI or NIL.
static int Lane_Slot(MeterRow *r)
{
    const char *l = r->label;
    if (Str_Same(l, "HOP") || Str_Same(l, "JUMP") || Color_Same(r->color, color_in_jump))
        return SLOT_JUMP;
    if (Str_Same(l, "WL") || Str_Same(l, "WD"))
        return SLOT_WAVE;
    if (Str_Same(l, "DRP"))
        return SLOT_OTHER;
    if (Str_Same(l, "AI") || Color_Same(r->color, land_kind_colors[LAND_AI]))
        return SLOT_AI;
    if (Str_Same(l, "NIL") || Color_Same(r->color, land_kind_colors[LAND_NIL]))
        return SLOT_NIL;
    return SLOT_OTHER;
}

// The lane a new row takes: its slot, else (two of a kind up) the slot's
// second row, else the rest's slot. An alternate route skips the second
// row, where it would sit under the chosen one, for the rest's. -1: none.
static int Lane_Free(const int *taken, int slot, int dim)
{
    int want[4] = {slot, dim ? SLOT_OTHER : slot + MT_SLOTS, SLOT_OTHER, SLOT_OTHER + MT_SLOTS};
    for (int i = 0; i < 4; i++)
        if (taken[want[i]] < 0)
            return want[i];
    return -1;
}

// The first cell from k = from on that is a window to press in, a
// touchdown or the first frame to act, or -1.
static int Row_TargetFrom(MeterRow *r, int from)
{
    for (int k = from; k < r->len && k < MT_CELLS; k++)
        if (r->cell[k] == CELL_PRESS || r->cell[k] == CELL_LAND || r->cell[k] == CELL_ACT)
            return k;
    return -1;
}

static int Row_FirstTarget(MeterRow *r)
{
    return Row_TargetFrom(r, 0);
}

// Whether a row plays a hit, miss or skip, or the flash on the first frame
// Falcon can act.
static int Row_Ending(MeterRow *r)
{
    return r->burst >= 0 || r->implode >= 0 || r->flash >= 0;
}

static int Row_Empty(MeterRow *r)
{
    for (int k = 0; k < r->len && k < MT_CELLS; k++)
        if (r->cell[k] != CELL_NONE)
            return 0;
    return 1;
}

// A row with no cells, only an ending (a ledge route's miss, "Route"),
// hands the ending to the live row of its slot and goes, so the fold plays
// in the slot where the note was due and the new route has that slot from
// its first frame (smoke 0.9.30: after a miss the chosen route took the NIL
// slot's second lane until the next drop). Returns the rows left.
static int Meter_HandEndings(MeterRow *built, int n)
{
    for (int i = 0; i < n; i++)
    {
        MeterRow *e = &built[i];
        if (!Row_Empty(e) || !(Row_Ending(e) || e->ghost[0] >= 0))
            continue;
        for (int j = 0; j < n; j++)
        {
            MeterRow *o = &built[j];
            if (j == i || o->dim || Row_Empty(o) || Row_Ending(o) || Lane_Slot(o) != Lane_Slot(e))
                continue;
            o->implode = e->implode;
            o->implode_tone = e->implode_tone;
            o->burst = e->burst;
            o->flash = e->flash;
            for (int g = 0; g < 4; g++)
                if (e->ghost[g] >= 0)
                    Row_Ghost(o, e->ghost[g], e->ghost_bright[g]);
            for (int m = i; m + 1 < n; m++)
                built[m] = built[m + 1];
            n--;
            i--;
            break;
        }
    }
    return n;
}

// A lane's plan this frame against the last: a re-timing of a few frames
// slides (the note keeps last frame's place a step on, then eases to its
// true row a third of the way a frame); more is a swap. The slide and drop
// are scaled to nothing by the row over the window, so the window and the
// hit, miss and skip in it never move.
static void Lane_Track(Lane *L, int l, MeterRow *r, int t)
{
    L->slide -= L->slide / 3;
    if (L->slide > -0.12f && L->slide < 0.12f)
        L->slide = 0;
    L->drop *= 0.6f;
    if (L->drop < 0.3f)
        L->drop = 0;
    if (L->leave_age)
        L->leave_age = L->leave_age < MT_LEAVE ? L->leave_age + 1 : 0;
    // where this frame's first target would be had nothing changed: last
    // frame's a step on, or its next one past the window
    int from = -1;
    if (L->k_prev > 0)
        from = L->k_prev - 1;
    else if (L->k_prev == 0)
    {
        int w = Row_TargetFrom(&L->last, 1);
        from = w > 0 ? w - 1 : -1;
    }
    int d = 0;
    // (a target held in the window, as the hop offered "now" while walking,
    // is no re-timing: the rest of the row keeps its rows. Neither is a plan
    // held a frame on, whose notes hold still: 0.9.32 slid them 3 rows off.
    // A preload set down or laid out again is judged too, but only a swap:
    // within a few rows it eases on by itself (settle); 0.9.32 jumped up
    // to 24 rows from standing to walking)
    int park_change = L->parked != r->parked;
    if (t >= 0 && L->k_prev >= 0 && !(t == 0 && L->k_prev == 0) && !r->held && !Row_Ending(r) &&
        (park_change || (!L->parked && !r->parked)))
        d = t - from;
    if (park_change && d >= -MT_SLIDE_MAX && d <= MT_SLIDE_MAX)
        d = 0;
    if (d != 0 && from >= 0 && d >= -MT_SLIDE_MAX && d <= MT_SLIDE_MAX)
    {
        L->slide -= d;
        if (L->slide < -MT_SLIDE_MAX)
            L->slide = -MT_SLIDE_MAX;
        if (L->slide > MT_SLIDE_MAX)
            L->slide = MT_SLIDE_MAX;
    }
    else if ((d > MT_SLIDE_MAX || d < -MT_SLIDE_MAX) && t > 0 && meter_leave)
    {
        meter_leave[l] = L->last;
        L->leave_age = 1;
        L->slide = 0;
        int track = Strip_Track(), drop = track - 1 - t; // (its first target on the belt)
        if (drop > MT_DROP_MAX)
            drop = MT_DROP_MAX;
        if (drop > 3 * (t - 1))
            drop = 3 * (t - 1); // a near one drops from nearer, so it's down by the window
        L->drop = t >= 4 && t < track ? drop : 0;
        if (cue_log)
            OSReport("LLLANE %d %s lane %d swap %d -> %d\n", event_vars->game_timer, L->label, l, from, t);
    }
    else if (t < 0 && L->k_prev > 0 && !L->parked && !r->parked && !Row_Ending(r) && meter_leave)
    {
        // its plan went out past the belt's end (or away) before reaching
        // the window: the notes fade out as a swap's do (0.9.33: the AI
        // pair vanished in a frame when walking under the top platform)
        meter_leave[l] = L->last;
        L->leave_age = 1;
        L->slide = 0;
        if (cue_log)
            OSReport("LLLANE %d %s lane %d swap %d -> none\n", event_vars->game_timer, L->label, l, L->k_prev);
    }
    float k = Clamp01((t - 1) / 4.f);
    r->slide = (L->slide + L->drop) * k;
    r->arrive = L->leave_age ? 1.f - Clamp01(L->leave_age / 3.f) : 0;
}

// The nearest whole row to v.
static int Row_Round(float v)
{
    return v >= 0 ? (int)(v + 0.5f) : -(int)(0.5f - v);
}

// The plan lane l swapped out, as it shows age frames on (1 to MT_LEAVE),
// or 0.
static MeterRow *Lane_Leave(MeterRow *r, int *age)
{
    int l = r - meter;
    if (!meter_leave || l < 0 || l >= MT_LANES || !lanes[l].used || !lanes[l].leave_age)
        return 0;
    *age = lanes[l].leave_age;
    return &meter_leave[l];
}

static void Meter_Settle(void)
{
    static MeterRow built[MT_ROWS];
    int now = event_vars->game_timer, n = meter_rows;
    int taken[MT_LANES], at[MT_ROWS];
    memcpy(built, meter, n * sizeof(MeterRow));
    n = Meter_HandEndings(built, n);
    for (int l = 0; l < MT_LANES; l++)
    {
        taken[l] = -1;
        if (lanes[l].used && (now < lanes[l].seen || now - lanes[l].seen > MT_LINGER))
        {
            lanes[l].used = 0; // gone long enough, or the clock went back (a save state)
            if (cue_log)
                OSReport("LLLANE %d %s lane %d gone\n", now, lanes[l].label, l);
        }
    }
    for (int i = 0; i < n; i++)
        at[i] = -1;
    // the cues and the chosen route first, then the alternates, so the
    // chosen route has its kind's slot from the first hang frame whatever
    // the alternates had. Each keeps the lane it had (the same label, in
    // its own slot or the rest's), then the others take a free one
    for (int dim = 0; dim < 2; dim++)
    {
        for (int i = 0; i < n; i++)
        {
            if (!built[i].dim != !dim)
                continue;
            int s = Lane_Slot(&built[i]);
            for (int l = 0; l < MT_LANES && at[i] < 0; l++)
                if (lanes[l].used && taken[l] < 0 && !lanes[l].dim == !dim && Str_Same(lanes[l].label, built[i].label) &&
                    (l % MT_SLOTS == s || l % MT_SLOTS == SLOT_OTHER))
                    at[i] = l;
            if (at[i] >= 0)
                taken[at[i]] = i;
        }
        for (int i = 0; i < n; i++)
        {
            if (!built[i].dim != !dim || at[i] >= 0)
                continue;
            int best = Lane_Free(taken, Lane_Slot(&built[i]), dim);
            if (best < 0)
                continue;
            Lane *L = &lanes[best];
            memset(L, 0, sizeof(*L));
            strcpy(L->label, built[i].label);
            L->dim = dim;
            L->k_prev = -1;
            if (cue_log)
                OSReport("LLLANE %d %s lane %d new\n", now, L->label, best);
            at[i] = best;
            taken[best] = i;
        }
    }
    for (int i = 0; i < n; i++)
    {
        if (at[i] < 0)
            continue;
        Lane *L = &lanes[at[i]];
        int t = Row_FirstTarget(&built[i]);
        Lane_Track(L, at[i], &built[i], t);
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
    for (int l = 0; l < MT_LANES; l++)
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
        L->leave_age = 0;
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

// The rows a lane strip (cells, highway, dial) draws in slot s: its first
// and its second, unless the second is an alternate route that would sit
// under the chosen one (the conveyor's belt shares every row). Returns how
// many.
static int Slot_Rows(int s, MeterRow **out)
{
    int n = 0;
    for (int l = s; l < meter_rows; l += MT_SLOTS)
    {
        MeterRow *r = &meter[l];
        if (r->gap || (n > 0 && r->dim))
            continue;
        out[n++] = r;
    }
    return n;
}

// The slots up to the last one with a row.
static int Slot_Count(void)
{
    int top = 0;
    for (int l = 0; l < meter_rows; l++)
        if (!meter[l].gap && l % MT_SLOTS + 1 > top)
            top = l % MT_SLOTS + 1;
    return top;
}

// The slots a fixed strip draws: the four always, the rest's while it's used.
static int Slot_Shown(void)
{
    int n = Slot_Count();
    return n > MT_SLOTS_SHOWN ? n : MT_SLOTS_SHOWN;
}

static const char *slot_labels[MT_SLOTS] = {"JUMP", "AI", "NIL", "WAVE", ""};

static GXColor Slot_Color(int s)
{
    switch (s)
    {
    case SLOT_JUMP:
        return color_in_jump;
    case SLOT_AI:
        return land_kind_colors[LAND_AI];
    case SLOT_NIL:
        return land_kind_colors[LAND_NIL];
    case SLOT_WAVE:
        return land_kind_colors[LAND_PERFECT_WL];
    }
    return color_galint;
}

// Whether row o has a note at cell k: a second row in the same slot leaves
// its frames in the air out there, under the note.
static int Row_NoteAt(MeterRow *o, int k)
{
    return o && k < o->len && k < MT_CELLS && o->cell[k] != CELL_NONE && o->cell[k] != CELL_AIR;
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
    int held = glyph & GLYPH_HELD;
    glyph &= ~GLYPH_HELD;
    for (int pass = 0; pass < 2; pass++)
    {
        if (held)
        {
            // the drift held after the jump: a bar under the button (Fable's
            // Conveyor feel plan: drift should say so)
            float hw = 0.8f * s + (pass ? 0 : 0.12f), by = y - s * 1.2f, bh = 0.14f * s + (pass ? 0 : 0.12f);
            Hud_Rect(x - hw, by - bh, x + hw, by + bh, pass ? c : shade);
        }
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
        case GLYPH_JUMP_UL:
        case GLYPH_JUMP_UR:
        case GLYPH_JUMP_L:
        case GLYPH_JUMP_R:
        {
            // a button with an arrow cut into it, pointing as the stick
            float ux = glyph == GLYPH_JUMP_UL ? -0.707f : glyph == GLYPH_JUMP_UR ? 0.707f : glyph == GLYPH_JUMP_L ? -1.f
                       : glyph == GLYPH_JUMP_R ? 1.f : 0;
            float uy = glyph == GLYPH_JUMP_L || glyph == GLYPH_JUMP_R ? 0 : glyph == GLYPH_JUMP ? 1.f : 0.707f;
            Hud_Disc(x, y, r, col);
            if (pass)
            {
                float bx = x - ux * s * 0.3f, by = y - uy * s * 0.3f; // the base, then the tip
                Hud_Tri(bx + uy * s * 0.55f, by - ux * s * 0.55f, bx - uy * s * 0.55f, by + ux * s * 0.55f, x + ux * s * 0.5f,
                        y + uy * s * 0.5f, shade);
            }
            break;
        }
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

// One cell of the meter's rows, cx0 to cx1 and y0 to y1 (ch tall).
static void Mt_Cell(int kind, GXColor col, float a, float cx0, float cx1, float y0, float y1, float ch, float scale)
{
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
}

// Whether cell k of a row is a window's later frame: the tail behind its
// head (the first frame), drawn fainter on every strip, so the note's
// first frame is what reads as due (conveyor measurements, 2026-10-16: the
// window's 2-4 frame body was read as the note, a frame or two early).
static int Row_WindowTail(MeterRow *r, int k)
{
    return k > 0 && k < MT_CELLS && r->cell[k] == CELL_PRESS && r->cell[k - 1] == CELL_PRESS && r->tone[k - 1] == r->tone[k];
}

static void Slot_Swatch(int s, MeterRow *r, float cx, float cy, float w, float h, float k); // meter_placement

// The meter with its gate's left edge at gx and its bottom at base. scale
// is 1 above Falcon and MT_FIXED for the fixed strip, which also gets
// labels. A row a slot, bottom up (Meter_Settle); the fixed strip always
// has the four, an empty one with its label dimmed and no cells.
static void Meter_Draw(float gx, float base, float scale, int max_cells, int labels)
{
    float pitch = (MT_PITCH + CELL_WIDE) * scale;
    float cw = (MT_CW + CELL_WIDE) * scale;
    float ch = MT_CH * scale, gap = MT_GAP * scale;
    int rows = labels ? Slot_Shown() : Slot_Count();

    int len = 1, glyphs = 0;
    for (int i = 0; i < meter_rows; i++)
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
    for (int i = 0; i < meter_rows; i++)
        if (!meter[i].gap && meter[i].fade > plate_k)
            plate_k = meter[i].fade;
    vis_k = base_k * plate_k;
    // (with labels, it reaches over their column too: one panel, no plate
    // per label to overlap the next row's)
    float plate_x0 = labels ? gx - MT_LABEL_GAP * scale - MT_LABEL_W - 0.25f : gx - 0.32f * scale;
    // (deeper under the rows than over them: the time ticks go there)
    float plate_y0 = base - 0.5f * scale, plate_y1 = top + 0.22f * scale; // the rows' part, under the glyphs
    Hud_Rect(plate_x0, plate_y0, x1 + 0.22f * scale, plate_y1 + glyph_h, Color_Fill(color_plate, 0.72f));

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

    // each slot's rows, from the bottom up. The fixed strip's empty frames
    // are brighter: the dark gray ones vanished on the dark stage underside
    // (visual audit, 2026-10-15)
    float empty_a = labels ? 0.14f : 0.1f, five_a = labels ? 0.22f : 0.16f;
    for (int s = 0; s < rows; s++)
    {
        MeterRow *sr[2];
        int m = Slot_Rows(s, sr);
        float y0 = base + s * (ch + gap), y1 = y0 + ch;
        for (int j = 0; j < m; j++)
        {
            // (two rows in a slot, a hop and its double jump: one's frames
            // in the air don't cross the other's notes)
            MeterRow *r = sr[j], *o = m > 1 ? sr[1 - j] : 0;
            vis_k = base_k * r->fade;
            float dim = r->dim ? 0.5f : 1.f;

            // empty frames, once for the slot
            for (int k = 1; k < len && j == 0; k++)
            {
                if (r->cell[k] == CELL_NONE && !(o && k < o->len && o->cell[k] != CELL_NONE))
                    Hud_Rect(gx + k * pitch, y0, gx + k * pitch + cw, y1, (now + k) % 5 == 0 ? Color_Over(color_white, five_a * dim)
                                                                                        : Color_Fill(color_white, empty_a * dim));
            }

            // a plan swapped out: its cells step on toward the gate,
            // fading, but never into it, and with no glyphs
            int la;
            MeterRow *lv = Lane_Leave(r, &la);
            for (int k = 0; lv && k < lv->len && k < MT_CELLS; k++)
            {
                float at = Note_At(lv, k, len) + lv->slide - la;
                if (lv->cell[k] == CELL_NONE || lv->cell[k] == CELL_AIR || at < 0.99f || at > len - 0.5f)
                    continue;
                float a = lv->alpha[k] / 255.f * (lv->dim ? 0.5f : 1.f) * (1.f - la / 5.f) * (Row_WindowTail(lv, k) ? 0.5f : 1.f);
                Mt_Cell(lv->cell[k], Tone_Color(lv, lv->tone[k]), a, gx + at * pitch, gx + at * pitch + cw, y0, y1, ch, scale);
            }

            float in_a = 1.f - r->arrive;
            for (int k = 0; k < r->len && k < len; k++)
            {
                int kind = r->cell[k];
                if (kind == CELL_NONE || (kind == CELL_AIR && Row_NoteAt(o, k)))
                    continue;
                float a = r->alpha[k] / 255.f * dim * in_a;
                if (Row_WindowTail(r, k))
                    a *= 0.5f;
                GXColor col = Tone_Color(r, r->tone[k]);
                float at = Note_At(r, k, len) + r->slide;
                if (at > len - 0.5f || at < -0.01f)
                    continue; // still off the far end, or slid past the gate (frames before its first target)
                float cx0 = gx + at * pitch, cx1 = cx0 + cw;
                Mt_Cell(kind, col, a, cx0, cx1, y0, y1, ch, scale);
                if (r->glyph[k])
                    Glyph_Draw(r->glyph[k], (cx0 + cx1) / 2, top + 0.22f * scale + glyph_h / 2, 0.36f * scale, Color_Fill(color_white, 0.95f * dim * in_a));
            }
        }
    }

    vis_k = base_k * plate_k;
    // the gate: two posts, lit while a press is due
    float gy0 = base - 0.3f * scale, gy1 = top + 0.3f * scale;
    float gcx = gx + cw / 2, gcy = (base + top) / 2;
    float hot = 0;
    GXColor hot_col = color_white;
    for (int i = 0; i < meter_rows; i++)
    {
        MeterRow *r = &meter[i];
        if (r->gap)
            continue;
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
        // the fixed strip's light and flash stay on the rows' plate (visual
        // audit, 2026-10-15: past it, the press frame was a tall white bar)
        float ly0 = gy0 - 0.2f * scale, ly1 = gy1 + 0.2f * scale, fy0 = gy0 - 0.45f * scale, fy1 = gy1 + 0.45f * scale;
        if (labels)
        {
            ly0 = fy0 = plate_y0;
            ly1 = fy1 = plate_y1;
        }
        GXColor fill = Color_Mix(hot_col, color_white, hot >= 2 ? 1.f : 0.4f);
        Hud_Rect(gx - 0.35f * scale, ly0, gx + cw + 0.35f * scale, ly1, Color_Fill(hot_col, 0.3f));
        Hud_Rect(gx - 0.1f * scale, gy0, gx + cw + 0.1f * scale, gy1, fill);
        if (hot >= 2)
        {
            // the press frame: a white flash past the plate, and two ticks
            Hud_Rect(gx - 0.2f * scale, fy0, gx + cw + 0.2f * scale, fy1, color_white);
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

    // endings at the gate, each on its slot's row
    for (int s = 0; s < rows; s++)
    {
        MeterRow *sr[2];
        int m = Slot_Rows(s, sr);
        for (int j = 0; j < m; j++)
        {
            MeterRow *r = sr[j];
            if (r->dim)
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
                // the fixed strip's swells up and down only as far as its
                // plate (0.9.30 stills: a double jump's ran off the screen's
                // bottom edge)
                float lo = gy0 - grow, hi = gy1 + grow;
                if (labels && lo < plate_y0)
                    lo = plate_y0;
                if (labels && hi > plate_y1 + glyph_h)
                    hi = plate_y1 + glyph_h;
                Hud_Rect(gx - grow, lo, gx + cw + grow, hi, Color_Over(tint, (b ? 0.55f : 0.22f) * fade));
                Hud_Frame(gx - grow, lo, gx + cw + grow, hi, 1.6f * PX * scale, Color_Over(tint, (b ? 0.95f : 0.45f) * fade));
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
                float ry0 = base + s * (ch + gap), ry1 = ry0 + ch, rcy = (ry0 + ry1) / 2;
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
    }

    vis_k = base_k;
    if (!labels)
        return;
    // the labels, right-aligned on their rows, just short of the gate's
    // ticks (the game aligns them, so they can't run long). No numbers past
    // the cells: changing every frame they can't be read at speed, and they
    // ran into the stock icon and "0%" (Stephen, 10-03; visual audit,
    // 2026-10-15). The panel says them. Always the slot's own word, never
    // the row's (WD, HOP, 1st, Route), so a row's label never changes
    // under it; the rest's slot has its row's swatch (0.9.30 stills: the
    // labels differed in all six moments)
    for (int s = 0; s < rows; s++)
    {
        MeterRow *sr[2];
        int m = Slot_Rows(s, sr);
        float y0 = base + s * (ch + gap);
        float ty = Text_MidY(y0 + ch / 2, ST_TEXT);
        float lx = gx - MT_LABEL_GAP * scale;
        GXColor tc;
        if (s >= MT_SLOTS_SHOWN)
        {
            if (m == 0)
                continue; // (shown only while it has a row)
            vis_k = base_k * sr[0]->fade;
            Slot_Swatch(s, sr[0], lx - 0.5f, y0 + ch / 2, 0.9f, 0.5f, 1.f);
            continue;
        }
        if (m == 0)
        {
            // an empty slot: its kind's name, dimmed
            vis_k = base_k * plate_k;
            tc = Color_Fill(Color_Mix(Slot_Color(s), color_white, 0.35f), 0.5f);
            tc.a = 255;
            Hud_TextAligned(slot_labels[s], lx, ty, ST_TEXT, tc, 2);
            continue;
        }
        MeterRow *r = sr[0];
        vis_k = base_k * r->fade;
        tc = Color_Mix(r->color, color_white, 0.35f);
        if (r->dim)
            tc = Color_Fill(tc, 0.6f);
        tc.a = 255;
        Hud_TextAligned(slot_labels[s], lx, ty, ST_TEXT, tc, 2);
    }
    vis_k = base_k;
}
