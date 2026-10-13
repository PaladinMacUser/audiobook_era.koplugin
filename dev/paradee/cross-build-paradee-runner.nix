# Cross-compile the Paradee benchmark runner for Kobo (armv7l, glibc dynamic).
# Links against the exact libonnxruntime.so.1.14.1 (armhf) that the plugin's
# Piper backend already bundles (ortlib/, copied from the Piper armhf tarball),
# so the device needs nothing new at runtime except this binary and the model.
# Headers are ORT v1.14.1 to match the bundled library's API version.
# Launch on device through the bundled loader, like piper itself:
#   <plugin>/espeak-ng/lib/ld-linux-armhf.so.3 --library-path <plugin>/piper/lib \
#     ./paradee_runner model.onnx sent.ids out.wav
# Usage: nix-build cross-build-paradee-runner.nix --no-out-link
let
  nixpkgsSrc = fetchTarball "https://github.com/NixOS/nixpkgs/archive/255a186666b6130ddddf8ad749887102a0820914.tar.gz";
  pkgs = import nixpkgsSrc {};
  crossPkgs = pkgs.pkgsCross.armv7l-hf-multiplatform;
  here = ./.;
in

crossPkgs.stdenv.mkDerivation {
  pname = "paradee-runner";
  version = "0.1.0";

  dontUnpack = true;
  dontConfigure = true;

  buildPhase = ''
    export NIX_CFLAGS_COMPILE="$NIX_CFLAGS_COMPILE -O2 -I${here}"
    $CC -o paradee_runner ${here}/paradee_runner.c \
      -L${here}/ortlib -l:libonnxruntime.so.1.14.1 -lm
  '';

  installPhase = ''
    mkdir -p $out/bin
    cp paradee_runner $out/bin/
  '';
}
