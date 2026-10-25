# PocketBook Era: English read-aloud through the built-in speaker

Target: **original Era (PB700), firmware U700.6.10.3209, KOReader 2026.03**.
Version **0.6.3.8** adds **RHVoice/SLT (American English)** as another offline
engine. It also includes sanoTTS/Kristin, eSpeak NG, pitch-preserving tempo
and **Read word**. Your existing engine choice is retained when upgrading
from the previous Era package; select RHVoice explicitly to try it.

The stock PocketBook **English (American) Julie** engine is not connected
in this build: the public InkView SDK has no speech-synthesis API, and its
firmware-specific entry point has not been verified. Kristin is a separate
bundled engine. It generated a short phrase in about 0.22 seconds under
emulation in the development check; actual Era speed needs a device test.

1. Quit KOReader. Connect the Era to your computer by USB.
2. If an older `audiobook.koplugin` or `audiobook_era.koplugin` is installed,
   move it outside the `plugins` folder as a backup. Keep only one installed
   copy of this plugin.
3. Extract **`audiobook-era-v0.6.3.8.zip`**. Copy its **`audiobook.koplugin`**
   folder into **`applications/koreader/plugins/`** on the Era's USB drive.
   The device path is `/mnt/ext1/applications/koreader/plugins/audiobook.koplugin/`.
   Do not use GitHub's “Source code” ZIP: it lacks the speech runtime.
4. Eject USB, disconnect Bluetooth headphones and USB audio, and start KOReader.
5. Open an English EPUB. Under **Tools → Audiobook Read-Along**, choose
   **Audio output (PocketBook) → Era built-in speaker (firmware audio routing)**.
   This is the default on a fresh installation of an original Era. Select
   **Voice settings → TTS engine → RHVoice (SLT, American English)** to try
   the new engine. Your saved rate, such as 0.5×, and volume still apply.
   To return to Kristin, select **sanoTTS (neural, lightweight)** and choose
   **kristin (English, 700 KB, fast)** under **sanoTTS voice**.
6. Long-press a word and tap **Read aloud from here**. Speech starts at that
   word and continues through the book. Use pause or close on the player bar
   to pause or stop.

No Wi-Fi, Bluetooth speaker, root access, or external TTS service is needed.
The stock PocketBook TTS voice packages are separate from this plugin's voice.

At 1.0×, try **Read word** again after installing this build. Version 0.6.3.8
adds a 350 ms silent speaker lead-in before each RHVoice clip to protect its
beginning while the firmware audio path opens. It does not change the speech
samples or rate, and estimated highlighting accounts for the delay.

## KOReader 2026.03 touch crash on the original Era

Version 0.6.3.8 includes a compatibility guard for
`frontend/device/gesturedetector.lua:295: attempt to index field 'initial_tev'`.
An interrupted touch contact may have no origin when KOReader checks a
two-finger tap. The plugin rejects that invalid two-finger gesture and lets
KOReader perform its normal single-tap fallback and contact cleanup. Valid
gestures still use the original detector. This runs only on PB700 with a
KOReader 2026.03 revision; no core files or firmware are changed.

The uploaded log also contained caught `dictionary-mode.koplugin` errors on
the home screen. Those are a separate plugin issue; this guard does not
change that plugin. The Bluetooth-helper/Piper startup warnings are not the
fatal error identified in the traceback.

## Additional RHVoice engine

SLT is an American English female voice. It uses statistical parametric
synthesis, independently of sanoTTS and Piper, and renders 24 kHz audio.
It is an alternative to compare, not modern large-model neural speech.
Both **Read aloud from here** and **Read word** use the selected engine.
No voice download is needed after installing the complete ZIP.

The development ARM-emulation check generated a short sentence in about
0.9 seconds and a word at half speed in about 0.7 seconds, including startup.
Actual Era performance is unmeasured. RHVoice loads for each request and
does not prefetch in this build, so short gaps between sentences are possible.
Failed synthesis stops reading instead of silently changing the voice or
skipping content. Word timing is estimated. See `rhvoice/README.md`.

