# Test harness (Stephen's laptop)

These scripts drive the isolated test Dolphin (`~/.local/share/tmce-landinglab-dolphin`, Slippi Mainline
AppImage) for Landing Lab captures and tests. Paths are hard-coded for that machine; copy and adjust
them elsewhere.

| Script | What it does |
|---|---|
| `run_iso.sh TAG` | runs `test-TAG.iso`: starts `tools/nav/nav.py` (closed-loop menus), launches Dolphin on the laptop panel without stealing focus, then padshots + watchdog until `LLDONE` |
| `launch.sh ISO` | the Dolphin command line (`-u` test user dir, `-b -e`) |
| `pickws.py` | picks the workspace for the test window (laptop panel, unless a fullscreen window of Stephen's is there) |
| `pad.py` | one-shot pipe-controller inputs (`A`, `stick X Y S`, `wait S`, hotkeys) |
| `run_tests.py TAG SCRIPT name:ckind ...` | one test build per fighter (LLCHAR=file), run, log to `llsim-capture/TAG_name.log` |
| `capture_all8.py` | the 26-fighter table capture (`*_bf8.log`) |
| `nav_boot_test.py [N]` | N boots into Landing Lab with nav (Fox/Mario, `hyprctl reload` on boots 3 and 8); passed 10/10 on 2026-10-12 |

Long runs go detached (`systemd-run --user --collect`), post to the job board, and wait while Stephen's own
Slippi/Dolphin is open. Menu navigation is `tools/nav/` (map in `menus.toml`); no timed menu inputs.
