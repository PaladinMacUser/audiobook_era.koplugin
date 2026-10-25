-- Host regression tests with a native InkView ABI fixture, no audio hardware.
local ffi = require("ffi")
-- Simulate declarations already loaded by KOReader's PocketBook FFI.
ffi.cdef[[
    typedef struct { int type; char description[32]; } audio_output_info;
    int get_audio_status(audio_output_info *status);
    void pb_test_set(int configured, int status_ok, int output);
    int pb_test_calls(void);
]]
local lib = ffi.load("inkview")
local Audio = dofile("pocketbookaudio.lua")
assert(ffi.sizeof("audio_output_info") == 36)

local checks = 0
local function check(value, message)
    checks = checks + 1
    assert(value, message)
end
check(Audio.isEra({model="PB700"}), "original Era detected")
for _, model in ipairs({"PBEraColor", "PB700K3", "PB710", "PB632", "unknown"}) do
    check(not Audio.isEra({model=model}), "must not enable speaker on " .. model)
end
local a = Audio:new()
lib.pb_test_set(1, 1, 3)
check(a:prepare(), "prepare speaker through real FFI call")
check(lib.pb_test_calls() == 1, "firmware preparation called")
check(a:prepare(), "cached player still checks speaker route")
check(lib.pb_test_calls() == 1, "avoid reconfiguring every sentence")
for _, output in ipairs({0, 1, 2, 4}) do
    lib.pb_test_set(1, 1, output)
    check(not a:prepare(), "reject non-speaker output " .. output)
    check(not a.prepared, "clear prepared state after route change")
end
lib.pb_test_set(0, 1, 3)
check(not a:prepare(), "firmware preparation failure")
lib.pb_test_set(1, 0, 3)
check(not a:prepare(), "status IPC failure")
lib.pb_test_set(1, 1, 3)
check(a:prepare(), "retry after firmware becomes ready")
a:reset()
check(not a.prepared, "stop resets session preparation")

-- Synthetic ALSA fixture. Physical firmware routing remains a device check.
local real_open, real_popen = io.open, io.popen
local files = {
    ["/etc/asound.conf"] = 'pcm.tts_sm { type softvol slave.pcm "hwout_mix" }\n'
        .. 'pcm.hwout_mix { type dmix slave { pcm "hw:Loopback,0" rate 48000 channels 2 } }',
    ["/usr/share/alsa/alsa.conf"] = "# firmware ALSA base config",
    ["/ebrmain/config/device.cfg"] = "bluetooth_device_name=PocketBook",
    ["/sys/class/bluetooth/hci0/address"] = "01:02:03:04:05:06",
}
io.open = function(path, mode)
    if files[path] ~= nil and (not mode or mode == "r") then
        return {read=function() return files[path] end, close=function() end}
    end
    if path:match("^/etc/") or path:match("^/usr/share/alsa/") then return nil end
    return real_open(path, mode)
