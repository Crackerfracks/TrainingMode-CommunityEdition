#!/bin/bash
# run_iso.sh TAG: run the prebuilt test-TAG.iso (symlink ok) with padshots+watchdog; no fetch/build. Output $U/TAG.
# HEADLESS=1: run it on a hidden X display (hlaunch.sh) instead of the laptop panel.
U=~/.local/share/tmce-landinglab-dolphin; T="$1"
pgrep -x AppRun.wrapped && { echo DOLPHIN RUNNING; exit 1; }
grep -q 'SlotA = 1' $U/Config/Dolphin.ini || echo "SlotA not 1"
: > $U/Logs/dolphin.log
# closed-loop menus (tools/nav): nav binds the MemoryWatcher socket before Dolphin boots, then steers
# event list -> CSS -> match from game memory. NAVFIGHTER picks the portrait (LLCHAR builds override it).
NAV="/run/media/svaughn/External HD/Emulation/Mods/GC/Melee/TM-CE/TM-CE (CURRENT)/TM-CE_Claude_Landing_Trainer/TM-CE-landinglab-cloud/tools/nav/nav.py"
python3 "$NAV" route "Landing Lab" "${NAVFIGHTER:-Falcon}" > $U/nav.out 2>&1 &
NAVPID=$!; sleep 1
if [ -n "$HEADLESS" ]; then
  # hidden X display (hlaunch.sh); no workspace, no focus guard. Stop the Xvfb we started on any exit.
  export HEADLESS; trap '$U/hlaunch.sh stop' EXIT
  ($U/hlaunch.sh $U/test-$T.iso &)
else
  ($U/focusguard.sh 1200 &)
  # the test Dolphin goes on the laptop panel's visible workspace, or the external monitor's when a window of
  # Stephen's is fullscreen there (pickws.py)
  WS=$($U/pickws.py)
  hyprctl dispatch "hl.dsp.exec_cmd('$U/launch.sh $U/test-$T.iso', { workspace = '$WS silent', float = true, no_initial_focus = true, suppress_event = 'fullscreen maximize fullscreenoutput activate activatefocus' })" >/dev/null
fi
wait $NAVPID; nrc=$?; grep NAV $U/nav.out; [ $nrc = 0 ] || { echo "NAV FAILED rc $nrc"; for p in $(pgrep -f "AppRun.wrapped -u $U -b -e"); do kill $p; done; exit 2; }
sleep 2
rm -rf $U/$T; mkdir -p $U/$T
(timeout 900 python3 $U/padshots.py 0 $U/$T > $U/$T/padshots.out 2>&1 &)
timeout 900 $U/watchdog.sh 20; echo wd=$?
tr -d '\r' < $U/Logs/dolphin.log | sed 's/^.*N\[OSREPORT\]: //' > $U/$T/full.log
grep -E '^LLDONE' $U/$T/full.log
