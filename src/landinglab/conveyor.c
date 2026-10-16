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
// fixed order by slot (Lane_Slot); a lone note takes the whole row. Kinds
// differ by shape as well as color: a jump has a raised cap, an AI is
// solid, a NIL notched, a waveland or wavedash hollow. A preload (a
// plan from a press not made yet) hovers over the rows it will take, its
// shadow on the belt, and sets down when the press comes. What's beyond the
// belt's far end waits over its entry, lower the nearer it is.
#define CV_SHELF 1.3f
#define CV_LIFT 0.4f // a preload's hover: most of a frame (0.22 didn't read as lifted)
#define CV_FADE_FROM 22 // the belt's rows that fade: the plate, belt, rails and lines from here
#define CV_NOTE_FROM 30 // ... the notes from here
// the key's swatches under the window, narrower than a share of the belt
// and centered, so they read as a legend, not as column heads over a
// three-share row (0.9.30 stills)
#define CV_SWATCH_W 0.45f
#define CV_SWATCH_H 0.4f
#define CV_SWATCH_GAP 0.22f

// A row's kind, for its notes' shape and the key.
static int Cv_Rank(MeterRow *r)
{
    return Lane_Slot(r);
}

// Whether a cell takes a share of its row: notes do, the frames in the air
// and the landing lag behind them don't.
static int Cv_Claims(int kind)
{
    return kind == CELL_PRESS || kind == CELL_ACT || kind == CELL_LAND || kind == CELL_TAIL;
}

// The rows to draw, in the belt's order (slot, then a slot's second row),
// a preload last: it takes the far share of each frame it's over.
static int Cv_Place(int i)
{
    return meter[i].parked ? 99 : (i % MT_SLOTS) * 2 + i / MT_SLOTS;
}

