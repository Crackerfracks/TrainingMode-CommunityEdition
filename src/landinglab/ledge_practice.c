/* landinglab/ledge_practice.c
 *
 * Holds ledge practice while hanging: route text and menu, slips, ledge
 * drops, Ledge_Think.
 *
 * Part of Landing Lab: src/landinglab.c includes it, in order, into one
 * translation unit, so it sees everything included before it (and mex.h,
 * events.h, landinglab_sim.h).
 *
 * Uses: option_values, menus_practice, menus_display, tracking_state, rumble,
 * logging, hud_text, drawing, meter, meter_placement, ledge_routes.
 * Refers ahead, through forward declarations, to: reset_drill.
 */

// A route's kind and GALINT, like "NIL  11 GALINT" or "AI Bair  15 GALINT".
static void Route_Name(char *t, LedgeRoute *r, int galint)
{
    t += sprintf(t, "%s", r->kind == LAND_AI ? "AI " : "NIL");
    if (r->kind == LAND_AI)
        t += sprintf(t, "%s", tracked_state_names[r->aerial]);
    if (galint > 0)
        sprintf(t, "  %d GALINT", galint);
}

// Its inputs, like "away, wait 1, FF 2, DJ in".
static void Route_Steps(char *t, LedgeRoute *r)
{
    t += sprintf(t, "%s", r->drop == DROP_AWAY ? "away" : "down");
    if (r->fall_away)
        t += sprintf(t, ", drift away");
    if (r->wait > 0)
        t += sprintf(t, ", wait %d", r->wait);
    if (r->ff > 0)
        t += sprintf(t, ", FF %d", r->ff);
    static const char *dj[LR_DJ_X] = {"DJ in", "DJ down-in", "DJ"};
    sprintf(t, ", %s%s", dj[r->dj_x], r->hold ? ", hold in" : "");
}

// The panel's lines for a route: which of the routes it is, its kind and
// GALINT, and its inputs under it.
static void Route_Text(LedgeRoute *r, int galint, int num, int total)
{
    char *t = text_next;
    if (num > 0)
        t += sprintf(t, "Route %d of %d: ", num, total);
    Route_Name(t, r, galint);
    Route_Steps(text_steps, r);
}

// The Route option, in the menu: its value ("4 of 23") and the chosen
// route's GALINT and inputs in the lines of its description. The HUD isn't
// drawn while the menu is open, so this is where browsing shows what each
// route is.
static void Route_MenuText(void)
{
    EventOption *o = &Options_Ledge[LOPT_PICK];
    int n = route_list_num;
    route_desc[0][0] = route_desc[1][0] = route_desc[2][0] = 0;
    strcpy(route_pick_fmt, "%d");
    if (route_list_ledge < 0)
        sprintf(route_desc[0], "Hang from a ledge to browse its routes.");
    else if (!ledges[route_list_ledge].have)
        sprintf(route_desc[0], "%s", Routes_On() ? "Still searching this ledge's routes..." : "Turn on Show Routes to find routes.");
    else if (n == 0)
    {
        strcpy(route_pick_fmt, "none");
        sprintf(route_desc[0], "No route of this kind keeps GALINT.");
    }
    else
    {
        int pick = o->val - 1;
        if (pick >= n)
            pick = n - 1;
        if (pick < 0)
            pick = 0;
        LedgeRoute *r = &route_list[pick];
        sprintf(route_pick_fmt, "%%d of %d", n);
        sprintf(route_desc[0], "Route %d of %d: ", pick + 1, n);
        Route_Name(route_desc[0] + strlen(route_desc[0]), r, Route_Galint(r, -1, (*stc_ftcommon)->cliff_invuln_time));
        Route_Steps(route_desc[1], r);
        if (r->kind == LAND_NIL)
            sprintf(route_desc[2], "No aerial: it lands with no lag.");
        else if (r->press_w == 1)
            sprintf(route_desc[2], "%s on frame %d after the jump.", tracked_state_names[r->aerial], r->press - r->dj);
        else
            sprintf(route_desc[2], "%s on frames %d to %d after the jump.", tracked_state_names[r->aerial],
                    r->press - r->dj, r->press - r->dj + r->press_w - 1);
    }
}

