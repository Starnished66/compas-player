#include "audio.h"

#include <arpa/inet.h>
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

size_t audio_test_net_mp3_sync_bytes(void);

typedef struct { int listener; const unsigned char *bytes; size_t length; const char *content_type; bool ignore_ranges; int sndbuf; atomic_uint ranges; atomic_uint body_bytes; atomic_bool saw_if_range; atomic_uint active; } server_t;

void install_thread_crash_altstack(void) {}

static size_t send_all(int fd, const void *data, size_t length) {
    const unsigned char *p=data; size_t sent=0;
    /* A seek closes the old connection while its sender may still be writing. */
    while(length){ssize_t n=send(fd,p,length,MSG_NOSIGNAL);if(n<=0)return sent;p+=n;length-=(size_t)n;sent+=(size_t)n;}
    return sent;
}

typedef struct { server_t *server; int fd; } connection_t;

/* One thread per connection: a seek opens and validates its replacement
 * response before the player closes the old one, so a server that finishes
 * each response before accepting the next would wait on the old sender. */
static void *connection_thread(void *opaque) {
    connection_t *c=opaque; server_t *s=c->server; int fd=c->fd; free(c);
    if(s->sndbuf>0){int snd=s->sndbuf;setsockopt(fd,SOL_SOCKET,SO_SNDBUF,&snd,sizeof(snd));}
    {
        char request[4096]={0}; size_t used=0;
        while(used<sizeof(request)-1&&!strstr(request,"\r\n\r\n")){
            ssize_t n=recv(fd,request+used,sizeof(request)-used-1,0);if(n<=0)break;used+=(size_t)n;request[used]=0;
        }
        const char *range=strstr(request,"Range: bytes="); unsigned long long start=0;
        if(range){sscanf(range,"Range: bytes=%llu-",&start);atomic_fetch_add(&s->ranges,1);}
        if (range && start > 0 && strstr(request,"If-Range: \"v1\"\r\n")) atomic_store(&s->saw_if_range,true);
        if (s->ignore_ranges && range) {
            char header[256];
            int n=snprintf(header,sizeof(header),"HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",s->content_type,s->length);
            send_all(fd,header,(size_t)n);atomic_fetch_add(&s->body_bytes,(unsigned)send_all(fd,s->bytes,s->length));
        } else if(!range || start>=s->length){
            const char *bad="HTTP/1.1 416 Range Not Satisfiable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            send_all(fd,bad,strlen(bad));
        } else {
            char header[256];
            int n=snprintf(header,sizeof(header),"HTTP/1.1 206 Partial Content\r\nContent-Type: %s\r\nContent-Range: bytes %llu-%zu/%zu\r\nContent-Length: %zu\r\nETag: \"v1\"\r\nConnection: close\r\n\r\n",s->content_type,start,s->length-1,s->length,s->length-(size_t)start);
            send_all(fd,header,(size_t)n);atomic_fetch_add(&s->body_bytes,(unsigned)send_all(fd,s->bytes+(size_t)start,s->length-(size_t)start));
        }
        close(fd);
    }
    atomic_fetch_sub(&s->active,1);
    return NULL;
}

static void *server_thread(void *opaque) {
    server_t *s=opaque;
    for (;;) {
        int fd=accept(s->listener,NULL,NULL); if(fd<0)break;
        connection_t *c=malloc(sizeof(*c)); assert(c); c->server=s; c->fd=fd;
        atomic_fetch_add(&s->active,1);
        pthread_t id; assert(pthread_create(&id,NULL,connection_thread,c)==0); pthread_detach(id);
    }
    return NULL;
}

/* Senders still blocked on a closed client fail out quickly; wait for them
 * before the fixture bytes they read from are freed. */
static void wait_connections_done(server_t *s) {
    struct timespec pause={.tv_sec=0,.tv_nsec=10000000};
    for(unsigned i=0;i<1000&&atomic_load(&s->active);i++)nanosleep(&pause,NULL);
    assert(atomic_load(&s->active)==0);
}

static bool wait_format(audio_current_format_info_t *out) {
    struct timespec pause={.tv_sec=0,.tv_nsec=10000000};
    for(unsigned i=0;i<1000;i++){
        if(audio_get_current_format_info(out))return true;
        nanosleep(&pause,NULL);
    }
    return false;
}

