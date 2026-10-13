# Cross-compile the Kokoro-family runtime driver for Kobo (armv7l, static musl).
# Same toolchain pin as the other cross builds so the runner reuses the cached
# toolchain. Usage: nix-build kokoro/cross-build-koko-tts.nix --no-out-link
let
  nixpkgsSrc = fetchTarball "https://github.com/NixOS/nixpkgs/archive/255a186666b6130ddddf8ad749887102a0820914.tar.gz";
  pkgs = import nixpkgsSrc {};
  crossPkgs = pkgs.pkgsCross.armv7l-hf-multiplatform.pkgsStatic;
  kok = ./.;   # the kokoro/ tree (sources and headers)
in

crossPkgs.stdenv.mkDerivation {
  pname = "koko-tts";
  version = "0.1.0";

  dontUnpack = true;
  dontConfigure = true;

  buildPhase = ''
    export NIX_CFLAGS_COMPILE="$NIX_CFLAGS_COMPILE -O2 -std=c99 -I${kok}/include"
    $CC -static -o kok_tts ${kok}/src/kok_tts.c ${kok}/src/kok_meta.c \
      ${kok}/src/kok_kernels.c ${kok}/src/kok_istft.c ${kok}/src/kok_rng.c \
      ${kok}/src/kok_source.c ${kok}/src/kok_text.c ${kok}/src/kok_dec.c -lm
  '';

  installPhase = ''
    mkdir -p $out/bin
    cp kok_tts $out/bin/
  '';
}
