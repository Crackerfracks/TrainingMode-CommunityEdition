#!/usr/bin/env python3
"""padshots.py OFF OUTDIR: on each LLSHOT, hold the pad state its label asks for, grab, release, D-pad down. Exits on LLDONE."""
import os, sys, time, subprocess, re
U = os.path.expanduser('~/.local/share/tmce-landinglab-dolphin')
off, out = int(sys.argv[1]), sys.argv[2]; os.makedirs(out, exist_ok=True)
pipe = os.open(U + '/Pipes/pad1', os.O_WRONLY)
def send(*cmds):
    for c in cmds: os.write(pipe, (c + '\n').encode())
RULES = [('nothing held', [], []),
         ('L about 20 percent', ['SET L 0.2', 'SET R 0.6', 'SET MAIN 0.85 0.15'], ['SET L 0', 'SET R 0', 'SET MAIN 0.5 0.5']),
         ('L clicked', ['SET L 1.0', 'PRESS L', 'PRESS A', 'PRESS Y', 'SET C 0.5 1.0'], ['RELEASE L', 'SET L 0', 'RELEASE A', 'RELEASE Y', 'SET C 0.5 0.5']),
         ('same as the crest medium', ['SET L 0.2', 'SET R 0.6', 'SET MAIN 0.85 0.15'], ['SET L 0', 'SET R 0', 'SET MAIN 0.5 0.5'])]
f = open(U + '/Logs/dolphin.log', 'rb'); f.seek(off); buf = b''; t0 = time.time(); n = 0
while time.time() - t0 < 900:
    chunk = f.read()
    if not chunk: time.sleep(0.1); continue
    buf += chunk; *ls, buf = buf.split(b'\n')
    for ln in ls:
        s = ln.decode(errors='replace').rstrip('\r\n ').split(']: ', 1)[-1]
        if s.startswith(('LLSHOT', 'LLMARK', 'LLVIEW', 'LLDONE', 'LLRUN', 'LLSCRIPT', 'LLDECK')): print(s, flush=True)
        if s.startswith('LLSHOT'):
            last_shot_frame = int(s.rsplit(' ', 1)[-1]); globals()['last_shot_frame'] = last_shot_frame
            n += 1; hold, rel = [], []
            for key, h, r in RULES:
                if key in s: hold, rel = h, r; break
            time.sleep(0.4); send(*hold); time.sleep(0.6)
            g = subprocess.run([U + '/gamearea.py'], capture_output=True, text=True).stdout.strip()
            if os.environ.get('HEADLESS'):      # test Dolphin on Xvfb: grab that display, not the screen
                subprocess.run(['import', '-display', os.environ.get('XDISPLAY', ':99'), '-window', 'root',
                                '-crop', g, '+repage', '-resize', '50%', os.path.join(out, 'shot%d.png' % n)])
            else:
                subprocess.run(['grim', '-t', 'png', '-s', '0.5', '-g', g, os.path.join(out, 'shot%d.png' % n)])
            send(*rel); time.sleep(0.3)
            send('PRESS D_DOWN'); time.sleep(0.12); send('RELEASE D_DOWN'); time.sleep(0.5)
        if s.startswith('LLDONE'): sys.exit(0)
