#!/usr/bin/env bash
# Step 08 - the Mini Linux boot logo instead of LG's (the picture the bootloader shows).
#
#   bash build/08-boot-logo.sh                  the Mini Linux logo (port/urbane-mini-linux/logo.png)
#   bash build/08-boot-logo.sh my.png           another picture (any size; PNG, 8-bit)
#   bash build/08-boot-logo.sh --restore        put LG's original pictures back
#
# The logo follows the screen rotation: "watch rotate 90" also turns it (from the
# next start), so it is written upright here and turned to the current setting.
#   options: --entry NAME (which picture to replace), --box (keep the old picture's
#            box instead of the whole 320x320 screen), --flip-y / --flip-x (if it
#            comes out mirrored), --ip ADDRESS
#
# The pictures live in the "imgdata" partition. The script copies it from the watch
# (LG's original is kept as out/bootlogo/imgdata-original.img, from backup/ if step 06 ran), swaps one picture,
# shows you the result and asks before it writes anything. The watch password is
# asked for by sudo on the watch. Nothing else is touched; --restore undoes it.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
KIT=$(dirname "$HERE")
OUT="$KIT/out/bootlogo"
TOOL="$HERE/lib/imgdata.py"
IP=172.16.42.1
PIC="$KIT/port/urbane-mini-linux/logo.png"
ENTRY=""
RESTORE=0
OPTS=(--full --round)
die() { echo "error: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
	case "$1" in
		--restore) RESTORE=1 ;;
		--entry) ENTRY=${2:?name}; shift ;;
		--box) OPTS=() ;;
		--flip-x|--flip-y) OPTS+=("$1") ;;
		--ip) IP=${2:?address}; shift ;;
		-h|--help) sed -n '2,16p' "$0"; exit 0 ;;
		*) PIC=$1 ;;
	esac
	shift
done

. "$HERE/lib/common.sh"
W="$(watch_user "$KIT")@$IP"
mkdir -p "$OUT"
ssh -o ConnectTimeout=5 "$W" true || die "cannot reach $W over SSH"

# the partition, found by its name (never by number)
PART=$(ssh "$W" 'p=$(readlink -f /dev/disk/by-partlabel/imgdata 2>/dev/null)
	if [ ! -b "$p" ]; then
		for u in /sys/class/block/*/uevent; do
			grep -qx PARTNAME=imgdata "$u" && { p=/dev/$(sed -n "s/^DEVNAME=//p" "$u"); break; }
		done
	fi
	[ -b "$p" ] && echo "$p"')
[ -n "$PART" ] || die "no partition named imgdata on the watch"
echo "==> imgdata partition: $PART"

echo "==> copying it from the watch (sudo password of the watch)"
ssh -t "$W" "sudo dd if=$PART of=/tmp/imgdata.img bs=64k status=none && sudo chown \$(id -u) /tmp/imgdata.img"
scp -q "$W:/tmp/imgdata.img" "$OUT/imgdata-current.img"
head -c 8 "$OUT/imgdata-current.img" | grep -q 'IMGDATA!' || die "the partition does not start with IMGDATA! - unknown format, stopping"
# LG's original: from the step-06 backup if there is one, else the first copy made here
if [ ! -f "$OUT/imgdata-original.img" ]; then
	if [ -f "$KIT/backup/imgdata.img" ]; then cp "$KIT/backup/imgdata.img" "$OUT/imgdata-original.img"
	else cp "$OUT/imgdata-current.img" "$OUT/imgdata-original.img"; fi
	echo "    LG's original kept: out/bootlogo/imgdata-original.img"
fi
SIZE=$(stat -c %s "$OUT/imgdata-current.img")

if [ $RESTORE = 1 ]; then
	NEW="$OUT/imgdata-original.img"
	echo "==> restoring LG's original pictures"
else
	python3 "$TOOL" extract "$OUT/imgdata-original.img" "$OUT/original" >/dev/null
	echo "==> pictures in the partition (originals saved as PNG in out/bootlogo/original/):"
	python3 "$TOOL" list "$OUT/imgdata-current.img" | sed 's/^/    /'
	if [ -z "$ENTRY" ]; then
		ENTRY=$(python3 "$TOOL" list "$OUT/imgdata-current.img" | awk 'tolower($1) ~ /^(boot|lglogo|lg_logo|logo|bootlogo)/ { print $1; exit }')
		[ -n "$ENTRY" ] || die "no picture looks like the boot logo - look at out/bootlogo/original/ and pass --entry NAME"
	fi
	echo "==> replacing '$ENTRY' with $(basename "$PIC") (this takes a few seconds)"
	NEW="$OUT/imgdata-new.img"
	python3 "$TOOL" replace "$OUT/imgdata-current.img" "$NEW" "$ENTRY" "$PIC" --max-size "$SIZE" "${OPTS[@]}"
	echo "    preview: out/bootlogo/imgdata-new.img.preview.png (compare with original/$ENTRY.png)"
fi

read -r -p "Write it to the watch now? [y/N] " ok
[ "${ok:-}" = y ] || [ "${ok:-}" = Y ] || { echo "nothing written"; exit 0; }

scp -q "$NEW" "$W:/tmp/imgdata-new.img"
want=$(sha256sum "$NEW" | cut -d' ' -f1)
ssh -t "$W" "set -e; head -c 8 /tmp/imgdata-new.img | grep -q 'IMGDATA!'
	[ \$(stat -c %s /tmp/imgdata-new.img) -le \$(sudo blockdev --getsize64 $PART) ]
	sudo dd if=/tmp/imgdata-new.img of=$PART bs=64k conv=fsync status=none
	got=\$(sudo head -c \$(stat -c %s /tmp/imgdata-new.img) $PART | sha256sum | cut -d' ' -f1)
	[ \"\$got\" = $want ] && echo 'written and verified' || { echo 'VERIFY FAILED - run: bash build/08-boot-logo.sh --restore'; exit 1; }
	rm -f /tmp/imgdata.img /tmp/imgdata-new.img
	# keep the logo turned like the screen (watch rotate) from now on
	if command -v watch-bootlogo >/dev/null; then
		if [ $RESTORE = 1 ]; then sudo watch-bootlogo forget
		else sudo watch-bootlogo mark '$ENTRY' && sudo watch-bootlogo apply; fi
	fi"
echo "==> done. The new logo shows at the next boot (ssh -t $W sudo reboot)."
echo "    Mini Linux is fully installed. Updates later: build/09-build-update.sh + flash\\10-update-watch.ps1"
