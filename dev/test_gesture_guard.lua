-- Reproduce the uploaded crash against KOReader's actual v2026.03 detector.
package.loaded.logger=setmetatable({}, {__index=function() return function() end end})
package.loaded["ui/time"]={ms=function(n) return n/1000 end,s=function(n) return n end,huge=math.huge}
package.loaded["ffi/posix_h"]={}
package.loaded.util={}
local Geom={}
function Geom:new(o) return setmetatable(o,{__index=self}) end
function Geom:distance(other) return math.sqrt((self.x-other.x)^2+(self.y-other.y)^2) end
function Geom:midpoint(other) return self:new{x=(self.x+other.x)/2,y=(self.y+other.y)/2} end
package.loaded["ui/geometry"]=Geom
G_reader_settings={readSetting=function() end}
local Guard=dofile("gestureguard.lua")
local checks=0
local function check(v,message) checks=checks+1;assert(v,message) end
local function fixture()
    local Detector=dofile("dev/fixtures/gesturedetector-v2026.03.lua")
    return Detector:new{active_contacts={},contact_count=0,clock_id=1,
        screen={scaleByDPI=function(_,n) return n end},
        input={main_finger_slot=0,clearTimeout=function() end}}
end
local function contacts(d)
    local a=d:newContact(0)
    a.initial_tev={timev=1,x=100,y=100};a.current_tev={timev=1.1,x=100,y=100,id=-1,slot=0}
    a.down=true
    local b=d:newContact(1)
    b.initial_tev={timev=1,x=150,y=100};b.current_tev={timev=1.1,x=150,y=100,id=2,slot=1}
    b.down=true
    return a,b
end
local d=fixture();local a,b=contacts(d)
check(a:isTwoFingerTap(b),"normal upstream two-finger tap is valid")
b.initial_tev=nil
local ok,err=pcall(a.isTwoFingerTap,a,b)
check(not ok and tostring(err):find("initial_tev",1,true),"reproduce the exact missing-initial_tev crash before the fix")
check(Guard.install(d,{model="PB700"},"v2026.03"),"install on the reported Era and KOReader version")
check(a:isTwoFingerTap(b)==false,"an interrupted buddy cannot cause the two-finger detector to crash")
a.state=getmetatable(a).tapState
local gestures=d:feedEvent({a.current_tev})
check(#gestures==1 and gestures[1].ges=="tap", "the real input state machine follows its existing single-tap fallback")
check(d.contact_count==1 and b.initial_tev~=nil,"upstream fallback repairs and cleans contact state")
b.current_tev.id=-1
d:feedEvent({b.current_tev})
check(d.contact_count==0,"both contacts can be released after the interrupted gesture")
local old_new=d.newContact
check(Guard.install(d,{model="PB700"},"v2026.03") and d.newContact==old_new,"file manager and reader initialization do not double-wrap")

d=fixture()
check(Guard.install(d,{model="PB700"},"v2026.03-12-gabc"),"cover development revisions based on the affected release")
a,b=contacts(d)
check(a:isTwoFingerTap(b),"newly created contacts retain normal two-finger taps")
a.state=getmetatable(a).tapState
gestures=d:feedEvent({a.current_tev})
check(#gestures==1 and gestures[1].ges=="two_finger_tap","the real state machine still emits valid two-finger gestures")

for _,case in ipairs({{model="PB632",version="v2026.03"},{model="PB700",version="v2026.04"},{model="PB700",version="unknown"}}) do
    d=fixture();old_new=d.newContact
    check(not Guard.install(d,{model=case.model},case.version) and d.newContact==old_new,"other devices and releases retain their detector")
end
check(not Guard.install({}, {model="PB700"},"v2026.03"),"unknown detector interfaces are left alone")
print("ok - "..checks.." real KOReader gesture crash and compatibility checks passed")
