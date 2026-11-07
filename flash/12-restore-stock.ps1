<#
  Step 12 - go back to Wear OS (the watch in fastboot mode).

    powershell -ExecutionPolicy Bypass -File flash\12-restore-stock.ps1

  Writes the Wear OS kernel and LG's boot pictures from your step-06 backup back
  (backup\boot.img, backup\imgdata.img) and erases userdata, so Wear OS starts like
  a new watch. Mini Linux and everything on it is gone afterwards. The Wear OS
  system partition was never changed, so nothing else is needed.
#>
param([string]$Fastboot = 'fastboot')
$ErrorActionPreference = 'Stop'
$b = Join-Path (Split-Path -Parent $PSScriptRoot) 'backup'
foreach ($f in 'boot.img', 'imgdata.img') {
    if (-not (Test-Path (Join-Path $b $f))) { throw "backup\$f missing - restore needs the step-06 backup." }
}
if (-not ((& $Fastboot devices) | Out-String).Trim()) { throw "No fastboot device. Swipe into fastboot at power-on first." }
Write-Host "This ERASES Mini Linux (userdata) and puts the Wear OS kernel back." -ForegroundColor Yellow
if ((Read-Host "Type YES to continue") -ne 'YES') { Write-Host 'Aborted.'; exit 1 }
& $Fastboot flash boot (Join-Path $b 'boot.img');       if ($LASTEXITCODE) { throw "flash boot failed" }
& $Fastboot flash imgdata (Join-Path $b 'imgdata.img'); if ($LASTEXITCODE) { Write-Host "imgdata not restored (logo stays) - not needed for Wear OS" -ForegroundColor Yellow }
& $Fastboot erase userdata;                             if ($LASTEXITCODE) { throw "erase userdata failed" }
& $Fastboot reboot
Write-Host "Done. Wear OS sets itself up as on a new watch (the first start takes a while)." -ForegroundColor Green
