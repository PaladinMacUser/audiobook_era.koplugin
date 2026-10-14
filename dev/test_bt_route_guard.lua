#!/usr/bin/env luajit
-- Smoke test for BtUI.warnIfBtRouteSilent / BtUI._showBtSilentDialog.
-- Run from the repo root: luajit dev/test_bt_route_guard.lua
--
-- The guard exists so that a paired-but-disconnected Bluetooth device cannot
-- present as silent playback with no explanation.  What matters is that it
-- fires in exactly one situation (routed to Bluetooth, nothing connected) and
-- stays quiet in every other, so this pins the decision, not the wording.

package.preload["device"] = function()
    return { isKindle = function() return false end }
end
package.preload["ffi/util"] = function()
    return {
        -- Just enough of KOReader's template(): substitute %1, %2, ...
        template = function(s, arg1, arg2)
            local args = { arg1, arg2 }
            return (s:gsub("%%(.)", function(d)
                return tostring(args[tonumber(d)] or "")
            end))
        end,
    }
end
package.preload["audiobook_gettext"] = function()
    return function(s) return s end
end
package.preload["logger"] = function()
    return setmetatable({}, { __index = function() return function() end end })
end
package.preload["ui/widget/infomessage"] = function()
    return { new = function(self, o) return setmetatable(o or {}, { __index = self }) end }
end
package.preload["ui/widget/buttondialog"] = function()
    return { new = function(self, o) return setmetatable(o or {}, { __index = self }) end }
end
package.preload["ui/uimanager"] = function()
    local M = { shown = {} }
    function M:show(w) table.insert(self.shown, w) end
    function M:close() end
    function M:scheduleIn() end
    return M
end

local UIManager = require("ui/uimanager")
local BtUI = dofile((arg and arg[0]):gsub("[^/]+$", "") .. "../btui.lua")

local failures, checks = {}, 0
local function ok(cond, label)
    checks = checks + 1
    if not cond then failures[#failures + 1] = label end
end

local function makePlugin(opts)
    opts = opts or {}
    local settings = opts.settings or {}
    local bt = {
        getStackType = function() return opts.stack or "mtk" end,
        isPowered = function() return opts.powered ~= false end,
        anyAudioDeviceConnected = function() return opts.connected == true end,
        connect = function() return opts.connect_ok ~= false, opts.connect_err end,
    }
    -- Note: `cond and nil or bt` would yield bt, so this must be explicit.
    local bt_manager = bt
    if opts.no_bt_manager then bt_manager = nil end
    return {
        bt_manager = bt_manager,
        getSetting = function(self, k, d)
            local v = settings[k]
            if v == nil then return d end
            return v
        end,
        setSetting = function(self, k, v) settings[k] = v end,
    }
end

local function dialogCount() return #UIManager.shown end
local function reset() UIManager.shown = {} end

-- 1. Fires when routed to Bluetooth with nothing connected.
reset()
local p = makePlugin{ settings = { bt_device_name = "JBL Go 3" } }
ok(BtUI.warnIfBtRouteSilent(p, true) == true, "warns when BT route has no device")
ok(dialogCount() == 1, "shows exactly one dialog")
ok(UIManager.shown[1] and UIManager.shown[1].title
    and UIManager.shown[1].title:find("JBL Go 3", 1, true) ~= nil,
    "title names the saved device")

-- 2. Silent when a device is connected.
reset()
p = makePlugin{ connected = true }
ok(BtUI.warnIfBtRouteSilent(p, true) == false, "silent when a device is connected")
ok(dialogCount() == 0, "no dialog when connected")

-- 3. Silent when the route is not Bluetooth (built-in speaker).
reset()
p = makePlugin{}
ok(BtUI.warnIfBtRouteSilent(p, false) == false, "silent when not routed to BT")
ok(BtUI.warnIfBtRouteSilent(p, nil) == false, "silent when the route flag is absent")
ok(dialogCount() == 0, "no dialog off the BT route")

-- 4. Silent on Kindle, where BT is OS-managed and cannot be enumerated.
reset()
p = makePlugin{ stack = "kindle" }
ok(BtUI.warnIfBtRouteSilent(p, true) == false, "silent on Kindle")
ok(dialogCount() == 0, "no dialog on Kindle")

-- 5. Silent when the user turned the advisory off (bt_disconnect_check = 0).
reset()
p = makePlugin{ settings = { bt_disconnect_check = 0 } }
ok(BtUI.warnIfBtRouteSilent(p, true) == false, "respects bt_disconnect_check = 0")
ok(dialogCount() == 0, "no dialog when disabled")

-- 6. Warns once, not once per tick.
reset()
p = makePlugin{}
ok(BtUI.warnIfBtRouteSilent(p, true) == true, "first call warns")
ok(BtUI.warnIfBtRouteSilent(p, true) == false, "second call is deduped")
ok(dialogCount() == 1, "still only one dialog")

-- 7. A live device clears the flag, so a later drop warns again.
reset()
p = makePlugin{}
BtUI.warnIfBtRouteSilent(p, true)
ok(p._bt_silent_warned == true, "flag set after warning")
p.bt_manager.anyAudioDeviceConnected = function() return true end
ok(BtUI.warnIfBtRouteSilent(p, true) == false, "no warn once a device connects")
ok(p._bt_silent_warned == nil, "flag cleared by a live device")
p.bt_manager.anyAudioDeviceConnected = function() return false end
ok(BtUI.warnIfBtRouteSilent(p, true) == true, "warns again after a re-drop")

-- 8. Bluetooth off is reported as off, not as a missing device.
reset()
p = makePlugin{ powered = false, settings = { bt_device_name = "JBL Go 3" } }
ok(BtUI.warnIfBtRouteSilent(p, true) == true, "warns when Bluetooth is off")
local title = UIManager.shown[1] and UIManager.shown[1].title or ""
ok(title:lower():find("off", 1, true) ~= nil, "title says Bluetooth is off, got: " .. title)

-- 9. No bt_manager at all (plugin mid-init) must not error.
reset()
p = makePlugin{ no_bt_manager = true }
ok(pcall(BtUI.warnIfBtRouteSilent, p, true) == true, "no error without bt_manager")
ok(dialogCount() == 0, "no dialog without bt_manager")

-- 10. The dialog offers Reconnect only when an address is known.
reset()
p = makePlugin{ settings = { bt_device_name = "JBL Go 3", bt_device_addr = "F8:5C:7D:D7:CB:CA" } }
BtUI.warnIfBtRouteSilent(p, true)
local btns = UIManager.shown[1].buttons or {}
ok(#btns == 2, "reconnect + play anyway when the address is known, got " .. #btns)
reset()
p = makePlugin{ settings = { bt_device_name = "JBL Go 3" } }
BtUI.warnIfBtRouteSilent(p, true)
btns = UIManager.shown[1].buttons or {}
ok(#btns == 1, "play anyway only when the address is unknown, got " .. #btns)

if #failures == 0 then
    print(string.format("ok - %d checks passed", checks))
else
    print(string.format("FAILED %d of %d:", #failures, checks))
    for _, f in ipairs(failures) do print("  - " .. f) end
    os.exit(1)
end
