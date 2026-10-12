#!/bin/bash
# hlaunch.sh ISO [extra dolphin args...]: launch.sh, but on a hidden X display (Xvfb), nothing on Stephen's screens.
# hlaunch.sh stop: stop the Xvfb that an earlier hlaunch.sh started (one that was already up is left alone).
# Vulkan presents through Mesa's software path (MESA_VK_WSI_DEBUG=sw; Xvfb has no DRI3) and still renders on the
# GPU: about 60 FPS, where OpenGL on Xvfb falls back to llvmpipe at ~3 FPS. Pipe input works without focus.
U=~/.local/share/tmce-landinglab-dolphin; XD=${XDISPLAY:-:99}; PIDF=$U/xvfb.pid
if [ "$1" = stop ]; then
  [ -f $PIDF ] && kill "$(cat $PIDF)" 2>/dev/null; rm -f $PIDF; exit 0
fi
if [ ! -e /tmp/.X11-unix/X${XD#:} ]; then
  Xvfb $XD -screen 0 1280x1024x24 -nolisten tcp > $U/xvfb.log 2>&1 &
  echo $! > $PIDF
  for _ in $(seq 50); do [ -e /tmp/.X11-unix/X${XD#:} ] && break; sleep 0.1; done
fi
iso="$1"; shift
exec env -u WAYLAND_DISPLAY DISPLAY=$XD QT_QPA_PLATFORM=xcb MESA_VK_WSI_DEBUG=sw \
  "$(dirname "$(readlink -f "$0")")/launch.sh" "$iso" -v Vulkan "$@"