void Event_ChangeRoutes(GOBJ *menu, int value)
{
    // a new kind or order rebuilds the list and keeps the chosen route; only
    // a new Route number is a new choice
    if (!Routes_Update(route_browse))
        Route_RememberPick();
    if (hang_ledge >= 0 && ledges[hang_ledge].have)
        Routes_Show(); // the rows follow the choice as soon as the game goes on
    Route_MenuText();
}

// The route was lost, or a step whose timing buzzes went wrong: say which
// and by how much, and drop it.
static void Route_Fail(const char *step, int off, int buzz)
{
    char buf[96];
    if (off == LR_PLAIN)
        sprintf(text_last, "%s", step);
    else if (off == 0)
        sprintf(text_last, "%s missed", step);
    else
        sprintf(text_last, "%s %df %s", step, off < 0 ? -off : off, off < 0 ? "early" : "late");
    last_kind = -1;
    route_active = 0;
    route_fail = event_vars->game_timer;
    if (buzz)
        SFX_PlayCommon(3);
    sprintf(buf, "LandingLab route: %s at %d after the drop\n", text_last, route_e);
    Log(buf);
    Drill_Finish(0);
}

// A step off its frame. When its timing is checked under Sounds, or Assist
// is drilling the route, it's a miss (returns 1, the route is dropped);
// otherwise it's only noted, and the attempt goes on to be judged by how it
// lands. opt -1: a slip no Sounds option checks (a bonk).
static int Route_Slip(int opt, const char *step, int off)
{
    int checked = opt >= 0 && Options_Sounds[opt].val;
    if (checked || Options_Ledge[LOPT_ASSIST].val)
    {
        Route_Fail(step, off, checked || Options_Sounds[SOPT_LOST].val);
        return 1;
    }
    char note[24], buf[80];
    if (off == LR_PLAIN)
        sprintf(note, "%s", step);
    else if (off == 0)
        sprintf(note, "%s missed", step);
    else
        sprintf(note, "%s %df %s", step, off < 0 ? -off : off, off < 0 ? "early" : "late");
    if (!route_note[0])
        strcpy(route_note, note);
    sprintf(buf, "LandingLab route: slip %s at %d after the drop\n", note, route_e);
    Log(buf);
    return 0;
}

// How the route was lost, with the slip that likely did it.
static const char *Route_Why(const char *what)
{
    static char why[48];
    if (route_note[0])
        sprintf(why, "%s, %s", what, route_note);
    else
        sprintf(why, "%s", what);
    return why;
}

static void Route_Log(const char *what, LedgeEntry *L, LedgeRoute *r, int galint)
{
    char buf[200];
    sprintf(buf, "LLROUTE %s ledge %.4f %.4f facing %d kind %s drop %s wait %d ff %d hold %d dj %d dj_x %d away %d ff_at %d press %d w %d aerial %d land %d act %d galint %d\n",
            what, L->x, L->y, L->facing, r->kind == LAND_AI ? "AI" : "NIL", r->drop == DROP_AWAY ? "away" : "down", r->wait,
            r->ff, r->hold, r->dj, r->dj_x, r->fall_away, r->ff_at, r->press, r->press_w, r->aerial, r->land, r->act, galint);
    Log(buf);
}

static void Route_Step(void)
{
    route_hit = event_vars->game_timer;
}

// Each frame: hanging, letting go, and every step of the route after it.
// A step is judged a frame after it was due, so one frame late reads as
// late rather than missed.
// The ledge drop drill. The game lets go of the ledge only after a frame
// of the hang with the stick at rest (CliffWait clears the flag as it
// starts and sets it on a frame the stick is in its deadzone), so holding
// down through the catch drops nothing: the stick rests on the hang's
// first frame and goes down or away on the second, the first frame a drop
// can come out. A drop on that frame or the next is a hit.
#define DROP_HIST 10
static int drop_w0 = -1;     // game_timer of the hang's first frame, -1 not hanging
static int drop_catch_left;  // frames of the catch left
static int drop_rest;        // the stick rested on the hang's first frame
static u8 drop_hist[DROP_HIST];
static int drop_hist_n, drop_hist_pos;
static int drop_age = 99;    // frames since the last graded drop
static int drop_last;        // its frame: 1 = the first possible

