-- One selected word, one audio clip, no sentence/page-advance callbacks.
local WordReader = {}
WordReader.__index = WordReader

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
    end
end

function WordReader:read(text)
    local word = self.selectedWord(text)
    if not word then return false end
    self.generation = self.generation + 1
    local generation = self.generation
    self.active = true
    self.engine:stop()
    local function current()
        return self.active and self.generation == generation
    end
    local function finish()
        if not current() then return end
        self.active = false
        self.engine:stop()
    end
    local started = true
    local result = self.engine:synthesize(word, function(ok)
        if not current() then return end
        if not ok then started = false; finish(); return end
        -- No word highlighting, playback bar, or read-along controller:
        -- completion only releases the selected word's clip.
        if not self.engine:play(nil, finish, finish) then
            started = false
            finish()
        end
    end)
    if result == false then started = false; finish() end
    return started
end

return WordReader
