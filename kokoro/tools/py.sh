#!/usr/bin/env bash
# Run the Kokoro-port venv with the native libs NixOS does not put on the
# default loader path (the PyPI wheels are manylinux builds). Keep every port
# command going through this wrapper so the environment is uniform.
#
# Usage: kokoro/tools/py.sh script.py [args...]      or:  ... -c 'code'
set -euo pipefail

REPO=$(cd "$(dirname "$0")/../.." && pwd)
PY="$REPO/.venv-kokoro/bin/python"
[ -x "$PY" ] || { echo "no venv; run kokoro/tools/setup-env.sh first" >&2; exit 1; }

LIBS=""
# libstdc++/libgcc for the wheels, zlib and libgomp for torch, espeak-ng and
# libsndfile for misaki's fallback G2P.
for attr in stdenv.cc.cc.lib zlib libgomp espeak-ng libsndfile; do
    p=$(nix eval --raw "nixpkgs#$attr" 2>/dev/null) || continue
    [ -d "$p/lib" ] && LIBS="$LIBS:$p/lib"
done
export LD_LIBRARY_PATH="${LIBS#:}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$PY" "$@"
