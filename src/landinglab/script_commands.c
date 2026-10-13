/* landinglab/script_commands.c
 *
 * Holds test script commands and the per-frame script runner, and the
 * reference demo callbacks.
 *
 * Part of Landing Gear: src/landinglab.c includes it, in order, into one
 * translation unit, so it sees everything included before it (and mex.h,
 * events.h, landinglab_sim.h).
 *
 * Uses: option_values, menus_practice, menus_display, presets, memory_card,
 * camera_views, tracking_state, logging, hud_text, drawing, corner_panel,
 * debug_camera, scripts, sim_dump.
 * Refers ahead, through forward declarations, to: jump_tracking,
 * ledge_practice, reset_drill, assist, position_chords.
 */

static void Script_Cmd(ScriptOp *op)
{
    char line[96], buf[160];
    int len = strlen(op->label);
    if (len > (int)sizeof(line) - 1)
        len = sizeof(line) - 1;
    memcpy(line, op->label, len);
    line[len] = 0;
    char *rest = line;
    int ok = 1;
    switch (op->count)
    {
    case SCMD_TRACE:
    {
        ok = Script_Is(line, "on") || Script_Is(line, "off");
        if (!ok)
            break;
        script_trace = Script_Is(line, "on");
        if (script_trace)
        {
            int i = 0;
            const char *name = scripts[script_cur].name;
            for (; name[i] && i < (int)sizeof(script_trace_id) - 1; i++)
            {
                char c = name[i];
                script_trace_id[i] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                      (c >= '0' && c <= '9') || c == '_' || c == '-') ? c : '_';
            }
            script_trace_id[i] = 0;
            script_trace_last = -1;
            FighterData *fp = Fighter_GetGObj(0)->userdata;
            sprintf(buf, "LLST1 BEGIN id=%s character=%d stage=%d\n", script_trace_id,
                    fp->kind, Stage_GetExternalID());
            Log(buf);
        }
        break;
    }
    case SCMD_SET:
    {
        char *eq = line;
        while (*eq && *eq != '=')
            eq++;
        ok = *eq == '=';
        if (!ok)
            break;
        *eq = 0;
        char *v = Script_Trim(eq + 1);
        EventOption *o = Script_FindOption(Script_Trim(line));
        int val = (int)Script_Number(&v);
        ok = o != 0 && *v == 0 && Preset_Fits(o, val);
        if (!ok)
            break;
        int cam = o == &Options_Camera[CAMOPT_MODE] && o->val != val;
        int start = o == &Options_Ledge[LOPT_START] && o->val != val;
        o->val = val;
        if (cam)
            preset_cam_pending = 1;
        if (start)
            Event_ChangeLedgeStart(0, val);
        Event_ChangeRoutes(0, 0);
        sprintf(buf, "LLSET %s = %d\n", line, val);
        Log(buf);
        return;
    }
    case SCMD_GET:
    {
        EventOption *o = Script_FindOption(Script_Trim(line));
        ok = o != 0;
        if (!ok)
            break;
        if (o->kind == OPTKIND_STRING && o->val >= 0 && o->val < o->value_num)
            sprintf(buf, "LLGET %s = %d (%s)\n", line, o->val, o->values[o->val]);
        else
            sprintf(buf, "LLGET %s = %d\n", line, o->val);
        Log(buf);
        return;
    }
    case SCMD_CLOSEMENU:
        Presets_KeepUser();
        return;
    case SCMD_VIEW:
    {
        char *what = Script_Word(&rest), *n = Script_Word(&rest);
        int v = n ? (int)Script_Number(&n) : 0;
        ok = what && v >= 1 && v <= VIEW_SLOTS;
        if (!ok)
            break;
        if (Script_Is(what, "show"))
        {
            // where the camera is now, to compare with view v as saved
            COBJ *cobj = View_CObj();
            CamView *w = View_Find(v);
            Vec3 eye = {0}, at = {0};
            if (cobj)
            {
                COBJ_GetEyePosition(cobj, &eye);
                COBJ_GetInterest(cobj, &at);
            }
            sprintf(buf, "LLVIEW show: eye %.1f %.1f %.1f at %.1f %.1f %.1f fov %.1f; view %d \"%s\" on stage %d %s\n", eye.X,
                    eye.Y, eye.Z, at.X, at.Y, at.Z, cobj ? cobj->projection_param.perspective.fov : 0.f, v, view_label[v - 1],
                    Stage_GetExternalID(),
                    !w ? "unsaved"
                    : fabs(eye.X - w->eye.X) + fabs(eye.Y - w->eye.Y) + fabs(eye.Z - w->eye.Z) < 0.5f ? "matches" : "differs");
            Log(buf);
            break;
        }
        if (Script_Is(what, "shift"))
        {
            // view shift 1 dx dy: slides the Advanced camera (eye and what
            // it looks at) by dx dy, to check effects follow it; the number
            // after shift is ignored
            char *a = Script_Word(&rest), *b = Script_Word(&rest);
            ok = a && b;
            if (ok)
            {
                float dx = Script_Number(&a), dy = Script_Number(&b);
                Options_Camera[CAMOPT_MODE].val = CAM_ADVANCED;
                Event_ChangeCamera(0, CAM_ADVANCED);
                COBJ *cobj = View_CObj();
                if (cobj)
                {
                    COBJ_GetEyePosition(cobj, &dev_cam->free_eye_pos);
                    COBJ_GetInterest(cobj, &dev_cam->free_int_pos);
                    dev_cam->free_fov = cobj->projection_param.perspective.fov;
                }
                dev_cam->free_eye_pos.X += dx;
                dev_cam->free_eye_pos.Y += dy;
                dev_cam->free_int_pos.X += dx;
                dev_cam->free_int_pos.Y += dy;
            }
            break;
        }
        if (Script_Is(what, "name"))
        {
            // view name N Word: names a saved view without the letter grid
            CamView *w = View_Find(v);
            char *t = Script_Word(&rest);
            ok = w && t;
            if (ok)
            {
                Name_Copy(w->name, t);
                preset_dirty = 1;
                Labels_Refresh();
            }
            break;
        }
        Options_Camera[CAMOPT_VIEW].val = v;
        if (Script_Is(what, "save"))
            Event_SaveView(0);
        else if (Script_Is(what, "load"))
            Event_ChangeView(0, v);
        else
            ok = 0;
        break;
    }
    case SCMD_PRESET:
    {
        char *what = Script_Word(&rest);
        int which = Script_Preset(Script_Trim(rest));
        ok = what && which >= 0;
        if (!ok)
            break;
        if (Script_Is(what, "load") || Script_Is(what, "save"))
        {
            Options_Presets[PROPT_PICK].val = which;
            if (what[0] == 'l')
                Event_PresetLoad(0);
            else
                Event_PresetSave(0);
        }
        else if (Script_Is(what, "start"))
        {
            Options_Presets[PROPT_START].val = which;
            Event_ChangePresetStart(0, which);
        }
        else
            ok = 0;
        break;
    }
    case SCMD_CARD:
        if (Script_Is(rest, "flush"))
            Card_Flush();
        else if (Script_Is(rest, "restart"))
        {
            Card_Flush();
            Preset_Apply(&preset_builtin[PS_DEFAULTS - PS_SAVED]);
            Card_Load();
        }
        else
            ok = 0;
        break;
    case SCMD_FADE:
        Event_FadeReset(0); // Auto Fade's hit rates from earlier runs
        break;
    case SCMD_DUMP:
        Sim_Dump(Fighter_GetGObj(0)->userdata);
        break;
    case SCMD_HOVER:
    {
        // hover x y [lean T]: held there from now on (still, no bottom
        // lock), so whole animations play in the air; with a lean, moving
        // at the drift speed that leans a fall toward T (< 0: back). hover
        // off lets go
        char *a = Script_Word(&rest), *b = Script_Word(&rest);
        char *l = Script_Word(&rest), *t = Script_Word(&rest);
        if (a && Script_Is(a, "off"))
            hover_on = 0;
        else
        {
            ok = a && b && (!l || (Script_Is(l, "lean") && t));
            if (ok)
            {
                hover_x = Script_Number(&a);
                hover_y = Script_Number(&b);
                hover_lean = l ? Script_Number(&t) : 0;
                hover_on = 1;
            }
        }
        break;
    }
    case SCMD_CAPTURE:
        // forget everything, baked data too: what's learned from here on is
        // this character's own
        Learned_Clear();
        if (!sim_falcon)
            Lean_Drop(); // a packed file's leans too: the second check is on the capture alone
        Spec_Clear();
        capture_on = 1;
        break;
    case SCMD_DUMPECB:
        Ecb_Dump(Fighter_GetGObj(0)->userdata);
        break;
    case SCMD_REFILL:
        // the jumps back, once (a double jump to capture): left alone
        // otherwise, so a double jump still falls into FallAerial
        ((FighterData *)Fighter_GetGObj(0)->userdata)->jump.jumps_used = 1;
        break;
    case SCMD_BAKE:
#ifdef LL_GROUND_GUIDE
        gd_bake = -2; // the ground guide's tables, from the next frame
#else
        ok = 0;
#endif
        break;
    case SCMD_CHORD:
    {
        char *mods = Script_Word(&rest), *dir = Script_Word(&rest);
        ok = mods && dir;
        if (!ok)
            break;
        int lr = mods[0] == 'l', z = mods[0] == 'z' || mods[2] == 'z';
        int down = Script_Is(dir, "up") ? HSD_BUTTON_DPAD_UP : Script_Is(dir, "down") ? HSD_BUTTON_DPAD_DOWN : Script_Is(dir, "left") ? HSD_BUTTON_DPAD_LEFT : Script_Is(dir, "right") ? HSD_BUTTON_DPAD_RIGHT : 0;
        ok = down != 0 && (lr || z);
        if (ok)
            Chord(lr, z, down);
        break;
    }
    }
    if (!ok)
    {
        sprintf(buf, "LLSCRIPT can't do this step: %s\n", op->label);
        Log(buf);
    }
}

