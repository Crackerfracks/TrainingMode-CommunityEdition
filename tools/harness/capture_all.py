#!/usr/bin/env python3
"""capture_all.py: Battlefield ECB/dump capture for every character, one build + run at a time.

Logs go to llsim-capture/<char>_bf<RUN>.log; RUN (default 9) is the capture's version, set in the
environment: RUN=10 python3 capture_all.py MARIO FOX. Posts to the job board ("TM-CE capture vRUN") and keeps the "TM-CE plan" card's "waiting on" current.
Waits between characters while Stephen's own Slippi/Dolphin is open, the 5-minute load average is
above LOAD_MAX, or the 2-minute average CPU package temp is above TEMP_MAX. Mid-run it only aborts
a character if the 2-minute average passes TEMP_ABORT. Characters whose log has LLLEN lines are skipped.
"""
import os, re, sys, time, glob, shutil, subprocess, threading, collections
sys.path.insert(0, os.path.expanduser("~/.local/share/jobboard"))
import jobboard

W = "/run/media/svaughn/External HD/Emulation/Mods/GC/Melee/TM-CE/TM-CE (CURRENT)"
C = f"{W}/TM-CE_Claude_Landing_Trainer/TM-CE-landinglab-cloud"
O = f"{W}/TM-CE_Claude_Landing_Trainer/llsim-capture"
V = f"{W}/Base Melee/Super Smash Bros. Melee v1-2.iso"
U = os.path.expanduser("~/.local/share/tmce-landinglab-dolphin")
ISO = f"{U}/test-cap.iso"
RUN = os.environ.get("RUN", "9")
TAG = f"_bf{RUN}.log"
PLAN = "tm-ce-plan-1012-165457-89fc"
ALL = ("FALCON DK FOX GAW KIRBY BOWSER LINK LUIGI MARIO MARTH MEWTWO NESS PEACH PIKACHU ICECLIMBERS "
       "JIGGLYPUFF SAMUS YOSHI ZELDA SHEIK FALCO YOUNGLINK DRMARIO ROY PICHU GANONDORF").split()
CKIND = {c: i for i, c in enumerate(ALL)}
CHARS = sys.argv[1:] or ALL
# a rerun names the run it replaces (its open issues close as superseded): REPLACES="TM-CE capture v9" RUN=10 python3 ...
REPLACES = os.environ.get("REPLACES") or None
CAP = os.path.expanduser("~/.local/share/tmce-landinglab-dolphin/capall")
BUILD_FILES = ["eventMenu", "lab", "labCSS", "lcancel", "ledgedash", "wavedash", "powershield", "dthrowknee",
               "edgeguard", "fc", "sweetspot", "laserland", "landinglab"]


def make_iso(n):
    """Fresh vanilla copy plus one gc_fst fs call (as build.sh does), with TM/llchar.txt = n."""
    open(f"{CAP}/llchar.txt", "w").write(f"{n}\n")
    shutil.copy(V, ISO)
    args = [f"{CAP}/gc_fst", "fs", ISO, "delete", "MvHowto.mth", "delete", "MvOmake15.mth", "delete", "MvOpen.mth"]
    for f in BUILD_FILES: args += ["insert", f"TM/{f}.dat", f"{CAP}/build/{f}.dat"]
    args += ["insert", "TM/llscript.txt", f"{CAP}/landinglab_capture.txt", "insert", "TM/llchar.txt", f"{CAP}/llchar.txt"]
    for f in ["eggs", "techchase", "slalom"]: args += ["insert", f"TM/{f}.dat", f"{CAP}/build/{f}.dat"]
    args += ["insert", "codes.gct", f"{CAP}/build/codes.gct", "insert", "Start.dol", f"{CAP}/build/Start.dol",
             "insert", "opening.bnr", f"{CAP}/opening.bnr"]
    subprocess.run(NICE + args, check=True, capture_output=True)
    subprocess.run([f"{CAP}/gc_fst", "set-header", ISO, "GTME01", "TM-CE + Landing Lab"], check=True, capture_output=True)
    tree = subprocess.run([f"{CAP}/gc_fst", "tree", ISO, "-s", "-o", "-x"], capture_output=True, text=True).stdout
    rows = sorted((int(p[1], 16), int(p[2], 16), p[0]) for p in (l.split("\t") for l in tree.splitlines())
                  if len(p) == 3 and p[1].startswith("0x") and int(p[2], 16) > 0)
    bad = [(a[2], b[2]) for a, b in zip(rows, rows[1:]) if a[0] + a[1] > b[0]]
    if bad: raise RuntimeError(f"files overlap on the capture ISO: {bad}")
