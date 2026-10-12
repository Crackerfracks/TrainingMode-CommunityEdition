/* landinglab/corner_panel.c
 *
 * Holds the corner panel's text lines and toasts.
 *
 * Part of Landing Lab: src/landinglab.c includes it, in order, into one
 * translation unit, so it sees everything included before it (and mex.h,
 * events.h, landinglab_sim.h).
 *
 * Uses: menus_practice, menus_display, rumble, drawing, hud_drawing.
 */

///////////////////////
/// Corner panel    ///
///////////////////////

// Two lines in a top corner: what's coming up, and how the last attempt
// went (a third, the accuracy count, in Developer). Each has a square in
// its cue's color at the screen's edge, and its text runs from there toward
// the middle, so a long line can't run off the screen.

static void Panel_Line(float x, float y, int right, const char *text, int kind)
{
    if (!text[0] || (text[0] == '-' && !text[1]))
        return; // nothing to say ("-"): no plate or square either
    float size = 0.45f;
    float w = 1.7f + Text_Width(text, size);
    float x0 = right ? x - w : x;
    Text_Plate(x0, x0 + w, y);
    GXColor sq = kind >= 0 ? Cue_Color(kind) : Color_Fill(color_white, kind == -2 ? 0 : 0.3f); // -2: no square
    GXColor tc = {235, 235, 235, 255};
    if (right)
    {
        Hud_Rect(x - 1.05f, y + 0.8f, x - 0.35f, y + 1.5f, sq);
        Hud_TextAligned(text, x - 1.4f, y, size, tc, 2);
    }
    else
    {
        Hud_Rect(x + 0.35f, y + 0.8f, x + 1.05f, y + 1.5f, sq);
        Hud_TextAligned(text, x + 1.4f, y, size, tc, 0);
    }
}

// A quick toggle's new setting, shown for a moment at the top of the panel
// (even with the panel off).
static char toast_text[40];
static int toast_timer;
static u8 hide_all; // everything the event draws hidden but the toast (L or R, Z and D-pad up or down)
#define TOAST_FRAMES 90

static void Toast(const char *t)
{
    strcpy(toast_text, t);
    toast_timer = TOAST_FRAMES;
    OSReport("LLTOAST %s\n", t);
}

static void Panel_Draw(void)
{
    int right = !panel_left;
    float x = right ? SAFE_W : -SAFE_W;
    float y = SAFE_H - 3.0f;
    if (toast_timer > 0)
    {
        Panel_Line(x, y, right, toast_text, -2);
        y -= 2.4f;
    }
    if (!Options_Hud[HOPT_PANEL].val || hide_all)
        return;
    Panel_Line(x, y, right, text_next, next_kind);
    if (text_steps[0])
    {
        y -= 2.4f;
        Panel_Line(x, y, right, text_steps, -2);
    }
    Panel_Line(x, y - 2.4f, right, text_last, last_kind);
    if (Options_Dev[DOPT_EXACT].val)
    {
        y -= 2.4f;
        Panel_Line(x, y - 2.4f, right, text_exact, -1);
    }
    // stepping frame by frame: which frame of which state is on screen, so
    // windows can be counted (a press now comes out on the next one)
    if (Options_Game[GOPT_FRAME_ADV].val && text_frame[0])
        Panel_Line(x, y - 4.8f, right, text_frame, -2);
}
