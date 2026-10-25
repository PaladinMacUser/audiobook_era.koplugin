#!/usr/bin/env python3
"""Check the packaged ARM English engine and WAV player with QEMU (no speaker)."""
import argparse
import os
import shutil
import subprocess
import tempfile
import wave
from pathlib import Path
from zipfile import ZipFile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("zip", type=Path)
    parser.add_argument("--qemu", default="qemu-arm")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as directory, tempfile.TemporaryDirectory(
        # ARM's 32-bit readdir can fail on a host filesystem with large
        # directory cookies. Use tmpfs for voice data when available.
        dir="/dev/shm" if Path("/dev/shm").is_dir() else None
    ) as data_directory:
        root = Path(directory)
        with ZipFile(args.zip) as archive:
            assert archive.testzip() is None
            for info in archive.infolist():
                path = Path(info.filename)
                assert not path.is_absolute() and ".." not in path.parts
                if info.is_dir():
                    continue
                target = root / path
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(archive.read(info))
                target.chmod((info.external_attr >> 16) & 0o777 or 0o644)
        plugin = root / "audiobook.koplugin"
        share = Path(data_directory) / "share"
        shutil.copytree(plugin / "espeak-ng/share", share)
        libraries = plugin / "espeak-ng/lib"
        loader = libraries / "ld-linux-armhf.so.3"
        env = os.environ.copy()
        env["ESPEAK_DATA_PATH"] = str(share)
        audio = root / "english.wav"
        subprocess.run([args.qemu, str(loader), "--library-path", str(libraries),
                        str(plugin / "espeak-ng/bin/espeak-ng.bin"), "-v", "en", "-w", str(audio),
                        "Hello. This is English speech for your PocketBook Era."],
                       env=env, check=True, timeout=30)
        with wave.open(str(audio)) as wav:
            assert wav.getframerate() == 22050 and wav.getsampwidth() == 2 and wav.getnchannels() == 1
            samples = wav.readframes(wav.getnframes())
            assert len(samples) > 44000 and any(samples), "Expected non-silent English PCM"
        config = root / "alsa-null.conf"
        config.write_text("pcm.tts_sm { type null }\n")
        env["ALSA_CONFIG_PATH"] = str(config)
        result = subprocess.run([args.qemu, str(loader), "--library-path",
                                 str(plugin / "wav-play/lib") + ":" + str(libraries),
                                 str(plugin / "wav-play/wav-play.bin"), "--no-mixer", "-D", "tts_sm",
                                 "--rate", "48000", "--channels", "2", str(audio)],
                                env=env, capture_output=True, text=True, check=True, timeout=30)
        assert "PLAYING\n" in result.stderr and "write error:" not in result.stderr
        print("ARM runtime OK: English synthesis, PCM conversion, ALSA null playback")


if __name__ == "__main__":
    main()
