#!/usr/bin/env python3
"""Drive the test Dolphin's pipe controller.
Usage: pad.py "A" "wait 0.5" "A" "stick 0.5 1.0 0.2" "hold START 0.1" ...
  BUTTON        tap (held 100 ms)
  hold B S      hold button B for S seconds
  wait S        sleep S seconds
  stick X Y S   main stick to X,Y (0..1, 0.5 neutral) for S seconds, then neutral
  cstick X Y S  same for the C-stick
  raw TEXT      send TEXT as a pipe command
  shot / pause / step [N] / savestate / loadstate   Dolphin hotkeys
"""
import os, sys, time
base = os.path.expanduser('~/.local/share/tmce-landinglab-dolphin/Pipes/')
fd = os.open(base + 'pad1', os.O_WRONLY)
hk = None
def send(s): os.write(fd, (s + '\n').encode())
def hotkey(btn, dur=0.4):
    # hotkeys pipe: A screenshot, B pause, X frame advance, Y save slot 1, Z load slot 1
    global hk
    if hk is None:
        hk = os.open(base + 'hotkeys', os.O_WRONLY)
    os.write(hk, ('PRESS %s\n' % btn).encode()); time.sleep(dur); os.write(hk, ('RELEASE %s\n' % btn).encode())
for arg in sys.argv[1:]:
    t = arg.split()
    op = t[0]
    if op == 'wait':
        time.sleep(float(t[1]))
    elif op == 'hold':
        send('PRESS ' + t[1]); time.sleep(float(t[2])); send('RELEASE ' + t[1])
    elif op in ('stick', 'cstick'):
        ax = 'MAIN' if op == 'stick' else 'C'
        send(f'SET {ax} {t[1]} {t[2]}'); time.sleep(float(t[3])); send(f'SET {ax} 0.5 0.5')
    elif op == 'shot':
        hotkey('A')
    elif op == 'pause':
        hotkey('B')
    elif op == 'step':
        for _ in range(int(t[1]) if len(t) > 1 else 1):
            hotkey('X', 0.03); time.sleep(0.25)
    elif op == 'savestate':
        hotkey('Y')
    elif op == 'loadstate':
        hotkey('Z')
    elif op == 'slower':
        for _ in range(int(t[1]) if len(t) > 1 else 1):
            hotkey('L'); time.sleep(0.1)
    elif op == 'faster':
        for _ in range(int(t[1]) if len(t) > 1 else 1):
            hotkey('R'); time.sleep(0.1)
    elif op == 'raw':
        send(' '.join(t[1:]))
    else:
        send('PRESS ' + op); time.sleep(0.1); send('RELEASE ' + op)
    time.sleep(0.05)
os.close(fd)
