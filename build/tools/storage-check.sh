#!/bin/sh
# Run on the watch or pipe to ssh USER@172.16.42.1 sh -s -- [SECONDS].
set -eu

seconds=${1:-60}
case "$seconds" in
	''|*[!0-9]*) echo "Interval must be a positive integer in seconds" >&2; exit 2 ;;
esac
[ "$seconds" -gt 0 ] || exit 2

stats=/sys/block/mmcblk0/stat
[ -r "$stats" ] || { echo "eMMC statistics unavailable at $stats" >&2; exit 1; }

echo 'Root and temporary mounts'
awk '$2 == "/" || $2 == "/boot" || $2 == "/tmp" || $2 == "/run" {print}' /proc/mounts
echo 'Active swap'
cat /proc/swaps
echo 'RAM and swap (kB)'
awk '/^(MemTotal|MemAvailable|SwapTotal|SwapFree):/ {print}' /proc/meminfo
if [ -r /sys/block/zram0/mm_stat ]; then
	awk '{printf "zram bytes: original %.0f, compressed %.0f, total RAM used %.0f\n", $1, $2, $3}' /sys/block/zram0/mm_stat
fi
if [ -r /sys/block/zram0/backing_dev ]; then
	printf 'zram backing device: '
	cat /sys/block/zram0/backing_dev
fi

before=$(cat "$stats")
printf 'Sampling eMMC host I/O for %s seconds...\n' "$seconds"
sleep "$seconds"
awk -v before="$before" '{
	split(before, start, " ")
	r = ($3 - start[3]) * 512
	w = ($7 - start[7]) * 512
	printf "Host I/O during sample: %.0f bytes read, %.0f bytes written\n", r, w
}' "$stats"