static unsigned char *read_file(const char *path,size_t *length) {
    FILE *f=fopen(path,"rb");assert(f);assert(fseek(f,0,SEEK_END)==0);long n=ftell(f);assert(n>0);rewind(f);
    unsigned char *bytes=malloc((size_t)n);assert(bytes);assert(fread(bytes,1,(size_t)n,f)==(size_t)n);fclose(f);*length=(size_t)n;return bytes;
}

static void play_and_seek(server_t *server, const char *path, const char *content_type,
                          const char *extension, audio_codec_t expected_codec) {
    size_t length; unsigned char *bytes=read_file(path,&length);
    atomic_store(&server->ranges,0);atomic_store(&server->saw_if_range,false);
    server->bytes=bytes;server->length=length;server->content_type=content_type;
    struct sockaddr_in addr={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    server->listener=socket(AF_INET,SOCK_STREAM,0);assert(server->listener>=0);
    int one=1;setsockopt(server->listener,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
    assert(bind(server->listener,(struct sockaddr*)&addr,sizeof(addr))==0);assert(listen(server->listener,16)==0);
    socklen_t addr_size=sizeof(addr);assert(getsockname(server->listener,(struct sockaddr*)&addr,&addr_size)==0);
    pthread_t server_id;assert(pthread_create(&server_id,NULL,server_thread,server)==0);
    char url[256];snprintf(url,sizeof(url),"http://127.0.0.1:%u/finite%s#%s",ntohs(addr.sin_port),extension,extension);
    audio_play_file_at(url,2.0,false,0.0,false,0.0);
    audio_current_format_info_t info;assert(wait_format(&info));
    assert(info.is_stream&&info.seekable&&info.duration_seconds>6.0&&info.codec==expected_codec);
    assert(audio_get_position_seconds() >= 1.8); /* remote resume seek during open */
    /* The fixtures are 120 s of noise, far larger than the stream's buffer, so a
     * seek this far must reach the server: a ranged, If-Range request past
     * byte 0. MP3 proves its seek table here, since decoding forward to the
     * target would read on through the open connection instead. */
    audio_seek(100.0);
    struct timespec pause={.tv_sec=0,.tv_nsec=20000000};bool reached=false;
    for(unsigned i=0;i<250;i++){if(audio_get_position_seconds()>99.8){reached=true;break;}nanosleep(&pause,NULL);}
    assert(reached);
    assert(atomic_load(&server->ranges)>=2); /* capability probe and the far seek */
    assert(atomic_load(&server->saw_if_range));
    audio_stop();
    for(unsigned i=0;i<500&&!audio_is_idle();i++)nanosleep(&pause,NULL);
    assert(audio_is_idle());
    shutdown(server->listener,SHUT_RDWR);close(server->listener);pthread_join(server_id,NULL);wait_connections_done(server);
    free(bytes);
}

static void verify_finite_ignored_range_is_not_seekable(server_t *server,const char *path) {
    size_t length;unsigned char *bytes=read_file(path,&length);
    atomic_store(&server->ranges,0);atomic_store(&server->saw_if_range,false);
    server->bytes=bytes;server->length=length;server->content_type="audio/flac";server->ignore_ranges=true;
    struct sockaddr_in addr={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    server->listener=socket(AF_INET,SOCK_STREAM,0);assert(server->listener>=0);int one=1;
    setsockopt(server->listener,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
    assert(bind(server->listener,(struct sockaddr*)&addr,sizeof(addr))==0);assert(listen(server->listener,4)==0);
    socklen_t z=sizeof(addr);assert(getsockname(server->listener,(struct sockaddr*)&addr,&z)==0);
    pthread_t server_id;assert(pthread_create(&server_id,NULL,server_thread,server)==0);
    char url[256];snprintf(url,sizeof(url),"http://127.0.0.1:%u/finite.flac#.flac",ntohs(addr.sin_port));
    audio_play_file_at(url,0.0,false,0.0,false,0.0);audio_current_format_info_t info;assert(wait_format(&info));
    assert(info.is_stream&&!info.seekable&&info.duration_seconds>6.0);
    audio_seek(4.0);struct timespec pause={.tv_sec=0,.tv_nsec=20000000};nanosleep(&pause,NULL);
    assert(atomic_load(&server->ranges)==1);
    audio_stop();for(unsigned i=0;i<500&&!audio_is_idle();i++)nanosleep(&pause,NULL);assert(audio_is_idle());
    shutdown(server->listener,SHUT_RDWR);close(server->listener);pthread_join(server_id,NULL);wait_connections_done(server);free(bytes);server->ignore_ranges=false;
}

static bool moov_precedes_mdat(const unsigned char *bytes, size_t length) {
    size_t off=0; int moov_at=-1, mdat_at=-1, seen=0;
    while(off+8<=length && seen<16){
        uint32_t size32=((uint32_t)bytes[off]<<24)|((uint32_t)bytes[off+1]<<16)|((uint32_t)bytes[off+2]<<8)|bytes[off+3];
        uint64_t box=size32; size_t header=8;
        if(size32==1){
            if(off+16>length) return false;
            box=0; for(int i=0;i<8;i++) box=(box<<8)|bytes[off+8+i];
            header=16;
        } else if(size32==0) box=length-off;
        if(box<header || box>length-off) return false;
        if(memcmp(bytes+off+4,"moov",4)==0) moov_at=seen;
        if(memcmp(bytes+off+4,"mdat",4)==0) mdat_at=seen;
        off+=(size_t)box; seen++;
    }
    return moov_at>=0 && mdat_at>=0 && moov_at<mdat_at;
}

static void start_listener(server_t *server, pthread_t *server_id, unsigned short *port) {
    struct sockaddr_in addr={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    server->listener=socket(AF_INET,SOCK_STREAM,0);assert(server->listener>=0);
    int one=1;setsockopt(server->listener,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
    assert(bind(server->listener,(struct sockaddr*)&addr,sizeof(addr))==0);
    assert(listen(server->listener,16)==0);
    socklen_t addr_size=sizeof(addr);
    assert(getsockname(server->listener,(struct sockaddr*)&addr,&addr_size)==0);
    *port=ntohs(addr.sin_port);
    assert(pthread_create(server_id,NULL,server_thread,server)==0);
}

static void stop_listener(server_t *server, pthread_t server_id) {
    shutdown(server->listener,SHUT_RDWR);close(server->listener);pthread_join(server_id,NULL);wait_connections_done(server);
}

static void stop_playback(void) {
    audio_stop();
    struct timespec pause={.tv_sec=0,.tv_nsec=20000000};
    for(unsigned i=0;i<500&&!audio_is_idle();i++) nanosleep(&pause,NULL);
    assert(audio_is_idle());
}

/* Open only: start_seconds 0 so a resume seek is not counted. */
static unsigned count_m4a_open_ranges(server_t *server, const unsigned char *bytes, size_t length, const char *name) {
    atomic_store(&server->ranges,0);atomic_store(&server->saw_if_range,false);atomic_store(&server->body_bytes,0);
    server->bytes=bytes;server->length=length;server->content_type="audio/mp4";server->ignore_ranges=false;server->sndbuf=0;
    pthread_t server_id; unsigned short port; start_listener(server,&server_id,&port);
    char url[256]; snprintf(url,sizeof(url),"http://127.0.0.1:%u/%s",port,name);
    audio_play_file_at(url,0.0,false,0.0,false,0.0);
    audio_current_format_info_t info; assert(wait_format(&info));
    unsigned ranges=atomic_load(&server->ranges);
    assert(info.is_stream&&info.seekable&&info.codec==AUDIO_CODEC_AAC&&info.duration_seconds>6.0);
    stop_playback(); stop_listener(server,server_id);
    return ranges;
}

static void verify_flac_extension_without_fragment(server_t *server, const char *path) {
    size_t length; unsigned char *bytes=read_file(path,&length);
    atomic_store(&server->ranges,0);atomic_store(&server->saw_if_range,false);
    server->bytes=bytes;server->length=length;server->content_type="audio/flac";server->ignore_ranges=false;server->sndbuf=0;
    pthread_t server_id; unsigned short port; start_listener(server,&server_id,&port);
    char url[256]; snprintf(url,sizeof(url),"http://127.0.0.1:%u/remote.flac",port);
    audio_play_file_at(url,0.0,false,0.0,false,0.0);
    audio_current_format_info_t info; assert(wait_format(&info));
    assert(info.is_stream&&info.seekable&&info.codec==AUDIO_CODEC_FLAC&&info.duration_seconds>6.0);
    stop_playback(); stop_listener(server,server_id); free(bytes);
}

/* Zeros are not an MP3 frame. The open must fail after a bounded read instead
 * of pulling this whole body. 256 KiB matches NET_MP3_SYNC_SCAN_BYTES. */
static void verify_mislabeled_mpeg_fails_fast(server_t *server) {
    size_t length=2u*1024u*1024u;
    unsigned char *bytes=calloc(1,length); assert(bytes);
    atomic_store(&server->ranges,0);atomic_store(&server->body_bytes,0);atomic_store(&server->saw_if_range,false);
    server->bytes=bytes;server->length=length;server->content_type="audio/mpeg";server->ignore_ranges=false;server->sndbuf=16*1024;
    pthread_t server_id; unsigned short port; start_listener(server,&server_id,&port);
    char url[256]; snprintf(url,sizeof(url),"http://127.0.0.1:%u/clip.mp3",port);
    audio_play_file_at(url,0.0,false,0.0,false,0.0);
    uint64_t generation=audio_get_playback_generation();
    audio_error_t err=AUDIO_ERROR_NONE; uint64_t err_gen=0; bool opened=false;
    struct timespec pause={.tv_sec=0,.tv_nsec=10000000};
    for(unsigned i=0;i<1000;i++){
        audio_current_format_info_t info;
        if(audio_get_current_format_info(&info)){opened=true;break;}
        err=audio_consume_error_ex(&err_gen);
        if(err!=AUDIO_ERROR_NONE && err_gen==generation) break;
        nanosleep(&pause,NULL);
    }
    assert(!opened);
    assert(err==AUDIO_ERROR_DECODER_FAILED);
    stop_playback(); stop_listener(server,server_id);
    size_t scanned=audio_test_net_mp3_sync_bytes();
    unsigned sent=atomic_load(&server->body_bytes);
    printf("mislabelled audio/mpeg sync bytes %zu, body bytes sent %u of %zu\n",scanned,sent,length);
    assert(scanned>0 && scanned<=256u*1024u);
    assert(sent<length);
    free(bytes); server->sndbuf=0;
}

int main(int argc,char **argv) {
    assert(argc==5);
    audio_init();
    server_t server={0};
    play_and_seek(&server,argv[1],"audio/flac",".flac",AUDIO_CODEC_FLAC);
    play_and_seek(&server,argv[2],"audio/mpeg",".mp3",AUDIO_CODEC_MP3);
    /* Declared AAC hint plus generic provider MIME exercises the ftyp sniff. */
    play_and_seek(&server,argv[3],"application/octet-stream",".aac",AUDIO_CODEC_AAC);
    verify_finite_ignored_range_is_not_seekable(&server,argv[1]);
    verify_flac_extension_without_fragment(&server,argv[1]);
    size_t fast_len=0, late_len=0;
    unsigned char *fast=read_file(argv[3],&fast_len), *late=read_file(argv[4],&late_len);
    assert(moov_precedes_mdat(fast,fast_len));
    assert(!moov_precedes_mdat(late,late_len));
    unsigned fast_ranges=count_m4a_open_ranges(&server,fast,fast_len,"faststart.m4a");
    unsigned late_ranges=count_m4a_open_ranges(&server,late,late_len,"late-moov.m4a");
    printf("faststart m4a open range requests: %u\n",fast_ranges);
    printf("moov-at-end m4a open range requests: %u\n",late_ranges);
    assert(fast_ranges>=1 && fast_ranges<=2);
    assert(late_ranges>=1 && late_ranges<=3);
    free(fast); free(late);
    verify_mislabeled_mpeg_fails_fast(&server);
    puts("audio HTTP range decoder seeks passed");return 0;
}
