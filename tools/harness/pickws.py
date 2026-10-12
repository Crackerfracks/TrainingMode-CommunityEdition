#!/usr/bin/env python3
"""Print the workspace for the test Dolphin (Stephen wants it on the laptop screen): the laptop panel's visible
workspace unless a window of his there is fullscreen; else the external monitor's visible workspace without one
(note on stderr if none). run_iso.sh also suppresses
fullscreen/maximize/activate events for the test Dolphin, so his windows are never maximized by it."""
import json, subprocess, sys
mons = json.loads(subprocess.check_output(['hyprctl', 'monitors', '-j'], timeout=5))
cl = [c for c in json.loads(subprocess.check_output(['hyprctl', 'clients', '-j'], timeout=5)) if c['class'] != 'dolphin-emu']
lap = next((m for m in mons if m['name'].startswith('eDP')), mons[0])
ext = [m for m in mons if m is not lap and not m.get('disabled')]
def his(ws): return [c for c in cl if c['workspace']['id'] == ws]
cands = [lap] + ext
for m in cands:
    if not any(c['fullscreen'] for c in his(m['activeWorkspace']['id'])):
        print(m['activeWorkspace']['id']); sys.exit()
print(f'pickws: every visible workspace has a fullscreen window; using the laptop panel', file=sys.stderr)
print(lap['activeWorkspace']['id'])
