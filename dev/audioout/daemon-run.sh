#!/usr/bin/env bash
# Deploy and protocol-test the audioout daemon on the MTK test Kobo
# (192.168.1.142:2222). Plays the 30 s tone through mtkbtmwrpcaudiosink:
# audible on whatever BT device is connected to the test unit.
#
# Usage: bash dev/audioout/daemon-run.sh [--build]
set -uo pipefail

DEV_HOST=192.168.1.142
DEV_PORT=2222
REMOTE_DIR=/tmp/audioout-spike

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
TS=$(date +%Y%m%d-%H%M%S)
RESULTS="$HERE/results/$TS"
mkdir -p "$RESULTS"

[ "${1:-}" = "--build" ] && bash "$HERE/build-daemon.sh"
BIN="$HERE/out/audioout"
[ -x "$BIN" ] || { echo "daemon missing; run with --build" >&2; exit 1; }

echo "== push daemon + wav + test =="
# A daemon left running holds its own binary (ETXTBSY): kill before push.
ssh -p "$DEV_PORT" "root@$DEV_HOST" "killall audioout 2>/dev/null; mkdir -p $REMOTE_DIR"
scp -P "$DEV_PORT" -q "$BIN" "root@$DEV_HOST:$REMOTE_DIR/audioout"
scp -P "$DEV_PORT" -q "$HERE/out/../out/spike_clock" "root@$DEV_HOST:$REMOTE_DIR/" 2>/dev/null || true
scp -P "$DEV_PORT" -q /tmp/audioout-spike/tone30.wav "root@$DEV_HOST:$REMOTE_DIR/"
scp -P "$DEV_PORT" -q "$HERE/daemon_test.lua" "root@$DEV_HOST:$REMOTE_DIR/"
# audioout.lua is two levels above dev/audioout in the repo layout; ship a
# copy beside the test so the dofile path matches.
cp "$REPO/audioout.lua" "$HERE/.audioout.lua.ship"
scp -P "$DEV_PORT" -q "$HERE/.audioout.lua.ship" "root@$DEV_HOST:$REMOTE_DIR/audioout.lua"
rm -f "$HERE/.audioout.lua.ship"

SINK_ARG="${SINK_ARG:-}"
echo "== start daemon (sink: ${SINK_ARG:-mtkbtmwrpcaudiosink}) =="
ssh -p "$DEV_PORT" "root@$DEV_HOST" "
    killall audioout 2>/dev/null
    rm -f /tmp/audiobook_audioout.sock /tmp/audiobook_audioout_fifo
    $REMOTE_DIR/audioout --sink "${SINK_ARG:-mtkbtmwrpcaudiosink}" > $REMOTE_DIR/daemon.log 2>&1 &
    sleep 2
    /mnt/onboard/.adds/koreader/luajit $REMOTE_DIR/daemon_test.lua /tmp/audiobook_audioout.sock $REMOTE_DIR/tone30.wav 30000
    rc=\$?
    killall audioout 2>/dev/null
    exit \$rc
" 2>&1 | grep -v askpass | tee "$RESULTS/daemon_test.log"
rc=${PIPESTATUS[0]:-1}

echo "== daemon log (tail) =="
ssh -p "$DEV_PORT" "root@$DEV_HOST" "tail -25 $REMOTE_DIR/daemon.log" 2>&1 | grep -v askpass | tee -a "$RESULTS/daemon.log"
echo "results in $RESULTS"
exit $rc
