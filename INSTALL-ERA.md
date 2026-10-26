# PocketBook Era: English read-aloud through the built-in speaker

## Ito offline neural voice (0.9.0)

1. Install this plugin ZIP, replacing the previous `audiobook.koplugin` folder.
2. Copy the **`ito_female_esp32s3.bin`** you downloaded from Hugging Face into
   **`audiobook.koplugin/ito/`**. Keep that exact filename. The gated model is
   separate from the public plugin download; it is 3,806,656 bytes.
3. Restart KOReader. Open **Tools → Audiobook Read-Along → Voice settings →
   TTS engine** and try **Preview: Ito (offline English neural voice)**.
4. Select **Ito (offline English neural voice)**, then try **Read word** and
   **Read aloud from here**. Speed and volume work as usual; Read word retains
   its separate pronunciation speed. RHVoice remains available in the engine menu.

Ito requires no Wi-Fi or account after the model is copied. It uses its own
neural waveform generator with the bundled eSpeak phonemizer, keeps its model
loaded between requests, and uses the same guarded speaker clip path as RHVoice.
Pitch controls apply to RHVoice only. Shared pronunciation corrections and pauses
are available under **Ito voice and pronunciation**. See [details](ito/README.md).


Target: **original Era (PB700), firmware U700.6.10.3209, KOReader 2026.03**.
Version **0.8.1** adds SLT/BDL/CLB/Alan English voices, per-voice previews,
one-sentence RHVoice lookahead, a persistent Era speaker stream, gentle pitch,
separate punctuation pauses, pronunciation corrections, and reading presets.
It retains word caching, independent word speed/repeat, diagnostics and the
KOReader 2026.03 Preview crash fix. Saved engine, voice, speed and output
choices remain selected; the default RHVoice voice remains SLT.

The stock PocketBook **English (American) Julie** engine is not connected
in this build: the public InkView SDK has no speech-synthesis API, and its
firmware-specific entry point has not been verified. Kristin is a separate
bundled engine. It generated a short phrase in about 0.22 seconds under
emulation in the development check; actual Era speed needs a device test.

1. Quit KOReader. Connect the Era to your computer by USB.
2. If an older `audiobook.koplugin` or `audiobook_era.koplugin` is installed,
   move it outside the `plugins` folder as a backup. Keep only one installed
   copy of this plugin.
3. Extract **`audiobook-era-v0.9.1.zip`**. Copy its **`audiobook.koplugin`**
   folder into **`applications/koreader/plugins/`** on the Era's USB drive.
   The device path is `/mnt/ext1/applications/koreader/plugins/audiobook.koplugin/`.
   Do not use GitHub's “Source code” ZIP: it lacks the speech runtime.
4. Eject USB, disconnect Bluetooth headphones and USB audio, and start KOReader.
5. Open an English EPUB. Under **Tools → Audiobook Read-Along**, choose
   **Audio output (PocketBook) → Era built-in speaker (firmware audio routing)**.
   This is the default on a fresh installation of an original Era. Select
   **Voice settings → TTS engine → RHVoice (English voices)** to try
   the new engine. Your saved rate, such as 0.5×, and volume still apply.
   To return to Kristin, select **sanoTTS (neural, lightweight)** and choose
   **kristin (English, 700 KB, fast)** under **sanoTTS voice**.
6. Long-press a word and tap **Read aloud from here**. Speech starts at that
   word and continues through the book. Use pause or close on the player bar
   to pause or stop.

No Wi-Fi, Bluetooth speaker, root access, or external TTS service is needed.
The stock PocketBook TTS voice packages are separate from this plugin's voice.

At 1.0×, try **Read word** again after installing this build. Version 0.8.1
adds a 350 ms silent speaker lead-in before each RHVoice clip to protect its
beginning while the firmware audio path opens. It does not change the speech
samples or rate, and estimated highlighting accounts for the delay.

## KOReader 2026.03 touch crash on the original Era

