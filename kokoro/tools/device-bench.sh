#!/usr/bin/env bash
# Push the Kokoro-family runtime to the test Kobo and run the first device
# benchmark. The binary is static musl armv7hf (no loader, no libraries), so the
# device needs nothing but the blobs.
#
# Usage: kokoro/tools/device-bench.sh [host] [port]
#   host defaults to root@192.168.1.142, port to 2222 (empty-password SSH).
#
# Leaves the kit in /mnt/onboard/kokoro_test/ on the device and pulls the WAV
# back to dev/kokoro/device-s1.wav.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/../.." && pwd)
HOST=${1:-root@192.168.1.142}
PORT=${2:-2222}
SP=/tmp/sshpass-link/bin/sshpass
[ -x "$SP" ] || { echo "no sshpass at $SP" >&2; exit 1; }

BIN=$(nix-build "$REPO/kokoro/cross-build-koko-tts.nix" --no-out-link)/bin/kok_tts
T=/mnt/onboard/kokoro_test

echo "pushing the kit to $HOST:$T"
$SP ssh -p "$PORT" "$HOST" "mkdir -p $T"
$SP scp -P "$PORT" "$BIN" "$HOST:$T/kok_tts"
$SP scp -P "$PORT" "$REPO/kokoro/model/blobs/kok_meta.bin" \
    "$REPO/kokoro/model/blobs/kok_text.bin" \
    "$REPO/kokoro/model/blobs/kok_dec.bin" "$HOST:$T/"
$SP scp -P "$PORT" "$REPO/kokoro/test/fixtures/paradee-8m/s1_ids.bin" "$HOST:$T/"

echo "running (short sentence, 120 MB arena)"
$SP ssh -p "$PORT" "$HOST" "cd $T && chmod +x kok_tts && \
    time ./kok_tts --meta kok_meta.bin --text kok_text.bin --dec kok_dec.bin \
        --ids s1_ids.bin --out s1.wav --seed 1234 --arena-mb 120"

echo "pulling the audio back"
$SP scp -P "$PORT" "$HOST:$T/s1.wav" "$REPO/dev/kokoro/device-s1.wav"
echo "wrote $REPO/dev/kokoro/device-s1.wav"
