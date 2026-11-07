#!/bin/sh
# Runs ON THE WATCH (via ssh). Read-only system report for debugging.
#   from Windows/WSL:  ssh <user>@172.16.42.1 sh -s < build/tools/watch-report.sh > debug/report.txt
sec() { echo; echo "===== $*"; }
sec "system"; uname -a; cat /etc/os-release 2>/dev/null | head -3; uptime; free -m
sec "dmesg (needs /tmp/dmesg.txt created with sudo, see the guide)"
if [ -r /tmp/dmesg.txt ]; then cat /tmp/dmesg.txt; else dmesg 2>&1; fi
sec "services"; rc-status -a 2>&1
sec "rfkill"; for r in /sys/class/rfkill/*; do [ -e "$r" ] && echo "$r name=$(cat $r/name) type=$(cat $r/type) soft=$(cat $r/soft 2>/dev/null) hard=$(cat $r/hard 2>/dev/null) state=$(cat $r/state)"; done
sec "bluetooth"; ls -la /sys/class/bluetooth 2>&1; bluetoothctl --timeout 3 show 2>&1 | head -20; ls -la /dev/ttyMSM* /dev/ttyHS* 2>&1
for d in /sys/bus/serial/devices/*; do [ -e "$d" ] && echo "$d driver=$(basename "$(readlink $d/driver 2>/dev/null)")"; done
sec "firmware files"; find /lib/firmware -iname '*brcm*' -o -iname '*bcm43*' -o -iname '*.hcd' 2>/dev/null | head -40; ls -la /lib/firmware/brcm 2>&1 | head -40
sec "wifi"; ip -br link; ip -br addr; iw dev 2>&1; nmcli -t dev 2>&1; ls /sys/bus/sdio/devices 2>&1; for d in /sys/bus/sdio/devices/*; do [ -e "$d" ] && echo "$d vendor=$(cat $d/vendor) device=$(cat $d/device) driver=$(basename "$(readlink $d/driver 2>/dev/null)")"; done
sec "display"; ls -la /dev/dri 2>&1; for s in /sys/class/drm/*/status; do [ -e "$s" ] && echo "$s $(cat $s)"; done; for b in /sys/class/backlight/*; do [ -e "$b" ] && echo "$b $(cat $b/brightness)/$(cat $b/max_brightness)"; done
sec "input"; cat /proc/bus/input/devices 2>&1 | grep -E '^(N|H):'
sec "power"; for p in /sys/class/power_supply/*; do [ -e "$p" ] || continue; echo "-- $p"; for f in type status capacity voltage_now current_now online health; do [ -r $p/$f ] && echo "  $f=$(cat $p/$f)"; done; done
sec "modules"; lsmod 2>&1 | head -60
sec "watch-session log"; cat ~/.cache/watch-session.log 2>&1 | tail -40
sec "storage"; df -h; lsblk 2>/dev/null || cat /proc/partitions
sec "end"
