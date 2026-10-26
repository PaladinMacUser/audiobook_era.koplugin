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
    if inventory[path] then return {close=function() end,read=function() return "#!/bin/sh" end,seek=function() return 3806656 end} end
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
inventory["/reader/ito/ito-speak"]=true;inventory["/reader/ito/phonemize.sh"]=true
inventory["/reader/ito/ito_female_esp32s3.bin"]=true
detected:detectBackend()
check(detected.ito_bin=="/reader/ito/ito-speak" and detected.backend==Engine.BACKENDS.ESPEAK,"Ito detection requires its user-installed model and preserves the current engine")
inventory["/reader/ito/ito_female_esp32s3.bin"]=nil;detected:detectBackend()
check(detected.ito_bin==nil,"removing the model disables Ito on re-detection")
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
        if row.text=="RHVoice (English voices)" then return row end
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


local requests={}
local service={binary=bin,closed=false}
function service:request(text,output,rate,volume,lead,current,callback,options)
    local job={text=text,output=output,rate=rate,volume=volume,lead=lead,current=current,callback=callback,options=options}
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
check(r.text:find("don&apos;t",1,true) and r.text:find("$(touch /tmp/not-a-command)",1,true) and r.options.format=="ssml", "book text is escaped as SSML and remains literal data")
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

-- One bounded lookahead slot must never modify the playing audio/timing.
e.current_audio_file="playing.wav";e.timing_data={{start_time=7,end_time=99}}
local old_timing=e.timing_data;local before_requests=#requests
check(e:prefetch("future sentence") and #requests==before_requests+1,"RHVoice queues one background request")
local prefetched=requests[#requests]
check(e.current_audio_file=="playing.wav" and e.timing_data==old_timing,"in-flight prefetch leaves playback untouched")
e:prefetch("future sentence");check(#requests==before_requests+1,"same lookahead is not generated twice")
finish(prefetched,true)
check(e.current_audio_file=="playing.wav" and e.timing_data==old_timing,"ready lookahead cannot overwrite playing state")
check(not e:usePrefetched("different") and e:usePrefetched("future sentence"),"only the matching ready sentence is adopted")
check(e.current_audio_file==prefetched.output and e:getAudioLeadInMs()==350,"adopted prefetch keeps its own WAV and lead-in")
os.remove(prefetched.output)
e.current_audio_file="playing.wav";e:prefetch("pending sentence");local pending=requests[#requests]
local completion=0
e:synthesize("pending sentence",function(success) check(success,"awaited prefetch succeeds");completion=completion+1 end)
check(requests[#requests]==pending and completion==0,"on-demand read awaits a pending lookahead instead of duplicating it")
finish(pending,true);check(completion==1 and e.current_audio_file==pending.output,"awaited lookahead completes exactly once")
os.remove(pending.output)
e.current_audio_file="playing.wav";e:prefetch("old options");local obsolete=requests[#requests]
settings.rhvoice_voice="Bdl";check(not e:usePrefetched("old options"),"voice change cannot consume old-voice cache")
e:synthesize("old options",function() end)
check(obsolete.cancelled and requests[#requests].options.voice=="Bdl","changed voice cancels stale lookahead and passes the new voice")
finish(obsolete,true);check(e.current_audio_file=="playing.wav","late cancelled prefetch cannot replace current audio");os.remove(obsolete.output)
e:stop();check(not e._rhvoice_slot and requests[#requests].cancelled,"stop cancels foreground and lookahead work")
settings.rhvoice_voice=nil
settings.rhvoice_prefetch=false
check(e:prefetch("disabled")==false,"lookahead can be disabled")
settings.rhvoice_prefetch=nil
local deferred=#ui.scheduled
local stopped=setmetatable({tts_engine=e,reading_sentence_idx=1,total_sentences=2,_chain_generation=4,state=Sync.STATE.PLAYING,
    parsed_data={sentences={{text="current"},{text="late"}}}},{__index=Sync})
stopped:_prefetchNextSentence();stopped.state=Sync.STATE.STOPPED
before_requests=#requests;ui.scheduled[deferred+1]()
check(#requests==before_requests,"deferred lookahead cannot restart synthesis after stop")

-- Exercise the engine bridge, including the acknowledgement used by sync.
local paused,resumed,played,completed,shutdown=0,0,{},0,0
local player={closed=false,request=function(_,text,output,rate,volume,lead,current,callback,options)
    played={text=text,lead=lead,current=current,callback=callback,options=options}
    return {pause=function() paused=paused+1 end,resume=function() resumed=resumed+1 end,cancel=function() end}
end,shutdown=function() shutdown=shutdown+1 end}
e._era_service=player;e._pb_speaker={pcm="hwout_mix"};e.is_speaking=true;e.is_paused=false
e.current_audio_file="guarded.wav";e._audio_lead_in_file="guarded.wav";e._audio_lead_in_ms=350
io.open=function(path,mode) if path=="/reader/rhvoice/era-player" then return {close=function() end} end;return real_open(path,mode) end
check(e:_playEraRHVoice() and played.options.no_output and played.lead==350,"continuous engine uses the resident speaker service with guard metadata")
played.options.on_playing(0)
check(e.playback_latency_ms==80 and e._era_playing_at==1,"warm PCM acknowledgement supplies the highlight anchor without the removed guard")
e:pause();e:resume();check(paused==1 and resumed==1,"engine pause/resume controls PCM without signalling its process")
e.on_complete_callback=function() completed=completed+1 end
played.callback(true);played.callback(true)
check(completed==1 and not e.is_speaking,"speaker completion advances exactly once")
e:_stopEraPlayer();check(shutdown==1 and not e._era_service,"stop releases the persistent speaker service")

local legacy_plays,failures=0,0
local real_play=e.play
e.play=function(self,word,complete,fail)
    legacy_plays=legacy_plays+1
    check(self.current_audio_file=="guarded.wav" and self._era_failed_session and not self._era_service,
        "recovery retains the exact WAV and releases the failed persistent player")
    return true
end
e._era_service=player;e.is_speaking=true;e.current_audio_file="guarded.wav"
e:_playEraRHVoice(nil,nil,function() failures=failures+1 end)
played.callback(false,"Cannot open firmware PCM")
check(legacy_plays==1 and failures==0,"unacknowledged player failure retries the same voice through the working clip player")
check(settings.last_audio_failure.message=="Cannot open firmware PCM","successful recovery retains the original output failure")
e._era_service=player;e.is_speaking=true;e.current_audio_file="guarded.wav"
e:_playEraRHVoice(nil,nil,function() failures=failures+1 end);played.options.on_playing(0)
played.callback(false,"Late output failure")
check(legacy_plays==1 and failures==1,"acknowledged playback is never replayed automatically")
e.play=real_play
io.open=real_open
os.execute=real_execute
print("ok - "..checks.." RHVoice selection, async synthesis and failure checks passed")
