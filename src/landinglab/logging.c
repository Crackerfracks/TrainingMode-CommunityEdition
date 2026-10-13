/* landinglab/logging.c
 *
 * Holds the log: fighter attributes, frames, the stage's lines, other
 * fighters' ECB tables.
 *
 * Part of Landing Gear: src/landinglab.c includes it, in order, into one
 * translation unit, so it sees everything included before it (and mex.h,
 * events.h, landinglab_sim.h).
 */

///////////////////////
/// Logging         ///
///////////////////////

static void Log(char *buf)
{
    OSReport("%s", buf);
}

// A disc read into an aligned buffer of its own: the disc reads whole
// 32-byte blocks into 32-byte aligned memory.
static u8 *Disc_Read(int entry, int offset, int size)
{
    u8 *buf = (u8 *)(((u32)calloc(size + 32) + 31) & ~31);
    DCFlushRange(buf, size);
    File_ReadSync(entry, offset, buf, size, 0x21, 1);
    DCInvalidateRange(buf, size);
    return buf;
}

// Another fighter than Falcon: its own ECB table from TM/llecb.bin
// (captured in game by src/landinglab_capture.txt, packed by
// tools/llsim/ecbpack.py). Without one it learns in play.
// Read again when Zelda and Sheik change (Event_ThinkFrame), so each one's
// read is kept to come back to rather than read (and allocated) each time.
static void Ecb_LoadDisc(FighterData *fp)
{
    static const u8 *read[2];
    static int read_kind[2] = {-1, -1}, read_size[2];
    char buf[96];
    Sim_ForFighter(fp->kind);
    own_table = 0;
    own_size = 0;
    if (sim_falcon)
        return;
    int entry = DVDConvertPathToEntrynum("TM/llecb.bin"), offset, size, rows = 0;
    for (int i = 0; i < 2 && !own_table; i++)
    {
        if (read_kind[i] == fp->kind)
        {
            own_table = read[i];
            own_size = read_size[i];
        }
    }
    if (!own_table && entry >= 0)
    {
        u8 *dir = Disc_Read(entry, 0, LLE_DIR_BYTES);
        if (Ecb_Find(dir, fp->kind, &offset, &size) && offset % 32 == 0 && size % 32 == 0)
        {
            own_table = Disc_Read(entry, offset, size);
            own_size = size;
            int i = read_kind[0] < 0 ? 0 : 1;
            read[i] = own_table;
            read_kind[i] = fp->kind;
            read_size[i] = size;
        }
    }
    if (own_table)
        rows = Ecb_Unpack(own_table, own_size);
    sprintf(buf, "LLECB fighter %d: %d rows from TM/llecb.bin%s\n", fp->kind, rows, entry < 0 ? " (no file)" : "");
    Log(buf);
}

static void Log_Attributes(FighterData *fp)
{
    char buf[200];

    sprintf(buf, "LandingLab attrs: gravity %.5f terminal %.5f fastfall %.5f\n",
            fp->attr.gravity, fp->attr.terminal_velocity, fp->attr.fastfall_velocity);
    Log(buf);
    sprintf(buf, "LandingLab attrs: drift mult %.5f base %.5f max %.5f friction %.5f air max %.5f\n",
            fp->attr.aerial_drift_stick_mult, fp->attr.aerial_drift_base, fp->attr.aerial_drift_max,
            fp->attr.aerial_friction, fp->attr.horizontal_air_mobility_constant);
    Log(buf);
    sprintf(buf, "LandingLab attrs: jump h %.5f v %.5f hop v %.5f dj v mult %.5f dj h mult %.5f\n",
            fp->attr.jump_h_initial_velocity, fp->attr.jump_v_initial_velocity, fp->attr.hop_v_initial_velocity,
            fp->attr.air_jump_v_multiplier, fp->attr.air_jump_h_multiplier);
    Log(buf);
    sprintf(buf, "LandingLab attrs: landing lag %.1f nair %.1f fair %.1f bair %.1f uair %.1f dair %.1f\n",
            fp->attr.normal_landing_lag, fp->attr.n_air_landing_lag, fp->attr.f_air_landing_lag,
            fp->attr.b_air_landing_lag, fp->attr.u_air_landing_lag, fp->attr.d_air_landing_lag);
    Log(buf);
    sprintf(buf, "LandingLab common: NIL above vel.y %.5f, fastfall stick %.4f window %d, platform drop stick %.4f, L-cancel window %d div %.2f\n",
            Fighter_GetSoftLandVelocity(fp), common_fastfall_stick, common_fastfall_window,
            common_platform_drop, common_lcancel_window, common_lcancel_div);
    Log(buf);
    sprintf(buf, "LandingLab common: nair below stick x %.4f y %.4f, uair/dair above %.4f rad\n",
            common_aerial_stick_x, common_aerial_stick_y, common_aerial_angle);
    Log(buf);
    sprintf(buf, "LandingLab common: shield drop stick %.4f window %d (raw %x), spotdodge stick %.4f window %d\n",
            common_drop_stick, common_drop_window, Common_Int(COMMON_DROP_WINDOW), common_spot_stick, common_spot_window);
    Log(buf);
    sprintf(buf, "LandingLab common: fall lean deadzone %.5f rate %.5f, max jumps %d\n",
            common_fall_lean_deadzone, common_fall_lean_rate, fp->attr.max_jumps);
    Log(buf);
}

