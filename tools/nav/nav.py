#!/usr/bin/env python3
"""nav.py: closed-loop menu navigation for the test Dolphin (menus.toml has the map).

  nav.py watch [SECONDS]              print memory values as they change (calibration)
  nav.py goto event "Landing Lab"     steer the event list onto an event and press A
  nav.py pick fighter Fox             steer the CSS cursor onto a portrait and press A
  nav.py route "Landing Lab" Falcon   the whole boot path: event, fighter, START

Start it BEFORE Dolphin: MemoryWatcher reads its Locations.txt at boot and only
sends values when they change. Every step is checked against game memory and
gives up after menus.toml's step_limit with "NAV FAILED <why>" (exit code 2).
"""
import sys, time
sys.path.insert(0, __import__("os").path.dirname(__file__))
from navlib import Mem, Pad, Nav, NavError, load_menus


def log(msg):
    print(time.strftime("%H:%M:%S"), msg, flush=True)


def main(argv):
    menus = load_menus()
    mem = Mem(menus["memory"]).start()
    cmd = argv[0] if argv else "help"
    if cmd == "watch":
        until = time.time() + (float(argv[1]) if len(argv) > 1 else 600)
        last = None
        while time.time() < until:
            snap = mem.snapshot()
            if snap != last:
                pretty = {k: f"{v:08x}" for k, v in snap.items()}
                pretty["css_xy"] = f"{mem.f32('css_x'):.2f},{mem.f32('css_y'):.2f}"
                log(f"frame {mem.frames} " + " ".join(f"{k}={v}" for k, v in pretty.items()))
                last = snap
            time.sleep(1 / 30)
        return 0
    if cmd not in ("goto", "pick", "route"):
        print(__doc__)
        return 1
    log("NAV waiting for Dolphin (MemoryWatcher)")
    mem.wait(lambda: mem.frames > 0, 120, "the first MemoryWatcher message (is Dolphin running?)")
    nav = Nav(mem, Pad(), menus, log)
    try:
        if cmd == "goto":
            nav.goto_event(argv[2])
        elif cmd == "pick":
            nav.pick_fighter(argv[2])
        else:
            nav.goto_event(argv[1])
            nav.pick_fighter(argv[2])
            nav.start_match()
        log("NAV done")
        return 0
    except NavError as e:
        log(f"NAV FAILED {e}")
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
