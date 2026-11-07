#!/usr/bin/env bash
# Put the msm8226 kernel source tarball into pmbootstrap's download cache, so
# the build never has to fetch it from GitHub (which may answer 429 Too Many Requests).
#
#   bash build/tools/fetch-kernel-source.sh                 download with retries
#   bash build/tools/fetch-kernel-source.sh FILE.tar.gz     use a file you downloaded in a browser,
#        e.g. /mnt/c/Users/<you>/Downloads/linux-6.16.12-msm8226-u2.tar.gz
#
# The file is checked against the sha512 in the kernel APKBUILD before it is used.
set -euo pipefail

APORTS=$(pmbootstrap config aports | tail -n 1)
APK="$APORTS/device/testing/linux-postmarketos-qcom-msm8226/APKBUILD"
[ -f "$APK" ] || APK=$(find "$APORTS/device" -path '*/linux-postmarketos-qcom-msm8226/APKBUILD' | head -n 1)
WORK=$(pmbootstrap config work | tail -n 1)
DIST="$WORK/cache_distfiles"

PKGVER=$(sed -n 's/^pkgver=//p' "$APK")
SUFFIX=$(sed -n 's/^_tag="v\${pkgver\/_\/-}\(.*\)"$/\1/p' "$APK")
TAG="v${PKGVER/_/-}${SUFFIX}"
NAME="linux-$TAG.tar.gz"
WANT=$(awk -v n="$NAME" '$2 == n {print $1}' "$APK")
[ -n "$WANT" ] || { echo "could not find the checksum for $NAME in $APK" >&2; exit 1; }

echo "Kernel source: $NAME"
TMP="$HOME/$NAME.part"

if [ $# -ge 1 ]; then
	cp -v "$1" "$TMP"
else
	URLS="https://github.com/msm8226-mainline/linux/archive/refs/tags/$TAG.tar.gz
https://codeload.github.com/msm8226-mainline/linux/tar.gz/refs/tags/$TAG"
	ok=0
	for attempt in 1 2 3 4 5; do
		for u in $URLS; do
			echo "Attempt $attempt: $u"
			if curl -fL --retry 2 -A "Mozilla/5.0 (Windows NT 10.0; Win64; x64)" -o "$TMP" "$u"; then
				ok=1
				break 2
			fi
		done
		wait=$((attempt * 120))
		echo "GitHub refused (probably 429). Waiting ${wait}s..."
		sleep "$wait"
	done
	if [ "$ok" != 1 ]; then
		cat <<EOF

Still blocked. Download it in your Windows browser instead:
  https://github.com/msm8226-mainline/linux/archive/refs/tags/$TAG.tar.gz
then run (adjust the path):
  bash build/tools/fetch-kernel-source.sh /mnt/c/Users/<you>/Downloads/linux-${TAG#v}.tar.gz
EOF
		exit 1
	fi
fi

GOT=$(sha512sum "$TMP" | cut -d' ' -f1)
if [ "$GOT" != "$WANT" ]; then
	echo "Checksum mismatch - the file is incomplete or not the right tag." >&2
	rm -f "$TMP"
	exit 1
fi
sudo mkdir -p "$DIST"
sudo mv "$TMP" "$DIST/$NAME"
sudo chmod 664 "$DIST/$NAME"
echo "OK - cached as $DIST/$NAME"
echo "Now run: bash build/03-build-system.sh"
