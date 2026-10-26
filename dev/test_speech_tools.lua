-- Independent pronunciation/preview state and persistent, text-free diagnostics.
package.loaded['ui/widget/container/widgetcontainer']={extend=function(_,t) return t end}
package.loaded.logger=setmetatable({}, {__index=function() return function() end end})
package.loaded.audiobook_gettext=function(s) return s end
package.loaded['ffi/util']={template=function(s,v) return (s:gsub('%%1',tostring(v))) end}
local ui={shown={},scheduled={}}
function ui:show(o) self.shown[#self.shown+1]=o end
function ui:scheduleIn(_,fn) self.scheduled[#self.scheduled+1]=fn end
package.loaded['ui/uimanager']=ui
local Info={new=function(_,o) return o end}
package.loaded['ui/widget/infomessage']=Info
local Plugin=dofile('main.lua')
local Menu=dofile('menubuilder.lua')
local Reader=dofile('wordreader.lua')
local D=dofile('speechdiagnostics.lua')
local checks=0
local function check(v,msg) checks=checks+1;assert(v,msg) end
local function seed(fn,name,value)
    for i=1,50 do local n=debug.getupvalue(fn,i);if not n then break end;if n==name then debug.setupvalue(fn,i,value);return end end
    error('Missing upvalue: '..name)
end
seed(Plugin.previewVoice,'UIManager',ui);seed(Plugin.previewVoice,'InfoMessage',Info)
seed(Plugin._ensureSpeechTools,'PLUGIN_PATH','./')
local settings={speech_rate=.5,word_speech_rate=.75,tts_backend='rhvoice'}
local flushes,stops,kills,shutdowns=0,0,0,0
local plugin=setmetatable({_init_ok=true,settings={flush=function() flushes=flushes+1 end}},{__index=Plugin})
function plugin:getSetting(k,d) if settings[k]~=nil then return settings[k] end;return d end
function plugin:setSetting(k,v) settings[k]=v end
function plugin:toggleSetting(k,d) self:setSetting(k,not self:getSetting(k,d)) end
function plugin:_audioOutputReady() return true end
function plugin:_ensureSpeechTools() end
function plugin:stopReadAlong() stops=stops+1;self.word_reader:stop() end
local aux={BACKENDS={PIPER='piper'},backend='rhvoice',audio_player_type='pb-era-speaker',_pb_speaker={pcm='hwout_mix'}}
for method,field in pairs({setBackend='backend',setRate='rate',setVolume='volume',setVoice='voice',setPitch='pitch',setWordGap='word_gap',setClausePause='clause_pause',setPiperSpeaker='piper_speaker',setSanottsVoice='sanotts_voice_name'}) do
    aux[method]=function(self,value) self[field]=value end
end
function aux:stop() end
function aux:forceKillAll() kills=kills+1 end
function aux:synthesize(text,callback) self.text=text;self.callback=callback;return nil end
function aux:play(_,complete,fail) self.complete=complete;self.fail=fail;return true end
function aux:getPocketBookAudioDevice() return 'era-speaker' end
plugin._speech_engine=aux
plugin.tts_engine={backend='rhvoice',rate=.5,volume=1,voice='en'}
function plugin.tts_engine:getPocketBookAudioDevice() return 'auto' end
plugin.word_reader=Reader:new{engine=aux}
plugin:readSelectedWord('reader more')
check(aux.text=='reader' and aux.rate==.75,'selected word uses pronunciation speed')
check(plugin.tts_engine.rate==.5 and settings.speech_rate==.5,'pronunciation does not change reading speed')
aux.callback(true);aux.complete()
plugin:repeatLastWord();check(aux.text=='reader' and aux.rate==.75,'repeat uses pronunciation setup')
local coalesce=plugin.word_reader.wouldCoalesce
plugin.word_reader.wouldCoalesce=function(_,text,target)
    return text=='reader' and target.rate==.75 and target.backend=='rhvoice'
end
local prior_stops=stops;plugin:readSelectedWord('reader')
check(stops==prior_stops,'same pending word is coalesced before cancelling active playback')
plugin.word_reader.wouldCoalesce=coalesce
local rates=Menu.buildPronunciationMenu(plugin)[1].sub_item_table
rates[2].callback();check(settings.word_speech_rate==.5 and settings.speech_rate==.5,'pronunciation menu stores independent setting')
rates[3].callback();check(settings.word_speech_rate==.75 and plugin.tts_engine.rate==.5,'word speed can change without changing reading engine')
plugin:previewVoice('sanotts',false,'Bdl')
check(aux.rhvoice_voice_override=='Bdl' and settings.rhvoice_voice==nil,'voice preview passes an override without saving it')
check(aux.backend=='sanotts' and aux.rate==.5,'preview uses requested voice and reading speed')
check(settings.tts_backend=='rhvoice' and plugin.tts_engine.backend=='rhvoice','preview preserves saved and active reading engine')
check(plugin.word_reader.last_word=='reader','preview preserves last selected word')
aux.callback(true);aux.complete();plugin:repeatLastWord()
check(aux.rhvoice_voice_override==nil and aux.backend=='rhvoice' and aux.text=='reader' and aux.rate==.75,'repeat restores chosen engine and pronunciation speed after preview')
local failure=D.record(plugin,aux,'playback','Test output failure')
check(flushes==1 and failure.engine=='rhvoice' and failure.output=='era-speaker','failure metadata is saved promptly')
check(not failure.text and not failure.word,'failure metadata contains no selected text')
plugin:previewVoice(nil,true);check(plugin._audio_test_status=='Running','audio test starts visibly')
aux.callback(true);aux.complete()
check(plugin._audio_test_status:find('confirm') and settings.last_audio_failure==failure,'completed player asks for audibility check and preserves latest failure')
plugin._last_audio_instance=aux
local status=D.describe(plugin,plugin.tts_engine)
check(status:find('hwout_mix') and status:find('era%-speaker') and status:find('Test output failure'),'diagnostics show actual last output and retained failure')
plugin:previewVoice(nil,true);plugin.word_reader:stop()
check(plugin._audio_test_status=='Cancelled' and settings.last_audio_failure==failure,'cancelled test does not remain running or overwrite failure')
plugin:previewVoice(nil,true);aux.callback(false)
check(plugin._audio_test_status=='Playback failed','audio test exposes synthesis failure')
local menu=Menu.buildAudioDiagnosticsMenu(plugin)
menu[4].callback();check(settings.last_audio_failure==nil,'explicit clear removes retained failure')
plugin._rhvoice_service={shutdown=function() shutdowns=shutdowns+1 end}
plugin:_shutdownSpeechTools();check(kills==1 and shutdowns==1 and not plugin._rhvoice_service,'book/suspend cleanup stops auxiliary engine and shared worker')
print('ok - '..checks..' pronunciation settings, previews and diagnostics checks passed')
