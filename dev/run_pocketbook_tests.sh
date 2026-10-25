#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT HUP INT TERM
# Native ABI fixture: these signatures match inkview.h 6.8 / 6.10.
cat > "$test_dir/inkview.c" <<'EOF'
typedef struct { int type; char description[32]; } audio_output_info;
static int configured = 1, status_ok = 1, output = 3, calls;
void pb_test_set(int c, int s, int o) { configured=c; status_ok=s; output=o; calls=0; }
int pb_test_calls(void) { return calls; }
int configureAudioOutput(void) { ++calls; return configured; }
int get_audio_status(audio_output_info *s) { s->type=output; return status_ok; }
EOF
${CC:-cc} -Wall -Wextra -Werror -shared -fPIC "$test_dir/inkview.c" -o "$test_dir/libinkview.so"
LD_LIBRARY_PATH="$test_dir${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" "${LUAJIT:-luajit}" dev/test_pocketbook_audio.lua
"${LUAJIT:-luajit}" dev/test_word_highlighting.lua
"${LUAJIT:-luajit}" dev/test_era_piper.lua
"${LUAJIT:-luajit}" dev/test_word_reader.lua
"${LUAJIT:-luajit}" dev/test_era_fast_tts.lua
"${LUAJIT:-luajit}" dev/test_rhvoice.lua
"${LUAJIT:-luajit}" dev/test_gesture_guard.lua
"${LUAJIT:-luajit}" dev/test_word_cache.lua
"${LUAJIT:-luajit}" dev/test_rhvoice_client.lua
"${LUAJIT:-luajit}" dev/test_rhvoice_client.lua koreader-first
"${LUAJIT:-luajit}" dev/test_rhvoice_client.lua plugin-first
"${LUAJIT:-luajit}" dev/test_speech_tools.lua
"${LUAJIT:-luajit}" dev/test_rhvoice_settings.lua
${CC:-cc} -std=c99 -O2 -Wall -Wextra -Werror rhvoice/era-player.c -ldl -o "$test_dir/era-player"
${CC:-cc} -shared -fPIC -Wall -Wextra -Werror dev/fixtures/alsa_pcm.c -o "$test_dir/libasound.so.2"
LD_LIBRARY_PATH="$test_dir${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" ERA_PLAYER_TEST_BIN="$test_dir/era-player" ERA_PCM_EVENTS="$test_dir/pcm.events" ERA_PCM_CAPTURE="$test_dir/pcm.capture" "${LUAJIT:-luajit}" dev/test_era_player_client.lua
