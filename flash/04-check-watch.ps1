<#
  Step 04 - check that Windows sees the watch in fastboot mode. Nothing is written.

    powershell -ExecutionPolicy Bypass -File flash\04-check-watch.ps1

  Fastboot mode: power the watch on (on its cradle, USB connected to the PC) and,
  during the first seconds, swipe diagonally from the top-left edge to the bottom-right.
  The bootloader must be unlocked (see the guide; "fastboot oem unlock" wipes the watch).

  Expected on a W150: product bass, unlocked yes, boot 0x1600000 (22 MiB),
  userdata 0xc2400000 (~3 GiB).
#>
& (Join-Path $PSScriptRoot 'tools\fastboot.ps1') -Mode check @args