static int Drop_On(void)
{
    return Options_Ledge[LOPT_DROP].val;
}

static void Drop_Think(FighterData *fp, int sid, int prev_sid)
{
    drop_sid = sid;
    if (prev_sid < 0)
        drop_w0 = -1; // a test script just put Falcon somewhere
    if (drop_age < 99)
        drop_age++;
    if (sid == ASID_CLIFFCATCH)
    {
        Figatree *anim = fp->figatree_curr;
        float rate = fp->state.rate > 0 ? fp->state.rate : 1.f;
        drop_catch_left = anim ? (int)((anim->frame_num - fp->state.frame) / rate + 0.999f) : 0;
        if (drop_w0 != -2 && Drop_On() && cue_log)
            OSReport("LLDROP catch at %d, %d left\n", event_vars->game_timer, drop_catch_left);
        drop_w0 = -2; // (-2: logged)
        return;
    }
    if (sid == ASID_CLIFFWAIT)
    {
        if (drop_w0 < 0)
        {
            drop_w0 = event_vars->game_timer;
            FtCliffCatch *cliff = (void *)&fp->state_var;
            drop_rest = cliff->timer != 0;
            if (Drop_On() && cue_log)
                OSReport("LLDROP hang at %d rest %d\n", drop_w0, drop_rest);
        }
        return;
    }
    if (prev_sid == ASID_CLIFFWAIT && drop_w0 >= 0 && sid == ASID_FALL && Drop_On())
    {
        int k = event_vars->game_timer - drop_w0;
        int hit = k >= 1 && k <= 2;
        drop_hist[drop_hist_pos] = hit;
        drop_hist_pos = (drop_hist_pos + 1) % DROP_HIST;
        if (drop_hist_n < DROP_HIST)
            drop_hist_n++;
        int n = 0;
        for (int i = 0; i < drop_hist_n; i++)
            n += drop_hist[i];
        drop_last = k;
        drop_age = 0;
        if (hit)
            sprintf(text_last, "Drop frame %d, %d of last %d on 1-2", k, n, drop_hist_n);
        else if (!drop_rest)
            sprintf(text_last, "Drop f%d: rest stick as the hang starts", k);
        else
            sprintf(text_last, "Drop frame %d, %d late", k, k - 2);
        last_kind = -1;
        if (hit && Options_Sounds[SOPT_CHIME].val)
            SFX_PlayRaw(303, 255, 128, 20, 3);
        else if (!hit && Options_Sounds[SOPT_WINDOW].val)
            SFX_PlayCommon(3);
        char buf[96];
        sprintf(buf, "LLDROP frame %d rest %d hits %d of %d\n", k, drop_rest, n, drop_hist_n);
        Log(buf);
    }
    drop_w0 = -1;
}

// Its row in the timers: the hang's first frame (stick at rest) and the two
// drop frames, counting down through the catch, then the result.
static void Meter_AddDrop(int sid)
{
    if (!Drop_On())
        return;
    int hanging = sid == ASID_CLIFFWAIT && drop_w0 >= 0;
    if (sid != ASID_CLIFFCATCH && !hanging && drop_age > 20)
        return;
    MeterRow *r = Meter_Add(color_galint, "DRP");
    if (!r)
        return;
    if (sid == ASID_CLIFFCATCH || hanging)
    {
        // index 0 is now; the hang's first frame is drop_catch_left away
        int rest = sid == ASID_CLIFFCATCH ? drop_catch_left : -(event_vars->game_timer - drop_w0);
        if (rest >= 0)
            Row_Set(r, rest, CELL_LAND, TONE_CUE, 1); // hollow: nothing held
        for (int j = 1; j <= 2; j++)
            if (rest + j >= 0)
                Row_Set(r, rest + j, CELL_PRESS, TONE_CUE, 1);
        if (rest + 1 <= 0 && rest + 2 >= 0)
        {
            r->hot = rest + 1 == 0 || rest + 2 == 0 ? 2 : 1;
            r->spent = -(rest + 1);
        }
        if (rest > 0)
            sprintf(r->info, "%df", rest + 1);
        else if (rest + 2 >= 0)
            sprintf(r->info, "now");
        else
            sprintf(r->info, "late");
    }
    else
    {
        int hit = drop_last >= 1 && drop_last <= 2;
        if (hit)
            r->burst = drop_age;
        else
        {
            r->implode = drop_age;
            r->implode_tone = TONE_MISS;
        }
        sprintf(r->info, "f%d", drop_last);
    }
}

