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
-- KOReader/LuaSocket can define sendto before the plugin is loaded. FFI
-- declarations are global: exercise both orders, not only a fresh Lua VM.
local load_order=arg[1] or 'standalone'
local function load_socket_header()
    ffi.cdef[[
        struct sockaddr { unsigned short sa_family; char sa_data[14]; };
        long sendto(int, const void *, size_t, int, const struct sockaddr *, unsigned int);
    ]]
end
if load_order=='koreader-first' then load_socket_header() end
local Client=dofile('rhvoiceclient.lua')
if load_order=='plugin-first' then load_socket_header() end
local lfs=require('lfs')
local checks=0
local function check(v,msg) checks=checks+1;assert(v,msg) end
local binary=os.tmpname();os.remove(binary)
local f=assert(io.open('dev/fixtures/rhvoice_worker.py','rb'));local script=f:read('*a');f:close()
f=assert(io.open(binary,'wb'));f:write(script);f:close();assert(os.execute('chmod 700 '..binary)==0)
local c=assert(Client:new{binary=binary,idle_seconds=1})
local reloaded=assert(dofile('rhvoiceclient.lua'):new{binary=binary})
check(c.directory~=reloaded.directory,'reloading module in the same second cannot reuse session directory')
reloaded:shutdown();lfs.rmdir(reloaded.directory)
local output=c.directory..'/delivered.wav'
local result,reason,count
local function finished(ok,why) result=ok;reason=why;count=(count or 0)+1 end
local function current() return true end
local function request(text,rate) result=nil;reason=nil;return c:request(text,output,rate or .5,1,350,current,finished) end
request('reader');check(pump(function() return result~=nil end,5) and result,'request succeeds through real Lua IPC')
local first=c.pid;check(c:status().starts==1 and c:status().pending==0,'first request starts one worker and cleans request files')
local size=lfs.attributes(output,'size');os.remove(output)
request('reader',1);check(pump(function() return result~=nil end,3) and result and c.pid==first,'second speed reuses same process')
check(lfs.attributes(output,'size')<size,'per-request rate reaches worker');os.remove(output)
local slow=request('slow');check(pump(function() return slow.sent end,2),'slow request entered worker')
slow:cancel();request('reader');local before=count
check(pump(function() return result~=nil end,3) and result and count==before+1,'cancelled old request has no callback or delayed playback')
check(c.pid==first and c.starts==1,'cancel does not reload voice');os.remove(output)
request('fail');check(pump(function() return result~=nil end,3) and result==false and reason:find('failed'),'worker failure is reported')
check(pump(function() return not c:alive() end,4),'idle worker unloads itself')
request('reader');check(pump(function() return result~=nil end,4) and result and c.pid~=first and c.starts==2,'request restarts idle worker')
os.remove(output)
request('crash');check(pump(function() return result~=nil end,3) and result==false and reason:find('stopped'),'worker death is surfaced without hanging')
request('reader');check(pump(function() return result~=nil end,4) and result,'next request recovers after worker crash');os.remove(output)
local timed=request('slow');check(pump(function() return timed.sent end,2),'worker has an in-flight request before timeout')
ui.offset=61
check(pump(function() return result~=nil end,2) and result==false and reason:find('timed out'),'request timeout is reported')
check(pump(function() return not c:alive() end,2),'timed out worker is terminated before retry')
ui.offset=0
request('reader');check(pump(function() return result~=nil end,4) and result,'next request recovers after timeout');os.remove(output)
local j=request('slow');check(pump(function() return j.sent end,2),'pending request exists before shutdown')
before=count;c:shutdown();check(pump(function() return not c:alive() and c:status().pending==0 end,3),'shutdown terminates worker and cleans pending request')
check(count==before,'shutdown never delivers pending callback')
request('reader');check(result==false and not c:alive(),'closed client cannot restart worker')
for name in lfs.dir(c.directory) do if name~='.' and name~='..' then os.remove(c.directory..'/'..name) end end
lfs.rmdir(c.directory);os.remove(binary)
print('ok - '..checks..' resident worker IPC and cancellation checks passed ('..load_order..')')
