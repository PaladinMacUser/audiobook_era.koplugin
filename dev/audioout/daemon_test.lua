#!/usr/bin/env luajit
-- Protocol test for the audioout daemon. Runs wherever luajit runs (host
-- for --sink fakesink, device for the MTK sink).
--
--   luajit dev/audioout/daemon_test.lua <socket> <wav> [expected_ms]
--
-- Assumes the daemon is already running on <socket> (start it from the
-- shell so logs stay visible). Exits 0 on PASS, 1 on FAIL.

local function fail(msg)
    print("FAIL - " .. msg)
    os.exit(1)
end

local socket_path = arg[1] or fail("usage: daemon_test.lua <socket> <wav> [expected_ms]")
local wav = arg[2] or fail("usage: daemon_test.lua <socket> <wav> [expected_ms]")
local expected_ms = tonumber(arg[3] or "") or 30000

-- Stub the KOReader modules audioout.lua touches.
package.preload.logger = function()
    return { warn = function() end, dbg = function() end, err = function() end }
end
-- Minimal flat-table JSON good enough for the daemon protocol.
package.preload.json = function()
    return (function()
    local function encode(v)
        if type(v) == "number" then
            local n = math.floor(v)
            return n == v and tostring(n) or string.format("%.3f", v)
        end
        if type(v) == "boolean" then return v and "true" or "false" end
        if type(v) == "string" then return string.format('"%s"', v) end
        local parts = {}
        for k, val in pairs(v) do
            parts[#parts + 1] = string.format('"%s":%s', k, encode(val))
        end
        return "{" .. table.concat(parts, ",") .. "}"
    end
    local function decode(s)
        local t = {}
        for k, v in s:gmatch('"([^"]+)":%s*("?[^,}"]*"?)') do
            v = v:gsub('^"', ""):gsub('"$', "")
            if v == "true" then t[k] = true
            elseif v == "false" then t[k] = false
            elseif tonumber(v) then t[k] = tonumber(v)
            else t[k] = v end
        end
        return t
    end
    return { encode = encode, decode = decode }
end)()
end

local here = (arg[0]:match("^(.*)/") or ".")
local function first_existing(p)
    local f = io.open(p, "r")
    if f then f:close() return p end
    return nil
end
local mod = first_existing(here .. "/audioout.lua")
        or first_existing(here .. "/../../audioout.lua")
        or fail("audioout.lua not found next to or above the test")
local AudioOut = dofile(mod)
local ao = AudioOut:new{ socket_path = socket_path }

local checks = 0
local function ok(cond, name)
    checks = checks + 1
    if not cond then fail(name) end
    print("ok - " .. name)
end

-- 1. liveness
local resp = ao:_command({ cmd = "ping" }, 1000)
ok(resp and resp.ok, "ping answers")

-- 2. clean slate
ao:flush()
resp = ao:status()
ok(resp and resp.ok and resp.queue == 0, "status after flush, queue empty")
local t0_fed = resp.fed_ms

-- 3. queue the tone
resp = ao:play(wav, { rate = 22050, channels = 1 })
ok(resp and resp.ok, "play accepted")
ok(math.abs(resp.fed_end_ms - t0_fed - expected_ms) < 1500,
   string.format("fed_end_ms ~= +%d (%+d)", expected_ms,
                 resp.fed_end_ms - t0_fed - expected_ms))
local fed_end = resp.fed_end_ms

-- 4. position advances
local last_pos = -1
local advanced = false
for _ = 1, 12 do
    os.execute("sleep 0.25")
    resp = ao:status()
    if resp.pos_ms > last_pos + 100 then advanced = true end
    last_pos = resp.pos_ms
end
ok(advanced, string.format("pos advances (at %d ms)", last_pos))

-- 5. pause freezes-ish (buffered tail still plays out: allow +2500)
resp = ao:pause()
ok(resp and resp.ok, "pause accepted")
os.execute("sleep 0.5")
local p1 = ao:status()
os.execute("sleep 1.0")
local p2 = ao:status()
ok(p2.pos_ms >= p1.pos_ms, "pos does not go backwards on pause")
resp = ao:resume()
ok(resp and resp.ok, "resume accepted")

-- 6. completion
local done0 = (ao:status() or {}).done_count or 0
local deadline = expected_ms / 1000 + 15
local t = 0
local finished = false
while t < deadline do
    os.execute("sleep 0.5")
    t = t + 0.5
    resp = ao:status()
    if (resp.done_count or 0) > done0 or resp.state == "idle" then
        finished = true
        break
    end
end
ok(finished, string.format("clip completes (pos=%d, state=%s, t=%.1fs)",
                           resp.pos_ms or -1, tostring(resp.state), t))
ok(math.abs((resp.pos_ms or 0) - fed_end) < 2500,
   string.format("final pos ~= fed_end (delta %d ms)",
                 (resp.pos_ms or 0) - fed_end))

-- 7. stop / flush round trip
resp = ao:play(wav, { rate = 22050, channels = 1 })
ok(resp and resp.ok, "re-queue ok")
resp = ao:stop()
ok(resp and resp.ok and resp.state == "idle", "stop drains queue")
resp = ao:flush()
ok(resp and resp.ok, "flush rebuild ok")
resp = ao:status()
ok(resp.fed_end_ms == 0, "fed_end reset by flush")

print(string.format("PASS - %d checks", checks))
