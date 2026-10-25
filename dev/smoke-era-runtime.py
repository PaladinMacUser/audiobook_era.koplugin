#!/usr/bin/env python3
"""Check the packaged ARM English engine and WAV player with QEMU (no speaker)."""
import argparse
import json
import os
import shutil
import shlex
import socket
import subprocess
import tempfile
import time
import wave
from pathlib import Path
from zipfile import ZipFile


def check_resident_rhvoice(binary, qemu):
    """Run the real packaged ARM worker, independent of Lua's IPC fixture."""
    with tempfile.TemporaryDirectory(prefix="ab-rh-") as directory:
        root = Path(directory)
        endpoint = root / "worker.sock"
        command = [qemu, str(binary), "--daemon", str(endpoint), "--idle-seconds", "2"]
        def wait(predicate, seconds=15):
            deadline = time.monotonic() + seconds
            while not predicate():
                assert time.monotonic() < deadline, "Resident RHVoice timed out"
                time.sleep(.02)
        with (root / "worker.log").open("w") as log:
            worker = subprocess.Popen(command, stdout=log, stderr=log)
            sender = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
            try:
                ready = Path(str(endpoint) + ".ready")
                wait(ready.exists)
                assert int(ready.read_text()) == worker.pid
                def submit(index, text, speed=.5, lead=350, voice="Slt", pitch=1, mode="text"):
                    job, src, dest = (root / f"{index}.{suffix}" for suffix in ("req", "txt", "wav"))
                    src.write_text(text)
                    job.write_text(f"{src}\n{dest}\n{speed}\n1\n{lead}\n{voice}\n{pitch}\n{mode}\n")
                    sender.sendto(str(job).encode(), str(endpoint))
                    return job, dest
                durations = []
                for index, speed in enumerate((1, .5)):
                    job, audio = submit(index, "Hello from your PocketBook reader.", speed)
                    done = Path(str(job) + ".done")
                    wait(done.exists)
                    assert done.read_text().strip() == "OK"
                    assert worker.poll() is None and int(ready.read_text()) == worker.pid
                    with wave.open(str(audio)) as wav:
                        assert wav.getframerate() == 24000 and wav.getsampwidth() == 2
                        assert wav.readframes(8400) == bytes(16800), "Resident mode keeps 350ms guard"
                        assert any(wav.readframes(wav.getnframes()))
                        durations.append(wav.getnframes() - 8400)
                assert 1.8 < durations[1] / durations[0] < 2.2, "One resident engine honors changed rate"
                waveforms = []
                for index, voice in enumerate(("Slt", "Bdl", "Clb", "Alan"),20):
                    job, audio = submit(index,"Hello from your PocketBook reader.",voice=voice)
                    wait(lambda: Path(str(job)+".done").exists())
                    assert Path(str(job)+".done").read_text().strip()=="OK",voice
                    with wave.open(str(audio)) as wav:
                        assert wav.getframerate()==24000
                        pcm=wav.readframes(wav.getnframes());assert any(pcm)
                        waveforms.append(pcm)
                    assert worker.poll() is None and int(ready.read_text())==worker.pid
                assert len(set(waveforms))==4,"Four distinct installed English voices"
                job, pitched = submit(30,"Hello from your PocketBook reader.",pitch=.9)
                wait(lambda: Path(str(job)+".done").exists())
                with wave.open(str(pitched)) as wav:
                    pcm=wav.readframes(wav.getnframes())
                    assert pcm!=waveforms[0] and .95<len(pcm)/len(waveforms[0])<1.05,"Pitch changes speech without changing its speed"
                pause_frames=[]
                for index, pause in enumerate((100,500),31):
                    job, audio=submit(index,f'<speak>Hello,<break time="{pause}ms"/> reader.</speak>',mode="ssml")
                    wait(lambda: Path(str(job)+".done").exists())
                    assert Path(str(job)+".done").read_text().strip()=="OK"
                    with wave.open(str(audio)) as wav: pause_frames.append(wav.getnframes())
                assert 8500<pause_frames[1]-pause_frames[0]<11000,"SSML comma pause controls actual PCM duration at half speed"
                job, audio = submit(2, "reader " * 1000)
                # Cancel a potentially running long request, then enqueue a fresh word.
                Path(str(job) + ".cancel").write_text("cancel")
                next_job, next_audio = submit(3, "reader")
                wait(lambda: Path(str(next_job) + ".done").exists())
                assert Path(str(job) + ".done").read_text().strip() == "CANCELLED"
                assert not audio.exists() and next_audio.exists() and worker.poll() is None
                # Malformed requests fail without killing the reusable process.
                bad = root / "bad.req"
                bad.write_text(f"{root}/missing.txt\n{root}/bad.wav\n0\n1\n350\n")
                sender.sendto(str(bad).encode(), str(endpoint))
                wait(lambda: Path(str(bad) + ".done").exists())
                assert Path(str(bad) + ".done").read_text().startswith("ERR")
                assert not (root / "bad.wav").exists()
                worker.wait(timeout=5)
                assert worker.returncode == 0 and not endpoint.exists() and not ready.exists()
                assert not list(root.glob("*.rhvoice.*")), "No partial output after cancellation"
                # Reusing the socket directory after idle exit must work, with clean TERM shutdown.
                worker = subprocess.Popen(command, stdout=log, stderr=log)
                wait(ready.exists)
                worker.terminate(); worker.wait(timeout=5)
                assert worker.returncode == 0 and not endpoint.exists() and not ready.exists()
            finally:
                sender.close()
                if worker.poll() is None:
                    worker.kill(); worker.wait()
    print("Resident RHVoice ARM check: four distinct English voices, pitch, SSML pauses, reused model, per-request rates, guarded clip starts, cancellation, invalid request, idle unload and restart")


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
        # Copy the RHVoice runtime to tmpfs for ARM's 32-bit directory API.
        # Omit --data: this also checks relocation beside the executable.
        rh = Path(data_directory) / "rhvoice"
        shutil.copytree(plugin / "rhvoice/data", rh / "data")
        shutil.copy2(plugin / "rhvoice/rhvoice-slt", rh / "rhvoice-slt")
        rh_started = time.monotonic()
        rh_text = root / "rhvoice-input.txt"
        rh_durations = []
        rh_paths = []
        for i, (text, speed) in enumerate([
            ("Hello from your PocketBook reader.", "1.0"),
            ("Hello from your PocketBook reader.", "0.5"),
            ("reader", "0.5"),
            ("Don't panic—it's 10:30.", "0.75"),
        ]):
            rh_text.write_text(text)
            path = root / f"rhvoice-{i}.wav"
            result = subprocess.run([args.qemu, str(rh / "rhvoice-slt"), "--input", str(rh_text),
                                    "--output", str(path), "--speed", speed],
                                   capture_output=True, text=True, check=True, timeout=30)
            with wave.open(str(path)) as wav:
                assert wav.getframerate() == 24000 and wav.getsampwidth() == 2 and wav.getnchannels() == 1
                assert wav.getnframes() > 1000 and any(wav.readframes(wav.getnframes()))
                rh_durations.append(wav.getnframes() / wav.getframerate())
            rh_paths.append(path)
            assert not list(root.glob(path.name + ".rhvoice.*")), "RHVoice must not leave partial WAVs"
        assert 1.8 < rh_durations[1] / rh_durations[0] < 2.2, "RHVoice honors the selected half speed"
        # Speaker wake-up guard must prepend silence without trimming,
        # fading, resampling or otherwise changing a single speech sample.
        for index, text, speed in [(0, "Hello from your PocketBook reader.", "1.0"), (2, "reader", "0.5")]:
            rh_text.write_text(text)
            guarded = root / f"rhvoice-guard-{index}.wav"
            subprocess.run([args.qemu, str(rh / "rhvoice-slt"), "--input", str(rh_text),
                            "--output", str(guarded), "--speed", speed, "--lead-in-ms", "350"],
                           capture_output=True, check=True, timeout=30)
            with wave.open(str(rh_paths[index])) as original, wave.open(str(guarded)) as padded:
                assert original.getframerate() == padded.getframerate() == 24000
                assert padded.getnframes() == original.getnframes() + 8400
                assert padded.readframes(8400) == bytes(16800), "Guard is exactly 350 ms of silence"
                assert padded.readframes(padded.getnframes()) == original.readframes(original.getnframes()), "Every speech sample, including the first and last, must be unchanged"
        failed = root / "rhvoice-missing-data.wav"
        result = subprocess.run([args.qemu, str(rh / "rhvoice-slt"), "--data", str(root / "missing-data"),
                                 "--input", str(rh_text), "--output", str(failed), "--speed", "0.5"],
                                capture_output=True, check=False, timeout=15)
        assert result.returncode and not failed.exists(), "Missing RHVoice data must fail without publishing a WAV"
        assert not list(root.glob("*.rhvoice.*")), "No partial RHVoice outputs after failure"
        print(f"RHVoice ARM check: four English clips, half-speed duration and failure handling in {time.monotonic()-rh_started:.2f}s under emulation")
        check_resident_rhvoice(rh / "rhvoice-slt", args.qemu)
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
                                 "--rate", "48000", "--channels", "2", str(rh_paths[1])],
                                env=env, capture_output=True, text=True, check=True, timeout=30)
        assert "PLAYING\n" in result.stderr and "write error:" not in result.stderr
        from test_era_player import check_player
        with tempfile.TemporaryDirectory(prefix="audiobook-arm-pcm-") as player_directory:
            check_player([args.qemu,str(loader),"--library-path",str(plugin/"wav-play/lib")+":"+str(libraries),
                          str(plugin/"rhvoice/era-player")],env,Path(player_directory))
        print("ARM runtime OK: espeak, resident sanoTTS, RHVoice/SLT, selected-word synthesis, PCM conversion, ALSA null playback")


if __name__ == "__main__":
    main()
