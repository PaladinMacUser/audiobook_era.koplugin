-- Persistent, text-free failure metadata for the visible audio diagnostics.
local Diagnostics = {}
function Diagnostics.record(plugin,engine,stage,message)
    if not plugin or not plugin.setSetting then return end
    local failure={time=os.date("%Y-%m-%d %H:%M:%S"),engine=engine and engine.backend or "unknown",
        stage=stage,message=tostring(message or "Audio request failed"):sub(1,500),
        output=engine and engine.getPocketBookAudioDevice and engine:getPocketBookAudioDevice() or "auto"}
    plugin:setSetting("last_audio_failure",failure)
    -- Save promptly: a subsequent KOReader crash must not discard this failure.
    if plugin.settings and plugin.settings.flush then pcall(function() plugin.settings:flush() end) end
    if G_reader_settings and G_reader_settings.flush then pcall(function() G_reader_settings:flush() end) end
    return failure
end
function Diagnostics.describe(plugin,engine)
    local lines={"Selected engine: "..tostring(engine and engine.backend or "not initialized")}
    engine=plugin._last_audio_instance or engine
    if engine then
        lines[#lines+1]="Audio output: "..tostring(engine:getPocketBookAudioDevice())
        lines[#lines+1]="Audio player: "..tostring(engine.audio_player_type or "not opened yet")
        lines[#lines+1]="Speaker PCM: "..tostring(engine._pb_speaker and engine._pb_speaker.pcm or "not opened yet")
    end
    local service=plugin._rhvoice_service
    if service then local s=service:status()
        lines[#lines+1]=string.format("RHVoice: %s; starts %d; requests %d; pending %d",
            s.running and "loaded" or "idle",s.starts,s.requests,s.pending)
    end
    if plugin._last_audio_engine then lines[#lines+1]="Last audio request: "..plugin._last_audio_engine end
    if plugin._audio_test_status then lines[#lines+1]="Audio test: "..plugin._audio_test_status end
    local reader=plugin.word_reader
    if reader and reader.cache then local s=reader.cache:stats()
        lines[#lines+1]=string.format("Word cache: %d clips, %.1f MB; hits %d, misses %d",s.count,s.bytes/1048576,s.hits,s.misses)
    end
    local f=plugin:getSetting("last_audio_failure",nil)
    lines[#lines+1]=""
    lines[#lines+1]=type(f)=="table" and string.format("Latest failure (%s)\n%s / %s / %s\n%s",
        f.time or "unknown",f.engine or "unknown",f.output or "auto",f.stage or "unknown",f.message or "unknown")
        or "No saved audio failure."
    return table.concat(lines,"\n")
end
return Diagnostics
