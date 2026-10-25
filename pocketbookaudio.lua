-- PocketBook Era speaker output. InkView configures the firmware redirector;
-- wav-play writes to its named loopback PCM, never to the physical codec.
local ffi = require("ffi")

-- Use a private struct name: KOReader may already declare audio_output_info.
pcall(ffi.cdef, [[
    typedef struct { int type; char description[32]; } audiobook_pb_audio_status;
]])
pcall(ffi.cdef, "int configureAudioOutput(void);")
pcall(ffi.cdef, "int get_audio_status(audiobook_pb_audio_status *status);")

local Audio = {}
Audio.__index = Audio

function Audio.isEra(device)
    -- KOReader 2026.03 identifies the original Era as PB700. Do not extend
    -- this default to Era Color, Era Lite, or speakerless PocketBooks.
    return device.model == "PB700"
end

local function readFile(path)
    local f = io.open(path, "r")
    if not f then return nil end
    local s = f:read("*a")
    f:close()
    return s
end

local function quote(s)
    return "'" .. s:gsub("'", "'\\''") .. "'"
end

local function absolute(path)
    if path:sub(1, 1) == "/" then return path end
    local h = io.popen("pwd")
    local cwd = h and h:read("*l")
    if h then h:close() end
    assert(cwd and cwd ~= "", "Cannot resolve the plugin directory")
    return cwd .. "/" .. path
end

function Audio:new()
    return setmetatable({}, self)
end

-- Detection has no playback side effects. In particular, do not call the
-- potentially interactive configureAudioOutput() during plugin startup.
function Audio:command(engine)
    self.error = nil
    if not engine._wav_play_bin then
        self.error = "The bundled wav-play player is missing. Install the Era release ZIP."
        return nil
    end
    local conf = readFile("/etc/asound.conf") or readFile("/usr/share/alsa/asound.conf") or ""
    conf = conf:gsub("#[^\n]*", "")
    local function declared(name)
        return conf:find("pcm%." .. name .. "%s*{")
            or conf:find('pcm%."' .. name .. '"%s*{')
    end
    self.has_tts_sm = declared("tts_sm") ~= nil
    self.has_hwout_mix = declared("hwout_mix") ~= nil
    -- Both names are firmware loopback PCMs. A non-default -D also makes
    -- wav-play's device selection strict: no plughw:0 / hw:0 fallback.
    self.pcm = self.has_tts_sm and "tts_sm" or self.has_hwout_mix and "hwout_mix" or nil
    if not self.pcm then
        self.error = "The firmware's tts_sm / hwout_mix audio route is missing. Generate a bug report."
        return nil
    end
    local alsa_conf
    for _, p in ipairs({"/usr/share/alsa/alsa.conf", "/etc/alsa/alsa.conf"}) do
        if readFile(p) then alsa_conf = p; break end
    end
    if not alsa_conf then
        self.error = "The firmware's ALSA base configuration is missing. Generate a bug report."
        return nil
    end
    local cmd = quote(absolute(engine._wav_play_bin))
    if engine.espeak_linker and engine._wav_play_lib and engine.espeak_lib_path then
        local libs = absolute(engine._wav_play_lib) .. ":" .. absolute(engine.espeak_lib_path)
            .. ":/usr/lib:/lib"
        cmd = quote(absolute(engine.espeak_linker)) .. " --library-path " .. quote(libs) .. " " .. cmd
    end
    -- Override the Nix paths compiled into the bundled libasound. The
    -- firmware's base config includes its device-specific /etc/asound.conf.
    cmd = "ALSA_CONFIG_PATH=" .. quote(alsa_conf)
        .. " ALSA_PLUGIN_DIR='/usr/lib/alsa-lib' " .. cmd .. " --no-mixer"
    engine._wav_play_base_cmd = cmd
    engine._wav_play_device = self.pcm
    engine._pb_has_tts_sm = self.has_tts_sm
    engine._pb_has_hwout_mix = self.has_hwout_mix
    engine._wav_play_force_prefix = nil
    engine._wav_play_retry_stage = 0
    cmd = cmd .. " -D " .. self.pcm
    if self.pcm == "hwout_mix" then cmd = cmd .. " --rate 48000 --channels 2" end
    engine._wav_play_cmd = cmd
    return cmd
end

function Audio:prepare()
    local function fail(message)
        self.error = message
        self.prepared = false
        return false, message
    end
    if not self.lib then
        local ok, lib = pcall(ffi.load, "inkview")
        if not ok then return fail("Cannot load the PocketBook audio routing library.") end
        -- Resolve the functions before calling either one. PlayFile is
        -- deliberately unused: firmware 6.10 removed that API.
        if not pcall(function() return lib.configureAudioOutput, lib.get_audio_status end) then
            return fail("This firmware does not expose the PocketBook audio routing API.")
        end
        self.lib = lib
    end
    if not self.prepared then
        local ok, result = pcall(self.lib.configureAudioOutput)
        if not ok or result ~= 1 then
            return fail("PocketBook could not prepare an audio output. Check speaker playback in the stock reader.")
        end
    end
    -- Check on EVERY sentence, including cached-player playback. Never
    -- silently follow a firmware route change to Bluetooth or a USB DAC.
    local status = ffi.new("audiobook_pb_audio_status[1]")
    local ok, result = pcall(self.lib.get_audio_status, ffi.cast("void *", status))
    self.output = ok and result == 1 and tonumber(status[0].type) or nil
    if not self.output then return fail("PocketBook did not report its active audio output.") end
    if self.output ~= 3 then -- AUDIO_OUT_SPEAKER, inkview.h 6.8+
        return fail("The built-in speaker is not the active PocketBook output. Disconnect Bluetooth headphones and USB audio, select the speaker in PocketBook, then try again.")
    end
    self.prepared = true
    self.error = nil
    return true
end

function Audio:reset()
    self.prepared = false
end

return Audio
