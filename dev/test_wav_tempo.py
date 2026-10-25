#!/usr/bin/env python3
"""Verify speech tempo preserves pitch, duration, clip endings and WAV safety."""
import array
import math
import shutil
import subprocess
import tempfile
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RATE = 22050


def write_wave(path, samples, channels=1):
    with wave.open(str(path), "wb") as wav:
        wav.setparams((channels, 2, RATE, 0, "NONE", "not compressed"))
        wav.writeframes(array.array("h", samples).tobytes())


def read_wave(path):
    with wave.open(str(path)) as wav:
        assert wav.getparams()[:3] == (1, 2, RATE), "Tempo must keep channels, format and sample rate"
        samples = array.array("h", wav.readframes(wav.getnframes()))
    return samples


def frequency(samples):
    crossings = sum(a <= 0 < b for a, b in zip(samples, samples[1:]))
    return crossings * RATE / len(samples)


def main():
    with tempfile.TemporaryDirectory() as tmp:
        directory = Path(tmp)
        binary = directory / "wav-tempo"
        subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-parameter", "-I" + str(ROOT / "tempo"),
                        str(ROOT / "tempo/wav-tempo.c"), str(ROOT / "tempo/sonic.c"),
                        "-lm", "-o", str(binary)], check=True)
        # A second frequency at the end catches accidental tail truncation.
        samples = [round(10000 * math.sin(2 * math.pi * (440 if i < RATE else 660) * i / RATE))
                   for i in range(round(RATE * 1.5))]
        original = directory / "original.wav"
        write_wave(original, samples)
        for speed in (0.25, 0.5, 0.75, 1.0, 1.5, 2.0):
            path = directory / (str(speed) + " space's.wav")
            shutil.copyfile(original, path)
            sidecar = path.with_suffix(".wav.timing")
            sidecar.write_text("native phoneme timestamps")
            subprocess.run([str(binary), "--speed", str(speed), str(path)], check=True)
            result = read_wave(path)
            assert abs(len(result) / len(samples) - 1 / speed) < 0.08 / speed, "Requested duration"
            middle = result[round(0.2 * RATE / speed):round(0.8 * RATE / speed)]
            assert abs(frequency(middle) - 440) < 8, "Slowdown must preserve pitch"
            tail = result[-round(0.3 * RATE / speed):-round(0.1 * RATE / speed)]
            assert abs(frequency(tail) - 660) < 16, "The last voiced segment must survive"
            if speed == 1:
                assert path.read_bytes() == original.read_bytes(), "1x is bit-identical"
                assert sidecar.exists(), "Unchanged audio keeps native timing"
            else:
                assert not sidecar.exists(), "Changed tempo invalidates native word timestamps"
        for duration in (0.1, 0.15, 0.3):
            path = directory / "word.wav"
            write_wave(path, samples[:round(RATE * duration)])
            subprocess.run([str(binary), "--speed", "0.5", str(path)], check=True)
            result = read_wave(path)
            assert len(result) > RATE * duration * 1.8 and any(result), "Short words are slowed too"
        bad = directory / "bad.wav"
        for data in (b"not a WAV", original.read_bytes()[:-2],
                     original.read_bytes()[:22] + b"\x02\x00" + original.read_bytes()[24:]):
            bad.write_bytes(data)
            assert subprocess.run([str(binary), "--speed", "0.5", str(bad)], capture_output=True).returncode
            assert bad.read_bytes() == data, "Failed conversion must preserve its input"
        for speed in ("nan", "inf", "0", "-1", "0.1", "3", "0.5junk", ""):
            assert subprocess.run([str(binary), "--speed", speed, str(original)], capture_output=True).returncode
        assert not list(directory.glob("*.tempo.*")), "No partial conversion files remain"
        print("ok - tempo: six rates preserve pitch and endings; short words, timing and error handling passed")


if __name__ == "__main__":
    main()
