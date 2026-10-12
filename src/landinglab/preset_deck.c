/* landinglab/preset_deck.c
 *
 * Holds the Preset Deck: browsing and playing presets in game.
 *
 * Part of Landing Lab: src/landinglab.c includes it, in order, into one
 * translation unit, so it sees everything included before it (and mex.h,
 * events.h, landinglab_sim.h).
 *
 * Uses 15 of the 48 modules before it.
 */

///////////////////////
/// Preset Deck     ///
///////////////////////

// The quick menu: a full L or R click with D-pad up or down opens it, and
// the game freezes. Across the top, a card for each preset with settings in
// it; left and right flip through them, and the stage changes with each.
// Down opens the chosen card's settings, a list where up and down pick one
// and left and right change it. A change is live but saved nowhere until A
// saves it to the chosen preset (a built-in one can't be changed, so it
// goes to a new one) or X or Y saves it as a new preset in the first free
// slot, the trigger still held. It closes when the trigger is let go, after
// a while with no D-pad, or after a moment of other buttons or the stick
// with no D-pad (Options_Hud). Standing on the ground, a full hop from where
// Falcon stands is put up as a still example, so the cues show something.
#define DECK_CARD_W 5.6f
#define DECK_CARD_H 6.2f
#define DECK_GAP 0.5f
#define DECK_TOP (SAFE_H + 0.5f)
#define DECK_ROW_H 1.75f
#define DECK_ROW_W 21.f
#define DECK_VIEW 8 // settings rows on screen at once, so the list stays clear of the percents

typedef struct DeckRow
{
    EventOption *o;
    const char *label;
} DeckRow;

static DeckRow deck_rows[] = {
    {&Options_Cues[COPT_AI], "AI cues"},
    {&Options_Cues[COPT_NIL], "NIL cues"},
    {&Options_Cues[COPT_WL], "Waveland cues"},
    {&Options_Timers[TOPT_NEAR], "Timer near you"},
    {&Options_Timers[TOPT_STRIP], "Fixed strip"},
    {&Options_Timers[TOPT_SPOT], "Landing spot"},
    {&Options_Timers[TOPT_WL], "Waveland timer"},
    {&Options_Paths[POPT_PATH], "Landing path"},
    {&Options_Paths[POPT_BODY], "Body path"},
    {&Options_Paths[POPT_TICKS], "Frame dots"},
    {&Options_Cues[COPT_GLOW], "Platform glow"},
    {&Options_Jump[JOPT_SHOW], "Jump timing"},
    {&Options_Hud[HOPT_LOOK], "Controller look"},
    {&Options_Hud[HOPT_PAD_CUES], "Controller cues"},
    {&Options_Cues[COPT_INTENSITY], "Intensity"},
    {&Options_Intensity[IOPT_FADE], "Auto fade"},
};
#define DECK_ROWS ((int)countof(deck_rows))

// what a card's marks stand for, and their colors
typedef struct DeckMark
{
    EventOption *o;
    u8 kind; // LAND_* color
} DeckMark;
static const DeckMark deck_marks[] = {
    {&Options_Cues[COPT_AI], LAND_AI},          {&Options_Cues[COPT_NIL], LAND_NIL},
    {&Options_Cues[COPT_WL], LAND_PERFECT_WL},  {&Options_Timers[TOPT_NEAR], LAND_AI},
    {&Options_Timers[TOPT_STRIP], LAND_AI},     {&Options_Paths[POPT_PATH], LAND_AI},
    {&Options_Timers[TOPT_SPOT], LAND_AI},      {&Options_Jump[JOPT_SHOW], LAND_NORMAL},
};

static struct
{
    int row;       // -1: the cards; else the chosen card's setting
    int changed;   // a setting was changed here and not saved yet
    int idle;      // frames with no D-pad or trigger press
    int play;      // frames since the stick moved it into playing out (0: not)
    int demo;      // the still example is up
    int trig_prev; // a trigger was in last frame
    float alpha;   // its fade as a timer runs out
    Text *left, *right, *mid;
} deck;
#define DECK_TEXTS 24

