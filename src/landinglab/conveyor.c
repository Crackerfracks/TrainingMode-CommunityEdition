/* landinglab/conveyor.c
 *
 * Holds the Conveyor strip.
 *
 * Part of Landing Gear: src/landinglab.c includes it, in order, into one
 * translation unit, so it sees everything included before it (and mex.h,
 * events.h, landinglab_sim.h).
 *
 * Uses: drawing, hud_drawing, meter, meter_placement, highway.
 */

// The conveyor (Fixed Strip): every cue's notes on one belt of a fixed
// width that moves a row a frame (Stephen, 0.9.10: the highway's lanes
// stacked too wide, and the background should show the game's time
// passing, like a fretboard). Notes on the same frame share its row, in a
// fixed order by kind; a lone note takes the whole row. Kinds differ by
// shape as well as color: a jump has a raised cap, an AI is solid, a NIL
// notched, a waveland or wavedash hollow. A preload (a
// plan from a press not made yet) hovers over the rows it will take, its
// shadow on the belt, and sets down when the press comes. What's beyond the
// belt's far end waits over its entry, lower the nearer it is.
#define CV_W (2 * HW_LANE + HW_LANE_GAP)
#define CV_SHELF 1.3f
#define CV_LIFT 0.4f // a preload's hover: most of a frame (0.22 didn't read as lifted)
#define CV_BASE 0.8f // under the window
#define CV_KEY_PITCH 1.2f // the key's rows (0.8 overlapped)

static int Cv_Rank(MeterRow *r)
{
    const char *l = r->label;
    if (Str_Same(l, "HOP") || Str_Same(l, "JUMP"))
        return 0;
    if (Str_Same(l, "AI"))
        return 1;
    if (Str_Same(l, "NIL"))
        return 2;
    if (Str_Same(l, "WL") || Str_Same(l, "WD"))
        return 3;
    return 4;
}

// Whether a cell takes a share of its row: notes do, the frames in the air
// and the landing lag behind them don't.
static int Cv_Claims(int kind)
{
    return kind == CELL_PRESS || kind == CELL_ACT || kind == CELL_LAND || kind == CELL_TAIL;
}

