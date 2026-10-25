-- Regressions for word boxes on KOReader's rolling/EPUB view. Exercise the
-- real highlightWord -> _wordScreenBoxes path without an e-ink display.
package.loaded["ffi/blitbuffer"] = {}
package.loaded.device = {screen = {}}
package.loaded["ui/geometry"] = {}
package.loaded["ui/uimanager"] = {}
package.loaded.logger = setmetatable({}, {__index=function() return function() end end})
package.loaded.audiobook_gettext = function(s) return s end
local HighlightManager = dofile("highlightmanager.lua")
local checks = 0
local function check(value, message)
    checks = checks + 1
    assert(value, message)
end

local function fixture(boxes, selection)
    local cum = {[0]=0}
    for i = 1, #boxes do cum[i] = i*10 end
    local calls = 0
    local doc = {getTextFromPositions = function(_, a, b)
        calls = calls+1
        if selection then return selection(a, b) end
        return nil
    end}
    local h = HighlightManager:new{
        ui = {rolling = {}, document = doc},
        _word_ctx = {n=#boxes, cum=cum, sboxes=boxes},
    }
    return h, function() return calls end
end
local line = {x=10,y=20,w=100,h=20}
-- This is the screenshot's crash: hi_c was local to a different function.
-- Leave the global unset so the regression cannot pass by accident.
_G.hi_c = nil
local h, calls = fixture({line}, function(a, b)
    return {text=string.rep("x", math.max(0, math.floor((b.x-a.x)/10)))}
end)
local boxes = h:_wordScreenBoxes(2, 5)
check(boxes and #boxes == 1, "single-line word must produce a box without crashing")
check(calls() > 2, "single-line glyph-boundary refinement runs")
check(boxes[1].x >= line.x and boxes[1].x+boxes[1].w <= line.x+line.w,
    "refined box stays on its line")
check(_G.hi_c == nil, "line length must not depend on a global")

h = fixture({line})
boxes = h:_wordScreenBoxes(2, 5)
check(boxes and boxes[1].x == 30 and boxes[1].w == 30,
    "missing native selection uses proportional word geometry")
h = fixture({line}, function() error("selection unavailable") end)
check(h:_wordScreenBoxes(2, 5), "selection errors do not interrupt highlighting")

-- A wrapped word normally ends further left than where it started.
h = fixture({line, {x=10,y=50,w=100,h=20}})
boxes = h:_wordScreenBoxes(8, 12)
check(boxes and #boxes == 2, "word crossing a line boundary keeps both boxes")
check(boxes[1].x == 90 and boxes[1].w == 20, "wrapped word covers first line's tail")
check(boxes[2].x == 10 and boxes[2].w == 20, "wrapped word covers next line's start")

-- When the second line starts further right, the old code reached its
-- last-line fallback but assigned nil to bw, causing a second crash.
h = fixture({line, {x=200,y=50,w=100,h=20}})
boxes = h:_wordScreenBoxes(8, 12)
check(boxes and #boxes == 2, "indented wrapped word must not compare nil width")
check(boxes[2].x == 200 and boxes[2].w == 20, "last line keeps its own x origin")
h = fixture({line, {x=10,y=50,w=100,h=20}, {x=10,y=80,w=100,h=20}})
boxes = h:_wordScreenBoxes(8, 22)
check(boxes and #boxes == 3 and boxes[2].w == 100, "middle lines retain their full width")

-- Native crengine boxes remain preferred when available.
h = fixture({line}, function() return {text="word",pos0="start",pos1="end"} end)
h.ui.document.getScreenBoxesFromPositions = function()
    return {{x=35,y=20,w=25,h=20}}
end
boxes = h:_wordScreenBoxes(2, 5)
check(boxes and boxes[1].x == 35 and boxes[1].w == 25, "use native glyph boxes")

-- Follow several scheduled word updates through the public entry point.
h = fixture({line})
h._word_ctx.sent_text = "one two end"
h._word_ctx.vis_start = 1 -- Lua string indices in the real sentence cache
h._word_ctx.cum[1] = #h._word_ctx.sent_text
h._boxesMatchCurrentView = function() return true end
h._ensureViewModule = function() end
local refreshed = 0
h._refreshWordHighlight = function(_, b)
    check(b and #b > 0, "word update supplies visible geometry")
    refreshed = refreshed+1
end
for _, word in ipairs({"one", "two", "end"}) do h:highlightWord({text=word}) end
check(refreshed == 3, "consecutive word updates continue without a KOReader crash")
h.ui.rolling = nil
h:highlightWord({text="one"})
check(refreshed == 3, "non-rolling documents keep their existing behavior")
print("ok - " .. checks .. " word highlighting checks passed")