static Text *Deck_Text(int align)
{
    HUDCamData *hud = event_vars->hudcam_gobj->userdata;
    Text *t = Text_CreateText(2, hud->canvas);
    t->kerning = 1;
    t->align = align;
    t->use_aspect = 0;
    t->is_depth_compare = 0;
    t->viewport_scale.X = 0.1f;
    t->viewport_scale.Y = 0.1f;
    for (int i = 0; i < DECK_TEXTS; i++)
        Text_AddSubtext(t, 0, 0, "");
    return t;
}

// a line of one of the deck's texts, as Hud_TextAligned places a row 2.5
// tall whose bottom is y; an empty one hides it
static void Deck_Line(Text *t, int i, const char *text, float x, float y, float size, GXColor c)
{
    if (!t || i >= DECK_TEXTS)
        return;
    Text_SetText(t, i, text);
    Text_SetScale(t, i, size, size);
    Text_SetPosition(t, i, x * 10.f, y * -10.f - 37.5f);
    c.a = c.a * deck.alpha; // fading out as its timer runs down
    Text_SetColor(t, i, &c);
}

static int Deck_Shown(int i)
{
    return Preset_Slot(i)->used;
}

static int Deck_Count(void)
{
    int n = 0;
    for (int i = 0; i < PS_COUNT; i++)
        n += Deck_Shown(i);
    return n;
}

// The first frame a cue the settings show would count down to in p, or 0.
static int Deck_DemoFirst(Prediction *p)
{
    int first = 0;
    int k[3] = {Cues_Ai() && p->ai_first < p->uncertain_from ? p->ai_first : 0,
                Cues_Waveland() && p->wl_first < p->uncertain_from ? p->wl_first : 0,
                Cues_Nil() && p->land_frame && p->land_kind == LAND_NIL && p->uncertain_from > p->land_frame
                    ? p->land_frame + 1
                    : 0};
    for (int i = 0; i < 3; i++)
        if (k[i] && (!first || k[i] < first))
            first = k[i];
    return first;
}

// The still example: a full hop (or a short one, if only that has a window
// to show) from where Falcon stands, its path and timers up as if it were
// happening, held at the moment its first countdown is half run. Done again
// whenever a setting changes, so it shows the settings as they are.
#define DECK_DEMO_TO_GO 12
static void Deck_Demo(FighterData *fp)
{
    if (deck.demo)
    {
        Cues_Clear();
        live_visible = 0;
        deck.demo = 0;
    }
    if (fp->phys.air_state != 0 || live_visible)
        return;
    SimStart start;
    Floor_BuildCache();
    int lead = Sim_GroundJump(fp, 0, &start);
    Predict(fp, &start, pred_live, BR_ALL);
    int first = Deck_DemoFirst(pred_live);
    if (!first)
    {
        lead = Sim_GroundJump(fp, 1, &start);
        Predict(fp, &start, pred_live, BR_ALL);
        first = Deck_DemoFirst(pred_live);
    }
    live_visible = 1;
    deck.demo = 1;
    Prediction *p = pred_live;
    int ref = first ? first : p->land_frame;
    if (ref)
    {
        // the first countdown as if partway there: started full, then run
        // on to DECK_DEMO_TO_GO
        Timing_Update(fp, p, LL_COUNT_FRAMES - ref);
        lead = DECK_DEMO_TO_GO - ref;
    }
    Timing_Update(fp, p, lead);
    // A hop from the ground seldom has a rising AI to show, so with AI cues
    // on and none here, one is put at the hop's touchdown, to see how it looks.
    int k = p->land_frame;
    if (Cues_Ai() && !cue_live[CUE_AI].phase && k > 1)
    {
        float x = p->pos[k].X, y = p->pos[k - 1].Y + p->bottom[k - 1];
        Cue_Set(CUE_AI, LL_COUNT_FRAMES, 3, 0, 0, x, y);
        Cue_Set(CUE_AI, DECK_DEMO_TO_GO, 3, 0, 0, x, y);
        Cue_Body(CUE_AI, p, k - 1);
    }
    OSReport("LLDECK demo first %d lead %d ai %d wl %d land %d\n", first, lead, pred_live->ai_first,
             pred_live->wl_first, pred_live->land_frame);
}

