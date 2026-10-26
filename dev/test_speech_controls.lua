-- Public menus must apply baked RHVoice settings to the current audio.
package.loaded.logger=setmetatable({},{__index=function() return function() end end})
package.loaded.audiobook_gettext=function(s) return s end
package.loaded['ffi/util']={template=function(s,v) return (s:gsub('%%1',tostring(v))) end}
package.loaded['ui/uimanager']={getTime=function() return 1 end}
package.loaded['ui/event']={}
package.loaded['ui/time']={}
package.loaded['ui/widget/infomessage']={}
local Sync=dofile('synccontroller.lua')
local Menu=dofile('menubuilder.lua')
local n=0
local function check(v,m) n=n+1;assert(v,m) end
local saved={}
local engine={backend='rhvoice',setRate=function(self,v) self.rate=v end,setVolume=function(self,v) self.volume=v end}
local plugin={tts_engine=engine,getSetting=function(_,k,d) if saved[k]~=nil then return saved[k] end;return d end,setSetting=function(_,k,v) saved[k]=v end}
local restarts=0
local sync=setmetatable({plugin=plugin,tts_engine=engine,state=Sync.STATE.PLAYING,
    _restartFromCurrentSentence=function() restarts=restarts+1 end},{__index=Sync})
plugin.sync_controller=sync
Menu.buildSpeechRateMenu(plugin)[2].callback()
check(engine.rate==.5 and saved.speech_rate==.5 and restarts==1,'main rate menu restarts current speech')
Menu.buildVolumeMenu(plugin)[5].callback()
check(engine.volume==.25 and saved.speech_volume==.25 and restarts==2,'main volume menu restarts current RHVoice speech')
sync.state=Sync.STATE.PAUSED
sync:setSpeechRate(.75);sync:setSpeechVolumePct(50)
check(restarts==2 and sync._speech_settings_dirty and sync.state==Sync.STATE.PAUSED,'settings leave paused speech silent')
sync._user_paused=true;sync:resume(true)
check(restarts==2 and sync._speech_settings_dirty,'overlay close respects explicit pause with new settings')
sync:resume()
check(restarts==3 and not sync._speech_settings_dirty and sync.state==Sync.STATE.PLAYING,'explicit resume regenerates with latest settings')
sync.state=Sync.STATE.LOADING;sync:setSpeechRate(1)
check(restarts==4,'rate change during synthesis cancels old speech and regenerates')
sync.state=Sync.STATE.STOPPED;sync:setSpeechVolumePct(0)
check(restarts==4 and engine.volume==0,'idle volume change persists mute without starting reading')
for _,row in ipairs(Menu.buildPronunciationMenu(plugin)) do check(row.text~='Repeat last word','removed repeat action stays absent') end
-- KOReader template substitutes %1 literally; unlike string.format it does
-- not consume %% escapes. Inspect the user-facing volume row.
local f=assert(io.open('menubuilder.lua'));local source=f:read('*a');f:close()
check(not source:find('Volume: %%1%%%%'),'volume template never doubles the percent sign')
print('ok - '..n..' speech-control integration checks passed')
