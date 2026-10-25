#!/usr/bin/env python3
"""Assemble the Era English release from a verified upstream ARM runtime."""
import argparse
import hashlib
import stat
import subprocess
import tempfile
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo

ROOT = Path(__file__).resolve().parents[1]
RUNTIME_URL = "https://github.com/stradichenko/audiobook.koplugin/releases/download/v0.6.3/audiobook-koplugin-v0.6.3.zip"
RUNTIME_SHA256 = "a479409534cea65af60923cab69716a9affcb9357ab160f4bad5c03925d96b9a"
PREFIX = "audiobook.koplugin/"


def package(runtime_zip, output):
    if hashlib.sha256(runtime_zip.read_bytes()).hexdigest() != RUNTIME_SHA256:
        raise ValueError("Upstream runtime ZIP checksum mismatch")
    content = {}
    with ZipFile(runtime_zip) as upstream:
        for info in upstream.infolist():
            name = info.filename
            if info.is_dir() or not name.startswith(PREFIX):
                continue
            relative = name[len(PREFIX):]
            if (relative.startswith(("espeak-ng/lib/", "espeak-ng/share/espeak-ng-data/", "wav-play/"))
                    or relative == "espeak-ng/bin/espeak-ng.bin"):
                content[name] = (upstream.read(info), info.external_attr >> 16)
    # Current source always replaces upstream Lua, including the fork updater.
    for source in sorted(ROOT.glob("*.lua")):
        content[PREFIX + source.name] = (source.read_bytes(), stat.S_IFREG | 0o644)
    for relative in ("LICENSE", "INSTALL-ERA.md", "RUNTIME-SOURCES.md", "generate-report.sh",
                     "cross-build-espeak.nix", "cross-build-wav-play.nix", "wav-play.c"):
        source = ROOT / relative
        content[PREFIX + relative] = (source.read_bytes(), stat.S_IFREG | (0o755 if relative.endswith(".sh") else 0o644))
    for source in sorted((ROOT / "l10n").rglob("*")):
        if source.is_file():
            content[PREFIX + source.relative_to(ROOT).as_posix()] = (source.read_bytes(), stat.S_IFREG | 0o644)
    required = ("pocketbookaudio.lua", "ttsengine.lua", "main.lua", "updater.lua",
                "espeak-ng/bin/espeak-ng.bin", "espeak-ng/lib/ld-linux-armhf.so.3",
                "espeak-ng/lib/libespeak-ng.so.1", "espeak-ng/share/espeak-ng-data/en_dict",
                "wav-play/wav-play.bin", "wav-play/lib/libasound.so.2")
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
    parser.add_argument("--output", type=Path, default=ROOT / "dist/audiobook-era-v0.6.3.1.zip")
    args = parser.parse_args()
    if args.runtime_zip:
        package(args.runtime_zip, args.output)
    else:
        with tempfile.TemporaryDirectory() as temporary:
            runtime = Path(temporary) / "upstream.zip"
            subprocess.run(["curl", "--fail", "--location", "--retry", "3", "--output", str(runtime), RUNTIME_URL], check=True)
            package(runtime, args.output)


if __name__ == "__main__":
    main()
