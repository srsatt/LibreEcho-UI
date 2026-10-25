/* LibreEcho finite ESPHome satellite, API 1.14 / aioesphomeapi 46.2.0.
 * One poll owner. Fixed queues; no fork, thread, blocking DNS or socket reads.
 * Native API capabilities: VA/API_AUDIO/TIMERS/ANNOUNCE/START_CONVERSATION,
 * deliberately NOT SPEAKER (HA must deliver finite URLs, not streamed PCM). */
#define _POSIX_C_SOURCE 200809L
#include "esphome_proto.h"
#include "esphome_frame.h"
#include "esphome_noise.h"
#include "esphome_playback.h"
#include "mdns_client.h"
#include "../config_store.h"
#include "../json.h"
#include "../esphome_health.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <mbedtls/base64.h>
#include <mbedtls/platform_util.h>
#define CLIENTS 2
#define TX_CAP (256U*1024U)
#define RX_CAP (EF_MAX_FRAME+16U)
#define JOBS 8
#define JOB_TIMER 7
#define JOB_TIMER_RETIRED 8
#define RING_SAMPLES 48000U
#define MEDIA_KEY 1U
#define MUTE_KEY 2U
struct client { int fd,mode,phase,hello,states,closing; unsigned char rx[RX_CAP],tx[TX_CAP];size_t rx_n,tx_n,tx_sent;uint64_t deadline,last_seen,ping_at;struct en_session noise; };
struct io {int fd,stage,kind;char tx[2048],rx[4096];size_t tx_n,tx_sent,rx_n;uint64_t deadline;int target;};
struct state {
 struct client clients[CLIENTS];struct io jobs[JOBS],wake,mic,timer_cleanup;
 const char *config_path,*status_path,*audio_path,*wake_path,*radio_path,*timer_path,*led_path,*mdns_path,*bus_path,*privacy_path,*ca_path,*idme_path;
 char name[32],friendly[64],mac[18];unsigned char psk[32];int provisioned,plaintext,ha_selected,ready,owner,turn,muted,local_mute_gate,privacy,volume,output_muted,restore_volume,radio_state,active_wake,stream_wav;
 uint64_t now,turn_deadline,turn_limit,last_audio_status,last_status_poll,wake_retry,mdns_retry,ring_first,ring_end,capture_next,timer_retry;
 int16_t ring[RING_SAMPLES];unsigned char mic_rx[24+2560];size_t mic_n;int mic_ack;
 char conversation[128],tts_url[1024],next_url[1024],result[24];int continue_turn,run_end,early_tts,playing,announcement,pipeline,tts_done,timer_pending;
 time_t result_time;struct esp_playback playback;int mdns_fd;
 char process_start[32],boot_id[64],listener_inode[32];unsigned listener_port;
};
static struct state S;static volatile sig_atomic_t running=1,reload_config;
static void sigstop(int sig){if(sig==SIGHUP)reload_config=1;else running=0;}
static uint64_t now_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+(unsigned)t.tv_nsec/1000000;}
static void io_close(struct io *j){if(j->fd>=0)close(j->fd);j->fd=-1;j->stage=0;j->rx_n=j->tx_n=j->tx_sent=0;}
static int io_connect(struct io *j,const char *path){struct sockaddr_un a;
 if(j->fd>=0||strlen(path)>=sizeof a.sun_path)return -1;memset(&a,0,sizeof a);a.sun_family=AF_UNIX;memcpy(a.sun_path,path,strlen(path)+1);
 j->fd=socket(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);if(j->fd<0)return -1;
 if(connect(j->fd,(struct sockaddr*)&a,sizeof a)<0&&errno!=EINPROGRESS){io_close(j);return -1;}
 return 0;
}
static int io_open(struct io *j,const char *path,const char *cmd,const char *args,int kind){int n;if(j->fd>=0||strlen(path)>=sizeof(((struct sockaddr_un*)0)->sun_path))return -1;
 n=snprintf(j->tx,sizeof j->tx,"{\"v\":1,\"id\":1,\"cmd\":\"%s\",\"args\":%s}\n",cmd,args);if(n<0||(size_t)n>=sizeof j->tx)return -1;
 int defer=kind==JOB_TIMER&&S.timer_pending;if(!defer&&io_connect(j,path)<0)return -1;j->tx_n=(size_t)n;j->tx_sent=j->rx_n=0;j->deadline=S.now+(kind==5?6000:500);j->stage=defer?3:1;j->kind=kind;return 0;
}
static int job(const char *path,const char *cmd,const char *args,int kind,int target){for(unsigned i=0;i<JOBS;i++)if(S.jobs[i].fd<0&&!S.jobs[i].stage){if(io_open(&S.jobs[i],path,cmd,args,kind)<0)return -1;S.jobs[i].target=target;return 0;}return -1;}
/* Returns one complete response line; remaining bytes stay buffered for LVS1. */
static int io_tick(struct io *j){ssize_t n;if(j->fd<0)return -1;if(S.now>j->deadline)return -1;
 if(j->stage==1){n=send(j->fd,j->tx+j->tx_sent,j->tx_n-j->tx_sent,MSG_DONTWAIT|MSG_NOSIGNAL);if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR))return 0;if(n<=0)return -1;j->tx_sent+=(size_t)n;if(j->tx_sent<j->tx_n)return 0;j->stage=2;}
 char *end=memchr(j->rx,'\n',j->rx_n);if(end)return 1;
 if(j->rx_n>=sizeof j->rx-1)return -1;n=recv(j->fd,j->rx+j->rx_n,sizeof j->rx-1-j->rx_n,MSG_DONTWAIT);if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR))return 0;if(n<=0)return -1;j->rx_n+=(size_t)n;j->rx[j->rx_n]=0;return memchr(j->rx,'\n',j->rx_n)?1:0;
}
/* Retire unsent old-owner events. Submitted transactions settle before clear;
 * replacement-owner events stay queued without sockets until cleanup is acked
 * (timerd has only four clients; idle queued connections could starve clear). */
