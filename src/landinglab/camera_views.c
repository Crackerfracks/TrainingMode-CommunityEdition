/* landinglab/camera_views.c
 *
 * Holds saved camera views (Advanced camera mode) and their menu callbacks.
 *
 * Part of Landing Gear: src/landinglab.c includes it, in order, into one
 * translation unit, so it sees everything included before it (and mex.h,
 * events.h, landinglab_sim.h).
 *
 * Uses: option_values, menus_practice, menus_display, presets.
 * Refers ahead, through forward declarations, to: reset_drill.
 */

// Camera views: where the camera's eye is, what it looks at, and its field
// of view. A view is held in Advanced mode, the game's develop camera, which
// puts the camera each frame where its own state says (decomp
// CameraDebugMode at 0x80453004); a view is written there.
typedef struct DevCam
{
    int last_mode, ply_slot;
    Vec3 follow_int_offset, follow_eye_offset, follow_eye_pos, follow_int_pos;
    float follow_fov;
    Vec3 free_int_pos, free_eye_pos;
    float free_fov;
} DevCam;
#define dev_cam ((DevCam *)0x80453004)

// A view saved on this stage in slot v (1 to VIEW_SLOTS), or 0.
static CamView *View_Find(int v)
{
    int stage = Stage_GetExternalID();
    for (int i = 0; i < VIEW_POOL; i++)
    {
        CamView *w = &preset_file->view[i];
        if (w->used && w->stage == stage && w->slot == v)
            return w;
    }
    return 0;
}

// Slot v on this stage to save into: the one there, or a free one (0 when
// the file has no room left).
static CamView *View_New(int v)
{
    CamView *w = View_Find(v);
    if (w)
        return w;
    for (int i = 0; i < VIEW_POOL; i++)
    {
        w = &preset_file->view[i];
        if (!w->used)
        {
            memset(w, 0, sizeof(*w));
            w->stage = Stage_GetExternalID();
            w->slot = v;
            return w;
        }
    }
    return 0;
}

// The menus' names for the views on this stage and the presets.
static void Labels_Refresh(void)
{
    for (int i = 1; i < PS_SAVED; i++)
    {
        if (preset_file->preset_name[i][0])
            strcpy(preset_label[i], preset_file->preset_name[i]);
        else
            sprintf(preset_label[i], "Preset %d", i);
    }
    for (int v = 1; v <= VIEW_SLOTS; v++)
    {
        CamView *w = View_Find(v);
        if (w && w->name[0])
            strcpy(view_label[v - 1], w->name);
        else
            sprintf(view_label[v - 1], w ? "View %d" : "View %d (empty)", v);
    }
}

// The match camera's COBJ: Match_GetCObj gives its GOBJ, despite the name.
// It's the one the stage is drawn with in every camera mode; MexTK's
// stc_matchcam_cobj is a copy the game only keeps up in the normal and fixed
// modes, so it goes stale in Advanced (the develop camera).
static COBJ *View_CObj(void)
{
    GOBJ *g = (GOBJ *)Match_GetCObj();
    return g ? g->hsd_object : *stc_matchcam_cobj;
}

static void View_Apply(int v)
{
    CamView *w = View_Find(v);
    if (!w)
        return;
    Options_Camera[CAMOPT_MODE].val = CAM_ADVANCED;
    Event_ChangeCamera(0, CAM_ADVANCED);
    dev_cam->free_eye_pos = w->eye;
    dev_cam->free_int_pos = w->interest;
    dev_cam->free_fov = w->fov;
}

void Event_ChangeView(GOBJ *menu, int value)
{
    if (value > 0 && !View_Find(value))
    {
        SFX_PlayCommon(3); // nothing saved there yet
        return;
    }
    View_Apply(value);
}

void Event_SaveView(GOBJ *menu)
{
    int v = Options_Camera[CAMOPT_VIEW].val;
    CamView *w = v > 0 ? View_New(v) : 0;
    COBJ *cobj = View_CObj();
    if (!w || !cobj)
    {
        SFX_PlayCommon(3); // no view picked, or every view in the file is used
        return;
    }
    COBJ_GetEyePosition(cobj, &w->eye);
    COBJ_GetInterest(cobj, &w->interest);
    w->fov = cobj->projection_param.perspective.fov;
    w->used = 1;
    preset_dirty = 1;
    Labels_Refresh();
    OSReport("LLVIEW saved %d on stage %d: eye %.1f %.1f %.1f at %.1f %.1f %.1f fov %.1f\n", v, w->stage, w->eye.X, w->eye.Y,
             w->eye.Z, w->interest.X, w->interest.Y, w->interest.Z, w->fov);
    View_Apply(v);
    SFX_PlayCommon(1);
}
