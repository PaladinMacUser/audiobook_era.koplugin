-- Ito's English frontend normalization, before preserving clause punctuation.
local dir=debug.getinfo(1,'S').source:match('^@(.*/)') or './'
local Settings=dofile(dir..'rhvoicesettings.lua')
local Ito={}
function Ito.prepare(text,options)
    text=Settings.prepare(text,options):gsub('"','')
    local titles={mr='mister',mrs='missus',ms='miz',dr='doctor',prof='professor',rev='reverend',
        capt='captain',lt='lieutenant',col='colonel',gen='general',sgt='sergeant'}
    text=text:gsub('%f[%a]([%a]+)%.(%s+)(%u)',function(title,space,name)
        return (titles[title:lower()] or (title..'.'))..space..name
    end)
    -- Strip commas only from valid groups of three digits. Invalid grouping
    -- remains punctuation, matching the upstream frontend's conservative rule.
    local previous
    repeat previous=text;text=text:gsub('(%d),(%d%d%d)%f[%D]','%1%2') until text==previous
    text=text:gsub('%$(%d+)%.(%d%d)%f[%D]',function(d,c) return d..' dollars and '..tonumber(c)..' cents' end)
    text=text:gsub('%$(%d+)','%1 dollars')
    local digits={['0']='zero',['1']='one',['2']='two',['3']='three',['4']='four',['5']='five',['6']='six',['7']='seven',['8']='eight',['9']='nine'}
    return (text:gsub('(%d+)%.(%d+)',function(a,b)
        return a..' point '..b:gsub('%d',function(d) return digits[d]..' ' end):gsub(' $','')
    end))
end
return Ito
