# Bundled runtime provenance

The Era ZIP reuses unmodified ARMv7 hard-float eSpeak NG, glibc and ALSA
runtime files and wav-play from
[upstream v0.6.3](https://github.com/stradichenko/audiobook.koplugin/releases/tag/v0.6.3).
The input ZIP SHA256 is
`a479409534cea65af60923cab69716a9affcb9357ab160f4bad5c03925d96b9a`.
Piper, sanoTTS, MBROLA databases and audiobook decoders are omitted from this
English speech package. The read-aloud engine works offline.

Runtime source and build recipes:

- [Upstream source at 94d9f6472c383b98d3a8a82ad1696c9721f420c3](https://github.com/stradichenko/audiobook.koplugin/tree/94d9f6472c383b98d3a8a82ad1696c9721f420c3)
- `cross-build-espeak.nix` (eSpeak NG and dependencies; source URLs pinned there)
- `cross-build-wav-play.nix` and `wav-play.c` (WAV player and ALSA dependency)
- `package-for-kobo.sh` in the source repository (collects the runtime closure)
- [eSpeak NG](https://github.com/espeak-ng/espeak-ng), GPL-3.0-or-later
- [alsa-lib](https://github.com/alsa-project/alsa-lib), LGPL-2.1-or-later
- [glibc](https://sourceware.org/git/glibc.git), LGPL-2.1-or-later

Firmware API declarations used by this fork:
[InkView 6.8 header](https://github.com/koreader/pocketbook-inkview/blob/4a3e32c39ed14ac7f2e3ca4f8c66a7d5fbe5e0fe/608/inkview.h)
(`configureAudioOutput`, `get_audio_status`, `AUDIO_OUT_SPEAKER`).
These routing APIs remain available in 6.10; `PlayFile` does not.
