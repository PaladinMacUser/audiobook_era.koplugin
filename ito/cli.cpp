// Offline Ito adapter for KOReader. GPL-3.0-or-later.
#include "itofs.h"
#include "sonic.h"
#include "symbols.h"
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <cerrno>
#include <csignal>
#include <string>
#include <vector>
#include <fstream>
#include <iterator>
#include <chrono>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
static volatile sig_atomic_t running=1;
static void stop_server(int){running=0;}
static bool number(const char *s,double lo,double hi,double &v){char *e;errno=0;v=strtod(s,&e);return e!=s&&!*e&&!errno&&std::isfinite(v)&&v>=lo&&v<=hi;}
static void le16(FILE*f,uint16_t n){fputc(n&255,f);fputc(n>>8,f);}
static void le32(FILE*f,uint32_t n){le16(f,n&65535);le16(f,n>>16);}
static void header(FILE*f,uint32_t rate,uint32_t frames){fseek(f,0,SEEK_SET);fwrite("RIFF",1,4,f);le32(f,frames*2+36);fwrite("WAVEfmt ",1,8,f);le32(f,16);le16(f,1);le16(f,1);le32(f,rate);le32(f,rate*2);le16(f,2);le16(f,16);fwrite("data",1,4,f);le32(f,frames*2);}
struct Ito {
    itofs_model_t model={}; itofs_ctx_t context={};
    void *blob=nullptr,*hot=nullptr,*bulk=nullptr;
    std::string phonemizer;
    ~Ito(){free(blob);free(hot);free(bulk);}
    bool load(const std::string &path){
        FILE*f=fopen(path.c_str(),"rb");if(!f)return false;
        fseek(f,0,SEEK_END);long size=ftell(f);rewind(f);
        bool ok=size>1024&&size<32*1024*1024&&!posix_memalign(&blob,64,size)&&fread(blob,1,size,f)==(size_t)size;
        fclose(f);if(!ok||itofs_model_init(&model,blob,size))return false;
        itofs_limits_t limits={400,24,8,0};size_t hb=0,bb=0;
        itofs_arena_bytes(&model,&limits,&hb,&bb);
        if(hb>32*1024*1024||bb>32*1024*1024||posix_memalign(&hot,64,hb)||posix_memalign(&bulk,64,bb))return false;
        return itofs_init(&context,&model,&limits,hot,hb,bulk,bb)==0&&model.sr==24000;
    }
};
static bool cancelled(const std::string &path){return !running||(!path.empty()&&access(path.c_str(),F_OK)==0);}
static void add_symbol(std::vector<int>&ids,const std::string &s){
    // Later duplicate symbols win, matching StyleTTS 2 and upstream Ito.
    for(int i=(int)(sizeof(symbols)/sizeof(*symbols))-1;i>=0;i--)if(s==symbols[i]){ids.push_back(i);return;}
}
static bool phonemize(Ito &engine,const std::string &text,std::vector<int>&ids){
    if(text.find_first_not_of(" \r\n\t")==std::string::npos)return true;
    char name[]="/tmp/audiobook-ito-text-XXXXXX";int input=mkstemp(name);if(input<0)return false;
    bool ok=write(input,text.data(),text.size())==(ssize_t)text.size();close(input);
    int pipes[2];if(!ok||pipe(pipes)){unlink(name);return false;}
    pid_t child=fork();
    if(child==0){dup2(pipes[1],STDOUT_FILENO);close(pipes[0]);close(pipes[1]);execl("/bin/sh","sh",engine.phonemizer.c_str(),name,(char*)nullptr);_exit(127);}
    close(pipes[1]);std::string ipa;char buffer[1024];ssize_t n;
    if(child>0)while((n=read(pipes[0],buffer,sizeof buffer))>0){ipa.append(buffer,n);if(ipa.size()>65536)break;}
    close(pipes[0]);int status=0;
    if(child>0){while(waitpid(child,&status,0)<0){if(errno!=EINTR)break;}}else status=1;
    unlink(name);if(child<0||status!=0||ipa.empty()||ipa.size()>65536)return false;
    bool space=false;
    for(size_t i=0;i<ipa.size();){
        unsigned char c=ipa[i];size_t length=c<128?1:c<224?2:c<240?3:4;
        std::string symbol=ipa.substr(i,length);i+=length;
        if(symbol=="\xe2\x80\x8d"||symbol=="\xe2\x80\x8c")continue;
        if(length==1&&(c==' '||c=='\n'||c=='\r'||c=='\t')){space=true;continue;}
        if(space&&ids.size()>1&&ids.back()!=16)add_symbol(ids," ");space=false;
        add_symbol(ids,symbol);
    }
    return ids.size()>1;
}
static bool tokens(Ito &engine,const std::string &text,std::vector<int>&ids){
    ids.push_back(0);size_t start=0;
    for(size_t i=0;i<=text.size();i++){
        bool punct=i<text.size()&&strchr(";:,.!?",text[i]);
        if(i==text.size()||punct){
            if(!phonemize(engine,text.substr(start,i-start),ids))return false;
            if(punct){if(ids.back()!=16)add_symbol(ids," ");add_symbol(ids,text.substr(i,1));add_symbol(ids," ");}
            start=i+1;
        }
    }
    while(ids.size()>1&&ids.back()==16)ids.pop_back();return ids.size()>1;
}
static bool render(Ito &engine,const std::string &input,const std::string &output,double speed,double volume,double lead,
                   const std::string &cancel="",const std::string &voice="ItoFemale",double pitch=1,bool ssml=false){
    if(voice!="ItoFemale"||pitch!=1||ssml||cancelled(cancel))return false;
    std::ifstream source(input.c_str(),std::ios::binary|std::ios::ate);
    if(!source||source.tellg()<=0||source.tellg()>16384)return false;source.seekg(0);
    std::string text((std::istreambuf_iterator<char>(source)),std::istreambuf_iterator<char>());std::vector<int> ids;
    if(!tokens(engine,text,ids)||cancelled(cancel))return false;
    std::string temporary=output+".ito.XXXXXX";int fd=mkstemp(&temporary[0]);if(fd<0)return false;
    FILE*f=fdopen(fd,"w+b");if(!f){close(fd);unlink(temporary.c_str());return false;}
    uint32_t frames=0;header(f,24000,0);short pcm[24000];bool ok=true;
    unsigned guard=(unsigned)(24*lead);
    memset(pcm,0,sizeof pcm);while(guard){unsigned n=guard>24000?24000:guard;if(fwrite(pcm,2,n,f)!=n){ok=false;break;}frames+=n;guard-=n;}
    sonicStream sonic=sonicCreateStream(24000,1);if(!sonic)ok=false;else sonicSetSpeed(sonic,speed);
    auto drain=[&](){int n;while((n=sonicReadShortFromStream(sonic,pcm,24000))>0){
        if(frames+(unsigned)n>24000*300)return false;
        for(int i=0;i<n;i++)pcm[i]=(short)std::lround(pcm[i]*volume);
        if(fwrite(pcm,2,n,f)!=(size_t)n)return false;frames+=n;
    }return true;};
    for(size_t at=1;ok&&at<ids.size();){
        size_t end=ids.size();if(end-at>398){end=at+398;while(end>at+1&&ids[end]!=16)--end;if(end<=at+1){ok=false;break;}}
        std::vector<int> part(1,0);part.insert(part.end(),ids.begin()+at,ids.begin()+end);
        if(itofs_begin(&engine.context,part.data(),part.size(),0,1)<0){ok=false;break;}
        while(ok&&!cancelled(cancel)){
            int n=itofs_next_chunk(&engine.context,pcm,24000);if(n<0){ok=false;break;}if(!n)break;
            ok=sonicWriteShortToStream(sonic,pcm,n)&&drain();
        }
        if(cancelled(cancel))ok=false;at=end;while(at<ids.size()&&ids[at]==16)at++;
    }
    if(ok){sonicFlushStream(sonic);ok=drain();}if(sonic)sonicDestroyStream(sonic);
    if(ok&&frames>(uint32_t)(24*lead))header(f,24000,frames);else ok=false;
    bool output_failed=ferror(f)!=0;
    if(fclose(f)||output_failed)ok=false;
    if(!ok||cancelled(cancel)){unlink(temporary.c_str());return false;}
    if(rename(temporary.c_str(),output.c_str())){unlink(temporary.c_str());return false;}return true;
}
#include "worker.inc"
int main(int argc,char**argv){
    std::string input,output,daemon,data,phonemizer;
    double speed=1,volume=1,lead=0,idle=120;
    for(int i=1;i<argc;i+=2){if(i+1>=argc)return 2;std::string key=argv[i];
        if(key=="--input")input=argv[i+1];else if(key=="--output")output=argv[i+1];
        else if(key=="--daemon")daemon=argv[i+1];else if(key=="--data")data=argv[i+1];else if(key=="--phonemizer")phonemizer=argv[i+1];
        else if(key=="--speed"){if(!number(argv[i+1],.25,2,speed))return 2;}
        else if(key=="--volume"){if(!number(argv[i+1],0,1,volume))return 2;}
        else if(key=="--lead-in-ms"){if(!number(argv[i+1],0,2000,lead))return 2;}
        else if(key=="--idle-seconds"){if(!number(argv[i+1],1,600,idle))return 2;}
        else return 2;
    }
    char exe[4096];ssize_t n=readlink("/proc/self/exe",exe,sizeof exe-1);if(n<=0)return 2;exe[n]=0;std::string dir=exe;dir=dir.substr(0,dir.find_last_of('/'));
    if(data.empty())data=dir+"/ito_female_esp32s3.bin";if(phonemizer.empty())phonemizer=dir+"/phonemize.sh";
    Ito engine;engine.phonemizer=phonemizer;if(!engine.load(data)){fprintf(stderr,"Ito: cannot load compatible female voice file\n");return 1;}
    if(!daemon.empty())return serve(engine,daemon,(unsigned)idle);
    if(input.empty()||output.empty())return 2;return render(engine,input,output,speed,volume,lead)?0:1;
}