import json as _json
_G = _json.load(open(os.path.expanduser("~/.local/share/tmce-landinglab-dolphin/gates.json")))
LOAD_MAX = float(_G.get("load5_max", 14))  # shared gate config (Stephen: the laptop can take more)
TEMP_MAX, TEMP_ABORT = 90.0, 95.0
RUN_TIMEOUT = 1200
LOG = f"{O}/capture_all{RUN}.log"
NICE = ["nice", "-n", "15", "ionice", "-c3"]


def log(msg):
    with open(LOG, "a") as f:
        f.write(time.strftime("%F %T ") + msg + "\n")


temps = collections.deque(maxlen=12)  # 2 minutes at one sample per 10 s


def sample_temps():
    while True:
        try:
            out = subprocess.run(["sensors"], capture_output=True, text=True).stdout
            m = re.search(r"^Package id 0:\s+\+([\d.]+)", out, re.M)
            if m: temps.append(float(m.group(1)))
        except Exception:
            pass
        time.sleep(10)


def temp_avg():
    return sum(temps) / len(temps) if temps else 0.0


def load5():
    return float(open("/proc/loadavg").read().split()[1])


def his_slippi():
    out = subprocess.run(["pgrep", "-a", "-f", "slippi-launcher|Slippi-Launcher|AppRun.wrapped|dolphin-emu"],
                         capture_output=True, text=True).stdout
    # only Stephen's own Slippi/Dolphin games: not our test user dir, not the Slippi Launcher, not other AppImages
    hits = [l for l in out.splitlines() if U not in l and "launcher" not in l.lower()
            and ("dolphin" in l.lower() or "slippi" in l.lower())]
    if hits: log("gate matched: " + " | ".join(h[:160] for h in hits))
    return hits


def gauges():
    return {"temp avg 2m": f"{temp_avg():.0f} C / limit {TEMP_MAX:.0f}",
            "load 5m": f"{load5():.1f} / limit {LOAD_MAX:.0f}"}


def blocked():
    if his_slippi(): return "Slippi/Dolphin of yours is open"
    if load5() > LOAD_MAX: return f"5-min load {load5():.1f} > {LOAD_MAX:.0f}"
    if temp_avg() > TEMP_MAX: return f"2-min CPU temp avg {temp_avg():.0f} C > {TEMP_MAX:.0f} C"
    return None


def plan_waiting(text):
    subprocess.run(["jb", "update", PLAN, "-f", f"waiting on={text}"], capture_output=True)


def test_dolphins():
    out = subprocess.run(["pgrep", "-f", f"AppRun.wrapped -u {U} -b -e {ISO}"], capture_output=True, text=True).stdout
    return [int(p) for p in out.split()]


def close_test_dolphin():
    for sig in (15, 9):                     # a Dolphin that lost its display can hang and ignore SIGTERM
        for p in test_dolphins():
            try: os.kill(p, sig)
            except OSError: pass
        time.sleep(3)
        if not test_dolphins():
            break


def count(lines, key):
    return sum(1 for l in lines if l.startswith(key))


threading.Thread(target=sample_temps, daemon=True).start()
time.sleep(1)

def stale_stage():
    """Why the staged capture build in CAP isn't this checkout's, or None. capall is staged by hand
    (build.sh test with LLSCRIPT=src/landinglab_capture.txt LLCHAR=file, then copied in), so a
    stale one silently captures the old script and code: check before every run."""
    want, have = f"{C}/src/landinglab_capture.txt", f"{CAP}/landinglab_capture.txt"
    if not os.path.exists(have) or open(want, "rb").read() != open(have, "rb").read():
        return f"{have} differs from {want}"
    dat = f"{CAP}/build/landinglab.dat"
    srcs = glob.glob(f"{C}/src/landinglab*.[ch]") + glob.glob(f"{C}/src/landinglab/*.[ch]")
    newest = max(srcs, key=os.path.getmtime) if srcs else None
    if not os.path.exists(dat) or (newest and os.path.getmtime(newest) > os.path.getmtime(dat)):
        return f"{dat} is older than {newest}: restage capall from this checkout"
    return None


