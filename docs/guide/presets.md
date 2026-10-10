# Presets

Presets save a whole setup so you can switch between them in one step, and pick the one the event starts with.

Four built-in presets, four slots of your own, and User Custom, which saves itself every time you close the menu, whether you meant it to or not.

<!-- GIF: the quick menu opening over a frozen jump, flipping to another preset card, then the stick playing out of it, at default settings -->

## Options

The Presets menu, in menu order.

| Option | Values | Default | What it does |
| --- | --- | --- | --- |
| Preset | User Custom, Preset 1 to Preset 4 (shows the name you gave it), Defaults, Minimal, Everything, Ledge Drill | User Custom | The preset the rows below act on. Its description shows three lines about it, such as the cues on, the timers and the paths. An empty slot says that Save to Preset keeps the current settings there. |
| Load Preset | Press A | n/a | Sets every menu the way the preset above has it. |
| Save to Preset | Press A | n/a | Keeps the current settings in the preset above: User Custom or Preset 1 to 4. Defaults, Minimal, Everything and Ledge Drill are built in and stay as they are. |
| Name Preset | Press A | n/a | Gives the preset above a name. Works on Preset 1 to 4 only. |
| Load at Start | User Custom, Preset 1 to Preset 4 (shows the name you gave it), Defaults, Minimal, Everything, Ledge Drill | User Custom | The preset the event starts with. User Custom is how you left the menus the last time you closed them. |
| Memory Card | Info only | n/a | Says whether anything is saved on the memory card in slot A, and whether settings are being kept. |

## The presets

User Custom and Preset 1 to 4 are yours and live on the memory card. They can be empty. The four built-in presets are Defaults, Minimal, Everything and Ledge Drill. You cannot save over or rename a built-in preset. Trying plays the error sound and does nothing. Load Preset on an empty slot plays the error sound too.

Minimal, Everything and Ledge Drill are Defaults with the changes below. Everything not listed matches Defaults.

### Defaults

The values in the Default columns of this guide. AI and waveland cues, the corner strip, the landing spot brackets and the controller display.

### Minimal

The AI cue and the corner strip, nothing else on screen.

| Menu | Option | Defaults | Minimal |
| --- | --- | --- | --- |
| Cues | Waveland Cues | All Floors | Off |
| Timers | Landing Spot | On | Off |
| Timers | Waveland | Ticks | Off |
| Timers | Wavedash | Cells | Off |
| Paths | Frame Dots | Always | Off |
| HUD | Controller | By Percent | Off |
| HUD | Info Panel | On | Off |

### Everything

Every cue, path, timer and sound.

| Menu | Option | Defaults | Everything |
| --- | --- | --- | --- |
| Cues | NIL Cues | Off | On |
| Cues | Platform Glow | Off | On |
| Timers | Near Falcon | Off | Bubble |
| Paths | Landing Path | Off | On |
| Paths | Body Path | Off | On |
| Paths | Slide-off Line | Off | On |
| Paths | Jump Preview | Off | Both |
| Sounds | Buzz: Skipped Window | Off | On |
| Sounds | Buzz: Fastfall Timing | Off | On |
| Sounds | Buzz: Jump Timing | Off | On |
| Sounds | Buzz: Aerial Timing | Off | On |

### Ledge Drill

Ledge routes from the ledge, both kinds, reset to the same ledge each time.

| Menu | Option | Defaults | Ledge Drill |
| --- | --- | --- | --- |
| Ledge Practice | Route Kind | AI | Both |
| Ledge Practice | Reset | None | Same Side |
| Cues | NIL Cues | Off | On |

<details>
<summary>The four built-in presets, side by side</summary>

<!-- GIF: the same jump on Battlefield under Defaults, Minimal, Everything and Ledge Drill -->

</details>

## Saving and naming

- Save to Preset writes the current settings into the preset chosen in Preset: User Custom or Preset 1 to 4.
- User Custom is rewritten for you whenever the menu closes with changes, and when you leave the event.
- Name Preset opens a letter grid. Names are up to 12 letters, and the name shows in the Preset and Load at Start lists.
- The quick menu can save too. See below.