static void Deck_Open(void)
{
    FighterData *fp = Fighter_GetGObj(0)->userdata;
    memset(&deck, 0, sizeof(deck));
    deck.row = -1;
    deck.left = Deck_Text(0);
    deck.right = Deck_Text(2);
    deck.mid = Deck_Text(1);
    deck.trig_prev = 1;
    deck.alpha = 1.f;
    deck_on = 1;
    deck_frozen = 1;
    Deck_Demo(fp);
    OSReport("LLDECK open %d preset %d\n", event_vars->game_timer, Options_Presets[PROPT_PICK].val);
    SFX_PlayCommon(1);
}

static void Deck_Close(const char *why)
{
    if (deck.demo)
    {
        // the example was never played: the cues start again from nothing
        Cues_Clear();
        live_visible = 0;
    }
    if (deck.left)
        Text_Destroy(deck.left);
    if (deck.right)
        Text_Destroy(deck.right);
    if (deck.mid)
        Text_Destroy(deck.mid);
    deck.left = deck.right = deck.mid = 0;
    deck_on = 0;
    deck_frozen = 0;
    OSReport("LLDECK close %d %s preset %d changed %d\n", event_vars->game_timer, why, Options_Presets[PROPT_PICK].val,
             deck.changed);
}

// the option's value one step on, round
static void Deck_Step(EventOption *o, int dir)
{
    int n = o->kind == OPTKIND_TOGGLE ? 2 : o->value_num;
    int v = o->val - (o->kind == OPTKIND_TOGGLE ? 0 : o->value_min);
    v = (v + dir + n) % n;
    o->val = v + (o->kind == OPTKIND_TOGGLE ? 0 : o->value_min);
}

// The settings as they are into preset slot which (User Custom or 1 to 4,
// or a new one when which is -1 or built in), and that preset chosen.
static void Deck_Save(int which)
{
    char buf[48];
    if (which < 0 || which >= PS_SAVED)
    {
        which = 0;
        for (int i = 1; i < PS_SAVED && !which; i++)
            if (!Preset_Slot(i)->used)
                which = i;
        if (!which)
        {
            Toast("No free preset: save over one");
            SFX_PlayCommon(3);
            return;
        }
    }
    Preset_Capture(&preset_file->slot[which]);
    preset_dirty = 1;
    Preset_Use(which);
    Options_Presets[PROPT_PICK].val = which;
    Preset_Describe(which);
    Labels_Refresh();
    deck.changed = 0;
    sprintf(buf, "Saved to %s", preset_names[which]);
    Toast(buf);
    OSReport("LLDECK save %d %s\n", which, preset_names[which]);
    SFX_PlayCommon(1);
}

// The quick menu stays up with the trigger let go. It closes after a while
// with no D-pad or trigger press (Quick Menu Idle), fading out over the
// last 3 seconds; the stick plays out of it: the game goes on at once and
// it fades out over Quick Menu Play, unless the D-pad or a trigger brings
// it back. B closes it. Saving wants a trigger in, so A, X and Y can't save
// by chance.
#define DECK_FADE 180
static void Deck_Play(int on)
{
    if (on && deck.demo)
    {
        Cues_Clear(); // the example goes; the real cues take over
        live_visible = 0;
        deck.demo = 0;
    }
    deck.play = on;
    deck_frozen = !on;
    OSReport("LLDECK %s %d\n", on ? "play" : "back", event_vars->game_timer);
}

