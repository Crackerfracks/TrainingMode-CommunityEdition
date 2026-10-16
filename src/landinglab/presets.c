/* landinglab/presets.c
 *
 * Holds settings presets: capture, apply, the built-in ones, and their menu
 * callbacks.
 *
 * Part of Landing Gear: src/landinglab.c includes it, in order, into one
 * translation unit, so it sees everything included before it (and mex.h,
 * events.h, landinglab_sim.h).
 *
 * Uses: option_values, menus_practice, menus_display.
 * Refers ahead, through forward declarations, to: memory_card, camera_views,
 * ledge_practice, reset_drill.
 */

///////////////////////
/// Presets         ///
///////////////////////

// A preset is a list of (option, value) pairs, each option known by a hash
// of its menu's and its own name, so a later version with options added,
// removed or moved still reads an older file: what it doesn't know stays as
// it is.
typedef struct PresetOpt
{
    u32 key;
    s16 val;
    u16 pad;
} PresetOpt;

// 72 until version 5, when the menus had 73 options to keep and Path Detail
// made 74: the last ones (Speed's) fell off the end unsaved. Room to grow.
#define PRESET_OPTS 96
typedef struct PresetSlot
{
    u16 used;
    u16 num;
    PresetOpt opt[PRESET_OPTS];
} PresetSlot;

#define PRESET_MAGIC 0x4C4C5052 // "LLPR"
// 4: the built-in presets were reordered (see Card_Load)
// 5: a preset keeps up to 96 options, not 72 (Card_Load reads a version 4
//    file's slots into the new layout)
#define PRESET_VERSION 5
#define NAME_LEN 12   // letters in a view's or preset's name
#define VIEW_POOL 48  // views kept, all stages together
typedef struct OldView // version 2's, one set for every stage
{
    u8 used;
    u8 pad[3];
    Vec3 eye, interest;
    float fov;
} OldView;
typedef struct CamView
{
    u8 used;
    u8 stage; // external stage id
    u8 slot;  // 1 to VIEW_SLOTS
    u8 pad;
    char name[NAME_LEN + 4];
    Vec3 eye, interest;
    float fov;
} CamView;
typedef struct PresetFile
{
    u32 magic;
    u16 version;
    u8 start; // the preset loaded at start
    u8 pad;
    PresetSlot slot[PS_SAVED];
    OldView old_view[4];                   // version 2's views: moved to the first stage it's read on
    char preset_name[PS_SAVED][NAME_LEN + 4]; // [0], User Custom, isn't named
    CamView view[VIEW_POOL];
    float skill[4]; // Auto Fade's hit rates, by cue kind
} PresetFile;

// The file up to version 4: the same, with 72 options a slot. Read and
// moved into the layout above (Preset_FromV4), so nothing saved is lost.
#define PRESET_OPTS_V4 72
typedef struct PresetSlotV4
{
    u16 used;
    u16 num;
    PresetOpt opt[PRESET_OPTS_V4];
} PresetSlotV4;
typedef struct PresetFileV4
{
    u32 magic;
    u16 version;
    u8 start;
    u8 pad;
    PresetSlotV4 slot[PS_SAVED];
    OldView old_view[4];
    char preset_name[PS_SAVED][NAME_LEN + 4];
    CamView view[VIEW_POOL];
    float skill[4];
} PresetFileV4;

static void Preset_FromV4(PresetFile *to, const PresetFileV4 *from)
{
    memset(to, 0, sizeof(*to));
    to->magic = from->magic;
    to->version = from->version;
    to->start = from->start;
    for (int i = 0; i < PS_SAVED; i++)
    {
        to->slot[i].used = from->slot[i].used;
        to->slot[i].num = from->slot[i].num <= PRESET_OPTS_V4 ? from->slot[i].num : PRESET_OPTS_V4;
        memcpy(to->slot[i].opt, from->slot[i].opt, sizeof(from->slot[i].opt));
    }
    memcpy(to->old_view, from->old_view, sizeof(to->old_view));
    memcpy(to->preset_name, from->preset_name, sizeof(to->preset_name));
    memcpy(to->view, from->view, sizeof(to->view));
    memcpy(to->skill, from->skill, sizeof(to->skill));
}
static float skill[4]; // the same, as they go (CUE_NUM used)
static CamView *View_Find(int v);
static CamView *View_New(int v);

// a name, cut to NAME_LEN letters
static void Name_Copy(char *to, const char *from)
{
    int i = 0;
    for (; i < NAME_LEN && from[i]; i++)
        to[i] = from[i];
    to[i] = 0;
}
static void Labels_Refresh(void);