## Read just a selected word

Long-press a word, then tap **Read word** in the dictionary popup. The popup
stays open so you can repeat it. The text-selection menu has the same action.
It speaks one selected word and stops, without advancing to the next sentence
or page. With several words selected, it speaks the first one. Choosing
another word, starting read-along, closing the book, or suspending cancels
any previous word request.

## Faster neural speech

Kristin uses a small int8 model and in-process word pronunciation, avoiding
Piper's large ONNX model. It trades some naturalness for speed. The default
package includes Kristin, RHVoice/SLT and eSpeak, with no voice download required. The
larger Piper/Danny alternative can be included by building with `--with-piper`.

### Slower speech without stretching the voice model

If 1.0× speech is too fast, keep **Voice settings → Speech rate** at **0.5×**
or **0.75×**. On the Era, version 0.6.3.8 renders Kristin at its native pace
and changes the completed WAV's tempo with Sonic, preserving pitch. It no
longer doubles the model's phoneme durations for 0.5× playback. The same
processing applies to **Read word**. Your saved speed is retained.

This may reduce the distortion at slow speeds; it does not make Kristin
sound like Julie. Highlighting after tempo conversion is estimated from
the slower clip. Normal 1.0× audio keeps its original waveform and timing.

To investigate reuse of Julie, generate the existing Audiobook bug report.
Its `pb_stock_tts_files` field lists candidate installed voice/library names;
it does not copy voice or license contents. Those firmware details are needed
before a stock-engine helper can be implemented and tested.

## First device check

Before installing, confirm that the stock PocketBook reader speaks through
the internal speaker. After installation, try a short sentence in KOReader,
pause/resume it, stop it, then check the stock reader again. Word highlighting
is estimated; it does not prove sound was produced.

If the firmware reports another audio output, the plugin asks you to select
the speaker. If the player cannot open its audio route, reading stops with an
error instead of advancing silently. Do not switch to `plughw:0` to work
around a speaker error.

For investigation, use **Tools → Audiobook Read-Along → Generate bug report**
after the failed attempt and copy these files before restarting KOReader:

- `/mnt/ext1/audiobook-bug-report-*.txt`
- `/mnt/ext1/applications/koreader/crash.log`

The report includes `pb_speaker_pcm`, `pb_speaker_output` (3 means speaker),
`pb_speaker_prepared`, and `pb_speaker_error`. These show which part needs
adjustment if your firmware differs from the host fixtures.

To remove the plugin, quit KOReader and delete its `audiobook.koplugin` folder,
or restore your backup. This build does not edit the firmware's audio files.

## Building the installable ZIP

Run `python3 tools/package-era.py` (Python 3, Git, curl, CMake and `g++-arm-linux-gnueabihf`). It verifies the pinned
upstream runtime checksum, adds this fork's current source, and writes the
installable ZIP and its SHA256 file under `dist/`. You can reuse a downloaded
runtime with `--runtime-zip /path/to/audiobook-koplugin-v0.6.3.zip`.
Add `--with-piper` to include the larger Piper runtime and Danny voice.
Alternatively pass `--tempo-binary /path/to/static-arm-wav-tempo`; see
`tempo/README.md` for compiler commands and Sonic provenance.
RHVoice source and English/SLT data are fetched at pinned revisions and built
statically. Use `--rhvoice-dir /path/to/runtime` to reuse a runtime built by
`tools/build-rhvoice.py`; the package includes corresponding source.

Run `LUAJIT=luajit sh dev/run_pocketbook_tests.sh` for the native ABI and engine
regression tests. These require a host C compiler and LuaJIT, not the reader.
Run `python3 dev/test_wav_tempo.py` for pitch, duration, word-ending and
malformed-input checks of the tempo converter.
Run `python3 dev/smoke-era-runtime.py dist/audiobook-era-v0.6.3.8.zip` with
`qemu-arm` installed to verify the packaged ARM English engine and player.
