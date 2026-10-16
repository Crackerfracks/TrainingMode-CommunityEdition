/* landinglab/highway.c
 *
 * Holds dial geometry, a strip row's ending, and the Highway strip.
 *
 * Part of Landing Gear: src/landinglab.c includes it, in order, into one
 * translation unit, so it sees everything included before it (and mex.h,
 * events.h, landinglab_sim.h).
 *
 * Uses: timers, drawing, hud_drawing, meter, meter_placement.
 */

// The dial's wedge, ring and hand are measured in degrees clockwise from
// twelve o'clock; a frame is DL_DEG of them.
#define DL_DEG 15.f

static void Dial_Pt(float cx, float cy, float r, float deg, float *x, float *y)
{
    float a = deg * 0.0174533f;
    *x = cx + sin(a) * r;
    *y = cy + cos(a) * r;
}

// A pie slice from one angle to the next (a whole disc for 360), in steps
// of at most 30 degrees.
static void Dial_Fan(float cx, float cy, float r, float from, float to, GXColor c)
{
    int n = (int)((to - from) / 30.f + 0.999f);
    if (n < 1)
        return;
    float step = (to - from) / n, px, py, nx, ny;
    Dial_Pt(cx, cy, r, from, &px, &py);
    for (int i = 1; i <= n; i++)
    {
        Dial_Pt(cx, cy, r, from + step * i, &nx, &ny);
        Hud_Tri(cx, cy, px, py, nx, ny, c);
        px = nx;
        py = ny;
    }
}

// An arc band between two radii.
static void Dial_Band(float cx, float cy, float r0, float r1, float from, float to, GXColor c)
{
    int n = (int)((to - from) / 30.f + 0.999f);
    if (n < 1)
        return;
    float step = (to - from) / n, ix, iy, ox, oy, jx, jy, kx, ky;
    Dial_Pt(cx, cy, r0, from, &ix, &iy);
    Dial_Pt(cx, cy, r1, from, &ox, &oy);
    for (int i = 1; i <= n; i++)
    {
        Dial_Pt(cx, cy, r0, from + step * i, &jx, &jy);
        Dial_Pt(cx, cy, r1, from + step * i, &kx, &ky);
        Quad_Add(ix, iy, ox, oy, kx, ky, jx, jy, c);
        ix = jx;
        iy = jy;
        ox = kx;
        oy = ky;
    }
}

