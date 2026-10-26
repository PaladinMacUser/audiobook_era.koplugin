-- One selected word, one audio clip, no sentence/page-advance callbacks.
local WordReader = {}
WordReader.__index = WordReader
local serial=0
local path=debug.getinfo(1,"S").source:match("^@(.*/)") or "./"
local Cache

function WordReader:new(o)
    assert(o and o.engine, "WordReader requires a TTS engine")
    o.generation = 0
    o.active = false
    return setmetatable(o, self)
end

function WordReader.selectedWord(text)
    if type(text) ~= "string" then return nil end
    return text:match("%S+")
end

function WordReader.dictionaryText(popup)
    local selected = popup.highlight and popup.highlight.selected_text
    return selected and selected.text or popup.word or popup.lookupword
end

function WordReader:stop()
    self.generation = self.generation + 1
    if self.active then
        self.active = false
        self.engine:stop()
        local done=self._on_done;self._on_done=nil
        if done then done(nil) end
    end
end

function WordReader:wouldCoalesce(text,engine)
    if not self.active or not self.loading or not self.cache then return false end
    Cache=Cache or dofile(path.."wordcache.lua")
    return self.pending_key==Cache.key(engine or self.engine,self.selectedWord(text))
end

function WordReader:read(text)
    local word = self.selectedWord(text)
    if not word then return false end
    self.last_word=word
    return self:_speak(word,true)
end

function WordReader:repeatLast()
    if not self.last_word then return false end
    return self:read(self.last_word)
end

function WordReader:preview(text,on_done)
    return self:_speak(text,false,on_done)
end

function WordReader:_speak(text,use_cache,on_done)
    if use_cache and self.cache then Cache=Cache or dofile(path.."wordcache.lua") end
    local key=use_cache and self.cache and Cache.key(self.engine,text)
    if self.active and self.loading and key and self.pending_key==key then return true end
    self:stop()
    self.generation = self.generation + 1
    self._on_done=on_done
    local generation = self.generation
    self.active = true
    self.engine:stop()
    self.loading=true;self.pending_key=key
    local function current()
        return self.active and self.generation == generation
    end
    local function finish(ok)
        if not current() then return end
        self.active = false
        self.loading=false
        self.engine:stop()
        self._on_done=nil
        if on_done then on_done(ok~=false) end
    end
    local started = true
    local function play()
        self.loading=false
        local failure_serial=self.engine._failure_serial
        if not self.engine:play(nil,function() finish(true) end,function() finish(false) end) then
            started=false
            if self.engine.recordAudioFailure and self.engine._failure_serial==failure_serial then
                self.engine:recordAudioFailure("playback","Could not open the selected audio output")
            end
            finish(false)
        end
    end
    local entry=key and self.cache:get(key)
    if entry then
        serial=serial+1
        local target="/tmp/audiobook_word_cache_"..os.time().."_"..serial..".wav"
        if self.cache:restore(entry,target) then
            self.engine.current_audio_file=target
            self.engine.timing_data={}
            self.engine._audio_lead_in_file=target;self.engine._audio_lead_in_ms=entry.lead
            play();return started
        end
    end
    local result = self.engine:synthesize(text, function(ok)
        if not current() then return end
        if not ok then started = false; finish(false); return end
        if key and Cache.key(self.engine,text)==key then
            self.cache:put(key,self.engine.current_audio_file,
                self.engine.getAudioLeadInMs and self.engine:getAudioLeadInMs() or 0)
        end
        play()
    end)
    if result == false then started = false; finish(false) end
    return started
end

return WordReader
