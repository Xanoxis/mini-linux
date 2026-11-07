#!/usr/bin/env bash
# Step 01 - run inside WSL2 (Ubuntu/Debian). Installs pmbootstrap, the Nura
# (formerly postmarketOS) image builder, and an SSH key for logging in to the watch.
set -euo pipefail

echo "==> Installing host packages (sudo password may be asked)"
sudo apt-get update
sudo apt-get install -y git python3 openssl curl kpartx openssh-client

if ! command -v pmbootstrap >/dev/null 2>&1; then
	echo "==> Installing pmbootstrap from git"
	mkdir -p "$HOME/src" "$HOME/.local/bin"
	if [ ! -d "$HOME/src/pmbootstrap" ]; then
		git clone --depth 1 https://gitlab.postmarketos.org/postmarketOS/pmbootstrap.git \
			"$HOME/src/pmbootstrap"
	fi
	ln -sf "$HOME/src/pmbootstrap/pmbootstrap.py" "$HOME/.local/bin/pmbootstrap"
	case ":$PATH:" in
		*":$HOME/.local/bin:"*) ;;
		*) echo 'export PATH="$HOME/.local/bin:$PATH"' >>"$HOME/.bashrc"
		   export PATH="$HOME/.local/bin:$PATH" ;;
	esac
fi
pmbootstrap --version

if [ ! -f "$HOME/.ssh/id_ed25519.pub" ]; then
	echo "==> Creating an SSH key (pmbootstrap can copy it onto the watch)"
	ssh-keygen -t ed25519 -N "" -f "$HOME/.ssh/id_ed25519"
fi

cat <<'EOF'

==> Next: run "pmbootstrap init" and answer:

  Work path ............ (default)
  Channel .............. edge
  Vendor ............... lg
  Device codename ...... lenok      <- temporary; step 02 switches to lg-bass
  Username ............. your choice (e.g. xan)
  User interface ....... none
  Init system .......... openrc     (if asked)
  Provider ............. wpa_supplicant (WiFi backend; other providers: default)
  Additional options ... n
  Locale ............... en_US (or your own)
  Extra packages ....... none       (step 02 adds them)
  Timezone ............. yours (e.g. Europe/Warsaw)
  Hostname ............. urbane
  Copy SSH keys ........ y
  Build options / others: defaults

Then:  bash build/02-add-watch-port.sh
EOF
