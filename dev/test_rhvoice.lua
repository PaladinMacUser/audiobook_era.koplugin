-- Optional RHVoice engine selection, asynchronous WAV ownership and failures.
local real_open, real_execute = io.open, os.execute
local device={model="PB700",isPocketBook=function() return true end,
    isKindle=function() return false end,isAndroid=function() return false end}
local ui={scheduled={},shown={}}
function ui:scheduleIn(_,fn) self.scheduled[#self.scheduled+1]=fn end
function ui:show(msg) self.shown[#self.shown+1]=msg end
function ui:getTime() return 1 end
package.loaded.device=device
package.loaded.logger=setmetatable({}, {__index=function() return function() end end})
package.loaded.audiobook_gettext=function(s) return s end
package.loaded["ui/uimanager"]=ui
package.loaded["ui/time"]={to_ms=function(n) return n*1000 end}
package.loaded["ui/event"]={}
package.loaded["ui/widget/infomessage"]={new=function(_,o) return o end}
package.loaded["ffi/util"]={template=function(s) return s end}
local Engine=dofile("ttsengine.lua")
local Menu=dofile("menubuilder.lua")
local Sync=dofile("synccontroller.lua")
local checks=0
local function check(v,message) checks=checks+1; assert(v,message) end
local inventory={
    ["/reader/espeak-ng/bin/espeak-ng"]=true,
    ["/reader/rhvoice/rhvoice-slt"]=true,
    ["/reader/rhvoice/data/languages/English/language.info"]=true,
    ["/reader/rhvoice/data/languages/English/cmulex.fst"]=true,
    ["/reader/rhvoice/data/voices/slt/voice.info"]=true,
    ["/reader/rhvoice/data/voices/slt/24000/voice.data"]=true,
}
io.open=function(path,mode)
    if inventory[path] then return {close=function() end,read=function() return "#!/bin/sh" end} end
    return real_open(path,mode)
end
os.execute=function() return 0 end
local detected=setmetatable({plugin_dir="/reader"},{__index=Engine})
detected:detectBackend()
check(detected.rhvoice_bin=="/reader/rhvoice/rhvoice-slt", "detect the complete bundled runtime")
check(detected.backend==Engine.BACKENDS.ESPEAK, "detecting RHVoice does not replace the default backend")
inventory["/reader/rhvoice/data/voices/slt/24000/voice.data"]=nil
detected:detectBackend()
check(detected.rhvoice_bin==nil, "missing voice data disables the RHVoice option on re-detection")
io.open,os.execute=real_open,real_execute
local settings={tts_backend="sanotts",speech_rate=0.5}
local stops=0
local plugin={getSetting=function(_,k,d) if settings[k]~=nil then return settings[k] end return d end,
    setSetting=function(_,k,v) settings[k]=v end,stopReadAlong=function() stops=stops+1 end}
local e=setmetatable({backend=Engine.BACKENDS.SANOTTS,sanotts_server="snt_server",
    rhvoice_bin="/reader/rhvoice/rhvoice-slt",plugin=plugin,plugin_dir="/reader",
    volume=0.75,rate=0.5,file_counter=0}, {__index=Engine})
plugin.tts_engine=e
e._piper=dofile("piperqueue.lua"):new{engine=e}
e.generateTimingEstimates=function(self) self.timing_data={{start_time=0,end_time=500}} end
local function rhrow()
    for _,row in ipairs(Menu.buildEngineSelectMenu(plugin)) do
        if row.text=="RHVoice (SLT, American English)" then return row end
    end
end
local previewed,preview_count
plugin.previewVoice=function(_,backend) previewed=backend;preview_count=(preview_count or 0)+1 end
local entries=Menu.buildEngineSelectMenu(plugin)
local offered=0
for i=1,#entries,2 do
    check(entries[i+1] and entries[i+1].text:find("Preview"),"each installed engine has a neighboring preview")
    entries[i+1].callback();offered=offered+1
end
check(preview_count==offered and previewed~=nil and settings.tts_backend=="sanotts" and e.backend=="sanotts",
    "preview rows call preview without selecting the reading engine")
local row=rhrow()
check(row and not row.checked_func(), "RHVoice is an additional, unselected engine")
check(settings.tts_backend=="sanotts" and e.backend=="sanotts", "building the menu preserves the existing voice")
local bin=e.rhvoice_bin;e.rhvoice_bin=nil
check(rhrow()==nil, "uninstalled RHVoice is not offered")
e.rhvoice_bin=bin;row.callback()
check(e.backend=="rhvoice" and e.backend_cmd==bin and settings.tts_backend=="rhvoice", "selecting RHVoice saves the new backend")
check(stops==1 and settings.espeak_only_mode==false, "engine switch stops old reading and clears forced espeak")
check(e.rate==0.5 and e.volume==0.75, "engine selection retains the chosen rate and volume")
e.current_audio_file="playing.wav"
check(e:prefetch("next sentence")==false and e.current_audio_file=="playing.wav", "generic prefetch cannot overwrite RHVoice's playing file")

local requests={}
local service={binary=bin,closed=false}
function service:request(text,output,rate,volume,lead,current,callback)
    local job={text=text,output=output,rate=rate,volume=volume,lead=lead,current=current,callback=callback}
    function job:cancel() self.cancelled=true end
    requests[#requests+1]=job;return job
end
plugin._rhvoice_service=service
os.execute=function() return 0 end
local function request(text)
    local completed,success=0,nil
    e:synthesize(text,function(ok) completed=completed+1;success=ok end)
    local job=requests[#requests];job.result=function() return completed,success end
    return job
end
local Wav=dofile("wavutils.lua")
local function finish(r,ok,empty)
    if ok then
        if empty then local f=assert(real_open(r.output,"wb"));f:write(string.rep("x",44));f:close()
        else assert(Wav.generateSilence(r.output,400,24000)) end
    end
    r.callback(ok,not ok and "Simulated RHVoice failure" or nil)
end
local r=request("don't; $(touch /tmp/not-a-command)")
check(r.text=="don't; $(touch /tmp/not-a-command)", "book text reaches the resident service literally")
check(r.rate==0.5 and r.volume==0.75, "pass separate per-request rate and volume")
check(r.lead==350, "Era speaker retains the clip-start guard")
check(r.result()==0, "resident synthesis is asynchronous")
finish(r,true)
local n,ok=r.result()
check(n==1 and ok and e.current_audio_file==r.output,"successful resident audio is delivered")
check(e:getAudioLeadInMs()==350,"guard metadata belongs to the completed clip")
e.getWavDurationMs=function() return 1350 end
check(e:getAudioDurationMs()==1000,"speech timing excludes the guard")
e.current_audio_file="different.wav"
check(e:getAudioLeadInMs()==0,"other clips do not inherit the guard")
os.remove(r.output)
settings.pb_alsa_device="tts_sm"
local no_guard=request("another route")
check(no_guard.lead==0,"other routes have no speaker guard")
finish(no_guard,true);os.remove(no_guard.output);settings.pb_alsa_device=nil
r=request("cancelled word")
e:stop();e.current_audio_file="newer.wav"
local newer=request("new word")
finish(r,true)
check(r.cancelled and r.result()==0 and e.current_audio_file=="newer.wav","cancelled audio cannot replace a newer clip")
check(e._rhvoice_pending==newer,"stale completion cannot forget the newer pending request")
os.remove(r.output)
finish(newer,true);os.remove(newer.output)
e.current_audio_file="newer.wav"
r=request("failure");finish(r,false)
n,ok=r.result()
check(n==1 and ok==false and e.current_audio_file=="newer.wav","failed resident request preserves current audio")
check(settings.last_audio_failure.message=="Simulated RHVoice failure","save the specific failure rather than overwriting it with a generic error")
r=request("empty WAV");finish(r,true,true)
n,ok=r.result();check(n==1 and ok==false,"header-only audio is rejected")
check(plugin._rhvoice_service==service and not service.closed,"stopping audio keeps the shared resident service available")

local fallback,advance=0,0
e.espeakSynthesizeFallback=function() fallback=fallback+1 end
local c=setmetatable({tts_engine=e,stop=function(self) self.stopped=true end,
    readNextSentence=function() advance=advance+1 end}, {__index=Sync})
check(c:_playViaEspeakOrSkip({text="word"},"failed")==false and c.stopped, "failed RHVoice stops reading")
check(fallback==0 and advance==0 and #ui.shown==1, "failure neither changes the voice nor skips content")
os.execute=real_execute
print("ok - "..checks.." RHVoice selection, async synthesis and failure checks passed")