Name grid inputs (the same grid names camera views):

| Input | Does |
| --- | --- |
| D-pad or stick | Moves over the keys. It wraps. |
| A | Types the key. On the bottom row: space, delete, case toggle, OK. OK saves and returns to the menu. |
| B | Deletes the last letter. With an empty name it cancels and returns to the menu. |
| X | Switches between capitals and small letters. |
| Y | Types a space. |
| Start | Saves the name and leaves. The game goes on, and Start opens the menu again. |

## What a preset stores

A preset stores every list, number and toggle in the Cues, Intensity by Group, Rumble, Timers, Paths, HUD, Sounds, Ledge Practice, Jump Timing, Camera and Speed menus. That is 68 options.

It does not store:

- Ledge Practice > Route (the route you picked).
- Speed > Frame Advance.
- The Presets menu itself.
- The whole Developer menu.

Camera Mode and View are stored. Views belong to a stage, so a preset keeps the view number it was saved with. Loading a preset that changes the camera moves it on the next frame.

## Memory card

Presets are kept in one file on the memory card in slot A, named TMCE_LandingLab, using 1 block. Changes save in the background when the menu closes, and again when you leave the event.

With no memory card, a full card or a read error, your settings last until you leave the event. The Memory Card row in the Presets menu says which case you are in.

## Load at Start

Load at Start picks the preset the event opens with. The default is User Custom, which is your settings as you left them.

On a fresh start with no memory card, no saved file, or an unreadable file, nothing is loaded and the menus start at the Defaults values.

## Quick menu

The quick menu opens a deck of your presets over a frozen game so you can switch between them without opening the main menu. It needs HUD > Quick Menu on, which it is by default.

On the ground it also puts up a still example hop, so the cues have something to show.

### Inputs

| Input | Does |
| --- | --- |
| L or R clicked all the way + D-pad up or down | Opens the quick menu and freezes the game. The trigger must be clicked, not partway. |
| L and R both clicked all the way + D-pad up or down | Hides everything Landing Lab draws, or shows it again. Works whether or not Quick Menu is on. |
| D-pad left or right on the cards | Goes to the previous or next preset card, skipping empty ones. The preset is applied at once. |
| D-pad down on the cards | Opens the settings list of the chosen card. |
| D-pad up or down in the list | Goes to the previous or next setting. Up from the first setting returns to the cards. Down stops at the last setting. |
| D-pad left or right in the list | Changes the highlighted setting's value, wrapping around. It takes effect at once but is not saved until you save it with A, X or Y. |
| A with a trigger held | Saves the current settings into the chosen preset (User Custom or Preset 1 to 4). If the chosen preset is built in, it saves into the first free Preset 1 to 4. |
| X or Y with a trigger held | Saves the current settings as a new preset in the first free Preset 1 to 4. If none is free it says "No free preset: save over one". |
| B | Closes the quick menu. |
| L and R both clicked + D-pad up or down | Closes the quick menu and hides or shows everything Landing Lab draws. |
| Moving the stick or C-stick | Plays out of the quick menu. The game goes on and the menu fades out over the HUD > Quick Menu Play time. Does nothing if Quick Menu Play is Never. |
| Start | Opens the main menu and closes the quick menu. |
| No D-pad or trigger press | Closes after the HUD > Quick Menu Idle time, fading over the last 3 seconds. Letting go of the trigger does not close it. |

A D-pad press or a new trigger press brings back a quick menu that is fading out from playing on. With a trigger only partway in, the D-pad does nothing, so shielding cannot open it by accident.

### Settings in the list

The list has 16 rows, in this order:

1. AI cues
2. NIL cues
3. Waveland cues
4. Timer near Falcon
5. Fixed strip
6. Landing spot
7. Waveland timer
8. Landing path
9. Body path
10. Frame dots
11. Platform glow
12. Jump timing (Jump Timing > Show)
13. Controller look
14. Controller cues
15. Intensity
16. Auto fade

How long the quick menu stays up or plays out is set in the HUD menu. See [HUD](hud.md).

[Back to the README](../../README.md)
