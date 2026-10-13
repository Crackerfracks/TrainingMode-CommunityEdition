/* landinglab/slide_off.c
 *
 * Holds the slide-off preview.
 *
 * Part of Landing Lab: src/landinglab.c includes it, in order, into one
 * translation unit, so it sees everything included before it (and mex.h,
 * events.h, landinglab_sim.h).
 *
 * Uses: menus_display, tracking_state, hud_text, drawing.
 */

///////////////////////
/// Slide-off       ///
///////////////////////

// Where a waveland or wavedash goes when its slide runs off the floor's
// end: friction along the floor to the edge, then falling. Before the
// press it assumes full stick that way; while sliding, the stick held now.
#define SLIDE_MAX 80
static Vec2 *slide_pos;     // [2][SLIDE_MAX], left side first
static float *slide_bottom; // [2][SLIDE_MAX]
static int slide_num[2];

static void Slide_Path(FighterData *fp, int side, float x, float y, float v, float edge, float stick_x, float stick_y)
{
    Vec2 *pos = &slide_pos[side * SLIDE_MAX];
    float *bottom = &slide_bottom[side * SLIDE_MAX];
    int n = 0;
    slide_num[side] = 0;
    pos[n] = (Vec2){x, y};
    bottom[n++] = 0;

    int off = 0;
    for (int i = 0; i < 120 && v != 0 && n < SLIDE_MAX; i++)
    {
        float friction = fp->attr.ground_friction;
        if (fabs(v) > fp->attr.walk_maximum_velocity)
            friction *= common_run_friction;
        if (fabs(v) <= friction)
            v = 0;
        else
            v += v > 0 ? -friction : friction;
        x += v;
        if ((v > 0 && x > edge) || (v < 0 && x < edge))
        {
            off = 1;
            break;
        }
        pos[n] = (Vec2){x, y};
        bottom[n++] = 0;
    }
    if (!off)
        return;

    // off the edge: falling with the slide's speed
    EcbSample *e = Ecb_Get(TS_FALL, 0);
    SimStart st;
    memset(&st, 0, sizeof(st));
    st.pos = (Vec2){edge, y};
    st.vel = (Vec2){v, 0};
    st.stick_x = stick_x;
    st.stick_y = stick_y;
    st.facing = fp->facing_direction;
    st.bottom = e->has_bottom ? e->bottom : 2.f;
    st.locked_bottom = st.bottom;
    st.ecb = (SimEcb){st.bottom, st.bottom + 16.f, st.bottom + 8.f, -3.f, 3.f};
    if (e->has_shape)
        Ecb_FromSample(e, st.facing, &st.ecb);
    Ecb_Fix(&st.ecb);
    st.ts = TS_FALL;
    st.frame = -1;
    st.tilt_timer = LL_TIMER_MAX;
    st.trigger_timer = LL_TIMER_MAX;
    st.skip_line = -1;
    st.jumps = 1; // leaving the ground uses one (ftCommon_8007D5D4)
    SimState s;
    SimStep step;
    Sim_Init(&st, &s);
    pos[n] = (Vec2){edge, y};
    bottom[n++] = 0;
    for (int i = 0; i < 60 && n < SLIDE_MAX; i++)
    {
        Sim_Step(fp, &st, &s, -1, 0, &step);
        pos[n] = (Vec2){s.x, s.y};
        bottom[n++] = s.bottom;
        if (step.landed || s.y < y - 80.f)
            break;
    }
    slide_num[side] = n;
}

static void Slide_Update(FighterData *fp)
{
    slide_num[0] = 0;
    slide_num[1] = 0;
    if (!Options_Paths[POPT_SLIDEOFF].val || !Cues_Waveland())
        return;
    Cue *c = &cue_live[CUE_WL], *e = &cue_end[CUE_WL];
    float reach = Slide_Distance(fp, common_dodge_force);
    if ((c->phase == PH_COUNT || c->phase == PH_WINDOW) && !c->dim)
    {
        Floor_BuildCache();
        if ((c->wd || (c->dirs & DODGE_LEFT)) && c->spot.X - reach < c->x0)
            Slide_Path(fp, 0, c->spot.X, c->spot.Y, -common_dodge_force, c->x0, -1.f, 0);
        if ((c->wd || (c->dirs & DODGE_RIGHT)) && c->spot.X + reach > c->x1)
            Slide_Path(fp, 1, c->spot.X, c->spot.Y, common_dodge_force, c->x1, 1.f, 0);
    }
    else if (e->phase == PH_HIT && fp->phys.air_state == 0 && fp->state_id == ASID_LANDINGFALLSPECIAL)
    {
        float v = fp->phys.self_vel_ground.X;
        if (v != 0)
        {
            Floor_BuildCache();
            Slide_Path(fp, v > 0, fp->phys.pos.X, fp->phys.pos.Y, v, v > 0 ? e->x1 : e->x0, fp->input.lstick.X, fp->input.lstick.Y);
        }
    }
}

static void Draw_SlideOff(void)
{
    if (!Options_Paths[POPT_SLIDEOFF].val)
        return;
    GXColor c = Color_Fill(Cue_Color(CUE_WL), 0.85f);
    for (int side = 0; side < 2; side++)
    {
        if (slide_num[side] >= 2)
            Draw_Dashed(&slide_pos[side * SLIDE_MAX], &slide_bottom[side * SLIDE_MAX], 0, slide_num[side] - 1, c, 18);
    }
}
