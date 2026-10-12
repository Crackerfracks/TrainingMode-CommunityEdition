/* landinglab/body_flash.c
 *
 * Holds Falcon's body flashing with the cues.
 *
 * Part of Landing Lab: src/landinglab.c includes it, in order, into one
 * translation unit, so it sees everything included before it (and mex.h,
 * events.h, landinglab_sim.h).
 *
 * Uses: menus_display, tracking_state, rumble, hud_text, drawing,
 * corner_panel, ledge_routes.
 */

///////////////////////
/// Body flash      ///
///////////////////////

// On the first frame Falcon can act after a hit, he flashes the cue's
// color, which dust can't hide; after a ledge route he's tinted periwinkle
// while ledge intangibility lasts (blinking over its last frames).
static int flash_kind = -1;
static int flash_age = 100;
static int tint_on;

static void Body_Flash(FighterData *fp, int sid)
{
    static const float flash_a[4] = {0.75f, 0.55f, 0.35f, 0.18f};
    for (int i = 0; i < CUE_NUM; i++)
    {
        Cue *e = &cue_end[i];
        if (e->phase == PH_HIT && e->age == Cue_FlashAge(e))
        {
            flash_kind = i;
            flash_age = 0;
        }
    }
    if (route_active && route_landed >= 0)
    {
        LedgeRoute *r = &route_cur;
        if (Route_ActCell(r) - route_e - 1 == 0)
        {
            flash_kind = r->kind == LAND_AI ? CUE_AI : CUE_NIL;
            flash_age = 0;
        }
    }

    float a = 0;
    GXColor c = color_galint;
    int landing = sid == ASID_LANDING || sid == ASID_LANDINGFALLSPECIAL || (sid >= ASID_LANDINGAIRN && sid <= ASID_LANDINGAIRLW);
    if (Options_Cues[COPT_FLASH].val)
    {
        if (flash_age < 4 && flash_kind >= 0)
        {
            c = Cue_Color(flash_kind);
            a = flash_a[flash_age];
        }
        else if (galint_now > 0 && fp->phys.air_state == 0 && !landing)
            a = galint_now <= 3 && (event_vars->game_timer & 1) ? 0.2f : 0.45f;
    }
    if (flash_age < 100)
        flash_age++;
    a *= hide_all ? 0 : Kind_K(VG_CUES, flash_age < 4 ? flash_kind : -1);
    if (a > 0.9f)
        a = 0.9f;

    if (a > 0)
    {
        memset(&fp->color[1], 0, sizeof(ColorOverlay));
        c.a = 255 * a;
        fp->color[1].hex = c;
        fp->color[1].color_enable = 1;
        tint_on = 1;
    }
    else if (tint_on)
    {
        memset(&fp->color[1], 0, sizeof(ColorOverlay));
        tint_on = 0;
    }
}
