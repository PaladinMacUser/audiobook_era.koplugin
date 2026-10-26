-- Conservative English ebook cleanup and sentence boundaries for RHVoice.
-- Source text/offsets remain unchanged; only the synthesis copy is cleaned.
local English={}
local substitutions={
    {"\194\160"," "}, {"\226\128\175"," "}, -- non-breaking spaces
    {"\194\173",""}, {"\226\129\160",""}, -- soft hyphen, word joiner
    {"\239\187\191",""}, -- byte-order mark
    {"’","'"}, {"‘","'"}, {"“",'"'}, {"”",'"'},
    {"…","..."},
    {"ﬀ","ff"}, {"ﬁ","fi"}, {"ﬂ","fl"}, {"ﬃ","ffi"}, {"ﬄ","ffl"},
}
function English.prepare(text)
    text=text:gsub("\194\173[ \t]*", "")
    for _,pair in ipairs(substitutions) do text=text:gsub(pair[1],pair[2]) end
    -- A discretionary hyphen can survive PDF line-unwrapping as "re­ ader".
    -- Do not guess at normal hyphens: mother-in-law and negative numbers stay.
    return text
end
local titles={mr=true,mrs=true,ms=true,dr=true,prof=true,rev=true,
    capt=true,lt=true,col=true,gen=true,sgt=true}
local closers={'"',"'",')',']','}', '”','’'}
local function continuation(line,s,e)
    if line:sub(s,e)~='.' then return false end
    local before=line:sub(1,s-1)
    local token=before:match('([%a%.]+)$') or ''
    local remainder=line:sub(e+1):gsub('\194\160',' '):gsub('\226\128\175',' ')
    local after=remainder:match('^%s+(.*)$') or ''
    if after=='' then return false end
    if titles[token:lower()] and after:match('^[A-Z]') then return true end
    -- Initials before a capitalized name, excluding common Roman numerals.
    if token:match('^[A-HJ-UW-Z]$') and after:match('^[A-Z]') then return true end
    local lower=token:lower()
    if lower=='e.g' or lower=='i.e' then return true end
    -- A time/acronym can also end a sentence; only join before lowercase.
    if (lower=='a.m' or lower=='p.m' or token:match('^%u%.%u$'))
        and after:match('^[a-z]') then return true end
    return false
end
function English.boundary(line,pos)
    local search=pos
    while search<=#line do
        local s,e=line:find('[%.%?!]+',search)
        if not s then return nil end
        local finish=e
        while true do
            local found=false
            for _,c in ipairs(closers) do
                if line:sub(finish+1,finish+#c)==c then
                    finish=finish+#c;found=true;break
                end
            end
            if not found then break end
        end
        local next_char=line:sub(finish+1,finish+1)
        if (next_char=='' or next_char:match('%s')
            or line:sub(finish+1,finish+2)=='\194\160'
            or line:sub(finish+1,finish+3)=='\226\128\175')
            and not (finish==e and continuation(line,s,e)) then
            return s,finish
        end
        search=e+1
    end
end
return English
