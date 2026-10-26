package.loaded.audiobook_gettext=function(s) return s end
package.loaded.logger=setmetatable({}, {__index=function() return function() end end})
package.loaded['ffi/util']={template=function(s,...) local args={...};return (s:gsub('%%(%d+)',function(i) return tostring(args[tonumber(i)]) end)) end}
local ui={shown={}}
function ui:show(o) self.shown[#self.shown+1]=o end
function ui:close(o) o.closed=true end
package.loaded['ui/uimanager']=ui
package.loaded['ui/widget/infomessage']={new=function(_,o) return o end}
package.loaded['ui/widget/inputdialog']={new=function(_,o)
    function o:getInputText() return self.input end
    function o:onShowKeyboard() end
    return o
end}
local Settings=dofile('rhvoicesettings.lua')
local Menu=dofile('menubuilder.lua')
local Cache=dofile('wordcache.lua')
local checks=0
local function check(v,msg) checks=checks+1;assert(v,msg) end
check(Settings.correct('Dr. Alice saw ALICE and malice.',{['dr.']='doctor',alice='Al iss'})=='doctor Al iss saw Al iss and malice.','corrections match case-insensitive whole words without changing substrings')
check(Settings.correct("reader's book",{reader='reeder'})=="reader's book",'partial possessives are not accidentally replaced')
check(Settings.correct('New York', {new='old',['new york']='Noo York'})=='Noo York','longer phrase correction wins')
check(Settings.correct('alpha',{alpha='beta',beta='gamma'})=='beta','corrections are applied once without recursive substitution')
local o={voice='Slt',pitch=1,comma=.3,corrections={}}
local ssml=Settings.ssml('<name> & "reader", next.',o,.5)
check(ssml:find('&lt;name&gt; &amp; &quot;reader&quot;',1,true) and ssml:find('<break time="300ms"/>',1,true),'text is escaped; comma pause stays independent of rate')
check(not Settings.ssml('Meet at 10:30 with 1,000 books.',o,1):find('<break',1,true),'clock and numeric separators remain intact')
check(Settings.signature({voice='Slt',pitch=1,comma=0,corrections={a='b',c='d'}})==Settings.signature({voice='Slt',pitch=1,comma=0,corrections={c='d',a='b'}}),'correction identity does not depend on table ordering')
local settings={speech_rate=.5,word_speech_rate=.75,tts_backend='rhvoice',rhvoice_voice='Slt',pb_alsa_device='era-speaker'}
local stopped,preview=0,nil
local plugin={getSetting=function(_,k,d) if settings[k]~=nil then return settings[k] end;return d end,
    setSetting=function(_,k,v) settings[k]=v end,stopReadAlong=function() stopped=stopped+1 end,
    toggleSetting=function(self,k,d) self:setSetting(k,not self:getSetting(k,d)) end,
    previewVoice=function(_,backend,test,voice) preview={backend,test,voice} end}
local engine={plugin=plugin,backend='rhvoice',rate=.5,volume=1,BACKENDS={RHVOICE='rhvoice'},
    getRHVoiceOptions=function() return Settings.options(plugin) end,setRate=function(self,v) self.rate=v end,
    getPocketBookAudioDevice=function() return 'era-speaker' end,supportsPocketBookSpeaker=function() return true end}
plugin.tts_engine=engine
local lfs=require('lfs');local root=os.tmpname();os.remove(root);assert(lfs.mkdir(root))
local function mkdir(p) if not lfs.attributes(p) then assert(lfs.mkdir(p)) end end
mkdir(root..'/rhvoice');mkdir(root..'/rhvoice/data');mkdir(root..'/rhvoice/data/voices')
for _,v in ipairs(Settings.voices) do
    local p=root..'/rhvoice/data/voices/'..v.folder;mkdir(p);mkdir(p..'/24000')
    local f=assert(io.open(p..'/voice.info','w'));f:write('name='..v.id);f:close()
    f=assert(io.open(p..'/24000/voice.data','w'));f:write('test');f:close()
end
engine.plugin_dir=root
Settings.migrateOutput(plugin,engine)
check(settings.rhvoice_keep_speaker==false,'Era upgrade restores the established clip output')
settings.rhvoice_keep_speaker=true;Settings.migrateOutput(plugin,engine)
check(settings.rhvoice_keep_speaker==true,'later explicit opt-in is retained')
settings.rhvoice_keep_speaker=nil
local menu=Menu.buildRHVoiceMenu(plugin);local voices=menu[1].sub_item_table
check(#voices==8 and voices[1].checked_func(),'all four installed voices have selection and preview rows')
voices[4].callback();check(preview[1]=='rhvoice' and preview[3]=='Bdl' and settings.rhvoice_voice=='Slt','voice preview passes temporary voice without selecting it')
voices[5].callback();check(settings.rhvoice_voice=='Clb' and voices[5].checked_func(),'selecting a voice saves the requested profile')
local key=Cache.key(engine,'reader')
menu[3].sub_item_table[3].callback();check(settings.rhvoice_pitch==.95 and settings.speech_rate==.5,'pitch tuning preserves speed')
check(Cache.key(engine,'reader')~=key,'pitch change invalidates word cache')
key=Cache.key(engine,'reader');menu[4].sub_item_table[5].callback()
check(settings.rhvoice_comma_pause==.25 and Cache.key(engine,'reader')~=key,'comma tuning changes its own setting and pronunciation identity')
key=Cache.key(engine,'reader');settings.rhvoice_voice='Bdl';check(Cache.key(engine,'reader')~=key,'voice selection invalidates word cache')
key=Cache.key(engine,'reader');settings.rhvoice_corrections={reader='reeder'};check(Cache.key(engine,'reader')~=key,'correction change invalidates cached pronunciation')
settings.rhvoice_corrections={}
local correction_menu=Menu.buildRHCorrectionsMenu(plugin);correction_menu[1].callback()
local dialog=ui.shown[#ui.shown];dialog.input='Dr.';dialog.buttons[1][2].callback()
dialog=ui.shown[#ui.shown];dialog.input='doctor';dialog.buttons[1][2].callback()
check(settings.rhvoice_corrections['dr.']=='doctor' and dialog.closed,'correction editor saves spoken spelling')
local saved=settings.rhvoice_corrections
Menu.editRHCorrection(plugin,'dr.');dialog=ui.shown[#ui.shown];dialog.input=' ';dialog.buttons[1][2].callback()
check(settings.rhvoice_corrections==saved and not dialog.closed,'blank correction cannot overwrite a valid entry')
correction_menu=Menu.buildRHCorrectionsMenu(plugin);correction_menu[2].sub_item_table[2].callback()
check(not settings.rhvoice_corrections['dr.'],'a correction can be removed')
menu[2].sub_item_table[1].callback()
check(settings.speech_rate==.5 and settings.word_speech_rate==.5 and settings.rhvoice_comma_pause==.25 and settings.rhvoice_paragraph_pause==1.2,'slow preset applies speed and independent prosody together')
check(settings.rhvoice_voice=='Bdl' and settings.tts_backend=='rhvoice' and settings.pb_alsa_device=='era-speaker','presets retain voice, engine and speaker choice')
menu[8].callback();check(settings.rhvoice_prefetch==false,'prefetch toggle is available')
menu[9].callback();check(settings.rhvoice_keep_speaker==true,'persistent speaker toggle is available')
key=Cache.key(engine,'reader');menu[10].callback()
check(settings.rhvoice_natural_english==false and Cache.key(engine,'reader')~=key,'natural phrasing toggle invalidates cached pronunciations')
settings.rhvoice_pitch=99;settings.rhvoice_comma_pause=-5
check(Settings.options(plugin).pitch==1.15 and Settings.options(plugin).comma==0,'saved prosody settings are clamped')
local function remove(p) for name in lfs.dir(p) do if name~='.' and name~='..' then local c=p..'/'..name;if lfs.attributes(c,'mode')=='directory' then remove(c) else os.remove(c) end end end;lfs.rmdir(p) end
remove(root)
print('ok - '..checks..' English voice, prosody, corrections, presets and cache identity checks passed')