static void Deck_Think(void)
{
    HSD_Pad *pad = PadGetMaster(Advance_Port());
    int down = pad->down, held = pad->held;
    int dpad = HSD_BUTTON_DPAD_UP | HSD_BUTTON_DPAD_DOWN | HSD_BUTTON_DPAD_LEFT | HSD_BUTTON_DPAD_RIGHT;
    int trig = (held & (HSD_TRIGGER_L | HSD_TRIGGER_R)) || pad->ftriggerLeft > 0.3f || pad->ftriggerRight > 0.3f;
    int trig_press = trig && !deck.trig_prev;
    deck.trig_prev = trig;
    // both triggers all the way with D-pad up or down: everything hidden, as outside
    if ((held & HSD_TRIGGER_L) && (held & HSD_TRIGGER_R) && (down & (HSD_BUTTON_DPAD_UP | HSD_BUTTON_DPAD_DOWN)))
    {
        Deck_Close("hide");
        Hide_Toggle();
        return;
    }
    if (down & HSD_BUTTON_B)
    {
        Deck_Close("B");
        return;
    }
    // saving, with a trigger in
    if (trig && (down & HSD_BUTTON_A))
    {
        Deck_Save(Options_Presets[PROPT_PICK].val);
        deck.idle = 0;
        if (deck.play)
            Deck_Play(0);
        return;
    }
    if (trig && (down & (HSD_BUTTON_X | HSD_BUTTON_Y)))
    {
        Deck_Save(-1);
        deck.idle = 0;
        if (deck.play)
            Deck_Play(0);
        return;
    }
    int stick = fabs(pad->fstickX) > 0.3f || fabs(pad->fstickY) > 0.3f || fabs(pad->fsubstickX) > 0.3f ||
                fabs(pad->fsubstickY) > 0.3f;
    if ((down & dpad) || trig_press)
    {
        deck.idle = 0;
        if (deck.play)
            Deck_Play(0);
    }
    else
        deck.idle++;
    if (stick && !deck.play && Options_Hud[HOPT_DECK_PLAY].val != 3)
        Deck_Play(1);
    else if (deck.play)
        deck.play++;

    static const int idle_frames[] = {180, 300, 480, 0}, play_frames[] = {60, 120, 180, 0};
    int idle_max = idle_frames[Options_Hud[HOPT_DECK_IDLE].val], play_max = play_frames[Options_Hud[HOPT_DECK_PLAY].val];
    deck.alpha = 1.f;
    if (idle_max && idle_max - deck.idle < DECK_FADE)
        deck.alpha = (float)(idle_max - deck.idle) / DECK_FADE;
    if (deck.play && play_max)
    {
        float a = 1.f - (float)deck.play / play_max;
        if (a < deck.alpha)
            deck.alpha = a;
    }
    if ((idle_max && deck.idle >= idle_max) || (deck.play && play_max && deck.play >= play_max))
    {
        Deck_Close(deck.play ? "playing" : "idle");
        return;
    }
    if (deck.play)
        return;
    if (!(down & dpad))
        return;

    int dir = down & HSD_BUTTON_DPAD_RIGHT ? 1 : down & HSD_BUTTON_DPAD_LEFT ? -1 : 0;
    if (deck.row < 0)
    {
        if (down & HSD_BUTTON_DPAD_DOWN)
            deck.row = 0;
        else if (dir)
        {
            // the next card with settings in it
            int cur = Options_Presets[PROPT_PICK].val;
            for (int n = 0; n < PS_COUNT; n++)
            {
                cur = Chord_Step(cur, PS_COUNT, dir);
                if (Deck_Shown(cur))
                    break;
            }
            Options_Presets[PROPT_PICK].val = cur;
            Preset_Apply(Preset_Slot(cur));
            Preset_Use(cur);
            Preset_Describe(cur);
            deck.changed = 0;
            Deck_Demo(Fighter_GetGObj(0)->userdata);
            OSReport("LLDECK card %d %s\n", cur, preset_names[cur]);
        }
    }
    else
    {
        if (down & HSD_BUTTON_DPAD_UP)
            deck.row--;
        else if (down & HSD_BUTTON_DPAD_DOWN)
            deck.row = deck.row + 1 < DECK_ROWS ? deck.row + 1 : deck.row;
        else if (dir)
        {
            DeckRow *r = &deck_rows[deck.row];
            Deck_Step(r->o, dir);
            if (r->o == &Options_Paths[POPT_PATH] || r->o == &Options_Jump[JOPT_SHOW])
                Event_ChangeRoutes(0, 0);
            deck.changed = 1;
            Deck_Demo(Fighter_GetGObj(0)->userdata);
            OSReport("LLDECK set %s = %d\n", r->label, r->o->val);
        }
    }
    SFX_PlayCommon(2);
}

static const char *Deck_Value(EventOption *o)
{
    if (o->kind == OPTKIND_TOGGLE)
        return o->val ? "On" : "Off";
    int v = o->val - o->value_min;
    return v >= 0 && v < o->value_num && o->values ? o->values[v] : "?";
}

