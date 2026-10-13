#!/usr/bin/env luajit
-- Smoke test for WavUtils.splitFileAtSamples (real word-level TTS timing).
-- Run from the repo root: luajit dev/test_split_at_samples.lua
-- wavutils.lua only needs the "logger" module, so a stub is enough.

local here = (arg and arg[0]):gsub("[^/]+$", "")
package.preload["logger"] = function()
    return setmetatable({}, { __index = function() return function() end end })
end

local WavUtils = dofile(here .. "../wavutils.lua")

local failures, checks = {}, 0
local function ok(cond, label)
    checks = checks + 1
    if not cond then failures[#failures + 1] = label end
end

local tmp = os.tmpname() .. "_snt"
os.execute("mkdir -p " .. tmp)

-- Build a deterministic mono/16-bit WAV.
local RATE, N = 22050, 5000
local function le16(v) return string.char(v % 256, math.floor(v / 256) % 256) end
local function le32(v)
    return string.char(v % 256, math.floor(v / 256) % 256,
        math.floor(v / 65536) % 256, math.floor(v / 16777216) % 256)
end
local pcm = {}
for i = 0, N - 1 do pcm[i] = ((i * 37) % 2000) - 1000 end
local function pcm_bytes(tbl, first, last)  -- inclusive
    local out = {}
    for i = first, last do
        local v = tbl[i]
        out[#out + 1] = le16(v < 0 and (v + 65536) or v)
    end
    return table.concat(out)
end
local data = pcm_bytes(pcm, 0, N - 1)
local src = tmp .. "/src.wav"
do
    local f = io.open(src, "wb")
    f:write("RIFF", le32(36 + #data), "WAVE", "fmt ", le32(16),
        le16(1), le16(1), le32(RATE), le32(RATE * 2), le16(2), le16(16),
        "data", le32(#data))
    f:write(data)
    f:close()
end

-- 1. Split at exact sample boundaries; concatenated PCM must reproduce source.
local bounds = { 0, 1234, 2000, 2001, N }
local outs = {}
for i = 1, #bounds - 1 do outs[i] = tmp .. "/seg" .. i .. ".wav" end
ok(WavUtils.splitFileAtSamples(src, bounds, outs), "split returns true")

local got = {}
for i = 1, #outs do
    local f = io.open(outs[i], "rb")
    local hdr = f:read(44)
    local size = WavUtils.readLE32(hdr:sub(41, 44))
    got[i] = f:read(size) or ""
    f:close()
    ok(size == (bounds[i + 1] - bounds[i]) * 2,
        "segment " .. i .. " byte size")
end
ok(table.concat(got) == data, "concatenated segments reproduce the source PCM")

-- 2. The property the word-timing anchoring relies on: a segment's measured
-- duration equals floor(samples * 1000 / rate) for the same sample count, so
-- a timing builder and getDurationMs agree exactly and the rescale sites
-- divide to 1.
for i = 1, #outs do
    local seg_samples = bounds[i + 1] - bounds[i]
    ok(math.floor(seg_samples * 1000 / RATE) == WavUtils.getDurationMs(outs[i]),
        "segment " .. i .. " ms agrees with its sample count")
end

-- 3. Guard rails.
ok(not WavUtils.splitFileAtSamples(src, { 0, N }, {}), "rejects wrong boundary count")
ok(not WavUtils.splitFileAtSamples(src, { 0, 900, 100, N }, { tmp .. "/a", tmp .. "/b", tmp .. "/c" }),
    "rejects non-monotonic boundaries")

os.execute("rm -rf " .. tmp)

if #failures == 0 then
    print(string.format("ok - %d checks passed", checks))
else
    print(string.format("FAILED %d of %d:", #failures, checks))
    for _, f in ipairs(failures) do print("  - " .. f) end
    os.exit(1)
end
