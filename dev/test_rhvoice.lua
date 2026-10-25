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

local launched
os.execute=function(cmd) launched=cmd;return 0 end
local function request(text)
    ui.scheduled={}
    local completed,success=0,nil
    e:synthesize(text,function(ok) completed=completed+1;success=ok end)
    local input=assert(launched:match("%-%-input '([^']+)'"))
    local output=assert(launched:match("%-%-output '([^']+)'"))
    local f=assert(real_open(input,"rb"));local contents=f:read("*a");f:close()
    return {input=input,output=output,contents=contents,poll=ui.scheduled[1],result=function() return completed,success end}
end
local function finish(r,exit,bytes)
    if bytes then local f=assert(real_open(r.output,"wb"));f:write(string.rep("x",bytes));f:close() end
    local f=assert(real_open(r.output..".done","w"));f:write(exit);f:close()
    r.poll()
end
local r=request("don't; $(touch /tmp/not-a-command)")
check(r.contents=="don't; $(touch /tmp/not-a-command)", "book text is written literally to a file")
check(not launched:find("touch /tmp/not-a-command",1,true), "book text cannot become shell code")
check(launched:find("--speed '0.500'",1,true) and launched:find("--volume '0.750'",1,true), "pass the saved speed and volume to RHVoice")
check(launched:find("--lead-in-ms 350",1,true), "Era speaker gets a short clip-start guard")
check(r.result()==0 and type(r.poll)=="function", "synthesis returns to the UI before audio is ready")
finish(r,"0",200)
local n,ok=r.result()
check(n==1 and ok and e.current_audio_file==r.output, "successful audio is delivered once")
check(e:getAudioLeadInMs()==350, "guard duration is attached to this completed clip")
e.getWavDurationMs=function() return 1350 end
check(e:getAudioDurationMs()==1000, "speech timing excludes the speaker guard")
e.current_audio_file="different.wav"
check(e:getAudioLeadInMs()==0, "other clips cannot inherit the old guard")
e.current_audio_file=r.output
os.remove(r.output)

settings.pb_alsa_device="tts_sm"
local no_guard=request("another output")
check(not launched:find("--lead-in-ms",1,true), "other audio routes keep their original PCM")
finish(no_guard,"0",200);os.remove(no_guard.output)
settings.pb_alsa_device=nil

r=request("cancelled word")
e:stop();e.current_audio_file="newer.wav"
finish(r,"0",200)
check(r.result()==0 and e.current_audio_file=="newer.wav", "cancelled synthesis cannot replace a newer clip")

r=request("failed synthesis")
finish(r,"1",200)
n,ok=r.result()
check(n==1 and ok==false and e.current_audio_file=="newer.wav", "failed synthesis rejects even a nonempty output")
os.remove(r.output)
r=request("empty synthesis")
finish(r,"0",44)
n,ok=r.result()
check(n==1 and ok==false, "header-only WAV is a synthesis failure")
os.remove(r.output)

local fallback,advance=0,0
e.espeakSynthesizeFallback=function() fallback=fallback+1 end
local c=setmetatable({tts_engine=e,stop=function(self) self.stopped=true end,
    readNextSentence=function() advance=advance+1 end}, {__index=Sync})
check(c:_playViaEspeakOrSkip({text="word"},"failed")==false and c.stopped, "failed RHVoice stops reading")
check(fallback==0 and advance==0 and #ui.shown==1, "failure neither changes the voice nor skips content")
os.execute=real_execute
print("ok - "..checks.." RHVoice selection, async synthesis and failure checks passed")