static void Deck_Draw(void)
{
    vis_k = sqrtf(deck.alpha > 0.f ? deck.alpha : 0.f); // Vis takes alpha down by its square
    GXColor ink = {235, 235, 240, 255}, dim = {160, 166, 186, 255}, lit = land_kind_colors[LAND_AI];
    int pick = Options_Presets[PROPT_PICK].val;
    int n = Deck_Count();
    float total = n * DECK_CARD_W + (n - 1) * DECK_GAP;
    float x = -total / 2, pick_x = 0;
    int li = 0, mi = 0, ri = 0;

    // the frozen screen's corners, like a camera's viewfinder: a still, not play
    for (int c = 0; c < 4; c++)
    {
        float cx = c & 1 ? SAFE_W + 1.f : -SAFE_W - 1.f, cy = c & 2 ? -SAFE_H - 1.f : SAFE_H + 1.f;
        float sx = c & 1 ? -1.f : 1.f, sy = c & 2 ? 1.f : -1.f;
        Hud_Rect(cx < cx + sx * 2.f ? cx : cx + sx * 2.f, cy - 0.12f, cx < cx + sx * 2.f ? cx + sx * 2.f : cx, cy + 0.12f,
                 Color_Over(color_white, 0.5f));
        Hud_Rect(cx - 0.12f, cy < cy + sy * 2.f ? cy : cy + sy * 2.f, cx + 0.12f, cy < cy + sy * 2.f ? cy + sy * 2.f : cy,
                 Color_Over(color_white, 0.5f));
    }

    for (int i = 0; i < PS_COUNT; i++)
    {
        if (!Deck_Shown(i))
            continue;
        int on = i == pick;
        PresetSlot *slot = Preset_Slot(i);
        float y1 = DECK_TOP - (on ? 0.f : 0.5f), y0 = y1 - (on ? DECK_CARD_H : DECK_CARD_H - 0.8f);
        float a = on ? 1.f : 0.7f;
        Hud_Rect(x, y0, x + DECK_CARD_W, y1, Color_Over(color_plate, 0.9f));
        Hud_Frame(x, y0, x + DECK_CARD_W, y1, on ? 0.18f : 0.08f, Color_Over(on ? lit : dim, on ? 1.f : 0.5f));
        char name[20];
        Name_Copy(name, preset_names[i]);
        // the name fits its card: smaller when long
        float ns = 0.3f, nw = Text_Width(name, ns), room = DECK_CARD_W - 0.7f;
        if (nw > room)
            ns *= room / nw;
        Deck_Line(deck.mid, mi++, name, x + DECK_CARD_W / 2, y1 - 2.0f, ns, Color_Over(on ? color_white : ink, a));
        if (i > 0 && i < PS_SAVED && preset_file->preset_name[i][0])
            Hud_Disc(x + DECK_CARD_W - 0.45f, y1 - 0.45f, 0.18f, Color_Over(color_in_jump, a)); // one he named
        if (on && deck.changed)
            Hud_Rect(x + 0.3f, y0 - 0.75f, x + DECK_CARD_W - 0.3f, y0 - 0.25f, Color_Over(color_in_jump, 1.f)); // not saved yet
        // what the preset has on: a mark each, lit when on
        for (int m = 0; m < (int)countof(deck_marks); m++)
        {
            EventOption *o = deck_marks[m].o;
            int v = on ? o->val : Preset_Value(slot, o);
            int lit_m = v != 0;
            float mx = x + 0.45f + (m % 4) * 1.2f, my = y1 - 3.3f - (m / 4) * 0.95f;
            GXColor mc = land_kind_colors[deck_marks[m].kind];
            if (lit_m)
                Hud_Rect(mx, my, mx + 0.9f, my + 0.6f, Color_Over(mc, 0.95f * a));
            else
                Hud_Frame(mx, my, mx + 0.9f, my + 0.6f, 0.06f, Color_Over(mc, 0.35f * a));
        }
        // its intensity, in bars
        int lv = on ? Options_Cues[COPT_INTENSITY].val : Preset_Value(slot, &Options_Cues[COPT_INTENSITY]);
        for (int b = 0; b <= lv && b < 5; b++)
            Hud_Rect(x + 0.45f + b * 0.5f, y0 + 0.35f, x + 0.8f + b * 0.5f, y0 + 0.75f, Color_Over(dim, a));
        if (on)
            pick_x = x;
        x += DECK_CARD_W + DECK_GAP;
    }

    float hy = DECK_TOP - DECK_CARD_H - 1.0f;
    // A writes over the chosen preset only when it's on the memory card
    const char *save_hint = pick < PS_SAVED ? "L/R + A: save to this preset    L/R + X: save as a new one"
                                            : "Built in:  L/R + A or X saves a new preset";
    if (deck.row < 0)
    {
        char both[112];
        sprintf(both, "%s    Down: settings", save_hint);
        const char *hint = deck.changed ? both : "Left/Right: preset    Down: its settings    B: close";
        float w = Text_Width(hint, 0.32f) / 2 + 0.6f;
        Hud_Rect(-w, hy - 2.0f, w, hy, Color_Over(color_plate, 0.85f));
        Deck_Line(deck.mid, mi++, hint, 0, hy - 2.2f, 0.32f, dim);
    }
    else
    {
        float px = pick_x + DECK_CARD_W / 2 - DECK_ROW_W / 2;
        if (px < -SAFE_W)
            px = -SAFE_W;
        if (px + DECK_ROW_W > SAFE_W)
            px = SAFE_W - DECK_ROW_W;
        // a window of the rows that keeps the chosen one in it
        int top = deck.row - DECK_VIEW / 2;
        if (top > DECK_ROWS - DECK_VIEW)
            top = DECK_ROWS - DECK_VIEW;
        if (top < 0)
            top = 0;
        int end = top + DECK_VIEW < DECK_ROWS ? top + DECK_VIEW : DECK_ROWS;
        float y1 = hy, y0 = y1 - (end - top) * DECK_ROW_H - 1.2f;
        Hud_Rect(px, y0, px + DECK_ROW_W, y1, Color_Over(color_plate, 0.9f));
        Hud_Frame(px, y0, px + DECK_ROW_W, y1, 0.08f, Color_Over(dim, 0.5f));
        // more above or below: a small bar at that edge
        if (top > 0)
            Hud_Rect(px + DECK_ROW_W / 2 - 1.f, y1 - 0.4f, px + DECK_ROW_W / 2 + 1.f, y1 - 0.2f, Color_Over(dim, 0.8f));
        if (end < DECK_ROWS)
            Hud_Rect(px + DECK_ROW_W / 2 - 1.f, y0 + 0.2f, px + DECK_ROW_W / 2 + 1.f, y0 + 0.4f, Color_Over(dim, 0.8f));
        for (int r = top; r < end; r++)
        {
            float ry = y1 - 0.6f - (r - top + 1) * DECK_ROW_H;
            int sel = r == deck.row;
            if (sel)
                Hud_Rect(px + 0.3f, ry, px + DECK_ROW_W - 0.3f, ry + DECK_ROW_H, Color_Over(lit, 0.25f));
            Deck_Line(deck.left, li++, deck_rows[r].label, px + 0.8f, ry - 0.45f, 0.34f, sel ? color_white : ink);
            char val[40];
            sprintf(val, sel ? "< %s >" : "%s", Deck_Value(deck_rows[r].o));
            Deck_Line(deck.right, ri++, val, px + DECK_ROW_W - 0.8f, ry - 0.45f, 0.34f, sel ? lit : dim);
        }
        if (deck.changed)
        {
            const char *hint = save_hint;
            float w = Text_Width(hint, 0.3f) + 1.2f;
            Hud_Rect(px, y0 - 2.1f, px + w, y0 - 0.1f, Color_Over(color_plate, 0.85f));
            Deck_Line(deck.left, li++, hint, px + 0.6f, y0 - 2.3f, 0.3f, dim);
        }
    }
    // the lines not used this frame
    while (li < DECK_TEXTS)
        Deck_Line(deck.left, li++, "", 0, 0, 0.1f, ink);
    while (ri < DECK_TEXTS)
        Deck_Line(deck.right, ri++, "", 0, 0, 0.1f, ink);
    while (mi < DECK_TEXTS)
        Deck_Line(deck.mid, mi++, "", 0, 0, 0.1f, ink);
    vis_k = 1.f;
}
