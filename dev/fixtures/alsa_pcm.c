/* Deterministic ALSA ABI fixture: no hardware access. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
static void event(const char *value) {FILE *f=fopen(getenv("ERA_PCM_EVENTS"),"a");if(f){fputs(value,f);fclose(f);}}
int snd_pcm_open(void **pcm,const char *name,int stream,int mode) {
    if(strcmp(name,"hwout_mix")||stream||mode!=1)return -1;
    *pcm=malloc(1);event("open\n");return 0;
}
int snd_pcm_set_params(void *pcm,int format,int access,unsigned channels,unsigned rate,int soft,unsigned latency) {
    (void)pcm;(void)latency;return format==2&&access==3&&channels==2&&rate==48000&&!soft?0:-1;
}
long snd_pcm_writei(void *pcm,const void *data,unsigned long count) {
    (void)pcm;unsigned long n=getenv("ERA_PCM_SHORT")&&count>1?count/2:count;
    FILE *f=fopen(getenv("ERA_PCM_CAPTURE"),"ab");if(!f)return -1;
    fwrite(data,4,n,f);fclose(f);return (long)n;
}
int snd_pcm_delay(void *pcm,long *delay) {(void)pcm;*delay=0;return 0;}
int snd_pcm_recover(void *pcm,int error,int silent) {(void)pcm;(void)error;(void)silent;return -1;}
int snd_pcm_drop(void *pcm) {(void)pcm;event("drop\n");return 0;}
int snd_pcm_close(void *pcm) {free(pcm);event("close\n");return 0;}