static int Cv_Order(int *order)
{
    int n = 0;
    for (int i = 0; i < meter_rows; i++)
    {
        if (meter[i].gap)
            continue;
        int j = n++;
        while (j > 0 && Cv_Place(order[j - 1]) > Cv_Place(i))
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
// (fill and rim at their own strengths: a window's tail is fainter)
static void Cv_Body2(float x0, float y0, float x1, float y1, GXColor col, float fa, float ra, int open_lo, int open_hi)
{
    GXColor rim = Color_Mix(col, color_white, 0.45f);
    rim.a = col.a;
    float ilo = open_lo ? y0 : y0 + PX, ihi = open_hi ? y1 : y1 - PX;
    Hud_Rect(x0 + PX, ilo, x1 - PX, ihi, Color_Fill(col, fa));
    Hud_Rect(x0, ilo, x0 + PX, ihi, Color_Fill(rim, ra));
    Hud_Rect(x1 - PX, ilo, x1, ihi, Color_Fill(rim, ra));
    if (!open_lo)
        Hud_Rect(x0 + PX, y0, x1 - PX, y0 + PX, Color_Fill(rim, ra));
    if (!open_hi)
        Hud_Rect(x0 + PX, y1 - PX, x1 - PX, y1, Color_Fill(rim, ra));
}

static void Cv_Body(float x0, float y0, float x1, float y1, GXColor col, float a, int open_lo, int open_hi)
{
    Cv_Body2(x0, y0, x1, y1, col, a, a, open_lo, open_hi);
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

// A window's tail behind its head (Row_WindowTail): its later frames,
// narrower and fainter, growing out of the head with no rim at its foot. A
// hollow note's is its two side rails and a top rail.
static void Cv_Tail(MeterRow *r, int tone, float a, float x0, float y0, float x1, float y1, int open_hi)
{
    GXColor col = Tone_Color(r, tone);
    int rank = tone == TONE_JUMP ? 0 : Cv_Rank(r);
    float hi = open_hi ? y1 : y1 - 0.05f;
    x0 += 0.12f;
    x1 -= 0.12f;
    if (hi <= y0 || x1 <= x0)
        return;
    if (rank == 3 && tone != TONE_MISS && tone != TONE_SKIP)
    {
        float t = 2 * PX;
        Hud_Rect(x0, y0, x0 + t, hi, Color_Fill(col, a * 0.7f));
        Hud_Rect(x1 - t, y0, x1, hi, Color_Fill(col, a * 0.7f));
        if (!open_hi)
            Hud_Rect(x0 + t, hi - t, x1 - t, hi, Color_Fill(col, a * 0.7f));
        return;
    }
    Cv_Body2(x0, y0, x1, hi, col, a * 0.4f, a * 0.7f, 1, open_hi);
}

// Cells k to e of row r as one note from y0 to y1 (lo / hi: it goes on
// past that end). A window is a head, its first frame drawn one row tall
// as the note, and a tail behind it, so the window is open while a head is
// in the box (conveyor measurements, 2026-10-16: the 2-4 row body was read
// as the note, a frame or two early).
static void Cv_Run(MeterRow *r, int k, int e, float a, float x0, float y0, float x1, float y1, int lo, int hi)
{
    int kind = r->cell[k], tone = r->tone[k];
    if (kind != CELL_PRESS)
    {
        Cv_Note(r, kind, tone, a, x0, y0, x1, y1, lo, hi);
        return;
    }
    if (Row_WindowTail(r, k))
    {
        Cv_Tail(r, tone, a, x0, y0, x1, y1, hi);
        return;
    }
    if (e > k)
        Cv_Tail(r, tone, a, x0, y0 + CV_PITCH - 0.05f, x1, y1, hi);
    Cv_Note(r, kind, tone, a, x0, y0, x1, y0 + CV_PITCH, 0, 0);
}

// The row a cell is drawn on, slid (r->slide) to the nearest whole row,
// for the shares of the belt.
static int Cv_Shift(MeterRow *r)
{
    return Row_Round(r->slide);
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
    // the belt on the side toward the screen's edge, the glyphs' column
    // (always kept) toward its middle
    float bl = x0 + STRIP_PAD + (right ? HW_GLYPH : 0), br = bl + CV_W;
    float glyph_x = right ? x0 + STRIP_PAD + HW_GLYPH / 2 : br + HW_GLYPH / 2;
    float line_y = y0 + CV_BASE, top = line_y + CV_ROWS * CV_PITCH, fade_y = line_y + CV_FADE_FROM * CV_PITCH;
    hw_pitch = CV_PITCH; // the window's frame is a row of the belt
    float base_k = vis_k, dim = Strip_Dim(fp);
    int now = event_vars->game_timer;
    int order[MT_LANES], n = Cv_Order(order);

    // the plate and belt stay up while the strip is on, so the belt's
    // motion is always there to read the notes' speed against (0.9.12's
    // faded with the cues, and the stage showed through it). Its far end
    // fades into the stage, and all of it fades back while Falcon is over
    // it; the notes, window and endings don't (Strip_Dim)
    Hud_RectFade(x0, y0, x0 + w, y0 + h, fade_y, top, color_plate, 0.85f * dim);
    Hud_RectFade(bl, line_y, br, top, fade_y, top, color_white, 0.07f * dim);
    // the belt moves: a line between every two frames, a heavier one every
    // fifth frame of the game's clock, with studs on the rails, so they
    // travel down a row each frame (brighter: low contrast, visual audit
    // 2026-10-15)
    for (int k = 1; k < CV_ROWS; k++)
    {
        float y = line_y + k * CV_PITCH, f = Track_Fade(k, CV_FADE_FROM, CV_ROWS) * dim;
        if ((now + k) % 5 == 0)
        {
            Hud_Rect(bl, y - 0.09f, br, y + 0.09f, Color_Over(color_white, 0.5f * f));
            Hud_Rect(bl - 0.42f, y - 0.14f, bl - 0.16f, y + 0.14f, Color_Over(color_white, 0.5f * f));
            Hud_Rect(br + 0.16f, y - 0.14f, br + 0.42f, y + 0.14f, Color_Over(color_white, 0.5f * f));
        }
        else
            Hud_Rect(bl, y - PX / 2, br, y + PX / 2, Color_Over(color_white, 0.22f * f));
    }
    Hud_RectFade(bl - 0.12f, line_y, bl - 0.06f, top, fade_y, top, color_white, 0.2f * dim);
    Hud_RectFade(br + 0.06f, line_y, br + 0.12f, top, fade_y, top, color_white, 0.2f * dim);

    // the notes: each row's runs of frames that keep their share of the
    // belt, drawn whole, so a window reads as one note. A re-timed plan's
    // notes slide (r->slide), and take their shares where they're drawn
    int slot[MT_LANES][MT_CELLS], cnt[CV_ROWS], sh[MT_LANES];
    for (int j = 0; j < n; j++)
    {
        sh[j] = Cv_Shift(&meter[order[j]]);
        for (int k = 0; k < MT_CELLS; k++)
            slot[j][k] = -1;
    }
    for (int d = 0; d < CV_ROWS; d++)
    {
        cnt[d] = 0;
        for (int j = 0; j < n; j++)
        {
            MeterRow *r = &meter[order[j]];
            int k = d - sh[j];
            if (k < 0 || k >= MT_CELLS)
                continue;
            // a preload hovers most of a frame up, so it takes its share of
            // the frame over each note too (Stephen, 0.9.28: moving, Jump
            // Timing's hop notes came up beside the wavedash's preload and
            // it hovered into them)
            int claims = k < r->len && Cv_Claims(r->cell[k]);
            if (r->parked && k > 0 && k - 1 < r->len && Cv_Claims(r->cell[k - 1]))
                claims = 1;
            slot[j][k] = claims ? cnt[d]++ : -1;
        }
    }

    // a plan swapped out: its notes step on with the belt, fading, in their
    // lane's share where it has one, never into the window, and with no
    // glyphs, hot or ending (laid under the notes coming in)
    for (int j = 0; j < n; j++)
    {
        int la;
        MeterRow *lv = Lane_Leave(&meter[order[j]], &la);
        if (!lv || lv->parked)
            continue;
        for (int k = 0; k < lv->len && k < MT_CELLS; k++)
        {
            int kind = lv->cell[k];
            float at = k + lv->slide - la;
            int d = Row_Round(at);
            if (!Cv_Claims(kind) || at < 0.99f || d >= CV_ROWS)
                continue;
            int kc = d - sh[j], own = kc >= 0 && kc < MT_CELLS && slot[j][kc] >= 0;
            int e = k;
            while (e + 1 < lv->len && e + 1 < MT_CELLS && d + (e + 1 - k) < CV_ROWS && lv->cell[e + 1] == kind &&
                   lv->tone[e + 1] == lv->tone[k] && lv->alpha[e + 1] == lv->alpha[k])
            {
                int dn = d + (e + 1 - k), kn = dn - sh[j];
                int own_n = kn >= 0 && kn < MT_CELLS && slot[j][kn] >= 0;
                if (own_n != own || (own && (slot[j][kn] != slot[j][kc] || cnt[dn] != cnt[d])))
                    break;
                e++;
            }
            float sx0 = bl + 0.15f, sx1 = br - 0.15f;
            if (own)
            {
                float share = (CV_W - 0.3f) / cnt[d];
                sx0 = bl + 0.15f + slot[j][kc] * share + (cnt[d] > 1 ? 0.06f : 0);
                sx1 = sx0 + share - (cnt[d] > 1 ? 0.12f : 0);
            }
            float y0 = line_y + at * CV_PITCH, y1 = line_y + (e + 1 + lv->slide - la) * CV_PITCH;
            if (y1 > top)
                y1 = top;
            int lo = k > 0 && lv->cell[k - 1] == kind && lv->tone[k - 1] == lv->tone[k];
            int hi = e + 1 < lv->len && lv->cell[e + 1] == kind && lv->tone[e + 1] == lv->tone[k];
            vis_k = base_k * lv->fade * Note_Far(at, CV_NOTE_FROM, CV_ROWS);
            float a = lv->alpha[k] / 255.f * (lv->dim ? 0.5f : 1.f) * (1.f - la / 5.f);
            Cv_Run(lv, k, e, a, sx0, y0, sx1, y1, lo, hi);
            k = e;
        }
    }
    vis_k = base_k;

    for (int j = 0; j < n; j++)
    {
        MeterRow *r = &meter[order[j]];
        float lift = Cv_Lift(r), in_a = 1.f - r->arrive;
        for (int k = 0; k < r->len && k < MT_CELLS; k++)
        {
            // what's drawn here: a note in its share, or lag across the
            // belt where no note is
            int kind = r->cell[k], d = k + sh[j];
            if (d < 0 || d >= CV_ROWS || k + r->slide < -0.01f)
                continue; // (slid past the line: frames before its first target)
            if (slot[j][k] < 0 && !(kind == CELL_LAG && cnt[d] == 0))
                continue;
            if (r->parked && !Cv_Claims(kind))
                continue; // only the room it hovers into, kept clear above a note
            int e = k;
            while (e + 1 + sh[j] < CV_ROWS && e + 1 < r->len && e + 1 < MT_CELLS && r->cell[e + 1] == kind &&
                   r->tone[e + 1] == r->tone[k] && r->alpha[e + 1] == r->alpha[k] && slot[j][e + 1] == slot[j][k] &&
                   (slot[j][k] < 0 ? cnt[e + 1 + sh[j]] == 0 : cnt[e + 1 + sh[j]] == cnt[d]))
                e++;
            float sx0 = bl + 0.15f, sx1 = br - 0.15f;
            int most = slot[j][k] >= 0 ? cnt[d] : 1;
            if (r->parked && slot[j][k] >= 0)
            {
                // the preload is last in the belt's order: the far share, as narrow as the most crowded frame it
                // hovers over, so it keeps one place and clears the rest
                for (int f = d; f <= d + (e - k) + 1 && f < CV_ROWS; f++)
                    if (cnt[f] > most)
                        most = cnt[f];
                float share = (CV_W - 0.3f) / most;
                sx1 = br - 0.15f - (most > 1 ? 0.06f : 0);
                sx0 = sx1 - share + (most > 1 ? 0.12f : 0);
            }
            else if (slot[j][k] >= 0)
            {
                float share = (CV_W - 0.3f) / cnt[d];
                sx0 = bl + 0.15f + slot[j][k] * share + (cnt[d] > 1 ? 0.06f : 0);
                sx1 = sx0 + share - (cnt[d] > 1 ? 0.12f : 0);
            }
            float y0 = line_y + (k + r->slide) * CV_PITCH, y1 = line_y + (e + 1 + r->slide) * CV_PITCH;
            if (y1 > top)
                y1 = top;
            int lo = k > 0 && r->cell[k - 1] == kind && r->tone[k - 1] == r->tone[k];
            int hi = e + 1 < r->len && r->cell[e + 1] == kind && r->tone[e + 1] == r->tone[k];
            vis_k = base_k * r->fade * Note_Far(k + r->slide, CV_NOTE_FROM, CV_ROWS);
            // where it will set down: a light outline on the belt under it
            // (a dark shadow was lost on the dark belt, 0.9.14 test)
            // (0.9.15 test: right under the note it was hidden, so the note
            // also leans left off it while it hovers)
            float lean = most > 1 ? 0 : -0.4f * lift; // (not into a neighbour's share)
            if (lift > 0 && kind != CELL_LAG)
                Hud_Frame(sx0, y0 + 0.05f, sx1, y1 - 0.05f, 1.5f * PX, Color_Over(color_white, 0.5f * (1.f - r->settle / 4.f)));
            float a = r->alpha[k] / 255.f * (r->dim ? 0.5f : 1.f) * in_a;
            Cv_Run(r, k, e, a, sx0 + lean, y0 + lift, sx1 + lean, y1 + lift, lo, hi);
            if (r->settle == 1 && kind != CELL_LAG)
                Hud_Frame(sx0 - PX, y0 - PX, sx1 + PX, y1 + PX, 1.4f * PX, Color_Fill(color_white, 0.8f)); // set down
            k = e;
        }
    }

    // over the entry: each row's next note past the far end, waiting, as
    // dim as a note coming in (floating ghosts, over a plate faded out)
    int wait[MT_LANES], wk[MT_LANES], q = 0;
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
        vis_k = base_k * r->fade * 0.8f * NOTE_FAR;
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
        float lift = Cv_Lift(r);
        for (int k = 0; k < r->len && k < MT_CELLS; k++)
        {
            float at = k + r->slide;
            if (!r->glyph[k] || at < -0.01f || at > CV_ROWS - 0.5f)
                continue;
            vis_k = base_k * r->fade * Note_Far(at, CV_NOTE_FROM, CV_ROWS);
            Glyph_Draw(r->glyph[k], glyph_x, line_y + (at + 0.5f) * CV_PITCH + lift, Glyph_Strip(r->glyph[k], 0.3f),
                       Color_Fill(color_white, 0.95f * (r->dim ? 0.5f : 1.f) * (1.f - r->arrive)));
        }
    }

    // the window: one across the belt, lit by the hottest row due, each
    // row's ending in its share
    int at[MT_LANES], e = 0, hot = -1;
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

    // the key under the window: every kind's swatch in the slots' order,
    // lit while one is on the belt, no words (Stephen, 0.9.10: no time to
    // read words; the colors do the work, the key is there to learn them.
    // Visual audit, 2026-10-15: its words overlapped the belt and widened
    // the strip)
    float kx = (bl + br) / 2 - (MT_SLOTS_SHOWN * CV_SWATCH_W + (MT_SLOTS_SHOWN - 1) * CV_SWATCH_GAP) / 2;
    for (int i = 0; i < MT_SLOTS_SHOWN; i++)
    {
        int on = 0;
        for (int j = 0; j < n; j++)
            if (Cv_Rank(&meter[order[j]]) == i)
                on = 1;
        vis_k = base_k;
        Slot_Swatch(i, 0, kx + CV_SWATCH_W / 2 + i * (CV_SWATCH_W + CV_SWATCH_GAP), y0 + 0.65f, CV_SWATCH_W, CV_SWATCH_H,
                    on ? 1.f : SLOT_DIM);
    }
    vis_k = base_k;
    hw_pitch = HW_PITCH;
}
