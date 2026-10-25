-- Compatibility for KOReader v2026.03's missing-touch-origin crash on PB700.
-- No core files are changed. Valid gestures still use KOReader's detector.
local Guard = {}

function Guard.install(detector, device, revision)
    if not device or device.model ~= "PB700" or type(revision) ~= "string"
            or not revision:match("^v?2026%.03") then return false end
    if type(detector) ~= "table" or type(detector.newContact) ~= "function" then return false end
    if detector._audiobook_touch_origin_guard then return true end
    local function complete(event)
        return type(event) == "table" and event.timev ~= nil and event.x ~= nil and event.y ~= nil
    end
    local function protect(contact)
        local class = getmetatable(contact)
        if type(class) ~= "table" or type(class.isTwoFingerTap) ~= "function"
                or class._audiobook_touch_origin_guard then return end
        local original = class.isTwoFingerTap
        class.isTwoFingerTap = function(self, buddy, ...)
            -- An interrupted contact may lose its origin while still being
            -- paired. It cannot qualify as a two-finger tap. Returning false
            -- lets the existing tap state perform its normal cleanup.
            if not buddy or not complete(self.initial_tev) or not complete(self.current_tev)
                    or not complete(buddy.initial_tev) or not complete(buddy.current_tev) then
                return false
            end
            return original(self, buddy, ...)
        end
        class._audiobook_touch_origin_guard = true
    end
    for _, contact in pairs(detector.active_contacts or {}) do protect(contact) end
    local original_new = detector.newContact
    detector.newContact = function(self, ...)
        local contact = original_new(self, ...)
        protect(contact)
        return contact
    end
    detector._audiobook_touch_origin_guard = true
    return true
end

return Guard
