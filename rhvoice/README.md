# RHVoice English voices for PocketBook Era

This is an additional offline English TTS engine. Select **Voice settings →
TTS engine → RHVoice (English voices)**. Installing it does not change
your saved engine, rate, volume, or speaker output. Kristin remains available.
Both **Read aloud from here** and **Read word** use the selected engine.

RHVoice uses statistical parametric synthesis with a voice trained from
recorded speech. SLT is an American English female voice, distinct from
Kristin and PocketBook Julie. This is an alternative voice to compare;
naturalness is subjective, and it is not modern large neural-model speech.

The adapter renders high-quality 24 kHz mono PCM and uses RHVoice's built-in
Sonic rate control, so a saved speed such as 0.5× is supported. The existing
Era speaker player converts it to the firmware's 48 kHz stereo route.
Synthesis runs in a separate process while KOReader remains responsive.
Version 0.7.0 reuses a resident engine across requests (see below). It
now supports one-sentence lookahead and a persistent Era PCM player in 0.8.0. Failed
synthesis stops reading instead of skipping text or changing to eSpeak.
Highlighting uses estimated word timing.

Version 0.6.3.7 adds 350 ms of silence before each RHVoice clip when the
Era built-in speaker is selected. This gives the firmware audio path time
to open before the first phoneme. Speech samples and the selected rate are
unchanged. Highlighting accounts for the lead-in, which is excluded from
speech-duration estimates. Other audio routes keep the original waveform.
This targets clipped clip beginnings; the physical speaker still needs a
listening check.

Development checks under ARM emulation generated a short sentence in about
0.9 seconds and a selected word at 0.5× in about 0.7 seconds, including
startup. These are not measurements on a physical Era.

## Source and build

Pinned inputs:

