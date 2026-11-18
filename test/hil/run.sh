#!/bin/sh
# Hardware checks for one Nano ESP32 running this firmware, through its USB
# serial port only. Builds both firmware images and the client, uploads the
# self-test build, runs it under three host loads, the protocol checks, a
# rerun and a fresh pad, then puts the normal firmware back and checks the
# pad's own setup is unchanged (the self-test uses its own NVS namespace).
# Exits 0 only if every check passed; prints FAIL lines otherwise.
#
# usage: test/hil/run.sh SERIAL   (the board's USB serial number)
# Needs: arduino-cli, cargo, pyserial. A failed run keeps its logs (one file
# per step) and says where. The self-test presses keys from firmware: it sends
# only F13-F24 and mouse button 8. Run it with the pad otherwise idle; uploads
# go through config mode.
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
fail() { echo "FAIL $*"; fails=$((fails + 1)); }
# Every line of a step's output is shown; each FAIL counts, and so does a step
# that printed no PASS when it should have.
nstep=0
step() {  # step NAME PASSES CMD...: CMD must exit 0 and print at least PASSES PASS lines, no FAIL
	name=$1 want=$2; shift 2
	nstep=$((nstep + 1)); out="$T/step-$nstep.log"
	"$@" > "$out" 2>&1; rc=$?
	cat "$out"
	got=$(grep -c '^PASS\|PASS ' "$out")
	if grep -q 'FAIL' "$out"; then fail "$name"
	elif [ "$rc" -ne 0 ]; then fail "$name: exit $rc"
	elif [ "$got" -lt "$want" ]; then fail "$name: $got of $want checks passed"; fi
}

dev() {  # the pad's tty, or nothing (never an unmatched glob)
	for p in /dev/serial/by-id/*"$SN"*; do [ -e "$p" ] && { readlink -f "$p"; return; }; done
}
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
	{ tail -5 "$T/build.log"; fail build; exit 1; }
[ -n "$(dev)" ] || { fail "no serial port for $SN"; exit 1; }
# The USB path of the pad's port, for the kernel log check at the end.
usbpath=$(udevadm info -q path -n "$(dev)" 2>/dev/null | sed -n 's|.*/\([0-9-]*[0-9.]*\):[0-9.]*/tty/.*|\1|p')
# The pad's own setup is read on the normal firmware (the self-test build has its own).
up .build/firmware || exit 1
timeout 30 $CLIENT pull --port "$(dev)" "$T/before.toml" > /dev/null 2>&1 || fail "pull the pad's setup before the run"
since=$(date '+%Y-%m-%d %H:%M:%S')

if up .build/selftest; then
	sleep 1
	for mode in idle flood noread; do
		# The fixture, nothing stored: the self-test runs.
		timeout 60 python3 -I -u "$H/fresh.py" "$SN" reset || fail "reset before the $mode self-test"
		sleep 1
		timeout 300 python3 -I -u "$H/st.py" "$SN" $mode "$T/st-$mode.log" 100 || fail "self-test, $mode: no report (exit $?)"
		summary=$(grep -hE 'FAIL|latency_us|max_gap|cleanup|checks,' "$T/st-$mode.log" | tr '\n' ' ')
		echo "self-test, $mode: $summary"
		grep -q "=== $CHECKS checks, 0 failed ===" "$T/st-$mode.log" || fail "self-test, $mode: not $CHECKS of $CHECKS"
	done
	timeout 60 python3 -I -u "$H/fresh.py" "$SN" reset || fail "reset before the rerun"
	sleep 100
	step "rerun: first report" 2 timeout 60 python3 -I -u "$H/rerun.py" "$SN" first "$T/first" "$CHECKS"
	step "rerun" 1 timeout 90 python3 -I -u "$H/rerun.py" "$SN" rerun "$T/first" "$CHECKS"
	timeout 30 python3 -I -u "$H/commit.py" "$SN" > "$T/commit.log" 2>&1 || { cat "$T/commit.log"; fail "commit before the protocol checks"; }
	sleep 3
	step "protocol checks" 10 timeout 500 python3 -I -u "$H/proto_hw.py" "$SN" "$T/proto.log"
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
	journalctl -k --since "$since" --no-pager > "$T/kernel.log"
	usb=$(grep -E 'error -71|device descriptor read|irq status|cannot reset|disabled by hub' "$T/kernel.log" || true)
	[ -z "$usb" ] || { echo "$usb"; fail "USB errors in the kernel log"; }
	# An attach that never enumerated logs "new ... device" without "New USB
	# device found": the stuck-restart state (PLAN 9ze), even if it recovered.
	if [ -n "$usbpath" ]; then
		attached=$(grep -c "usb $usbpath: new .*USB device number" "$T/kernel.log")
		found=$(grep -c "usb $usbpath: New USB device found" "$T/kernel.log")
		echo "USB attaches on $usbpath: $attached, enumerated: $found"
		[ "$attached" -eq "$found" ] || fail "$((attached - found)) USB attaches never enumerated"
	else
		fail "couldn't find the pad's USB path for the kernel log check"
	fi
else
	fail "can't read the kernel log (journalctl -k): USB errors unchecked"
fi

[ "$fails" -eq 0 ] && { echo "ALL PASSED"; exit 0; }
echo "$fails FAILED"; exit 1
