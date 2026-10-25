-- Piper selection, memory limits and delayed playback on an Era.
local real_open = io.open
io.open = function(path, mode)
    if path == "/sys/devices/system/cpu/possible" then
        return {read=function() return "0-1" end, close=function() end}
    end
    return real_open(path, mode)
end
local device = {model="PB700", isPocketBook=function() return true end,
    isKindle=function() return false end, isAndroid=function() return false end}
local ui = {scheduled={},shown={}}
function ui:getTime() return 10 end
function ui:scheduleIn(_, fn) self.scheduled[#self.scheduled+1] = fn end
function ui:show(msg) self.shown[#self.shown+1] = msg end
package.loaded.device = device
package.loaded.logger = setmetatable({}, {__index=function() return function() end end})
package.loaded.audiobook_gettext = function(s) return s end
package.loaded["ui/uimanager"] = ui
package.loaded["ui/time"] = {to_s=function(s) return s end, to_ms=function(s) return s*1000 end}
package.loaded["ui/event"] = {}
package.loaded["ui/widget/infomessage"] = {new=function(_, o) return o end}
package.loaded["ffi/util"] = {template=function(s) return s end}
local Engine = dofile("ttsengine.lua")
local PiperQueue = dofile("piperqueue.lua")
local Sync = dofile("synccontroller.lua")
local checks = 0
local function check(value, message)
    checks = checks+1
    assert(value, message)
end
local settings = {}
local plugin = {getSetting=function(_, k, d)
    if settings[k] ~= nil then return settings[k] end
    return d
end}
local e = setmetatable({plugin=plugin,backend=Engine.BACKENDS.PIPER}, {__index=Engine})
check(e:preferPiperVoice(), "Era keeps the explicitly selected neural voice by default")
check(e:_piperLowResource(), "Era synthesizes one sentence per batch by default")
check(e:_piperAggressiveSplit(), "Era splits long synthesis chunks by default")
local q = PiperQueue:new{engine=e}
for _ = 1, 8 do check(q:_pickServer() == 1, "Era uses one resident model on two cores") end
check(q:_batchSize() == 1, "Era queue dispatches one sentence at a time")

settings.piper_keep_voice = false
settings.piper_low_resource = false
settings.piper_aggressive_split = false
check(not e:preferPiperVoice() and not e:_piperLowResource() and not e:_piperAggressiveSplit(),
    "explicit user overrides are respected")
settings = {}
device.model = "PB632"
check(not e:preferPiperVoice() and not e:_piperLowResource() and not e:_piperAggressiveSplit(),
    "other PocketBooks retain existing defaults")
q._server_rr = 0
check(q:_pickServer() == 1 and q:_pickServer() == 2, "other dual-core devices keep their worker pool")
device.model = "PB700"
e.backend = e.BACKENDS.ESPEAK
check(not e:preferPiperVoice(), "espeak remains an explicit available choice")
e.backend = e.BACKENDS.PIPER

local shutdown = 0
e._piper = {getRtf=function() return 10 end, getRtfSampleCount=function() return 10 end,
    shutdown=function() shutdown=shutdown+1 end, setAbandoned=function() end}
local c = setmetatable({tts_engine=e,plugin=plugin}, {__index=Sync})
c:_checkPiperRtfEscalation()
check(not c._piper_abandoned and shutdown == 0, "slow synthesis does not replace the chosen voice")
settings.piper_keep_voice = false
c:_checkPiperRtfEscalation()
check(c._piper_abandoned and shutdown == 1, "speed-first opt-out retains the existing fallback")
settings = {}

local fallback = 0
e.espeak_bin = "espeak-ng"
e.espeakSynthesizeFallback = function() fallback=fallback+1; return "/tmp/espeak.wav" end
c = setmetatable({tts_engine=e,plugin=plugin,stop=function(self) self.stopped=true end}, {__index=Sync})
check(c:_playViaEspeakOrSkip({text="test"}, "synthesis timed out") == false and c.stopped,
    "a real neural failure stops instead of changing the voice or skipping text")
check(fallback == 0 and #ui.shown > 0, "failure gives an error without robotic speech")

-- Both readNextSentence startup paths must wait for the Piper file. An
-- existing quick-start setting must not force espeak over Keep Piper voice.
for _, initial in ipairs({"missing", "pending"}) do
    local ready, status = false, initial == "pending" and "pending" or nil
    local started = 0
    e.usePrefetched = function() return ready end
    e.peekPrefetch = function() return ready and "/tmp/piper.wav" or nil end
    e.getPiperPrefetchStatus = function() return ready and "ready" or status end
    e.getPiperQueueSnapshot = function() return "Piper test queue" end
    e.piperPrefetchAsync = function() status="pending" end
    e._launchNextPiperPrefetch = function() end
    e.timing_data = {}
    c = setmetatable({tts_engine=e,plugin=plugin,state=Sync.STATE.PLAYING,
        reading_sentence_idx=0,total_sentences=1,
        parsed_data={sentences={{text="Neural English speech."}}},
        applySentenceTiming=function() end,
        beginSentencePlayback=function() started=started+1 end}, {__index=Sync})
    ui.scheduled = {}
    c:readNextSentence()
    check(fallback == 0 and started == 0 and #ui.scheduled > 0,
        initial .. ": wait for neural speech during startup")
    ready = true
    ui.scheduled[#ui.scheduled]()
    check(started == 1 and fallback == 0, initial .. ": start the Piper file when synthesis finishes")
end
io.open = real_open
print("ok - " .. checks .. " Era Piper checks passed")
