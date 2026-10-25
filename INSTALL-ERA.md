# PocketBook Era: English read-aloud through the built-in speaker

Target: **original Era (PB700), firmware U700.6.10.3209, KOReader 2026.03**.
Version **0.6.3.5** bundles the **sanoTTS Kristin English neural voice** for
faster synthesis, plus eSpeak NG and pitch-preserving speech tempo. Speaker playback and highlighting fixes
are retained. On first launch this build switches an existing Era Piper
setup to sanoTTS/Kristin once; later engine choices remain yours.

The stock PocketBook **English (American) Julie** engine is not connected
in this build: the public InkView SDK has no speech-synthesis API, and its
firmware-specific entry point has not been verified. Kristin is a separate
bundled engine. It generated a short phrase in about 0.22 seconds under
emulation in the development check; actual Era speed needs a device test.

1. Quit KOReader. Connect the Era to your computer by USB.
2. If an older `audiobook.koplugin` or `audiobook_era.koplugin` is installed,
   move it outside the `plugins` folder as a backup. Keep only one installed
   copy of this plugin.
3. Extract **`audiobook-era-v0.6.3.5.zip`**. Copy its **`audiobook.koplugin`**
   folder into **`applications/koreader/plugins/`** on the Era's USB drive.
   The device path is `/mnt/ext1/applications/koreader/plugins/audiobook.koplugin/`.
   Do not use GitHub's “Source code” ZIP: it lacks the speech runtime.
4. Eject USB, disconnect Bluetooth headphones and USB audio, and start KOReader.
5. Open an English EPUB. Under **Tools → Audiobook Read-Along**, choose
   **Audio output (PocketBook) → Era built-in speaker (firmware audio routing)**.
   This is the default on a fresh installation of an original Era. Select
   **Voice settings → TTS engine → sanoTTS (neural, lightweight)**.
   Under **sanoTTS voice**, choose **kristin (English, 700 KB, fast)** if needed.
6. Long-press a word and tap **Read aloud from here**. Speech starts at that
   word and continues through the book. Use pause or close on the player bar
   to pause or stop.

No Wi-Fi, Bluetooth speaker, root access, or external TTS service is needed.
The stock PocketBook TTS voice packages are separate from this plugin's voice.

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
package includes Kristin and eSpeak, with no voice download required. The
larger Piper/Danny alternative can be included by building with `--with-piper`.

### Slower speech without stretching the voice model

If 1.0× speech is too fast, keep **Voice settings → Speech rate** at **0.5×**
or **0.75×**. On the Era, version 0.6.3.5 renders Kristin at its native pace
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

Run `python3 tools/package-era.py` (Python 3, curl and `gcc-arm-linux-gnueabihf`). It verifies the pinned
upstream runtime checksum, adds this fork's current source, and writes the
installable ZIP and its SHA256 file under `dist/`. You can reuse a downloaded
runtime with `--runtime-zip /path/to/audiobook-koplugin-v0.6.3.zip`.
Add `--with-piper` to include the larger Piper runtime and Danny voice.
Alternatively pass `--tempo-binary /path/to/static-arm-wav-tempo`; see
`tempo/README.md` for compiler commands and Sonic provenance.

Run `LUAJIT=luajit sh dev/run_pocketbook_tests.sh` for the native ABI and engine
regression tests. These require a host C compiler and LuaJIT, not the reader.
Run `python3 dev/test_wav_tempo.py` for pitch, duration, word-ending and
malformed-input checks of the tempo converter.
Run `python3 dev/smoke-era-runtime.py dist/audiobook-era-v0.6.3.5.zip` with
`qemu-arm` installed to verify the packaged ARM English engine and player.
