-- RHVoice pronunciation, prosody and installed English voice metadata.
local Settings = {}
Settings.voices = {
    {id="Slt", folder="slt", label="SLT — American English, female"},
    {id="Bdl", folder="bdl", label="BDL — American English, male"},
    {id="Clb", folder="clb", label="CLB — American English, female"},
    {id="Alan", folder="alan", label="Alan — Scottish English, male"},
}
Settings.presets = {
    {name="Slow and clear",rate=0.5,word_rate=0.5,pitch=1,comma=0.25,sentence=0.4,paragraph=1.2},
    {name="Everyday reading",rate=0.75,word_rate=0.75,pitch=1,comma=0.12,sentence=0.2,paragraph=0.8},
    {name="Word practice",rate=0.5,word_rate=0.4,pitch=0.95,comma=0.3,sentence=0.5,paragraph=1.4},
}
function Settings.options(plugin,override)
    local function get(k,d) return plugin and plugin:getSetting(k,d) or d end
    return {voice=override or get("rhvoice_voice","Slt"),
        pitch=math.max(0.85,math.min(1.15,tonumber(get("rhvoice_pitch",1)) or 1)),
        comma=math.max(0,math.min(1.5,tonumber(get("rhvoice_comma_pause",0)) or 0)),
        sentence=math.max(0,math.min(3,tonumber(get("rhvoice_sentence_pause",get("sentence_pause",0.1))) or 0.1)),
        paragraph=math.max(0,math.min(5,tonumber(get("rhvoice_paragraph_pause",get("paragraph_pause",0.8))) or 0.8)),
        corrections=get("rhvoice_corrections",{})}
end
local function word_byte(s) return s~="" and (s:match("[%w_']")~=nil or s:byte()>=128) end
function Settings.correct(text,corrections)
    local keys={}
    if type(corrections)~="table" then return text end
    for from,to in pairs(corrections) do
        if type(from)=="string" and from~="" and type(to)=="string" and to~="" then keys[#keys+1]=from end
    end
    table.sort(keys,function(a,b) return #a==#b and a<b or #a>#b end)
    local lower=text:lower();local result={};local i=1
    while i<=#text do
        local found=false
        for _,from in ipairs(keys) do
            local last=i+#from-1
            if lower:sub(i,last)==from:lower() and not word_byte(text:sub(i-1,i-1))
                    and not word_byte(text:sub(last+1,last+1)) then
                result[#result+1]=corrections[from];i=last+1;found=true;break
            end
        end
        if not found then result[#result+1]=text:sub(i,i);i=i+1 end
    end
    return table.concat(result)
end
function Settings.escape(text)
    return (text:gsub('&','&amp;'):gsub('<','&lt;'):gsub('>','&gt;'):gsub('"','&quot;'):gsub("'",'&apos;'))
end
function Settings.ssml(text,options)
    text=Settings.correct(text,options.corrections)
    local out={"<speak>"}
    for i=1,#text do
        local c=text:sub(i,i);out[#out+1]=Settings.escape(c)
        -- RHVoice keeps explicit SSML breaks in wall-clock milliseconds,
        -- independently of relative speech rate (verified on the ARM runtime).
        if options.comma>0 and c:match("[,;:]") and not ((c==":" or c==",") and text:sub(i-1,i-1):match("%d") and text:sub(i+1,i+1):match("%d")) then
            out[#out+1]=string.format('<break time="%dms"/>',math.floor(options.comma*1000+0.5))
        end
    end
    out[#out+1]="</speak>";return table.concat(out)
end
function Settings.signature(options)
    local fields={options.voice,tostring(options.pitch),tostring(options.comma)}
    local keys={};for k,v in pairs(type(options.corrections)=="table" and options.corrections or {}) do
        if type(k)=="string" and type(v)=="string" then keys[#keys+1]=k end
    end
    table.sort(keys)
    for _,k in ipairs(keys) do local v=options.corrections[k];fields[#fields+1]=#k..":"..k..#v..":"..v end
    return table.concat(fields,"|")
end
function Settings.installed(engine)
    local result={};local root=(engine.plugin_dir or ""):gsub("/+$","").."/rhvoice/data/voices/"
    for _,v in ipairs(Settings.voices) do
        local info=io.open(root..v.folder.."/voice.info","rb")
        local data=io.open(root..v.folder.."/24000/voice.data","rb")
        if info and data then result[#result+1]=v end
        if info then info:close() end;if data then data:close() end
    end
    return result
end
function Settings.applyPreset(plugin,preset)
    plugin:stopReadAlong()
    plugin:setSetting("speech_rate",preset.rate);plugin.tts_engine:setRate(preset.rate)
    plugin:setSetting("word_speech_rate",preset.word_rate)
    for k,v in pairs({pitch=preset.pitch,comma_pause=preset.comma,sentence_pause=preset.sentence,paragraph_pause=preset.paragraph}) do
        plugin:setSetting("rhvoice_"..k,v)
    end
end
return Settings
