# Test harness (Stephen's laptop)

These scripts drive the isolated test Dolphin (`~/.local/share/tmce-landinglab-dolphin`, Slippi Mainline
AppImage) for Landing Gear captures and tests. Paths are hard-coded for that machine; copy and adjust
them elsewhere.

| Script | What it does |
|---|---|
| `run_iso.sh TAG` | runs `test-TAG.iso`: starts `tools/nav/nav.py` (closed-loop menus), launches Dolphin on the laptop panel without stealing focus, then padshots + watchdog until `LLDONE` |
| `hlaunch.sh ISO` / `hlaunch.sh stop` | headless launch: starts Xvfb `:99` if needed and runs Dolphin there (Vulkan, `MESA_VK_WSI_DEBUG=sw`, ~60 FPS); `stop` ends the Xvfb it started. Set `HEADLESS=1` for `run_iso.sh`, `run_tests.py`, `capture_all.py` and `nav_boot_test.py` |
| `launch.sh ISO` | the Dolphin command line (`-u` test user dir, `-b -e`) |
| `pickws.py` | picks the workspace for the test window (laptop panel, unless a fullscreen window of Stephen's is there) |
| `padshots.py OFF DIR` | on each `LLSHOT`: hold the pad state the label asks for, grab the game area (grim, or `import` from Xvfb when `HEADLESS=1`), release |
| `gamearea.py` | prints the render area to grab (Hyprland window, or the largest window on Xvfb when `HEADLESS=1`) |
| `pad.py` | one-shot pipe-controller inputs (`A`, `stick X Y S`, `wait S`, hotkeys) |
| `run_tests.py TAG SCRIPT name:ckind ...` | one test build per fighter (LLCHAR=file), run, log to `llsim-capture/TAG_name.log` |
| `RUN=9 capture_all.py [CHAR ...]` | the table capture for all 26 fighters or the ones named (`*_bfRUN.log`); a log whose fighter changes mid-capture is failed and set aside. Refuses to start when the staged build in `capall` is older than the checkout's event code or its capture script differs (restage, or `STALE_OK=1`) |
| `nav_boot_test.py [N]` | N boots into Landing Gear with nav (Fox/Mario, `hyprctl reload` on boots 3 and 8); passed 10/10 on 2026-10-12 |

Long runs go detached (`systemd-run --user --collect`), post to the job board, and wait while Stephen's own
Slippi/Dolphin is open. Menu navigation is `tools/nav/` (map in `menus.toml`); no timed menu inputs.
