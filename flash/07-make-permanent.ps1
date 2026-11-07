<#
  Step 07 - make Mini Linux permanent: write its kernel to the boot partition.

    powershell -ExecutionPolicy Bypass -File flash\07-make-permanent.ps1

  Do step 06 (backup) first - this replaces the Wear OS kernel. The running watch
  is sent into fastboot mode over SSH (or put it there by hand: swipe at power-on),
  out\boot.img is written, and the watch restarts into Mini Linux.
#>
param(
    [string]$WatchHost = '172.16.42.1',
    [string]$Fastboot = 'fastboot'
)
$ErrorActionPreference = 'Stop'
$kit = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'tools\common.ps1')
if (-not (Test-Path (Join-Path $kit 'backup\boot.img'))) {
    Write-Host "No backup\boot.img - the Wear OS kernel has not been backed up (step 06)." -ForegroundColor Yellow
    if ((Read-Host "Continue anyway? Type YES") -ne 'YES') { exit 1 }
}
if (-not ((& $Fastboot devices) | Out-String).Trim()) {
    $user = Get-WatchUser $kit
    Write-Host "==> asking the watch to restart into fastboot"
    & ssh -t "$user@$WatchHost" 'sudo /usr/bin/watch-power fastboot' 2>$null
    if (-not (Wait-Fastboot $Fastboot 90)) {
        throw "No fastboot device. Put the watch into fastboot by hand (swipe at power-on) and run this step again."
    }
}
& (Join-Path $PSScriptRoot 'tools\fastboot.ps1') -Mode kernel -Boot (Join-Path $kit 'out\boot.img') -Fastboot $Fastboot
Write-Host "Mini Linux is installed. Next: step 08 (boot logo, in WSL)." -ForegroundColor Green