typedef struct PresetMenu
{
    const char *tag;
    EventOption *opts;
    int num;
} PresetMenu;

// Every menu a player sets up: not Developer, and not Frame Advance or the
// chosen ledge route, which belong to the moment.
static const PresetMenu preset_menus[] = {
    {"Cues", Options_Cues, COPT_COUNT},
    {"Intensity", Options_Intensity, IOPT_COUNT},
    {"Rumble", Options_Rumble, RUOPT_COUNT},
    {"Timers", Options_Timers, TOPT_COUNT},
    {"Paths", Options_Paths, POPT_COUNT},
    {"HUD", Options_Hud, HOPT_COUNT},
    {"Sounds", Options_Sounds, SOPT_COUNT},
    {"Ledge", Options_Ledge, LOPT_COUNT},
    {"Jump", Options_Jump, JOPT_COUNT},
    {"Camera", Options_Camera, CAMOPT_COUNT},
    {"Speed", Options_Game, GOPT_COUNT},
};

static PresetFile *preset_file;    // what's on the card, and what gets written to it
static PresetSlot *preset_builtin; // [PS_COUNT - PS_SAVED]
static int preset_active;          // the preset the settings came from
static PresetSlot *preset_as_set;  // and the settings as it set them
static u8 preset_dirty;            // preset_file changed since it was last written
static u8 preset_was_paused;
static u8 preset_cam_pending; // a preset changed the camera mode
static u8 view_pending;       // ... or its view

static int Preset_Kept(EventOption *o)
{
    if (o == &Options_Ledge[LOPT_PICK] || o == &Options_Game[GOPT_FRAME_ADV])
        return 0;
    return o->kind == OPTKIND_STRING || o->kind == OPTKIND_INT || o->kind == OPTKIND_TOGGLE;
}

static u32 Preset_Hash(u32 h, const char *t)
{
    for (; *t; t++)
        h = (h ^ (u8)*t) * 16777619u;
    return h;
}

static u32 Preset_Key(const char *tag, EventOption *o)
{
    return Preset_Hash(Preset_Hash(2166136261u, tag) * 16777619u, o->name);
}

// The key of an option from any of the menus above, 0 if it's in none.
static u32 Preset_KeyOf(EventOption *o)
{
    for (int m = 0; m < (int)countof(preset_menus); m++)
        if (o >= preset_menus[m].opts && o < preset_menus[m].opts + preset_menus[m].num)
            return Preset_Key(preset_menus[m].tag, o);
    return 0;
}

static PresetSlot *Preset_Slot(int i)
{
    return i < PS_SAVED ? &preset_file->slot[i] : &preset_builtin[i - PS_SAVED];
}

static void Preset_Capture(PresetSlot *s)
{
    s->num = 0;
    for (int m = 0; m < (int)countof(preset_menus); m++)
        for (int i = 0; i < preset_menus[m].num; i++)
        {
            EventOption *o = &preset_menus[m].opts[i];
            if (!Preset_Kept(o) || s->num >= PRESET_OPTS)
                continue;
            PresetOpt *p = &s->opt[s->num++];
            p->key = Preset_Key(preset_menus[m].tag, o);
            p->val = Reference_Preference(o);
            p->pad = 0;
        }
    s->used = 1;
}

static PresetOpt *Preset_Find(PresetSlot *s, u32 key)
{
    for (int i = 0; i < s->num && i < PRESET_OPTS; i++)
        if (s->opt[i].key == key)
            return &s->opt[i];
    return 0;
}

// What the preset sets an option to: its own value when the preset doesn't
// have it.
static int Preset_Value(PresetSlot *s, EventOption *o)
{
    PresetOpt *p = Preset_Find(s, Preset_KeyOf(o));
    return p ? p->val : o->val;
}

static int Preset_Fits(EventOption *o, int v)
{
    if (o->kind == OPTKIND_TOGGLE)
        return v == 0 || v == 1;
    return v >= o->value_min && v - o->value_min < o->value_num;
}

