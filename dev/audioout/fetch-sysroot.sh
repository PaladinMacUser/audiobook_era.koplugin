#!/usr/bin/env bash
# Fetch a Debian Jessie (glibc 2.19) armhf sysroot for building audioout
# binaries that run BARE under the Kobo's own loader (the device libc is
# 2.19, crosstool-NG). Same technique as build-bluealsa-compat.sh, trimmed
# to the minimal package set.
#
# Output: dev/audioout/.sysroot/   (not committed)
# Usage: bash dev/audioout/fetch-sysroot.sh
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
SYSROOT="$HERE/.sysroot"
DEBS="$HERE/.sysroot-debs"
DEB_MIRROR="http://archive.debian.org/debian"

# Minimal set: runtime libc + dev (headers, .so linker scripts, crt1.o)
# + kernel headers. No alsa/bluetooth/dbus: the audioout daemon gets those
# from the device at runtime, and the spike needs nothing but libc/dl.
DEBIAN_PACKAGES="
libc6
libc6-dev
linux-libc-dev
"

fix_symlinks() {
    # libc6-dev ships libdl.so / libpthread.so as ABSOLUTE symlinks
    # (-> /lib/arm-linux-...) which resolve to nothing on the build host,
    # so ld silently falls back to the static .a and fails on __dlsym /
    # pthread_mutex_*. Replace them with proper linker scripts in the same
    # style libc.so uses. Idempotent: rerun any time.
    for spec in "libdl libdl.so.2" "libpthread libpthread.so.0"; do
        set -- $spec
        lib=$1; soname=$2
        rm -f "$SYSROOT/usr/lib/arm-linux-gnueabihf/$lib.so"
        cat > "$SYSROOT/usr/lib/arm-linux-gnueabihf/$lib.so" <<EOF
/* GNU ld script */
GROUP ( /lib/arm-linux-gnueabihf/$soname /usr/lib/arm-linux-gnueabihf/$lib.a )
EOF
    done
}

if [ -f "$SYSROOT/lib/arm-linux-gnueabihf/libc.so.6" ]; then
    fix_symlinks
    echo "sysroot already present: $SYSROOT"
    exit 0
fi

mkdir -p "$SYSROOT" "$DEBS"

echo "downloading Debian Jessie armhf package index"
curl -fsL "$DEB_MIRROR/dists/jessie/main/binary-armhf/Packages.gz" \
    | gunzip > "$DEBS/Packages"

for pkg in $DEBIAN_PACKAGES; do
    block=$(awk -v p="$pkg" '
        /^Package: / { in_pkg = ($2 == p) }
        in_pkg {
            print
            if (/^$/) { exit }
        }
    ' "$DEBS/Packages")
    if [ -z "$block" ]; then
        echo "WARNING: package '$pkg' not found in the Jessie index" >&2
        continue
    fi
    filename=$(echo "$block" | awk '/^Filename: / { print $2; exit }')
    sha256=$(echo "$block" | awk '/^SHA256: / { print $2; exit }')
    debname=$(basename "$filename")
    echo "  $pkg -> $debname"
    curl -fsL "$DEB_MIRROR/$filename" -o "$DEBS/$debname"
    echo "$sha256  $DEBS/$debname" | sha256sum -c - >/dev/null
    mkdir -p "$DEBS/extract"
    (cd "$DEBS/extract" && ar x "$DEBS/$debname")
    for tarfile in data.tar.xz data.tar.gz data.tar.bz2; do
        if [ -f "$DEBS/extract/$tarfile" ]; then
            case "$tarfile" in
                *.xz) tar xf "$DEBS/extract/$tarfile" -C "$SYSROOT" ;;
                *.gz) tar xf "$DEBS/extract/$tarfile" -C "$SYSROOT" ;;
                *.bz2) tar xf "$DEBS/extract/$tarfile" -C "$SYSROOT" ;;
            esac
            break
        fi
    done
    rm -rf "$DEBS/extract"
done

if [ ! -f "$SYSROOT/lib/arm-linux-gnueabihf/libc.so.6" ]; then
    echo "ERROR: sysroot creation failed; libc.so.6 not found" >&2
    exit 1
fi

fix_symlinks

echo "sysroot ready: $SYSROOT"