Version 0.8.1 includes a compatibility guard for
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
Actual Era performance is unmeasured. RHVoice now loads once and reuses the
same process until idle for 120 seconds, book close, suspend or KOReader exit.
It pre-synthesizes one upcoming sentence while the current one plays, and
continuous reading can keep the Era speaker stream open between sentences.
Page turns or slow synthesis can still add gaps.
Failed synthesis stops reading instead of silently changing the voice or
skipping content. Word timing is estimated. See `rhvoice/README.md`.

## Read just a selected word

Long-press a word, then tap **Read word** in the dictionary popup. The popup
stays open so you can repeat it. The text-selection menu has the same action.
It speaks one selected word and stops, without advancing to the next sentence
or page. With several words selected, it speaks the first one. Choosing
another word, starting read-along, closing the book, or suspending cancels
any previous word request.

## RHVoice voices, pauses and pronunciation

Open **Voice settings → RHVoice voices and tuning** after selecting RHVoice.
All four voices are included in the complete ZIP, with no separate download:

| Voice | Accent | Gender |
| --- | --- | --- |
| SLT | American English | Female |
| BDL | American English | Male |
| CLB | American English | Female |
| Alan | Scottish English | Male |

Under **English voice**, each selection has its own **Preview** row. A sample
uses your current speed and pitch without changing the saved voice or Repeat
word. **Pitch** ranges from 85–115%; 100% preserves the original voice.
**Comma pause** controls commas, semicolons and non-numeric colons; 0 keeps
natural model pauses. Clock/number separators remain intact. Explicit comma
pauses stay in seconds when reading speed changes. **Added sentence pause**
and **Added paragraph pause** add independent time after the voice's natural
ending. The paragraph setting is used at paragraph boundaries.

**Pronunciation corrections → Add pronunciation correction** lets you enter
a word/name/abbreviation and how it should be spoken (for example, `Dr.` →
`doctor`). Matching ignores English letter case, respects word boundaries,
uses longer phrases first, and does not recursively replace the result.
Corrections apply to both continuous reading and Read word. Edit or remove an
entry in the same menu. Changes invalidate affected pronunciation lookups;
old speech cannot be replayed from a different voice/pitch/correction setup.

**Reading presets** explicitly set both reading and word speeds, RHVoice pitch,
and pauses; they preserve the chosen voice, engine and output. Afterwards,
word speed and reading speed can still be adjusted independently:

| Preset | Reading speed | Word speed | Pitch | Comma | Sentence | Paragraph |
| --- | --- | --- | --- | --- | --- | --- |
| Slow and clear | 0.5× | 0.5× | 100% | 0.25 s | 0.4 s | 1.2 s |
| Everyday reading | 0.75× | 0.75× | 100% | 0.12 s | 0.2 s | 0.8 s |
| Word practice | 0.5× | 0.4× | 95% | 0.3 s | 0.5 s | 1.4 s |

**Pre-synthesize next sentence** and **Keep Era speaker open between
sentences** can be configured separately. Lookahead is on by default; the
persistent speaker is opt-in in 0.8.1 after silence was reported on an Era.
The upgrade resets that option once; later explicit choices are retained.
The default continuous path uses the same established player as words and
previews. An opt-in persistent-player failure before playback acknowledgement
retries the same WAV through that player, retains the failure in diagnostics,
and disables the persistent player for the current session. The lookahead
is bounded to one sentence on the current page and does not alter playing
audio or silently fall back to another voice. The continuous player only
opens the firmware's selected `hwout_mix`/`tts_sm` route at 48 kHz stereo;
it never changes hardware mixer switches. It keeps PCM flowing with silence
through sentence gaps and pause/resume. Only an already-open stream skips
the clip's wake-up guard. Stop, suspend, book close and exit release it;
it also exits after ten idle seconds. Words/previews keep the existing clip
player and guard. Page transitions can still have a gap.

