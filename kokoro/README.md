# Kokoro-family C runtime

Hand-written C for Kokoro-family neural TTS on the plugin's devices, replacing
the onnxruntime dependency. Configuration-driven so one runtime serves Paradee
presets, Kokoro-7M-Distill, and Kokoro-82M with only blobs and a config.

First target: **Paradee-8M v1.0** (8.07M params, Apache-2.0, 24 kHz, English).
Progress against the approved plan is tracked in the plan file
(`~/.claude/plans/nested-kindling-breeze.md`).

## Pipeline (verified from the author's source and the shipped ONNX)

phoneme ids `[0] + ids + [0]` (max 510, vocab 178) to durations to an
alignment expansion to F frames (80 fps) to F0 and N at 2F to a decoder plus an
iSTFT generator plus a phase-lock filter, output 24 kHz.

- Text side (4.23M, preset `s+mlp`): ALBERT with one shared block (hidden 256,
  4 heads, intermediate 768, 6 layers, embedding 128) then a linear to 192,
  kokoro's ProsodyPredictor (bidirectional LSTMs, AdaLayerNorm blocks, summed
  sigmoid durations, F0/N heads), kokoro's TextEncoder (embedding, CNN,
  bidirectional LSTM), and a 192/512/512 GELU projection. Style vector dim 32,
  baked (one voice).
- Decoder (3.85M, preset `A`): AdaIN residual blocks 514 to 256 then three at
  256 plus one upsampling block, stride-2 F0/N convs, 512 to 64 conv, then
  kokoro's istftnet Generator (upsample 10 and 6, initial channel 128, kernels
  3/7/11, dilations 1/3/5, upsample kernels 20 and 12, istft n_fft 20, hop 5)
  with a harmonic sine source, Snake activations, and an inverse STFT written
  as ConvTranspose1d over DFT bases. Then the phase-lock filter: 1024-point
  STFT at hop 256, band 2 to 8 kHz (bins 86 to 341), Hann smoothing over 33
  frames, voiced frames only (f0 above 60 Hz).
- The shipped ONNX uses the author's `ConvSTFT` stand-in (reflect padding,
  DC/Nyquist imaginary parts zeroed, inverse scaled by 1/n and divided by the
  window-overlap envelope), not torch.stft. The C port matches ConvSTFT.
- The sine source contains `RandomUniformLike`/`RandomNormalLike`, so the model
  is stochastic per run and the ONNX cannot serve as a golden for it. The port
  seeds it from the sentence text and reproduces ATen's MT19937/Box-Muller
  (the same approach as `sanotts/mcu/src/snt_nano.c`).

## Toolchain

`kokoro` and `misaki` are not in nixpkgs, so the tooling is a pip venv over a
nix python. Everything must run through the wrapper so NixOS finds the wheels'
native libraries.

```sh
kokoro/tools/setup-env.sh                      # once
kokoro/tools/py.sh kokoro/tools/gen_goldens.py --help
```

## Reference goldens (M0, done)

`kokoro/tools/gen_goldens.py` rebuilds the author's shipped inference path in
PyTorch fp32 (inlining `Paradee`/`ConvSTFT`/`Lock` from
`training/scripts/export_paradee.py` at commit `9c8b4d75`, because that file
exports at import time) and dumps every stage boundary for a fixed sentence set:

```sh
kokoro/tools/py.sh kokoro/tools/gen_goldens.py \
  --repo kokoro/model/paradee-repo --text kokoro/model/text_side.pt \
  --dec kokoro/model/decoder.pt --config dev/paradee/config.json \
  --sentences dev/paradee/sentences.txt --out kokoro/goldens/paradee-8m --seed 0
```

16 stages per sentence (ids, phonemes, dur_raw, durs, aln, d, en, F0, N,
asr_tok, asr, F0_in, N_in, src, wave_pre_lock, waveform), plus
`manifest.json` and `SHA256SUMS`. About 22 MB for five sentences.

Gate result (2026-10-13): the reference reproduces the shipped int8 ONNX at
spectral correlation 0.993-0.995 with byte-identical output lengths on three of
five sentences; the other two differ by exactly two generator frames, which is
consistent with int8-versus-fp32 rounding flips at half-frame duration
boundaries. The ONNX was used as the gate, not the published sample WAVs: those
are 5 to 6 percent shorter than both the reference and the shipped ONNX and
match neither (spectral correlation 0.15 to 0.30), so they come from a
different revision than the weights that ship beside them.

## Layout

`include/` and `src/` hold the runtime (`kok_` prefix, own port header,
caller-owned arenas and stage taps in the style of
`sanotts/mcu/src/snt_piperlite.c`); `tools/` the venv wrapper, golden dumper
and blob converters; `model/` is gitignored working data (checkpoints, the
reference checkout, samples); `test/` the C gates mirroring
`sanotts/mcu/test/`.