// Steps that take no frame. Before a frame (pre) a start or air places
// Falcon; after one, a shot or mark goes right after the inputs before it,
// and a start or air waits for the next frame. Returns 0 once the script is over.
static int Script_Instant(GOBJ *ft, int pre)
{
    char buf[160];
    for (;;)
    {
        ScriptOp *op = Script_Op();
        if (!op)
        {
            Script_End();
            return 0;
        }
        if (op->kind == SOP_START)
        {
            if (!pre)
                return 1;
            if (!Script_Place(ft, op->x, op->y, op->facing))
            {
                sprintf(buf, "LLSCRIPT no floor at %.2f %.2f, skipping %s\n", op->x, op->y, scripts[script_cur].name);
                Log(buf);
                Script_End();
                return 0;
            }
        }
        else if (op->kind == SOP_AIR)
        {
            if (!pre)
                return 1;
            Script_PlaceAir(ft, op);
        }
        else if (op->kind == SOP_LEDGE)
        {
            if (!pre)
                return 1;
            if (!Script_PlaceLedge(ft, op))
            {
                sprintf(buf, "LLSCRIPT no ledge, skipping %s\n", scripts[script_cur].name);
                Log(buf);
                Script_End();
                return 0;
            }
        }
        else if (op->kind == SOP_SHOT)
        {
            Options_Game[GOPT_FRAME_ADV].val = 1;
            int n = strlen(op->label);
            if (n > (int)sizeof(shot_label) - 1)
                n = sizeof(shot_label) - 1;
            memcpy(shot_label, op->label, n);
            shot_label[n] = 0;
            sprintf(buf, "LLSHOT %s %s at %d\n", scripts[script_cur].name, op->label, event_vars->game_timer);
            Log(buf);
        }
        else if (op->kind == SOP_MARK)
        {
            sprintf(buf, "LLMARK %s %s at %d\n", scripts[script_cur].name, op->label, event_vars->game_timer);
            Log(buf);
        }
        else if (op->kind == SOP_CMD)
            Script_Cmd(op);
        else
            return 1;
        Script_Next();
        // the steps after a shot wait for the next frame, so the frozen
        // frame shows what the script set up for it
        if (op->kind == SOP_SHOT && !pre)
            return 1;
    }
}

