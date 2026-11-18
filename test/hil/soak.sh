#!/bin/sh
# Restart soak: how often a software restart fails to come back on USB.
# PUSHES saves of the pad's own setup on the normal firmware (each a COMMIT,
# a restart and the client's reboot check), then CYCLES restarts into recovery
# and back on the self-test build (soak.py), then the normal firmware again
# with the pad's setup checked unchanged (it starts by uploading the normal
# firmware to read that setup). Start it right after plugging the
# pad in, so no earlier self-test detach shares the power session.
# Exits 0 only if every restart came back.
#
# usage: test/hil/soak.sh SERIAL [PUSHES] [CYCLES]   (defaults 50 and 20)
set -u
SN=${1:?usage: soak.sh SERIAL [PUSHES] [CYCLES]}
PUSHES=${2:-50}
CYCLES=${3:-20}
H=$(cd "$(dirname "$0")" && pwd)
cd "$H/../.." || exit 1
fails=0
T=$(mktemp -d)
trap '[ "$fails" -eq 0 ] && rm -rf "$T" || echo "logs kept in $T"' EXIT
CLIENT=./client/target/release/macropad-config
fail() { echo "FAIL $*"; fails=$((fails + 1)); }
dev() {  # the pad's tty, or nothing (never an unmatched glob)
	for p in /dev/serial/by-id/*"$SN"*; do [ -e "$p" ] && { readlink -f "$p"; return; }; done
}
nup=0
up() {
	timeout 20 python3 -I "$H/quiet.py" "$SN" > "$T/quiet-$((nup + 1)).log" 2>&1 ||
		{ cat "$T/quiet-$((nup + 1)).log"; fail "config mode before the upload"; return 1; }
	nup=$((nup + 1)); ulog="$T/upload-$nup.log"
	arduino-cli upload --input-dir "$1" -p "$(dev)" . > "$ulog" 2>&1 || { tail -3 "$ulog"; fail "upload $1"; return 1; }
	for _ in $(seq 1 20); do [ -z "$(dev)" ] && break; sleep 0.25; done
	for _ in $(seq 1 80); do
		if [ -n "$(dev)" ]; then
			sleep 1
			stty -F "$(dev)" hupcl 2> /dev/null
			return 0
		fi
		sleep 0.25
	done
	fail "the pad didn't come back after the upload"; return 1
}

make > "$T/build.log" 2>&1 && make SELFTEST=1 >> "$T/build.log" 2>&1 && make -C client build >> "$T/build.log" 2>&1 ||
	{ tail -5 "$T/build.log"; fail build; exit 1; }
[ -n "$(dev)" ] || { fail "no serial port for $SN"; exit 1; }
usbpath=$(udevadm info -q path -n "$(dev)" 2>/dev/null | sed -n 's|.*/\([0-9-]*[0-9.]*\):[0-9.]*/tty/.*|\1|p')
since=$(date '+%Y-%m-%d %H:%M:%S')
# The pad's own setup is read on the normal firmware (the self-test build has its own).
up .build/firmware || exit 1
timeout 30 $CLIENT pull --port "$(dev)" "$T/before.toml" > /dev/null 2>&1 || { fail "pull the pad's setup first"; exit 1; }

ok=0
for k in $(seq 1 "$PUSHES"); do
	[ -n "$(dev)" ] || { fail "push $k: the pad is gone"; break; }
	if timeout 60 $CLIENT push --port "$(dev)" "$T/before.toml" > "$T/push.log" 2>&1; then
		ok=$((ok + 1))
	else
		tail -2 "$T/push.log"; fail "push $k"
		for _ in $(seq 1 60); do [ -n "$(dev)" ] && break; sleep 0.5; done
	fi
done
echo "pushes: $ok of $PUSHES saved and came back"

if up .build/selftest; then
	sleep 1
	timeout $((CYCLES * 40 + 30)) python3 -I -u "$H/soak.py" "$SN" "$CYCLES" > "$T/soak.log" 2>&1 || fail "recovery cycles"
	echo "recovery cycles: $(grep -c '^PASS' "$T/soak.log") of $CYCLES came back both times"
	grep '^FAIL' "$T/soak.log"
fi

if up .build/firmware; then
	pulled=false
	for _ in 1 2 3; do
		timeout 30 $CLIENT pull --port "$(dev)" "$T/after.toml" > /dev/null 2>&1 && { pulled=true; break; }
		sleep 2
	done
	$pulled && cmp -s "$T/before.toml" "$T/after.toml" && echo "PASS normal firmware back, the pad's setup unchanged" ||
		fail "the pad's setup after the soak differs or can't be read"
fi
if [ -n "$usbpath" ] && journalctl -k --since "$since" --no-pager > "$T/kernel.log" 2>&1; then
	usb=$(grep -E 'error -71|device descriptor read|irq status|cannot reset|disabled by hub' "$T/kernel.log" || true)
	[ -z "$usb" ] || { echo "$usb"; fail "USB errors in the kernel log"; }
	# An attach that never enumerated: the stuck-restart state (PLAN 9ze), even if it recovered.
	attached=$(grep -cF "usb $usbpath: new full-speed USB device number" "$T/kernel.log")
	found=$(grep -cF "usb $usbpath: New USB device found" "$T/kernel.log")
	echo "USB attaches on $usbpath: $attached, enumerated: $found"
	[ "$attached" -gt 0 ] || fail "no USB attach on $usbpath in the kernel log: the check saw nothing"
	[ "$attached" -eq "$found" ] || fail "$((attached - found)) USB attaches never enumerated"
else
	fail "can't read the kernel log or the pad's USB path: USB errors unchecked"
fi

[ "$fails" -eq 0 ] && { echo "ALL CAME BACK"; exit 0; }
echo "$fails FAILED"; exit 1
