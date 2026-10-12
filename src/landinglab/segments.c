/* landinglab/segments.c
 *
 * Holds input segments: when the player changes input, and drift from the
 * prediction.
 *
 * Part of Landing Lab: src/landinglab.c includes it, in order, into one
 * translation unit, so it sees everything included before it (and mex.h,
 * events.h, landinglab_sim.h).
 *
 * Uses: tracking_state, logging.
 */

///////////////////////
/// Segments        ///
///////////////////////

static int Stick_Down(float y)
{
    return y <= -common_fastfall_stick;
}

static int Stick_Drop(float y)
{
    return y <= common_platform_drop;
}

// The player did something "keep holding" didn't assume.
static int Segment_InputChanged(FighterData *fp, int ts)
{
    if (!seg_valid)
        return 1;
    if (event_vars->game_timer - seg_start_timer >= LL_SIM_FRAMES)
        return 1;
    if (fp->input.lstick.X != seg_stick_x)
        return 1;
    if (Stick_Down(fp->input.lstick.Y) != seg_stick_down)
        return 1;
    if (Stick_Drop(fp->input.lstick.Y) != seg_stick_drop)
        return 1;
    if ((u8)fp->input.timer_lstick_tilt_y < prev_tilt_timer) // a new flick
        return 1;
    if (ts != prev_ts && !Tracked_EndedInto(prev_ts, ts)) // new aerial, double jump, ...
        return 1;
    return 0;
}

static void Segment_Start(FighterData *fp)
{
    memcpy(pred_seg, pred_live, sizeof(*pred_seg));
    seg_valid = 1;
    seg_start_timer = event_vars->game_timer;
    seg_drift_logged = 0;
    seg_stick_x = fp->input.lstick.X;
    seg_stick_down = Stick_Down(fp->input.lstick.Y);
    seg_stick_drop = Stick_Drop(fp->input.lstick.Y);
    actual_num = 0;
}

static void Segment_AddActual(FighterData *fp)
{
    int k = event_vars->game_timer - seg_start_timer;
    if (k < 0 || k > LL_SIM_FRAMES || k > actual_num)
        return;
    actual_pos[k].X = fp->phys.pos.X;
    actual_pos[k].Y = fp->phys.pos.Y;
    actual_bottom[k] = fp->coll_data.ecbCurrCorrect_bot.Y;
    actual_num = k + 1;
}

// Log the first frame where the real path leaves the predicted one while the
// input is unchanged. That points at a gap in the model.
static void Segment_CheckDrift(FighterData *fp, int ts)
{
    int k = event_vars->game_timer - seg_start_timer;
    if (seg_drift_logged || k < 1 || k > pred_seg->num)
        return;

    float dx = fp->phys.pos.X - pred_seg->pos[k].X;
    float dy = fp->phys.pos.Y - pred_seg->pos[k].Y;
    if (fabs(dx) < 0.001f && fabs(dy) < 0.001f && pred_seg->ts[k] == ts)
        return;

    char buf[200];
    sprintf(buf, "LandingLab drift at +%d: predicted %s %.4f %.4f, actual %s %.4f %.4f vel %.5f %.5f\n",
            k, tracked_state_names[(int)pred_seg->ts[k]], pred_seg->pos[k].X, pred_seg->pos[k].Y,
            tracked_state_names[ts], fp->phys.pos.X, fp->phys.pos.Y,
            fp->phys.self_vel.X, fp->phys.self_vel.Y);
    Log(buf);
    seg_drift_logged = 1;
}