Word highlighting uses estimated timing. Long punctuation pauses or spoken
expansions may reduce its accuracy. These changes do not modify firmware
or KOReader core files; try previews and a paragraph on the Era after updating.

## Word replay, speed and cache

Use **Read word** in the dictionary or selection menu. Select the same word
again to replay its cached pronunciation. **Repeat last word** menu entries
have been removed.
The pronunciation speed in that submenu starts at your current reading
speed on first upgrade and is independent thereafter. Continuous reading
keeps its own **Voice settings → Speech rate**.

Completed word clips are cached on disk, so repeated words skip synthesis,
including after a KOReader restart. The speaker still needs its protective
350 ms lead-in. The cache holds at most **200 clips / 20 MiB**, evicting the
least recently used clips. Word, engine, voice, speed, volume, output route
and relevant model settings determine the cache entry; changing them generates
a new pronunciation. Interrupted or failed clips are not cached. Playback
uses a copy so stopping cannot delete cached audio.

The cache is under KOReader's data directory, `cache/audiobook-words`.
**Word pronunciation → Cache word pronunciations** disables caching;
**Clear pronunciation cache** removes stored clips. Ordinary stop keeps
RHVoice loaded for the next request; the first clip after idle unload still
needs model startup.

## Compare voices and test audio

Open **Voice settings → TTS engine** and tap the **Preview** row immediately
below an installed engine. The same short English sample plays using your
reading speed and chosen output. Previews stop current playback and do not
change the saved reading engine, reading speed or last word for Repeat.

Open **Tools → Audiobook Read-Along → Audio diagnostics and test → Test
audio output** for one spoken test sentence. **Audio status and latest
failure** shows the selected engine, last playback engine, output route,
player/PCM, resident-worker state, cache counts and test result. Completing
playback reports that the player finished; listen to confirm the sound.
**Stop test or preview** cancels it.

The latest synthesis/output failure is saved immediately and survives a
restart or later successful playback. It contains engine/output metadata
and an error message, without book text. **Clear saved audio failure**
removes it explicitly. These diagnostics are included in **Generate bug
report** as well.

## Faster neural speech

Kristin uses a small int8 model and in-process word pronunciation, avoiding
Piper's large ONNX model. It trades some naturalness for speed. The default
package includes Kristin, RHVoice/SLT and eSpeak, with no voice download required. The
larger Piper/Danny alternative can be included by building with `--with-piper`.

### Slower speech without stretching the voice model

If 1.0× speech is too fast, keep **Voice settings → Speech rate** at **0.5×**
or **0.75×**. On the Era, version 0.8.1 renders Kristin at its native pace
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
Run `python3 dev/smoke-era-runtime.py dist/audiobook-era-v0.9.1.zip` with
`qemu-arm` installed to verify the packaged ARM English engine and player.

In 0.8.1, continuous-reading speed and volume changes regenerate the current
sentence. If paused, new settings apply when you press play. RHVoice volume
is applied directly to PCM, including true mute. **Read word** uses its
separate **Word pronunciation → Pronunciation speed** setting.

## Natural English phrasing (0.8.2)

Enabled by default in **Voice settings → RHVoice voices and tuning**. This
keeps English titles/initials with the following name and closing quotes with
their sentence, avoiding separate clips such as “Dr.” then “Brown arrived.”
The synthesis copy normalizes curly quotes, ligatures (ﬁ → fi), soft hyphens,
joiners and non-breaking spaces before applying your pronunciation corrections.
Book text is unchanged and source positions remain valid for highlighting.
Other engines retain their existing sentence splitting. Toggle the option off
to compare, then use a voice preview or **Read aloud from here** again.

This build retains maximum-quality 24 kHz RHVoice synthesis. It does not add
a neural model or change saved voice, speed, pitch, pauses, volume or output.
Speech quality is subjective; compare a paragraph on the actual Era speaker.

Version 0.9.1 offers volume levels from 5% through 100% in 5% increments
in both volume settings menus. Keep your existing Ito model in the `ito` folder
when updating the plugin.
