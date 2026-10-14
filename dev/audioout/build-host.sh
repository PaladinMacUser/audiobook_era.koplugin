#!/usr/bin/env bash
# Build audioout for the HOST (x86 Linux), for protocol testing with
# --sink fakesink (no device needed). Run the binary inside a nix shell so
# dlopen finds libgstreamer and the plugins:
#   nix shell nixpkgs#gst_all_1.gstreamer nixpkgs#gst_all_1.gst-plugins-base \
#     -c dev/audioout/out/audioout-host --sink fakesink ...
#
# Usage: bash dev/audioout/build-host.sh
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
mkdir -p "$HERE/out"

hdr() {
    nix build --no-link --print-out-paths --impure --expr \
        "(import <nixpkgs> {}).$1" | tail -1
}

GST_DEV=$(hdr gst_all_1.gstreamer.dev)
GLIB_DEV=$(hdr glib.dev)
GLIB_OUT=$(hdr glib.out)

nix shell nixpkgs#gcc -c bash -c "
    set -e
    GCCINC=\$(gcc -print-file-name=include)
    gcc -O2 -Wall -o '$HERE/out/audioout-host' '$REPO/audioout.c' \
        -I$GST_DEV/include/gstreamer-1.0 \
        -I$GLIB_DEV/include/glib-2.0 \
        -I$GLIB_OUT/lib/glib-2.0/include \
        -ldl -lpthread
"
echo "built: $HERE/out/audioout-host"
