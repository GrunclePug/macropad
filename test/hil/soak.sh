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
dev() { readlink -f /dev/serial/by-id/*"$SN"* 2>/dev/null; }
up() {
	timeout 20 python3 -I "$H/quiet.py" "$SN" > "$T/quiet.log" 2>&1 || { cat "$T/quiet.log"; fail "config mode before the upload"; return 1; }
	arduino-cli upload --input-dir "$1" -p "$(dev)" . > "$T/upload.log" 2>&1 || { tail -3 "$T/upload.log"; fail "upload $1"; return 1; }
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
	{ tail -5 "$T/build.log"; echo "FAIL build"; exit 1; }
[ -n "$(dev)" ] || { echo "FAIL no serial port for $SN"; exit 1; }
since=$(date '+%Y-%m-%d %H:%M:%S')
# The pad's own setup is read on the normal firmware (the self-test build has its own).
up .build/firmware || exit 1
timeout 30 $CLIENT pull --port "$(dev)" "$T/before.toml" > /dev/null 2>&1 || { echo "FAIL pull the pad's setup first"; exit 1; }

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
usb=$(journalctl -k --since "$since" --no-pager 2>/dev/null | grep -E 'error -71|device descriptor read|irq status|cannot reset|disabled by hub' || true)
[ -z "$usb" ] || { echo "$usb"; fail "USB errors in the kernel log"; }

[ "$fails" -eq 0 ] && { echo "ALL CAME BACK"; exit 0; }
echo "$fails FAILED"; exit 1
