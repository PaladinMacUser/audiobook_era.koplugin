#!/usr/bin/env bash
# Build the audioout M0 spike for Kobo armv7l against the Debian Jessie
# (glibc 2.19) sysroot, so it runs BARE under the device's own loader.
#
# Why not nix-build: the nixpkgs cross-cc wrapper appends its own
# --sysroot (nix glibc 2.42) AFTER the user's flag, so the Jessie libc.so
# linker script's /lib/arm-linux-gnueabihf/... paths stop resolving inside
# the sysroot and the link dies. The same outside-nix escape hatch the
# bluealsa compat build uses.
#
# Output: dev/audioout/out/spike_clock   (ELF armv7l, interp /lib/ld-linux-armhf.so.3,
#         max GLIBC symbol version 2.17; device libc is 2.19)
#
# Usage: bash dev/audioout/build-spike.sh
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
NIXPKGS_PIN="https://github.com/NixOS/nixpkgs/archive/255a186666b6130ddddf8ad749887102a0820914.tar.gz"

bash "$HERE/fetch-sysroot.sh"

hdr() {
    nix build --no-link --print-out-paths --impure --expr \
        "let p = builtins.fetchTarball \"$NIXPKGS_PIN\"; in (import p {}).pkgsCross.armv7l-hf-multiplatform.$1" \
        | tail -1
}

SYSROOT="$HERE/.sysroot"
OUT="$HERE/out"
mkdir -p "$OUT"

GST_DEV=$(hdr gst_all_1.gstreamer.dev)
GLIB_DEV=$(hdr glib.dev)
GLIB_OUT=$(hdr glib.out)
echo "headers: $GST_DEV / $GLIB_DEV"

# Flags (see PLAN.md "Architecture decisions"):
#   -nostdinc + explicit -I order   the gcc wrapper's sys/cdefs.h shadows
#                                   the Jessie libc's and breaks the
#                                   namespace macros
#   --sysroot + -B + -L             crt1.o and libc from the sysroot, NOT
#                                   the wrapper's glibc 2.42 (its
#                                   __libc_start_main@GLIBC_2.34 is absent
#                                   on the device's 2.19)
#   --dynamic-linker                the DEVICE's loader; the binary runs
#                                   bare, no bundled ld-linux
#   -static-libgcc                  no libgcc_s dependency
cd "$REPO"
nix shell "nixpkgs#pkgsCross.armv7l-hf-multiplatform.stdenv.cc" -c \
    armv7l-unknown-linux-gnueabihf-gcc \
    -O2 -Wall -static-libgcc -nostdinc \
    -o "$OUT/spike_clock" \
    "$HERE/spike_clock.c" \
    -ldl \
    --sysroot="$SYSROOT" \
    -I"$SYSROOT/usr/include" \
    -I"$SYSROOT/usr/include/arm-linux-gnueabihf" \
    -isystem "$(nix shell nixpkgs#pkgsCross.armv7l-hf-multiplatform.stdenv.cc -c armv7l-unknown-linux-gnueabihf-gcc -print-file-name=include)" \
    -I"$GST_DEV/include/gstreamer-1.0" \
    -I"$GLIB_DEV/include/glib-2.0" \
    -I"$GLIB_OUT/lib/glib-2.0/include" \
    -Wl,--dynamic-linker=/lib/ld-linux-armhf.so.3 \
    -B"$SYSROOT/usr/lib/arm-linux-gnueabihf/" \
    -L"$SYSROOT/usr/lib/arm-linux-gnueabihf" \
    -L"$SYSROOT/lib/arm-linux-gnueabihf"

echo "built: $OUT/spike_clock"
