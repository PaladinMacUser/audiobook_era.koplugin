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
model; the int8 path (M6, done) brings that to 16.4 MB, short of the plan's 9 MB
class only because the LSTM weights stay fp32: there is no int8 LSTM kernel).

## On-device G2P (done, one gap)

`kok_text_to_ids` in `src/kok_g2p.c` wraps the plugin's existing espeak-free
misaki lexicon port (`sanotts/nano_lex_g2p.c`) and maps its phoneme string onto
Paradee's vocabulary, a table generated from the model's own config.json by
`tools/gen_vocab.py` (114 of the 178 symbols are reachable). No pronunciation
logic is re-implemented and no new G2P data ships, so the binary grows to about
3.4 MB by linking the 3 MB of lexicon tables. `kok_tts` gained `--sentence`
(text, as against `--ids`) and `--dump-ids`, so both host and device can go from
raw text to a WAV, and the ids can be diffed against the fixture misaki
produced.

Measured on the fixture sentence: 86 ids against misaki's 93, 81.4% identical at
the best alignment, with the whole difference in the one OOV word
("supernovae"). The id gap is the missing fallback, not a mapping error: the
lexicon port has no OOV path, so a word outside it loses its pronunciation where
the Piper path would spell it through espeak. Text to audio end to end is RTF
0.53 on the host.

The fallback was attempted and reverted. A per-word retry that re-tokenised the
sentence by hand and sent only the OOV words through espeak dropped the
punctuation and lost the sentence context the dictionary relies on, taking the
fixture from 81.4% to 18.6% agreement, so the lexicon-only path is what ships.
If the fallback comes back, the shape to use is one whole-sentence espeak call
with misaki's E2M substitution map applied to the result, never a per-word
retry.

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

### Source module and RNG (resolved as far as torch allows)

`src/kok_rng.c` implements the torch-CPU RNG the source needs: SHA-256 sentence
seeding, ATen's mt19937 (init, tempering), `(raw & 0xffffff) * 2^-24` uniforms
and 16-sample float32 Box-Muller normals. Verified against torch in isolation:
the uniform stream is bit-exact, and a 2.25M-element normal draw matches
`torch.rand(9)` followed by `torch.randn(250200, 9)` with zero differing
elements.

`src/kok_source.c` assembles the source: harmonics 1..9 of the upsampled f0,
radians mod 1, the random initial phase on the first timestep, the linear
downsample to frame rate, cumsum in float32 times 2*pi, the linear upsample
back, sin, the voiced mask at 10 Hz, additive noise and the tanh merge. The
deterministic half is confirmed equal to the live module: capturing the model's
own `_f02sine` output gives a bit-identical tensor to the C's sines (max error
0.0), and the frame-rate phase matches to 1e-04.

The one irreducible difference is the noise draw. `torch.randn_like` on the
module's `sine_waves` receives a NON-CONTIGUOUS input (strides
`(2251800, 1, 250200)`, the transposed layout), and ATen then takes its scalar
path rather than the vectorised 16-sample one, consuming the stream in a
different order. The C implements the 16-sample path, which is the one that IS
bit-exact against `torch.rand(9)` followed by `torch.randn(250200, 9)`. The two
streams are statistically identical (mean 0.0002 against 0.0006, std 1.0006
against 1.0002) and both are deterministic, so a sentence renders reproducibly
either way; the audio differs from torch's draw-for-draw in the unvoiced
texture only, which is the same class of difference as changing the seed.

`KOK_DUMP_STAGES` in the build (and `KOK_DEBUG_STAGES` for prints) writes the
generator's intermediate stages to /tmp for the bisect harness.

## Device benchmark (M7, in progress)

`kokoro/tools/device-bench.sh` pushes the static binary and the fp32 blobs
(32.3 MB) to the test Kobo and runs one sentence. The int8 rows below were run
the same way over the `--int8` set (`model/blobs_i8`, 16.4 MB). Same sentence
throughout (93 tokens, 241 frames, 6.03 s of audio), audio checked against the
golden waveform every time:

| build | total | text | decoder | RTF | audio |
|---|---|---|---|---|---|
| first working port | 105.4 s | 14.1 s | 91.2 s | 17.49 | correct |
| + FFT lock + loop order + hand NEON | 43.7 s | 8.7 s | 34.9 s | 7.25 | **broken** (corr -0.004) |
| intrinsics removed, `-O3` | 81.4 s | 12.1 s | 69.2 s | 13.51 | correct |
| + portable 4-way unroll | 79.9 s | 11.9 s | 68.0 s | 13.27 | correct |
| + `restrict` on the kernels | 50.3 s | 8.7 s | 41.6 s | 8.35 | correct (corr 0.992686) |
| + int8 weights (16.4 MB of blobs) | 49.6 s | 8.6 s | 41.0 s | 8.24 | correct |
| + NEON, index arithmetic shared | 42.9 s | 7.8 s | 35.0 s | 7.12 | correct (64.7 dB against the golden) |
| the same, int8 weights | 42.9 s | 7.9 s | 34.9 s | 7.12 | correct (34.0 dB against fp32) |

The int8 row ran before the fixed-source comparison existed, so its audio check
is against the golden and carries the source-phase confound (10.6 dB, an
artefact of the int8 f0 shift). The last two rows were measured with the
harmonic source held fixed (`kok_tts --src`), which isolates the weights: the
int8 agreement with fp32 at 34.0 dB matches the host's 33.7 dB. The onnxruntime
baseline to beat is 2.59, so the optimised port is 2.75x short.

What was learned, in order of usefulness:

- **The loop order was the first big win.** Walking the taps outside the time
  loop, with the valid output range hoisted so the inner loop is branch-free,
  took the decoder from 91 s to 34 s.
- **`restrict` was the second.** Without it gcc must assume the store to `out`
  may alias the load from `x`, so it keeps the scalar order and no
  vectorisation happens; with it the same loops halve again (host 1.185 -> 0.654).
  The host golden gate is what makes this safe to apply: it passes, so no caller
  aliases.
- **Hand-written NEON was faster and wrong, and then right.** An intrinsics
  version of the stride-1 tap loop ran the device at RTF 7.25, but its index
  arithmetic was wrong (a stray `- t0` in the load, bounds computed with
  inverted signs) and the host gate could not see it because the path only
  compiles on ARM; the device audio came back decorrelated. The second attempt
  keeps the loop control, the indices and the scalar tail identical in both
  paths and swaps only the four-wide body, so the host gate exercises the same
  arithmetic the intrinsics run. That version is correct and worth 1.17x
  (8.35 to 7.12).
- **The FFT lock** replaced the reference's DFT-as-convolution transforms
  (512 basis rows times 1024 taps per frame) and also shrank the arena from
  121 MB to 66 MB. On the host it was worth little because the lock was already
  cheap there; it matters more on the device.
- **int8 is a size win, not a speed win.** The kernels accumulate in fp32 over
  the int8 weights and scale once per output row, which is what weight-only
  quantisation means: the arithmetic is unchanged, so the device time is
  unchanged (8.35 to 8.24), and the gain is the blobs, 32.3 MB to 16.4 MB, plus
  memory traffic that the test sentence is too small to show.

Both of those levers are now in, which leaves the 2.75x to the baseline. What
remains is concentrated in the strided paths NEON cannot reach directly: the
ConvTranspose upsampling stages, whose scatter form can be rewritten as gather
passes over the output by residue class (each one a contiguous stride-1
convolution, and so vectorisable), and then the fuller int8 arithmetic with
quantised activations, which is the sanoTTS scheme and a substantially larger
job. A separate small win is an int8 LSTM kernel, which would take the blobs
from 16.4 MB to the plan's 9 MB class.

## Layout

`include/` and `src/` hold the runtime (`kok_` prefix, own port header,
caller-owned arenas and stage taps in the style of
`sanotts/mcu/src/snt_piperlite.c`); `tools/` the venv wrapper, golden dumper
and blob converters; `model/` is gitignored working data (checkpoints, the
reference checkout, samples); `test/` the C gates mirroring
`sanotts/mcu/test/`.