- [RHVoice 1.18.4, fa9dd196fd2dac3b0bf089a2d80fc8477c2380e3](https://github.com/RHVoice/RHVoice/tree/fa9dd196fd2dac3b0bf089a2d80fc8477c2380e3)
- [English data, 8a2ea34df72a190dae245460aeb20a867d64775c](https://github.com/RHVoice/English/tree/8a2ea34df72a190dae245460aeb20a867d64775c)
- [SLT voice, c17dded68322016aba2868d9c29f11bba14d275f](https://github.com/RHVoice/slt-eng/tree/c17dded68322016aba2868d9c29f11bba14d275f)
- The same pinned Sonic source as `../tempo/README.md`.

RHVoice core source carries LGPL-2.1-or-later notices, its HTS component has
BSD notices, and its data retains upstream licensing and readme files. SLT's
upstream readme identifies the CMU license; its original CMU ARCTIC notice
is reproduced in `LICENSE-CMU-SLT` (also retained by
[MaryTTS's SLT voice](https://github.com/marytts/voice-cmu-slt/blob/master/LICENSE.txt)).
The adapter is GPL-3.0-or-later;
Sonic is Apache-2.0. All corresponding source and notices are included in
`corresponding-source.tar.gz` next to the installed binary. No PocketBook
libraries, Julie voice data, or device license documents are included.

The standalone build replaces one Boost string-expression with equivalent
standard-library code; it does not change speech generation. Only the core,
HTS, C API and Sonic are linked. No desktop audio or network-service library
is required at runtime.

Build with Python 3, Git, CMake, and `g++-arm-linux-gnueabihf`:

```sh
python3 tools/build-rhvoice.py
python3 tools/package-era.py --rhvoice-dir .cache/rhvoice-runtime
```

The first command writes the static ARM binary, English/SLT data, configuration,
revision manifest and corresponding-source archive. `--cc` and `--cxx` can
select alternative cross-compiler executables, including wrappers around
`zig cc/c++ -target arm-linux-musleabihf`. No loader or shared libraries from
the build machine are needed on the Era.

To rebuild offline from the included archive, extract it and run:

```sh
cmake -S source/era/rhvoice -B build \
  -DRHVOICE_SOURCE="$PWD/source/RHVoice" \
  -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=arm \
  -DCMAKE_C_COMPILER=arm-linux-gnueabihf-gcc \
  -DCMAKE_CXX_COMPILER=arm-linux-gnueabihf-g++
cmake --build build --parallel 2
```

Place `rhvoice-slt` alongside the installed `data` folder, or pass `--data`
explicitly when testing outside the plugin. The helper accepts `--input`,
`--output`, `--speed` and `--volume`, writes its WAV atomically, and exits
nonzero if synthesis fails.

## Resident worker (0.7.0)

KOReader now starts one `rhvoice-slt --daemon SOCKET --idle-seconds 120`
process and shares it between continuous reading, word pronunciation and
voice previews. The same RHVoice engine and voice data remain loaded across
requests; speed, volume and speaker lead-in are supplied per request. Ordinary
Stop cancels synthesis without unloading the model. Closing the book,
suspending or leaving KOReader terminates the worker; it also exits after
120 seconds without a request. Synthesis remains sequential inside the worker. Version 0.8.0 adds a bounded
lookahead slot while the current sentence is playing, so synthesis overlaps
playback. Sentence gaps can remain if synthesis is slower than playback. The first request after unloading
still pays the model startup cost.

The private 0700 directory under `/tmp/audiobook-rh-*` contains a Unix datagram
socket, a PID readiness marker and temporary request files. No network port
is opened. Each datagram names a `.req` file with five newline-terminated
fields: input path, output path, speed (0.25–2), volume (0–1), lead-in ms
(0–2000). Input and output must be flat paths in the socket directory. The
worker atomically writes `.req.done` containing `OK`, `CANCELLED`, or `ERR`.
Creating `.req.cancel` interrupts synthesis without terminating the process.
Only complete WAVs reach playback, and a cancelled request cannot deliver
late audio. Missing/dead workers fail promptly; the next request can restart
one. Lua also terminates a worker whose request exceeds 60 seconds.

Volume is applied as linear PCM attenuation after synthesis, bypassing the
voice minimum that prevented zero from muting. The synthesis gain stays at
its normal level, preserving the selected voice and pitch.

The ARM smoke test covers worker reuse, changed SSML speed and PCM volume
(including mute), exact speaker lead-in,
cancellation followed by another request, invalid input, idle exit and restart.

Version 0.7.1 fixes a Preview crash when KOReader has already declared
`sendto` with a `const struct sockaddr *` parameter. The Lua client uses the
standard socket signature and explicitly casts its private Unix-address
struct to that pointer type. The IPC suite runs with both socket-header
load orders, as well as in a fresh Lua VM.

## English voices and prosody (0.8.0)

The bundle also pins BDL (`ea6b944550af6f9b33f2cc5e20f38d3b19087068`),
CLB (`e0aa06cf50334f10844d1a28a72b3fdcf3cdd150`) and Alan
(`bb3fdb15b9c64109f396f56f6155965bfcf89977`) from the official
[bdl-eng](https://github.com/RHVoice/bdl-eng),
[clb-eng](https://github.com/RHVoice/clb-eng), and
[alan-eng](https://github.com/RHVoice/alan-eng) repositories. BDL is American
male, CLB American female, Alan Scottish male. The CMU ARCTIC voice notice
is included as `LICENSE-CMU-VOICES`; all four original models are unmodified.
The corresponding-source archive includes all four pinned voice directories.

The optional three extra request lines are voice (`Slt`, `Bdl`, `Clb`, `Alan`),
pitch (0.85–1.15) and input format (`text` or `ssml`). Old five-line requests
retain SLT, pitch 1 and plain text. One-shot testing also accepts `--voice`,
`--pitch` and `--format`. User/book text is escaped before SSML generation;
pronunciation substitutions are literal, case-insensitive, whole-word and
non-recursive. The engine preserves explicit break duration independently of
speech rate, as verified against generated PCM. Sentence/paragraph additions
are scheduled independently by the reading controller.

`era-player.c` builds a separate small ARM process with `tools/build-era-player.py`.
It uses libasound's public opaque-handle ABI through the bundled loader and
libraries, opens only `hwout_mix` or `tts_sm`, and keeps a nonblocking 48 kHz
stereo PCM open while feeding audio or gap/pause silence. It makes no mixer
calls. Requests use the same private Unix datagram transport, with the input
text file naming a canonical PCM WAV under `/tmp/`. `.playing` acknowledges
the retained lead-in in milliseconds; `.done` acknowledges that the queued
speech passed the PCM delay, not merely that it was sent. `.pause` pauses
advancement, `.cancel` drops and closes PCM. Normal completion keeps it open
for the next sentence; ten idle seconds or TERM closes it. Fresh streams
retain the 350 ms leading silence; warm streams skip precisely those silent
frames. UI stop/book close/suspend/exit terminates the tracked process.

Host fixture tests compare exact speech frames, one-PCM reuse and lifecycle.
ARM tests exercise all voices, pitch, SSML, cancellation, idle restart, and
real ALSA null playback through the packaged loader. Physical Era listening
and latency still require device validation. Highlighting remains estimated.
