#!/usr/bin/env bash
# Toolchain for the Kokoro-family port: reference goldens and G2P.
#
# kokoro and misaki are not in nixpkgs and the CPU torch wheel is the sane
# reference build, so this is a pinned pip venv over a nix python rather than a
# pure devShell. The venv lives in the repo and is gitignored; run every python
# command through kokoro/tools/py.sh so NixOS gets the native libs onto the
# loader path.
#
# Usage: kokoro/tools/setup-env.sh
set -euo pipefail

REPO=$(cd "$(dirname "$0")/../.." && pwd)
VENV="$REPO/.venv-kokoro"
cd "$REPO"

if [ ! -x "$VENV/bin/python" ]; then
    echo "creating $VENV (python 3.12)"
    nix shell nixpkgs#python312 -c bash -c "python3 -m venv '$VENV'"
fi

"$VENV/bin/pip" install -q --upgrade pip
echo "installing CPU torch"
"$VENV/bin/pip" install -q torch --index-url https://download.pytorch.org/whl/cpu
# kokoro carries the module definitions the students subclass (kokoro.modules,
# kokoro.istftnet); misaki is the G2P; the rest is goldens and cross-checks.
"$VENV/bin/pip" install -q transformers kokoro 'misaki[en]' numpy soundfile onnx onnxruntime

echo "env ready: $VENV"
# The PyPI wheels are manylinux builds; NixOS needs py.sh's loader path.
"$REPO/kokoro/tools/py.sh" - <<'EOF'
import importlib
for mod in ("torch", "transformers", "kokoro", "misaki", "numpy", "soundfile", "onnx", "onnxruntime"):
    m = importlib.import_module(mod)
    print(f"  {mod:12s} {getattr(m, '__version__', '?')}")
EOF