<#
  screenshot.ps1 (tool) - take a screenshot of the watch screen and copy it to the PC.

    powershell -ExecutionPolicy Bypass -File flash\tools\screenshot.ps1
    ... -Out C:\path\shot.png     (default: kit folder\shots\shot-<time>.png)
#>
param(
    [string]$WatchHost = '172.16.42.1',
    [string]$User = '',
    [string]$Out = ''
)
$ErrorActionPreference = 'Stop'
$kit = (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
. (Join-Path $PSScriptRoot 'common.ps1')
if (-not $User) { $User = Get-WatchUser $kit }
$target = "$User@$WatchHost"
if (-not $Out) {
    $dir = Join-Path $kit 'shots'
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $Out = Join-Path $dir ("shot-{0}.png" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
& ssh $target 'watch-shot /tmp/watch-shot.png >/dev/null'
if ($LASTEXITCODE -ne 0) { throw "watch-shot failed on the watch (is grim installed? sudo apk add grim)" }
& scp "${target}:/tmp/watch-shot.png" $Out
if ($LASTEXITCODE -ne 0) { throw "scp failed." }
Write-Host "Saved $Out" -ForegroundColor Green