end
local spawned = {}
io.popen = function(cmd)
    spawned[#spawned+1] = cmd
    local s = cmd == "pwd" and "/tmp/Era Reader's folder" or cmd:find("echo $!", 1, true) and "42424" or ""
    return {read=function() return s end, close=function() end}
end
local function enginePaths()
    return {
        _wav_play_bin = "plugins/audiobook.koplugin/wav-play/wav-play",
        _wav_play_lib = "plugins/audiobook.koplugin/wav-play/lib",
        espeak_linker = "plugins/audiobook.koplugin/espeak-ng/lib/ld-linux-armhf.so.3",
        espeak_lib_path = "plugins/audiobook.koplugin/espeak-ng/lib",
    }
end
local paths = enginePaths()
local command = assert(a:command(paths))
check(command:find("--no-mixer", 1, true), "never change codec mixer")
check(command:find(" -D tts_sm", 1, true), "explicit firmware PCM disables hardware fallback")
check(not command:find("plughw", 1, true), "no physical codec fallback")
check(command:find("'\\''", 1, true), "shell quote paths including apostrophes")
files["/etc/asound.conf"] = 'pcm.hwout_mix { type dmix }'
command = assert(a:command(paths))
check(command:find("-D hwout_mix --rate 48000 --channels 2", 1, true), "dmix fallback converts in software")
files["/etc/asound.conf"] = '# pcm.tts_sm { type hw }'
check(not a:command(paths), "commented-out PCMs must not qualify")
files["/etc/asound.conf"] = 'pcm.tts_sm { type softvol }'
files["/usr/share/alsa/alsa.conf"] = nil
check(not a:command(paths), "reject missing ALSA base configuration")
files["/usr/share/alsa/alsa.conf"] = "base config"
check(not a:command({}), "reject missing bundled player")

-- Load the actual TTSEngine with lightweight KOReader UI shims.
local device = {model="PB700", isPocketBook=function() return true end,
    isAndroid=function() return false end, isKindle=function() return false end,
    isKobo=function() return false end}
local ui = {scheduled={}, shown={}}
function ui:getTime() return 10 end
function ui:scheduleIn(_, fn) self.scheduled[#self.scheduled+1] = fn end
function ui:unschedule() end
function ui:show(msg) self.shown[#self.shown+1] = msg end
package.loaded.device = device
package.loaded.logger = setmetatable({}, {__index=function() return function() end end})
package.loaded["audiobook_gettext"] = function(s) return s end
package.loaded["ui/uimanager"] = ui
package.loaded["ui/time"] = {to_ms=function(s) return s*1000 end}
package.loaded["ui/widget/infomessage"] = {new=function(_, o) return o end}
package.loaded["ffi/util"] = {template=function(s) return s end}
local Engine = dofile("ttsengine.lua")
local settings = {}
local plugin = {getSetting=function(_, k, d) if settings[k] ~= nil then return settings[k] end return d end}
local e = setmetatable(enginePaths(), {__index=Engine})
e.plugin = plugin
check(e:getPocketBookAudioDevice() == "era-speaker", "Era speaker selected by default")
check(e:findAudioPlayer(), "select speaker without soundcard probing or BT")
check(e._pb_speaker_active and e.audio_player_type == "aplay", "speaker uses WAV player")
check(not e._no_real_audio_output, "speaker is real output even without BT")
check(lib.pb_test_calls() == 1, "player discovery does not configure firmware")

e.current_audio_file = "/tmp/audiobook_test.wav"
e._current_audio_duration_ms = 1200
e._socket_clean = true
e.startTimingLoop = function() end
e._startProcessWatcher = function() end
lib.pb_test_set(1, 1, 2)
local function launchCount()
    local n = 0
    for _, cmd in ipairs(spawned) do if cmd:find("echo $!", 1, true) then n = n+1 end end
    return n
end
local launches = launchCount()
check(not e:play(), "BT output must stop speaker-mode playback")
check(launchCount() == launches, "no player launch on wrong output")
check(not e.is_speaking and #ui.shown == 1, "show actionable speaker error")
lib.pb_test_set(1, 1, 3)
check(e:play(), "speaker works without connected Bluetooth despite BT hardware")
check(e.audio_pid == 42424, "real play path launches selected player")
check(e._last_pid_cmd:find(" -D tts_sm", 1, true), "launch keeps strict loopback device")
check(not e._last_pid_cmd:find("bluealsa", 1, true), "speaker does not start Bluetooth output")

-- Exercise the real process watcher: open errors stop, PLAYING can advance.
e._startProcessWatcher = nil
e._isAudioProcessRunning = function() return false end
e.on_fail_callback = function() e.failed = true end
e.onPlaybackComplete = function() e.completed = true end
files["/tmp/.gst_status"] = "wav-play: all devices failed\n"
e:_startProcessWatcher(false)
ui.scheduled[#ui.scheduled]()
check(e.failed and not e.completed, "open failure must never advance silently")
e.failed = false; e.is_speaking = true
files["/tmp/.gst_status"] = "PLAYING\n"
e:_startProcessWatcher(false)
ui.scheduled[#ui.scheduled]()
check(e.completed and not e.failed, "normal PCM completion advances")
settings.pb_alsa_device = "tts_sm"
check(e:getPocketBookAudioDevice() == "tts_sm", "preserve explicit existing ALSA choice")
settings.pb_alsa_device = nil
device.model = "PB632"
check(e:getPocketBookAudioDevice() ~= "era-speaker", "speakerless PocketBooks keep old default")
settings.pb_alsa_device = "era-speaker"
check(not e:findAudioPlayer(), "explicit Era mode cannot open audio on another model")
io.open, io.popen = real_open, real_popen
print("ok - " .. checks .. " PocketBook speaker checks passed (native ABI + engine integration)")
