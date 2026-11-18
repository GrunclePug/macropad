#!/bin/sh
# Hardware checks for one Nano ESP32 running this firmware, through its USB
# serial port only. Builds both firmware images and the client, uploads the
# self-test build, runs it under three host loads, the protocol checks, a
# rerun and a fresh pad, then puts the normal firmware back and checks the
# pad's own setup is unchanged (the self-test uses its own NVS namespace).
# Exits 0 only if every check passed; prints FAIL lines otherwise.
#
# usage: test/hil/run.sh SERIAL   (the board's USB serial number)
# Needs: arduino-cli, cargo, pyserial. A failed run keeps its logs and says where. The self-test presses keys from
# firmware: it sends only F13-F24 and mouse button 8. Run it with the pad
# otherwise idle; uploads go through config mode.
set -u
SN=${1:?usage: run.sh SERIAL}
H=$(cd "$(dirname "$0")" && pwd)
cd "$H/../.." || exit 1
fails=0
CHECKS=45  # the self-test's check count
T=$(mktemp -d)
# Logs stay when anything failed: they're the evidence.
trap '[ "$fails" -eq 0 ] && rm -rf "$T" || echo "logs kept in $T"' EXIT
CLIENT=./client/target/release/macropad-config
fails=0
fail() { echo "FAIL $*"; fails=$((fails + 1)); }
# Every line of a step's output is shown; each FAIL counts, and so does a step
# that printed no PASS when it should have.
step() {  # step NAME PASSES CMD...: CMD must print at least PASSES lines starting with PASS
	name=$1 want=$2; shift 2
	"$@" > "$T/out" 2>&1; rc=$?
	cat "$T/out"
	got=$(grep -c '^PASS\|PASS ' "$T/out")
	if grep -q 'FAIL' "$T/out"; then fail "$name"
	elif [ "$rc" -ne 0 ]; then fail "$name: exit $rc"
	elif [ "$got" -lt "$want" ]; then fail "$name: $got of $want checks passed"; fi
}

dev() { readlink -f /dev/serial/by-id/*"$SN"* 2>/dev/null; }
up() {
	timeout 20 python3 -I "$H/quiet.py" "$SN" > "$T/quiet.log" 2>&1 || { cat "$T/quiet.log"; fail "config mode before the upload"; return 1; }
	arduino-cli upload --input-dir "$1" -p "$(dev)" . > "$T/upload.log" 2>&1 || { tail -3 "$T/upload.log"; fail "upload $1"; return 1; }
	# The old port may still be there: wait for it to go (up to 5 s), then come back.
	for _ in $(seq 1 20); do [ -z "$(dev)" ] && break; sleep 0.25; done
	for _ in $(seq 1 80); do
		if [ -n "$(dev)" ]; then
			sleep 1
			stty -F "$(dev)" hupcl 2> /dev/null  # quiet.py turned it off for the upload
			return 0
		fi
		sleep 0.25
	done
	fail "the pad didn't come back after the upload"; return 1
}

make > "$T/build.log" 2>&1 && make SELFTEST=1 >> "$T/build.log" 2>&1 && make -C client build >> "$T/build.log" 2>&1 ||
	{ tail -5 "$T/build.log"; echo "FAIL build"; exit 1; }
[ -n "$(dev)" ] || { echo "FAIL no serial port for $SN"; exit 1; }
timeout 30 $CLIENT pull --port "$(dev)" "$T/before.toml" > /dev/null 2>&1 || fail "pull the pad's setup before the run"
since=$(date '+%Y-%m-%d %H:%M:%S')

if up .build/selftest; then
	sleep 1
	for mode in idle flood noread; do
		# The fixture, nothing stored: the self-test runs.
		timeout 60 python3 -I -u "$H/fresh.py" "$SN" reset || fail "reset before the $mode self-test"
		sleep 1
		timeout 300 python3 -I -u "$H/st.py" "$SN" $mode "$T/st-$mode.log" 100 || fail "self-test, $mode: no report"
		summary=$(grep -hE 'FAIL|latency_us|max_gap|cleanup|checks,' "$T/st-$mode.log" | tr '\n' ' ')
		echo "self-test, $mode: $summary"
		grep -q "=== $CHECKS checks, 0 failed ===" "$T/st-$mode.log" || fail "self-test, $mode: not $CHECKS of $CHECKS"
	done
	timeout 60 python3 -I -u "$H/fresh.py" "$SN" reset || fail "reset before the rerun"
	sleep 100
	step "rerun: first report" 1 timeout 60 python3 -I -u "$H/rerun.py" "$SN" first "$T/first"
	step "rerun" 1 timeout 90 python3 -I -u "$H/rerun.py" "$SN" rerun "$T/first"
	timeout 30 python3 -I -u "$H/commit.py" "$SN" > "$T/commit.log" 2>&1 || fail "commit before the protocol checks"
	sleep 3
	timeout 500 python3 -I -u "$H/proto_hw.py" "$SN" "$T/proto.log" > /dev/null 2>&1
	step "protocol checks" 10 grep -E "PASS|FAIL|checks," "$T/proto.log"
	step "fresh pad" 2 timeout 90 python3 -I -u "$H/fresh.py" "$SN" erase
	if [ -f "$T/before.toml" ]; then
		if timeout 60 $CLIENT push --port "$(dev)" "$T/before.toml" > "$T/push.log" 2>&1; then
			echo "PASS push to a fresh pad: $(tail -1 "$T/push.log")"
		else
			tail -3 "$T/push.log"; fail "push to a fresh pad"
		fi
	fi
fi

# Always try to put the normal firmware back.
if up .build/firmware; then
	pulled=false
	for _ in 1 2 3; do
		timeout 30 $CLIENT pull --port "$(dev)" "$T/after.toml" > /dev/null 2>&1 && { pulled=true; break; }
		sleep 2
	done
	if [ -f "$T/before.toml" ] && $pulled && cmp -s "$T/before.toml" "$T/after.toml"; then
		echo "PASS normal firmware back, the pad's setup unchanged"
	else
		fail "the pad's setup after the run differs or can't be read"
	fi
fi

if journalctl -k -n 1 --no-pager > /dev/null 2>&1; then
	usb=$(journalctl -k --since "$since" --no-pager | grep -E 'error -71|device descriptor read|irq status|cannot reset|disabled by hub' || true)
	[ -z "$usb" ] || { echo "$usb"; fail "USB errors in the kernel log"; }
else
	echo "note: can't read the kernel log; USB errors not checked"
fi

[ "$fails" -eq 0 ] && { echo "ALL PASSED"; exit 0; }
echo "$fails FAILED"; exit 1
