#!/bin/sh
# Hardware checks for one Nano ESP32 running this firmware, through its USB
# serial port only. Uploads the self-test build, runs it under three host loads
# and the protocol checks, then puts the normal firmware back and checks the
# pad's own setup is unchanged (the self-test uses its own NVS namespace).
#
# usage: test/hil/run.sh SERIAL   (the board's USB serial number)
# Needs: make, make SELFTEST=1, client/target/release/macropad-config, pyserial.
# The self-test presses keys from firmware: it sends only F13-F24 and mouse
# button 8. Run it with the pad otherwise idle; uploads go through config mode.
set -u
SN=${1:?usage: run.sh SERIAL}
H=$(cd "$(dirname "$0")" && pwd)
cd "$H/../.." || exit 1
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
CLIENT=./client/target/release/macropad-config

dev() { readlink -f /dev/serial/by-id/*"$SN"* 2>/dev/null; }
up() {
	timeout 20 python3 -I "$H/quiet.py" "$SN" > /dev/null 2>&1  # config mode: idle during the upload
	arduino-cli upload --input-dir "$1" -p "$(dev)" . > "$T/upload.log" 2>&1 || { echo "upload failed:"; tail -3 "$T/upload.log"; exit 1; }
	for _ in $(seq 1 80); do [ -n "$(dev)" ] && break; sleep 0.25; done
}

[ -n "$(dev)" ] || { echo "no serial port for $SN"; exit 1; }
timeout 30 $CLIENT pull --port "$(dev)" "$T/before.toml" > /dev/null 2>&1 || echo "note: no setup to compare afterwards"

up .build/selftest; sleep 1
for mode in idle flood noread; do
	timeout 60 python3 -I -u "$H/fresh.py" "$SN" reset; sleep 1  # the fixture, nothing stored: the self-test runs
	timeout 300 python3 -I -u "$H/st.py" "$SN" $mode "$T/st-$mode.log" 100
	echo "self-test, $mode: $(grep -hE 'FAIL|latency_us|max_gap|cleanup|checks,' "$T/st-$mode.log" | tr '\n' ' ')"
done
timeout 60 python3 -I -u "$H/fresh.py" "$SN" reset; sleep 100
timeout 60 python3 -I -u "$H/rerun.py" "$SN" first
timeout 90 python3 -I -u "$H/rerun.py" "$SN" rerun
timeout 30 python3 -I -u "$H/commit.py" "$SN" > /dev/null 2>&1; sleep 3
timeout 500 python3 -I -u "$H/proto_hw.py" "$SN" "$T/proto.log" > /dev/null 2>&1
grep -E "PASS|FAIL|checks," "$T/proto.log"
timeout 90 python3 -I -u "$H/fresh.py" "$SN" erase
if [ -f "$T/before.toml" ]; then
	timeout 60 $CLIENT push --port "$(dev)" "$T/before.toml" 2>&1 | tail -1 | sed 's/^/push to a fresh pad: /'
fi

up .build/firmware; sleep 2
if [ -f "$T/before.toml" ]; then
	timeout 30 $CLIENT pull --port "$(dev)" "$T/after.toml" > /dev/null 2>&1 &&
		cmp -s "$T/before.toml" "$T/after.toml" && echo "normal firmware back, the pad's setup unchanged" ||
		echo "FAIL the pad's setup changed or can't be read"
fi
