#!/usr/bin/env bash
# Step 09 - build the Mini Linux package as .apk files you can install on the running watch
# (no re-flashing, the watch keeps its WiFi, pairings and files).
#
#   bash build/09-build-update.sh            urbane-mini-linux (the only kit package)
#
# The .apk lands in <kit>/out/apk/. Install it from Windows with
#   powershell -ExecutionPolicy Bypass -File flash\10-update-watch.ps1 -NoKernel
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
KIT=$(dirname "$HERE")
OUT="$KIT/out/apk"
die() { echo "error: $*" >&2; exit 1; }

command -v pmbootstrap >/dev/null || die "pmbootstrap not found"
APORTS=$(pmbootstrap config aports | tail -n 1)
WORK=$(pmbootstrap config work | tail -n 1)
ARCH=armv7
[ $# -gt 0 ] || set -- urbane-mini-linux
mkdir -p "$OUT"
# 0.21: no Minecraft servers in the kit any more; old ones must not be installed again
rm -f "$OUT"/cuberite-[0-9]*.apk "$OUT"/bareiron-[0-9]*.apk

apkvar() { sed -n "s/^$2=//p" "$1/APKBUILD" | head -n 1; }

# ------------------------------------------------------------------ build
for pkg in "$@"; do
	case "$pkg" in
		urbane-mini-linux) src="$KIT/port/urbane-mini-linux" ;;
		*) die "unknown package: $pkg (only urbane-mini-linux)" ;;
	esac
	echo "==> $pkg"
	dst="$APORTS/main/$pkg"
	rm -rf "$dst"
	cp -r "$src" "$dst"
	pmbootstrap checksum "$pkg"
	pmbootstrap build --force --arch "$ARCH" "$pkg"

	ver="$(apkvar "$dst" pkgver)-r$(apkvar "$dst" pkgrel)"
	apk=$(find "$WORK/packages" -path "*/$ARCH/$pkg-$ver.apk" | head -n 1)
	[ -n "$apk" ] || die "built $pkg but cannot find $pkg-$ver.apk under $WORK/packages"
	rm -f "$OUT/$pkg"-[0-9]*.apk
	cp -v "$apk" "$OUT/"
done

cat <<EOF

==> Packages in $OUT:
$(ls -1 "$OUT")

Install on the watch (USB), from Windows PowerShell in the kit folder:
    powershell -ExecutionPolicy Bypass -File flash\\10-update-watch.ps1 -NoKernel
EOF
