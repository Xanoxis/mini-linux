<#
  fastboot.ps1 (tool) - fastboot actions for Mini Linux on the LG Watch Urbane.
  The numbered steps 04, 05, 07 call it; use it directly for the other modes.

  Run from the kit folder in PowerShell:
    powershell -ExecutionPolicy Bypass -File flash\tools\fastboot.ps1 -Mode check
    powershell -ExecutionPolicy Bypass -File flash\tools\fastboot.ps1 -Mode test
    powershell -ExecutionPolicy Bypass -File flash\tools\fastboot.ps1 -Mode kernel -Boot out\boot-keep-rootfs.img

  Modes
    check     show the watch as fastboot sees it (nothing is changed)
    test      boot a kernel ONCE from RAM (fastboot boot). Nothing is written;
              the next reboot returns to the flashed kernel.
    tethered  WRITES rootfs -> userdata (wipes Wear OS user data), then boots
              the kernel from RAM. The boot partition keeps the Wear OS kernel,
              so a plain reboot still returns to Wear OS.
    install   WRITES rootfs -> userdata and kernel -> boot (permanent)
    kernel    WRITES only the boot partition (after rebuilding the kernel)

  test/kernel use out\boot-keep-rootfs.img when it exists (new kernel + the rootfs
  UUIDs of the system already on the watch), else out\boot.img. -Boot overrides.

  Needs Android platform-tools (fastboot.exe) in PATH or -Fastboot C:\path\fastboot.exe
#>
param(
    [ValidateSet('check', 'test', 'tethered', 'install', 'kernel')]
    [string]$Mode = 'check',
    [string]$Fastboot = 'fastboot',
    # boot image to use instead of the default (see above)
    [string]$Boot = '',
    [string]$OutDir = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'out')
)
$ErrorActionPreference = 'Stop'

function Invoke-Fastboot {
    Write-Host "> fastboot $($args -join ' ')" -ForegroundColor DarkGray
    & $Fastboot @args
    if ($LASTEXITCODE -ne 0) { throw "fastboot $($args -join ' ') failed (exit $LASTEXITCODE)" }
}

if (-not (Get-Command $Fastboot -ErrorAction SilentlyContinue)) {
    throw "fastboot not found. Install Android platform-tools and add it to PATH, or pass -Fastboot <path>."
}


# --- Urbane ramdisk guard -------------------------------------------------------------
# LG's bootloader always places the ramdisk at 0x02900000; protected firmware memory starts
# at 0x02F00000. A bigger ramdisk crashes the watch into Qualcomm crash-dump mode (and was the
# likely cause of the GPT damage). Refuse such images.
function Assert-RamdiskFits([string]$imgPath) {
    $fs = [System.IO.File]::OpenRead($imgPath)
    try { $h = New-Object byte[] 48; [void]$fs.Read($h, 0, 48) } finally { $fs.Close() }
    if ([Text.Encoding]::ASCII.GetString($h, 0, 8) -ne 'ANDROID!') { return }
    $rd = [BitConverter]::ToUInt32($h, 16)
    $limit = 6MB - 64KB
    if ($rd -gt $limit) {
        throw ("{0}: ramdisk is {1:N1} MiB - over the Urbane limit of {2:N1} MiB. Refusing to boot it." -f (Split-Path -Leaf $imgPath), ($rd / 1MB), ($limit / 1MB))
    }
    Write-Host ("{0}: ramdisk {1:N1} MiB (limit {2:N1} MiB) OK" -f (Split-Path -Leaf $imgPath), ($rd / 1MB), ($limit / 1MB))
}
# ---------------------------------------------------------------------------------------
$keep = Join-Path $OutDir 'boot-keep-rootfs.img'
$boot = if ($Boot) { (Resolve-Path $Boot).Path }
        elseif (($Mode -in @('test', 'kernel')) -and (Test-Path $keep)) { $keep }
        else { Join-Path $OutDir 'boot.img' }
