#!/usr/bin/env python3
"""Print the Dolphin render area as a grab geometry.

Windowed (Hyprland): a grim geometry "X,Y WxH", only if Dolphin is floating, on the visible workspace of its
monitor, not hidden, and no other floating/fullscreen window on that workspace overlaps it. Exit 1 otherwise.
Headless (DISPLAY set and HEADLESS=1, the test Dolphin on Xvfb): an ImageMagick crop "WxH+X+Y" of the largest
mapped window on that X display (the render window; no window manager runs there). Exit 1 if none."""
import json, os, subprocess, sys

if os.environ.get("HEADLESS"):
    from Xlib import display, X
    d = display.Display(os.environ.get("XDISPLAY", ":99"))
    root = d.screen().root
    best = None
    def walk(w, ox, oy):
        global best
        for c in w.query_tree().children:
            a = c.get_attributes()
            if a.map_state != X.IsViewable:
                continue
            g = c.get_geometry()
            x, y = ox + g.x, oy + g.y
            if g.width * g.height > (best[0] * best[1] if best else 0):
                best = (g.width, g.height, x, y)
            walk(c, x, y)
    walk(root, 0, 0)
    if not best or best[0] < 64:
        sys.exit(1)
    w, h, x, y = best
    gw = round(h * 73 / 60)
    if gw < w:
        x, w = x + (w - gw) // 2, gw
    print('%dx%d+%d+%d' % (w, h, x, y))
    sys.exit(0)

cl = json.loads(subprocess.check_output(['hyprctl', 'clients', '-j'], timeout=5))
mons = json.loads(subprocess.check_output(['hyprctl', 'monitors', '-j'], timeout=5))
d = [c for c in cl if c['class'] == 'dolphin-emu' and c['mapped'] and not c['hidden']]
if len(d) != 1 or not d[0]['floating']: sys.exit(1)
d = d[0]; (x, y), (w, h) = d['at'], d['size']
gw = round(h * 73 / 60); rx, ry, rw, rh = x + (w - gw) // 2, y, gw, h
if not any(m['id'] == d['monitor'] and m['activeWorkspace']['id'] == d['workspace']['id'] for m in mons): sys.exit(1)
for c in cl:
    if c is d or not c['mapped'] or c['hidden'] or c['workspace']['id'] != d['workspace']['id']: continue
    if not (c['floating'] or c['fullscreen']): continue
    (cx, cy), (cw, ch) = c['at'], c['size']
    if cx < rx + rw and rx < cx + cw and cy < ry + rh and ry < cy + ch: sys.exit(1)
print('%d,%d %dx%d' % (rx, ry, rw, rh))
