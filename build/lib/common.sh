# common.sh - shared helpers for the build/ scripts (sourced).

# the user name on the watch (the one chosen in "pmbootstrap init"); step 03 also
# writes it to out/watch-user.txt for the Windows scripts
watch_user() {
	local kit=$1 f="$1/out/watch-user.txt" u=""
	[ -s "$f" ] && u=$(head -n 1 "$f" | tr -d '\r ')
	[ -n "$u" ] || u=$(pmbootstrap config user 2>/dev/null | tail -n 1)
	[ -n "$u" ] || read -r -p "Your user name on the watch: " u
	echo "$u"
}
