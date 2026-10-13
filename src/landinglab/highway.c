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

// One lane of the highway: its cells as notes, a cell a frame up from the
// hit line.
static void Hw_Lane(MeterRow *r, float glyph_x, float lx, float line_y)
{
    float rx = lx + HW_LANE, in = 0.15f, top = line_y + HW_ROWS * HW_PITCH;
    float dim = r->dim ? 0.5f : 1.f;
    int len = r->len < HW_ROWS ? r->len : HW_ROWS;

    for (int k = 0; k < len; k++)
    {
        int kind = r->cell[k];
        if (kind == CELL_NONE)
            continue;
        int tone = r->tone[k], al = r->alpha[k];
        int j = k;
        // a window two frames wide is one note two rows tall
        if (kind == CELL_PRESS || kind == CELL_LAG || kind == CELL_TAIL)
        {
            while (j + 1 < len && r->cell[j + 1] == kind && r->tone[j + 1] == tone && r->alpha[j + 1] == al)
                j++;
        }
        float a = al / 255.f * dim;
        GXColor col = Tone_Color(r, tone);
        float y0 = line_y + Note_At(r, k, HW_ROWS) * HW_PITCH + 0.04f;
        float y1 = line_y + (Note_At(r, j, HW_ROWS) + 1.f) * HW_PITCH - 0.04f;
        if (y0 >= top - 0.1f)
        {
            k = j;
            continue; // still off the far end
        }
        if (y1 > top)
            y1 = top;
        float yc = (y0 + y1) / 2, mid = (lx + rx) / 2;
        switch (kind)
        {
        case CELL_PRESS:
            Hud_Rect(lx + in, y0, rx - in, y1, Color_Fill(col, a));
            break;
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
            Hud_Rect(mid - 0.5f, y0, mid + 0.5f, y1, Color_Fill(col, a * 0.75f));
            break;
        case CELL_TAIL:
            Hud_Rect(lx + in, y0, rx - in, y1, Color_Fill(color_galint, a));
            break;
        }
        k = j;
    }
    for (int k = 0; k < len; k++)
    {
        float at = Note_At(r, k, HW_ROWS);
        if (r->glyph[k] && at < HW_ROWS - 0.5f)
            Glyph_Draw(r->glyph[k], glyph_x, line_y + (at + 0.5f) * HW_PITCH, 0.3f, Color_Fill(color_white, 0.95f * dim));
    }
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

// The window's frame, lit while r's press is due (white on its first or
// last frame), in r's ending colors as it fades.
static void Hw_WindowFrame(MeterRow *r, float lx, float rx, float line_y)
{
    float wx0 = lx - 0.12f, wx1 = rx + 0.12f, wy0 = line_y - 0.14f, wy1 = line_y + hw_pitch + 0.1f;
    int hot = r->dim ? 0 : r->flash >= 0 ? 2 : r->hot;
    int ending = !r->dim && r->implode >= 0;
    GXColor dc = r->implode_tone == TONE_MISS ? color_miss : color_skip;
    if (hot > 0)
        Hud_Rect(wx0 - 0.2f, wy0 - 0.2f, wx1 + 0.2f, wy1 + 0.2f, Color_Fill(r->color, 0.3f));
    GXColor fc = hot >= 2 ? color_white : hot ? Color_Mix(r->color, color_white, 0.4f)
                 : ending ? Color_Mix(dc, color_white, 0.3f)
                          : Color_Over(color_white, r->dim ? 0.3f : 0.6f);
    float t = hot >= 2 ? 0.16f : 0.1f;
    Hud_Frame(wx0, wy0, wx1, wy1, t, fc);
    Hud_Rect(wx0, wy0, wx1, wy0 + t + 0.06f, fc); // the floor of it, heavier
}

static void Hw_Window(MeterRow *r, float lx, float rx, float line_y)
{
    Hw_WindowEnd(r, lx, rx, line_y);
    Hw_WindowFrame(r, lx, rx, line_y);
}

// The highway (Fixed Strip): one vertical lane per row, notes falling onto a
// hit line at the bottom, a row a frame with every fifth one heavier, the
// label under the lane. The hit line lights while a press is due, and the
// ghost, burst and fold are the cells' at the line.
static void Highway_Draw(FighterData *fp)
{
    int rows = meter_rows;
    float x0, y0, w, h;
    Strip_Place(fp, STRIP_HIGHWAY, &x0, &y0, &w, &h);
    // lanes count from the screen's edge and the glyphs' column comes after
    // them, so neither a lane added nor the glyphs showing up moves any lane
    // (Stephen, 0.8.6: the lanes shifted when the markers came in)
    int right = x0 > 0;
    float glyph = Strip_Glyphs() ? HW_GLYPH : 0;
    float glyph_x = right ? x0 + STRIP_PAD + glyph / 2 : x0 + w - STRIP_PAD - glyph / 2;
    float lanes_l = x0 + STRIP_PAD + (right ? glyph : 0), lanes_r = x0 + w - STRIP_PAD - (right ? 0 : glyph);
    float line_y = y0 + HW_LABEL;
    float base_k = vis_k;

    vis_k = base_k * Strip_PlateK();
    Hud_Rect(x0, y0, x0 + w, y0 + h, Color_Fill(color_plate, 0.72f));
    // every fifth frame of the game's clock heavier, so the lines travel
    // down a row a frame, like the conveyor's
    for (int k = 1; k <= HW_ROWS; k++)
    {
        int five = (event_vars->game_timer + k) % 5 == 0;
        float y = line_y + k * HW_PITCH, e = five ? 0.1f : 0.05f;
        Hud_Rect(lanes_l - 0.1f, y - e, lanes_r + 0.1f, y + e, Color_Over(color_white, five ? 0.3f : 0.12f));
    }

    // the strikeline: under every lane's window, a tick out past each end
    // that stretches on the press frame
    int press = 0;
    for (int i = 0; i < rows; i++)
        if (!meter[i].gap && !meter[i].dim && (meter[i].flash >= 0 || meter[i].hot >= 2))
            press = 1;
    float tick = press ? 0.6f : 0.35f;
    GXColor sc = press ? color_white : Color_Over(color_white, 0.45f);
    // (not into the glyphs' column, where the next input's glyph sits)
    float tl = glyph > 0 && right ? 0 : tick, tr = glyph > 0 && !right ? 0 : tick;
    float my = line_y + HW_PITCH / 2;
    Hud_Rect(lanes_l - 0.12f - tl, line_y - 0.2f, lanes_r + 0.12f + tr, line_y - 0.12f, sc);
    if (tl > 0)
        Hud_Rect(lanes_l - 0.12f - tl, my - 0.06f, lanes_l - 0.2f, my + 0.06f, sc);
    if (tr > 0)
        Hud_Rect(lanes_r + 0.2f, my - 0.06f, lanes_r + 0.12f + tr, my + 0.06f, sc);

    for (int i = 0; i < rows; i++)
    {
        MeterRow *r = &meter[i];
        if (r->gap)
            continue;
        vis_k = base_k * r->fade;
        float lx = right ? lanes_r - (i + 1) * HW_LANE - i * HW_LANE_GAP : lanes_l + i * (HW_LANE + HW_LANE_GAP);
        float rx = lx + HW_LANE;
        Hud_Rect(lx, line_y, rx, line_y + HW_ROWS * HW_PITCH, Color_Over(color_white, 0.05f));
        Hw_Lane(r, glyph_x, lx, line_y);

        Hw_Window(r, lx, rx, line_y);
        Strip_Label(r, (lx + rx) / 2, y0 + 0.75f, HW_LANE - 0.3f);
    }
    vis_k = base_k;
}
