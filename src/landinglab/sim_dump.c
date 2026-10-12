/* landinglab/sim_dump.c
 *
 * Holds the data dumps for tools/llsim (dump, dumpecb), the capture hover,
 * and the lean and double jump capture logs.
 *
 * Part of Landing Lab: src/landinglab.c includes it, in order, into one
 * translation unit, so it sees everything included before it (and mex.h,
 * events.h, landinglab_sim.h).
 *
 * Uses: tracking_state, scripts.
 */

// Script command "dump": what the PC build of the sim (tools/llsim) needs
// from the game, to the log. The common fighter table, Falcon's attributes
// and special attributes as raw words, the soft landing speed, and the
// stage's collision: each group's ranges, each line's description (its two
// flag bytes raw) and flags, and each vertex. tools/llsim/dump.py reads it.
static void Dump_Words(const char *what, void *data, int bytes)
{
    char buf[200];
    for (int at = 0; at < bytes; at += 32)
    {
        char *t = buf + sprintf(buf, "LLDUMP %s %x", what, at);
        for (int i = at; i < at + 32 && i < bytes; i += 4)
            t += sprintf(t, " %08x", *(u32 *)((u8 *)data + i));
        OSReport("%s\n", buf);
    }
}

static u32 Float_Bits(float f)
{
    union
    {
        float f;
        u32 u;
    } v = {f};
    return v.u;
}

static Prediction *pred_route; // (Ledge routes, below)

static void Sim_Dump(FighterData *fp)
{
    float soft = Fighter_GetSoftLandVelocity(fp);
    OSReport("LLDUMP begin stage %d char %d\n", Stage_GetExternalID(), fp->kind);
    Dump_Words("common", *stc_ftcommon, 0x480);
    Dump_Words("attr", &fp->attr, sizeof(fp->attr));
    Dump_Words("special", fp->special_attributes, 0x60);
    OSReport("LLDUMP softland %08x\n", Float_Bits(soft));
    RawCollLine *lines = (RawCollLine *)*stc_collline;
    CollVert *verts = *stc_collvert;
    int line_n = 0, vert_n = 0, g = 0;
    for (CollGroup *group = *stc_firstcollgroup; group != 0; group = group->next, g++)
    {
        CollGroupDesc *d = group->desc;
        OSReport("LLDUMP group %d %d %d %d %d %d %d %d %d %d %d %d %d %d\n", g, d->floor_start, d->floor_num,
                 d->ceil_start, d->ceil_num, d->rwall_start, d->rwall_num, d->lwall_start, d->lwall_num,
                 d->dyn_start, d->dyn_num, d->vert_start, d->vert_num, group->is_enabled);
        int ends[5] = {d->floor_start + d->floor_num, d->ceil_start + d->ceil_num, d->rwall_start + d->rwall_num,
                       d->lwall_start + d->lwall_num, d->dyn_start + d->dyn_num};
        for (int i = 0; i < 5; i++)
            if (ends[i] > line_n)
                line_n = ends[i];
        if (d->vert_start + d->vert_num > vert_n)
            vert_n = d->vert_start + d->vert_num;
    }
    for (int i = 0; i < line_n; i++)
    {
        CollLineDesc *d = lines[i].desc;
        u8 *raw = (u8 *)d;
        OSReport("LLDUMP line %d %d %d %d %d %d %d %02x %02x %02x %08x\n", i, d->vert_prev, d->vert_next, d->line_prev,
                 d->line_next, d->line_prev_altgroup, d->line_next_altgroup, raw[0xC], raw[0xD], raw[0xE], lines[i].flags);
    }
    for (int i = 0; i < vert_n; i++)
        OSReport("LLDUMP vert %d %08x %08x %08x %08x\n", i, Float_Bits(verts[i].pos_curr.X), Float_Bits(verts[i].pos_curr.Y),
                 Float_Bits(verts[i].pos_orig.X), Float_Bits(verts[i].pos_orig.Y));
    OSReport("LLDUMP end %d lines %d verts\n", line_n, vert_n);
    Sim_Check(fp, pred_route); // the route's own prediction, redone next time it's needed
}

#define COLL_ECB_LOCKED 0x10 // CollData x130: the ECB bottom stays where it was (CollData_X130_Locked)

// The drift speed that leans a fall toward target t (Lean_Target backward):
// past the deadzone, t of the way to the drift max, forward if t > 0.
static float Hover_LeanSpeed(FighterData *fp, float t)
{
    if (t == 0)
        return 0;
    float frac = common_fall_lean_deadzone + fabs(t) * (1.f - common_fall_lean_deadzone);
    return (t > 0 ? frac : -frac) * fp->attr.aerial_drift_max * fp->facing_direction;
}

