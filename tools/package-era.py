#!/usr/bin/env python3
"""Assemble the Era English release from a verified upstream ARM runtime."""
import argparse
import hashlib
import stat
import subprocess
import sys
import tempfile
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo

ROOT = Path(__file__).resolve().parents[1]
RUNTIME_URL = "https://github.com/stradichenko/audiobook.koplugin/releases/download/v0.6.3/audiobook-koplugin-v0.6.3.zip"
RUNTIME_SHA256 = "a479409534cea65af60923cab69716a9affcb9357ab160f4bad5c03925d96b9a"
PREFIX = "audiobook.koplugin/"


def package(runtime_zip, output, with_piper=False, tempo_binary=None, rhvoice_dir=None):
    if hashlib.sha256(runtime_zip.read_bytes()).hexdigest() != RUNTIME_SHA256:
        raise ValueError("Upstream runtime ZIP checksum mismatch")
    content = {}
    with ZipFile(runtime_zip) as upstream:
        for info in upstream.infolist():
            name = info.filename
            if info.is_dir() or not name.startswith(PREFIX):
                continue
            relative = name[len(PREFIX):]
            if (relative.startswith(("espeak-ng/lib/", "espeak-ng/share/espeak-ng-data/", "wav-play/", "sanotts/voice/"))
                    or relative in ("espeak-ng/bin/espeak-ng.bin", "sanotts/snt_server")
                    or with_piper and relative.startswith("piper/")):
                content[name] = (upstream.read(info), info.external_attr >> 16)
    # Current source always replaces upstream Lua, including the fork updater.
    for source in sorted(ROOT.glob("*.lua")):
        content[PREFIX + source.name] = (source.read_bytes(), stat.S_IFREG | 0o644)
    for relative in ("LICENSE", "INSTALL-ERA.md", "RUNTIME-SOURCES.md", "generate-report.sh",
                     "cross-build-espeak.nix", "cross-build-wav-play.nix", "cross-build-snt-server.nix", "wav-play.c",
                     "sanotts/README.md", "sanotts/LICENSE-sanoTTS", "sanotts/LICENSE-sanoTTS-MIT",
                     "sanotts/LICENSE-misaki", "sanotts/NOTICE-misaki.md"):
        source = ROOT / relative
        content[PREFIX + relative] = (source.read_bytes(), stat.S_IFREG | (0o755 if relative.endswith(".sh") else 0o644))
    for source in sorted((ROOT / "tempo").iterdir()):
        if source.is_file():
            content[PREFIX + "tempo/" + source.name] = (source.read_bytes(), stat.S_IFREG | 0o644)
    with tempfile.TemporaryDirectory() as build_dir:
        if tempo_binary is None:
            tempo_binary = Path(build_dir) / "wav-tempo"
            subprocess.run(["arm-linux-gnueabihf-gcc", "-std=c11", "-O2", "-static",
                            "-I" + str(ROOT / "tempo"), str(ROOT / "tempo/wav-tempo.c"),
                            str(ROOT / "tempo/sonic.c"), "-lm", "-o", str(tempo_binary)], check=True)
        binary = tempo_binary.read_bytes()
        if binary[:4] != b"\x7fELF" or binary[4:6] != b"\x01\x01" or binary[18:20] != b"\x28\x00":
            raise ValueError("wav-tempo must be a little-endian 32-bit ARM ELF binary")
        content[PREFIX + "tempo/wav-tempo"] = (binary, stat.S_IFREG | 0o755)
    if rhvoice_dir is None:
        subprocess.run([sys.executable, str(ROOT / "tools/build-rhvoice.py")], check=True)
        rhvoice_dir = ROOT / ".cache/rhvoice-runtime"
        subprocess.run([sys.executable,str(ROOT/"tools/build-era-player.py"),"--output",str(rhvoice_dir/"era-player")],check=True)
    rh_binary = (rhvoice_dir / "rhvoice-slt").read_bytes()
    if rh_binary[:6] != b"\x7fELF\x01\x01" or rh_binary[18:20] != b"\x28\x00":
        raise ValueError("RHVoice must be a little-endian 32-bit ARM ELF binary")
    player_binary=(rhvoice_dir/"era-player").read_bytes()
    if player_binary[:6]!=b"\x7fELF\x01\x01" or player_binary[18:20]!=b"\x28\x00":
        raise ValueError("Era player must be a little-endian 32-bit ARM ELF binary")
    for source in sorted(rhvoice_dir.rglob("*")):
        if source.is_file():
            relative = source.relative_to(rhvoice_dir).as_posix()
            content[PREFIX + "rhvoice/" + relative] = (source.read_bytes(), stat.S_IFREG | (0o755 if relative in ("rhvoice-slt","era-player") else 0o644))
    for source in sorted((ROOT / "rhvoice").iterdir()):
        if source.is_file():
            content[PREFIX + "rhvoice/" + source.name] = (source.read_bytes(), stat.S_IFREG | 0o644)
    content[PREFIX + "tools/build-era-player.py"] = ((ROOT / "tools/build-era-player.py").read_bytes(), stat.S_IFREG | 0o644)
    content[PREFIX + "tools/build-rhvoice.py"] = ((ROOT / "tools/build-rhvoice.py").read_bytes(), stat.S_IFREG | 0o644)
    for source in sorted((ROOT / "l10n").rglob("*")):
        if source.is_file():
            content[PREFIX + source.relative_to(ROOT).as_posix()] = (source.read_bytes(), stat.S_IFREG | 0o644)
    required = ("pocketbookaudio.lua", "ttsengine.lua", "main.lua", "updater.lua",
                "espeak-ng/bin/espeak-ng.bin", "espeak-ng/lib/ld-linux-armhf.so.3",
                "espeak-ng/lib/libespeak-ng.so.1", "espeak-ng/share/espeak-ng-data/en_dict",
                "wav-play/wav-play.bin", "wav-play/lib/libasound.so.2", "wordreader.lua",
                "sanotts/snt_server", "sanotts/voice/front_q8.bin", "sanotts/voice/model_q8.bin",
                "tempo/wav-tempo", "tempo/LICENSE", "rhvoice/rhvoice-slt", "gestureguard.lua",
                "rhvoice/data/languages/English/cmulex.fst", "rhvoice/data/voices/slt/24000/voice.data",
                "rhvoice/data/voices/bdl/24000/voice.data", "rhvoice/data/voices/clb/24000/voice.data", "rhvoice/data/voices/alan/24000/voice.data",
                "rhvoice/data/config/RHVoice.conf", "rhvoice/corresponding-source.tar.gz", "rhvoice/era-player", "rhvoicesettings.lua", "wordcache.lua", "rhvoiceclient.lua", "speechdiagnostics.lua")
    if with_piper:
        required += (
                "piper/piper.bin", "piper/lib/libonnxruntime.so.1.14.1",
                "piper/lib/libpiper_phonemize.so.1", "piper/espeak-ng-data/phontab",
                "piper/en_US-danny-low.onnx", "piper/en_US-danny-low.onnx.json")
    for relative in required:
        if PREFIX + relative not in content:
            raise ValueError("Missing release component: " + relative)
    output.parent.mkdir(parents=True, exist_ok=True)
    with ZipFile(output, "w", ZIP_DEFLATED, compresslevel=9) as archive:
        for name, (data, mode) in sorted(content.items()):
            info = ZipInfo(name, date_time=(2026, 10, 25, 0, 0, 0))
            info.create_system = 3
            info.external_attr = mode << 16
            info.compress_type = ZIP_DEFLATED
            archive.writestr(info, data)
    checksum = hashlib.sha256(output.read_bytes()).hexdigest()
    output.with_suffix(output.suffix + ".sha256").write_text(checksum + "  " + output.name + "\n")
    print(f"{output}: {output.stat().st_size:,} bytes; SHA256 {checksum}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime-zip", type=Path, help="Use a previously downloaded v0.6.3 runtime ZIP")
    parser.add_argument("--with-piper", action="store_true", help="Also include the slower Piper engine and Danny voice")
    parser.add_argument("--tempo-binary", type=Path, help="Use an already built static ARM wav-tempo")
    parser.add_argument("--rhvoice-dir", type=Path, help="Use a runtime already built by tools/build-rhvoice.py")
    parser.add_argument("--output", type=Path, default=ROOT / "dist/audiobook-era-v0.8.2.zip")
    args = parser.parse_args()
    if args.runtime_zip:
        package(args.runtime_zip, args.output, args.with_piper, args.tempo_binary, args.rhvoice_dir)
    else:
        with tempfile.TemporaryDirectory() as temporary:
            runtime = Path(temporary) / "upstream.zip"
            subprocess.run(["curl", "--fail", "--location", "--retry", "3", "--output", str(runtime), RUNTIME_URL], check=True)
            package(runtime, args.output, args.with_piper, args.tempo_binary, args.rhvoice_dir)


if __name__ == "__main__":
    main()
