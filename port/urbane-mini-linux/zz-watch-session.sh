# /etc/profile.d/zz-watch-session.sh
# Start the round-screen terminal session when logging in on the watch screen (tty1).
# SSH logins and other TTYs get a normal shell.
if [ "$(tty 2>/dev/null)" = /dev/tty1 ] && [ -z "${WAYLAND_DISPLAY:-}" ] && [ -z "${SSH_CONNECTION:-}" ]; then
	AUTOSTART=1
	. /etc/urbane/watch.conf
	[ -f "$HOME/.config/urbane/watch.conf" ] && . "$HOME/.config/urbane/watch.conf"
	if [ "$AUTOSTART" = 1 ] && command -v watch-session >/dev/null; then
		watch-session --log || echo "watch-session failed. Retry with: watch-session"
	fi
fi
