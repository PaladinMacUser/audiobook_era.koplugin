-- Exercise actual disk cache, eviction, identity and disposable playback.
package.loaded.audiobook_gettext=function(s) return s end
package.loaded.logger=setmetatable({}, {__index=function() return function() end end})
local lfs=require('lfs')
local Cache=dofile('wordcache.lua')
local Wav=dofile('wavutils.lua')
local Reader=dofile('wordreader.lua')
local checks=0
local function check(v,msg) checks=checks+1;assert(v,msg) end
local root=os.tmpname();os.remove(root);assert(lfs.mkdir(root))
local function write(p,s) local f=assert(io.open(p,'wb'));assert(f:write(s));f:close() end
local source=root..'/source.wav';assert(Wav.generateSilence(source,200,24000))
local cache=assert(Cache:new{directory=root..'/cache',max_count=2,max_bytes=100000})
local engine={backend='rhvoice',rate=0.5,volume=1,voice='en',getPocketBookAudioDevice=function() return 'era-speaker' end}
local key=Cache.key(engine,'reader')
check(not cache:get(key),'first lookup misses')
check(cache:put(key,source,350),'complete WAV can be cached')
local reopened=assert(Cache:new{directory=root..'/cache'})
check(reopened:get(key),'cache survives creating a new session')
local hit=cache:get(key)
check(hit and hit.lead==350,'hit preserves speaker wakeup lead-in')
local disposable=root..'/play.wav';check(cache:restore(hit,disposable),'restore creates playback copy')
os.remove(disposable);check(cache:get(key),'deleting playback file does not delete cached audio')
engine.rate=0.75;check(Cache.key(engine,'reader')~=key,'speed invalidates cache key');engine.rate=0.5
engine.backend='sanotts';check(Cache.key(engine,'reader')~=key,'engine invalidates key');engine.backend='rhvoice'
engine.volume=0.5;check(Cache.key(engine,'reader')~=key,'volume invalidates key');engine.volume=1
engine.voice='en-us';check(Cache.key(engine,'reader')~=key,'voice invalidates key');engine.voice='en'
engine.piper_speaker=2;check(Cache.key(engine,'reader')~=key,'sparse settings do not truncate key');engine.piper_speaker=nil
engine.getPocketBookAudioDevice=function() return 'auto' end;check(Cache.key(engine,'reader')~=key,'speaker route invalidates lead-in key')
engine.getPocketBookAudioDevice=function() return 'era-speaker' end
local binary=root..'/engine';write(binary,'first');engine.rhvoice_bin=binary
local original=Cache.key(engine,'reader');write(binary,'updated binary');check(Cache.key(engine,'reader')~=original,'updated engine invalidates cache');engine.rhvoice_bin=nil
cache:clear();cache:put('one',source,0);cache:put('two',source,0);cache:get('one');cache:put('three',source,0)
check(cache:get('one') and not cache:get('two') and cache:get('three'),'LRU eviction respects recent replay within one second')
check(cache:stats().count==2,'entry bound enforced')
cache.max_bytes=10000;cache:prune();check(cache:stats().bytes<=10000,'byte bound enforced')
cache:clear();local hash=Cache.hash;Cache.hash=function() return 'abcdef' end
cache:put('first',source,0);cache:put('second',source,0)
check(not cache:get('first') and cache:get('second'),'hash collision cannot play the wrong word')
write(root..'/cache/abcdef.wav','RIFF broken')
check(not cache:get('second'),'corrupt audio is ignored')
check(not cache:put('bad',root..'/cache/abcdef.wav',0),'corrupt source cannot enter cache')
Cache.hash=hash;cache:clear();cache.max_bytes=100000
local generations,plays,stops=0,0,0
function engine:stop() stops=stops+1; if self.current_audio_file then os.remove(self.current_audio_file);self.current_audio_file=nil end end
function engine:synthesize(text,callback)
    generations=generations+1;self.current_audio_file=root..'/generated.wav'
    assert(Wav.generateSilence(self.current_audio_file,200,24000));self.callback=callback;return nil
end
function engine:play(_,complete) plays=plays+1;self.complete=complete;return true end
function engine:getAudioLeadInMs() return 350 end
local reader=Reader:new{engine=engine,cache=cache}
reader:read('reader extra');check(reader:wouldCoalesce('reader') and generations==1,'same pending word is identifiable')
reader:read('reader');check(generations==1,'pending same-key taps coalesce')
engine.callback(true);engine.complete()
reader:repeatLast();check(generations==1 and plays==2,'repeat uses cached word without synthesis')
engine.complete();engine.rate=0.75;reader:repeatLast();check(generations==2,'changed pronunciation speed regenerates audio');engine.callback(true);engine.complete()
reader:preview('Hello sample');check(reader.last_word=='reader' and generations==3,'preview leaves repeat word intact')
engine.callback(true);engine.complete();reader:preview('Hello sample');check(generations==4,'voice previews bypass word cache')
reader:stop();engine.callback(true);check(plays==4,'cancelled preview cannot play later')
cache:clear();check(cache:stats().count==0 and cache:stats().hits==0,'clear removes entries and counters')
local function remove_tree(p) for name in lfs.dir(p) do if name~='.' and name~='..' then local c=p..'/'..name;if lfs.attributes(c,'mode')=='directory' then remove_tree(c) else os.remove(c) end end end;lfs.rmdir(p) end
remove_tree(root)
print('ok - '..checks..' disk pronunciation cache and replay checks passed')
