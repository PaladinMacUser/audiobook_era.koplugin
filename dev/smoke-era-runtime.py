#!/usr/bin/env python3
"""Check the packaged ARM English engine and WAV player with QEMU (no speaker)."""
import argparse
import json
import os
import shutil
import shlex
import subprocess
import tempfile
import time
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
        # Static sanoTTS runs directly. Its OOV phonemizer child is itself
        # ARM, so QEMU needs a host wrapper where the reader uses --loader.
        wrapper = root / "espeak-wrapper"
        wrapper.write_text("#!/bin/sh\nexec " + shlex.join([
            args.qemu, str(loader), "--library-path", str(libraries),
            str(plugin / "espeak-ng/bin/espeak-ng.bin")]) + ' "$@"\n')
        wrapper.chmod(0o755)
        neural_audio = [root / ("sano-" + str(i) + ".wav") for i in range(4)]
        texts = ["Hello from the reader.", "Welcome to your book.", "reader", "Hello qxzvabcdef"]
        requests = "".join(json.dumps({"text": text, "output_file": str(path)}) + "\n"
                           for text, path in zip(texts, neural_audio))
        env["SNT_THREADS"] = "2"
        started = time.monotonic()
        result = subprocess.run([args.qemu, str(plugin / "sanotts/snt_server"), "--model",
                                 str(plugin / "sanotts/voice"), "--engine", "int8", "--g2p", "nano",
                                 "--espeak-bin", str(wrapper), "--espeak-data", str(share)],
                                env=env, input=requests, capture_output=True, text=True,
                                check=True, timeout=60)
        for path in neural_audio:
            assert str(path) in result.stdout, "sanoTTS must deliver each sentence/word"
            with wave.open(str(path)) as wav:
                assert wav.getframerate() == 22050 and wav.getsampwidth() == 2 and wav.getnchannels() == 1
                assert wav.getnframes() > 1000 and any(wav.readframes(wav.getnframes()))
        assert "espeak_fb=1" in result.stderr, "Verify the out-of-vocabulary espeak child"
        print(f"sanoTTS ARM check: two sentences, one word and OOV fallback in {time.monotonic()-started:.2f}s under emulation")
        tempo_started = time.monotonic()
        for path in neural_audio:
            with wave.open(str(path)) as wav:
                native_frames = wav.getnframes()
            subprocess.run([args.qemu, str(plugin / "tempo/wav-tempo"), "--speed", "0.5", str(path)],
                           check=True, timeout=15)
            with wave.open(str(path)) as wav:
                assert wav.getframerate() == 22050 and wav.getsampwidth() == 2 and wav.getnchannels() == 1
                assert 1.8 < wav.getnframes() / native_frames < 2.1, "Half-speed speech duration"
                assert any(wav.readframes(wav.getnframes()))
            assert not path.with_suffix(".wav.timing").exists(), "Native timing must not survive tempo conversion"
        print(f"ARM tempo check: four clips at half speed in {time.monotonic()-tempo_started:.2f}s under emulation")
        # The optional larger build keeps the slow Piper alternative tested.
        piper = plugin / "piper"
        if (piper / "piper.bin").exists():
            phonemes = Path(data_directory) / "piper-data"
            shutil.copytree(piper / "espeak-ng-data", phonemes)
            piper_audio = root / "piper.wav"
            requests = json.dumps({"text": "Hello from Piper.", "output_file": str(piper_audio)}) + "\n"
            result = subprocess.run([args.qemu, str(loader), "--library-path",
                                 str(piper / "lib") + ":" + str(libraries),
                                 str(piper / "piper.bin"), "--model", str(piper / "en_US-danny-low.onnx"),
                                 "--espeak_data", str(phonemes), "--json-input", "--sentence_silence", "0"],
                                env=env, input=requests, capture_output=True, text=True,
                                check=True, timeout=180)
            assert str(piper_audio) in result.stdout
            with wave.open(str(piper_audio)) as wav:
                assert wav.getframerate() == 16000 and wav.getsampwidth() == 2 and wav.getnchannels() == 1
                samples = wav.readframes(wav.getnframes())
                assert len(samples) > 8000 and any(samples), "Expected non-silent neural speech"
        config = root / "alsa-null.conf"
        config.write_text("pcm.hwout_mix { type null }\n")
        env["ALSA_CONFIG_PATH"] = str(config)
        result = subprocess.run([args.qemu, str(loader), "--library-path",
                                 str(plugin / "wav-play/lib") + ":" + str(libraries),
                                 str(plugin / "wav-play/wav-play.bin"), "--no-mixer", "-D", "hwout_mix",
                                 "--rate", "48000", "--channels", "2", str(neural_audio[-1])],
                                env=env, capture_output=True, text=True, check=True, timeout=30)
        assert "PLAYING\n" in result.stderr and "write error:" not in result.stderr
        print("ARM runtime OK: espeak, resident sanoTTS, selected-word synthesis, PCM conversion, ALSA null playback")


if __name__ == "__main__":
    main()
