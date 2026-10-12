#!/bin/bash
# launch.sh ISO [extra dolphin args...]
iso="$1"; shift
exec nice -n 10 "/home/svaughn/.config/Slippi Launcher/netplay-beta/Slippi_Netplay_Mainline-x86_64.AppImage" -u "/home/svaughn/.local/share/tmce-landinglab-dolphin" -b -e "$iso" "$@" > "/home/svaughn/.local/share/tmce-landinglab-dolphin/stdout.log" 2>&1