static void timer_owner_end(void){S.timer_pending=1;S.timer_retry=S.now;for(unsigned i=0;i<JOBS;i++){struct io *j=&S.jobs[i];if((j->fd>=0||j->stage==3)&&j->kind==JOB_TIMER){if(!j->tx_sent)io_close(j);else j->kind=JOB_TIMER_RETIRED;}}}
static void timer_cleanup_tick(void){if(!S.timer_pending)return;for(unsigned i=0;i<JOBS;i++)if(S.jobs[i].fd>=0&&S.jobs[i].kind==JOB_TIMER_RETIRED)return;
 struct io *j=&S.timer_cleanup;if(j->fd<0){if(S.now<S.timer_retry)return;if(io_open(j,S.timer_path,"remote_clear","{}",0)<0){S.timer_retry=S.now+1000;return;}}
 int rc=io_tick(j);if(!rc)return;int ok=0;if(rc>0){char *end=memchr(j->rx,'\n',j->rx_n);*end=0;(void)json_get_bool(j->rx,"ok",&ok);}io_close(j);if(ok)S.timer_pending=0;else S.timer_retry=S.now+1000;
}
static int queued(struct client *c,const unsigned char *p,size_t n){if(c->tx_sent){memmove(c->tx,c->tx+c->tx_sent,c->tx_n-c->tx_sent);c->tx_n-=c->tx_sent;c->tx_sent=0;}if(n>sizeof c->tx-c->tx_n)return -1;memcpy(c->tx+c->tx_n,p,n);c->tx_n+=n;return 0;}
static int outer(struct client *c,const unsigned char *p,size_t n){unsigned char h[3]={1,(unsigned char)(n>>8),(unsigned char)n};if(n>65535||n+3>sizeof c->tx-c->tx_n)return -1;return queued(c,h,3)||queued(c,p,n)?-1:0;}
static int send_msg(struct client *c,unsigned type,const void *p,size_t n){unsigned char buf[RX_CAP],plain[RX_CAP];size_t z;
 if(c->fd<0||c->closing)return -1;
 if(c->mode==1){if(n>65515)return -1;plain[0]=(unsigned char)(type>>8);plain[1]=(unsigned char)type;plain[2]=(unsigned char)(n>>8);plain[3]=(unsigned char)n;if(n)memcpy(plain+4,p,n);if(en_encrypt(&c->noise,plain,n+4,buf,sizeof buf,&z))return -1;int rc=outer(c,buf,z);if(rc)c->closing=1;return rc;}
 z=ef_plain_encode(buf,sizeof buf,type,p,n);int rc=z?queued(c,buf,z):-1;if(rc)c->closing=1;return rc;
}
static int send_writer(struct client *c,unsigned t,struct ep_writer *w){return w->error?-1:send_msg(c,t,w->data,w->length);}
static void status_write(void){char b[512],tmp[512];int connected=0,fd,n;for(unsigned i=0;i<CLIENTS;i++)if(S.clients[i].fd>=0&&S.clients[i].hello)connected=1;
 n=snprintf(b,sizeof b,"{\"pid\":%ld,\"start_time\":\"%s\",\"boot_id\":\"%s\",\"listener_inode\":\"%s\",\"port\":%u,\"ready\":%s,\"connected\":%s,\"in_progress\":%s,\"last_result\":\"%s\",\"last_result_time\":%lld}\n",(long)getpid(),S.process_start,S.boot_id,S.listener_inode,S.listener_port,S.ready?"true":"false",connected?"true":"false",S.turn?"true":"false",S.result,(long long)S.result_time);
 if(n<0||(size_t)n>=sizeof b||snprintf(tmp,sizeof tmp,"%s.tmp",S.status_path)>=(int)sizeof tmp)return;
 fd=open(tmp,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600);if(fd<0)return;if(write(fd,b,(size_t)n)==n){close(fd);if(rename(tmp,S.status_path))unlink(tmp);}else{close(fd);unlink(tmp);}
}
static int mic_muted(void){return S.local_mute_gate||S.muted||S.privacy!=0||!S.last_audio_status||S.now-S.last_audio_status>1500;}
/* Turn indicators use ledd's owner-scoped pattern layer, never "animate":
 * animate persists the theme as the ring's saved colour and breathes until
 * another command replaces it, and nothing ever ended it -- a failed turn left
 * the ring pulsing the error colour indefinitely. Colours are ledd's default
 * themes; brightness follows the user's theme. Errors flash briefly and expire;
 * idle releases only this owner, leaving the saved ring settings untouched. */
static void profile(const char *name){char a[192];if(!strcmp(name,"idle")){(void)job(S.led_path,"pattern","{\"name\":\"stop\",\"owner\":\"esphome\"}",0,0);return;}
 int error=!strcmp(name,"error");unsigned r=72,g=185,b=255;if(!strcmp(name,"thinking")){r=168;g=115;b=239;}else if(error){r=239;g=80;b=80;}
 snprintf(a,sizeof a,"{\"name\":\"%s\",\"r\":%u,\"g\":%u,\"b\":%u,\"brightness\":100,\"repeats\":%d,\"profile\":\"%s\",\"owner\":\"esphome\"}",error?"flash":"pulse",r,g,b,error?3:0,name);(void)job(S.led_path,"pattern",a,0,0);}
static void states_send(struct client *c){unsigned char b[64];struct ep_writer w={b,sizeof b,0,0};float f=(float)S.volume/100.0f;uint32_t bits;memcpy(&bits,&f,4);
 ep_fixed32(&w,1,MEDIA_KEY);ep_uint(&w,2,S.playing?2:S.radio_state);ep_fixed32(&w,3,bits);ep_uint(&w,4,S.output_muted);if(S.last_audio_status)(void)send_writer(c,64,&w);
 w.length=0;ep_fixed32(&w,1,MUTE_KEY);ep_uint(&w,2,mic_muted());(void)send_writer(c,26,&w);
}
static void all_states(void){for(unsigned i=0;i<CLIENTS;i++)if(S.clients[i].fd>=0&&S.clients[i].states)states_send(&S.clients[i]);}
static int announce_done(int success){unsigned char b[4];struct ep_writer w={b,sizeof b,0,0};ep_uint(&w,1,(unsigned)success);return S.owner>=0?send_writer(&S.clients[S.owner],120,&w):-1;}
static void capture_end(void){if(S.turn==2&&S.owner>=0){unsigned char b[4];struct ep_writer w={b,sizeof b,0,0};ep_uint(&w,2,1);(void)send_writer(&S.clients[S.owner],106,&w);}if(S.turn==1||S.turn==2)S.turn=3;profile("thinking");}
static void turn_clear(const char *result,int notify){int active=S.turn||S.playing;if(notify)(void)announce_done(0);esp_playback_close(&S.playback);S.playing=S.turn=S.continue_turn=S.run_end=S.early_tts=S.announcement=S.pipeline=S.tts_done=0;S.next_url[0]=S.tts_url[0]=S.conversation[0]=0;snprintf(S.result,sizeof S.result,"%s",result);S.result_time=time(NULL);/* Error flash only for a turn that was really interrupted: an idle HA
 * reconnect is not a user-visible failure. */
 profile(active&&strcmp(result,"success")?"error":"idle");status_write();all_states();}