// The ending a row plays at its gate, shared by the highway and the dial:
// the ghost's swell (a filled copy of the gate that grows and fades, strong
// on a press that worked), the hit's burst and the fold of a miss or skip.
// (cx, cy) is the gate's middle, hw by hh its half size (a round gate uses
// hw), s a size to scale the effects by.
static void Strip_Ending(MeterRow *r, float cx, float cy, float hw, float hh, float s, int round)
{
    for (int g = 0; g < 4; g++)
    {
        if (r->ghost[g] < 0)
            continue;
        float q = (float)r->ghost[g] / LL_GHOST, fade = (1.f - q) * (1.f - q);
        int b = r->ghost_bright[g];
        float grow = (b ? 0.1f + 1.0f * Ease_Out(q) : 0.05f + 0.45f * Ease_Out(q)) * s;
        GXColor tint = Color_Mix(r->color, color_white, b ? 0.35f : 0.2f);
        if (round)
            Dial_Fan(cx, cy, hw + grow, 0, 360, Color_Over(tint, (b ? 0.5f : 0.2f) * fade));
        else
        {
            Hud_Rect(cx - hw - grow, cy - hh - grow, cx + hw + grow, cy + hh + grow, Color_Over(tint, (b ? 0.55f : 0.22f) * fade));
            Hud_Frame(cx - hw - grow, cy - hh - grow, cx + hw + grow, cy + hh + grow, 1.6f * PX, Color_Over(tint, (b ? 0.95f : 0.45f) * fade));
        }
    }
    if (r->burst >= 0)
    {
        float q = (float)r->burst / LL_BURST;
        float rad = (round ? hw + 0.3f * s : 0.5f * s) + 1.6f * s * Ease_Out(q);
        float from = rad * 0.55f;
        if (round)
            from = rad - 0.8f * s > hw + 0.1f * s ? rad - 0.8f * s : hw + 0.1f * s; // a dial's rays start outside its rim
        GXColor bc = Color_Fill(r->color, 1.f - q);
        for (int ray = 0; ray < 6; ray++)
        {
            float ang = ray * 1.0471976f + 0.5235988f;
            float dx = cos(ang), dy = sin(ang);
            Hud_Seg(cx + dx * from, cy + dy * from, cx + dx * rad, cy + dy * rad, 0.18f * s, bc);
        }
    }
    if (r->implode >= 0)
    {
        float q = (float)r->implode / LL_FADE;
        GXColor dc = r->implode_tone == TONE_MISS ? color_miss : color_skip;
        float k = 1.f - Ease_Out(q);
        if (round)
        {
            float rr = (hw + 0.8f * s) * k;
            if (rr > 0.15f)
                Dial_Band(cx, cy, rr - 0.1f, rr + 0.1f, 0, 360, Color_Fill(dc, 0.6f * (1.f - q)));
        }
        else
        {
            if (r->implode == 0)
                Hud_Rect(cx - hw, cy - hh, cx + hw, cy + hh, dc); // the frame that was needed
            float fw = (hw + 0.8f * s) * k, fh = (hh + 0.6f * s) * k;
            if (fw > 0.05f)
                Hud_Frame(cx - fw, cy - fh, cx + fw, cy + fh, 1.4f * PX, Color_Fill(dc, 0.6f * (1.f - q)));
        }
    }
}

// The highway's and conveyor's far end fades into the stage, so their
// height (the lead, Stephen 0.9.19) isn't a wall (visual audit,
// 2026-10-15). Their plate, lanes and lines go from whole at row from to
// nothing at row to; a note there only dims, to NOTE_FAR, so one coming in
// is dim, not gone.
#define NOTE_FAR 0.6f

static float Track_Fade(float row, float from, float to)
{
    return 1.f - Clamp01((row - from) / (to - from));
}

static float Note_Far(float row, float from, float to)
{
    return 1.f - (1.f - NOTE_FAR) * Clamp01((row - from) / (to - from));
}

// A rect of c at alpha a up to fy0, fading from there to nothing at fy1.
static void Hud_RectFade(float x0, float y0, float x1, float y1, float fy0, float fy1, GXColor c, float a)
{
    if (y0 < fy0)
        Hud_Rect(x0, y0, x1, y1 < fy0 ? y1 : fy0, Color_Over(c, a));
    float lo = y0 > fy0 ? y0 : fy0, hi = y1 < fy1 ? y1 : fy1;
    if (hi <= lo)
        return;
    float ka = 1.f - (lo - fy0) / (fy1 - fy0), kb = 1.f - (hi - fy0) / (fy1 - fy0);
    Quad_Add2(x0, lo, x1, lo, x1, hi, x0, hi, Color_Over(c, a * ka), Color_Over(c, a * kb));
}

#define HW_FADE_FROM 14 // the highway's rows that fade: the plate and lines from here
#define HW_NOTE_FROM 20 // ... the notes from here