static void Ledge_Think(FighterData *fp, int sid)
{
    if (!ledges_found)
    {
        Ledge_FindMain();
        ledges_found = 1;
    }

    int hanging = sid == ASID_CLIFFWAIT;
    if (hanging && Options_Ledge[LOPT_INV].val)
    {
        fp->hurt.intang_frames.ledge = (*stc_ftcommon)->cliff_invuln_time + 1;
        FtCliffCatch *cliff = (void *)&fp->state_var;
        cliff->fall_timer = 2;
    }
    int intang = fp->hurt.intang_frames.ledge;
    galint_now = hanging ? 0 : intang;
    if (!hanging)
        Routes_Update(route_browse); // the Route option's text, before the first ledge too

    if (hanging)
    {
        // learn where Falcon really hangs, and keep the ledge's routes ready
        int facing = fp->facing_direction > 0 ? 1 : -1;
        hang_ledge = Ledge_Add(fp->phys.pos.X, fp->phys.pos.Y, facing);
        attempt_active = 0;
        route_active = 0;
        route_show_num = 0;
        if (hang_ledge >= 0)
            route_browse = hang_ledge;
        Routes_Update(hang_ledge);
        // a ledge's routes show once its first search is done, and the
        // ones of the search before keep showing while it's searched again
        if (reference_mode == 2 && hang_ledge >= 0)
        {
            // practice: the reference route, whatever the search found
            route_show[0] = ref_15galint;
            route_show_num = 1;
            route_show_rank = 0;
            Route_Path(fp, hang_ledge, &route_show[0]);
            Route_Text(&route_show[0], Route_Galint(&route_show[0], -1, intang), 1, 1);
            next_kind = CUE_AI;
        }
        else if (hang_ledge >= 0 && ledges[hang_ledge].have)
        {
            Routes_Show();
            if (Log_Level())
            {
                LedgeEntry *L = &ledges[hang_ledge];
                if (!L->logged)
                {
                    char buf[96];
                    sprintf(buf, "LLROUTES ledge %.4f %.4f facing %d found %d nil %d ai %d\n", L->x, L->y, L->facing,
                            route_list_num, L->list.n[ROUTES_SORT_GALINT][0], L->list.n[ROUTES_SORT_GALINT][1]);
                    Log(buf);
                    for (int i = 0; i < LR_KEEP && i < route_list_num; i++)
                        Route_Log(Ordinal(i + 1), L, &route_list[i], Route_Galint(&route_list[i], -1, intang));
                }
                L->logged = 1;
            }
            if (route_show_num > 0 && Routes_On())
            {
                Route_Path(fp, hang_ledge, &route_show[0]);
                Route_Text(&route_show[0], Route_Galint(&route_show[0], -1, intang), route_show_rank + 1, route_list_num);
                next_kind = route_show[0].kind == LAND_AI ? CUE_AI : CUE_NIL;
            }
            else if (Routes_On())
            {
                int kind = Options_Ledge[LOPT_KIND].val;
                sprintf(text_next, "No %s route with GALINT", kind == ROUTES_BOTH ? "NIL or AI" : route_kind_names[kind]);
                next_kind = -1;
            }
        }
        else if (Routes_On())
        {
            sprintf(text_next, "Finding ledge routes...");
            next_kind = -1;
        }
    }

    // letting go
    if (prev_state_id == ASID_CLIFFWAIT && !hanging)
    {
        int ledge = hang_ledge;
        hang_ledge = -1;
        attempt_active = 1;
        attempt_facing = fp->facing_direction > 0 ? 1 : -1;
        route_active = 0;
        if (sid == ASID_FALL && ledge >= 0 && route_show_num > 0 && Routes_On())
        {
            // follow the chosen route, or the best one let go the same way
            int drop = fp->input.lstick.Y <= -Common_Float(0x494) && fabs(fp->input.lstick.X) < -fp->input.lstick.Y ? DROP_DOWN : DROP_AWAY;
            LedgeRoute *pick = 0;
            route_cur_num = 0;
            if (route_show[0].drop == drop)
            {
                pick = &route_show[0];
                route_cur_num = route_show_rank + 1;
            }
            for (int i = 0; i < route_list_num && !pick; i++)
            {
                if (route_list[i].drop == drop)
                {
                    pick = &route_list[i];
                    route_cur_num = i + 1;
                }
            }
            if (pick)
            {
                route_cur = *pick;
                route_cur_total = route_list_num;
                route_ledge = ledge;
                route_active = 1;
                route_drop = event_vars->game_timer;
                route_dj_done = 0;
                route_aerial_done = 0;
                route_ff_done = 0;
                route_off = 0;
                route_bonked = 0;
                route_any_aerial = 0;
                route_note[0] = 0;
                route_landed = -1;
                route_galint = -1;
                route_pred = Route_Galint(&route_cur, 0, intang);
                if (Log_Level())
                    Route_Log("follow", &ledges[ledge], &route_cur, route_pred);
                route_hit = event_vars->game_timer;
                prev_fastfall = 0;
                Route_Path(fp, ledge, &route_cur);
            }
        }
        route_show_num = 0;
    }

    if (route_active)
    {
        LedgeRoute *r = &route_cur;
        int e = event_vars->game_timer - route_drop;
        route_e = e;
        int ff_now = fp->flags.is_fastfall && !prev_fastfall;
        prev_fastfall = fp->flags.is_fastfall;
        int jumped = (sid == ASID_JUMPAERIALF || sid == ASID_JUMPAERIALB) && prev_state_id != sid;
        int in_aerial = sid >= ASID_ATTACKAIRN && sid <= ASID_ATTACKAIRLW;
        int aerial = in_aerial && prev_state_id != sid;
        int landed = fp->phys.air_state == 0;
        // an aerial whose ECB touched down on its first frame never shows
        if (!aerial && landed && sid == ASID_LANDING && !route_any_aerial && !route_aerial_done)
            aerial = Aerial_Pressed(fp) >= 0;

        if (fp->flags.hitlag || fp->flags.hitstun || sid == ASID_CLIFFCATCH || hanging)
        {
            route_active = 0; // hit, or back on a ledge: no longer the route
            return;
        }
        Route_Text(r, route_galint >= 0 ? route_galint : Route_Galint(r, e, intang), route_cur_num, route_cur_total);
        next_kind = r->kind == LAND_AI ? CUE_AI : CUE_NIL;

        if (route_landed < 0)
        {
            // the routes never touch a ceiling: one that does went another way
            if ((fp->coll_data.envFlags & COLL_CEILING) && !route_bonked)
            {
                route_bonked = 1;
                route_off = 1;
                if (Route_Slip(-1, "Bonked", LR_PLAIN))
                    return;
            }
            int ff = Route_FF(r);
            if (ff_now && route_dj_done)
                ff_now = 0; // after the jump: the route is past its inputs
            if (ff_now && !route_ff_done)
            {
                route_ff_done = 1;
                if (r->ff == 0 ? Route_Slip(SOPT_FF, "FF not in route", LR_PLAIN)
                               : e != ff ? Route_Slip(SOPT_FF, "FF", e - ff) : (Route_Step(), 0))
                    return;
            }
            else if (r->ff > 0 && e > ff && !route_ff_done)
            {
                route_ff_done = 1;
                if (Route_Slip(SOPT_FF, "FF", 0))
                    return;
            }

            if (jumped && !route_dj_done)
            {
                route_dj_done = 1;
                if (e != r->dj)
                {
                    route_off = 1;
                    if (Route_Slip(SOPT_JUMP, "Jump", e - r->dj))
                        return;
                }
                else
                    Route_Step();
            }
            else if (e > r->dj && !route_dj_done)
            {
                route_dj_done = 1;
                route_off = 1;
                if (Route_Slip(SOPT_JUMP, "Jump", 0))
                    return;
            }

            int last = r->press + r->press_w - 1;
            if (r->kind == LAND_AI && aerial && !route_aerial_done)
            {
                route_aerial_done = 1;
                if (!route_off && (e < r->press || e > last))
                {
                    route_off = 1;
                    if (Route_Slip(SOPT_AERIAL, tracked_state_names[r->aerial], e < r->press ? e - r->press : e - last))
                        return;
                }
                else if (!route_off)
                    Route_Step();
            }
            else if (r->kind == LAND_AI && !route_aerial_done && !route_off && e > last && !landed)
            {
                route_off = 1;
                if (Route_Slip(SOPT_AERIAL, tracked_state_names[r->aerial], 0))
                    return;
            }

            if (aerial)
                route_any_aerial = 1;
            if (landed)
            {
                // judged by how it landed, whatever the route: on the stage
                // with no lag (a NIL), or out of an aerial with only the
                // normal landing's lag (an AI)
                if (sid != ASID_WAIT && !(sid == ASID_LANDING && route_any_aerial))
                {
                    Route_Fail(Route_Why("Landed with lag"), LR_PLAIN, Options_Sounds[SOPT_LOST].val);
                    return;
                }
                route_landed = e;
                route_act = sid == ASID_WAIT ? e : e + (int)fp->attr.normal_landing_lag;
                // the GALINT it will have at the first frame he can act, up
                // as soon as he touches down
                sprintf(text_last, "Ledge %s  %d GALINT", route_act == route_landed ? "NIL" : "AI",
                        intang - (route_act - e));
            }
            else if (fp->phys.pos.Y < ledges[route_ledge].y - 40.f)
            {
                Route_Fail(Route_Why("Missed the stage"), LR_PLAIN, Options_Sounds[SOPT_LOST].val);
                return;
            }
        }
        if (route_landed >= 0 && route_galint < 0 && e >= route_act)
        {
            // GALINT, read where ledgedash training reads it
            char buf[96];
            route_galint = intang;
            if (intang <= 0)
            {
                Route_Fail(Route_Why("No GALINT left"), LR_PLAIN, Options_Sounds[SOPT_LOST].val);
                return;
            }
            int t = sprintf(text_last, "Ledge %s  %d GALINT", route_act == route_landed ? "NIL" : "AI", intang);
            if (route_note[0])
                sprintf(text_last + t, ", %s", route_note);
            last_kind = route_act == route_landed ? CUE_NIL : CUE_AI;
            if (Options_Sounds[SOPT_ROUTE_CHIME].val)
                SFX_PlayRaw(303, 255, 128, 20, 3);
            sprintf(buf, "LandingLab route: %s, predicted %d on the drop\n", text_last, route_pred);
            Log(buf);
            Drill_Finish(1);
        }
        if (route_galint >= 0)
        {
            // the row runs on until the tail and the ACT flash are gone
            int end = Route_TailEnd(r, route_galint);
            if (end < Route_ActCell(r) + 3)
                end = Route_ActCell(r) + 3;
            if (end - e - 1 < 0)
                route_active = 0;
        }
        return;
    }

    // an attempt without a route ends at the landing, or far below
    if (attempt_active)
    {
        if (fp->phys.air_state == 0 && !hanging)
        {
            attempt_active = 0;
            Drill_Finish(last_kind == CUE_NIL || last_kind == CUE_AI); // as Landing_Resolve judged it
        }
        else if (fp->phys.pos.Y < -150.f || fp->flags.dead)
        {
            attempt_active = 0;
            Drill_Finish(0);
        }
    }
}
