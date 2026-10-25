-- One private resident RHVoice process, shared by a book's speech sessions.
-- Datagrams never block the UI or raise SIGPIPE if a worker disappears.
local ffi = require("ffi")
local UIManager = require("ui/uimanager")
local time = require("ui/time")
pcall(ffi.cdef, [[typedef struct { unsigned short family; char path[108]; } ab_rh_address;]])
pcall(ffi.cdef, [[struct sockaddr; int socket(int,int,int);
    long sendto(int,const void*,size_t,int,const struct sockaddr*,unsigned int);]])
pcall(ffi.cdef, [[int close(int); int getpid(void); int kill(int,int); char *mkdtemp(char*);]])
local Client = {}; Client.__index = Client
local function quote(s) return "'" .. s:gsub("'", "'\\''") .. "'" end
local function read(path)
    local f=io.open(path,"rb");if not f then return nil end
    local s=f:read("*a");f:close();return s
end
local function write(path,s)
    local f=io.open(path..".new","wb");if not f then return false end
    local ok=f:write(s);local closed=f:close()
    if not ok or not closed then os.remove(path..".new");return false end
    return os.rename(path..".new",path) ~= nil
end
function Client:new(o)
    local pattern=string.format("/tmp/audiobook-rh-%d-XXXXXX",tonumber(ffi.C.getpid()))
    local directory=ffi.new("char[?]",#pattern+1,pattern)
    if ffi.C.mkdtemp(directory)==nil then return nil end -- atomic, unique, mode 0700
    o.directory=ffi.string(directory)
    o.socket=o.directory.."/service.sock";o.jobs={};o.next_id=0;o.starts=0;o.requests=0
    return setmetatable(o,self)
end
function Client:alive()
    if not self.pid or ffi.C.kill(self.pid,0)~=0 then return false end
    -- A stale PID must never authorize signalling a different process.
    local command=read("/proc/"..self.pid.."/cmdline") or ""
    return command:find(self.binary,1,true)~=nil
end
function Client:ready()
    return self:alive() and tonumber(read(self.socket..".ready"))==self.pid
end
function Client:start()
    if self.closed then return false end
    if self:alive() then return true end
    os.remove(self.socket..".ready")
    local arguments=""
    for _,arg in ipairs(self.arguments or {}) do arguments=arguments.." "..quote(arg) end
    local p=io.popen((self.launch_prefix or "")..quote(self.binary).." --daemon "..quote(self.socket)
        .." --idle-seconds "..tostring(self.idle_seconds or 120)..arguments
        .." >"..quote(self.directory.."/worker.log").." 2>&1 & echo $!")
    local result=p and p:read("*a") or "";if p then p:close() end
    self.pid=tonumber(result:match("(%d+)"))
    if self.pid then self.starts=self.starts+1 end
    return self.pid~=nil
end
function Client:request(text,output,rate,volume,lead,current,callback,options)
    options=options or {}
    self.next_id=self.next_id+1;self.requests=self.requests+1
    local base=self.directory.."/job-"..self.next_id
    local job={path=base..".req",input=base..".txt",output=base..".wav",external_output=output}
    self.jobs[job.path]=job
    local function cleanup()
        for _,path in ipairs({job.input,job.path,job.path..".done",job.path..".cancel",job.path..".pause",job.path..".playing",job.output}) do os.remove(path) end
        self.jobs[job.path]=nil
    end
    function job:cancel() if self.finished then return end;self.cancelled=true;write(self.path..".cancel","cancel\n") end
    function job:pause()
        if self.finished or self.paused_at then return end
        self.paused_at=UIManager:getTime();write(self.path..".pause","pause\n")
    end
    function job:resume()
        if self.paused_at then self.paused_seconds=(self.paused_seconds or 0)+time.to_s(UIManager:getTime()-self.paused_at);self.paused_at=nil end
        os.remove(self.path..".pause")
    end
    local function done(ok,reason)
        job.finished=true
        if ok and not options.no_output then
            -- Both paths are on /tmp; rename only after the worker has closed the complete WAV.
            ok=os.rename(job.output,output)~=nil
            if not ok then reason="Could not move RHVoice audio into the playback session" end
        end
        cleanup()
        if current() and not job.cancelled then callback(ok,reason) end
    end
    if not write(job.input,text) or not write(job.path,table.concat({job.input,job.output,
            string.format("%.3f",rate),string.format("%.3f",volume),tostring(lead),
            options.voice or "Slt",string.format("%.3f",options.pitch or 1),options.format or "text",""},"\n")) then
        done(false,"Cannot write RHVoice request");return job
    end
    if not self:start() then done(false,"Cannot start the RHVoice process");return job end
    local started=UIManager:getTime();local fd
    local function poll()
        local active=current() and not job.cancelled and not self.closed
        if not active and not job.cancelled then job:cancel() end
        if options.on_playing and not job.playing_notified then
            local playing=read(job.path..".playing")
            if playing and active then job.playing_notified=true;options.on_playing(tonumber(playing) or 0) end
        end
        local marker=read(job.path..".done")
        if marker then
            if fd then ffi.C.close(fd);fd=nil end
            done(active and marker:match("^OK")~=nil,marker:match("^ERR") and (options.no_output and marker:gsub("%s+$","") or "RHVoice synthesis failed") or nil)
            return
        end
        local elapsed=time.to_s(UIManager:getTime()-started)-(job.paused_seconds or 0)
            -(job.paused_at and time.to_s(UIManager:getTime()-job.paused_at) or 0)
        local timeout=options.timeout or 60
        if not self:alive() or elapsed>=timeout then
            if elapsed>=timeout and self:alive() then ffi.C.kill(self.pid,15) end
            if fd then ffi.C.close(fd);fd=nil end
            done(false,elapsed>=timeout and ((options.label or "RHVoice").." request timed out") or ((options.label or "RHVoice").." process stopped"))
            return
        end
        if active and not job.sent and self:ready() then
            fd=ffi.C.socket(1,2+2048,0) -- AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK
            if fd>=0 then
                local address=ffi.new("ab_rh_address");address.family=1
                ffi.copy(address.path,self.socket)
                -- KOReader may already have declared sendto with sockaddr*.
                -- The private Unix-address struct needs an explicit pointer cast.
                local count=ffi.C.sendto(fd,job.path,#job.path,0,
                    ffi.cast("const struct sockaddr *",address),2+#self.socket+1)
                ffi.C.close(fd);fd=nil
                if count==#job.path then job.sent=true end
            else fd=nil end
        elseif not active and not job.sent then job.finished=true;cleanup();return end
        UIManager:scheduleIn(0.1,poll)
    end
    UIManager:scheduleIn(0.1,poll)
    return job
end
function Client:status()
    local pending=0;for _ in pairs(self.jobs) do pending=pending+1 end
    return {running=self:ready(),starts=self.starts,requests=self.requests,pending=pending}
end
function Client:shutdown()
    self.closed=true
    for _,job in pairs(self.jobs) do job:cancel() end
    if self:alive() then ffi.C.kill(self.pid,15) end
end
return Client
