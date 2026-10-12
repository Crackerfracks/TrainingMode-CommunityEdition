"""Closed-loop menu navigation for the test Dolphin.

Three small pieces:
  Mem    reads game memory through Slippi Dolphin's MemoryWatcher socket
  Pad    drives the pipe controller (Pipes/pad1)
  Nav    steers a menu cursor onto a named item from menus.toml, confirms it,
         presses A and checks that the scene moved on

The menu map lives in menus.toml next to this file; nothing here is a timing.
"""
import os, socket, struct, threading, time, tomllib

HERE = os.path.dirname(os.path.abspath(__file__))
USERDIR = os.path.expanduser("~/.local/share/tmce-landinglab-dolphin")


class NavError(RuntimeError):
    pass


def load_menus(path=os.path.join(HERE, "menus.toml")):
    with open(path, "rb") as f:
        return tomllib.load(f)


class Mem:
    """MemoryWatcher reader. Dolphin reads Locations.txt only when emulation
    starts and sends a value only when it changes (a value of 0 is never
    sent), so start() must run before Dolphin does."""

    def __init__(self, entries, userdir=USERDIR):
        self.entries = dict(entries)          # name -> Locations.txt line
        self.dir = os.path.join(userdir, "MemoryWatcher")
        self.values = {}                      # Locations line -> u32
        self.changed = {}                     # Locations line -> time of last change
        self.frames = 0                       # datagrams seen (one per emulated frame)
        self.lock = threading.Lock()
        self.sock = None

    def start(self):
        os.makedirs(self.dir, exist_ok=True)
        with open(os.path.join(self.dir, "Locations.txt"), "w") as f:
            f.write("\n".join(self.entries.values()) + "\n")
        path = os.path.join(self.dir, "MemoryWatcher")
        try:
            os.unlink(path)
        except OSError:
            pass
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        self.sock.bind(path)
        threading.Thread(target=self._pump, daemon=True).start()
        return self

    def _pump(self):
        while True:
            data = self.sock.recv(65536)
            lines = data.rstrip(b"\0").decode(errors="replace").split("\n")
            now = time.time()
            with self.lock:
                self.frames += 1
                for key, val in zip(lines[0::2], lines[1::2]):
                    try:
                        self.values[key] = int(val, 16)
                        self.changed[key] = now
                    except ValueError:
                        pass

    def u32(self, name):
        with self.lock:
            return self.values.get(self.entries[name], 0)

    def byte(self, name, i):
        """byte i (0 = most significant) of the u32 at name"""
        return (self.u32(name) >> (24 - 8 * i)) & 0xFF

    def f32(self, name):
        return struct.unpack(">f", self.u32(name).to_bytes(4, "big"))[0]

    def scene(self):
        return self.byte("scene", 0), self.byte("scene", 3)

    def snapshot(self):
        with self.lock:
            return {n: self.values.get(k, 0) for n, k in self.entries.items()}

    def wait(self, pred, limit, what):
        t0 = time.time()
        while time.time() - t0 < limit:
            if pred():
                return
            time.sleep(1 / 120)
        raise NavError(f"timed out after {limit}s waiting for {what}")

    def wait_frames(self, n):
        start = self.frames
        self.wait(lambda: self.frames >= start + n, 5 + n / 30, f"{n} frames (is emulation paused?)")


class Pad:
    """Pipe controller 1. Stick axes are 0..1 with 0.5 neutral; y = 1 is up."""

    def __init__(self, userdir=USERDIR):
        self.fd = os.open(os.path.join(userdir, "Pipes", "pad1"), os.O_WRONLY)

    def send(self, s):
        os.write(self.fd, (s + "\n").encode())

    def stick(self, x, y):
        self.send(f"SET MAIN {x:.3f} {y:.3f}")

    def neutral(self):
        self.stick(0.5, 0.5)

    def press(self, button, mem, frames=4):
        self.send("PRESS " + button)
        mem.wait_frames(frames)
        self.send("RELEASE " + button)
        mem.wait_frames(2)