why = stale_stage()
if why and os.environ.get("STALE_OK") != "1":
    sys.exit(f"capture_all: the staged capture build is stale ({why}); restage it, or STALE_OK=1 to run anyway")


with jobboard.Job(f"TM-CE capture v{RUN}", total=len(CHARS), unit="chars", group="TM-CE", stale_after=300, owner_label="Landing trainer research", replaces=REPLACES,
                  description="Builds a capture ISO per character, plays its jump/aerial/airdodge animations "
                              "on Battlefield, logs ECB + specials per frame to llsim-capture/<char>_bf{RUN}.log (one build, character swapped via TM/llchar.txt)",
                  fields={"char": "-", "ok": 0, "failed": 0, **gauges()}) as job:
    log(f"start (py; load5 limit {LOAD_MAX:.0f}, temp avg limit {TEMP_MAX:.0f}, abort {TEMP_ABORT:.0f})")
    ok = failed = 0
    for c in CHARS:
        name = c.lower(); out = f"{O}/{name}{TAG}"
        if os.path.exists(out) and any(l.startswith("LLLEAN") for l in open(out, errors="replace")):
            ok += 1; job.advance(1, msg=f"{c} already captured"); job.update(fields={"ok": ok}); continue
        job.update(phase=f"checking conditions for {c}", fields={"char": c, **gauges()})
        paused = False
        while (why := blocked()):
            if not paused:
                job.pause(f"{c}: waiting, {why}"); log(f"{c} waiting: {why}"); plan_waiting(why); paused = True
            job.update(fields={"waiting on": why, **gauges()})
            time.sleep(30)
        if paused:
            job.resume(); log(f"{c} resumed")
        job.update(fields={"waiting on": "-"}); plan_waiting(f"nothing; capturing {c}")

        job.update(phase=f"making ISO for {c} (llchar {CKIND[c]})", fields=gauges())
        log(f"{c} iso llchar {CKIND[c]}")
        try:
            make_iso(CKIND[c])
        except Exception as e:
            failed += 1; job.advance(1); job.update(fields={"failed": failed})
            job.log(f"{c} ISO failed: {e}", "error"); log(f"{c} FAIL iso: {e}")
            if os.path.exists(ISO): os.remove(ISO)
            continue

        job.update(phase=f"running test Dolphin: {c}", fields=gauges())
        close_test_dolphin()
        open(f"{U}/Logs/dolphin.log", "w").close()          # never read the previous fighter's log
        r = subprocess.Popen(["nice", "-n", "15", f"{U}/run_iso.sh", "cap"], stdout=open(f"{O}/run_{name}.txt", "w"),
                             stderr=subprocess.STDOUT)
        t0, aborted = time.time(), None
        while r.poll() is None:
            el = time.time() - t0
            job.update(fields={"run time": f"{el:.0f}s / {RUN_TIMEOUT}s", **gauges()})
            if temp_avg() > TEMP_ABORT: aborted = f"2-min temp avg {temp_avg():.0f} C > {TEMP_ABORT:.0f}"
            elif el > RUN_TIMEOUT: aborted = "timeout"
            if aborted:
                close_test_dolphin(); r.kill(); break
            time.sleep(10)
        close_test_dolphin()
        if os.path.exists(ISO): os.remove(ISO)

        job.update(phase=f"saving {c}")
        raw = open(f"{U}/Logs/dolphin.log", errors="replace").read().replace("\r", "").splitlines()
        full = [re.sub(r"^.*N\[OSREPORT\]: ", "", l) for l in raw]
        os.makedirs(f"{U}/cap", exist_ok=True)
        open(f"{U}/cap/full.log", "w").write("\n".join(full) + "\n")
        keep = [l for l in full if l.startswith(("LLCHAR", "LLDUMP", "LLCHECK", "LLECB", "LLLEN", "LLSPEC", "LLFALL", "LLLOOP", "LLLEAN", "LLDJ", "LLE1", "LLSKIP"))]
        open(out, "w").write("\n".join(keep) + ("\n" if keep else ""))
        crash = next((l for l in full if "assertion" in l), "")
        if crash: shutil.copy(f"{U}/cap/full.log", f"{O}/{name}_bf_crash.log")
        begin = next((l for l in keep if l.startswith("LLDUMP begin")), "no begin line")
        n = {k: count(keep, k) for k in ("LLDUMP", "LLCHECK", "LLECB", "LLLEN", "LLSPEC ", "LLSPECLEN")}
        spec_end = next((l for l in keep if l.startswith("LLSPEC end")), "no LLSPEC end")
        llchar = next((l for l in keep if l.startswith("LLCHAR")), "no LLCHAR line")
        # guard: the fighter must not change during the capture (Zelda/Sheik transformed in bf8 and mixed
        # two characters into one log): the event's fighter line and the table header must name one kind
        kinds = {m.group(1) for l in keep for m in [re.match(r"LLECB (?:fighter|begin char) (\d+)", l)] if m}
        kind_changed = len(kinds) > 1
        # guard: the run must be this fighter's (a run that never started leaves another fighter's lines)
        m = re.match(r"LLCHAR (\d+)", llchar)
        wrong_char = not m or int(m.group(1)) != CKIND[c]
        if wrong_char and os.path.exists(out):
            os.replace(out, f"{O}/{name}_bf{RUN}_wrongchar.log")
            job.log(f"{c}: {llchar!r}, want LLCHAR {CKIND[c]}; the run did not capture this fighter (log kept as "
                    f"{name}_bf{RUN}_wrongchar.log)", "error")
        elif kind_changed:
            os.replace(out, f"{O}/{name}_bf{RUN}_kindchange.log")
            job.log(f"{c}: fighter kind changed during the capture ({sorted(kinds)}); log kept as "
                    f"{name}_bf{RUN}_kindchange.log and left out of packing", "error")
        good = n["LLLEN"] > 0 and spec_end.startswith("LLSPEC end") and not crash and not aborted and not kind_changed and not wrong_char
        summary = (f"{c} {'OK' if good else 'FAIL'} lines {len(keep)} dump {n['LLDUMP']} check {n['LLCHECK']} "
                   f"ecb {n['LLECB']} len {n['LLLEN']} spec {n['LLSPEC ']} speclen {n['LLSPECLEN']} | {llchar} | {begin} | {spec_end}" + (f" | {crash}" if crash else "")
                   + (f" | aborted: {aborted}" if aborted else "") + (f" | KIND CHANGED {sorted(kinds)}" if kind_changed else "")
                   + (f" | WRONG FIGHTER, want LLCHAR {CKIND[c]}" if wrong_char else ""))
        log(summary)
        if good: ok += 1
        else: failed += 1
        job.advance(1, msg=summary[:120])
        job.log(summary[:200], "ok" if good else "error")
        job.update(fields={"ok": ok, "failed": failed, "last": f"{c}: {'OK' if good else 'FAIL'}", "run time": "-"})
        lean = count(keep, "LLLEAN")
        lc = subprocess.run(["python3", f"{C}/tools/llsim/leancheck.py", out], capture_output=True, text=True, cwd=f"{C}/tools/llsim")
        lcl = (lc.stdout.strip().splitlines() or [lc.stderr.strip()[-100:]])
        log(f"{c} lean rows {lean} | leancheck: " + " ; ".join(lcl[-3:]))
        job.update(fields={"fighter": c, "step": "captured", "lean rows": lean, "leancheck": lcl[-1][:80] if lcl else "-"})
    if os.path.exists(ISO): os.remove(ISO)
    log("end")
    plan_waiting("nothing; capture finished")
    if failed:
        raise RuntimeError(f"{failed} of {len(CHARS)} characters failed; see capture_all{RUN}.log")
