#!/usr/bin/env python3
"""nav_boot_test.py [N]: boot the test Dolphin N times (default 10, alternating Fox and Mario) and drive it
into Landing Lab with tools/nav/nav.py (closed loop on MemoryWatcher). A boot passes when nav exits 0 and the
event logs the picked fighter ("LLECB fighter <internal id>"). Boots 3 and 8 get a `hyprctl reload` right
after nav confirms the event. The build has no LLCHAR, so the fighter really comes from the CSS pick.
Report: llsim-capture/nav_boot_test.txt; per-boot nav logs in nav_boot_<i>.txt.
"""
import os, re, sys, time, shutil, subprocess
sys.path.insert(0, os.path.expanduser("~/.local/share/jobboard"))
import jobboard

W = "/run/media/svaughn/External HD/Emulation/Mods/GC/Melee/TM-CE/TM-CE (CURRENT)"
C = f"{W}/TM-CE_Claude_Landing_Trainer/TM-CE-landinglab-cloud"
O = f"{W}/TM-CE_Claude_Landing_Trainer/llsim-capture"
V = f"{W}/Base Melee/Super Smash Bros. Melee v1-2.iso"
U = os.path.expanduser("~/.local/share/tmce-landinglab-dolphin")
ISO = f"{U}/test-navboot.iso"
NAV = f"{C}/tools/nav/nav.py"
FIGHTERS = [("Fox", 1), ("Mario", 0)]          # CSS name, internal kind (fp->kind)
HEADLESS = bool(os.environ.get("HEADLESS"))   # HEADLESS=1: run on Xvfb (hlaunch.sh)
RELOAD_AT = set() if HEADLESS else {3, 8}
N = int(sys.argv[1]) if len(sys.argv) > 1 else 10
R = open(f"{O}/nav_boot_test.txt", "w", buffering=1)


def his_slippi():
    out = subprocess.run(["pgrep", "-a", "-f", "slippi|Slippi|AppRun.wrapped|dolphin-emu"],
                         capture_output=True, text=True).stdout
    return [l for l in out.splitlines() if U not in l and "launcher" not in l.lower()
            and ("dolphin" in l.lower() or "slippi" in l.lower())]


def close_test_dolphin():
    for p in subprocess.run(["pgrep", "-f", f"AppRun.wrapped -u {U} -b -e {ISO}"],
                            capture_output=True, text=True).stdout.split():
        try: os.kill(int(p), 15)
        except OSError: pass
    time.sleep(3)


def log_lines():
    raw = open(f"{U}/Logs/dolphin.log", errors="replace").read().replace("\r", "").splitlines()
    return [re.sub(r"^.*N\[OSREPORT\]: ", "", l) for l in raw]


with jobboard.Job("TM-CE nav boot test (closed-loop menus)", total=N, unit="boots", group="TM-CE", stale_after=400,
                  owner_label="Landing trainer research", replaces=os.environ.get("REPLACES") or None,
                  description="nav.py route into Landing Lab, Fox/Mario alternating, hyprctl reload on boots 3 and 8",
                  fields={"boot": 0, "fighter": "-", "passed": 0, "failed": 0}) as job:
    job.update(phase="building the test ISO (no LLCHAR)")
    env = dict(os.environ, LLSCRIPT="src/landinglab_dump.txt")
    env.pop("LLCHAR", None); env.pop("LLCHARFILE", None)
    b = subprocess.run(["nice", "-n", "15", "ionice", "-c3", "./build.sh", V, "test"], cwd=C, env=env,
                       capture_output=True, text=True)
    if b.returncode or not os.path.exists(f"{C}/TM-CE.iso"):
        R.write(b.stdout[-2000:] + b.stderr[-2000:]); raise RuntimeError("test build failed")
    shutil.move(f"{C}/TM-CE.iso", ISO)
    passed = failed = 0
    for i in range(1, N + 1):
        name, kind = FIGHTERS[(i - 1) % 2]
        while (h := his_slippi()):
            job.pause(f"waiting: Stephen's Slippi/Dolphin open ({h[0][:60]})"); time.sleep(60)
        job.resume()
        job.update(phase=f"boot {i}: {name}", fields={"boot": i, "fighter": name})
        open(f"{U}/Logs/dolphin.log", "w").close()
        navlog = open(f"{O}/nav_boot_{i}.txt", "w")
        nav = subprocess.Popen(["python3", NAV, "route", "Landing Lab", name], stdout=navlog, stderr=subprocess.STDOUT)
        time.sleep(1)                                   # nav binds the socket and writes Locations.txt first
        if HEADLESS:                                    # hidden X display; Stephen's screens untouched
            subprocess.Popen([f"{U}/hlaunch.sh", ISO])
        else:
            subprocess.Popen([f"{U}/focusguard.sh", "300"])
            ws = subprocess.run([f"{U}/pickws.py"], capture_output=True, text=True).stdout.strip()
            subprocess.run(["hyprctl", "dispatch", f"hl.dsp.exec_cmd('{U}/launch.sh {ISO}', {{ workspace = '{ws} silent', "
                            "float = true, no_initial_focus = true, suppress_event = 'fullscreen maximize fullscreenoutput "
                            "activate activatefocus' })"], capture_output=True)
        t0, reloaded, seen = time.time(), False, 0
        while nav.poll() is None and time.time() - t0 < 240:
            if HEADLESS:                                # a headless boot must never map a window on Hyprland
                cl = subprocess.run(["hyprctl", "clients", "-j"], capture_output=True, text=True).stdout
                seen += '"dolphin-emu"' in cl
            if i in RELOAD_AT and not reloaded and "NAV confirmed event" in open(f"{O}/nav_boot_{i}.txt").read():
                subprocess.run(["hyprctl", "reload"], capture_output=True); reloaded = True
                job.log(f"boot {i}: hyprctl reload mid-route", "info")
            time.sleep(0.2)
        if nav.poll() is None:
            nav.kill()
        rc = nav.wait()
        fighter = ""
        for _ in range(60):                             # the event logs its fighter once it starts
            fighter = next((l for l in log_lines() if l.startswith("LLECB fighter")), "")
            if fighter: break
            time.sleep(0.5)
        close_test_dolphin()
        navtxt = open(f"{O}/nav_boot_{i}.txt").read()
        got = re.match(r"LLECB fighter (\d+)", fighter)
        ok = rc == 0 and got and int(got.group(1)) == kind
        why = "" if ok else (re.findall(r"NAV FAILED.*", navtxt) or [f"nav rc {rc}"])[0] + f" | {fighter or 'no LLECB fighter line'}"
        if seen:
            ok, why = False, why + f" | a Dolphin window showed on Hyprland ({seen} polls)"
        line = f"boot {i} {name}{' +reload' if reloaded else ''}{' headless' if HEADLESS else ''}: {'PASS' if ok else 'FAIL'} " \
               f"{time.time() - t0:.0f}s | {fighter} {why}"
        R.write(line + "\n")
        R.write("".join("    " + l + "\n" for l in navtxt.splitlines() if "NAV" in l))
        passed += bool(ok); failed += not ok
        job.advance(1, msg=line[:120]); job.log(line[:200], "ok" if ok else "error")
        job.update(fields={"passed": passed, "failed": failed})
    if os.path.exists(ISO): os.remove(ISO)
    if HEADLESS:
        subprocess.run([f"{U}/hlaunch.sh", "stop"])
    R.write(f"passed {passed}/{N}\n")
    if failed:
        raise RuntimeError(f"{failed} of {N} boots failed")
