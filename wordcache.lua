-- Bounded disk LRU of pronunciations. Playback always uses a disposable copy.
local Cache={};Cache.__index=Cache
local dir=debug.getinfo(1,"S").source:match("^@(.*/)") or "./"
local Wav=dofile(dir.."wavutils.lua")
local function read(path)
    local f=io.open(path,"rb");if not f then return nil end
    local s=f:read("*a");f:close();return s
end
local function copy(source,target)
    local input=io.open(source,"rb");if not input then return false end
    local out=io.open(target,"wb");if not out then input:close();return false end
    local ok=true
    while true do local chunk=input:read(65536);if not chunk then break end
        if not out:write(chunk) then ok=false;break end
    end
    input:close();if not out:close() then ok=false end
    if not ok then os.remove(target) end
    return ok
end
local function write(path,s)
    local f=io.open(path,"wb");if not f then return false end
    local ok=f:write(s);local closed=f:close();return ok and closed
end
local function valid(path)
    local f=io.open(path,"rb");if not f then return false end
    local h=f:read(44);local size=f:seek("end");f:close()
    if not h or #h~=44 or h:sub(1,4)~="RIFF" or h:sub(9,16)~="WAVEfmt "
            or h:sub(37,40)~="data" or size<=44 then return false end
    local channels=Wav.readLE16(h:sub(23,24))
    local rate=Wav.readLE32(h:sub(25,28))
    return Wav.readLE16(h:sub(21,22))==1 and (channels==1 or channels==2)
        and Wav.readLE16(h:sub(35,36))==16 and rate>=8000 and rate<=48000
        and Wav.readLE16(h:sub(33,34))==channels*2
        and Wav.readLE32(h:sub(29,32))==rate*channels*2
        and Wav.readLE32(h:sub(41,44))==size-44 and (size-44)%(channels*2)==0
end
function Cache:new(o)
    o=o or {};local ok,lfs=pcall(require,"libs/libkoreader-lfs")
    if not ok then ok,lfs=pcall(require,"lfs") end
    o.lfs=o.lfs or (ok and lfs);if not o.lfs then return nil end
    o.max_count=o.max_count or 200;o.max_bytes=o.max_bytes or 20*1048576
    o.hits=0;o.misses=0;o.counter=0;o.access={};o.access_counter=0
    local path=""
    for part in o.directory:gmatch("[^/]+") do
        path=(path=="" and o.directory:sub(1,1)=="/" and "/" or path..(path~="" and "/" or ""))..part
        if not o.lfs.attributes(path) then local made=o.lfs.mkdir(path);if not made then return nil end end
    end
    return setmetatable(o,self)
end
function Cache.hash(key)
    local a,b=5381,2166136261
    for i=1,#key do local byte=key:byte(i);a=(a*33+byte)%4294967296;b=(b*65599+byte)%4294967296 end
    return string.format("%08x%08x",a,b)
end
function Cache.key(engine,word)
    local out={}
    local function add(...)
        for i=1,select("#",...) do local value=select(i,...);local s=tostring(value or "")
            out[#out+1]=#s..":"..s
        end
    end
    add("word-cache-v1",word,engine.backend,engine.voice,engine.rate,engine.volume,engine.pitch,
        engine.word_gap,engine.clause_pause,engine.piper_model,engine.piper_speaker,
        engine.sanotts_voice_name,engine.sanotts_tempo,engine.sanotts_voice_dir,engine.sanotts_server,
        engine.rhvoice_bin,engine.backend_cmd,engine.getPocketBookAudioDevice and engine:getPocketBookAudioDevice())
    local plugin=engine.plugin
    for _,k in ipairs({"sanotts_g2p","native_helper_path","native_input_encoding","native_prestep_command"}) do
        add(plugin and plugin:getSetting(k,"") or "")
    end
    add(dofile(dir.."_meta.lua").version)
    local ok,lfs=pcall(require,"libs/libkoreader-lfs");if not ok then ok,lfs=pcall(require,"lfs") end
    local function fingerprint(path)
        local a=type(path)=="string" and ok and lfs.attributes(path)
        add(path,a and a.size,a and a.modification)
    end
    fingerprint(engine.piper_model);fingerprint(engine.rhvoice_bin)
    fingerprint(engine.sanotts_server);fingerprint(engine.backend_cmd)
    return table.concat(out,"|")
end

function Cache:get(key)
    local base=self.directory.."/"..self.hash(key)
    local metadata=read(base..".key")
    local lead,saved
    if metadata then lead,saved=metadata:match("^(%d+)\n(.*)$") end
    if saved==key and valid(base..".wav") then
        self.hits=self.hits+1
        self.access_counter=self.access_counter+1;self.access[base..".wav"]=self.access_counter
        if self.lfs.touch then self.lfs.touch(base..".wav") end
        return {file=base..".wav",lead=tonumber(lead)}
    end
    self.misses=self.misses+1;return nil
end
function Cache:entries()
    local entries={};local count,bytes=0,0
    for name in self.lfs.dir(self.directory) do
        if name:match("^[%da-f]+%.wav$") then
            local path=self.directory.."/"..name;local a=self.lfs.attributes(path)
            if a then entries[#entries+1]={file=path,size=a.size,time=a.modification or 0,access=self.access[path] or 0};count=count+1;bytes=bytes+a.size end
        end
    end
    return entries,count,bytes
end
function Cache:prune()
    local entries,count,bytes=self:entries()
    table.sort(entries,function(a,b)
        if a.time~=b.time then return a.time<b.time end
        if a.access~=b.access then return a.access<b.access end
        return a.file<b.file
    end)
    for _,e in ipairs(entries) do
        if count<=self.max_count and bytes<=self.max_bytes then break end
        os.remove(e.file);os.remove(e.file:gsub("%.wav$",".key"));count=count-1;bytes=bytes-e.size
    end
end
function Cache:put(key,source,lead)
    local a=self.lfs.attributes(source)
    if not a or a.size>self.max_bytes or not valid(source) then return false end
    local base=self.directory.."/"..self.hash(key)
    self.counter=self.counter+1
    local temporary=base..".tmp-"..self.counter
    if not copy(source,temporary) then return false end
    -- Invalidate metadata before replacing a possible hash collision.
    -- Interrupted writes can cause a miss, never playback of the wrong word.
    os.remove(base..".key")
    if not os.rename(temporary,base..".wav") then os.remove(temporary);return false end
    if not write(base..".key.new",tostring(math.floor(lead or 0)).."\n"..key)
            or not os.rename(base..".key.new",base..".key") then os.remove(base..".key.new");return false end
    self.access_counter=self.access_counter+1;self.access[base..".wav"]=self.access_counter
    self:prune();return true
end
function Cache:restore(entry,target)
    return valid(entry.file) and copy(entry.file,target)
end
function Cache:clear()
    for name in self.lfs.dir(self.directory) do
        if name:match("^[%da-f]+%.wav$") or name:match("^[%da-f]+%.key$") then os.remove(self.directory.."/"..name) end
    end
    self.hits=0;self.misses=0;self.access={}
end
function Cache:stats()
    local _,count,bytes=self:entries();return {count=count,bytes=bytes,hits=self.hits,misses=self.misses}
end
return Cache
