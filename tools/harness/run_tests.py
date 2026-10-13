#!/usr/bin/env python3
"""run_tests.py TAG SCRIPT NAME:CKIND ... : one test build per fighter (LLCHAR=file) from the checkout head,
run in the test Dolphin, log to llsim-capture/<TAG>_<name>.log. Posts to the job board with the issue flow.

    REPLACES="<old job name>" python3 run_tests.py load src/landinglab_dump.txt mario:8 bowser:5
    SHOTS=1: keep the shots folder as <TAG>_<name>_shots/ (for screenshot tests).
The pack in the checkout's build/llecb.bin goes in by default (build.sh). Waits while Stephen's own Slippi/Dolphin
is open (not the launcher, not our test user dir).
"""
import os, re, sys, time, shutil, subprocess
sys.path.insert(0, os.path.expanduser("~/.local/share/jobboard"))
import jobboard

W = "/run/media/svaughn/External HD/Emulation/Mods/GC/Melee/TM-CE/TM-CE (CURRENT)"
C = f"{W}/TM-CE_Claude_Landing_Trainer/TM-CE-landinglab-cloud"
O = f"{W}/TM-CE_Claude_Landing_Trainer/llsim-capture"
V = f"{W}/Base Melee/Super Smash Bros. Melee v1-2.iso"
U = os.path.expanduser("~/.local/share/tmce-landinglab-dolphin")
NICE = ["nice", "-n", "15", "ionice", "-c3"]
TAG, SCRIPT, PAIRS = sys.argv[1], sys.argv[2], [p.split(":") for p in sys.argv[3:]]
ISO = f"{U}/test-{TAG}.iso"


def his_slippi():
    out = subprocess.run(["pgrep", "-a", "-f", "slippi|Slippi|AppRun.wrapped|dolphin-emu"],
                         capture_output=True, text=True).stdout
    return [l for l in out.splitlines() if U not in l and "launcher" not in l.lower()
            and ("dolphin" in l.lower() or "slippi" in l.lower())]


def close_test_dolphin():
    out = subprocess.run(["pgrep", "-f", f"AppRun.wrapped -u {U} -b -e {ISO}"], capture_output=True, text=True).stdout
    for p in out.split():
        try: os.kill(int(p), 15)
        except OSError: pass
    time.sleep(3)


head = subprocess.run(["git", "-C", C, "log", "--oneline", "-1"], capture_output=True, text=True).stdout.strip()
with jobboard.Job(f"TM-CE test: {TAG}", total=len(PAIRS), unit="fighters", group="TM-CE", stale_after=1500,
                  owner_label="Landing trainer research", replaces=os.environ.get("REPLACES") or None,
                  description=f"{SCRIPT} per fighter at {head}; logs llsim-capture/{TAG}_<name>.log",
                  fields={"script": SCRIPT, "head": head[:40], "fighter": "-", "ok": 0, "failed": 0}) as job:
    ok = failed = 0
    for name, ck in PAIRS:
        while (h := his_slippi()):
            job.pause(f"waiting: Stephen's Slippi/Dolphin open ({h[0][:60]})"); time.sleep(60)
        job.resume()
        job.update(phase=f"building {name} (llchar {ck})", fields={"fighter": name})
        open(f"{U}/capall/test_llchar.txt", "w").write(ck + "\n")
        env = dict(os.environ, LLSCRIPT=SCRIPT, LLCHAR="file", LLCHARFILE=f"{U}/capall/test_llchar.txt")
        b = subprocess.run(NICE + ["./build.sh", V, "test"], cwd=C, env=env, capture_output=True, text=True)
        if b.returncode or not os.path.exists(f"{C}/TM-CE.iso"):
            open(f"{O}/{TAG}_{name}_build.txt", "w").write(b.stdout + b.stderr)
            failed += 1; job.advance(1); job.update(fields={"failed": failed})
            job.log(f"{name}: build failed, see {TAG}_{name}_build.txt", "error"); continue
        shutil.move(f"{C}/TM-CE.iso", ISO)
        # the menu navigation is timed in wall-clock time, so a slow Dolphin can land on the wrong event: a run
        # counts only if Landing Gear itself logged (its LLCHAR line); otherwise retry, up to 3 tries
        for attempt in range(1, 4):
            job.update(phase=f"running test Dolphin: {name} (try {attempt})")
            subprocess.run(["timeout", "1200", "nice", "-n", "15", f"{U}/run_iso.sh", TAG],
                           stdout=open(f"{O}/{TAG}_{name}_run.txt", "w"), stderr=subprocess.STDOUT)
            close_test_dolphin()
            raw = open(f"{U}/Logs/dolphin.log", errors="replace").read().replace("\r", "").splitlines()
            full = [re.sub(r"^.*N\[OSREPORT\]: ", "", l) for l in raw]
            if any(l.startswith("LLCHAR") for l in full):
                break
            job.log(f"{name}: try {attempt} never reached Landing Gear (no LLCHAR line; menu navigation missed)", "warn")
        if os.path.exists(ISO): os.remove(ISO)
        open(f"{O}/{TAG}_{name}.log", "w").write("\n".join(full) + "\n")
        if os.environ.get("SHOTS") and os.path.isdir(f"{U}/{TAG}"):
            shutil.rmtree(f"{O}/{TAG}_{name}_shots", ignore_errors=True)
            shutil.copytree(f"{U}/{TAG}", f"{O}/{TAG}_{name}_shots")
        crash = next((l for l in full if "assertion" in l), "")
        fighter = next((l for l in full if l.startswith("LLECB fighter")), "no LLECB fighter line")
        good = not crash and any(l.startswith("LLCHAR") for l in full)
        msg = f"{name}: {'OK' if good else 'FAIL'} | {fighter}" + (f" | {crash}" if crash else "")
        ok += good; failed += not good
        job.advance(1, msg=msg[:120]); job.log(msg[:200], "ok" if good else "error")
        job.update(fields={"ok": ok, "failed": failed})
    if failed:
        raise RuntimeError(f"{failed} of {len(PAIRS)} fighters failed")
