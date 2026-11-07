<#
  Step 05 - install Mini Linux and start it once from RAM (the watch in fastboot mode).

    powershell -ExecutionPolicy Bypass -File flash\05-install-system.ps1

  WRITES the Mini Linux system to the "userdata" partition (this erases Wear OS user
  data) and then starts the Mini Linux kernel from RAM. The boot partition still holds
  the Wear OS kernel, so step 06 can back it up. A reboot now would go to Wear OS
  (with its data erased); step 07 makes Mini Linux permanent.

  The first start resizes the file system and takes a few minutes. When the round
  terminal shows up, SSH works too:  ssh <user>@172.16.42.1
#>
& (Join-Path $PSScriptRoot 'tools\fastboot.ps1') -Mode tethered -Boot (Join-Path (Split-Path -Parent $PSScriptRoot) 'out\boot.img') @args