static void Preset_Apply(PresetSlot *s)
{
    if (!s->used)
        return;
    int cam = Options_Camera[CAMOPT_MODE].val, view = Options_Camera[CAMOPT_VIEW].val, start = Options_Ledge[LOPT_START].val;
    for (int m = 0; m < (int)countof(preset_menus); m++)
        for (int i = 0; i < preset_menus[m].num; i++)
        {
            EventOption *o = &preset_menus[m].opts[i];
            if (!Preset_Kept(o))
                continue;
            PresetOpt *p = Preset_Find(s, Preset_Key(preset_menus[m].tag, o));
            if (p && Preset_Fits(o, p->val))
                o->val = p->val;
        }
    // the few settings that act when they change; the rest are read as they're
    // used. The camera waits for the next frame: this can run at start, before
    // the match's camera is ready.
    if (Options_Camera[CAMOPT_MODE].val != cam)
        preset_cam_pending = 1;
    if (Options_Camera[CAMOPT_VIEW].val != view && Options_Camera[CAMOPT_VIEW].val)
        view_pending = 1;
    if (Options_Ledge[LOPT_START].val != start)
        Event_ChangeLedgeStart(0, Options_Ledge[LOPT_START].val);
    Event_ChangeRoutes(0, 0);
}

static int Preset_Same(PresetSlot *a, PresetSlot *b)
{
    if (a->used != b->used || a->num != b->num)
        return 0;
    for (int i = 0; i < a->num && i < PRESET_OPTS; i++)
        if (a->opt[i].key != b->opt[i].key || a->opt[i].val != b->opt[i].val)
            return 0;
    return 1;
}

// The settings now came from preset which. Until they're changed by hand,
// closing the menu leaves User Custom alone.
static void Preset_Use(int which)
{
    preset_active = which;
    Preset_Capture(preset_as_set);
}

// The built-in presets, as changes to the defaults.
typedef struct PresetSet
{
    EventOption *o;
    s16 val;
} PresetSet;

// AIs with nothing fixed on screen, for just playing: the landing spot, the
// aerial picker and the body flash
static const PresetSet preset_quiet[] = {
    {&Options_Cues[COPT_WL], 0},
    {&Options_Timers[TOPT_NEAR], NEAR_OFF},
    {&Options_Timers[TOPT_STRIP], STRIP_OFF},
    {&Options_Timers[TOPT_SPOT], 1},
    {&Options_Paths[POPT_INPUTS], 1},
    {&Options_Hud[HOPT_STICK], STICK_OFF},
    {&Options_Hud[HOPT_PANEL], 0},
    {&Options_Sounds[SOPT_CHIME], 1},
};

// Wavelands and wavedashes, timed at the feet (Stephen asked for a wavedash
// timer: this is where it lives)
static const PresetSet preset_wavedash[] = {
    {&Options_Cues[COPT_NIL], 0},
    {&Options_Cues[COPT_AI], 0},
    {&Options_Cues[COPT_WL], 2},
    {&Options_Cues[COPT_GLOW], 1},
    {&Options_Timers[TOPT_STRIP], STRIP_CONVEYOR},
    {&Options_Timers[TOPT_WL], WLT_TICKS},
    {&Options_Timers[TOPT_WD], WDT_PIPS},
    {&Options_Paths[POPT_SLIDEOFF], 1},
    {&Options_Hud[HOPT_PAD_CUES], PADCUE_RING},
};

// Ledge routes from the ledge, both kinds, reset to the same ledge each time
static const PresetSet preset_ledge[] = {
    {&Options_Ledge[LOPT_ROUTES], 1},
    {&Options_Ledge[LOPT_KIND], ROUTES_BOTH},
    {&Options_Ledge[LOPT_START], START_LEDGE},
    {&Options_Ledge[LOPT_RESET], RESET_SAME},
    {&Options_Ledge[LOPT_DELAY], 1},
    {&Options_Cues[COPT_NIL], 1},
    {&Options_Cues[COPT_AI], 1},
    {&Options_Cues[COPT_WL], 0},
    {&Options_Paths[POPT_ROUTE], 1},
    {&Options_Paths[POPT_DETAIL], DETAIL_FULL}, // the ledge is a fixed anchor: the whole route holds still (Fable's live paths plan)
    {&Options_Paths[POPT_INPUTS], 1},
    {&Options_Paths[POPT_TICKS], 1},
    {&Options_Timers[TOPT_STRIP], STRIP_CONVEYOR},
    {&Options_Sounds[SOPT_ROUTE_CHIME], 1},
    {&Options_Sounds[SOPT_LOST], 1},
};

