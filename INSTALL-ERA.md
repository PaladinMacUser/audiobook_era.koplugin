# PocketBook Era: English read-aloud through the built-in speaker

Target: **original Era (PB700), firmware U700.6.10.3209, KOReader 2026.03**.
This build includes offline English speech with eSpeak NG. Device playback
still needs verification on a physical Era; host tests cannot verify sound.

1. Quit KOReader. Connect the Era to your computer by USB.
2. If an older `audiobook.koplugin` or `audiobook_era.koplugin` is installed,
   move it outside the `plugins` folder as a backup. Keep only one installed
   copy of this plugin.
3. Extract **`audiobook-era-v0.6.3.1.zip`**. Copy its **`audiobook.koplugin`**
   folder into **`applications/koreader/plugins/`** on the Era's USB drive.
   The device path is `/mnt/ext1/applications/koreader/plugins/audiobook.koplugin/`.
   Do not use GitHub's “Source code” ZIP: it lacks the speech runtime.
4. Eject USB, disconnect Bluetooth headphones and USB audio, and start KOReader.
5. Open an English EPUB. Under **Tools → Audiobook Read-Along**, choose
   **Audio output (PocketBook) → Era built-in speaker (firmware audio routing)**.
   This is the default on a fresh installation of an original Era. Select
   **Voice settings → TTS engine → espeak-ng** if an older saved engine setting remains.
6. Long-press a word and tap **Read aloud from here**. Speech starts at that
   word and continues through the book. Use pause or close on the player bar
   to pause or stop. Set **Voice settings → Voice → English** if needed.

No Wi-Fi, Bluetooth speaker, root access, or external TTS service is needed.
The stock PocketBook TTS voice packages are separate from this plugin's voice.

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

Run `python3 tools/package-era.py` (Python 3 and curl). It verifies the pinned
upstream runtime checksum, adds this fork's current source, and writes the
installable ZIP and its SHA256 file under `dist/`. You can reuse a downloaded
runtime with `--runtime-zip /path/to/audiobook-koplugin-v0.6.3.zip`.

Run `LUAJIT=luajit sh dev/run_pocketbook_tests.sh` for the native ABI and engine
regression tests. These require a host C compiler and LuaJIT, not the reader.
Run `python3 dev/smoke-era-runtime.py dist/audiobook-era-v0.6.3.1.zip` with
`qemu-arm` installed to verify the packaged ARM English engine and player.
