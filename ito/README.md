# Ito: offline English neural speech for the original PocketBook Era

Select **Ito (offline English neural voice)** in **Voice settings → TTS engine**.
The female English model is an optional addition. RHVoice and the saved reading
engine remain available. The separate gated weights must be copied once into:

```
audiobook.koplugin/ito/ito_female_esp32s3.bin
```

Use the file downloaded after accepting the terms at
https://huggingface.co/lokutor-ai/ito. The tested file is 3,806,656 bytes,
SHA256 `0266caf56597639b8d1d7d4c26e5f689436bc63fddc8797310b6a877389879f5`.
Public plugin artifacts never contain these weights. No network, account or
Python is used during synthesis on the Era.

## Behavior

- One resident private worker retains the model, unloads after 120 idle seconds,
  and shuts down on book close, suspend or exit. Requests can be cancelled.
- The bundled eSpeak NG provides English phonemes, not the audible voice. Ito's
  neural model generates the waveform from the StyleTTS 2 phoneme vocabulary.
- Pronunciation corrections and ebook cleanup run on a synthesis copy. Common
  titles, decimal numbers and dollar amounts are normalized before punctuation
  is restored to the phoneme stream. Quotes follow upstream Ito's convention.
- English word/phoneme separation approximates upstream's documented fallback
  tokenizer; Python, NLTK downloads and serial hardware are unnecessary.
- At most 400 tokens enter the model per chunk. Long input splits at phoneme-word
  boundaries and continues through the final word instead of truncating silently.
  One pathological word longer than the model window fails explicitly.
- Speech rate uses Sonic's pitch-preserving algorithm on native-duration audio;
  volume uses PCM attenuation, including true mute. Unsupported pitch controls
  are not offered for Ito. Sentence and paragraph pauses are shared with RHVoice.
- Single-word speech, previews and continuous reading use the established Era
  clip player and a 350 ms leading guard. The unreliable keep-open PCM mode is
  never used for Ito. One-sentence prefetch and the pronunciation cache work.
- A synthesis failure is reported and retained in diagnostics. It does not
  substitute eSpeak speech or silently skip the failed sentence.

## Source and licensing

Runtime source: https://github.com/lokutor-ai/ito/tree/118a238b3fbb754d3364c52a167f11b9142ca6a2
The portable `itofs.c`/`itofs.h` core is GPLv3. The adapter is GPL-3.0-or-later;
Sonic is Apache-2.0; StyleTTS 2's phoneme symbol table is MIT. The build copies
upstream LICENSE and NOTICE, and includes corresponding source and its recipe.
The separately obtained voice carries upstream CC BY-NC-SA 4.0 and additional
model terms; LICENSE-WEIGHTS and TERMS.md are included for reference.

Build: `python3 tools/build-ito.py` (ARM GCC/G++, CMake). No gated weights are
needed to build the runtime. `tools/package-era.py --ito-dir ...` accepts a
prebuilt directory and whitelists only runtime/source/license files.

Verification with a locally provided model:

```
LUAJIT=luajit python3 dev/test_ito_runtime.py --binary /path/to/ito-speak \
  --model /path/to/ito_female_esp32s3.bin --phonemizer /path/to/phonemize.sh \
  --qemu qemu-arm
```

Tests verify actual ARM synthesis, phonemization, rate, volume, mute, guarded
starts, word synthesis, resident reuse, cancellation, long text, idle and exit.
Emulation timings do not establish real Era latency or subjective quality.
Compare the preview and a paragraph on the device before choosing a daily voice.