// A row's cells on a highway lane, shift rows up from their place, at a_k
// of their strength, none under row from. A window's first frame is its
// head, one row at full strength, and its later frames a fainter, narrower
// tail behind it, so the head is what reads as due: the window is open
// while a head is in the box (conveyor measurements, 2026-10-16: the 2-4
// row body was read as the note, a frame or two early).
static void Hw_Cells(MeterRow *r, MeterRow *o, float lx, float rx, float line_y, float shift, float a_k, float from)
{
    float in = 0.15f, top = line_y + HW_ROWS * HW_PITCH, floor_y = line_y + from * HW_PITCH + 0.04f;
    float dim = r->dim ? 0.5f : 1.f, base_k = vis_k;
    int len = r->len < MT_CELLS ? r->len : MT_CELLS;

    for (int k = 0; k < len; k++)
    {
        int kind = r->cell[k];
        if (kind == CELL_NONE || (kind == CELL_AIR && Row_NoteAt(o, k)))
            continue;
        int tone = r->tone[k], al = r->alpha[k];
        int j = k;
        // a window two frames wide is one note two rows tall
        if (kind == CELL_PRESS || kind == CELL_LAG || kind == CELL_TAIL)
        {
            while (j + 1 < len && r->cell[j + 1] == kind && r->tone[j + 1] == tone && r->alpha[j + 1] == al)
                j++;
        }
        float a = al / 255.f * dim * a_k;
        GXColor col = Tone_Color(r, tone);
        float at = Note_At(r, k, HW_ROWS) + shift;
        float y0 = line_y + at * HW_PITCH + 0.04f;
        float y1 = line_y + (Note_At(r, j, HW_ROWS) + shift + 1.f) * HW_PITCH - 0.04f;
        if (y0 >= top - 0.1f)
        {
            k = j;
            continue; // still off the far end
        }
        if (y1 > top)
            y1 = top;
        if (y0 < floor_y)
            y0 = floor_y; // (slid past the line: frames before its first target)
        if (y1 <= y0)
        {
            k = j;
            continue;
        }
        float yc = (y0 + y1) / 2, mid = (lx + rx) / 2, lag = (rx - lx) * 0.24f;
        vis_k = base_k * Note_Far(at, HW_NOTE_FROM, HW_ROWS);
        switch (kind)
        {
        case CELL_PRESS:
        {
            float hy = Row_WindowTail(r, k) ? y0 : line_y + (at + 1.f) * HW_PITCH - 0.04f;
            if (hy > y1)
                hy = y1;
            if (hy < y0)
                hy = y0;
            if (hy > y0)
                Hud_Rect(lx + in, y0, rx - in, hy, Color_Fill(col, a));
            if (y1 > hy)
                Hud_Rect(lx + in + 0.1f, hy, rx - in - 0.1f, y1, Color_Fill(col, a * 0.4f));
            break;
        }
        case CELL_ACT:
            Hud_Rect(lx + in, y0, rx - in, y1, Color_Fill(col, a));
            Hud_Rect(lx + in, y0, rx - in, y0 + 0.2f * HW_PITCH + 0.04f, Color_Fill(color_white, a * 0.9f));
            break;
        case CELL_AIR:
            Hud_Rect(lx + 0.45f, yc - 0.07f, rx - 0.45f, yc + 0.07f, Color_Over(col, 0.45f * a));
            break;
        case CELL_LAND:
            Hud_Rect(lx + in, y0, rx - in, y1, Color_Fill(color_plate, a * 0.6f));
            Hud_Frame(lx + in, y0, rx - in, y1, 1.4f * PX, Color_Fill(col, a));
            Hud_Rect(lx + in, y0, rx - in, y0 + 0.24f * HW_PITCH + 0.04f, Color_Fill(col, a));
            break;
        case CELL_LAG:
            Hud_Rect(mid - lag, y0, mid + lag, y1, Color_Fill(col, a * 0.75f));
            break;
        case CELL_TAIL:
            Hud_Rect(lx + in, y0, rx - in, y1, Color_Fill(color_galint, a));
            break;
        }
        k = j;
    }
    vis_k = base_k;
}

