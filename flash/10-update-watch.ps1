<#
  10-update-watch.ps1 - one-step PERMANENT update of the running watch (no RAM boot).

    powershell -ExecutionPolicy Bypass -File flash\10-update-watch.ps1
    ... -NoKernel        only the urbane-mini-linux package (no fastboot at all)
    ... -WatchHost 192.168.1.50   (over WiFi; the fastboot part still needs the cable)

  1. copies out\apk\*.apk and out\kernel\*.apk to the watch and installs them (sudo once)
  2. reboots the watch into fastboot, flashes out\boot-keep-rootfs.img, reboots
  3. waits for SSH and shows the running kernel and package version
  WiFi, pairings, settings (watch.local.conf) and files stay.
#>
param(
    [string]$WatchHost = '172.16.42.1',
    [string]$User = '',
    [string]$Fastboot = 'fastboot',
    [switch]$NoKernel,
    [string]$OutDir = (Join-Path (Split-Path -Parent $PSScriptRoot) 'out')
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'tools\common.ps1')
if (-not $User) { $User = Get-WatchUser (Split-Path -Parent $OutDir) }
$target = "$User@$WatchHost"

$apks = @(Get-ChildItem (Join-Path $OutDir 'apk') -Filter *.apk -ErrorAction SilentlyContinue)
if (-not $NoKernel) {
    $kapk = Get-ChildItem (Join-Path $OutDir 'kernel') -Filter *.apk -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    $boot = Join-Path $OutDir 'boot-keep-rootfs.img'
    if (-not $kapk) { throw "No out\kernel\*.apk - run build/11-build-kernel.sh in WSL first (or use -NoKernel)." }
    if (-not (Test-Path $boot)) { throw "No out\boot-keep-rootfs.img - run build/11-build-kernel.sh in WSL first (or use -NoKernel)." }
    if (-not (Get-Command $Fastboot -ErrorAction SilentlyContinue)) { throw "fastboot not found (PATH or -Fastboot C:\path\fastboot.exe)." }
    # same ramdisk guard as tools\fastboot.ps1: LK puts it at 0x02900000, protected memory at 0x02F00000
    $fs = [IO.File]::OpenRead($boot); try { $h = New-Object byte[] 48; [void]$fs.Read($h, 0, 48) } finally { $fs.Close() }
    if ([BitConverter]::ToUInt32($h, 16) -gt (6MB - 64KB)) { throw "boot-keep-rootfs.img ramdisk is over the Urbane limit - refusing." }
    $apks += $kapk
}
if ($apks.Count -eq 0) { throw "Nothing to install - run build/09-build-update.sh in WSL first." }
Write-Host "Packages:" ($apks.Name -join ', ')

# ---- 1. packages
& ssh $target 'rm -rf /tmp/kit-apks && mkdir -p /tmp/kit-apks'
if ($LASTEXITCODE -ne 0) { throw "Cannot reach $target over SSH." }
& scp @($apks.FullName) "${target}:/tmp/kit-apks/"
if ($LASTEXITCODE -ne 0) { throw "scp failed." }
# (0.21: Minecraft servers are no longer part of the system - removed if still installed)
& ssh -t $target 'cd /tmp/kit-apks && for p in cuberite bareiron; do apk info -e $p >/dev/null 2>&1 && sudo apk del -q $p; done; sudo apk add --allow-untrusted ./*.apk; r=$?; cd / && rm -rf /tmp/kit-apks; exit $r'
if ($LASTEXITCODE -ne 0) { throw "apk install failed - see above. Nothing was flashed." }

if ($NoKernel) {
    Write-Host "Package installed (live; services restarted). On the watch: Menu -> Power -> Restart screen (or: ssh -t $target sudo reboot)" -ForegroundColor Green
    return
}

# ---- 2. kernel: fastboot flash of the boot partition
Write-Host "==> rebooting the watch into fastboot"
& ssh -t $target 'sudo /usr/bin/watch-power fastboot' 2>$null
$deadline = (Get-Date).AddSeconds(90)
while (-not ((& $Fastboot devices) | Out-String).Trim()) {
    if ((Get-Date) -gt $deadline) {
        throw "No fastboot device after 90 s. Enter it by hand (Menu -> Power -> Fastboot, or the diagonal swipe at boot), then run: flash\tools\fastboot.ps1 -Mode kernel"
    }
    Start-Sleep 2
}
Write-Host "> fastboot flash boot $boot" -ForegroundColor DarkGray
& $Fastboot flash boot $boot
if ($LASTEXITCODE -ne 0) { throw "fastboot flash failed - the old boot partition is unchanged; 'fastboot reboot' to go back." }
Copy-Item $boot (Join-Path $OutDir 'boot-flashed.img') -Force
& $Fastboot reboot | Out-Null

# ---- 3. wait for the system
Write-Host "==> waiting for the watch (up to 3 min)"
$deadline = (Get-Date).AddMinutes(3)
do {
    Start-Sleep 5
    $info = & ssh -o ConnectTimeout=4 -o BatchMode=yes $target 'uname -v; apk info -v 2>/dev/null | grep -E "^(urbane-mini-linux|linux-postmarketos)"' 2>$null
} while ($LASTEXITCODE -ne 0 -and (Get-Date) -lt $deadline)
if ($LASTEXITCODE -ne 0) { Write-Host "No SSH yet - check the watch screen (key login may need a password: ssh $target)." -ForegroundColor Yellow; return }
$info | ForEach-Object { Write-Host "    $_" }
Write-Host "Done." -ForegroundColor Green
