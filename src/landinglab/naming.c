/* landinglab/naming.c
 *
 * Holds the letter grid for naming views and presets, and the per-frame
 * preset upkeep.
 *
 * Part of Landing Lab: src/landinglab.c includes it, in order, into one
 * translation unit, so it sees everything included before it (and mex.h,
 * events.h, landinglab_sim.h).
 *
 * Uses: menus_practice, menus_display, presets, memory_card, camera_views.
 * Refers ahead, through forward declarations, to: reset_drill.
 */

///////////////////////
/// Naming          ///
///////////////////////

// Views and presets are named on a letter grid drawn over the paused game,
// in place of the menu: the stick or D-pad picks a key, A types it, B
// deletes (or leaves, with nothing left to delete), Y types a space, X
// switches case and Start keeps the name.
enum { NAME_VIEW, NAME_PRESET };
#define NAMER_COLS 10
#define NAMER_KEYS 4 // the wide keys on the last row
#define NAMER_ROWS 5
#define NAMER_SUBTEXTS (4 * NAMER_COLS + NAMER_KEYS)
static const char *namer_rows[2][4] = {
    {"ABCDEFGHIJ", "KLMNOPQRST", "UVWXYZ-.'!", "1234567890"},
    {"abcdefghij", "klmnopqrst", "uvwxyz-.'!", "1234567890"},
};
static struct
{
    u8 on, what, slot, lower, dirty, leave_menu;
    int cx, cy, len;
    char buf[NAME_LEN + 1];
    char title[48];
    Text *text;
} namer;
static int namer_port; // the controller that opened the menu

static const char *Stage_Name(void)
{
    switch (Stage_GetExternalID())
    {
    case GRKINDEXT_BATTLE:
        return "Battlefield";
    case GRKINDEXT_FD:
        return "Final Destination";
    case GRKINDEXT_OLDPU:
        return "Dream Land";
    case GRKINDEXT_STORY:
        return "Yoshi's Story";
    case GRKINDEXT_IZUMI:
        return "Fountain of Dreams";
    case GRKINDEXT_PSTAD:
        return "Pokemon Stadium";
    }
    return "this stage";
}

// The grid is drawn on the HUD, which the game doesn't draw while paused:
// the pause menu closes for it, and the game stays frozen the way Frame
// Advance freezes it (Advance_CheckPause) until it's done, when the menu
// opens again.
static void Namer_Open(int what, int slot, const char *name)
{
    MenuData *md = event_vars->menu_gobj->userdata;
    namer_port = md->controller_index;
    HUDCamData *hud = event_vars->hudcam_gobj->userdata;
    memset(&namer, 0, sizeof(namer));
    namer.what = what;
    namer.slot = slot;
    Name_Copy(namer.buf, name);
    namer.len = strlen(namer.buf);
    namer.lower = namer.len > 0;
    if (what == NAME_VIEW)
        sprintf(namer.title, "Name view %d on %s", slot, Stage_Name());
    else
        sprintf(namer.title, "Name preset %d", slot);
    Text *t = Text_CreateText(2, hud->canvas);
    t->kerning = 1;
    t->align = 1;
    t->use_aspect = 0;
    t->is_depth_compare = 0;
    t->viewport_scale.X = 0.1f;
    t->viewport_scale.Y = 0.1f;
    for (int i = 0; i < NAMER_SUBTEXTS; i++)
        Text_AddSubtext(t, 0, 0, "");
    namer.text = t;
    namer.dirty = 1;
    namer.on = 1;
    namer.leave_menu = 1; // next frame: not from inside the menu's own call
    SFX_PlayCommon(1);
}

// reopen: back to the menu (Start opens it by itself)
static void Namer_Close(int keep, int reopen)
{
    MenuData *md = event_vars->menu_gobj->userdata;
    if (keep)
    {
        while (namer.len > 0 && namer.buf[namer.len - 1] == ' ')
            namer.buf[--namer.len] = 0;
        char *to = 0;
        if (namer.what == NAME_VIEW)
        {
            CamView *w = View_Find(namer.slot);
            to = w ? w->name : 0;
        }
        else
            to = preset_file->preset_name[namer.slot];
        if (to)
        {
            memset(to, 0, NAME_LEN + 4);
            strcpy(to, namer.buf);
            preset_dirty = 1;
            Labels_Refresh();
            OSReport("LLNAME %s %d \"%s\"\n", namer.what == NAME_VIEW ? "view" : "preset", namer.slot, namer.buf);
        }
    }
    if (namer.text)
        Text_Destroy(namer.text);
    namer.text = 0;
    namer.on = 0;
    if (reopen && Pause_CheckStatus(1) != 2)
    {
        event_vars->Menu_Enter(event_vars->menu_gobj);
        md->controller_index = namer_port;
    }
    SFX_PlayCommon(keep ? 1 : 0);
}

