# RHVoice / SLT for PocketBook Era

This is an additional offline English TTS engine. Select **Voice settings →
TTS engine → RHVoice (SLT, American English)**. Installing it does not change
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
The engine is loaded for each request; this version does not prefetch with
RHVoice. A short pause between sentences is therefore possible. Failed
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
