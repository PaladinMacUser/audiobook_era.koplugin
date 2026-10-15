# Tier 2: owned audio output (`audioout` daemon)

Roadmap tier 2 from the 2026-10-13 infrastructure review: replace the
shell-out audio output layer with one native daemon that owns the output
session, plays continuously, reports a REAL playback position, and recovers
from device loss. First target: Kobo MTK (the test device). Kindle, Android
and PocketBook keep their existing paths until later increments.

## What is wrong today (evidence)

- The MTK persistent pipeline exists twice, in `ttsengine.lua` (6277-6684)
  and `mediaengine.lua` (2193-2523), with different feeders, control-file
  schemas, retry policies and completion models.
- There is no playback clock anywhere on the path. Completion is open loop:
  `expected_duration_ms + pipe_buffer_delay_ms (370-1500) + fudge (150/500)`
  against wall-clock elapsed minus paused time. Highlight sync subtracts a
  hand-tuned latency: 300 ms (persistent pipeline), 500/1500 ms (BT warm or
  cold), 3000 ms fallback. Pipeline restarts are triggered by rate and volume
  changes and cost up to 3 s (socket wait 5 x 200 ms + gst_pid wait 3 s).
- Feeding depends on shell constructs (`dd` slicing, control files, PID
  files, `.done` markers) that cannot answer "what is the sink playing now".

## Architecture decisions

1. **In-process daemon, dlopen GStreamer.** `audioout/audioout.c` runs the
   GStreamer pipeline itself (dlopen the device's `libgstreamer-1.0.so.0`,
   like `kindle/gst-play.c`, which binds ~9 symbols by name and has no
   link-time or header dependency at runtime). The device has GStreamer
   1.14.4; 0.10 is out of scope for MTK.
2. **Pipeline topology unchanged** from the proven one:
   `filesrc ! rawaudioparse ! audioconvert ! audioresample !
   audio/x-raw,format=S16LE,rate=48000,channels=2 ! queue ! <sink>
   sync=false`. The daemon replaces the shell wrapper, not the pipeline.
3. **Clock from a sink-pad probe** (`gst_pad_add_probe`,
   `GST_PAD_PROBE_TYPE_BUFFER`), counting bytes accepted by the sink.
   Position = accepted_output_bytes / (2 ch * 2 B * 48000). This measures
   "handed to the sink", which tracks the real A2DP pacing via queue
   backpressure; the fixed sink-to-air offset is small and calibrated once.
   `gst_element_query_position` and `GST_QUERY_LATENCY` are measured too
   (M0) but not relied on, since the vendor sink is a black box.
4. **Transport: AF_UNIX stream socket, line protocol** (`/tmp/audiobook_audioout.sock`),
   replacing the control-dir scheme. Bidirectional is required for position
   queries; the Lua side already has an FFI AF_UNIX client to copy
   (`mediaengine.lua:21-28`). Errors and completion events are pushed as
   JSON lines on the same socket.
5. **Build: dynamic glibc under the bundled loader** (the
   `kindle/gst-play` compat pattern), because the daemon dlopens the
   device's GStreamer and Kobo system glibc is 2.11-2.19. The loader and
   libs are already in the deployed bundle (`espeak-ng/lib/`).

## M1 status (2026-10-14)

The daemon and its Lua client exist and pass the full protocol suite
(14 checks) both on the host (x86, fakesink) and on the MTK test Kobo
(ARM, fakesink):

- `audioout.c` (repo root): persistent pipeline owner. Sink-pad-probe clock
  with the M0 interpolation model, clip queue (8 entries, gapless, gap
  silence fed between clips), pause/resume/stop/flush, idle keepalive
  silence (replaces the 120 s keepalive WAV), live volume via g_object_set,
  lazy pipeline build, AF_UNIX line-JSON protocol, status polling with
  done_count for deterministic completion.
- `audioout.lua` (plugin root): KOReader-side client, mpv-style
  connect-per-command FFI transport with SO_RCVTIMEO; spawn/kill lifecycle.
- Tests: `dev/audioout/daemon_test.lua` (the 14 checks, runs anywhere
  luajit runs), `dev/audioout/host-test.sh` (x86 + fakesink),
  `dev/audioout/daemon-run.sh` (deploy to the test Kobo and run).
