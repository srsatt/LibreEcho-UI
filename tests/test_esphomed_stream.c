#define _POSIX_C_SOURCE 200809L
#include "../src/adapter/esphome_playback.c"
#include <assert.h>
#include <sys/stat.h>

static struct esp_playback p;
static char bus[256];
static void put32(unsigned char *b,uint32_t n){for(unsigned i=0;i<4;i++)b[i]=(unsigned char)(n>>(8*i));}
static void make_wav(unsigned char *b,size_t samples){size_t n=44+samples*2;memset(b,0,n);memcpy(b,"RIFF",4);put32(b+4,UINT32_MAX);memcpy(b+8,"WAVEfmt ",8);put32(b+16,16);b[20]=1;b[22]=1;put32(b+24,24000);put32(b+28,48000);b[32]=2;b[34]=16;memcpy(b+36,"data",4);put32(b+40,UINT32_MAX);for(size_t i=0;i<samples;i++){b[44+i*2]=0xe8;b[45+i*2]=3;}}
static int fetch(void){int f[2];assert(socketpair(AF_UNIX,SOCK_STREAM,0,f)==0);esp_playback_init(&p);p.fd=f[0];p.state=5;p.stream_wav=1;p.expected=SIZE_MAX;p.deadline=120000;p.stream_idle=15000;snprintf(p.bus,sizeof p.bus,"%s",bus);return f[1];}
static void send_all(int fd,const void *bytes,size_t len){const unsigned char *b=bytes;while(len){ssize_t n=send(fd,b,len,MSG_NOSIGNAL);assert(n>0);b+=n;len-=(size_t)n;}}
static int pump(uint64_t *now,unsigned n){int rc=0;for(unsigned i=0;i<n&&!rc;i++){rc=esp_playback_tick(&p,*now);*now+=20;}return rc;}
static void chunk(int fd,const unsigned char *b,size_t len){char line[32];int n=snprintf(line,sizeof line,"%zx\r\n",len);assert(n>0&&(size_t)n<sizeof line);send_all(fd,line,(size_t)n);send_all(fd,b,len);send_all(fd,"\r\n",2);}
static void early_chunked(void){static unsigned char wav[44+48000];uint64_t now=0;int peer=fetch();make_wav(wav,24000);
 const char *h="HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";send_all(peer,h,19);assert(!pump(&now,1)&&!p.header_done);send_all(peer,h+19,strlen(h)-19);
 chunk(peer,wav,44+16000);assert(!pump(&now,20));assert(p.state==6&&p.frames>0&&!p.stream_done&&p.fd>=0); /* speaker began before HTTP completion */
 for(size_t pos=44+16000;pos<sizeof wav;){size_t n=sizeof wav-pos;if(n>4096)n=4096;chunk(peer,wav+pos,n);pos+=n;assert(!pump(&now,2));}
 send_all(peer,"0\r\nX-Test: yes\r\n",16);assert(!pump(&now,1)&&!p.stream_done);send_all(peer,"\r\n",2);
 assert(pump(&now,200)==1&&p.result==1&&p.frames==48000);close(peer);
 int fd=open(bus,O_RDONLY);assert(fd>=0);struct stat st;assert(fstat(fd,&st)==0&&st.st_size==192000);unsigned char out[4];assert(read(fd,out,4)==4);assert(out[0]==0xe8&&out[1]==3&&out[2]==0xe8&&out[3]==3);close(fd);
 puts("chunked PCM WAV starts before response completion, drains and resamples: OK");
}
static void stereo_before_eof(void){static unsigned char wav[44+96000];uint64_t now=0;int peer=fetch();make_wav(wav,48000);
 wav[22]=2;wav[32]=4;put32(wav+24,48000);put32(wav+28,192000);
 const char *h="HTTP/1.1 200 OK\r\n\r\n";send_all(peer,h,strlen(h));send_all(peer,wav,44+48000);assert(!pump(&now,15));
 assert(p.state==6&&p.frames>0&&!p.stream_done&&p.fd>=0);
 for(size_t pos=44+48000;pos<sizeof wav;){size_t n=sizeof wav-pos;if(n>4096)n=4096;send_all(peer,wav+pos,n);pos+=n;assert(!pump(&now,2));}
 assert(shutdown(peer,SHUT_WR)==0);assert(pump(&now,100)==1&&p.frames==24000);close(peer);
 puts("48 kHz stereo starts with 250 ms buffered before EOF: OK");
}
static void longer_than_finite_cap(void){static unsigned char wav[44+2200000];uint64_t now=0;int peer=fetch();make_wav(wav,1100000);
 const char *h="HTTP/1.1 200 OK\r\n\r\n";send_all(peer,h,strlen(h));for(size_t pos=0;pos<sizeof wav;){size_t n=sizeof wav-pos;if(n>4096)n=4096;send_all(peer,wav+pos,n);pos+=n;assert(!pump(&now,5));assert(p.stream_used<=ESP_PLAY_STREAM_BUFFER);}
 assert(p.stream_body_seen>ESP_PLAY_BODY_MAX&&!p.stream_done);assert(shutdown(peer,SHUT_WR)==0);assert(pump(&now,300)==1&&p.frames==2200000);close(peer);
 int fd=open(bus,O_RDONLY);assert(fd>=0);struct stat st;assert(fstat(fd,&st)==0&&st.st_size==8800000);close(fd);
 puts(">2 MiB WAV streams with bounded PCM queue and exact output length: OK");
}
static void backpressure(void){static unsigned char wav[44+100000];char fifo[256];unsigned char sink[32768];uint64_t now=0;size_t output=0;int rc=0;
 make_wav(wav,50000);int n=snprintf(fifo,sizeof fifo,"%s.fifo",bus);assert(n>0&&(size_t)n<sizeof fifo);assert(mkfifo(fifo,0600)==0);int reader=open(fifo,O_RDONLY|O_NONBLOCK);assert(reader>=0);
 int peer=fetch();snprintf(p.bus,sizeof p.bus,"%s",fifo);const char *h="HTTP/1.1 200 OK\r\n\r\n";send_all(peer,h,strlen(h));send_all(peer,wav,44+85000);
 for(unsigned i=0;i<100&&!p.stream_blocked;i++)assert(!pump(&now,1));assert(p.stream_blocked&&p.state==6);
 now=p.stream_idle+1000;assert(!esp_playback_tick(&p,now)&&p.state==6); /* full speaker queue is not an idle network failure */
 send_all(peer,wav+44+85000,sizeof wav-(44+85000));assert(shutdown(peer,SHUT_WR)==0);
 for(unsigned i=0;i<5000&&!rc;i++){ssize_t got;while((got=read(reader,sink,sizeof sink))>0)output+=(size_t)got;rc=esp_playback_tick(&p,now);now+=20;}
 ssize_t got;while((got=read(reader,sink,sizeof sink))>0)output+=(size_t)got;
 assert(rc==1&&output==400000&&p.frames==100000);close(peer);close(reader);assert(unlink(fifo)==0);
 puts("full PCM queue pauses network without idle timeout, then resumes without loss: OK");
}
static void timeouts_and_cancel(void){unsigned char wav[44+16000];make_wav(wav,8000);uint64_t now=0;int peer=fetch();const char *h="HTTP/1.1 200 OK\r\n\r\n";send_all(peer,h,strlen(h));send_all(peer,wav,sizeof wav);assert(!pump(&now,10)&&p.state==6&&!p.stream_done);
 int network=p.fd,speaker=p.bus_fd;esp_playback_close(&p);assert(p.state==0&&p.fd==-1&&p.bus_fd==-1&&fcntl(network,F_GETFD)==-1&&errno==EBADF&&fcntl(speaker,F_GETFD)==-1&&errno==EBADF);close(peer);
 now=0;peer=fetch();send_all(peer,h,strlen(h));send_all(peer,wav,sizeof wav);assert(!pump(&now,10)&&p.state==6);now=p.stream_idle+1;assert(esp_playback_tick(&p,now)==-1&&p.bus_fd==-1);close(peer);
 now=0;peer=fetch();send_all(peer,h,strlen(h));send_all(peer,wav,sizeof wav);assert(!pump(&now,10)&&p.state==6);now=p.deadline+1;assert(esp_playback_tick(&p,now)==-1&&p.bus_fd==-1);close(peer);
 peer=fetch();int connecting=p.fd;p.state=2;p.stream_idle=15000;assert(esp_playback_tick(&p,15001)==-1&&p.fd==-1&&fcntl(connecting,F_GETFD)==-1&&errno==EBADF);close(peer);
 puts("cancellation closes network and speaker; startup, network idle and total deadlines fail: OK");
}
static void finite_and_eof(void){unsigned char wav[44+2400];make_wav(wav,1200);uint64_t now=0;int peer=fetch();char h[100];int n=snprintf(h,sizeof h,"HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n\r\n",sizeof wav);assert(n>0);
 send_all(peer,h,(size_t)n);send_all(peer,wav,sizeof wav-1);assert(!pump(&now,3)&&p.state==5&&p.bus_fd==-1);send_all(peer,wav+sizeof wav-1,1);assert(pump(&now,100)==1);close(peer);
 now=0;peer=fetch();send_all(peer,"HTTP/1.1 200 OK\r\n\r\n",19);send_all(peer,wav,sizeof wav);assert(!pump(&now,3)&&!p.stream_done);assert(shutdown(peer,SHUT_WR)==0);assert(pump(&now,100)==1);close(peer);
 unsigned char trailing[44+2400+10];make_wav(trailing,1200);put32(trailing+4,(uint32_t)sizeof trailing-8);put32(trailing+40,2400);memcpy(trailing+44+2400,"JUNK",4);put32(trailing+44+2404,1);trailing[44+2408]='x';trailing[44+2409]=0;
 now=0;peer=fetch();n=snprintf(h,sizeof h,"HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n\r\n",sizeof trailing);assert(n>0);send_all(peer,h,(size_t)n);send_all(peer,trailing,sizeof trailing);assert(pump(&now,100)==1);close(peer);
 puts("finite Content-Length and EOF PCM WAV complete only on valid framing: OK");
}
static void rejects(void){unsigned char wav[44+2400];make_wav(wav,1200);uint64_t now=0;int peer=fetch();
 const char *bad="HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nz\r\n";send_all(peer,bad,strlen(bad));assert(pump(&now,3)==-1&&p.bus_fd==-1);close(peer);
 now=0;peer=fetch();const char *head="HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";send_all(peer,head,strlen(head));chunk(peer,wav,sizeof wav);assert(!pump(&now,3));assert(shutdown(peer,SHUT_WR)==0);assert(pump(&now,3)==-1);close(peer);
 now=0;peer=fetch();send_all(peer,"HTTP/1.1 200 OK\r\n\r\n",19);send_all(peer,wav,sizeof wav-1);assert(shutdown(peer,SHUT_WR)==0);assert(pump(&now,4)==-1);close(peer);
 now=0;peer=fetch();wav[20]=3;send_all(peer,"HTTP/1.1 200 OK\r\n\r\n",19);send_all(peer,wav,sizeof wav);assert(pump(&now,4)==-1&&p.bus_fd==-1);close(peer);
 static unsigned char long_wav[44+24000];make_wav(long_wav,12000);char finite[100];int n=snprintf(finite,sizeof finite,"HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n\r\n",sizeof long_wav);assert(n>0);
 now=0;peer=fetch();send_all(peer,finite,(size_t)n);send_all(peer,long_wav,44+16000);assert(!pump(&now,10)&&p.state==6&&p.frames>0);assert(shutdown(peer,SHUT_WR)==0);assert(pump(&now,3)==-1&&p.bus_fd==-1);close(peer);
 now=0;peer=fetch();send_all(peer,head,strlen(head));chunk(peer,long_wav,44+16000);assert(!pump(&now,10)&&p.state==6&&p.frames>0);assert(shutdown(peer,SHUT_WR)==0);assert(pump(&now,3)==-1&&p.bus_fd==-1);close(peer);
 puts("malformed chunk/PCM and truncated framing before or after playback rejected: OK");
}
int main(void){const char *tmp=getenv("TMPDIR");assert(tmp&&*tmp);int n=snprintf(bus,sizeof bus,"%s/esphome-stream-XXXXXX",tmp);assert(n>0&&(size_t)n<sizeof bus);int fd=mkstemp(bus);assert(fd>=0&&close(fd)==0);early_chunked();assert(truncate(bus,0)==0);stereo_before_eof();assert(truncate(bus,0)==0);longer_than_finite_cap();assert(truncate(bus,0)==0);backpressure();assert(truncate(bus,0)==0);timeouts_and_cancel();assert(truncate(bus,0)==0);finite_and_eof();rejects();assert(unlink(bus)==0);return 0;}