// The aerial an input starts when pressed with nothing held before
// (Aerial_Pressed), or -1.
static int Script_Aerial(ScriptInput *in, float facing)
{
    float cx = in->cx / 80.f, cy = in->cy / 80.f;
    int cstick = (fabs(script_last.cx / 80.f) < common_aerial_stick_x && fabs(cx) >= common_aerial_stick_x) ||
                 (fabs(script_last.cy / 80.f) < common_aerial_stick_y && fabs(cy) >= common_aerial_stick_y);
    if (!cstick && !(in->buttons & HSD_BUTTON_A))
        return -1;

    float x = cstick ? cx : in->lx / 80.f;
    float y = cstick ? cy : in->ly / 80.f;
    if (fabs(x) < common_aerial_stick_x && fabs(y) < common_aerial_stick_y)
        return TS_AIRN;
    float angle = atan2(y, fabs(x));
    if (angle > common_aerial_angle)
        return TS_AIRHI;
    if (angle < -common_aerial_angle)
        return TS_AIRLW;
    return x * facing >= 0 ? TS_AIRF : TS_AIRB;
}

// The frame the next window for a wl/ai step's own airdodge or aerial opens
// on, by the prediction made last frame (its step k is frame live_timer +
// k), or -1. Every window counts, whatever the AI Filter shows. Its length
// by that prediction goes in width.
static int Script_WindowStart(ScriptOp *op, FighterData *fp, int *width)
{
    Prediction *p = pred_live;
    if (!live_visible || live_timer != event_vars->game_timer - 1)
        return -1;

    int wl = op->kind == SOP_WL;
    int last = p->land_frame ? p->land_frame - !wl : p->num;
    int bit;
    if (wl)
        bit = Dodge_Dir(op->in.lx / 80.f, op->in.ly / 80.f);
    else
    {
        int aerial = Script_Aerial(&op->in, fp->facing_direction);
        bit = aerial >= 0 ? AERIAL_BIT(aerial) : 0;
    }
    if (!bit)
        bit = 0xFF;

    // the windows the cues show: for a waveland, a late one when that's all
    // there is
    for (int k = 1; k <= last && k < p->uncertain_from; k++)
    {
        if ((wl ? WL_Mask(p, k) : p->ai_mask[k]) & bit)
        {
            *width = 0;
            while (k + *width <= last && k + *width < p->uncertain_from &&
                   ((wl ? WL_Mask(p, k + *width) : p->ai_mask[k + *width]) & bit))
                (*width)++;
            return live_timer + k;
        }
    }
    return -1;
}

