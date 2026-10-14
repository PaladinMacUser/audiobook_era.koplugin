#!/usr/bin/env bash
# Run the M0 clock spike on the MTK test Kobo (192.168.1.142:2222).
# Usage: bash dev/audioout/spike-run.sh [--mtk-only]
#
# Plays a 30 s 440 Hz tone: through fakesink (mechanism control, inaudible),
# then through mtkbtmwrpcaudiosink (the real question; audible on whatever
# BT device is connected to the test unit). Logs land in
# dev/audioout/results/<timestamp>/.
set -uo pipefail

DEV_HOST=192.168.1.142
DEV_PORT=2222
REMOTE_DIR=/tmp/audioout-spike

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
TS=$(date +%Y%m%d-%H%M%S)
RESULTS="$HERE/results/$TS"
mkdir -p "$RESULTS"

BIN="$HERE/out/spike_clock"
if [ ! -x "$BIN" ]; then
    echo "spike_clock not found; run: bash dev/audioout/build-spike.sh" >&2
    exit 1
fi
echo "using $BIN"

echo "== push binary + tone =="
ssh -p "$DEV_PORT" "root@$DEV_HOST" "mkdir -p $REMOTE_DIR"
scp -P "$DEV_PORT" -q "$BIN" "root@$DEV_HOST:$REMOTE_DIR/spike_clock"
scp -P "$DEV_PORT" -q /tmp/audioout-spike/tone30.raw "root@$DEV_HOST:$REMOTE_DIR/"

MTK_ONLY=0
[ "${1:-}" = "--mtk-only" ] && MTK_ONLY=1

run_spike() {
    local label=$1; shift
    echo "== run: $label =="
    # Bare run: the binary is built against the device's own glibc 2.19 and
    # uses the device loader, so no bundled loader and no library path.
    timeout 180 ssh -p "$DEV_PORT" "root@$DEV_HOST" \
        "$REMOTE_DIR/spike_clock $*" \
        2>&1 | tee "$RESULTS/$label.log"
    echo
}

if [ "$MTK_ONLY" -ne 1 ]; then
    run_spike fakesink --pcm "$REMOTE_DIR/tone30.raw" --sink fakesink --max-ms 30000
    run_spike mtk_pause --pcm "$REMOTE_DIR/tone30.raw" --sink mtkbtmwrpcaudiosink \
        --pause-at 10000 --max-ms 60000
else
    run_spike mtk_pause --pcm "$REMOTE_DIR/tone30.raw" --sink mtkbtmwrpcaudiosink \
        --pause-at 10000 --max-ms 60000
fi

echo "results in $RESULTS"