// One lane of the highway, lx to rx: its cells as notes, a cell a frame up
// from the hit line, a re-timed plan slid (r->slide), and the plan it
// swapped out stepping down fading, never into the window. o is the slot's
// other row, if any: this one's frames in the air don't cross its notes.
static void Hw_Lane(MeterRow *r, MeterRow *o, float glyph_x, float lx, float rx, float line_y)
{
    float dim = r->dim ? 0.5f : 1.f, base_k = vis_k, in_a = 1.f - r->arrive;
    int la;
    MeterRow *lv = Lane_Leave(r, &la);
    if (lv)
        Hw_Cells(lv, o, lx, rx, line_y, lv->slide - la, 1.f - la / 5.f, 1.f);
    Hw_Cells(r, o, lx, rx, line_y, r->slide, in_a, 0);
    int len = r->len < MT_CELLS ? r->len : MT_CELLS;
    for (int k = 0; k < len; k++)
    {
        float at = Note_At(r, k, HW_ROWS) + r->slide;
        vis_k = base_k * Note_Far(at, HW_NOTE_FROM, HW_ROWS);
        if (r->glyph[k] && at < HW_ROWS - 0.5f && at > -0.01f)
            Glyph_Draw(r->glyph[k], glyph_x, line_y + (at + 0.5f) * HW_PITCH, 0.3f, Color_Fill(color_white, 0.95f * dim * in_a));
    }
    vis_k = base_k;
}

// A lane's window at the bottom: a frame one note tall that notes drop
// into, the way the cells' gate has posts
// (Stephen, 0.9.8: notes slammed into the bottom with nothing to stop in).
// It lights while a press is due. Its endings play in it: a hit burns the
// note white and sends a beam up the lane; a skip leaves the note in the
// window in gray, fading; a miss leaves it cracked in two, the halves
// drifting apart. Lanes sit too close for side ticks, so the strikeline
// under them all carries those (Highway_Draw).
static void Hw_WindowEnd(MeterRow *r, float lx, float rx, float line_y)
{
    float in = 0.15f, mid = (lx + rx) / 2;
    int ending = !r->dim && r->implode >= 0;
    float q = ending ? (float)r->implode / LL_FADE : 1.f;
    GXColor dc = r->implode_tone == TONE_MISS ? color_miss : color_skip;

    if (ending)
    {
        // the note that was due, kept in the window
        float a = 0.9f * (1.f - q);
        float ny0 = line_y + 0.04f, ny1 = line_y + hw_pitch - 0.04f;
        if (r->implode_tone == TONE_MISS)
        {
            float d = 0.45f * Ease_Out(q), h = (mid - lx - in) - 0.06f;
            Hud_Rect(mid - 0.06f - h - d, ny0, mid - 0.06f - d, ny1, Color_Fill(dc, a));
            Hud_Rect(mid + 0.06f + d, ny0, mid + 0.06f + h + d, ny1, Color_Fill(dc, a));
        }
        else
        {
            Hud_Rect(lx + in, ny0, rx - in, ny1, Color_Fill(dc, a * 0.75f));
            Hud_Frame(lx + in, ny0, rx - in, ny1, 1.4f * PX, Color_Fill(color_white, a * 0.35f));
        }
    }
    if (!r->dim && r->burst >= 0)
    {
        // the burn: white, cooling to the row's color, and a beam up the lane
        float b = (float)r->burst / LL_BURST, fade = 1.f - b;
        Hud_Rect(lx + 0.05f, line_y, rx - 0.05f, line_y + hw_pitch, Color_Fill(Color_Mix(color_white, r->color, b), fade));
        float beam = (2.f + 5.f * Ease_Out(b)) * hw_pitch, bw = (rx - lx) * (0.5f - 0.3f * b);
        for (int i = 0; i < 4; i++)
        {
            float y = line_y + hw_pitch + beam * i / 4.f;
            Hud_Rect(mid - bw, y, mid + bw, y + beam / 4.f, Color_Over(Color_Mix(r->color, color_white, 0.5f), 0.5f * fade * (1.f - i / 4.f)));
        }
    }
    if (!r->dim)
    {
        // the hit's ghost; the burn and beam above stand in for the burst's
        // rays, and the window plays the fold
        MeterRow e = *r;
        e.burst = -1;
        e.implode = -1;
        Strip_Ending(&e, mid, line_y + hw_pitch / 2, (rx - lx) / 2, hw_pitch / 2, 1.f, 0);
    }
}

// How much a row lights its window: due on the press frame, due, ending.
static int Hw_FrameRank(MeterRow *r)
{
    if (r->dim)
        return 0;
    return r->flash >= 0 || r->hot >= 2 ? 3 : r->hot ? 2 : r->implode >= 0 ? 1 : 0;
}