- Build: `dev/audioout/build-daemon.sh` (Jessie 2.19 sysroot, bare run) and
  `build-host.sh` (x86). Nix shell gotchas recorded in the scripts: the
  plugin-path variable is `GST_PLUGIN_SYSTEM_PATH_1_0` (the nix shell
  presets it to a bogus /usr/lib default), `.out` is a separate store path
  from the default attribute, and `glibconfig.h` lives in `glib.out`.

Measured on device: clip end detected at pos=30091 ms for a 30.0 s clip;
the keepalive streams a queued-next clip at its true stream offset
(span [33250..63250] after 3.25 s of idle silence).

Known behavior, not yet handled: with no A2DP peer connected, the vendor
sink stalls ~26 s at pipeline init retrying the A2DP control socket (the
old gst-launch path stalls the same way; the MTK_FATAL_ERRORS watch does
not match these messages). Mitigations in place: the daemon builds its
pipeline lazily on the first play, so startup never blocks; BT routing
decisions stay in btmanager/ttsengine, which already refuse to route
without a connected device.

Remaining for M1: the audible MTK run (needs a BT device connected to the
test unit), ttsengine wiring behind a default-off setting, packaging
(package-for-kobo.sh, release.yml assertions, .gitignore for dev/audioout
build output), and the orphan-sweep entry.

## Sync wiring (the Tier 1 + Tier 2 composition)

