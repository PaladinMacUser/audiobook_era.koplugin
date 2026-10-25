local files = {
    ["/sys/devices/system/cpu/possible"]="0-1",
    ["/reader/espeak-ng/lib/ld-linux-armhf.so.3"]="loader",
}
local real_open = io.open
io.open = function(path, mode)
    if files[path] then return {read=function() return files[path] end,close=function() end} end
    return real_open(path,mode)
end
package.loaded.device = {model="PB700",isPocketBook=function() return true end,isKindle=function() return false end}
package.loaded.logger = setmetatable({}, {__index=function() return function() end end})
package.loaded.audiobook_gettext = function(s) return s end
local ui = {scheduled={}}
function ui:scheduleIn(_,fn) self.scheduled[#self.scheduled+1]=fn end
function ui:show() error("Cancelled synthesis must not show an error") end
package.loaded["ui/uimanager"] = ui
package.loaded["ui/time"] = {}
package.loaded["ui/widget/infomessage"] = {new=function(_,o) return o end}
package.loaded["ffi/util"] = {template=function(s) return s end}
local Engine = dofile("ttsengine.lua")
local Queue = dofile("piperqueue.lua")
local checks = 0
local function check(value,message) checks=checks+1; assert(value,message) end
local settings={}
local plugin={getSetting=function(_,k,d) if settings[k]~=nil then return settings[k] end return d end,
    setSetting=function(_,k,v) settings[k]=v end}
local e=setmetatable({plugin=plugin,sanotts_voices={kristin="/reader/sanotts/voice"}}, {__index=Engine})
check(e:applyEraFastVoiceDefault(e.BACKENDS.PIPER)==e.BACKENDS.SANOTTS, "upgrade from slow Piper selects sanoTTS")
check(settings.sanotts_voice=="kristin" and settings.espeak_only_mode==false, "select the fast English voice")
check(e:applyEraFastVoiceDefault(e.BACKENDS.PIPER)==e.BACKENDS.PIPER, "later explicit user choices are preserved")
settings={}
check(e:applyEraFastVoiceDefault(e.BACKENDS.ESPEAK)==e.BACKENDS.ESPEAK, "keep an explicit espeak preference")
settings={}; e.sanotts_voices={}
check(e:applyEraFastVoiceDefault(e.BACKENDS.PIPER)==e.BACKENDS.PIPER, "no migration without actual voice data")
e.backend=e.BACKENDS.SANOTTS; e.rate=1; e.plugin_dir="/reader"
e.sanotts_server="/reader/sanotts/snt_server"; e.sanotts_voice_dir="/reader/sanotts/voice"
e.sanotts_voice_name="kristin"; e.sanotts_voice_cfg={engine="int8",g2p="nano",espeak_voice="en-us"}
e._piper=Queue:new{engine=e}
local cmd=e._piper:buildBaseCommand()
check(cmd:find('nice %-n 19 "/reader/sanotts/snt_server"'), "launch the static engine directly")
check(cmd:find('--loader "/reader/espeak%-ng/lib/ld%-linux%-armhf.so.3"'), "retain the loader for the optional espeak child")
check(cmd:find("--engine int8",1,true) and cmd:find("--g2p nano",1,true), "use fast inference and in-process pronunciation")

-- A cancelled word's real async synthesis poll must not mutate a newer
-- request's file or call its completion callback after stop().
e._piper.buildCommand=function() return "mock-synth", nil end
local execute, remove = os.execute, os.remove
os.execute=function() return 0 end
os.remove=function() return true end
local completed=0
e.current_audio_file="newer-request.wav"
e:synthesize("old word",function() completed=completed+1 end)
check(#ui.scheduled==1, "neural one-shot schedules a completion poll")
e:stop()
e.current_audio_file="newer-request.wav"
ui.scheduled[1]()
check(completed==0 and e.current_audio_file=="newer-request.wav", "cancelled synthesis cannot overwrite new audio")
io.open,os.execute,os.remove=real_open,execute,remove
print("ok - " .. checks .. " Era fast-TTS checks passed")
