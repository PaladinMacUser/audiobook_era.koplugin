-- Real nonblocking Lua->Unix-datagram transport and worker lifecycle.
local ffi=require('ffi')
ffi.cdef[[int usleep(unsigned int); int gettimeofday(void*,void*); typedef struct {long sec;long usec;} ab_test_time;]]
local function now() local t=ffi.new('ab_test_time');ffi.C.gettimeofday(t,nil);return tonumber(t.sec)+tonumber(t.usec)/1e6 end
local ui={scheduled={}}
function ui:getTime() return now()+(self.offset or 0) end
function ui:scheduleIn(delay,fn) self.scheduled[#self.scheduled+1]={at=now()+delay,fn=fn} end
local function pump(until_true,seconds)
    local deadline=now()+seconds
    repeat
        for i=#ui.scheduled,1,-1 do local item=ui.scheduled[i];if now()>=item.at then table.remove(ui.scheduled,i);item.fn() end end
        if until_true() then return true end
        ffi.C.usleep(10000)
    until now()>deadline
    return until_true()
end
package.loaded['ui/uimanager']=ui
package.loaded['ui/time']={to_s=function(s) return s end}
local Client=dofile('rhvoiceclient.lua')
local c=assert(Client:new{binary=arg[1],launch_prefix="'"..arg[2].."' ",
    arguments={'--data',arg[3],'--phonemizer',arg[4]},idle_seconds=2})
local file=c.directory..'/delivered.wav'
local result,reason
local function request(rate,volume)
    result=nil
    c:request('reader',file,rate,volume,350,function()return true end,function(ok,why)result=ok;reason=why end,
        {voice='ItoFemale',pitch=1,format='text',label='Ito'})
    assert(pump(function()return result~=nil end,20), 'real Ito Lua request timed out')
    assert(result,reason)
    local f=assert(io.open(file,'rb'));local pcm=f:read('*a');f:close();os.remove(file);return pcm
end
local ok,err=pcall(function()
    local normal=request(1,1);local pid=c.pid
    assert(normal:sub(1,4)=='RIFF' and normal:sub(45,16844)==string.rep('\0',16800),'real client retains guard')
    local slow=request(.5,1)
    assert(c.pid==pid and #slow>#normal*1.5,'real client reuses model and sends rate')
    local mute=request(1,0)
    assert(mute:sub(45)==string.rep('\0',#mute-44),'real client sends true mute')
end)
c:shutdown();assert(pump(function()return not c:alive() end,5),'shutdown releases real ARM worker')
assert(ok,err)
print('ok - 4 real Lua-to-Ito ARM request, guarded WAV, rate, mute and shutdown checks')