// Every cue and path, to explore with, not to play with: Pincers, which read
// best and never cover the body, no body path or jump previews, and the
// frame dots only in Frame Advance
static const PresetSet preset_everything[] = {
    {&Options_Cues[COPT_NIL], 1},
    {&Options_Cues[COPT_AI], 1},
    {&Options_Cues[COPT_WL], 2},
    {&Options_Cues[COPT_FLASH], 1},
    {&Options_Cues[COPT_GLOW], 1},
    {&Options_Timers[TOPT_NEAR], NEAR_PINCERS},
    {&Options_Timers[TOPT_STRIP], STRIP_CONVEYOR},
    {&Options_Timers[TOPT_SPOT], 1},
    {&Options_Timers[TOPT_WL], WLT_TICKS},
    {&Options_Timers[TOPT_WD], WDT_CELLS},
    {&Options_Paths[POPT_PATH], 1},
    {&Options_Paths[POPT_BODY], 0},
    // Lead, as the default: exploring at full speed is what the whole arcs
    // ruined, and Frame Advance shows them (Fable's live paths plan)
    {&Options_Paths[POPT_DETAIL], DETAIL_LEAD},
    {&Options_Paths[POPT_ROUTE], 1},
    {&Options_Paths[POPT_INPUTS], 1},
    {&Options_Paths[POPT_WINDOWS], 1},
    {&Options_Paths[POPT_TICKS], 0},
    {&Options_Paths[POPT_SLIDEOFF], 1},
    {&Options_Paths[POPT_PREVIEW], PREVIEW_OFF},
    {&Options_Jump[JOPT_SHOW], 1},
    {&Options_Jump[JOPT_PATH], 1},
    {&Options_Jump[JOPT_TEXT], 1}, // the plan in words, off by default since 0.9.30
#ifdef LL_GROUND_GUIDE
    {&Options_Jump[JOPT_BANDS], 1},
    {&Options_Jump[JOPT_MARKS], 1},
#endif
    {&Options_Hud[HOPT_STICK], STICK_PERCENT},
    {&Options_Hud[HOPT_PANEL], 1},
    {&Options_Sounds[SOPT_SKIP], 1},
};

static void Preset_Build(PresetSlot *s, PresetSlot *base, const PresetSet *set, int n)
{
    memcpy(s, base, sizeof(*s));
    for (int i = 0; i < n; i++)
    {
        PresetOpt *p = Preset_Find(s, Preset_KeyOf(set[i].o));
        if (p)
            p->val = set[i].val;
    }
}

// The chosen preset's settings in three short lines, for the menu.
static void Preset_Describe(int which)
{
    PresetSlot *s = Preset_Slot(which);
    if (!s->used)
    {
        sprintf(preset_desc[0], "Empty: Save to Preset keeps the current");
        sprintf(preset_desc[1], "settings here.");
        preset_desc[2][0] = 0;
        return;
    }
    char *t = preset_desc[0];
    int nil = Preset_Value(s, &Options_Cues[COPT_NIL]), ai = Preset_Value(s, &Options_Cues[COPT_AI]);
    int wl = Preset_Value(s, &Options_Cues[COPT_WL]);
    int n = 0;
    t += sprintf(t, "Cues:");
    if (ai)
        t += sprintf(t, "%s AI", n++ ? "," : "");
    if (nil)
        t += sprintf(t, "%s NIL", n++ ? "," : "");
    if (wl)
        t += sprintf(t, "%s waveland", n++ ? "," : "");
    if (!n)
        t += sprintf(t, " none");
    // the biggest difference on screen between two cards
    sprintf(t, ". Jump timing %s", Preset_Value(s, &Options_Jump[JOPT_SHOW]) ? "on" : "off");

    int near = Preset_Value(s, &Options_Timers[TOPT_NEAR]), strip = Preset_Value(s, &Options_Timers[TOPT_STRIP]);
    t = preset_desc[1];
    t += sprintf(t, "Timers: %s strip", strip ? strip_names[strip] : "no");
    if (near != NEAR_OFF)
        sprintf(t, ", %s near you", near_names[near]);

    int path = Preset_Value(s, &Options_Paths[POPT_PATH]), body = Preset_Value(s, &Options_Paths[POPT_BODY]);
    int route = Preset_Value(s, &Options_Paths[POPT_ROUTE]), pad = Preset_Value(s, &Options_Hud[HOPT_STICK]);
    t = preset_desc[2];
    n = 0;
    t += sprintf(t, "Paths:");
    if (path)
        t += sprintf(t, "%s landing", n++ ? "," : "");
    if (body)
        t += sprintf(t, "%s body", n++ ? "," : "");
    if (route)
        t += sprintf(t, "%s ledge", n++ ? "," : "");
    if (!n)
        t += sprintf(t, " none");
    sprintf(t, ". Pad %s", pad == 3 ? "off" : "on");
}

