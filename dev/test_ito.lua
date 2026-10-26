-- Ito uses the guarded, cancellable resident-speech path, never a different voice.
package.loaded.logger=setmetatable({},{__index=function() return function() end end})
package.loaded.audiobook_gettext=function(s) return s end
package.loaded.device={isAndroid=function() return false end,isPocketBook=function() return true end,isKindle=function() return false end,model='PB700'}
package.loaded['ui/uimanager']={scheduleIn=function() end}
package.loaded['ui/time']={}
package.loaded['ui/widget/infomessage']={}
package.loaded['ffi/util']={template=function(s) return s end}
local E=dofile('ttsengine.lua');local S=dofile('rhvoicesettings.lua');local I=dofile('itosettings.lua')
local Menu=dofile('menubuilder.lua');local Cache=dofile('wordcache.lua')
local n=0;local function check(v,m)n=n+1;assert(v,m)end
check(I.prepare('$1,250.75 and 3.05.',S.options(nil))=='1250 dollars and 75 cents and 3 point zero five.','currency, grouped numbers and decimals keep their meaning')
check(I.prepare('1,000,000 items; 1,25 is punctuation.',S.options(nil))=='1000000 items; 1,25 is punctuation.','large grouped numbers normalize without guessing malformed grouping')
check(I.prepare('Dr. Brown met Mr. Smith.',S.options(nil))=='doctor Brown met mister Smith.','titles do not become false sentence endings')
check(I.prepare('“ﬁne”',S.options(nil))=='fine','Ito frontend normalizes ligatures and removes double quotes')
local settings={rhvoice_voice='Bdl',rhvoice_pitch=.9,rhvoice_corrections={reader='reeder'}}
local plugin={getSetting=function(_,k,d)if settings[k]~=nil then return settings[k]end;return d end,setSetting=function(_,k,v)settings[k]=v end,stopReadAlong=function() end}
local engine=setmetatable({plugin=plugin,backend='ito',ito_bin='/reader/ito/ito-speak',rhvoice_bin='/reader/rhvoice/rhvoice-slt',rate=.5,volume=.25,plugin_dir='/reader'}, {__index=E})
plugin.tts_engine=engine
engine.supportsPocketBookSpeaker=function()return true end;engine.getPocketBookAudioDevice=function()return 'era-speaker'end
local requested
local client={binary=engine.ito_bin,request=function(_,text,output,rate,volume,lead,current,done,options)
    requested={text=text,output=output,rate=rate,volume=volume,lead=lead,options=options};return {cancel=function()end}
end}
plugin._rhvoice_service=client
local opts=engine:getRHVoiceOptions()
check(opts.voice=='ItoFemale' and opts.pitch==1 and settings.rhvoice_voice=='Bdl' and settings.rhvoice_pitch==.9,'Ito uses its female model without overwriting RHVoice settings')
check(engine:_getRHVoiceClient()==client,'resident worker uses Ito binary')
engine:_requestRHVoice('reader costs $1.25','/tmp/not-generated.wav',function()return true end,function()end)
check(requested.text=='reeder costs 1 dollars and 25 cents' and requested.options.format=='text','Ito receives corrected plain text and proper numbers')
check(requested.rate==.5 and requested.volume==.25 and requested.lead==350,'selected speed, volume and speaker guard reach actual synthesis')
check(requested.options.voice=='ItoFemale' and requested.options.pitch==1,'a saved RHVoice override cannot select the wrong Ito voice')
local signature=engine:_rhvoiceSignature('reader');engine.volume=.5
check(signature~=engine:_rhvoiceSignature('reader'),'prefetch identity follows volume')
local key=Cache.key(engine,'reader');settings.rhvoice_corrections.reader='reed er'
check(key~=Cache.key(engine,'reader'),'Ito word cache follows pronunciation corrections')
local selected,preview
plugin.previewVoice=function(_,id)preview=id end
engine.espeak_lib_path='test';engine._nativeHelperConfigured=function()return false end;engine.getSntJpStatus=function()return {complete=false}end
engine.setBackend=function(self,b)self.backend=b;selected=b end
local rows=Menu.buildEngineSelectMenu(plugin);local found
for i,row in ipairs(rows)do if row.text=='Ito (offline English neural voice)' then found=i end end
check(found and rows[found+1].text:find('Preview'),'Ito offers engine selection and an adjacent preview')
rows[found+1].callback();check(preview=='ito' and settings.tts_backend==nil,'preview leaves saved reading engine unchanged')
rows[found].callback();check(selected=='ito' and settings.tts_backend=='ito','explicit selection persists Ito')
local menu=Menu.buildItoMenu(plugin);menu[2].callback();check(preview=='ito','Ito settings preview uses its own voice')
engine.ito_bin=nil;local absent=true
for _,row in ipairs(Menu.buildEngineSelectMenu(plugin)) do if row.text=='Ito (offline English neural voice)' then absent=false end end
check(absent,'missing Ito model/runtime is not offered as a working engine')
print('ok - '..n..' Ito frontend, engine, preview, settings and pronunciation-cache checks passed')