static void cancel_turn(const char *reason){if(S.turn&&S.owner>=0){unsigned char b[4];struct ep_writer w={b,sizeof b,0,0};ep_uint(&w,1,0);(void)send_writer(&S.clients[S.owner],90,&w);}turn_clear(reason,S.announcement);}
static void client_close(unsigned i){struct client *c=&S.clients[i];if(c->fd>=0)close(c->fd);c->fd=-1;if(S.owner==(int)i){cancel_turn("disconnected");S.owner=-1;timer_owner_end();}en_free(&c->noise);memset(c,0,sizeof *c);c->fd=-1;status_write();}
static int voice_start(uint64_t sample,int continued){unsigned char b[256];struct ep_writer w={b,sizeof b,0,0};if(!S.ha_selected||S.owner<0||S.turn||mic_muted()||(!continued&&!S.active_wake))return -1;
 ep_uint(&w,1,1);ep_string(&w,2,S.conversation);/* flags: USE_VAD only. USE_WAKE_WORD asks Home Assistant to run its own
    wake-word engine on the stream (ESPHome use_wake_word: true); the word was
    already detected here, so HA must start at STT -- with no HA wake engine the
    run otherwise fails instantly with wake-engine-missing. */
 ep_uint(&w,3,1);if(!continued)ep_string(&w,5,"Alexa");if(send_writer(&S.clients[S.owner],90,&w))return -1;
 S.capture_next=sample>3200?sample-3200:0;if(S.capture_next<S.ring_first)S.capture_next=S.ring_first;S.turn=1;S.turn_deadline=S.now+S.turn_limit;S.continue_turn=S.run_end=S.early_tts=S.tts_done=0;S.tts_url[0]=S.next_url[0]=0;S.announcement=0;S.pipeline=1;profile("listening");status_write();return 0;
}
static int playback_start(const char *url){if(!url[0]||S.playing)return -1;if((S.stream_wav?esp_playback_start_stream:esp_playback_start)(&S.playback,url,S.bus_path,S.ca_path,S.now)<0){esp_playback_close(&S.playback);return -1;}S.playing=1;S.turn=4;all_states();return 0;}
/* HA may await the drain ack before RUN_END. Ack once, but keep this pipeline
 * alive until RUN_END so its delayed event cannot address a new capture. */
static void completed(void){if(!S.tts_done)(void)announce_done(1);S.tts_done=1;if(S.pipeline&&!S.run_end)return;char conv[128];int cont=S.continue_turn;snprintf(conv,sizeof conv,"%s",S.conversation);turn_clear("success",0);if(cont&&!mic_muted()){snprintf(S.conversation,sizeof S.conversation,"%s",conv);(void)voice_start(S.ring_end,1);}}
static int valid_proto(const void *p,size_t n){struct ep_reader r;struct ep_field f;ep_reader_init(&r,p,n);while(ep_next(&r,&f)>0){}return r.error?-1:0;}
static void device_info(struct client *c){unsigned char b[512];struct ep_writer w={b,sizeof b,0,0};ep_string(&w,2,S.name);ep_string(&w,3,S.mac);ep_string(&w,4,"2026.9.0");ep_string(&w,6,"radar-puffin");ep_string(&w,8,"libreecho.satellite");ep_string(&w,9,"0.14.0");ep_string(&w,12,"LibreEcho");ep_string(&w,13,S.friendly);ep_uint(&w,17,61);ep_uint(&w,19,1);ep_uint(&w,26,!S.provisioned);(void)send_writer(c,10,&w);}
static void entities(struct client *c){unsigned char b[768],nested[80];struct ep_writer w={b,sizeof b,0,0};ep_string(&w,1,"media_player");ep_fixed32(&w,2,MEDIA_KEY);ep_string(&w,3,"Media player");ep_uint(&w,8,1);
 /* aioesphomeapi 46.2.0: PAUSE, VOLUME_SET/MUTE, PLAY_MEDIA, STOP, PLAY, MEDIA_ANNOUNCE. */
 ep_uint(&w,11,(1U<<0)|(1U<<2)|(1U<<3)|(1U<<9)|(1U<<12)|(1U<<14)|(1U<<20));
 for(unsigned i=0;i<3;i++){struct ep_writer n={nested,sizeof nested,0,0};ep_string(&n,1,i==1?"wav":"mp3");ep_uint(&n,2,48000);ep_uint(&n,3,i==1?1:2);ep_uint(&n,4,i?1:0);if(i==1)ep_uint(&n,5,2);ep_bytes(&w,9,n.data,n.length);}(void)send_writer(c,63,&w);
 w.length=0;ep_string(&w,1,"microphone_mute");ep_fixed32(&w,2,MUTE_KEY);ep_string(&w,3,"Microphone mute");ep_string(&w,5,"mdi:microphone-off");(void)send_writer(c,17,&w);(void)send_msg(c,19,NULL,0);
}
static void configuration(struct client *c){unsigned char b[256],x[128];struct ep_writer w={b,sizeof b,0,0},n={x,sizeof x,0,0};ep_string(&n,1,"alexa_v0.1");ep_string(&n,2,"Alexa");ep_string(&n,3,"en");ep_bytes(&w,1,x,n.length);if(S.active_wake)ep_string(&w,2,"alexa_v0.1");ep_uint(&w,3,1);(void)send_writer(c,122,&w);}
/* Preserve all unrelated fields; only replace a validated top-level string or
 * insert before the closing brace. config_store enforces 0600 + fsync/rename. */
static char *top_field(char *json,const char *needle){int depth=0;size_t len=strlen(needle);for(char *q=json;*q;q++){
 if(*q=='{'||*q=='['){depth++;continue;}if(*q=='}'||*q==']'){depth--;continue;}if(*q!='"')continue;
 char *start=q,*end=q+1;while(*end){if(*end=='\\'){if(!end[1])return NULL;end+=2;}else if(*end=='"')break;else end++;}if(!*end)return NULL;
 char *colon=end+1;while(isspace((unsigned char)*colon))colon++;if(depth==1&&*colon==':'&&(size_t)(end-start+1)==len&&!memcmp(start,needle,len))return start;q=end;
 }return NULL;}