static void Jump_Follow(ScriptInput *in);
static int Route_Play(FighterData *fp, ScriptInput *in, int row, int age);

// Runs before the fighters read their controllers (the lab's Record_Think
// slot), so the inputs count on this very frame.
static void Script_Think(GOBJ *gobj)
{
    char buf[160];

    // (autorun gives the match a moment to settle first)
    if (script_pending && event_vars->game_timer >= 30)
    {
        script_pending = 0;
        int sel = Options_Dev[DOPT_SCRIPT].val;
        if (sel > 0 && script_num > 0)
            Script_Begin(sel <= script_shown ? sel - 1 : 0);
    }
    // a ledge placement (a script's, or the drill's reset) that left the
    // fighter standing on the corner
    Script_LedgeCheck(Fighter_GetGObj(0));
    if (script_cur < 0 || script_trace_end_pending)
        return;

    GOBJ *ft = Fighter_GetGObj(0);
    FighterData *fp = ft->userdata;
    ScriptInput in;
    int done = 0;
    ScriptOp *op;

    for (;;)
    {
        if (!Script_Instant(ft, 1))
            return;
        op = Script_Op();
        // land: over once Falcon is back on the ground, and this frame is
        // the next step's
        if (op->kind == SOP_LAND && fp->phys.air_state == 1)
            script_air = 1;
        if (op->kind == SOP_LAND && script_air && fp->phys.air_state == 0)
        {
            Script_Next();
            continue;
        }
        break;
    }

    switch (op->kind)
    {
    case SOP_INPUT:
        in = op->in;
        done = --script_left <= 0;
        break;
    case SOP_FOLLOW:
        in = op->in;
        Jump_Follow(&in);
        done = --script_left <= 0;
        break;
    case SOP_LAND:
        in = op->in;
        done = ++script_wait >= (op->count > 0 ? op->count : LL_SCRIPT_WAIT);
        break;
    case SOP_PLAY:
        memset(&in, 0, sizeof(in));
        done = Route_Play(fp, &in, op->count, script_wait++);
        break;
    default: // SOP_WL, SOP_AI
    {
        // once the window has opened, its start is kept: the predictions
        // after it only see what's left of it
        int width = script_width;
        int start = script_window >= 0 ? script_window : Script_WindowStart(op, fp, &width);
        if (start >= 0 && start <= event_vars->game_timer && script_window < 0)
        {
            script_window = start;
            script_width = width;
        }
        int target = start >= 0 ? start + op->count : -1;
        if (target >= 0 && event_vars->game_timer >= target)
        {
            in = op->in;
            done = 1;
            sprintf(buf, "LLSCRIPT %s window opens at %d, %d frames predicted, pressed at %d (offset %d)\n",
                    op->kind == SOP_WL ? "wl" : "ai", start, width, event_vars->game_timer,
                    event_vars->game_timer - start);
            Log(buf);
        }
        else
        {
            // keep the stick, let go of the rest
            memset(&in, 0, sizeof(in));
            in.lx = script_last.lx;
            in.ly = script_last.ly;
            if (++script_wait >= LL_SCRIPT_WAIT)
            {
                sprintf(buf, "LLSCRIPT no %s window came, going on\n", op->kind == SOP_WL ? "wl" : "ai");
                Log(buf);
                done = 1;
            }
        }
        break;
    }
    }

    Script_Apply(fp, &in);
    script_last = in;
    if (!reference_mode || Log_Level() >= LOG_FRAMES)
    {
        sprintf(buf, "LLIN %d s %d %d c %d %d lt %d btn %x\n", event_vars->game_timer,
                in.lx, in.ly, in.cx, in.cy, in.trigger, in.buttons);
        Log(buf);
    }

    if (done)
    {
        Script_Next();
        Script_Instant(ft, 0);
    }
}

