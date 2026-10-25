local WordReader = dofile("wordreader.lua")
local checks = 0
local function check(value, message)
    checks = checks+1; assert(value, message)
end
check(WordReader.selectedWord("  reader  next sentence") == "reader", "speak only the first selected token")
check(WordReader.selectedWord("can't") == "can't", "keep apostrophes")
check(WordReader.selectedWord("mother-in-law") == "mother-in-law", "keep compound words")
check(not WordReader.selectedWord(" \n\t") and not WordReader.selectedWord(nil), "empty selections are ignored")
local synth, plays, stops = {}, {}, 0
local engine = {
    stop=function() stops=stops+1 end,
    synthesize=function(_, text, callback) synth[#synth+1]={text=text,callback=callback}; return nil end,
    play=function(_, on_word, on_complete, on_fail)
        plays[#plays+1]={on_word=on_word,on_complete=on_complete,on_fail=on_fail}; return true
    end,
}
local reader = WordReader:new{engine=engine}
check(not reader:read(" ") and #synth == 0, "empty selection launches no synthesis")
check(reader:read("reader remaining words") and synth[1].text == "reader", "one-word input reaches TTS")
synth[1].callback(true)
check(#plays == 1 and plays[1].on_word == nil, "word preview does not start read-along highlighting")
local before = stops
plays[1].on_complete()
check(not reader.active and stops == before+1, "completion stops after one clip")
check(#synth == 1, "completion never queues the next word or page")
reader:read("first"); reader:read("second")
synth[2].callback(true)
check(#plays == 1, "superseded synthesis cannot play later")
synth[3].callback(true)
check(#plays == 2, "latest selection plays")
reader:read("third")
before = stops
plays[2].on_complete()
check(stops == before and reader.active, "old completion cannot stop a newer word")
reader:stop(); synth[4].callback(true)
check(#plays == 2 and not reader.active, "cancelled request stays silent")
reader:read("failed"); synth[5].callback(false)
check(#plays == 2 and not reader.active, "synthesis failure never advances or plays")

-- Register the actual plugin's menu entries with small KOReader UI shims.
package.loaded["ui/widget/container/widgetcontainer"] = {extend=function(_, t) return t end}
package.loaded.logger = setmetatable({}, {__index=function() return function() end end})
package.loaded.audiobook_gettext = function(s) return s end
local Plugin = dofile("main.lua")
local ui = {scheduled={}}
function ui:scheduleIn(_, fn) self.scheduled[#self.scheduled+1] = fn end
local function seed(fn, name, value)
    for i=1,30 do
        local n = debug.getupvalue(fn,i)
        if not n then break end
        if n == name then debug.setupvalue(fn,i,value); return end
    end
    error("Missing test UI upvalue: " .. name)
end
seed(Plugin._registerHighlightButtons,"UIManager",ui)
seed(Plugin._registerDictButtons,"PLUGIN_PATH","./")
local chosen, registered, highlight = {}, {}, {}
local repeats=0
local plugin = setmetatable({_init_ok=true,ui={
    dictionary={addToDictButtons=function(_, b) registered[b.id]=b end},
    highlight={addToHighlightDialog=function(_, id, fn) highlight[id]=fn end}},
    repeatLastWord=function() repeats=repeats+1 end,
    readSelectedWord=function(_, text) chosen[#chosen+1]=text end}, {__index=Plugin})
plugin:_registerDictButtons()
check(registered.audiobook_read_word and registered.audiobook_read, "modern dictionary keeps both reading actions")
check(registered.audiobook_repeat_word,"modern dictionary offers repeat")
registered.audiobook_repeat_word.callback();check(repeats==1,"modern dictionary repeat uses the last word")
check(registered.audiobook_read_word.text == "Read word", "exact requested button label")
check(not registered.audiobook_read_word.show_func({is_wiki=true}), "exclude wiki actions")
registered.audiobook_read_word.callback({word="lookup",highlight={selected_text={text="selected"}}})
check(chosen[#chosen] == "selected", "modern dictionary uses the original selection")
local legacy = {}
plugin:onDictButtonsReady({lookupword="legacyword"},legacy)
local word_button, aloud_button, repeat_button
for _, row in ipairs(legacy) do for _, b in ipairs(row) do
    if b.id == "audiobook_read_word" then word_button=b end
    if b.id == "audiobook_read" then aloud_button=b end
    if b.id == "audiobook_repeat_word" then repeat_button=b end
end end
check(word_button and aloud_button, "KOReader 2026.03 dictionary has both actions")
check(repeat_button,"legacy dictionary offers repeat")
repeat_button.callback();check(repeats==2,"legacy dictionary repeat uses last word")
word_button.callback()
check(chosen[#chosen] == "legacyword", "legacy dictionary passes only its selected word")
plugin:_registerHighlightButtons()
check(highlight["15_read_word"] and highlight["15_read_aloud"], "text selection has both reading actions")
local popup = {selected_text={text="snapshot"},onClose=function(self) self.closed=true; self.selected_text=nil end}
highlight["15_read_word"](popup).callback()
check(popup.closed, "close text-selection popup before playback")
ui.scheduled[#ui.scheduled]()
check(chosen[#chosen] == "snapshot", "selection text survives popup closure")
local repeat_popup={onClose=function(self) self.closed=true end}
highlight["15_repeat_word"](repeat_popup).callback()
ui.scheduled[#ui.scheduled]()
check(repeat_popup.closed and repeats==3,"text selection closes popup then repeats last word")
print("ok - " .. checks .. " selected-word and menu integration checks passed")
