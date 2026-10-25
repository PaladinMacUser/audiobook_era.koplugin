-- Use the actual C player and KOReader-style socket types through the Lua IPC.
local ffi=require('ffi')
ffi.cdef[[int usleep(unsigned int);int gettimeofday(void*,void*);typedef struct {long sec;long usec;} ab_player_clock;
struct sockaddr { unsigned short family;char data[14]; };
long sendto(int,const void*,size_t,int,const struct sockaddr*,unsigned int);]]
local function now() local t=ffi.new('ab_player_clock');ffi.C.gettimeofday(t,nil);return tonumber(t.sec)+tonumber(t.usec)/1e6 end
local ui={scheduled={}}
function ui:getTime() return now() end
function ui:scheduleIn(delay,fn) self.scheduled[#self.scheduled+1]={at=now()+delay,fn=fn} end
local function pump(predicate,seconds)
    local deadline=now()+seconds
    repeat
        for i=#ui.scheduled,1,-1 do local p=ui.scheduled[i];if p.at<=now() then table.remove(ui.scheduled,i);p.fn() end end
        if predicate() then return true end;ffi.C.usleep(10000)
    until now()>deadline
    return predicate()
end
package.loaded['ui/uimanager']=ui;package.loaded['ui/time']={to_s=function(s) return s end}
package.loaded.audiobook_gettext=function(s) return s end
package.loaded.logger=setmetatable({}, {__index=function() return function() end end})
local Client=dofile('rhvoiceclient.lua');local Wav=dofile('wavutils.lua');local lfs=require('lfs')
local c=assert(Client:new{binary=assert(os.getenv('ERA_PLAYER_TEST_BIN')),idle_seconds=1,arguments={'--device','hwout_mix'}})
local audio=c.directory..'/clip.wav';assert(Wav.generateSilence(audio,950,24000))
local result,reason,lead
local checks=0
local function check(v,msg) checks=checks+1;assert(v,msg) end
local function request(timeout)
    result=nil;reason=nil;lead=nil
    return c:request(audio,'/tmp/audiobook-unused-player-output.wav',1,1,350,function() return true end,
        function(ok,why) result=ok;reason=why end,
        {no_output=true,label='Era speaker',timeout=timeout or 5,on_playing=function(ms) lead=ms end})
end
request();check(pump(function() return result~=nil end,4) and result and lead==350,'actual player acknowledges fresh guarded playback without generating an output WAV')
local pid=c.pid
local job=request();check(pump(function() return lead~=nil end,2) and lead==0,'warm playback acknowledgement updates the lead-in')
job:pause();check(not pump(function() return result~=nil end,.2),'paused clip cannot complete or time out prematurely')
job:resume();check(pump(function() return result~=nil end,3) and result and c.pid==pid,'resume finishes in the same speaker process')
job=request();check(pump(function() return lead~=nil end,2),'cancellable clip starts')
job:cancel();check(pump(function() return job.finished end,2) and result==nil,'cancelled playback never calls completion')
request(.2);check(pump(function() return result~=nil end,2) and result==false and reason:find('timed out'),'playback timeout is reported instead of crashing UI')
check(pump(function() return not c:alive() end,2),'timed-out player terminates and releases PCM')
request();check(pump(function() return result~=nil end,4) and result and lead==350,'next request restarts with the full speaker guard')
c:shutdown();check(pump(function() return not c:alive() end,2),'shutdown stops the tracked player')
for name in lfs.dir(c.directory) do if name~='.' and name~='..' then os.remove(c.directory..'/'..name) end end;lfs.rmdir(c.directory)
print('ok - '..checks..' real Lua/PCM IPC, acknowledgement, pause and cancellation checks passed')
