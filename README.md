<p align="center"><img width="934" height="315" alt="IMG_2216" src="https://github.com/user-attachments/assets/313a0c7c-101c-41a5-86ed-bbb31d709f2a" /></p>

A compact Linux OS for LG W150 Urbane. Small terminal UI, menu, bluetooth keyboard setup, and software keys, in case you have no keyboard.
You could potentially even run a Minecraft server on it! I did, as a test.

The OS is highly customised for my specific watch, as it has battery voltage issues and sits on charge cradle 100.01% of the time. What crashes for me, might not crash for you, feel free to customise the cpu kernel.

Current version: **0.21.4**

## Screenshots


<img width="320" height="320" alt="main" src="https://github.com/user-attachments/assets/60a4a795-64cc-4432-a1c7-e7b438eb57b8" />
<img width="320" height="320" alt="menu" src="https://github.com/user-attachments/assets/694f33d5-b774-4296-9e8d-305bd8c3b78c" />
<img width="320" height="320" alt="keyboard" src="https://github.com/user-attachments/assets/5666fa5d-0c02-41fc-84b9-e2670b923964" />


## 0. What you need

| | |
|---|---|
| Watch | LG Watch Urbane W150 (bass), bootloader **unlocked**, connected to USB via charging cradle. |
| PC | Windows 10/11 with **WSL2** (Ubuntu), about 15 GB free disk, internet. |
| Windows tools | [Android platform-tools](https://developer.android.com/tools/releases/platform-tools) (`fastboot.exe` in PATH) |
| Optional | A Bluetooth keyboard.

**Fastboot mode:** put the watch on the cradle, power it on, and in the first seconds swipe diagonally from the top-left edge to the bottom-right.
**Unlocking:** in fastboot, `fastboot oem unlock`. This wipes the watch.


## 1. Install

`.sh` steps run in WSL, `.ps1` steps in PowerShell. Both from the repo folder. Every script tells you what it does at the top, and what's next when it's done. Nothing is written to the watch before step 05.

| Step | Where | Command | What it does |
|---|---|---|---|
| 01 | WSL | `bash build/01-setup-wsl.sh`, then `pmbootstrap init` | Installs pmbootstrap. Prints what to answer in `pmbootstrap init`. |
| 02 | WSL | `bash build/02-add-watch-port.sh` | Adds the Urbane port and the Mini Linux package. |
| 03 | WSL | `bash build/03-build-system.sh` | Builds the system. First time takes 5-30 min. |
| 04 | PS | `flash\04-check-watch.ps1` | Only checks the watch in fastboot. |
| 05 | PS | `flash\05-install-system.ps1` | Writes the system and starts it from RAM. Wear OS data is wiped. |
| 06 | PS | `flash\06-backup-watch.ps1` | Backs up the original partitions (Wear OS kernel, bootloader, logo) to `backup\`. |
| 07 | PS | `flash\07-make-permanent.ps1` | Writes the Mini Linux kernel. Watch restarts into Mini Linux. |
| 08 | WSL | `bash build/08-boot-logo.sh` | Replaces the LG boot logo with the Mini Linux one. |

Don't skip step 06. It's the only way back to Wear OS later.
If you forget and lose your img, I can send you my Wear OS copy, for tree fiddy.

After step 07, the first start takes a few minutes. Then:

- SSH from the PC: `ssh <user>@172.16.42.1`
- WiFi: Menu → WiFi
- Keyboard: `watch kbd`


## 2. Updating

If you make the changes in the code, for the most part the updates keep WiFi, pairings, settings and files.
Mini Linux package can be updated in the OS, restart afterwards. The kernel should be updated through fastboot.

| To change | What to do |
|---|---|
| The Mini Linux package | `bash build/09-build-update.sh`, then `flash\10-update-watch.ps1 -NoKernel` |
| The kernel | `bash build/11-build-kernel.sh`, `bash build/09-build-update.sh`, then `flash\10-update-watch.ps1` |

## 3. Using it

**Side button**

| Press | Does |
|---|---|
| 1x | Screen on/off |
| 2x | Menu |
| 3x | Touch buttons back (gets you out of full screen apps, or opens Keys) |
| Long | Power menu |

**Screen**

- Top: time, battery, WiFi, BT, network speed, disk and uptime. Tap it for the menu.
- Left: free RAM, RAM/swap/disk bars. Right: temperature, clock and CPU cores.
- Bottom: ≡ menu, ◀ ▶ windows, **Keys**, ^C.
- **Keys** page: Esc, Tab, Pg↑, Pg↓, arrows, Enter and **abc**, a phone keypad for typing. Tap a key again within 1 s for the next letter.

**The `watch` command**

Everything watch-specific is in one command. `watch help` shows all of it, `watch help cpu` shows one part.

```sh
watch set                    # all settings
watch set cpu_cores 4        # change one
watch cpu                    # cores, clock, temperature
watch charge                 # battery and charger
watch rotate 90              # turn the screen (touch and boot logo too)
watch view font Terminus 12  # font
watch buttons idle 120       # screen off after 120 s
watch shot                   # screenshot to ~/shots
watch power reboot           # or: poweroff, fastboot
```
## 4. Going back to Wear OS

Put the watch in fastboot and run `flash\12-restore-stock.ps1`. It writes the Wear OS kernel and LG logo back from `backup\`, and wipes Mini Linux. Wear OS starts like a new watch.

Only the logo: `bash build/08-boot-logo.sh --restore`

## 5. What's in here

```
build/   WSL scripts: setup, port, build, boot logo, updates
         lib/   helpers (port patches, boot image, logo tool)
         tools/ kernel log, reports, kernel source download
flash/   PowerShell scripts: check, install, backup, permanent, update, restore
         tools/ fastboot modes, screenshot, push single scripts
port/    the Urbane device tree, and the Mini Linux package (urbane-mini-linux)
```

## 6. Known issues

- CPU: Stable for me at 4 cores, 300 MHz, powersave governor. Going faster crashes my watch, likely the battery issue. It currently boots up on 1 core and steps up after a minute. Change it with `watch set cpu_boot_ramp`, `cpu_cores`, `cpu_governor`.
- If you're surprised shutdown 'doesn't work' - if the watch is on the charger, it will just power up the watch to spite you.
- If you're stuck in a full screen app without a keyboard: press the side button 3 times.
- If you screw something up, and the watch crashes into LG/Qualcomm debug mode - Windows will spam you with explorer windows and asks to format a "Qualcomm" disk: click **Cancel**, hold the button 10-30 s to restart, go into fastboot.

## Credits

Made by **Xanoxis** - [art.lucario.ink](https://art.lucario.ink)

Built on:

- [Nura](https://nura.eco/) (formerly postmarketOS) and [Alpine Linux](https://alpinelinux.org/)
- Mainline Linux for msm8226, and Nura's LG G Watch R (lenok) port
- [msm8226-mainline/linux PR #22](https://github.com/msm8226-mainline/linux/pull/22), the Urbane device tree
- [AsteroidOS](https://release.asteroidos.org/nightlies/bass/), for the boot image layout
- [FairBlobs lenok-firmware](https://github.com/FairBlobs/lenok-firmware), WiFi/BT firmware
- cage, foot, tmux, NetworkManager, BlueZ, grim, wlr-randr
- [bitdomo/imgdata](https://github.com/bitdomo/imgdata/tree/hammerhead), for the boot logo format

## License

AGPL 3.0, except:

- `port/qcom-apq8026-lg-bass.dts` keeps its BSD-3-Clause license.
- The kernel patches in `build/lib/pmaports_patch.py` change GPL-2.0 code. Whoops!
- The Mini Linux logo (`port/urbane-mini-linux/logo.png`) is my work based on LG logo, all rights reserved. Lets hope they don't notice.
