/* Private persistent PocketBook PCM player. GPL-3.0-or-later.
 * Uses the public ALSA opaque-handle ABI through libasound.so.2. Keeping
 * this small adapter independent of ALSA headers avoids changing the
 * firmware's library or mixer. It only opens the selected firmware PCM.
 */
#define _POSIX_C_SOURCE 200809L
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

typedef struct _snd_pcm snd_pcm_t;
typedef long snd_pcm_sframes_t;
static int (*pcm_open)(snd_pcm_t **,const char *,int,int);
static int (*pcm_close)(snd_pcm_t *);
static int (*pcm_params)(snd_pcm_t *,int,int,unsigned,unsigned,int,unsigned);
static snd_pcm_sframes_t (*pcm_write)(snd_pcm_t *,const void *,unsigned long);
static int (*pcm_delay)(snd_pcm_t *,snd_pcm_sframes_t *);
static int (*pcm_recover)(snd_pcm_t *,int,int);
static int (*pcm_drop)(snd_pcm_t *);
static volatile sig_atomic_t running=1;
static void stop(int n) { (void)n;running=0; }
static double now(void) { struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9; }
static uint32_t le32(const unsigned char *p) { return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static unsigned le16(const unsigned char *p) { return p[0]|(unsigned)p[1]<<8; }
static int marker(const char *path,const char *suffix,const char *value) {
    char target[4096],temporary[4100];
    snprintf(target,sizeof target,"%s%s",path,suffix);snprintf(temporary,sizeof temporary,"%s.new",target);
    FILE *f=fopen(temporary,"wb");if(!f)return 0;
    int ok=fputs(value,f)>=0;if(fclose(f))ok=0;
    if(ok&&rename(temporary,target))ok=0;
    unlink(temporary);return ok;
}
static int exists(const char *job,const char *suffix) { char p[4096];snprintf(p,sizeof p,"%s%s",job,suffix);return access(p,F_OK)==0; }
static int local_path(const char *p,const char *dir) {
    size_t n=strlen(dir);return strncmp(p,dir,n)==0&&p[n]=='/'&&p[n+1]&& !strchr(p+n+1,'/')&&!strstr(p+n+1,"..");
}
static int line(FILE *f,char *s,size_t size) {
    if(!fgets(s,(int)size,f))return 0;
    size_t n=strlen(s);if(!n||s[n-1]!='\n')return 0;s[n-1]=0;return !strchr(s,'\r');
}
struct clip {
    char job[4096];unsigned char *data;unsigned rate,channels;
    uint64_t frames,position,total,end_frame,guard_frames;int ending;
};
static void clear_clip(struct clip *c) { free(c->data);memset(c,0,sizeof *c); }
static int load_clip(struct clip *c,const char *job,const char *dir,int warm) {
    char input[4096],output[4096],speed[32],volume[32],lead[32],audio[4096];
    FILE *f=fopen(job,"rb");if(!f)return 0;
    int valid=line(f,input,sizeof input)&&line(f,output,sizeof output)&&line(f,speed,sizeof speed)
        &&line(f,volume,sizeof volume)&&line(f,lead,sizeof lead)&&local_path(input,dir)&&local_path(output,dir);
    fclose(f);if(!valid)return 0;
    f=fopen(input,"rb");if(!f)return 0;
    size_t n=fread(audio,1,sizeof audio-1,f);fclose(f);audio[n]=0;
    if(!n||strncmp(audio,"/tmp/",5)||strchr(audio,'\n')||strchr(audio,'\r'))return 0;
    f=fopen(audio,"rb");if(!f)return 0;
    unsigned char h[44]={0};valid=fread(h,1,sizeof h,f)==sizeof h&&!memcmp(h,"RIFF",4)&&!memcmp(h+8,"WAVEfmt ",8)
        &&!memcmp(h+36,"data",4)&&le16(h+20)==1&&le16(h+34)==16;
    unsigned channels=le16(h+22),rate=le32(h+24);uint32_t bytes=le32(h+40);
    valid=valid&&(channels==1||channels==2)&&rate>=8000&&rate<=48000&&bytes>0&&bytes<=32*1024*1024
        &&le16(h+32)==channels*2&&bytes%(channels*2)==0;
    if(!valid){fclose(f);return 0;}
    unsigned char *data=malloc(bytes);if(!data){fclose(f);return 0;}
    valid=fread(data,1,bytes,f)==bytes&&fgetc(f)==EOF;fclose(f);
    if(!valid){free(data);return 0;}
    char *end;long lead_ms=strtol(lead,&end,10);
    if(*end||lead_ms<0||lead_ms>2000){free(data);return 0;}
    strcpy(c->job,job);c->data=data;c->channels=channels;c->rate=rate;c->frames=bytes/(channels*2);
    uint64_t skip=warm?(uint64_t)rate*lead_ms/1000:0;
    if(skip>=c->frames){clear_clip(c);return 0;}
    if(skip){memmove(data,data+skip*channels*2,bytes-skip*channels*2);c->frames-=skip;}
    c->guard_frames=warm?0:(uint64_t)rate*lead_ms/1000;
    c->total=c->frames*48000/rate;c->position=0;c->ending=0;
    char status[32];snprintf(status,sizeof status,"%ld\n",warm?0:lead_ms);
    marker(job,".playing",status);return 1;
}
static void samples(struct clip *c,int16_t *out,unsigned frames) {
    for(unsigned i=0;i<frames;i++) {
        uint64_t p=(c->position+i)*c->rate;uint64_t a=p/48000,b=a+1<c->frames?a+1:a;unsigned frac=p%48000;
        if(a<c->guard_frames&&b>=c->guard_frames)b=a;
        for(unsigned ch=0;ch<2;ch++) {
            unsigned k=c->channels==1?0:ch;
            int x=(int16_t)le16(c->data+(a*c->channels+k)*2),y=(int16_t)le16(c->data+(b*c->channels+k)*2);
            out[i*2+ch]=(int16_t)(x+(int64_t)(y-x)*frac/48000);
        }
    }
}
int main(int argc,char **argv) {
    const char *socket_path=NULL,*device="hwout_mix";unsigned idle=10;
    for(int i=1;i<argc;i+=2) {
        if(i+1>=argc)return 2;
        if(!strcmp(argv[i],"--daemon"))socket_path=argv[i+1];
        else if(!strcmp(argv[i],"--device"))device=argv[i+1];
        else if(!strcmp(argv[i],"--idle-seconds"))idle=(unsigned)atoi(argv[i+1]);else return 2;
    }
    if(!socket_path||strlen(socket_path)>=108||idle<1||idle>120
        ||(strcmp(device,"hwout_mix")&&strcmp(device,"tts_sm")))return 2;
    void *alsa=dlopen("libasound.so.2",RTLD_NOW|RTLD_LOCAL);if(!alsa){fprintf(stderr,"era-player: %s\n",dlerror());return 1;}
#define LOAD(field,name) do { *(void **)(&field)=dlsym(alsa,name);if(!field)return 1; } while(0)
    LOAD(pcm_open,"snd_pcm_open");LOAD(pcm_close,"snd_pcm_close");LOAD(pcm_params,"snd_pcm_set_params");
    LOAD(pcm_write,"snd_pcm_writei");LOAD(pcm_delay,"snd_pcm_delay");LOAD(pcm_recover,"snd_pcm_recover");LOAD(pcm_drop,"snd_pcm_drop");
    char directory[4096],lock_path[4096];strcpy(directory,socket_path);char *slash=strrchr(directory,'/');if(!slash)return 2;*slash=0;
    snprintf(lock_path,sizeof lock_path,"%s.lock",socket_path);int lock=open(lock_path,O_CREAT|O_RDWR,0600);
    if(lock<0||flock(lock,LOCK_EX|LOCK_NB))return 1;
    int fd=socket(AF_UNIX,SOCK_DGRAM|SOCK_NONBLOCK,0);if(fd<0)return 1;
    struct sockaddr_un addr={0};addr.sun_family=AF_UNIX;strcpy(addr.sun_path,socket_path);unlink(socket_path);
    if(bind(fd,(struct sockaddr *)&addr,sizeof addr))return 1;
    chmod(socket_path,0600);
    signal(SIGTERM,stop);signal(SIGINT,stop);
    char pid[32];snprintf(pid,sizeof pid,"%d\n",getpid());marker(socket_path,".ready",pid);
    struct clip clip={0};snd_pcm_t *pcm=NULL;uint64_t queued=0;double last=now();
    while(running) {
        if(clip.data&&exists(clip.job,".cancel")) {
            marker(clip.job,".done","CANCELLED\n");clear_clip(&clip);
            if(pcm){pcm_drop(pcm);pcm_close(pcm);pcm=NULL;queued=0;}last=now();
        }
        char packet[4096];ssize_t n=recv(fd,packet,sizeof packet-1,0);
        if(n>0) {
            packet[n]=0;
            if(local_path(packet,directory)&&n>4&&!strcmp(packet+n-4,".req")) {
                if(clip.data)marker(packet,".done","ERR player busy\n");
                else if(exists(packet,".cancel"))marker(packet,".done","CANCELLED\n");
                else {
                    int warm=pcm!=NULL;
                    if(!pcm&&(pcm_open(&pcm,device,0,1)<0||pcm_params(pcm,2,3,2,48000,0,80000)<0)) {
                        if(pcm){pcm_close(pcm);pcm=NULL;}
                        marker(packet,".done","ERR cannot open firmware PCM\n");
                    } else if(!load_clip(&clip,packet,directory,warm))marker(packet,".done","ERR invalid WAV\n");
                }
                last=now();
            }
        }
        if(!clip.data&&now()-last>=idle)break;
        if(pcm) {
            int16_t buffer[480*2]={0};unsigned frames=480;
            int speaking=clip.data&&!clip.ending&&!exists(clip.job,".pause");
            if(speaking) {
                if(clip.total-clip.position<frames)frames=(unsigned)(clip.total-clip.position);
                samples(&clip,buffer,frames);
            }
            snd_pcm_sframes_t written=pcm_write(pcm,buffer,frames);
            if(written<0&&written!=-EAGAIN) {
                if(pcm_recover(pcm,(int)written,1)<0) {
                    if(clip.data){marker(clip.job,".done","ERR firmware PCM write failed\n");clear_clip(&clip);}
                    pcm_drop(pcm);pcm_close(pcm);pcm=NULL;queued=0;
                }
            } else if(written>0) {
                queued+=(uint64_t)written;
                if(speaking) {
                    clip.position+=(uint64_t)written;
                    if(clip.position==clip.total){clip.ending=1;clip.end_frame=queued;}
                }
            }
            if(clip.data&&clip.ending&&!exists(clip.job,".pause")) {
                snd_pcm_sframes_t delay=0;
                if(pcm_delay(pcm,&delay)>=0&&(uint64_t)(delay>0?delay:0)<=queued
                        &&queued-(uint64_t)(delay>0?delay:0)>=clip.end_frame) {
                    marker(clip.job,".done","OK\n");clear_clip(&clip);last=now();
                }
            }
        }
        struct pollfd event={fd,POLLIN,0};poll(&event,1,pcm?10:100);
    }
    if(clip.data){marker(clip.job,".done","CANCELLED\n");clear_clip(&clip);}
    if(pcm){pcm_drop(pcm);pcm_close(pcm);}
    close(fd);unlink(socket_path);char ready[4096];snprintf(ready,sizeof ready,"%s.ready",socket_path);unlink(ready);close(lock);dlclose(alsa);return 0;
}