if ($Mode -ne 'check') { Write-Host "Boot image: $boot" }
$flashed = Join-Path $OutDir 'boot-flashed.img'   # copy of what is in the boot partition
if ((Test-Path $boot) -and $Mode -ne 'check') { Assert-RamdiskFits $boot }
$root = Join-Path $OutDir 'lg-bass.img'

$devices = (& $Fastboot devices) | Out-String
if (-not $devices.Trim()) {
    throw "No device in fastboot mode. Put the watch on the cradle and enter fastboot (swipe diagonally top-left -> bottom-right during the first seconds of boot)."
}
Write-Host "Fastboot device(s):`n$devices"

# Partition limits measured on a W150 (fastboot getvar partition-size:*)
# Decimal on purpose: PowerShell reads 0xC2400000 as a negative 32-bit number.
$limits = @{ $boot = [long]23068672; $root = [long]3258974208 }   # boot 22 MiB, userdata ~3.04 GiB
$needed = switch ($Mode) {
    'test'   { @($boot) }
    'kernel' { @($boot) }
    'check'  { @() }
    default  { @($boot, $root) }
}
if ($needed.Count -gt 0) {
    foreach ($f in $needed) {
        if (Test-Path $f) {
            $size = (Get-Item $f).Length
            $max = $limits[$f]
            Write-Host ("{0}: {1:N1} MiB of {2:N1} MiB" -f (Split-Path -Leaf $f), ($size / 1MB), ($max / 1MB))
            if ($size -gt $max) { throw "$f is larger than its partition - the initramfs is too big - see the guide, 'boot.img too large'." }
        }
    }
}

switch ($Mode) {
    'check' {
        # fastboot prints getvar results on stderr
        $ErrorActionPreference = 'Continue'
        $vars = 'product', 'unlocked', 'max-download-size', 'partition-size:boot', 'partition-size:userdata', 'partition-size:system'
        foreach ($v in $vars) {
            & $Fastboot getvar $v 2>&1 | ForEach-Object { "$_" } | Where-Object { $_ -match '^\S+:' } | Select-Object -First 1
        }
    }
    'test' {
        if (-not (Test-Path $boot)) { throw "$boot missing - run build/03-build-system.sh in WSL first" }
        Invoke-Fastboot boot $boot
        Write-Host "Booting from RAM. Nothing was written; a restart goes back to the installed system." -ForegroundColor Green
    }
    'tethered' {
        foreach ($f in $boot, $root) { if (-not (Test-Path $f)) { throw "$f missing - run the build first" } }
        Write-Host "This ERASES Wear OS user data (the Wear OS kernel stays in place)." -ForegroundColor Yellow
        if ((Read-Host "Type YES to continue") -ne 'YES') { Write-Host 'Aborted.'; exit 1 }
        Invoke-Fastboot -S 100M flash userdata $root
        Invoke-Fastboot boot $boot
        Write-Host "Mini Linux starts from RAM (first start: a few minutes). Next: step 06 (backup), then step 07." -ForegroundColor Green
    }
    'install' {
        foreach ($f in $boot, $root) { if (-not (Test-Path $f)) { throw "$f missing - run the build first" } }
        Write-Host "This ERASES Wear OS user data and replaces the Wear OS kernel." -ForegroundColor Yellow
        Write-Host "The Android 'system' partition is not touched (WiFi calibration stays readable)." -ForegroundColor Yellow
        if ((Read-Host "Type YES to continue") -ne 'YES') { Write-Host 'Aborted.'; exit 1 }
        # -S: send the rootfs in sparse chunks small enough for the old LG bootloader
        Invoke-Fastboot -S 100M flash userdata $root
        Invoke-Fastboot flash boot $boot
        Copy-Item $boot $flashed -Force
        Invoke-Fastboot reboot
        Write-Host "Done. First boot resizes the filesystem and can take a few minutes." -ForegroundColor Green
    }
    'kernel' {
        if (-not (Test-Path $boot)) { throw "$boot missing" }
        Invoke-Fastboot flash boot $boot
        Copy-Item $boot $flashed -Force
        Invoke-Fastboot reboot
    }
}