static void Namer_Type(char c)
{
    if (namer.len >= NAME_LEN)
    {
        SFX_PlayCommon(3);
        return;
    }
    namer.buf[namer.len++] = c;
    namer.buf[namer.len] = 0;
    // a capital to start with, then small letters, as names are written
    if (namer.len == 1 && c >= 'A' && c <= 'Z')
        namer.lower = 1;
    SFX_PlayCommon(1);
}

// Each frame while it's up, from Event_Update.
static void Namer_Think(void)
{
    HSD_Pad *pad = PadGetMaster(namer_port);
    int rep = pad->repeat, down = pad->down;
    int cols = namer.cy == NAMER_ROWS - 1 ? NAMER_KEYS : NAMER_COLS;
    int prev_cx = namer.cx, prev_cy = namer.cy;
    if (rep & (HSD_BUTTON_LEFT | HSD_BUTTON_DPAD_LEFT))
        namer.cx = (namer.cx + cols - 1) % cols;
    else if (rep & (HSD_BUTTON_RIGHT | HSD_BUTTON_DPAD_RIGHT))
        namer.cx = (namer.cx + 1) % cols;
    else if (rep & (HSD_BUTTON_UP | HSD_BUTTON_DPAD_UP | HSD_BUTTON_DOWN | HSD_BUTTON_DPAD_DOWN))
    {
        int dy = rep & (HSD_BUTTON_UP | HSD_BUTTON_DPAD_UP) ? NAMER_ROWS - 1 : 1;
        int from_keys = namer.cy == NAMER_ROWS - 1;
        namer.cy = (namer.cy + dy) % NAMER_ROWS;
        int to_keys = namer.cy == NAMER_ROWS - 1;
        // the wide keys sit under the columns they span
        if (to_keys && !from_keys)
            namer.cx = namer.cx * NAMER_KEYS / NAMER_COLS;
        else if (from_keys && !to_keys)
            namer.cx = namer.cx * NAMER_COLS / NAMER_KEYS + 1;
    }
    if (namer.cx != prev_cx || namer.cy != prev_cy)
    {
        namer.dirty = 1;
        SFX_PlayCommon(2);
    }

    if (down & HSD_BUTTON_START)
    {
        Namer_Close(1, 0);
        return;
    }
    if (down & HSD_BUTTON_A)
    {
        if (namer.cy < NAMER_ROWS - 1)
            Namer_Type(namer_rows[namer.lower][namer.cy][namer.cx]);
        else if (namer.cx == 0)
            Namer_Type(' ');
        else if (namer.cx == 1 && namer.len > 0)
        {
            namer.buf[--namer.len] = 0;
            SFX_PlayCommon(0);
        }
        else if (namer.cx == 2)
            namer.lower ^= 1;
        else if (namer.cx == 3)
        {
            Namer_Close(1, 1);
            return;
        }
        namer.dirty = 1;
    }
    else if (down & HSD_BUTTON_B)
    {
        if (namer.len == 0)
        {
            Namer_Close(0, 1);
            return;
        }
        namer.buf[--namer.len] = 0;
        SFX_PlayCommon(0);
    }
    else if (down & HSD_BUTTON_Y)
        Namer_Type(' ');
    else if (down & HSD_BUTTON_X)
    {
        namer.lower ^= 1;
        namer.dirty = 1;
    }
}

void Event_NameView(GOBJ *menu)
{
    int v = Options_Camera[CAMOPT_VIEW].val;
    CamView *w = v > 0 ? View_Find(v) : 0;
    if (!w)
    {
        SFX_PlayCommon(3); // pick a saved view first
        return;
    }
    Namer_Open(NAME_VIEW, v, w->name);
}

void Event_PresetName(GOBJ *menu)
{
    int p = Options_Presets[PROPT_PICK].val;
    if (p < PS_1 || p >= PS_SAVED)
    {
        SFX_PlayCommon(3); // User Custom and the built-in ones keep their names
        return;
    }
    Namer_Open(NAME_PRESET, p, preset_file->preset_name[p]);
}

// Each frame: closing the menu keeps the settings in User Custom, and the
// card is written when anything it holds changed.
static void Presets_Update(void)
{
    int paused = Pause_CheckStatus(1) == 2;
    if (preset_was_paused && !paused)
        Presets_KeepUser();
    // Auto Fade's hit rates go to the card when the menu opens, not after
    // every try
    if (paused && !preset_was_paused)
    {
        for (int i = 0; i < 4; i++)
        {
            if (preset_file->skill[i] != skill[i])
                preset_dirty = 1;
            preset_file->skill[i] = skill[i];
        }
    }
    preset_was_paused = paused;
    if (preset_cam_pending)
    {
        preset_cam_pending = 0;
        Event_ChangeCamera(0, Options_Camera[CAMOPT_MODE].val);
    }
    if (view_pending)
    {
        view_pending = 0;
        View_Apply(Options_Camera[CAMOPT_VIEW].val);
    }
    Card_Update();
}