static int config_set_string(const char *field,const char *value){char old[16384],out[16384],needle[128],escaped[512];int n=config_read(S.config_path,old,sizeof old);if(n<0||!json_valid_object(old,(size_t)n)||json_duplicate_key(old,(size_t)n,field))return -1;
 json_escape(escaped,sizeof escaped,value);snprintf(needle,sizeof needle,"\"%s\"",field);char *start=top_field(old,needle),*end;size_t prefix;
 if(start){/* Locate exactly the top-level field, rejecting ambiguous nesting. */
 char test[512];if(json_get_string_top_level(old,field,test,sizeof test)<=0)return -1;start=strchr(start+strlen(needle),':');if(!start)return -1;start++;while(isspace((unsigned char)*start))start++;if(*start!='\"')return -1;end=start+1;while(*end){if(*end=='\\'){if(!end[1])return -1;end+=2;}else if(*end=='\"'){end++;break;}else end++;}if(end[-1]!='\"')return -1;prefix=(size_t)(start-old);
 int z=snprintf(out,sizeof out,"%.*s\"%s\"%s",(int)prefix,old,escaped,end);if(z<0||(size_t)z>=sizeof out)return -1;return config_write_atomic(S.config_path,out,(size_t)z);
 }
 end=old+n;while(end>old&&isspace((unsigned char)end[-1]))end--;if(end==old||end[-1]!='}')return -1;prefix=(size_t)(end-old-1);char *q=old+1;while(isspace((unsigned char)*q))q++;int z=snprintf(out,sizeof out,"%.*s%s\"%s\":\"%s\"}\n",(int)prefix,old,*q=='}'?"":",",field,escaped);if(z<0||(size_t)z>=sizeof out)return -1;return config_write_atomic(S.config_path,out,(size_t)z);
}
static int provision(struct client *c,const void *p,size_t n){struct ep_reader r;struct ep_field f;unsigned char key[32],b[8];char base64[45];size_t z;int found=0,ok=0;ep_reader_init(&r,p,n);while(ep_next(&r,&f)>0)if(f.number==1){if(found++||f.wire!=2||f.length!=32)return -1;memcpy(key,f.data,32);}if(!found)return -1;
 if(c->mode==1&&!mbedtls_base64_encode((unsigned char*)base64,sizeof base64,&z,key,32)&&z==44){base64[z]=0;if(config_set_string("esphome_noise_key",base64)==0){memcpy(S.psk,key,32);S.provisioned=1;ok=1;}}
 struct ep_writer w={b,sizeof b,0,0};ep_uint(&w,1,ok);int rc=send_writer(c,125,&w);mbedtls_platform_zeroize(key,sizeof key);mbedtls_platform_zeroize(base64,sizeof base64);if(ok){/* current encrypted session can read its ack; all sessions reconnect */for(unsigned i=0;i<CLIENTS;i++)if(S.clients[i].fd>=0)S.clients[i].closing=1;}return rc;
}
static int event_data(const void *p,size_t n,const char *name,char *out,size_t cap){struct ep_reader r;struct ep_field f;unsigned count=0;ep_reader_init(&r,p,n);out[0]=0;while(ep_next(&r,&f)>0)if(f.number==2&&f.wire==2){char k[64],v[1024];if(++count>32||valid_proto(f.data,f.length)||ep_get_text(f.data,f.length,1,k,sizeof k)||ep_get_text(f.data,f.length,2,v,sizeof v))return -1;if(!strcmp(k,name)){if(strlen(v)>=cap)return -1;memcpy(out,v,strlen(v)+1);return 1;}}return r.error?-1:0;}
static void voice_event(const void *p,size_t n){unsigned event=(unsigned)ep_get_uint(p,n,1,0);char v[1024];if(!S.turn)return;
 if(event_data(p,n,"url",v,sizeof v)<0){cancel_turn("bad_event");return;}if(v[0])snprintf(S.tts_url,sizeof S.tts_url,"%s",v);
 if(event==0){cancel_turn("ha_error");return;}if(event==12)capture_end();
 if(event==6){if(event_data(p,n,"conversation_id",S.conversation,sizeof S.conversation)<0){cancel_turn("bad_event");return;}if(event_data(p,n,"continue_conversation",v,sizeof v)<0){cancel_turn("bad_event");return;}S.continue_turn=!strcmp(v,"1");}
 if(event==100){if(event_data(p,n,"tts_start_streaming",v,sizeof v)<0){cancel_turn("bad_event");return;}S.early_tts=!strcmp(v,"1");}
 if((event==100&&S.early_tts)||event==8){capture_end();if(!S.playing&&!S.tts_done&&S.tts_url[0]&&playback_start(S.tts_url)<0){(void)announce_done(0);cancel_turn("playback_error");return;}}
 if(event==2){S.run_end=1;if(!S.playing&&(S.tts_done||!S.tts_url[0])){capture_end();completed();}}
}
static int dispatch(unsigned idx,unsigned type,const unsigned char *p,size_t n){struct client *c=&S.clients[idx];unsigned char b[128];struct ep_writer w={b,sizeof b,0,0};char args[2048],url[1024],escaped[2048];if(valid_proto(p,n))return -1;
 if(type==1){if(c->hello)return -1;ep_uint(&w,1,1);ep_uint(&w,2,14);ep_string(&w,3,"LibreEcho 0.14.0");ep_string(&w,4,S.name);c->hello=1;c->deadline=S.now+120000;status_write();return send_writer(c,2,&w);}
 if(type==3)return 0;/* Legacy Auth is obsolete; pinned client does not await it. */
 if(!c->hello)return -1;
 switch(type){case 7:return send_msg(c,8,NULL,0);case 8:return 0;case 5:if(send_msg(c,6,NULL,0))return -1;c->closing=1;return 0;case 9:device_info(c);return 0;case 11:entities(c);return 0;
 case 20:c->states=1;states_send(c);return 0;
 case 89:if(!ep_get_uint(p,n,1,0)){if(S.owner==(int)idx){cancel_turn("unsubscribed");S.owner=-1;timer_owner_end();}return 0;}if(S.owner>=0&&S.owner!=(int)idx)return -1;
 /* Pinned client API_AUDIO subscription flag is 1<<2 (4), not 1. */
 if(!(ep_get_uint(p,n,2,0)&4))return -1;S.owner=(int)idx;return 0;
 case 91:if(S.owner!=(int)idx||S.turn!=1)return 0;if(ep_get_uint(p,n,2,0)||ep_get_uint(p,n,1,0)){cancel_turn("ha_rejected");return 0;}S.turn=2;return 0;
 case 92:if(S.owner==(int)idx)voice_event(p,n);return 0;
 case 119:if(S.owner!=(int)idx)return -1;if(S.turn){(void)announce_done(0);return 0;}
 if(ep_get_text(p,n,1,url,sizeof url)||ep_get_text(p,n,3,S.next_url,sizeof S.next_url))return -1;S.turn=3;S.announcement=1;S.turn_deadline=S.now+S.turn_limit;S.continue_turn=!!ep_get_uint(p,n,4,0);snprintf(S.tts_url,sizeof S.tts_url,"%s",url);
 if(S.next_url[0]){char pre[1024];snprintf(pre,sizeof pre,"%s",S.next_url);snprintf(S.next_url,sizeof S.next_url,"%s",url);if(playback_start(pre)<0)turn_clear("playback_error",1);}else if(playback_start(url)<0)turn_clear("playback_error",1);status_write();return 0;
 case 121:configuration(c);return 0;
 case 123:{struct ep_reader r;struct ep_field f;unsigned count=0;int active=0;ep_reader_init(&r,p,n);while(ep_next(&r,&f)>0)if(f.number==1){if(++count>1||ep_text(&f,url,sizeof url)||strcmp(url,"alexa_v0.1"))return -1;active=1;}
 if(config_set_string("esphome_active_wake_word",active?"alexa_v0.1":"")<0)return -1;
 if(!active){S.active_wake=0;cancel_turn("wake_disabled");configuration(c);return 0;}
 if(job(S.wake_path,"set_word","{\"word\":\"Alexa\"}",5,1)<0)return -1;S.active_wake=1;configuration(c);return 0;}
 case 124:return provision(c,p,n);
 case 33:if(ep_get_uint(p,n,1,0)!=MUTE_KEY)return -1;{int mute=!!ep_get_uint(p,n,2,0);if(!mute&&S.privacy!=0){all_states();return 0;}if(mute){S.local_mute_gate=1;S.muted=1;if(S.turn==1)cancel_turn("muted");else if(S.turn==2)capture_end();all_states();}snprintf(args,sizeof args,"{\"muted\":%s}",mute?"true":"false");return job(S.audio_path,"set_mute",args,3,mute);}
 case 65:if(ep_get_uint(p,n,1,0)!=MEDIA_KEY)return -1;
 if(ep_get_uint(p,n,4,0)){uint32_t bits=(uint32_t)ep_get_uint(p,n,5,0);float volume;memcpy(&volume,&bits,4);if(!isfinite(volume)||volume<0||volume>1)return -1;snprintf(args,sizeof args,"{\"volume\":%d}",(int)lroundf(volume*100));if(job(S.audio_path,"set_volume",args,4,(int)lroundf(volume*100)))return -1;}
 if(ep_get_uint(p,n,6,0)){if(ep_get_text(p,n,7,url,sizeof url)||strlen(url)>900)return -1;json_escape(escaped,sizeof escaped,url);snprintf(args,sizeof args,"{\"url\":\"%.1900s\"}",escaped);
 /* Finite announcement completion (type120) belongs to the voice owner.
 * Other clients retain ordinary radio/media controls, never this channel. */
 if(ep_get_uint(p,n,8,0)&&ep_get_uint(p,n,9,0)){if(S.owner!=(int)idx||S.turn)return 0;S.turn=3;S.turn_deadline=S.now+S.turn_limit;if(playback_start(url)<0)turn_clear("playback_error",1);}else if(job(S.radio_path,"play",args,0,0))return -1;}
 if(ep_get_uint(p,n,2,0)){unsigned command=(unsigned)ep_get_uint(p,n,3,0);const char *cmd=NULL;if(command==0)cmd="resume";else if(command==1)cmd="pause";else if(command==2)cmd="stop";else if(command==5)cmd=S.radio_state==2?"pause":"resume";
 if(cmd){if(job(S.radio_path,cmd,"{}",0,0))return -1;}else if(command==3||command==4){int target=command==3?0:S.restore_volume;if(command==3&&!S.output_muted)S.restore_volume=S.volume;snprintf(args,sizeof args,"{\"volume\":%d}",target);if(job(S.audio_path,"set_volume",args,6,command==3))return -1;}else return -1;}return 0;
 case 115:if(S.owner!=(int)idx)return -1;{char id[128],name[128],ei[256],en[256];unsigned event=(unsigned)ep_get_uint(p,n,1,0);uint64_t total=ep_get_uint(p,n,4,0),left=ep_get_uint(p,n,5,0);if(event>3||total>604800||left>604800||ep_get_text(p,n,2,id,sizeof id)||!id[0]||ep_get_text(p,n,3,name,sizeof name))return -1;json_escape(ei,sizeof ei,id);json_escape(en,sizeof en,name);snprintf(args,sizeof args,"{\"event_type\":%u,\"timer_id\":\"%s\",\"name\":\"%s\",\"total_seconds\":%llu,\"seconds_left\":%llu,\"is_active\":%s}",event,ei,en,(unsigned long long)total,(unsigned long long)left,ep_get_uint(p,n,6,0)?"true":"false");return job(S.timer_path,"remote_event",args,JOB_TIMER,0);}
 default:return 0;}
}
static int process_rx(unsigned idx){struct client *c=&S.clients[idx];unsigned packets=0;
 while(c->rx_n&&packets++<32){size_t used,len;unsigned type;int rc;if(c->mode<0){if(c->rx[0]==1){c->mode=1;c->phase=0;}else if(c->rx[0]==0&&S.plaintext&&!S.provisioned){c->mode=0;c->phase=2;}
 /* Real ESPHome answers a plaintext hello on an encrypted API with a Noise-framed
  * "\1Bad indicator byte"; clients map the 0x01 preamble to "requires encryption"
  * and Home Assistant then asks for the key instead of claiming the YAML lacks api:. */
 else if(c->rx[0]==0){static const unsigned char reject[]="\1Bad indicator byte";c->mode=1;c->phase=0;c->rx_n=0;if(outer(c,reject,sizeof reject-1))return -1;c->closing=1;return 0;}else return -1;}
 rc=ef_parse(c->rx,c->rx_n,c->mode==1,&used,&type,&len);if(rc<=0)return rc;
 const unsigned char *p=c->rx+used-len;if(c->mode==1&&c->phase<2){unsigned char reply[128];size_t z;
 if(c->phase==0){if(len)return -1;reply[0]=1;z=1;size_t n=strlen(S.name)+1;memcpy(reply+z,S.name,n);z+=n;/* ESPHome sends the bare 12-digit lowercase MAC here; aioesphomeapi caps this field at 16
    characters, so the colon form would arrive truncated and Home Assistant would treat a key
    change as a different device instead of asking for the new key. */
 {char hello_mac[13];size_t m=0;for(size_t i=0;S.mac[i]&&m<12;i++)if(S.mac[i]!=':')hello_mac[m++]=(char)tolower((unsigned char)S.mac[i]);hello_mac[m]=0;n=m+1;memcpy(reply+z,hello_mac,n);z+=n;}if(outer(c,reply,z))return -1;c->phase=1;}
 else{if(en_handshake(&c->noise,S.psk,p,len,reply,&z)){static const unsigned char err[]="\1Handshake MAC failure";if(outer(c,err,sizeof err-1))return -1;c->closing=1;}else{if(outer(c,reply,z))return -1;c->phase=2;}}
 }else if(c->mode==1){unsigned char plain[RX_CAP];size_t z;if(en_decrypt(&c->noise,p,len,plain,sizeof plain,&z)||z<4||(((size_t)plain[2]<<8)|plain[3])!=z-4)return -1;type=((unsigned)plain[0]<<8)|plain[1];int result=dispatch(idx,type,plain+4,z-4);mbedtls_platform_zeroize(plain,z);if(result)return -1;}
 else if(dispatch(idx,type,p,len))return -1;
 memmove(c->rx,c->rx+used,c->rx_n-used);c->rx_n-=used;if(c->closing)return 0;
 }return 0;
}
static void client_tick(unsigned idx,short events){struct client *c=&S.clients[idx];if(c->fd<0)return;if(S.now>c->deadline){client_close(idx);return;}
 /* aioesphomeapi only pings after hearing nothing, and our state stream keeps
  * it quiet, so probe a silent peer ourselves well inside the idle deadline. */
 if(c->hello&&!c->closing&&S.now>=c->last_seen+60000&&S.now>=c->ping_at+30000){c->ping_at=S.now;(void)send_msg(c,7,NULL,0);}
 if((events&POLLIN)&&!c->closing){ssize_t n=recv(c->fd,c->rx+c->rx_n,sizeof c->rx-c->rx_n,MSG_DONTWAIT);if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK))n=-2;if(n==-2){}else if(n<=0){client_close(idx);return;}else{c->rx_n+=(size_t)n;c->last_seen=S.now;if(c->hello)c->deadline=S.now+120000;if(process_rx(idx)<0||c->rx_n==sizeof c->rx){client_close(idx);return;}}}
 if(c->tx_sent<c->tx_n){ssize_t n=send(c->fd,c->tx+c->tx_sent,c->tx_n-c->tx_sent,MSG_DONTWAIT|MSG_NOSIGNAL);if(n<0&&errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR){client_close(idx);return;}if(n>0)c->tx_sent+=(size_t)n;if(c->tx_sent==c->tx_n)c->tx_sent=c->tx_n=0;}
 if((c->closing&&!c->tx_n)||(events&(POLLERR|POLLNVAL))){client_close(idx);return;}
 /* A complete buffered burst is consumed on later ticks even without POLLIN. */
 if(c->rx_n&&!c->closing&&process_rx(idx)<0)client_close(idx);
}
static uint32_t le32(const unsigned char *p){return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static void ring_frame(const unsigned char *p){uint64_t first=le32(p+8)|((uint64_t)le32(p+12)<<32);unsigned count=le32(p+16);if(first>UINT64_MAX-count){cancel_turn("stream_error");return;}
 if(S.ring_end&&first!=S.ring_end){S.ring_first=S.ring_end=first;if(S.turn==2)cancel_turn("stream_gap");}
 if(!S.ring_end)S.ring_first=first;for(unsigned i=0;i<count;i++){unsigned char const *a=p+24+2*i;S.ring[(first+i)%RING_SAMPLES]=(int16_t)((unsigned)a[0]|((unsigned)a[1]<<8));}S.ring_end=first+count;if(S.ring_end-S.ring_first>RING_SAMPLES)S.ring_first=S.ring_end-RING_SAMPLES;
}
static void stream_loss(struct io *j){io_close(j);if(j==&S.mic){S.mic_ack=0;S.mic_n=0;S.ring_first=S.ring_end=0;}if(S.turn==1||S.turn==2)cancel_turn("stream_error");S.wake_retry=S.now+1000;}
static void stream_tick(struct io *j,int audio){if(j->fd<0)return;
 if(j->stage<3){int rc=io_tick(j);if(rc<0){stream_loss(j);return;}if(!rc)return;char *end=memchr(j->rx,'\n',j->rx_n);*end=0;int ok=0;if(json_get_bool(j->rx,"ok",&ok)<=0||!ok){stream_loss(j);return;}
 if(audio){char format[32];int rate,channels,header,indexed;if(json_get_string(j->rx,"format",format,sizeof format)<=0||strcmp(format,"pcm_s16_le")||json_get_int(j->rx,"sample_rate",&rate)<=0||rate!=16000||json_get_int(j->rx,"channels",&channels)<=0||channels!=1||json_get_int(j->rx,"frame_header_bytes",&header)<=0||header!=24||json_get_bool(j->rx,"sample_indexed",&indexed)<=0||!indexed){stream_loss(j);return;}}
 size_t used=(size_t)(end-j->rx)+1;memmove(j->rx,j->rx+used,j->rx_n-used);j->rx_n-=used;j->stage=3;j->deadline=UINT64_MAX;}
 if(!audio){/* Bounded event lines, including fragmented/coalesced reads. */
 for(unsigned loop=0;loop<8;loop++){char *end=memchr(j->rx,'\n',j->rx_n);if(!end)break;*end=0;long long sample;char event[64];if(json_get_string(j->rx,"event",event,sizeof event)>0&&!strcmp(event,"wake_detected")&&json_get_int64(j->rx,"detection_sample",&sample)>0&&sample>=0)(void)voice_start((uint64_t)sample,0);size_t z=(size_t)(end-j->rx)+1;memmove(j->rx,j->rx+z,j->rx_n-z);j->rx_n-=z;}
 if(j->rx_n==sizeof j->rx-1){stream_loss(j);return;}ssize_t n=recv(j->fd,j->rx+j->rx_n,sizeof j->rx-1-j->rx_n,MSG_DONTWAIT);if(n>0){j->rx_n+=(size_t)n;j->rx[j->rx_n]=0;}else if(!n||(errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR))stream_loss(j);return;
 }
 /* First consume bytes coalesced with the JSON stream handshake. */
 for(unsigned loop=0;loop<16;loop++){size_t need=24;if(S.mic_n>=24){unsigned count=le32(S.mic_rx+16);if(memcmp(S.mic_rx,"LVS1",4)||S.mic_rx[4]!=1||S.mic_rx[5]||S.mic_rx[6]||S.mic_rx[7]||le32(S.mic_rx+20)||!count||count>1280){stream_loss(j);return;}need+=count*2;}
 if(S.mic_n==need&&need>24){ring_frame(S.mic_rx);S.mic_n=0;continue;}
 size_t want=need-S.mic_n;if(j->rx_n){size_t z=j->rx_n<want?j->rx_n:want;memcpy(S.mic_rx+S.mic_n,j->rx,z);S.mic_n+=z;memmove(j->rx,j->rx+z,j->rx_n-z);j->rx_n-=z;continue;}
 ssize_t n=recv(j->fd,S.mic_rx+S.mic_n,want,MSG_DONTWAIT);if(n>0){S.mic_n+=(size_t)n;continue;}if(!n||(errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR))stream_loss(j);break;}
}
static void capture_tick(void){if(S.turn!=2||S.owner<0)return;if(mic_muted()){capture_end();return;}if(S.capture_next<S.ring_first){cancel_turn("capture_overrun");return;}
 /* 20 ms PCM frames, at most four per tick, retaining indexed 200 ms preroll. */
 for(unsigned i=0;i<4&&S.ring_end-S.capture_next>=320;i++){unsigned char pcm[640],b[660];struct ep_writer w={b,sizeof b,0,0};for(unsigned k=0;k<320;k++){uint16_t v=(uint16_t)S.ring[(S.capture_next+k)%RING_SAMPLES];pcm[2*k]=(unsigned char)v;pcm[2*k+1]=(unsigned char)(v>>8);}ep_bytes(&w,1,pcm,sizeof pcm);
 if(send_writer(&S.clients[S.owner],106,&w)){cancel_turn("backpressure");return;}S.capture_next+=320;}
}
static void jobs_tick(void){for(unsigned i=0;i<JOBS;i++){struct io *j=&S.jobs[i];if(j->stage==3&&j->kind==JOB_TIMER){if(S.timer_pending)continue;if(io_connect(j,S.timer_path)<0){io_close(j);continue;}j->stage=1;j->deadline=S.now+500;}if(j->fd<0)continue;int rc=io_tick(j);if(!rc)continue;int ok=0;if(rc>0){char *end=memchr(j->rx,'\n',j->rx_n);*end=0;(void)json_get_bool(j->rx,"ok",&ok);}if(ok){
 if(j->kind==1){int muted,volume;if(json_get_bool(j->rx,"muted",&muted)>0&&json_get_int(j->rx,"volume",&volume)>0&&volume>=0&&volume<=100){S.muted=muted;S.volume=volume;S.last_audio_status=S.now;}}
 if(j->kind==2){int playing=0,paused=0;(void)json_get_bool(j->rx,"playing",&playing);(void)json_get_bool(j->rx,"paused",&paused);S.radio_state=paused?3:playing?2:1;}
 if(j->kind==3){S.muted=j->target;S.local_mute_gate=j->target;}if(j->kind==4)S.volume=j->target;if(j->kind==6){S.output_muted=j->target;S.volume=j->target?0:S.restore_volume;}
 }else if(j->kind>=3&&j->kind<=6&&S.turn)cancel_turn("adapter_error");io_close(j);all_states();}timer_cleanup_tick();}
static void privacy_poll(void){char b[32];int n=config_read(S.privacy_path,b,sizeof b),old=S.privacy;if(n<0)S.privacy=-1;else if(b[0]=='0')S.privacy=0;else if(b[0]=='1')S.privacy=1;else S.privacy=-1;if(old!=S.privacy){if(mic_muted()&&(S.turn==1||S.turn==2))capture_end();all_states();}}
static int normalize_mac(const char *input,char *out){unsigned char b[6];unsigned n=0,high=0;int hi=-1;for(size_t i=0;input[i];i++){int x=input[i];if(x==':'||isspace((unsigned char)x))continue;if(x>='0'&&x<='9')x-='0';else if(x>='a'&&x<='f')x=x-'a'+10;else if(x>='A'&&x<='F')x=x-'A'+10;else return -1;if(hi<0)hi=x;else{if(n>=6)return -1;b[n++]=(unsigned char)(hi*16+x);high|=b[n-1];hi=-1;}}if(n!=6||hi>=0||!high||(b[0]&1))return -1;snprintf(out,18,"%02x:%02x:%02x:%02x:%02x:%02x",b[0],b[1],b[2],b[3],b[4],b[5]);return 0;}
static int load_configuration(void){char b[16384],key[45],mac[64],name[64],wake[64];int n=config_read(S.config_path,b,sizeof b);struct le_esphome_config cfg;size_t z;if(n<0||config_esphome_read(S.config_path,&cfg))return -1;
 int selected=json_get_string_top_level(b,"esphome_active_wake_word",wake,sizeof wake);if(selected<0||json_duplicate_key(b,(size_t)n,"esphome_active_wake_word")||(selected>0&&wake[0]&&strcmp(wake,"alexa_v0.1")))return -1;S.active_wake=selected<=0||!!wake[0];
 /* HA ownership is what the web API persists: integrations bit 1 or the
  * home-assistant pipeline mode (api.c applies the same rule on load).
  * voice_assistant_mode is a legacy field the API never writes. */
 {unsigned integrations=0;char pipeline[32];int mode=0;
  S.ha_selected=(json_get_uint(b,"integrations",&integrations)==1&&(integrations&1u))||
   (json_get_string_top_level(b,"voice_pipeline_mode",pipeline,sizeof pipeline)>0&&!strcmp(pipeline,"home-assistant"))||
   (json_get_int(b,"voice_assistant_mode",&mode)>0&&(mode&1));}memcpy(key,cfg.esphome_noise_key,sizeof key);
 /* Like an ESPHome YAML `api: encryption: key:`, the satellite always owns a
  * key. Generate one on first start so the owner can copy it from the web UI
  * into Home Assistant; a key Home Assistant later pushes replaces it. */
 if(!key[0]&&!S.plaintext){unsigned char fresh[32];size_t fz;int fd=open("/dev/urandom",O_RDONLY|O_CLOEXEC),got=fd>=0&&read(fd,fresh,32)==32;if(fd>=0)close(fd);
  if(!got||mbedtls_base64_encode((unsigned char*)key,sizeof key,&fz,fresh,32)||fz!=44||config_set_string("esphome_noise_key",key)){mbedtls_platform_zeroize(fresh,32);mbedtls_platform_zeroize(key,sizeof key);return -1;}
  key[44]=0;mbedtls_platform_zeroize(fresh,32);n=config_read(S.config_path,b,sizeof b);if(n<0)return -1;}
 S.provisioned=!!key[0];memset(S.psk,0,32);if(S.provisioned&&(mbedtls_base64_decode(S.psk,32,&z,(unsigned char*)key,44)||z!=32)){mbedtls_platform_zeroize(key,sizeof key);return -1;}mbedtls_platform_zeroize(key,sizeof key);mbedtls_platform_zeroize(&cfg,sizeof cfg);
 if(!S.name[0]){if(json_get_string(b,"hostname",name,sizeof name)<=0)snprintf(name,sizeof name,"libreecho");if(strlen(name)>31||!name[0])return -1;for(size_t i=0;name[i];i++)if(!(isalnum((unsigned char)name[i])||name[i]=='-'))return -1;snprintf(S.name,sizeof S.name,"%.31s",name);}
 if(!S.friendly[0])snprintf(S.friendly,sizeof S.friendly,"%s",S.name);
 if(!S.mac[0]){if(json_get_string(b,"wifi_mac",mac,sizeof mac)>0&&mac[0]){if(normalize_mac(mac,S.mac))return -1;}else{static const char *fields[]={"mac_addr","macaddr","wifi_mac","wifi_mac_addr","mac"};int found=0;for(unsigned i=0;i<5;i++){char path[512];if(snprintf(path,sizeof path,"%s/%s/value",S.idme_path,fields[i])>=(int)sizeof path)return -1;if(config_read(path,mac,sizeof mac)>0&&!normalize_mac(mac,S.mac)){found=1;break;}}if(!found)return -1;}}
 return 0;
}
static void mdns_tick(unsigned port){if(!S.ha_selected){if(S.mdns_fd>=0){close(S.mdns_fd);S.mdns_fd=-1;}return;}if(S.mdns_fd>=0){if(le_mdns_receive(S.mdns_fd)<0){close(S.mdns_fd);S.mdns_fd=-1;S.mdns_retry=S.now+1000;}return;}if(S.now<S.mdns_retry)return;struct le_mdns_esphome_metadata m;memset(&m,0,sizeof m);snprintf(m.version,sizeof m.version,"2026.9.0");snprintf(m.mac,sizeof m.mac,"%s",S.mac);snprintf(m.platform,sizeof m.platform,"LibreEcho");snprintf(m.board,sizeof m.board,"radar-puffin");snprintf(m.network,sizeof m.network,"wifi");snprintf(m.friendly_name,sizeof m.friendly_name,"%s",S.friendly);snprintf(m.api_encryption,sizeof m.api_encryption,"%s",LE_MDNS_ESPHOME_NOISE);snprintf(m.project_name,sizeof m.project_name,"libreecho.satellite");snprintf(m.project_version,sizeof m.project_version,"0.14.0");S.mdns_fd=le_mdns_register_esphome(S.mdns_path,port,&m);S.mdns_retry=S.now+1000;}
int main(int argc,char **argv){const char *bind_addr="0.0.0.0";unsigned port=6053;struct sockaddr_in a;int listener;
 if(argc==7&&(!strcmp(argv[1],"--health-check")||!strcmp(argv[1],"--process-check"))){
  char *end;unsigned long p;if(!le_eh_decimal(argv[4]))return 2;p=strtoul(argv[4],&end,10);if(*end||p<1||p>65535)return 2;
  return le_esphome_health(argv[2],argv[3],argv[5],(unsigned)p,argv[6],!strcmp(argv[1],"--process-check"))?0:1;
 }
 memset(&S,0,sizeof S);S.owner=-1;S.mdns_fd=-1;S.muted=1;S.privacy=-1;S.radio_state=1;S.active_wake=1;S.restore_volume=66;S.turn_limit=120000;
 S.config_path=getenv("LE_CONFIG_PATH");if(!S.config_path||!S.config_path[0])S.config_path="/etc/libreecho/web-config.json";S.status_path="/run/libreecho/esphome-status.json";S.audio_path="/run/libreecho/audio.sock";S.wake_path="/run/libreecho/wakeword.sock";S.radio_path="/run/libreecho/radio.sock";S.timer_path="/run/libreecho/timer.sock";S.led_path="/run/libreecho/led.sock";S.mdns_path=LE_MDNS_SOCKET;S.bus_path="/run/libreecho-audio/system.pcm";S.privacy_path="/sys/devices/platform/amz_privacy/privacy_state";S.idme_path="/sys/firmware/devicetree/base/idme";
 for(unsigned i=0;i<CLIENTS;i++)S.clients[i].fd=-1;for(unsigned i=0;i<JOBS;i++)S.jobs[i].fd=-1;S.wake.fd=S.mic.fd=S.timer_cleanup.fd=-1;esp_playback_init(&S.playback);
 for(int i=1;i<argc;i++){const char *v=i+1<argc?argv[i+1]:NULL;if(!strcmp(argv[i],"--plaintext")){S.plaintext=1;continue;}if(!strcmp(argv[i],"--stream-wav")){S.stream_wav=1;continue;}if(!v)return 2;
 if(!strcmp(argv[i],"--port")){char *end;unsigned long p=strtoul(v,&end,10);if(*end||p<1||p>65535)return 2;port=(unsigned)p;}
 else if(!strcmp(argv[i],"--bind")||!strcmp(argv[i],"--listen"))bind_addr=v;else if(!strcmp(argv[i],"--config"))S.config_path=v;else if(!strcmp(argv[i],"--status-file"))S.status_path=v;
 else if(!strcmp(argv[i],"--audio-socket"))S.audio_path=v;else if(!strcmp(argv[i],"--wake-socket"))S.wake_path=v;else if(!strcmp(argv[i],"--radio-socket"))S.radio_path=v;else if(!strcmp(argv[i],"--timer-socket"))S.timer_path=v;else if(!strcmp(argv[i],"--led-socket"))S.led_path=v;else if(!strcmp(argv[i],"--mdns-socket"))S.mdns_path=v;else if(!strcmp(argv[i],"--audio-bus"))S.bus_path=v;else if(!strcmp(argv[i],"--privacy-state"))S.privacy_path=v;else if(!strcmp(argv[i],"--tls-ca"))S.ca_path=v;else if(!strcmp(argv[i],"--idme-root"))S.idme_path=v;
 else if(!strcmp(argv[i],"--name")){if(strlen(v)>=sizeof S.name)return 2;snprintf(S.name,sizeof S.name,"%s",v);}else if(!strcmp(argv[i],"--mac")){if(normalize_mac(v,S.mac))return 2;}
 else if(!strcmp(argv[i],"--turn-timeout-ms")){char *end;unsigned long p=strtoul(v,&end,10);if(*end||p<100||p>120000)return 2;S.turn_limit=p;}else return 2;i++;}
 if(load_configuration()){fprintf(stderr,"esphomed: invalid config or missing stable MAC\n");return 1;}
 /* Explicit diagnostics never overrides a provisioned key. */
 memset(&a,0,sizeof a);a.sin_family=AF_INET;a.sin_port=htons((uint16_t)port);if(inet_pton(AF_INET,bind_addr,&a.sin_addr)!=1)return 2;
 listener=socket(AF_INET,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);int yes=1;if(listener<0)return 1;(void)setsockopt(listener,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof yes);if(bind(listener,(struct sockaddr*)&a,sizeof a)||listen(listener,CLIENTS)){close(listener);return 1;}
 {struct stat st;if(fstat(listener,&st)||!le_eh_start((int)getpid(),S.process_start)||!le_eh_boot(S.boot_id)){close(listener);return 1;}snprintf(S.listener_inode,sizeof S.listener_inode,"%llu",(unsigned long long)st.st_ino);S.listener_port=port;}
 signal(SIGINT,sigstop);signal(SIGTERM,sigstop);signal(SIGHUP,sigstop);signal(SIGPIPE,SIG_IGN);S.now=now_ms();
 /* timerd outlives this process. Fence replacement events until a retried
  * reserved clear is ACKed, including recovery after SIGKILL/power loss. */
 timer_owner_end();S.ready=1;status_write();
 while(running){struct pollfd fds[1+CLIENTS];fds[0]=(struct pollfd){listener,POLLIN,0};for(unsigned i=0;i<CLIENTS;i++)fds[i+1]=(struct pollfd){S.clients[i].fd,(short)(POLLIN|(S.clients[i].tx_n?POLLOUT:0)),0};int ready=poll(fds,1+CLIENTS,10);S.now=now_ms();if(ready<0&&errno!=EINTR)break;
 if(reload_config){reload_config=0;for(unsigned i=0;i<CLIENTS;i++)client_close(i);if(load_configuration())break;}
 if(fds[0].revents&POLLIN){for(unsigned n=0;n<4;n++){int fd=accept(listener,NULL,NULL);if(fd<0)break;int flags=fcntl(fd,F_GETFL,0);if(flags<0||fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0){close(fd);continue;}unsigned idx;for(idx=0;idx<CLIENTS;idx++)if(S.clients[idx].fd<0)break;if(idx==CLIENTS){close(fd);continue;}struct client *c=&S.clients[idx];memset(c,0,sizeof *c);c->fd=fd;c->mode=-1;c->deadline=S.now+15000;c->last_seen=S.now;en_init(&c->noise);}}
 for(unsigned i=0;i<CLIENTS;i++)client_tick(i,fds[i+1].revents);
 if(S.now>=S.last_status_poll+250){S.last_status_poll=S.now;privacy_poll();(void)job(S.audio_path,"status","{}",1,0);(void)job(S.radio_path,"status","{}",2,0);}
 jobs_tick();if(S.now>=S.wake_retry){if(S.wake.fd<0)(void)io_open(&S.wake,S.wake_path,"subscribe","{}",0);if(S.mic.fd<0)(void)io_open(&S.mic,S.wake_path,"stream_audio","{}",0);S.wake_retry=S.now+1000;}
 stream_tick(&S.mic,1);stream_tick(&S.wake,0);capture_tick();
 if(S.turn&&S.now>S.turn_deadline)cancel_turn("timeout");
 if(S.playing){int rc=esp_playback_tick(&S.playback,S.now);if(rc){S.playing=0;if(rc<0)turn_clear("playback_error",1);else if(S.next_url[0]){char next[1024];snprintf(next,sizeof next,"%s",S.next_url);S.next_url[0]=0;if(playback_start(next)<0)turn_clear("playback_error",1);}else completed();}}
 mdns_tick(port);
 }
 S.ready=0;for(unsigned i=0;i<CLIENTS;i++)client_close(i);
 close(listener);io_close(&S.wake);io_close(&S.mic);esp_playback_close(&S.playback);if(S.mdns_fd>=0)close(S.mdns_fd);
 /* No new owners/events during shutdown. Retire unsent jobs, settle sent
  * ones, then clear via the reserved slot. Adapter outage must not hang exit;
  * the next startup keeps its cleanup fence until it receives an ACK. */
 S.now=now_ms();timer_owner_end();
 for(unsigned i=0;i<JOBS;i++)if(S.jobs[i].kind!=JOB_TIMER_RETIRED)io_close(&S.jobs[i]);
 uint64_t cleanup_limit=S.now+1500;
 while(S.timer_pending&&S.now<cleanup_limit){jobs_tick();if(S.timer_pending)(void)poll(NULL,0,10);S.now=now_ms();}
 for(unsigned i=0;i<JOBS;i++)io_close(&S.jobs[i]);io_close(&S.timer_cleanup);mbedtls_platform_zeroize(S.psk,sizeof S.psk);return 0;
}