// The rows to draw, in the belt's order (kind, then lane).
static int Cv_Order(int *order)
{
    int n = 0;
    for (int i = 0; i < meter_rows; i++)
    {
        if (meter[i].gap)
            continue;
        int j = n++;
        while (j > 0 && Cv_Rank(&meter[order[j - 1]]) > Cv_Rank(&meter[i]))
        {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = i;
    }
    return n;
}

// How high a row's notes ride over the belt: a preload hovers, bobbing,
// and one that set down eases onto it over three frames.
static float Cv_Lift(MeterRow *r)
{
    if (r->parked)
        return CV_LIFT + 0.07f * sin(event_vars->game_timer * 0.12f);
    if (r->settle && r->settle <= 3)
        return CV_LIFT * (1.f - r->settle / 3.f);
    return 0;
}

// A rect with its corners cut a pixel, so notes read rounded. The three
// strips don't overlap, so a faint note has no brighter cross.
static void Cv_Round(float x0, float y0, float x1, float y1, GXColor c)
{
    Hud_Rect(x0 + PX, y0, x1 - PX, y1, c);
    Hud_Rect(x0, y0 + PX, x0 + PX, y1 - PX, c);
    Hud_Rect(x1 - PX, y0 + PX, x1, y1 - PX, c);
}

// A note's body: a rim a pixel wide, lighter than its fill, round the
// fill, with its corners cut. No strips overlap, so a faint note has no
// brighter seams. An open end (the note goes on past it) has no rim.
static void Cv_Body(float x0, float y0, float x1, float y1, GXColor col, float a, int open_lo, int open_hi)
{
    GXColor rim = Color_Mix(col, color_white, 0.45f);
    rim.a = col.a;
    float ilo = open_lo ? y0 : y0 + PX, ihi = open_hi ? y1 : y1 - PX;
    Hud_Rect(x0 + PX, ilo, x1 - PX, ihi, Color_Fill(col, a));
    Hud_Rect(x0, ilo, x0 + PX, ihi, Color_Fill(rim, a));
    Hud_Rect(x1 - PX, ilo, x1, ihi, Color_Fill(rim, a));
    if (!open_lo)
        Hud_Rect(x0 + PX, y0, x1 - PX, y0 + PX, Color_Fill(rim, a));
    if (!open_hi)
        Hud_Rect(x0 + PX, y1 - PX, x1 - PX, y1, Color_Fill(rim, a));
}

// One note: kind and tone of row r, from y0 up to y1 (open_lo / open_hi:
// the note goes on past that end, where the share of the row changed).
// Kinds differ in shape as well as color, all square-edged (the GameCube
// draws no smooth diagonals): a jump has a tab on its top, an AI is solid,
// a NIL is notched, a waveland or wavedash hollow. Each has a rim lighter
// than its fill (Stephen, 0.9.12: the notes looked rough round the edges;
// a dark rim was lost on the dark belt).
static void Cv_Note(MeterRow *r, int kind, int tone, float a, float x0, float y0, float x1, float y1, int open_lo,
                    int open_hi)
{
    GXColor col = Tone_Color(r, tone);
    int rank = tone == TONE_JUMP ? 0 : Cv_Rank(r);
    float lo = open_lo ? y0 : y0 + 0.05f, hi = open_hi ? y1 : y1 - 0.05f, m = (x0 + x1) / 2;
    if (kind == CELL_LAG)
    {
        Cv_Round(m - 0.4f, lo, m + 0.4f, hi, Color_Fill(col, a * 0.6f));
        return;
    }
    switch (kind)
    {
    case CELL_PRESS:
        if (rank == 3 && tone != TONE_MISS && tone != TONE_SKIP)
        {
            // hollow: a thick rim round a faint core
            float t = 2 * PX;
            Hud_Rect(x0, lo, x1, lo + t, Color_Fill(col, a));
            Hud_Rect(x0, hi - t, x1, hi, Color_Fill(col, a));
            Hud_Rect(x0, lo + t, x0 + t, hi - t, Color_Fill(col, a));
            Hud_Rect(x1 - t, lo + t, x1, hi - t, Color_Fill(col, a));
            Hud_Rect(x0 + t, lo + t, x1 - t, hi - t, Color_Fill(col, a * 0.4f));
            return;
        }
        Cv_Body(x0, lo, x1, hi, col, a, open_lo, open_hi);
        if (rank == 0 && !open_hi)
            Cv_Round(m - (x1 - x0) * 0.18f, hi - PX, m + (x1 - x0) * 0.18f, hi + 0.16f, Color_Fill(col, a));
        if (rank == 2 && !open_hi)
            Hud_Rect(m - 0.13f, hi - 0.24f, m + 0.13f, hi, Color_Fill(color_plate, a * 0.9f));
        break;
    case CELL_ACT:
        Cv_Body(x0, lo, x1, hi, col, a, open_lo, open_hi);
        Hud_Rect(x0 + PX, lo, x1 - PX, lo + 0.2f * CV_PITCH, Color_Fill(color_white, a * 0.9f));
        break;
    case CELL_LAND:
        Cv_Round(x0, lo, x1, hi, Color_Fill(color_plate, a * 0.6f));
        Hud_Frame(x0, lo, x1, hi, 1.4f * PX, Color_Fill(col, a));
        Hud_Rect(x0, lo, x1, lo + 0.24f * CV_PITCH, Color_Fill(col, a));
        return;
    case CELL_TAIL:
        Cv_Body(x0, lo, x1, hi, color_galint, a, open_lo, open_hi);
        break;
    }
}

// The first note a row has at or past frame from, or -1.
static int Cv_FirstClaim(MeterRow *r, int from)
{
    for (int k = from; k < r->len && k < MT_CELLS; k++)
        if (Cv_Claims(r->cell[k]))
            return k;
    return -1;
}

static void Conveyor_Draw(FighterData *fp)
{
    float x0, y0, w, h;
    Strip_Place(fp, STRIP_CONVEYOR, &x0, &y0, &w, &h);
    int right = x0 > 0;
    float glyph = Strip_Glyphs() ? HW_GLYPH : 0;
    float glyph_x = right ? x0 + STRIP_PAD + glyph / 2 : x0 + w - STRIP_PAD - glyph / 2;
    // the key on the side toward the screen's edge, the glyphs toward its middle
    float bl = x0 + STRIP_PAD + (right ? glyph : CV_KEY), br = bl + CV_W;
    float kx = right ? br + 0.5f : x0 + STRIP_PAD;
    float line_y = y0 + CV_BASE, top = line_y + CV_ROWS * CV_PITCH;
    hw_pitch = CV_PITCH; // the window's frame is a row of the belt
    float base_k = vis_k;
    int now = event_vars->game_timer;
    int order[MT_ROWS], n = Cv_Order(order);

    // the plate and belt stay up, solid, while the strip is on, so the
    // belt's motion is always there to read the notes' speed against
    // (0.9.12's faded with the cues, and the stage showed through it)
    Hud_Rect(x0, y0, x0 + w, y0 + h, Color_Over(color_plate, 0.95f));
    Hud_Rect(bl, line_y, br, top, Color_Over(color_white, 0.07f));
    // the belt moves: a line between every two frames, a heavier one every
    // fifth frame of the game's clock, with studs on the rails, so they
    // travel down a row each frame
    for (int k = 1; k <= CV_ROWS; k++)
    {
        float y = line_y + k * CV_PITCH;
        if ((now + k) % 5 == 0)
        {
            Hud_Rect(bl, y - 0.09f, br, y + 0.09f, Color_Over(color_white, 0.42f));
            if (k < CV_ROWS)
            {
                Hud_Rect(bl - 0.42f, y - 0.14f, bl - 0.16f, y + 0.14f, Color_Over(color_white, 0.5f));
                Hud_Rect(br + 0.16f, y - 0.14f, br + 0.42f, y + 0.14f, Color_Over(color_white, 0.5f));
            }
        }
        else
            Hud_Rect(bl, y - PX / 2, br, y + PX / 2, Color_Over(color_white, 0.17f));
    }
    Hud_Rect(bl - 0.12f, line_y, bl - 0.06f, top, Color_Over(color_white, 0.2f));
    Hud_Rect(br + 0.06f, line_y, br + 0.12f, top, Color_Over(color_white, 0.2f));
    Hud_Rect(bl - 0.2f, top, br + 0.2f, top + 0.08f, Color_Over(color_white, 0.35f)); // the entry's lip

    // the notes: each row's runs of frames that keep their share of the
    // belt, drawn whole, so a window reads as one note
    int slot[MT_ROWS][CV_ROWS], cnt[CV_ROWS];
    for (int k = 0; k < CV_ROWS; k++)
    {
        cnt[k] = 0;
        for (int j = 0; j < n; j++)
        {
            MeterRow *r = &meter[order[j]];
            slot[j][k] = k < r->len && Cv_Claims(r->cell[k]) ? cnt[k]++ : -1;
        }
    }
    for (int j = 0; j < n; j++)
    {
        MeterRow *r = &meter[order[j]];
        float lift = Cv_Lift(r);
        for (int k = 0; k < CV_ROWS && k < r->len; k++)
        {
            // what's drawn here: a note in its share, or lag across the
            // belt where no note is
            int kind = r->cell[k];
            if (slot[j][k] < 0 && !(kind == CELL_LAG && cnt[k] == 0))
                continue;
            int e = k;
            while (e + 1 < CV_ROWS && e + 1 < r->len && r->cell[e + 1] == kind && r->tone[e + 1] == r->tone[k] &&
                   r->alpha[e + 1] == r->alpha[k] && slot[j][e + 1] == slot[j][k] &&
                   (slot[j][k] < 0 ? cnt[e + 1] == 0 : cnt[e + 1] == cnt[k]))
                e++;
            float sx0 = bl + 0.15f, sx1 = br - 0.15f;
            if (slot[j][k] >= 0)
            {
                float share = (CV_W - 0.3f) / cnt[k];
                sx0 = bl + 0.15f + slot[j][k] * share + (cnt[k] > 1 ? 0.06f : 0);
                sx1 = sx0 + share - (cnt[k] > 1 ? 0.12f : 0);
            }
            float y0 = line_y + k * CV_PITCH, y1 = line_y + (e + 1) * CV_PITCH;
            int lo = k > 0 && r->cell[k - 1] == kind && r->tone[k - 1] == r->tone[k];
            int hi = e + 1 < r->len && r->cell[e + 1] == kind && r->tone[e + 1] == r->tone[k];
            vis_k = base_k * r->fade;
            // where it will set down: a light outline on the belt under it
            // (a dark shadow was lost on the dark belt, 0.9.14 test)
            // (0.9.15 test: right under the note it was hidden, so the note
            // also leans left off it while it hovers)
            float lean = -0.4f * lift;
            if (lift > 0 && kind != CELL_LAG)
                Hud_Frame(sx0, y0 + 0.05f, sx1, y1 - 0.05f, 1.5f * PX, Color_Over(color_white, 0.5f * (1.f - r->settle / 4.f)));
            float a = r->alpha[k] / 255.f * (r->dim ? 0.5f : 1.f);
            Cv_Note(r, kind, r->tone[k], a, sx0 + lean, y0 + lift, sx1 + lean, y1 + lift, lo, hi);
            if (r->settle == 1 && kind != CELL_LAG)
                Hud_Frame(sx0 - PX, y0 - PX, sx1 + PX, y1 + PX, 1.4f * PX, Color_Fill(color_white, 0.8f)); // set down
            k = e;
        }
    }

    // over the entry: each row's next note past the far end, waiting
    int wait[MT_ROWS], wk[MT_ROWS], q = 0;
    for (int j = 0; j < n; j++)
    {
        MeterRow *r = &meter[order[j]];
        int k = Cv_FirstClaim(r, CV_ROWS);
        if (k >= 0 && Cv_FirstClaim(r, 0) == k)
        {
            wait[q] = order[j];
            wk[q++] = k;
        }
    }
    for (int j = 0; j < q; j++)
    {
        MeterRow *r = &meter[wait[j]];
        vis_k = base_k * r->fade * 0.8f;
        float share = (CV_W - 0.3f) / q;
        float sx0 = bl + 0.15f + j * share + 0.1f, sx1 = sx0 + share - 0.2f;
        int far = wk[j] - CV_ROWS;
        float wy = top + 0.3f + (far < 20 ? far : 20) * 0.035f + 0.05f * sin(now * 0.12f + j);
        Cv_Note(r, r->cell[wk[j]], r->tone[wk[j]], r->alpha[wk[j]] / 255.f, sx0, wy, sx1, wy + 0.55f, 0, 0);
    }

    // the input glyphs beside the belt
    for (int j = 0; j < n; j++)
    {
        MeterRow *r = &meter[order[j]];
        vis_k = base_k * r->fade;
        float lift = Cv_Lift(r);
        for (int k = 0; k < r->len && k < CV_ROWS; k++)
            if (r->glyph[k])
                Glyph_Draw(r->glyph[k], glyph_x, line_y + (k + 0.5f) * CV_PITCH + lift, 0.3f,
                           Color_Fill(color_white, 0.95f * (r->dim ? 0.5f : 1.f)));
    }

    // the window: one across the belt, lit by the hottest row due, each
    // row's ending in its share
    int at[MT_ROWS], e = 0, hot = -1;
    for (int j = 0; j < n; j++)
    {
        MeterRow *r = &meter[order[j]];
        if ((r->len > 0 && Cv_Claims(r->cell[0]) && !r->parked) || (!r->dim && (r->burst >= 0 || r->implode >= 0)))
            at[e++] = order[j];
        int hj = r->dim || r->parked ? 0 : r->flash >= 0 ? 2 : r->hot;
        if (hj > 0 && (hot < 0 || hj > (meter[hot].flash >= 0 ? 2 : meter[hot].hot)))
            hot = order[j];
    }
    for (int j = 0; j < e; j++)
    {
        MeterRow *r = &meter[at[j]];
        vis_k = base_k * r->fade;
        float share = CV_W / e;
        Hw_WindowEnd(r, bl + j * share, bl + (j + 1) * share, line_y);
    }
    vis_k = base_k;
    MeterRow frame = {0};
    if (hot >= 0)
        frame = meter[hot];
    else
    {
        frame.color = color_white;
        frame.implode = -1;
        frame.flash = -1;
        if (e > 0)
            frame = meter[at[0]], frame.hot = 0, frame.flash = -1;
    }
    Hw_WindowFrame(&frame, bl, br, line_y);

    // the strikeline under it, a tick out each side that stretches on the
    // press frame (not into the glyphs' column)
    int press = hot >= 0 && (meter[hot].flash >= 0 || meter[hot].hot >= 2);
    float tick = press ? 0.6f : 0.35f;
    GXColor sc = press ? color_white : Color_Over(color_white, 0.45f);
    float tl = glyph > 0 && right ? 0 : tick, tr = glyph > 0 && !right ? 0 : tick;
    float my = line_y + CV_PITCH / 2;
    Hud_Rect(bl - 0.12f - tl, line_y - 0.2f, br + 0.12f + tr, line_y - 0.12f, sc);
    if (tl > 0)
        Hud_Rect(bl - 0.12f - tl, my - 0.06f, bl - 0.2f, my + 0.06f, sc);
    if (tr > 0)
        Hud_Rect(br + 0.2f, my - 0.06f, br + 0.12f + tr, my + 0.06f, sc);

    // the key beside the belt: every kind in its own place, lit while
    // one is up (Stephen, 0.9.10: no time to read words under the belt;
    // the colors do the work, the key is there to learn them)
    static const char *key_label[4] = {"JUMP", "AI", "NIL", "WAVE"}; // no wider than JUMP (WL/WD ran into the belt)
    static const char *key_rank[4] = {"JUMP", "AI", "NIL", "WL"};
    GXColor key_color[4] = {color_in_jump, land_kind_colors[LAND_AI], land_kind_colors[LAND_NIL],
                            land_kind_colors[LAND_PERFECT_WL]};
    for (int i = 0; i < 4; i++)
    {
        int on = 0;
        for (int j = 0; j < n; j++)
            if (Cv_Rank(&meter[order[j]]) == i)
                on = 1;
        MeterRow sw = {0};
        sw.color = key_color[i];
        strcpy(sw.label, key_rank[i]);
        float y = top - 0.6f - i * CV_KEY_PITCH; // a line of ST_TEXT apart
        vis_k = base_k * (on ? 1.f : 0.55f);
        Cv_Note(&sw, CELL_PRESS, TONE_CUE, 1.f, kx, y - 0.22f, kx + 0.55f, y + 0.22f, 0, 0);
        GXColor tc = Color_Mix(key_color[i], color_white, 0.35f);
        tc.a = 255;
        Hud_TextAligned(key_label[i], kx + 0.75f, Text_MidY(y, ST_TEXT), ST_TEXT, tc, 0);
    }
    vis_k = base_k;
    hw_pitch = HW_PITCH;
}
