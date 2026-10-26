#!/bin/sh
set -eu
ito_plugin_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ito_espeak_dir="$ito_plugin_dir/espeak-ng"
ito_espeak_bin="$ito_espeak_dir/bin/espeak-ng"
if [ ! -f "$ito_espeak_bin" ]; then ito_espeak_bin="$ito_espeak_bin.bin"; fi
exec "$ito_espeak_dir/lib/ld-linux-armhf.so.3" --library-path "$ito_espeak_dir/lib" "$ito_espeak_bin" --path="$ito_espeak_dir/share" -q --ipa=3 -v en-us -b 1 -f "$1"
