#!/bin/sh
# Runs ON THE WATCH: why can't a program reach the internet over HTTPS?
#   ssh <user>@172.16.42.1 sh -s < build/tools/net-check.sh > debug/net-check.txt
sec() { echo; echo "===== $*"; }
sec "clock (must be today, TLS fails otherwise)"; date; date -u
sec "addresses"; ip -br addr
sec "routes"; ip route; ip -6 route 2>/dev/null | head -5
sec "dns"; cat /etc/resolv.conf
for h in nura.eco github.com; do
	sec "resolve $h"; nslookup "$h" 2>&1 | tail -n 6
done
sec "https"
for u in https://nura.eco/ https://github.com/; do
	if command -v curl >/dev/null; then
		curl -sS -o /dev/null -m 15 -w "$u -> %{http_code} (%{remote_ip})\n" "$u" 2>&1
	else
		wget -q -T 15 -O /dev/null "$u" 2>&1 && echo "$u -> ok" || echo "$u -> failed"
	fi
done
sec "time service"; rc-service urbane-timesync status 2>&1; pgrep -a ntpd
sec "end"
