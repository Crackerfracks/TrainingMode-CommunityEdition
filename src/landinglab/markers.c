/* landinglab/markers.c
 *
 * Holds the input markers along the paths, and the aerial pickers
 * (Compass_Draw).
 *
 * Part of Landing Lab: src/landinglab.c includes it, in order, into one
 * translation unit, so it sees everything included before it (and mex.h,
 * events.h, landinglab_sim.h).
 *
 * Uses: option_values, menus_display, tracking_state, hud_text, drawing,
 * hud_drawing, meter, meter_placement, debug_camera, ledge_routes,
 * jump_timing, jump_tracking.
 */

// The inputs along the paths, where each is due: a chip beside the line
// with the stick's direction, the jump, the aerial or the airdodge, joined
// to its frame by a thin leader. Chips that would overlap stack downward in
// order, so the quick inputs at the ledge stay apart. On the HUD, so they
// stay on top of everything and keep their size.
#define MK_MAX 6
#define MK_H 1.2f   // chip height, at Marker Size 1
#define MK_GW 1.0f  // width per glyph
#define MK_GAP 0.25f

static float Marker_Scale(void)
{
    return mark_sizes[Options_Paths[POPT_MARK_SIZE].val];
}

typedef struct Marker
{
    float px, py; // the frame's point
    float cx, cy; // the chip's middle
    float w;
    int n;
    int due; // frames to its press, 0 the next one; -1 not counted
    u8 glyph[3];
    GXColor color[3];
} Marker;

static Marker mk[MK_MAX];
static int mk_num;

static Marker *Marker_Add(float wx, float wy)
{
    if (mk_num >= MK_MAX)
        return 0;
    Marker *m = &mk[mk_num];
    if (!Hud_FromWorld(wx, wy, &m->px, &m->py))
        return 0;
    m->n = 0;
    m->due = -1;
    mk_num++;
    return m;
}

static void Marker_Due(Marker *m, int due)
{
    if (m)
        m->due = due;
}

static void Marker_Glyph(Marker *m, int glyph, GXColor c)
{
    if (m && glyph && m->n < 3)
    {
        m->glyph[m->n] = glyph;
        m->color[m->n++] = c;
    }
}

// The chip placed before this one that it would overlap, -1 none.
static int Marker_Hit(int i, float h, float gap)
{
    Marker *m = &mk[i];
    for (int j = 0; j < i; j++)
    {
        Marker *o = &mk[j];
        if (fabs(o->cx - m->cx) < (o->w + m->w) / 2 + gap && fabs(o->cy - m->cy) < h + gap)
            return j;
    }
    return -1;
}

// side: -1 puts the chips left of the line, 1 right.
static void Markers_Flush(int side)
{
    float S = Marker_Scale();
    float h = MK_H * S, gw = MK_GW * S, gap = MK_GAP * S;
    for (int i = 0; i < mk_num; i++)
    {
        Marker *m = &mk[i];
        m->w = m->n * gw + 0.3f * S;
        m->cx = m->px + side * (1.3f * S + m->w / 2);
        float lim = SAFE_W - m->w / 2;
        if (m->cx > lim)
            m->cx = lim;
        if (m->cx < -lim)
            m->cx = -lim;
        // stacked down out of each other's way, or up once that would
        // leave the screen, and kept on it
        float low = -SAFE_H + h / 2, high = SAFE_H - h / 2;
        int dir = -1;
        m->cy = m->py;
        for (int tries = 0, j; tries < 2 * MK_MAX && (j = Marker_Hit(i, h, gap)) >= 0; tries++)
        {
            m->cy = mk[j].cy + dir * (h + gap);
            if (dir < 0 && m->cy < low)
            {
                dir = 1;
                m->cy = m->py;
            }
        }
        if (m->cy < low)
            m->cy = low;
        if (m->cy > high)
            m->cy = high;
    }
    for (int i = 0; i < mk_num; i++)
    {
        Marker *m = &mk[i];
        float ex = m->cx - side * m->w / 2; // the chip's edge facing the line
        Hud_Seg(m->px, m->py, ex, m->cy, 0.1f, Color_Fill(color_plate, 0.7f));
        Hud_Seg(m->px, m->py, ex, m->cy, 0.05f, Color_Fill(color_white, 0.6f));
        Hud_Diamond(m->px, m->py, 0.3f, Color_Fill(color_plate, 0.8f));
        Hud_Diamond(m->px, m->py, 0.2f, m->color[0]);
    }
    for (int i = 0; i < mk_num; i++)
    {
        Marker *m = &mk[i];
        float x0 = m->cx - m->w / 2, x1 = m->cx + m->w / 2, y0 = m->cy - h / 2, y1 = m->cy + h / 2;
        // a chip counts in like the timers' gates: its frame brightens over
        // the last frames, and on the press it fills with its color in a
        // white frame, the same flash as a lane's gate
        int due = m->due;
        float lead = due >= 1 && due <= 6 ? (7 - due) / 6.f : 0;
        Hud_Rect(x0, y0, x1, y1, due == 0 ? Color_Fill(m->color[0], 0.9f) : Color_Fill(color_plate, 0.85f));
        Hud_Frame(x0, y0, x1, y1, (0.07f + 0.08f * lead) * S, Color_Fill(m->color[0], 0.7f + 0.3f * lead));
        if (due == 0)
            Hud_Frame(x0 - 0.12f * S, y0 - 0.12f * S, x1 + 0.12f * S, y1 + 0.12f * S, 0.14f * S, color_white);
        for (int g = 0; g < m->n; g++)
            Glyph_Draw(m->glyph[g], x0 + 0.15f * S + gw * (g + 0.5f), m->cy, 0.36f * S,
                       due == 0 ? color_plate : m->color[g]);
    }
    mk_num = 0;
}

