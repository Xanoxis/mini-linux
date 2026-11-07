#!/usr/bin/env bash
# Keep a live copy of the watch's kernel messages on the PC (survives a watch crash).
#   bash build/tools/klog.sh [IP]     -> <kit>/debug/klog-<time>.txt   (Ctrl-C to stop)
# Keeps retrying, so it can be started BEFORE the watch boots: it connects as soon as
# SSH is up and starts a new file for every boot.
HERE=$(cd "$(dirname "$0")" && pwd)
KIT=$(dirname "$(dirname "$HERE")")
OUT="$KIT/debug"
. "$KIT/build/lib/common.sh"
U=$(watch_user "$KIT")
IP=${1:-172.16.42.1}
mkdir -p "$OUT"
echo "Waiting for the watch at $IP (Ctrl-C to stop) ..."
while :; do
	if ssh -o ConnectTimeout=3 -o ServerAliveInterval=2 -o ServerAliveCountMax=2 \
		-o BatchMode=yes "$U@$IP" true 2>/dev/null; then
		F="$OUT/klog-$(date +%Y%m%d-%H%M%S).txt"
		echo "== connected, writing $F"
		ssh -o ServerAliveInterval=2 -o ServerAliveCountMax=2 -o BatchMode=yes \
			"$U@$IP" 'uname -v; cat /proc/cmdline; sudo -n watch-cpu klog' | tee "$F"
		echo "== connection lost $(date +%T) (watch rebooted or crashed?)"
	fi
	sleep 1
done
