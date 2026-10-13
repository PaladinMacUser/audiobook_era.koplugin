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

## Torch-free numpy reference (M1, done)

`numpy_ref.py` (primitives), `numpy_model.py` (assembly) and
`check_numpy_ref.py` (the gate) reimplement the whole pipeline without torch and
are checked stage by stage against the goldens, with the generator's stochastic
source fed from the golden tensor so the deterministic kernels are verified
independently of RNG parity. `debug_bisect.py` and `debug_gen.py` compare
individual modules against the author's PyTorch modules on identical inputs.

Gate result (2026-10-13, five sentences): frame counts and alignment exact
(417/241/320/408/451 frames, zero sample drift), durations raw 7.6e-06, prosody
3.2e-06, N 4.1e-06, asr 1.5e-06, generator output 1.2e-05, final waveform
1.1e-05, all correlations 1.00000. Errors are relative to each stage's own
magnitude (F0 peaks near 300, the waveform near 0.5), so the gate is relative;
every stage lands at or under 2e-06 relative, which is float32 accumulation
noise.

Semantics this pinned, each of which is easy to get wrong:

- ALBERT: one shared `albert_layer_groups.0.albert_layers.0` block applied six
  times, absolute position embeddings, `layer_norm_eps` 1e-12, and the FFN
  activation is `gelu_new` (tanh form) while the asr projection uses exact erf
  GELU.
- Bidirectional LSTMs: PyTorch stores the backward direction's states reversed
  relative to the order they are produced (aligned to the input index).
- Attention: `[heads, T, head_size]`, scores contracting the head-size axis,
  scaling by head_size^-0.5.
- Decoder: F0/N enter through stride-2 convs, the asr skip is re-concatenated
  before every non-upsampling decode block, AdainResBlk1d's shortcut is
  nearest-neighbour interpolation while its residual path uses a learned
  depthwise ConvTranspose1d, and the output is scaled by 1/sqrt(2).
- Generator: Snake is `x + (1/alpha)*sin(alpha*x)^2` with per-channel alpha,
  `ReflectionPad1d((1,0))` on the last upsample aligns the audio branch with the
  STFT frame count (which is odd), `conv_post` splits into exp() magnitude and
  sin() phase over `post_n_fft`, and the inverse STFT is ConvTranspose1d over
  DFT bases with reflect padding and a 1/n scaling divided by the window-overlap
  envelope.
- Phase lock: 1024-point analysis at hop 256, bins 86 to 341 (2 to 8 kHz), a
  33-tap Hann smoothing kernel built as `hann_window(35)[1:-1]`, and the voiced
  mask from f0 above 60 Hz sampled at the 300-sample generator hop.

## C runtime: text half (M2, done)

`include/kok_meta.h` + `src/kok_meta.c` parse the model config and the by-name
weight slot table; `include/kok_kernels.h` + `src/kok_kernels.c` are the
primitives (conv1d/ConvTranspose1d with groups and dilation, linear, layer and
instance norm, AdaIN, AdaLayerNorm, Snake, leaky ReLU, the two GELU forms,
softmax, embeddings, LSTM and bidirectional LSTM, and Kokoro's AdainResBlk1d);
`src/kok_text.c` is the text half: ALBERT, text encoder, duration encoder,
prosody with the F0/N heads, and the asr projection. Channel-major [C, T]
throughout, no dynamic allocation (callers pass an arena), no VLAs.

Weights are resolved by name once at bind time so a mismatch fails at startup,
including the element-count check against the config-implied shape.

```sh
nix shell nixpkgs#gcc nixpkgs#gnumake -c make -C kokoro test
```

Gate result (2026-10-13): durations integer-exact on all five sentences, and
every other stage at or under 1.5e-06 relative error against the torch goldens
(d, en, asr at 6e-07 to 1.4e-06; F0 at 7e-07 relative, its absolute error being
larger only because F0 runs to ~300). Arena peak 6 to 12 MB for 93 to 192
token sentences. Two sentences of fixtures are committed as the contract
(`test/fixtures/paradee-8m`, 2.7 MB); the full set regenerates with
`tools/dump_stage_bin.py`.

Blobs are not committed: `tools/export_kokoro_blobs.py` rebuilds them from the
checkpoints into the gitignored `kokoro/model/blobs` (fp32, 32 MB for this
model; the int8 path in M6 will bring that to the 9 MB class).

## C runtime: decoder half (M3, first pass)

`src/kok_istft.c` holds the spectral kernels (the author's ConvSTFT as
convolutions over generated DFT bases, plus a raw real/imaginary variant the
phase lock needs, the Snake resblock, the nearest-neighbour upsampler) and
`src/kok_dec.c` the decoder stack, the istftnet generator and the 2-8 kHz phase
lock. The harmonic source is passed in rather than generated: the RNG path is
not written yet, and the goldens supply it.

Gate state against the torch goldens: durations integer-exact, every text-side
stage and the decoder stack at 1e-06 to 2e-06 relative, the generator output
and the locked waveform at about 1e-03 relative on the final audio (-69 dB,
inaudible). The residual is a phase branch cut, not a logic error: exactly three samples of
the generator's phase feature differ by 2*pi between the C and torch because the
imaginary part lands on opposite sides of +/-0 in the two summation orders
(1e-8-level float noise at a bin where the real part is negative). The noise
convolution is linear in that phase channel, so it smears those three samples
into the ~1.5 percent difference there and the ~1e-3 relative on the audio.
Reproducing torch's summation order bit-for-bit is the only way to remove it and
is not worth doing; the author's own export notes document the same fragility
("float noise in the DC and Nyquist imaginary parts whose random sign flips the
phase feature between +pi and -pi"). The gate therefore allows 2e-3 relative on
the two audio stages and 1e-4 everywhere else.

Arena peak is 73 MB for a 417-frame sentence, which is over what the device
budget will allow (the plan's M7): the lock's 1024-point bases and the
generator's buffers are the obvious targets.

The gate builds and runs with:

```sh
nix shell nixpkgs#gcc nixpkgs#gnumake -c make -C kokoro test
```

`KOK_DUMP_STAGES` in the build (and `KOK_DEBUG_STAGES` for prints) writes the
generator's intermediate stages to /tmp for the bisect harness.

## Layout

`include/` and `src/` hold the runtime (`kok_` prefix, own port header,
caller-owned arenas and stage taps in the style of
`sanotts/mcu/src/snt_piperlite.c`); `tools/` the venv wrapper, golden dumper
and blob converters; `model/` is gitignored working data (checkpoints, the
reference checkout, samples); `test/` the C gates mirroring
`sanotts/mcu/test/`.
