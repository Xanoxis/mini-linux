<#
  Step 06 - back up the watch's own partitions (bootloader, LG data, Wear OS kernel).

    powershell -ExecutionPolicy Bypass -File flash\06-backup-watch.ps1

  Run it while Mini Linux runs from RAM after step 05: at that point the boot
  partition still holds the original Wear OS kernel, and nothing else of LG's has
  been touched. Every partition except "userdata" (where Mini Linux now lives) is
  copied into the kit's backup\ folder, with SHA-256 checksums, plus the partition
  table (first and last MiB of the storage). Reading only - nothing is written.

  About 1 GB; takes a few minutes over the USB cable. You are asked for the watch
  password once (sudo on the watch). Keep the backup\ folder somewhere safe: step 12
  (restore) and the boot logo step use it.
#>
param(
    [string]$WatchHost = '172.16.42.1',
    [string]$User = '',
    [string]$KitDir = (Split-Path -Parent $PSScriptRoot)
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'tools\common.ps1')
if (-not $User) { $User = Get-WatchUser $KitDir }
$target = "$User@$WatchHost"
$dest = Join-Path $KitDir 'backup'
New-Item -ItemType Directory -Force -Path $dest | Out-Null

& ssh -o ConnectTimeout=10 $target true
if ($LASTEXITCODE -ne 0) { throw "Cannot reach $target over SSH. Is Mini Linux running (step 05) and the cradle connected?" }

# On the watch: every eMMC partition by name, except userdata, into ~/watch-backup
$remote = @'
set -e
dir=$HOME/watch-backup; rm -rf "$dir"; mkdir -p "$dir"
disk=/dev/mmcblk0
[ -b $disk ] || { echo "no $disk"; exit 1; }
sudo -v
for u in /sys/class/block/mmcblk0p*/uevent; do
	name=$(sed -n 's/^PARTNAME=//p' "$u"); dev=/dev/$(sed -n 's/^DEVNAME=//p' "$u")
	[ -n "$name" ] || continue
	case "$name" in userdata|grow) echo "  skip $name"; continue ;; esac
	printf '  %-10s %s\n' "$name" "$dev"
	sudo dd if="$dev" of="$dir/$name.img" bs=1M status=none
done
sects=$(cat /sys/class/block/mmcblk0/size)
sudo dd if=$disk of="$dir/gpt-head.bin" bs=512 count=2048 status=none
sudo dd if=$disk of="$dir/gpt-tail.bin" bs=512 skip=$((sects - 2048)) count=2048 status=none
for p in /sys/class/block/mmcblk0p*; do
	echo "$(basename "$p") $(sed -n 's/^PARTNAME=//p' "$p/uevent") start=$(cat "$p/start") sectors=$(cat "$p/size")"
done >"$dir/partitions.txt"
sudo chown -R "$(id -u):$(id -g)" "$dir"
cd "$dir" && sha256sum *.img *.bin >SHA256SUMS
du -sh "$dir" | cut -f1
'@
Write-Host "==> Reading the partitions on the watch (watch password for sudo)"
# the script goes over as a file: Windows PowerShell mangles quotes in ssh arguments
$tmp = Join-Path $env:TEMP 'mini-linux-backup.sh'
[IO.File]::WriteAllText($tmp, ($remote -replace "`r", ''), (New-Object System.Text.UTF8Encoding $false))
& scp -q $tmp "${target}:mini-linux-backup.sh"
if ($LASTEXITCODE -ne 0) { throw "scp failed." }
& ssh -t $target 'sh ./mini-linux-backup.sh; r=$?; rm -f ./mini-linux-backup.sh; exit $r'
if ($LASTEXITCODE -ne 0) { throw "Backup on the watch failed - see the messages above." }

Write-Host "==> Copying to $dest"
& scp -r -q "${target}:watch-backup/*" $dest
if ($LASTEXITCODE -ne 0) { throw "scp failed." }

Write-Host "==> Checking the copies"
$bad = 0
foreach ($line in Get-Content (Join-Path $dest 'SHA256SUMS')) {
    $hash, $file = $line -split '\s+', 2
    $file = $file.TrimStart('*')
    $got = (Get-FileHash (Join-Path $dest $file) -Algorithm SHA256).Hash.ToLower()
    if ($got -ne $hash) { Write-Host "  MISMATCH $file" -ForegroundColor Red; $bad++ }
}
if ($bad) { throw "$bad file(s) did not copy correctly - run this step again." }
& ssh $target 'rm -rf ~/watch-backup'
Write-Host "Backup complete and verified: $dest" -ForegroundColor Green
Write-Host "Most important files: boot.img (Wear OS kernel), aboot.img (bootloader), imgdata.img (boot logo), gpt-*.bin."
Write-Host "Next: step 07 (make Mini Linux permanent)."