class Nav:
    def __init__(self, mem, pad, menus, log=print):
        self.mem, self.pad, self.menus, self.log = mem, pad, menus, log
        self.steer = menus["steer"]

    # -- event list: discrete moves ------------------------------------------
    def event_state(self):
        m = self.menus["menu"]["event"]
        page = self.mem.byte(m["page"], 1)
        index = self.mem.byte(m["row"], 0) + self.mem.u32(m["scroll"])
        return page, index

    def tap(self, x, y, changed, what):
        """push the stick until changed() turns true, then release"""
        self.pad.stick(x, y)
        try:
            self.mem.wait(changed, 2, what)
        finally:
            self.pad.neutral()
        self.mem.wait_frames(3)

    def goto_event(self, name):
        m = self.menus["menu"]["event"]
        target = m["items"][name]
        self.mem.wait(lambda: list(self.mem.scene()) == m["scene"], 60, "the event list")
        self.mem.wait_frames(20)
        t0 = time.time()
        while True:
            page, index = self.event_state()
            if (page, index) == (target["page"], target["index"]):
                break
            if time.time() - t0 > self.steer["step_limit"]:
                raise NavError(f"event list: stuck at page {page} index {index}, want {target}")
            if page != target["page"]:
                x = 1.0 if page < target["page"] else 0.0
                self.tap(x, 0.5, lambda: self.event_state()[0] != page, f"page change from {page}")
            else:
                y = 0.0 if index < target["index"] else 1.0
                self.tap(0.5, y, lambda: self.event_state()[1] != index, f"row change from {index}")
        self.log(f"NAV confirmed event {name!r} (page {page}, index {index})")
        before = self.mem.scene()
        self.pad.press("A", self.mem)
        self.mem.wait(lambda: self.mem.scene() != before, 10, f"the scene to leave the event list after A on {name!r}")
        self.log(f"NAV scene {before} -> {self.mem.scene()}")

    # -- character select: free pointer ----------------------------------------
    def pick_fighter(self, name):
        m = self.menus["menu"]["css"]
        l, r, u, d = m["items"][name]
        tx, ty = (l + r) / 2, (u + d) / 2
        s = self.steer
        self.mem.wait(lambda: list(self.mem.scene()) == m["scene"] and (self.mem.f32(m["x"]) != 0 or self.mem.f32(m["y"]) != 0),
                      s["step_limit"], "the CSS cursor")
        t0, settled = time.time(), 0
        while True:
            x, y = self.mem.f32(m["x"]), self.mem.f32(m["y"])
            dx, dy = tx - x, ty - y
            inside = l + 0.5 < x < r - 0.5 and d + 0.5 < y < u - 0.5
            if inside and abs(dx) < s["dead_zone"] and abs(dy) < s["dead_zone"]:
                self.pad.neutral()
                settled += 1
                if settled >= 3:
                    break
            else:
                settled = 0
                self.pad.stick(0.5 + self._push(dx), 0.5 + self._push(dy))
            if time.time() - t0 > s["step_limit"]:
                self.pad.neutral()
                raise NavError(f"CSS: cursor stuck at ({x:.1f}, {y:.1f}), want {name} at ({tx:.1f}, {ty:.1f})")
            self.mem.wait_frames(1)
        self.log(f"NAV confirmed fighter {name!r} at ({x:.1f}, {y:.1f})")
        state = lambda: (self.mem.u32("css_state") >> 16) & 0xFF
        before = state()
        self.pad.press("A", self.mem)
        self.mem.wait(lambda: state() != before, 3, f"the puck to drop on {name!r}")
        self.log(f"NAV puck down on {name!r} (cursor state {before} -> {state()})")

    def _push(self, dist):
        s = self.steer
        if abs(dist) < s["dead_zone"] / 2:
            return 0.0
        p = min(s["max_push"], max(s["min_push"], abs(dist) * s["gain"]))
        return p if dist > 0 else -p

    def start_match(self):
        before = self.mem.scene()
        self.pad.press("START", self.mem)
        self.mem.wait(lambda: self.mem.scene() != before, 10, "the match to start after START")
        self.log(f"NAV scene {before} -> {self.mem.scene()} (match)")