// The window's frame's thickness: heavier with Cell Size, as the rows are.
static float Hw_FrameT(void)
{
    return 0.14f * (0.5f + 0.5f * WIDE_K);
}

// The window's frame, lit while r's press is due (white on its first or
// last frame), in r's ending colors as it fades. Heavier at rest than it
// was, so the eye finds it first (visual audit, 2026-10-15: thinner than
// the belt's fifth-frame lines). Its lid sits just over the window's row,
// so a note a frame early sits on the lid, never in the box, and its floor
// stops at the line (conveyor measurements, 2026-10-16: the lid was entered
// a frame before the press, and the floor ate the window's row).
static void Hw_WindowFrame(MeterRow *r, float lx, float rx, float line_y)
{
    float t = Hw_FrameT();
    float wx0 = lx - 0.12f, wx1 = rx + 0.12f, wy0 = line_y - t, wy1 = line_y + hw_pitch + 0.04f;
    int hot = r->dim ? 0 : r->flash >= 0 ? 2 : r->hot;
    int ending = !r->dim && r->implode >= 0;
    GXColor dc = r->implode_tone == TONE_MISS ? color_miss : color_skip;
    if (hot > 0)
        Hud_Rect(wx0 - 0.2f, wy0 - 0.2f, wx1 + 0.2f, wy1 + 0.2f, Color_Fill(r->color, 0.3f));
    GXColor fc = hot >= 2 ? color_white : hot ? Color_Mix(r->color, color_white, 0.4f)
                 : ending ? Color_Mix(dc, color_white, 0.3f)
                          : Color_Over(color_white, r->dim ? 0.3f : 0.6f);
    if (hot >= 2)
        t += 0.02f;
    Hud_Frame(wx0, wy0, wx1, wy1, t, fc);
    Hud_Rect(wx0, wy0, wx1, wy0 + t, fc); // the floor of it, heavier
}

// An empty slot's window: its frame, faint, and no floor, so the lane is
// there with nothing due (brighter: 0.25 was lost over the stage, 0.9.30
// stills).
static void Hw_EmptyWindow(float lx, float rx, float line_y)
{
    Hud_Frame(lx - 0.12f, line_y - Hw_FrameT(), rx + 0.12f, line_y + hw_pitch + 0.04f, 0.1f, Color_Over(color_white, 0.4f));
}