static void Log_Frame(FighterData *fp, int ts, int frame)
{
    char buf[256];
    CollData *cd = &fp->coll_data;

    sprintf(buf, "LL %d %s f%d pos %.4f %.4f vel %.5f %.5f ff%d lock%d ecb top %.4f bot %.4f l %.4f r %.4f side %.4f used bot %.4f ac%d stick %.4f %.4f face %d\n",
            event_vars->game_timer, tracked_state_names[ts], frame,
            fp->phys.pos.X, fp->phys.pos.Y, fp->phys.self_vel.X, fp->phys.self_vel.Y,
            fp->flags.is_fastfall, cd->u.ecb_bot_lock_frames,
            cd->ecbCurr_top.Y, cd->ecbCurr_bot.Y, cd->ecbCurr_left.X, cd->ecbCurr_right.X, cd->ecbCurr_right.Y,
            cd->ecbCurrCorrect_bot.Y, fp->ftcmd_var.flag0 != 0,
            fp->input.lstick.X, fp->input.lstick.Y, fp->facing_direction > 0 ? 1 : -1);
    Log(buf);

    // a fall leans its pose toward FallF or FallB with the drift speed
    // (ftCo_Fall_Anim_Inner): which one, and how far
    if (ts == TS_FALL || ts == TS_FALLAERIAL || ts == TS_FALLSPECIAL)
    {
        float weight;
        memcpy(&weight, &fp->state_var.state_var2, sizeof(weight));
        sprintf(buf, "LLLEAN %d sm %d w %.5f\n", event_vars->game_timer, fp->state_var.state_var1, weight);
        Log(buf);
    }

    // Falcon Dive: its own drift speed and flags, to check the learned
    // animation movement against
    if (Tracked_IsUpB(ts))
    {
        Vec2 drift = Upb_Drift(fp);
        sprintf(buf, "LLUPB %d drift %.5f %.5f flags %02x\n", event_vars->game_timer, drift.X, drift.Y,
                ((u8 *)&fp->state_var)[UPB_FLAGS]);
        Log(buf);
    }
}

// A jumpsquat frame: animation frame, position, ground speed, stick and
// buttons, so a ground preview can be replayed.
static void Log_Squat(FighterData *fp)
{
    char buf[200];
    sprintf(buf, "LLSQ %d f%.2f pos %.4f %.4f gvel %.5f stick %.4f %.4f held %x face %d\n",
            event_vars->game_timer, fp->state.frame, fp->phys.pos.X, fp->phys.pos.Y,
            fp->phys.self_vel_ground.X, fp->input.lstick.X, fp->input.lstick.Y, fp->input.held,
            fp->facing_direction > 0 ? 1 : -1);
    Log(buf);
}

// Every collision line of the stage, so a wrong prediction near a wall or
// ledge can be replayed exactly: id, kind flags, ends, previous and next
// line (and the other group's, if any).
static void Log_Stage(void)
{
    char buf[160];
    RawCollLine *lines = (RawCollLine *)*stc_collline;
    CollVert *verts = *stc_collvert;

    for (CollGroup *group = *stc_firstcollgroup; group != 0; group = group->next)
    {
        CollGroupDesc *d = group->desc;
        int starts[5] = {d->floor_start, d->ceil_start, d->rwall_start, d->lwall_start, d->dyn_start};
        int nums[5] = {d->floor_num, d->ceil_num, d->rwall_num, d->lwall_num, d->dyn_num};
        for (int r = 0; r < 5; r++)
        {
            for (int i = 0; i < nums[r]; i++)
            {
                int id = starts[r] + i;
                CollLineDesc *desc = lines[id].desc;
                Vec2 *v0 = &verts[(u16)desc->vert_prev].pos_curr;
                Vec2 *v1 = &verts[(u16)desc->vert_next].pos_curr;
                sprintf(buf, "LLLINE %d flags %x %.4f %.4f %.4f %.4f prev %d next %d alt %d %d plat %d\n",
                        id, lines[id].flags, v0->X, v0->Y, v1->X, v1->Y,
                        desc->line_prev, desc->line_next, desc->line_prev_altgroup, desc->line_next_altgroup,
                        desc->is_unk);
                Log(buf);
            }
        }
    }
}
