#!/usr/bin/env bash
# Step 03 - build the Mini Linux image (Nura base) for the Urbane and copy the flashable files to
# <kit>/out so Windows' fastboot can reach them.
# The first run compiles the kernel and takes a while (30-90 min under WSL2).
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
KIT=$(dirname "$HERE")
OUT="$KIT/out"
EXPORT="$HOME/nura-export-lg-bass"

[ "$(pmbootstrap config device | tail -n 1)" = "lg-bass" ] \
	|| { echo "device is not lg-bass - run build/02-add-watch-port.sh first" >&2; exit 1; }

echo "==> Rebuilding the kit packages (pmbootstrap would otherwise reuse stale builds)"
pmbootstrap shutdown >/dev/null 2>&1 || true
pmbootstrap build --force device-lg-bass
pmbootstrap build --force --arch armv7 urbane-mini-linux

echo "==> Building and installing the rootfs (you will be asked for the user password)"
pmbootstrap install "$@"

echo "==> Exporting"
rm -rf "$EXPORT"
pmbootstrap export "$EXPORT"

mkdir -p "$OUT"
cp -Lv "$EXPORT"/boot.img "$OUT"/boot.img
cp -Lv "$EXPORT"/lg-bass.img "$OUT"/lg-bass.img
( cd "$OUT" && sha256sum boot.img lg-bass.img >SHA256SUMS )
# the user name on the watch, for the Windows scripts
pmbootstrap config user | tail -n 1 >"$OUT/watch-user.txt"

# LG's bootloader loads the ramdisk at 0x02900000; protected memory starts at 0x02f00000.
RD=$(python3 -c "import struct;print(struct.unpack('<I',open('$OUT/boot.img','rb').read(20)[16:20])[0])")
LIMIT=$((6 * 1024 * 1024 - 64 * 1024))
echo "==> initramfs in boot.img: $RD bytes (limit $LIMIT)"
if [ "$RD" -gt "$LIMIT" ]; then
	echo "ERROR: initramfs too big for the Urbane bootloader - do NOT boot this image." >&2
	mv "$OUT/boot.img" "$OUT/boot.img.TOO-BIG"
	exit 1
fi

# The matching kernel package (modules) for a permanent kernel update:
#   flash\10-update-watch.ps1 installs it together with boot-keep-rootfs.img
mkdir -p "$OUT/kernel"
rm -f "$OUT"/kernel/*.apk
KAPK=$(find "$(pmbootstrap config work | tail -n 1)/packages" -name 'linux-postmarketos-qcom-msm8226-*.apk' \
	-printf '%T@ %p\n' 2>/dev/null | sort -n | tail -n 1 | cut -d' ' -f2-)
[ -n "$KAPK" ] && cp -v "$KAPK" "$OUT/kernel/"

# A kernel for the rootfs that is ALREADY on the watch: same kernel/initramfs, but
# the rootfs UUIDs of the last flashed boot image (see flash\tools\fastboot.ps1, boot-flashed.img).
if [ -f "$OUT/boot-flashed.img" ]; then
	python3 "$HERE/lib/bootimg_uuid.py" "$OUT/boot-flashed.img" "$OUT/boot.img" "$OUT/boot-keep-rootfs.img"
	echo "    boot-keep-rootfs.img = new kernel for the rootfs already on the watch"
	echo "    (installed by flash\\10-update-watch.ps1)"
fi

cat <<EOF

==> Ready in: $OUT
    boot.img     kernel + initramfs + appended DTB  (fastboot boot / flash boot)
    lg-bass.img  root filesystem image               (fastboot flash userdata)

Next, on Windows, in PowerShell from the kit folder (watch in fastboot mode):
    powershell -ExecutionPolicy Bypass -File flash\\04-check-watch.ps1
    powershell -ExecutionPolicy Bypass -File flash\\05-install-system.ps1
(updates later: build/09-build-update.sh, then flash\\10-update-watch.ps1)
EOF