// Each frame after the learning: held at the hover spot, still or at the
// drift speed of a lean.
static void Hover_Think(FighterData *fp)
{
    if (!hover_on)
        return;
    fp->phys.pos.X = hover_x;
    fp->phys.pos.Y = hover_y;
    fp->phys.self_vel.X = Hover_LeanSpeed(fp, hover_lean);
    fp->phys.self_vel.Y = 0;
    // no bottom lock: the count and the flag both (ftCommon_UnlockECB). The
    // count alone kept the takeoff's bottom, at the feet, for good, and
    // every capture before 0.9.20 learned that instead of the bones' bottom
    fp->coll_data.u.ecb_bot_lock_frames = 0;
    fp->coll_data.flags &= ~COLL_ECB_LOCKED;
}

// A capture's fall frame with its lean, for the fighter's lean grid
// (tools/llsim/ecbpack.py): LLLEANECB state, the hover's lean target in
// thousandths (9999 falling free), frame in the state, the game's own lean
// side (sm), the sim's (Lean_Target on the speed the animation saw), the
// bottom's flag, then the blend and the ECB (bottom top side front back) as
// bits. Falling free, the frames check the grid against the real fall.
static void Lean_Log(FighterData *fp, int ts, int frame)
{
    EcbSample e = {0};
    float target;
    Ecb_Fill(fp, &e);
    int side = Lean_Target(fp, prev_vel.X, fp->facing_direction, &target);
    int tag = hover_on ? (int)(hover_lean * 1000.f + (hover_lean < 0 ? -0.5f : 0.5f)) : 9999;
    OSReport("LLLEANECB %d %d %d %d %d %d %08x %08x %08x %08x %08x %08x\n", ts, tag, frame, fp->state_var.state_var1, side,
             e.has_bottom, Float_Bits(Lean_FromFighter(fp, ts)), Float_Bits(e.bottom), Float_Bits(e.top),
             Float_Bits(e.side_y), Float_Bits(e.front), Float_Bits(e.back));
}

// A capture's double jump frame, for the double-jump-cancel fighters'
// movement (tools/llsim/ecbpack.py, DJMV): LLDJMV state, frame in the
// state, facing, then as bits the speed after this frame's physics (held
// still by the hover only after it) and Ness's own drift speed
// (mv.co.jumpaerial.init_h_vel).
static void Dj_Log(FighterData *fp, int ts, int frame)
{
    OSReport("LLDJMV %d %d %d %08x %08x %08x\n", ts, frame, fp->facing_direction > 0 ? 1 : -1,
             Float_Bits(fp->phys.self_vel.X), Float_Bits(fp->phys.self_vel.Y), fp->state_var.state_var2);
}

// Script command "dumpecb": the learned ECB of every tracked state, frame by
// frame (LLECB ts frame flags bottom top side front back, the floats as
// bits; flags as in BakedEcb), and how long each state ran before ending by
// itself (LLLEN ts len next).
static void Ecb_Dump(FighterData *fp)
{
    char buf[160];
    OSReport("LLECB begin char %d\n", fp->kind);
    for (int ts = 0; ts < TS_COUNT; ts++)
    {
        for (int f = 0; f < LL_STATE_FRAMES; f++)
        {
            EcbSample *e = Ecb_Slot(ts, f);
            if (!e->seen)
                continue;
            int flags = (e->has_bottom ? BAKED_BOTTOM : 0) | (e->has_shape ? BAKED_SHAPE : 0) | (e->aerial_lag ? BAKED_LAG : 0);
            sprintf(buf, "LLECB %d %d %d %08x %08x %08x %08x %08x\n", ts, f, flags, Float_Bits(e->bottom),
                    Float_Bits(e->top), Float_Bits(e->side_y), Float_Bits(e->front), Float_Bits(e->back));
            OSReport("%s", buf);
        }
        OSReport("LLLEN %d %d %d\n", ts, state_len[ts], state_next[ts]);
    }
    // the character's own states (LLSPEC sid frame flags ..., as LLECB;
    // LLSPECLEN sid frames-seen next-state)
    for (int i = 0; i < spec_n && spec_table; i++)
    {
        for (int f = 0; f < LL_STATE_FRAMES; f++)
        {
            EcbSample *e = &spec_table[i * LL_STATE_FRAMES + f];
            if (!e->seen)
                continue;
            int flags = (e->has_bottom ? BAKED_BOTTOM : 0) | (e->has_shape ? BAKED_SHAPE : 0) | (e->aerial_lag ? BAKED_LAG : 0);
            sprintf(buf, "LLSPEC %d %d %d %08x %08x %08x %08x %08x\n", spec_sid[i], f, flags, Float_Bits(e->bottom),
                    Float_Bits(e->top), Float_Bits(e->side_y), Float_Bits(e->front), Float_Bits(e->back));
            OSReport("%s", buf);
        }
        OSReport("LLSPECLEN %d %d %d\n", spec_sid[i], spec_len[i], spec_next[i]);
    }
    OSReport("LLSPEC end %d states\n", spec_n);
    OSReport("LLLOOP %d %d %d\n", fall_loop[0], fall_loop[1], fall_loop[2]); // the falls' loops
    OSReport("LLECB end\n");
}
