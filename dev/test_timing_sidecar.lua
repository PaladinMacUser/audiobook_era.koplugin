#!/usr/bin/env luajit
-- Smoke test for the snt_server timing sidecar path in piperqueue.lua.
-- Run from the repo root: luajit dev/test_timing_sidecar.lua
--
-- The fixture below is verbatim output from the pq8 engine for
-- "Hello, world. This is a test of word timing.", so these checks also
-- pin the on-the-wire format.

local here = (arg and arg[0]):gsub("[^/]+$", "")

package.preload["ui/uimanager"] = function() return {} end
package.preload["ui/time"] = function() return { to_ms = function() return 0 end } end
package.preload["logger"] = function()
    return setmetatable({}, { __index = function() return function() end end })
end
package.preload["audiobook_gettext"] = function() return function(s) return s end end

local PiperQueue = dofile(here .. "../piperqueue.lua")

local failures, checks = {}, 0
local function ok(cond, label)
    checks = checks + 1
    if not cond then failures[#failures + 1] = label end
end

local tmp = os.tmpname() .. "_snts"
os.execute("mkdir -p " .. tmp)
local function write(name, body)
    local p = tmp .. "/" .. name
    local f = io.open(p, "w")
    f:write(body)
    f:close()
    return p
end

local HEADER = "sntt 1 22050 256 85008 3 9\n"
local CLAUSES =
    "c 0 0 6 44 0 16363 0 1\n" ..
    "c 1 6 13 46 16363 37648 1 1\n" ..
    "c 2 13 44 46 37648 85008 2 7\n"
local WORDS =
    "w 0 0 13056\n"      .. "w 1 16363 29931\n" .. "w 2 37648 44560\n" ..
    "w 3 45072 48144\n"  .. "w 4 48656 50192\n" .. "w 5 50960 57872\n" ..
    "w 6 58896 62224\n"  .. "w 7 62736 68368\n" .. "w 8 69136 85008\n"
local FIXTURE = HEADER .. CLAUSES .. WORDS

local TEXT = "Hello, world. This is a test of word timing."

-- 1. Parsing.
local good = write("good.timing", FIXTURE)
local side = PiperQueue:_parseTimingSidecar(good)
ok(side ~= nil, "parses a well-formed sidecar")
ok(side and side.rate == 22050 and side.hop == 256, "rate and hop")
ok(side and side.total == 85008, "total samples")
ok(side and #side.clauses == 3 and #side.words == 9, "clause and word counts")
ok(side and side.clauses[3].char_start == 13 and side.clauses[3].word0 == 2
    and side.clauses[3].nwords == 7, "clause 3 fields")

-- 2. Rejections: nil is the fall-back signal, so each of these must be nil.
ok(PiperQueue:_parseTimingSidecar(tmp .. "/absent.timing") == nil, "missing file")
ok(PiperQueue:_parseTimingSidecar(
    write("v2.timing", FIXTURE:gsub("^sntt 1 ", "sntt 2 "))) == nil, "unknown version")
ok(PiperQueue:_parseTimingSidecar(
    write("short.timing", HEADER .. CLAUSES .. "w 0 0 13056\n" .. "w 1 16363 29931\n")) == nil,
    "truncated word list")
ok(PiperQueue:_parseTimingSidecar(write("empty.timing", "")) == nil, "empty file")

-- 3. Building per-word timing for one sentence.
local recs = PiperQueue:_buildSentenceTiming(side, 1, 3, 0, 85008, TEXT)
ok(recs ~= nil, "builds timing for a matching sentence")
ok(recs and #recs == 9, "one record per word")
ok(recs and recs[1].word == "Hello," and recs[1].start_time == 0, "first record")
ok(recs and recs[#recs].end_time == math.floor(85008 * 1000 / 22050),
    "last word anchored to the segment end (so the rescales divide to 1)")
local monotonic = true
for i = 2, #(recs or {}) do
    if recs[i].start_time < recs[i - 1].end_time then monotonic = false end
end
ok(monotonic, "records are non-overlapping and increasing")
ok(recs ~= nil and recs[9].end_time > recs[9].start_time, "last word non-empty")

-- 4. Word-count mismatch falls back instead of mis-aligning (espeak expands
-- "1984" into several spoken words, so the groups outnumber the words).
ok(PiperQueue:_buildSentenceTiming(side, 1, 3, 0, 85008,
    "The year 1984 was cold.") == nil, "word-count mismatch returns nil")

-- 5. Sentence-to-clause mapping.
local parts = { "Hello, world.", "This is a test of word timing." }
local ref = table.concat(parts, " ")
local plan, bounds = PiperQueue:_mapSentencesToClauses(side, parts, ref)
ok(plan ~= nil, "maps sentences onto clauses")
ok(plan and plan[1].first == 1 and plan[1].last == 2, "sentence 1 takes clauses 1-2")
ok(plan and plan[2].first == 3 and plan[2].last == 3, "sentence 2 takes clause 3")
-- The first sentence owns two clauses (the comma splits it), so its segment
-- runs to clause 3's start, not to clause 2's.
ok(bounds and bounds[1] == 0 and bounds[2] == 37648 and bounds[3] == 85008,
    "sample boundaries follow the clause starts")
ok(plan and plan[2].seg_end == side.total, "last segment ends at the WAV end")

-- A per-sentence segment split must reconstruct the sentence's own words.
local r2 = PiperQueue:_buildSentenceTiming(side, 3, 3, plan[2].seg_start, plan[2].seg_end, parts[2])
ok(r2 ~= nil and #r2 == 7, "second sentence builds 7 words over its segment")
ok(r2 and r2[#r2].end_time == math.floor((plan[2].seg_end - plan[2].seg_start) * 1000 / 22050),
    "second sentence anchored to its own segment")

-- 6. Mapping rejections -> nil, so the estimated split is kept.
ok(PiperQueue:_mapSentencesToClauses(side, { "unrelated text", "more" }, ref) == nil,
    "rejects parts that do not reassemble the request string")
ok(PiperQueue:_mapSentencesToClauses(side, { "Hello," }, "Hello,") == nil,
    "rejects parts that leave later clauses unclaimed")

-- 7. Timing-source counters, which the bug report reads.
local fake_engine = {}
local fake_queue = setmetatable({ engine = fake_engine }, { __index = PiperQueue })
fake_queue:_noteTimingSource(true)
fake_queue:_noteTimingSource(true)
fake_queue:_noteTimingSource(false)
ok(fake_engine._piper_timing_real == 2, "counts real-timing utterances")
ok(fake_engine._piper_timing_estimated == 1, "counts estimated utterances")
local pct = math.floor(100 * fake_engine._piper_timing_real
    / (fake_engine._piper_timing_real + fake_engine._piper_timing_estimated) + 0.5)
ok(pct == 67, "percentage the bug report prints, got " .. tostring(pct))

os.execute("rm -rf " .. tmp)

if #failures == 0 then
    print(string.format("ok - %d checks passed", checks))
else
    print(string.format("FAILED %d of %d:", #failures, checks))
    for _, f in ipairs(failures) do print("  - " .. f) end
    os.exit(1)
end
