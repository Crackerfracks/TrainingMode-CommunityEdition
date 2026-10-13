# GIF shot list

Every GIF the README and guide pages ask for, with the script that plays it.
Scripts are in src/gif/ (one file per stage, plus one for the cast; how a
clip is cut is in src/gif/README.md). A script used for more than one GIF
(hero, ai-plain, ledge-route, pad-ring, waveland, wavedash) is recorded
once and cut into each. Falcon unless it says the
cast. All at the defaults plus the lines the script sets.

Specs: game picture only (no window, no Dolphin UI), true game speed from
Dolphin's frame dump, 30 fps, 480 px wide (README top: 640 px), looping.
Aim for under 3 MB; up to 5 MB is fine (GitHub shows images up to 10 MB),
and a long clip may drop to 24 fps before it drops below 480 px. Each clip
has 15 frames before the action and 30 after the landing. Each comparison set is one script with only the look
changed, so the look is the only thing that moves; cut each clip of a set
to the same frames.

## README

| File | Script (stage) | Placeholder |
|---|---|---|
| hero-ai.gif | hero (Battlefield) | README top |
| cast-reel.gif | cast-top or cast-side (landinglab_gif_cast.txt), eight fighters cut together, about 1.5 s each: Fox, Marth, Sheik, Jigglypuff, Peach, Falco, Ice Climbers, Falcon (Battlefield) | README top, under the hero |
| first-minute.gif | first-minute (Battlefield, the README's steps) | README "Your first minute" |
| nil.gif | nil (Dream Land) | README effects: NIL |
| waveland.gif | waveland (Pokemon Stadium) | README effects: Perfect waveland |
| wavedash.gif | wavedash (Final Destination) | README effects: Wavedash timer |
| controller.gif | pad-crest (Battlefield), cropped to the controller and Falcon | README effects: Controller |
| everything.gif | everything-jump (Battlefield) | README "Everything preset" |
| quick-menu.gif | quick-menu, then by pad: open the quick menu, flip a card, change a setting, play out with the stick (Battlefield) | README presets, release notes |
| ledge-route.gif | ledge-route (Battlefield) | README ledge: a route into a nair AI |
| assist.gif | assist-1s, then by pad (Battlefield) | README ledge: Assist waiting on you |
| jump-timing.gif | jump-timing (Fountain of Dreams) | README Jump Timing |

## Guide pages

| File | Script (stage) | Page |
|---|---|---|
| cues-ai.gif | ai-plain (Battlefield) | cues.md top |
| wl-plain.gif, wl-flash.gif, wl-flash-glow.gif | wl-plain, wl-flash, wl-flash-glow (Stadium) | cues.md Body Flash / Platform Glow |
| spot-path.gif, spot-timer.gif, spot-glow.gif | spot-path, spot-timer, spot-glow (Dream Land) | cues.md spot cues |
| glossary-ai.gif | ai-plain (Battlefield) | glossary.md top |
| glossary-frames.gif | ai-slow (Battlefield), the same AI at 1/2 speed | glossary.md ECB |
| glossary-waveland.gif | slide-off (Stadium) | glossary.md waveland |
| glossary-lcancel.gif, glossary-fastfall.gif, glossary-jumpsquat.gif, glossary-wavedash.gif | lcancel, fastfall, jumpsquat (Battlefield), wd-slow (Final Destination) | glossary.md |
| paths.gif | paths (Battlefield) | paths.md top |
| slide-off.gif | slide-off (Stadium) | paths.md Slide-off Line |
| timers.gif | strip-cells (Battlefield) | timers.md top |
| strip-cells.gif, strip-highway.gif, strip-dial.gif, strip-conveyor.gif | strip-* (Battlefield) | timers.md Fixed Strip |
| near-bubble.gif ... near-oldstrip.gif (6) | near-* (Battlefield) | timers.md Near You |
| wl-ticks.gif, wl-rails.gif, wl-chevrons.gif | waveland, wl-rails, wl-chevrons (Stadium) | timers.md Waveland |
| wd-cells.gif, wd-pips.gif, wd-ring.gif | wavedash (first half), wd-pips, wd-ring (Final Destination) | timers.md Wavedash |
| travel-fixed.gif, travel-catchup.gif | hero, travel-catchup (Battlefield) | timers.md Note Travel |
| hud.gif | pad-ring (Battlefield) | hud.md top |
| pad-small.gif, pad-medium.gif, pad-large.gif | pad-ring, pad-ring-medium, pad-ring-large | hud.md sizes |
| pad-crest.gif, pad-classic.gif | pad-crest, pad-classic | hud.md looks |
| padcue-ring.gif, padcue-gauge.gif | padcue-ring, padcue-gauge | hud.md Controller Cues |
| shield-drop.gif | shield-drop (Battlefield) | hud.md Shield Drop Zone |
| ledge.gif | ledge-route (Battlefield) | ledge.md top |
| ledge-galint.gif, ledge-easiest.gif | ledge-route, ledge-sort-easiest | ledge.md Route Sort |
| assist-5s.gif, assist-01s.gif | assist-5s, assist-01s, then by pad | ledge.md Assist |
| jump-timing-run.gif | jump-timing-run (Fountain of Dreams) | jump-timing.md |
| cam-normal.gif, cam-zoom.gif, cam-fixed.gif, cam-advanced.gif | cam-* (Battlefield) | camera.md modes |
| preset-defaults.gif, preset-minimal.gif, preset-everything.gif, preset-ledgedrill.gif | hero, preset-minimal, everything-jump, preset-ledgedrill | presets.md |
| everything-wl.gif, everything-ledge.gif | everything-wl (Stadium), everything-ledge (Battlefield) | each feature's collapsed section |

## The cast

| File | Script (stage) | Page |
|---|---|---|
| cast/<fighter>.gif, 26 of them (Zelda and Sheik both), 320 px | cast-top or cast-side (landinglab_gif_cast.txt), whichever lands the fighter while still rising (Battlefield, one run per fighter) | a cast gallery in the README's collapsed section and guide/README.md |
| cast/<fighter>-ledge.gif for the fighters with a ledge route | cast-ledge (landinglab_gif_cast.txt) | ledge.md, a collapsed "every fighter" section |

A fighter whose clip can't show a rising AI is left out and listed, not
faked.