void Event_ChangeScript(GOBJ *menu, int value)
{
    int mode = 0;
    if (value > 0 && value <= script_shown)
        for (int i = 0; i < (int)countof(reference_names); i++)
            if (Script_Is(scripts[value - 1].name, reference_names[i]))
                mode = i + 1;
    Reference_Restore(0);
    if (mode)
    {
        FighterData *fp = Fighter_GetGObj(0)->userdata;
        if (fp->kind != FTKIND_FALCON || Stage_GetExternalID() != GRKINDEXT_BATTLE)
        {
            // nothing runs: the menu's Script goes back to Off, so D-pad
            // left resets as usual instead of trying it again
            Toast("Requires Falcon on Battlefield");
            Options_Dev[DOPT_SCRIPT].val = 0;
            script_cur = -1;
            script_pending = 0;
            return;
        }
        for (int i = 0; i < (int)countof(reference_options); i++)
        {
            reference_saved[i] = reference_options[i]->val;
            reference_options[i]->val = 0;
        }
        // Practice is Assist on the reference route: the game waits on
        // the frame before each input (Stephen, 0.8.8: it should work the
        // way Assist does, not leave him in Frame Advance)
        if (mode == 2)
            Options_Ledge[LOPT_ASSIST].val = 1;
        reference_mode = mode;
        Assist_Clear();
        OSReport("LLREF start mode=%d frame=%d assist=%d reset=%d inv=%d drop=%d routes=%d advance=%d\n",
                 mode, event_vars->game_timer, reference_saved[0], reference_saved[1], reference_saved[2],
                 reference_saved[3], reference_saved[4], reference_saved[5]);
    }
    script_trace = 0;
    script_trace_end_pending = 0;
    script_cur = -1;
    script_pending = value != 0;
}

static void Reference_Play(GOBJ *menu, int mode)
{
    FighterData *fp = Fighter_GetGObj(0)->userdata;
    if (fp->kind != FTKIND_FALCON || Stage_GetExternalID() != GRKINDEXT_BATTLE)
    {
        Toast("Requires Falcon on Battlefield");
        return;
    }
    int index = -1;
    for (int i = 0; i < script_num; i++)
        if (Script_Is(scripts[i].name, reference_names[mode - 1]))
        {
            index = i;
            break;
        }
    if (index < 0 || index >= script_shown)
    {
        Toast("Reference script missing from disc");
        return;
    }
    // Preserve the menu's real settings before applying temporary helpers.
    Presets_KeepUser();
    Options_Dev[DOPT_SCRIPT].val = index + 1;
    Event_ChangeScript(menu, index + 1);
    if (menu)
        event_vars->Menu_Exit(menu);
}

void Event_ReferenceDemo(GOBJ *menu)
{
    Reference_Play(menu, 1);
}

void Event_ReferencePractice(GOBJ *menu)
{
    if (reference_mode == 2)
    {
        // picked again: practice off, the helpers back as they were
        Options_Dev[DOPT_SCRIPT].val = 0;
        Event_ChangeScript(menu, 0);
        Toast("15 GALINT practice off");
        if (menu)
            event_vars->Menu_Exit(menu);
        return;
    }
    Reference_Play(menu, 2);
}

///////////////////////
/// Event callbacks ///
///////////////////////

// Grounded states a jump can start from.