// The aerial picker: at the start of an aerial interrupt's window, a small
// C-stick face with the directions that interrupt there lit (the middle for
// a nair), forward turned the way Falcon faces. Queued from the stage's
// drawing and drawn on the HUD, above its frame.
static void Compass_Draw(void)
{
    float S = Marker_Scale();
    float r = 0.95f * S;
    for (int i = 0; i < compass_num; i++)
    {
        Compass *c = &compass[i];
        float px, py;
        if (!Hud_FromWorld(c->x, c->y, &px, &py))
            continue;
        float cx = px, cy = py + 1.2f * S + r;
        Hud_Seg(px, py, cx, cy - r, 0.1f, Color_Fill(color_plate, 0.7f));
        Hud_Seg(px, py, cx, cy - r, 0.05f, Color_Fill(c->color, 0.7f));
        Hud_Disc(cx, cy, r + 0.15f * S, Color_Fill(color_plate, 0.85f));
        Hud_Ring(cx, cy, r, 0.08f * S, Color_Fill(c->color, 0.6f));
        GXColor off = Color_Fill(color_white, 0.12f);
        // N F B U D, as the aerial bits
        static const float dirs[5][2] = {{0, 0}, {1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (int a = 0; a < 5; a++)
        {
            GXColor col = (c->mask & (1 << a)) ? c->color : off;
            if (a == 0)
            {
                Hud_Disc(cx, cy, 0.22f * r, col);
                continue;
            }
            float dx = dirs[a][0] * c->facing, dy = dirs[a][1];
            float tx = cx + dx * 0.9f * r, ty = cy + dy * 0.9f * r;     // tip
            float bx = cx + dx * 0.38f * r, by = cy + dy * 0.38f * r;   // base
            float wx = -dy * 0.36f * r, wy = dx * 0.36f * r;
            Hud_Tri(tx, ty, bx + wx, by + wy, bx - wx, by - wy, col);
        }
    }
    compass_num = 0;
}

// The C-stick direction of an aerial, nothing for nair.
static int Aerial_Glyph(int aerial, int facing)
{
    switch (aerial)
    {
    case TS_AIRF:
        return facing > 0 ? GLYPH_RIGHT : GLYPH_LEFT;
    case TS_AIRB:
        return facing > 0 ? GLYPH_LEFT : GLYPH_RIGHT;
    case TS_AIRHI:
        return GLYPH_UP;
    case TS_AIRLW:
        return GLYPH_DOWN;
    }
    return 0;
}

static void Route_Marker(int frame, int g0, GXColor c0, int g1, GXColor c1)
{
    int i = frame < route_path_num ? frame : route_path_num - 1;
    Marker *m = Marker_Add(route_path[i].X, route_path[i].Y + route_path_bottom[i]);
    Marker_Glyph(m, g0, c0);
    Marker_Glyph(m, g1, c1);
    if (route_active)
        Marker_Due(m, frame - route_e - 1);
}

static void Markers_Draw(void)
{
    if (!Options_Paths[POPT_INPUTS].val || capture_clean)
        return;
    mk_num = 0;

    // a ledge route: the inputs still to come
    int route = (hang_ledge >= 0 && route_show_num > 0) || (route_active && !route_dj_done);
    if (route && Routes_On() && route_path_num >= 2 && route_path_ledge >= 0)
    {
        LedgeRoute *r = &route_path_of;
        int facing = ledges[route_path_ledge].facing;
        int from = route_active ? route_e + 1 : 0;
        int away = facing > 0 ? GLYPH_LEFT : GLYPH_RIGHT;
        int in = facing > 0 ? GLYPH_RIGHT : GLYPH_LEFT;
        int ff_away = facing > 0 ? GLYPH_FF_DOWN_LEFT : GLYPH_FF_DOWN_RIGHT;
        if (from <= 0)
            Route_Marker(0, r->drop == DROP_AWAY ? away : GLYPH_DOWN, color_white,
                         r->fall_away && r->drop != DROP_AWAY ? away : 0, color_white);
        if (r->ff > 0 && Route_FF(r) >= from)
            Route_Marker(Route_FF(r), r->fall_away ? ff_away : GLYPH_FF, color_white, 0, color_white);
        if (r->dj >= from)
        {
            int dj_glyph = r->dj_x == LR_DJ_IN     ? in
                           : r->dj_x == LR_DJ_DIAG ? (facing > 0 ? GLYPH_DOWN_RIGHT : GLYPH_DOWN_LEFT)
                                                   : 0;
            Route_Marker(r->dj, GLYPH_JUMP, color_in_jump, dj_glyph, color_white);
        }
        if (r->kind == LAND_AI && r->press >= from)
            Route_Marker(r->press, GLYPH_AERIAL, color_in_aerial, Aerial_Glyph(r->aerial, facing), color_in_aerial);
        // beside the line on the stage's side, clear of Falcon hanging
        // and dropping off it
        Markers_Flush(facing > 0 ? 1 : -1);
        return;
    }

    // a jump: the stick and the jump where it is due, then the aerial
    if (Jump_Showing() && jt_path_num >= 2)
    {
        JumpRoute *r = &jt_route;
        int e = Jump_E();
        int i;
        Marker *m;
        if (r->hop && r->hp > e)
        {
            // the hop: the jump alone for a short hop, held (up) for a full one
            i = r->hp < jt_path_num ? r->hp : jt_path_num - 1;
            m = Marker_Add(jt_path[i].X, jt_path[i].Y + jt_path_bottom[i]);
            Marker_Glyph(m, GLYPH_JUMP, color_in_jump);
            Marker_Due(m, r->hp - e - 1);
            if (r->hop == GT_FH)
                Marker_Glyph(m, GLYPH_UP, color_white);
        }
        if (!r->hop || r->hj >= 0)
        {
            i = r->dj < jt_path_num ? r->dj : jt_path_num - 1;
            m = Marker_Add(jt_path[i].X, jt_path[i].Y + jt_path_bottom[i]);
            Marker_Glyph(m, GLYPH_JUMP, color_in_jump);
            if (!(jt_ground && gt_still))
                Marker_Due(m, r->dj - e - 1);
            if (r->stick <= JT_UPRIGHT)
                Marker_Glyph(m, GLYPH_UP, color_white);
            if (r->stick == JT_LEFT || r->stick == JT_UPLEFT)
                Marker_Glyph(m, GLYPH_LEFT, color_white);
            if (r->stick == JT_RIGHT || r->stick == JT_UPRIGHT)
                Marker_Glyph(m, GLYPH_RIGHT, color_white);
        }
        if (r->kind == LAND_AI && r->press > e)
        {
            i = r->press < jt_path_num ? r->press : jt_path_num - 1;
            m = Marker_Add(jt_path[i].X, jt_path[i].Y + jt_path_bottom[i]);
            Marker_Glyph(m, GLYPH_AERIAL, color_in_aerial);
            Marker_Glyph(m, Aerial_Glyph(r->aerial, Jump_Facing(r) > 0 ? 1 : -1), color_in_aerial);
            if (!(jt_ground && gt_still))
                Marker_Due(m, r->press - e - 1);
        }
        // on the side the path came from, out of its way
        Markers_Flush(jt_path[jt_path_num - 1].X >= jt_path[0].X ? -1 : 1);
    }

    // the next waveland's airdodge, while its timer runs
    Cue *c = &cue_live[CUE_WL];
    Prediction *p = pred_live;
    if (live_visible && Cues_Waveland() && (c->phase == PH_COUNT || c->phase == PH_WINDOW) && !c->dim && !c->wd &&
        p->wl_first > 0 && p->wl_first < p->uncertain_from)
    {
        int k = p->wl_first - 1; // where Falcon is as he presses it
        Marker *m = Marker_Add(p->pos[k].X, p->pos[k].Y + p->bottom[k]);
        Marker_Glyph(m, GLYPH_DODGE, color_in_dodge);
        Marker_Due(m, c->phase == PH_WINDOW ? 0 : c->left - 1); // with the timer's count
        if (p->wl_dirs & DODGE_LEFT)
            Marker_Glyph(m, GLYPH_LEFT, color_white);
        if (p->wl_dirs & DODGE_RIGHT)
            Marker_Glyph(m, GLYPH_RIGHT, color_white);
        // on the side the path came from, out of its way
        int last = p->land_frame ? p->land_frame : p->num;
        Markers_Flush(p->pos[last].X >= p->pos[0].X ? -1 : 1);
    }
}
