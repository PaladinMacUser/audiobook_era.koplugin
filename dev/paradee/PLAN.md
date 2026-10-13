# Paradee-8M on-device experiment (Libra Color)

Model: sahilmahendrakar/Paradee-8M-v1.0 (HF, Apache-2.0), an 8.07M-param
distill of Kokoro-82M. English only, single voice (af_heart), 24 kHz,
misaki phoneme ids. Ships int8 ONNX (9.0 MB) and fp32 (37 MB).

Claimed (paper, Apple M4 Pro, single thread): 18x realtime in onnxruntime
(int8), UTMOS 4.41, WER 5.7%. Piper lessac-medium on the same harness:
15.4x, UTMOS 4.36. On their Kobo anchor, sanoTTS kristin/amy run at
RTF ~0.15 (Libra Color, dual-core).

## The question

Is Paradee usable as an on-device voice at all (RTF < ~1 on a Libra Color),
and if so is it audibly better than amy (1.5 MB, UTMOS 4.10-class) and
piper danny-low? Note: upstream sanoTTS already ships a Kokoro distill
(heart, 2.27M) and in upstream's own measurements it does NOT beat amy
(SCOREQ 3.48 vs 4.13). Cross-paper UTMOS numbers are not comparable, so the
on-device listening test is the only arbiter.

## Runtime on Kobo

No official onnxruntime armv7hf build exists (only aarch64/x64 upstream).
The bundled piper armv7 binary statically links its own ORT (not reusable).
Options, in order:

1. Build onnxruntime for armv7hf via the nix cross toolchain
   (pkgsCross.armv7l-hf-multiplatform, shared or static lib), then link
   `paradee_runner.c` against it. ORT is heavy; expect a slow first build
   and possible nixpkgs patches. Shared-lib variant preferred: bundle
   libonnxruntime.so alongside the runner like the espeak-ng/piper libs.
2. If the nix build fights back: build a plain fp32 Kokoro graph port
   through the sanoTTS mcu kernels is the long-term path anyway; the
   experiment's job is to decide whether that work is justified.

The runner takes misaki ids (int64 LE), feeds input_ids [1,T] + speed [1],
writes the waveform [1,N] to a 24 kHz WAV, and prints synthesis wall time
and RTF.

## Steps

1. Gary: gen_ids.py with misaki[en] (pip in a venv; misaki is not in
   nixpkgs). Test sentences committed as .ids files so the device side
   needs no Python.
2. Host sanity: build paradee_runner.c for x86 against nixpkgs onnxruntime,
   verify ids -> plausible audio (listen, check duration/silence).
3. Kobo (root@192.168.1.142:2222, device sleeps; keep sessions short):
   scp runner + libonnxruntime.so + paradee_int8.onnx + ids files, run,
   record RTF per sentence, pull WAVs back for listening.
4. Compare RTF against sanoTTS anchors measured on the same device, and
   quality against amy/piper by listening.

## Stop conditions

- RTF > ~1.5 on the Libra Color with the fp32 graph (int8 broken/slow on
  armv7 MLAS): drop for Kobo. Re-evaluate only if Android gets an on-device
  neural tier (ORT ships arm64 AARs there).
- RTF ~0.5-1.0 but audio no better than amy: drop; amy stays the quality
  voice at 1.5 MB.
- RTF <= ~0.5 and audible win: promote to a real backend. The path is a
  Kokoro-family C port in the sanoTTS mcu pattern (bit-exact ARM port),
  bundling the 9 MB int8 blobs. English-only; amy stays for the fast tier
  and piper stays for quality until the port lands.

## Results (2026-10-09, on-device)

Test device: the plugin's ARMv7 single-core MTK test Kobo (192.168.1.142),
int8 graph, bundled libonnxruntime.so.1.14.1 (armhf, from the Piper bundle),
ORT_DISABLE_ALL, single thread, ids generated on gary with misaki[en].

- RTF 2.51-2.66 (five WikiText-103 sentences, 6-11.3 s each; avg ~2.59).
- Session load ~3 s (18 s wall for a 15.2 s-synth run); piper cold start on
  the Clara benchmark was 2.7-4 s, so same class.
- Device output verified against the host run: identical frame counts, RMS
  equal to 4 decimals; max per-sample diff ~0.1 percent of full scale
  (fp32 accumulation order between x86 and armhf kernels; inaudible).
- Works ONLY with ORT_DISABLE_ALL: with default graph optimizations Run
  returns success but the output OrtValue is unconstructed (and the
  process later segfaults reading it). The runner sets
  SetSessionGraphOptimizationLevel(ORT_DISABLE_ALL) accordingly.
- The launch must go through the bundled loader with --library-path
  (LD_LIBRARY_PATH env var form segfaults in the loader); same pattern as
  piperqueue uses for piper.

Verdict against the stop conditions: RTF 2.6 on this old single-core class
is above the 1.5 cutoff -> not usable as a general Kobo voice. On the same
device class it still beats Piper low (measured RTF 3.0 best amortized
config, 3.1-5.4 typical, Clara benchmark) while single-shot, smaller
(9 vs 15 MB) and likely better-sounding (UTMOS 4.41 vs 4.36-class), so it
remains a candidate Piper replacement on NEWER hardware only (Libra
Color-class estimate RTF ~1.0-1.3, borderline; the plugin's background
next-sentence prefetch could hide a sub-1.0 RTF). Open for production:
on-device misaki G2P (ids were precomputed on gary for this benchmark).

Listening files: dev/paradee/device-wav/sentences.*.wav (device),
dev/paradee/sentences.*.wav (host).