static void Card_Update(void);
static void Card_Load(void);

void Event_ChangePreset(GOBJ *menu, int value)
{
    Preset_Describe(value);
}

void Event_ChangePresetStart(GOBJ *menu, int value)
{
    preset_file->start = value;
    preset_dirty = 1;
}

void Event_PresetLoad(GOBJ *menu)
{
    int which = Options_Presets[PROPT_PICK].val;
    PresetSlot *s = Preset_Slot(which);
    if (!s->used)
    {
        SFX_PlayCommon(3);
        return;
    }
    Preset_Apply(s);
    Preset_Use(which);
    OSReport("LLPRESET load %s\n", preset_names[which]);
    SFX_PlayCommon(1);
}

void Event_PresetSave(GOBJ *menu)
{
    int which = Options_Presets[PROPT_PICK].val;
    if (which >= PS_SAVED)
    {
        SFX_PlayCommon(3); // built in
        OSReport("LLPRESET save refused: %s is built in\n", preset_names[which]);
        return;
    }
    Preset_Capture(&preset_file->slot[which]);
    preset_dirty = 1;
    Preset_Use(which);
    OSReport("LLPRESET save %s, %d settings\n", preset_names[which], preset_file->slot[which].num);
    Preset_Describe(which);
    SFX_PlayCommon(1);
}

// At start: the defaults and the built-in presets are made from the menus as
// the event sets them, then the card is read.
static void Presets_Init(void)
{
    preset_file = calloc(sizeof(PresetFile));
    preset_builtin = calloc(sizeof(PresetSlot) * (PS_COUNT - PS_SAVED));
    preset_as_set = calloc(sizeof(PresetSlot));
    preset_file->magic = PRESET_MAGIC;
    preset_file->version = PRESET_VERSION;
    preset_file->start = PS_USER;
    PresetSlot *def = &preset_builtin[PS_DEFAULTS - PS_SAVED];
    Preset_Capture(def);
    Preset_Build(&preset_builtin[PS_QUIET - PS_SAVED], def, preset_quiet, countof(preset_quiet));
    Preset_Build(&preset_builtin[PS_WAVEDASH - PS_SAVED], def, preset_wavedash, countof(preset_wavedash));
    Preset_Build(&preset_builtin[PS_LEDGE - PS_SAVED], def, preset_ledge, countof(preset_ledge));
    Preset_Build(&preset_builtin[PS_EVERYTHING - PS_SAVED], def, preset_everything, countof(preset_everything));
    Options_Presets[PROPT_START].val = PS_USER;
    Labels_Refresh();
    Preset_Describe(Options_Presets[PROPT_PICK].val);
    Card_Load();
}

// The card's file was read (or there is none): start with the preset it
// asks for.
static void Presets_Loaded(int ok)
{
    if (!ok || preset_file->magic != PRESET_MAGIC || preset_file->version != PRESET_VERSION)
    {
        memset(preset_file, 0, sizeof(*preset_file));
        preset_file->magic = PRESET_MAGIC;
        preset_file->version = PRESET_VERSION;
        preset_file->start = PS_USER;
    }
    // version 2's views, from before views were per stage, go to this one
    for (int i = 0; i < 4; i++)
    {
        OldView *o = &preset_file->old_view[i];
        CamView *w = o->used ? View_New(i + 1) : 0;
        if (w)
        {
            w->eye = o->eye;
            w->interest = o->interest;
            w->fov = o->fov;
            w->used = 1;
            preset_dirty = 1;
        }
        memset(o, 0, sizeof(*o));
    }
    for (int i = 0; i < PS_SAVED; i++)
        preset_file->preset_name[i][NAME_LEN] = 0;
    for (int i = 0; i < VIEW_POOL; i++)
        preset_file->view[i].name[NAME_LEN] = 0;
    for (int i = 0; i < 4; i++)
    {
        float k = preset_file->skill[i];
        skill[i] = k >= 0.f && k <= 1.f ? k : 0.f; // (a NaN fails both)
    }
    Labels_Refresh();
    int start = preset_file->start < PS_COUNT ? preset_file->start : PS_USER;
    Options_Presets[PROPT_START].val = start;
    Preset_Apply(Preset_Slot(start));
    Preset_Use(start);
    OSReport("LLPRESET start %s (%s), card file %s\n", preset_names[start], Preset_Slot(start)->used ? "applied" : "empty", ok ? "read" : "not read");
    Preset_Describe(Options_Presets[PROPT_PICK].val);
}
