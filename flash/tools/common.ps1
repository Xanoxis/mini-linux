# common.ps1 - shared helpers for the flash\ scripts (dot-sourced, not run directly).

# The user name on the watch: written to out\watch-user.txt by step 03 (build).
function Get-WatchUser([string]$KitDir) {
    $f = Join-Path $KitDir 'out\watch-user.txt'
    if (Test-Path $f) {
        $u = (Get-Content $f -TotalCount 1).Trim()
        if ($u) { return $u }
    }
    $u = Read-Host 'Your user name on the watch (the one you chose in "pmbootstrap init")'
    New-Item -ItemType Directory -Force -Path (Split-Path $f) | Out-Null
    Set-Content -Path $f -Value $u
    return $u
}

# Wait until fastboot sees the watch (or give up after $Seconds).
function Wait-Fastboot([string]$Fastboot = 'fastboot', [int]$Seconds = 90) {
    $deadline = (Get-Date).AddSeconds($Seconds)
    while (-not ((& $Fastboot devices) | Out-String).Trim()) {
        if ((Get-Date) -gt $deadline) { return $false }
        Start-Sleep 2
    }
    return $true
}
