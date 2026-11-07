#!/usr/bin/env bash
# Step 02 - add an "lg-bass" (LG Watch Urbane) port to your local pmaports.
#
# What it does
#   1. copies device-lg-lenok (LG G Watch R, the Urbane's near twin) to
#      device/testing/device-lg-bass and adapts it:
#        - boots with the stock LG bootloader + appended DTB (lk2nd has no bass support)
#        - kernel -> "boot" partition, rootfs image -> "userdata"
#        - boot image addresses read from a known-good bass boot image (AsteroidOS)
#   2. patches the msm8226 mainline kernel package to also build
#      qcom-apq8026-lg-bass.dtb, and enables BT keyboard + zram options
#   3. adds the "urbane-mini-linux" package (round-screen terminal session,
#      BT keyboard helper, WiFi firmware links, zram)
#   4. points pmbootstrap at lg-bass with UI "none" + urbane-mini-linux
#
# Safe to run again (e.g. after "pmbootstrap pull").
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
KIT=$(dirname "$HERE")
PATCH="python3 $HERE/lib/pmaports_patch.py"
die() { echo "error: $*" >&2; exit 1; }

command -v pmbootstrap >/dev/null || die "pmbootstrap not found - run build/01-setup-wsl.sh"
APORTS=$(pmbootstrap config aports 2>/dev/null | tail -n 1)
[ -d "$APORTS/device" ] || die "pmaports not found ($APORTS) - run 'pmbootstrap init' first"
echo "==> pmaports: $APORTS"

LENOK=$(find "$APORTS/device" -mindepth 2 -maxdepth 2 -type d -name device-lg-lenok | head -n 1)
KERNEL=$(find "$APORTS/device" -mindepth 2 -maxdepth 2 -type d -name linux-postmarketos-qcom-msm8226 | head -n 1)
[ -n "$LENOK" ] || die "device-lg-lenok not found in pmaports"
[ -n "$KERNEL" ] || die "linux-postmarketos-qcom-msm8226 not found in pmaports"

# ---------------------------------------------------------------- boot image layout
echo "==> Reading the boot image layout of a known-good Urbane image (AsteroidOS nightly)"
OFFSETS=""
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
for ASTEROID_URL in \
	https://release.asteroidos.org/nightlies/bass/asteroid-bass-boot.img \
	https://release.asteroidos.org/nightlies/bass/zImage-dtb-bass.fastboot; do
	if curl -fsSL --max-time 120 -r 0-4095 -o "$TMP/bass.img" "$ASTEROID_URL" 2>/dev/null &&
		OFFSETS=$($PATCH offsets "$TMP/bass.img"); then
		echo "    using $OFFSETS"
		echo "    (from $ASTEROID_URL)"
		break
	fi
	OFFSETS=""
done
[ -n "$OFFSETS" ] || echo "    could not read it - using built-in msm8226 defaults (see the guide, 'Boot loops')"

# ---------------------------------------------------------------- device package
BASS="$APORTS/device/testing/device-lg-bass"
echo "==> Creating $BASS from $(basename "$(dirname "$LENOK")")/device-lg-lenok"
rm -rf "$BASS"
mkdir -p "$(dirname "$BASS")"
cp -r "$LENOK" "$BASS"
for f in "$BASS"/*lenok*; do
	[ -e "$f" ] && mv "$f" "${f//lenok/bass}"
done
find "$BASS" -type f -exec sed -i \
	-e 's/lenok/bass/g' \
	-e 's/LG G Watch R/LG Watch Urbane/g' \
	-e 's/firmware-lg-bass-/firmware-lg-lenok-/g' {} +
if [ -n "$OFFSETS" ]; then
	$PATCH device "$BASS" --offsets "$OFFSETS"
else
	$PATCH device "$BASS"
fi

# ---------------------------------------------------------------- kernel
echo "==> Patching kernel package $KERNEL"
$PATCH kernel "$KERNEL" "$KIT/port/qcom-apq8026-lg-bass.dts"

# ---------------------------------------------------------------- customisation package
echo "==> Installing urbane-mini-linux package into pmaports"
rm -rf "$APORTS/main/urbane-mini-linux"
cp -r "$KIT/port/urbane-mini-linux" "$APORTS/main/urbane-mini-linux"

echo "==> Updating checksums"
# kernel sums were refreshed locally by pmaports_patch.py (no 200 MB download)
pmbootstrap checksum device-lg-bass urbane-mini-linux

# ---------------------------------------------------------------- pmbootstrap config
echo "==> Configuring pmbootstrap for lg-bass"
pmbootstrap config device lg-bass
pmbootstrap config ui none
EXTRA=$(pmbootstrap config extra_packages 2>/dev/null | tail -n 1 || true)
case ",$EXTRA," in
	*,urbane-mini-linux,*) ;;
	,none,|,,) pmbootstrap config extra_packages urbane-mini-linux ;;
	*) pmbootstrap config extra_packages "$EXTRA,urbane-mini-linux" ;;
esac

cat <<EOF

==> Done. Review the generated deviceinfo if you like:
      $BASS/deviceinfo
    Then build:   bash build/03-build-system.sh
EOF
