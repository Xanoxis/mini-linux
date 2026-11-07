<#
  push-scripts.ps1 (tool, for development) - copy the kit's shell tools (watch-cpu, watch-menu, ...) straight to
  /usr/bin on the watch. Fast path for script changes: no package build, no reboot.
  (The C daemon, services and config files still come with build/09-build-update.sh.)

    powershell -ExecutionPolicy Bypass -File flash\tools\push-scripts.ps1
    powershell -ExecutionPolicy Bypass -File flash\tools\push-scripts.ps1 watch-cpu watch-menu
#>
[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(Position = 0, ValueFromRemainingArguments = $true)][string[]]$Names,
    [string]$WatchHost = '172.16.42.1',
    [string]$User = ''
)
$ErrorActionPreference = 'Stop'
$kit = (Split-Path -Parent (Split-Path -Parent $PSScriptRoot))
. (Join-Path $PSScriptRoot 'common.ps1')
if (-not $User) { $User = Get-WatchUser $kit }
$src = Join-Path $kit 'port\urbane-mini-linux'
$all = 'watch', 'bt-kbd', 'urbane-extract-nvram', 'watch-buttons', 'watch-charge', 'watch-cpu', 'watch-info',
       'watch-menu', 'watch-power', 'watch-rotate', 'watch-screen', 'watch-session', 'watch-set', 'watch-shot',
       'watch-status', 'watch-viewport', 'wide'
if (-not $Names) { $Names = $all }
$files = @(foreach ($n in $Names) {
    $f = Join-Path $src $n
    if (-not (Test-Path $f)) { throw "unknown script: $n" }
    $f
})
$target = "$User@$WatchHost"
& ssh $target 'rm -rf /tmp/kit-scripts && mkdir -p /tmp/kit-scripts'
if ($LASTEXITCODE -ne 0) { throw "Cannot reach $target over SSH." }
& scp @files "${target}:/tmp/kit-scripts/"
if ($LASTEXITCODE -ne 0) { throw "scp failed." }
# strip Windows line endings in case an editor added them, then install
& ssh -t $target 'cd /tmp/kit-scripts && sed -i "s/\r$//" * && sudo install -m755 * /usr/bin/ && ls && rm -rf /tmp/kit-scripts'
if ($LASTEXITCODE -ne 0) { throw "install failed." }
Write-Host "Installed: $($Names -join ', ')" -ForegroundColor Green