// The highway (Fixed Strip): a vertical lane a slot, notes falling onto a
// hit line at the bottom, a row a frame with every fifth one heavier, the
// slot's swatch under its lane. The hit line lights while a press is due,
// and the ghost, burst and fold are the cells' at the line.
static void Highway_Draw(FighterData *fp)
{
    float x0, y0, w, h;
    Strip_Place(fp, STRIP_HIGHWAY, &x0, &y0, &w, &h);
    // the lanes count from the screen's edge, in the slots' order left to
    // right, with the glyphs' column after them toward the middle (kept
    // whether or not there are glyphs) and the rest's half lane past that
    // while it's used, so nothing showing up moves a lane (Stephen, 0.8.6:
    // the lanes shifted when the markers came in)
    int right = x0 > 0, glyphs = Strip_Glyphs();
    float lanes_l = right ? x0 + w - STRIP_PAD - HW_LANES : x0 + STRIP_PAD, lanes_r = lanes_l + HW_LANES;
    float glyph_x = right ? lanes_l - HW_GLYPH / 2 : lanes_r + HW_GLYPH / 2;
    float other_l = right ? lanes_l - HW_GLYPH - HW_OTHER_GAP - HW_OTHER : lanes_r + HW_GLYPH + HW_OTHER_GAP;
    float line_y = y0 + HW_LABEL, top = line_y + HW_ROWS * HW_PITCH, fade_y = line_y + HW_FADE_FROM * HW_PITCH;
    int slots = Slot_Shown();
    float base_k = vis_k, plate_k = Strip_PlateK(), dim = Strip_Dim(fp);
    hw_pitch = HW_PITCH;

    vis_k = base_k * plate_k;
    Hud_RectFade(x0, y0, x0 + w, y0 + h, fade_y, top, color_plate, 0.72f * dim);
    // every fifth frame of the game's clock heavier, so the lines travel
    // down a row a frame, like the conveyor's (brighter: they were faint,
    // visual audit 2026-10-15)
    for (int k = 1; k <= HW_ROWS; k++)
    {
        int five = (event_vars->game_timer + k) % 5 == 0;
        float y = line_y + k * HW_PITCH, e = five ? 0.1f : 0.05f;
        GXColor lc = Color_Over(color_white, (five ? 0.4f : 0.18f) * Track_Fade(k, HW_FADE_FROM, HW_ROWS) * dim);
        Hud_Rect(lanes_l - 0.1f, y - e, lanes_r + 0.1f, y + e, lc);
        if (slots > MT_SLOTS_SHOWN)
            Hud_Rect(other_l - 0.1f, y - e, other_l + HW_OTHER + 0.1f, y + e, lc);
    }

    // the strikeline: under every lane's window, a tick out past each end
    // that stretches on the press frame
    int press = 0;
    for (int i = 0; i < meter_rows; i++)
        if (!meter[i].gap && !meter[i].dim && (meter[i].flash >= 0 || meter[i].hot >= 2))
            press = 1;
    float tick = press ? 0.6f : 0.35f;
    GXColor sc = press ? color_white : Color_Over(color_white, 0.45f);
    // (not into the glyphs' column, where the next input's glyph sits)
    float tl = glyphs && right ? 0 : tick, tr = glyphs && !right ? 0 : tick;
    float my = line_y + HW_PITCH / 2;
    Hud_Rect(lanes_l - 0.12f - tl, line_y - 0.2f, lanes_r + 0.12f + tr, line_y - 0.12f, sc);
    if (tl > 0)
        Hud_Rect(lanes_l - 0.12f - tl, my - 0.06f, lanes_l - 0.2f, my + 0.06f, sc);
    if (tr > 0)
        Hud_Rect(lanes_r + 0.2f, my - 0.06f, lanes_r + 0.12f + tr, my + 0.06f, sc);

    for (int s = 0; s < slots; s++)
    {
        MeterRow *sr[2];
        int m = Slot_Rows(s, sr);
        float lx = s < MT_SLOTS_SHOWN ? lanes_l + s * (HW_LANE + HW_LANE_GAP) : other_l;
        float rx = lx + (s < MT_SLOTS_SHOWN ? HW_LANE : HW_OTHER);
        // the lane: fainter while empty, and the rest's always (visual
        // audit, 2026-10-15)
        vis_k = base_k * (m ? sr[0]->fade : plate_k);
        Hud_RectFade(lx, line_y, rx, top, fade_y, top, color_white, (m && s < MT_SLOTS_SHOWN ? 0.05f : 0.03f) * dim);
        MeterRow *f = m ? sr[0] : 0;
        for (int j = 0; j < m; j++)
        {
            vis_k = base_k * sr[j]->fade;
            Hw_Lane(sr[j], m > 1 ? sr[1 - j] : 0, glyph_x, lx, rx, line_y);
            Hw_WindowEnd(sr[j], lx, rx, line_y);
            if (Hw_FrameRank(sr[j]) > Hw_FrameRank(f))
                f = sr[j];
        }
        if (f)
        {
            vis_k = base_k * f->fade;
            Hw_WindowFrame(f, lx, rx, line_y);
        }
        else
        {
            vis_k = base_k * plate_k;
            Hw_EmptyWindow(lx, rx, line_y);
        }
        Slot_Swatch(s, f, (lx + rx) / 2, y0 + 0.75f, s < MT_SLOTS_SHOWN ? 0.9f : 0.5f, 0.45f, m ? 1.f : SLOT_DIM);
    }
    vis_k = base_k;
}