SyncController's word tick no longer guesses latency on the daemon path:
when `tts_engine._audioout_active`, the tick takes the daemon's
`status().pos_ms` (probe-measured, interpolated, capped at real audio end)
minus the clip's `start_ms` from the play response, and skips the whole
GStreamer-PLAYING anchor dance and its latency lock (100/300/500/1500/3000
ms). Before the first buffer renders, pos is 0 and the highlight simply
waits: the anchor is self-arming. The legacy wall-clock path is untouched
for every other backend. The read-along bar and read-from-here still use
the wall clock (polish, not correctness: the daemon offset is within the
old lock's error band).

## Increments

| Step | Scope | Ships |
|---|---|---|
| M0 | Clock spike on device: does a sink-pad probe give a usable clock from `mtkbtmwrpcaudiosink`? (this directory) | A number, not code |
| M1 | `audioout` daemon (MTK, WAV/raw PCM clips, queue, pause via feed-stop + silence, real position, deterministic completion) + `audioout.lua` client + TTS path wired | TTS read-along on MTK with a real clock |
| M2 | MediaEngine wired (ffmpeg decodes to raw PCM piped into the daemon), seek support | audiobook playback on MTK |
| M3 | Delete both duplicated pipeline implementations; orphan sweep knows the daemon; bugreport/generate-report sections | the code reduction |
| M4 | BlueZ Kobo (ALSA/BlueALSA output inside the same daemon, reusing `wav-play.c`'s ALSA block) | second platform |
| later | Kindle (mixersink via dlopen), PocketBook, Android | - |

## M0 spike design

`spike_clock.c`: dlopens the device GStreamer, builds the production
pipeline against a raw PCM file, adds a buffer probe on the sink pad, and
prints every 250 ms: monotonic time, probe buffers/bytes, derived position,
`gst_element_query_position` (pipeline and sink) and `GST_QUERY_LATENCY`.
Optional `--pause-at MS` freezes the pipeline for 2 s to verify the clock
freezes with it. Runs against `mtkbtmwrpcaudiosink` (the real question) and,
as mechanism controls, `alsasink` and `fakesink`.

## M0 results (2026-10-14, MTK test Kobo, fw 4.45, GStreamer 1.14.4)

The clock is real and the architecture holds. Measured:

- **Sink-pad probe works.** Byte-exact acceptance counter on
  `mtkbtmwrpcaudiosink`; the 30 s clip reached probe_pos 29999 ms and EOS.
- **`gst_element_query_position` works on the vendor sink** and returns the
  RENDERED position: in steady state it equals the probe position minus one
  buffer (probe 10401 ms / query 8916 ms, buffer 1485 ms), and 0 while the
  first buffer renders.
- **Pause freezes the clock** (pause_frozen=1; bytes identical across the
  2 s PAUSED hold).
- **EOS is a deterministic end-of-audio signal**: it fires when the sink
  renders the last buffer, not when the feeder finishes.
- **Delivery is buffer-granular:** the sink accepts ~286 KB buffers
  (1.49 s of audio at the 48 kHz stereo output caps), one per render
  period, real-time paced by the A2DP link (`bt_a2dp_hw: adev_open_output`
  in the log; a device was connected). After a warm-up phase (audioresample
  CPU) arrival is exactly one buffer per ~1.5 s.
- **The device GStreamer is a stripped build:** `gst_state_get_name` and
  `gst_element_query_latency` are not exported. The daemon must soft-bind
  every optional symbol and hard-require only the working core (gst_init,
  gst_parse_launch, gst_element_set_state, gst_element_get_bus, gst_bus_poll,
  gst_message_parse_error, gst_object_unref, gst_bin_get_by_name,
  gst_element_get_static_pad, gst_pad_add_probe, gst_buffer_get_size,
  gst_element_query_position).

### The clock model (verified by the data)

Stamp each probe arrival with the monotonic time. With buffer i accepted at
wall time W_i at stream offset S_i (cumulative accepted bytes), the sink
begins rendering buffer i at W_i, so for W_i <= T < W_(i+1):

    rendered(T) = S_i + (T - W_i)

This holds for the first buffer too (query reported 0 during its render).
Resolution is the A2DP pacing jitter, tens of ms — an order below the
word-highlight need. `query_position` serves as a cross-check log line.

### Build technique (also new, and reusable)

The first approach (nix-glibc binary under the bundled loader, device
GStreamer dlopened) dies on the device's own `librt.so.1` requiring
`GLIBC_PRIVATE` pthread symbols the bundled 2.42 does not have. The working
recipe matches the device's actual libc (2.19, crosstool-NG):

- `dev/audioout/fetch-sysroot.sh` unpacks Debian Jessie armhf libc6,
  libc6-dev, linux-libc-dev (same source as build-bluealsa-compat.sh) into
  `dev/audioout/.sysroot` (not committed), and rewrites `libdl.so` as a
  linker script (the deb's absolute symlink breaks host-side resolution).
- `dev/audioout/build-spike.sh` compiles with the nixpkgs cross-cc OUTSIDE
  the sandbox (`nix shell ... -c`), `-nostdinc` with an explicit include
  order, `--sysroot` + `-B` + `-L` so crt1.o and libc come from the
  sysroot, `--dynamic-linker=/lib/ld-linux-armhf.so.3`. Result: interpreter
  = device loader, max GLIBC symbol 2.17. GStreamer/glib headers come from
  the nix cross store (declarations only).
- A nix-build derivation of the same does not work: the cc wrapper appends
  its own `--sysroot` after the user's, which breaks the Jessie libc.so
  script's absolute paths. This mirrors build-bluealsa-compat.sh building
  outside nix.

## Before/after metrics (already instrumented)

- `PiperQueue: RTF sample`, `BATCH READY in <ms> ms`
- `SyncController: Piper prefetch arrived ... after <s>`
- `SyncController: Scheduling next sentence in <s>`
- `play() pre-launch took <ms>` / `Feeder done in <ms>`
- New in M1: a sync-accuracy log line comparing reported position against
  the expected sentence start (real timing from the Tier 1 sidecar), and
  restart counts (should drop to ~0 outside error recovery).

## Risks

- The vendor sink is a black box: probes are passive, but M0 exists because
  nothing about it is guaranteed. Fallback clock: bytes-written minus
  measured backlog.
- dlopen of the device GStreamer from a process running under the bundled
  glibc 2.42 loader: proven pattern on Kindle (`kindle/gst-play` compat
  build), still verified first on Kobo in M0.
- Pause must keep feeding silence to hold the A2DP link (1-2 s idle
  timeout), same as today, but inside the daemon.
- KOReader suspend: the daemon is added to the startup orphan sweep
  (`main.lua:_killOrphanProcessesFromPreviousSession`) like gst-launch.
