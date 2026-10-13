/* landinglab/dial.c
 *
 * Holds the Dial strip, and the fixed strip's dispatch (Meter_Fixed).
 *
 * Part of Landing Gear: src/landinglab.c includes it, in order, into one
 * translation unit, so it sees everything included before it (and mex.h,
 * events.h, landinglab_sim.h).
 *
 * Uses: menus_display, timers, drawing, hud_drawing, meter, meter_placement,
 * highway, conveyor.
 */

static int Dial_Target(int kind)
{
    return kind == CELL_PRESS || kind == CELL_LAND || kind == CELL_ACT;
}

// One dial for a row: the first run of cells that matter (a press, a
// touchdown, the first frame Falcon can act) is its wedge at twelve
// o'clock, and the hand sweeps clockwise a frame at a time into it, with
// ticks for the last six frames. A hand more than 21 frames out waits
// hidden. The ledge intangibility after the wedge is a band on the rim.
static void Dial_One(MeterRow *r, float cx, float cy)
{
    float R = DL_R, dim = r->dim ? 0.5f : 1.f;
    int a = -1, n = 0, tail = 0;
    for (int k = 0; k < r->len && k < MT_CELLS; k++)
    {
        if (Dial_Target(r->cell[k]))
        {
            a = k;
            break;
        }
    }
    if (a >= 0)
    {
        for (n = 1; a + n < r->len && a + n < MT_CELLS && n < 24 && Dial_Target(r->cell[a + n]); n++)
            ;
        while (a + n + tail < r->len && a + n + tail < MT_CELLS && n + tail < 24 && r->cell[a + n + tail] == CELL_TAIL)
            tail++;
    }

    Dial_Fan(cx, cy, R + 0.35f, 0, 360, Color_Fill(color_plate, 0.8f));
    Dial_Band(cx, cy, R - 0.05f, R + 0.05f, 0, 360, Color_Over(color_white, 0.2f * dim));

    // the game's clock round the rim: a tick between every two frames,
    // heavier before every fifth. They hold still while a window waits,
    // so the hand passes them at the game's speed, and turn a slice a
    // frame while none does, so the time going by always shows.
    int at = event_vars->game_timer + (a >= 0 ? a - r->spent : 0); // the wedge's frame
    for (int d = 0; d < (int)(360.f / DL_DEG); d++)
    {
        int five = (at - d) % 5 == 0;
        float deg = -d * DL_DEG, x0, y0, x1, y1;
        Dial_Pt(cx, cy, R + 0.08f, deg, &x0, &y0);
        Dial_Pt(cx, cy, R + (five ? 0.3f : 0.18f), deg, &x1, &y1);
        Hud_Seg(x0, y0, x1, y1, five ? 0.1f : 0.06f, Color_Over(color_white, (five ? 0.5f : 0.22f) * dim));
    }

    if (a >= 0)
    {
        int tone = r->tone[a];
        float al = r->alpha[a] / 255.f * dim;
        GXColor col = Tone_Color(r, tone);
        int hot = r->flash >= 0 ? 2 : r->hot;
        float span = (n + r->spent) * DL_DEG;
        if (span > 360.f)
            span = 360.f;
        GXColor wc = hot >= 2 && tone == TONE_CUE ? Color_Mix(col, color_white, 0.6f) : col;
        Dial_Fan(cx, cy, R - 0.05f, 0, span, Color_Over(wc, (hot ? 1.f : 0.75f) * al));
        if (tail > 0 && span < 360.f)
        {
            float end = span + tail * DL_DEG;
            Dial_Band(cx, cy, R - 0.38f, R - 0.1f, span, end > 360.f ? 360.f : end, Color_Over(color_galint, 0.85f * al));
        }

        // ticks for the last six frames, where the hand will be (not on the
        // routes that aren't the chosen one)
        for (int d = 1; d <= 6 && !r->dim; d++)
        {
            float deg = (0.5f - d) * DL_DEG, x0, y0, x1, y1;
            Dial_Pt(cx, cy, R - 0.4f, deg, &x0, &y0);
            Dial_Pt(cx, cy, R, deg, &x1, &y1);
            Hud_Seg(x0, y0, x1, y1, 0.1f, Color_Over(color_white, 0.6f * al));
        }

        // the hand, in the middle of its frame's slice
        if (a <= 21)
        {
            float deg = (r->spent - a + 0.5f) * DL_DEG, hx, hy;
            Dial_Pt(cx, cy, R + 0.2f, deg, &hx, &hy);
            GXColor hc = tone == TONE_CUE ? Color_Mix(r->color, color_white, hot ? 1.f : 0.55f) : Tone_Color(r, tone);
            Hud_Seg(cx, cy, hx, hy, 0.34f, Color_Over(color_plate, 0.5f * al));
            Hud_Seg(cx, cy, hx, hy, 0.2f, Color_Over(hc, al));
        }
    }
    if (r->implode >= 0)
    {
        // the frame that was due, kept in the wedge, fading; a miss's split
        float q = (float)r->implode / LL_FADE, a = 0.9f * (1.f - q);
        if (r->implode_tone == TONE_MISS)
        {
            float d = 4.f * Ease_Out(q);
            Dial_Fan(cx, cy, R - 0.05f, -d, DL_DEG * 0.5f - 1.f - d, Color_Fill(color_miss, a));
            Dial_Fan(cx, cy, R - 0.05f, DL_DEG * 0.5f + 1.f + d, DL_DEG + d, Color_Fill(color_miss, a));
        }
        else
            Dial_Fan(cx, cy, R - 0.05f, 0, DL_DEG, Color_Fill(color_skip, a * 0.8f));
    }
    if (!r->dim)
        Strip_Ending(r, cx, cy, R, R, 1.f, 1);
}

// The dial (Fixed Strip): a small dial for each row, side by side, the label
// under it.
static void Dial_Draw(FighterData *fp)
{
    float x0, y0, w, h;
    Strip_Place(fp, STRIP_DIAL, &x0, &y0, &w, &h);
    int right = x0 > 0; // counted from the screen's edge, like the highway's lanes
    float base_k = vis_k;
    for (int i = 0; i < meter_rows; i++)
    {
        MeterRow *r = &meter[i];
        if (r->gap)
            continue;
        vis_k = base_k * r->fade;
        float off = STRIP_PAD + DL_R + 0.35f + i * DL_CELL;
        float cx = right ? x0 + w - off : x0 + off, cy = y0 + DL_LABEL + DL_R + 0.45f;
        Dial_One(r, cx, cy);
        Strip_Label(r, cx, y0 + 0.75f, DL_CELL - 0.5f);
    }
    vis_k = base_k;
}

// The fixed strip: bottom left, or bottom right when the controller display
// is on the left half of the screen. Cells, or the highway or dial in a
// corner.
static void Meter_Fixed(FighterData *fp)
{
    int look = Options_Timers[TOPT_STRIP].val;
    if (look == STRIP_HIGHWAY)
    {
        Highway_Draw(fp);
        return;
    }
    if (look == STRIP_DIAL)
    {
        Dial_Draw(fp);
        return;
    }
    if (look == STRIP_CONVEYOR)
    {
        Conveyor_Draw(fp);
        return;
    }
    float gx, base;
    Strip_CellsAt(fp, &gx, &base);
    Meter_Draw(gx, base, MT_FIXED, MT_FIXED_MAX, 1);
}
