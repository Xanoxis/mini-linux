#!/usr/bin/env bash
# Step 11 - kernel update (only when the kernel or the device tree changed):
# re-applies the kit's kernel/DTS patches (step 02) and builds again (step 03).
# Result: out/boot-keep-rootfs.img (new kernel for the system already on the watch)
# and out/kernel/*.apk (its modules). Then: build/09-build-update.sh and, in Windows,
# flash\10-update-watch.ps1 (installs both and flashes the boot partition).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
bash "$HERE/02-add-watch-port.sh"
bash "$HERE/03-build-system.sh"
