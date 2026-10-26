package.loaded.logger=setmetatable({},{__index=function() return function() end end})
local English=dofile('englishspeech.lua')
local Settings=dofile('rhvoicesettings.lua')
local Parser=dofile('textparser.lua')
local checks=0
local function check(v,m) checks=checks+1;assert(v,m) end
local p=Parser:new{english_phrasing_fn=function() return true end}
local function sentences(text,expected)
    local out=p:parseSentences(text)
    check(#out==#expected,'sentence count: '..text)
    for i,s in ipairs(out) do
        check(s.text==expected[i],'sentence text: '..s.text)
        check(text:sub(s.start_pos,s.end_pos)==s.text,'original byte offsets remain valid')
    end
end
sentences('Dr. Morgan met Mr. Brown. They left.',{'Dr. Morgan met Mr. Brown.','They left.'})
sentences('J. R. R. Tolkien wrote books. I. Next section.',{'J. R. R. Tolkien wrote books.','I.','Next section.'})
sentences('Try e.g. apples or pears. Then go.',{'Try e.g. apples or pears.','Then go.'})
sentences('At 3.14 p.m. we left. It cost 3.14.',{'At 3.14 p.m. we left.','It cost 3.14.'})
sentences('He said “Hello!” She said "Goodbye." Then left.',{'He said “Hello!”','She said "Goodbye."','Then left.'})
sentences('They live in the U.S. He left.',{'They live in the U.S.','He left.'})
sentences('One?! Two... Three.',{'One?!','Two...','Three.'})
sentences('Hello.\194\160Next!',{'Hello.','\194\160Next!'})
sentences('Dr.\194\160Brown arrived. Next.',{'Dr.\194\160Brown arrived.','Next.'})
local raw='“Don’t rush,” said Dr. Brown. A ﬁne ofﬁce, a ﬂower, and re\194\173 ader…\194\160Done.'
local prepared=English.prepare(raw)
check(prepared=='"Don\'t rush," said Dr. Brown. A fine office, a flower, and reader... Done.','ebook typography becomes ordinary spoken English')
check(English.prepare('mother-in-law at -5 and 1,000.50')=='mother-in-law at -5 and 1,000.50','semantic punctuation and numbers are untouched')
check(English.prepare('A\226\129\160B\239\187\191 C\226\128\175D')=='AB C D','invisible joiners and narrow spaces are handled')
local options=Settings.options(nil)
check(options.natural,'natural phrasing enabled by default')
options.corrections={['fine']='lovely',["don't"]='do not'}
local ssml=Settings.ssml(raw..'<break/>',options)
check(ssml:find('lovely office',1,true) and ssml:find('do not rush',1,true),'corrections apply after typographic cleanup')
check(ssml:find('&lt;break/&gt;',1,true) and not ssml:find('<break/>',1,true),'book markup cannot become synthesis instructions')
local saved_corrections={["O’Neil"]="Oh Neel",['ﬁne']='feyn',fine='lovely'}
local corrected=Settings.options(nil);corrected.corrections=saved_corrections
local spoken=Settings.ssml('O’Neil saw a ﬁne book.',corrected)
check(spoken:find('Oh Neel saw a lovely book.',1,true),'existing typographic corrections still match; canonical keys win collisions')
check(saved_corrections['ﬁne']=='feyn' and saved_corrections["O’Neil"]=='Oh Neel','cleanup never mutates saved corrections')
local disabled=Settings.options({getSetting=function(_,k,d) if k=='rhvoice_natural_english' then return false end;return d end})
check(disabled.natural==false,'explicit opt-out is respected')
check(Settings.ssml('ﬁne',disabled):find('ﬁne',1,true),'disabled cleanup preserves synthesis input')
local enabled=Settings.options(nil)
check(Settings.signature(enabled)~=Settings.signature(disabled),'cleanup option invalidates prefetched and cached speech')
local legacy=Parser:new():parseSentences('Dr. Brown left.')
check(#legacy==2,'other engines retain their existing sentence behavior')
local text='Dr. Brown found a ﬁne book. “Read it!” Then go.'
local parsed=p:parse(text)
for _,w in ipairs(parsed.words) do check(parsed.text:sub(w.start_pos,w.end_pos)==w.text,'word positions remain anchored to visible book text') end
print('ok - '..checks..' natural English phrasing, cleanup and source-offset checks passed')
