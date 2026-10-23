#define _POSIX_C_SOURCE 200809L
#include "../src/audio/sacd_reader.h"
#include "../src/audio/dsd_decoder.h"
#include "../src/library/sacd_iso.h"
#include "../src/audio/dst_decoder.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SECTOR 2048u
#define MASTER 510u
#define AREA 600u
#define AUDIO 700u
#define FRAMES 2u
#define ROWS (FRAMES * DST_STEREO_CHANNEL_BYTES)
#define FRAME_BYTES DST_STEREO_FRAME_BYTES

static void be16(uint8_t *p, unsigned v) { p[0] = v >> 8; p[1] = v; }
static void be32(uint8_t *p, uint32_t v) { p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v; }
static void be64(uint8_t *p, uint64_t v) { for (int i=7;i>=0;--i) { p[i]=v; v>>=8; } }
static uint8_t *sec(uint8_t *iso, unsigned n) { return iso + (size_t)n * SECTOR; }
static size_t raw_at(unsigned row) { return (size_t)(row / DST_STEREO_CHANNEL_BYTES) * (FRAME_BYTES + 1) + 1 + (size_t)(row % DST_STEREO_CHANNEL_BYTES) * 2; }
static bool read_row(sacd_reader_t *reader, uint8_t row[2]) {
    return sacd_reader_bytes(reader, &row[0], &row[1], 1);
}

typedef struct { const uint8_t *data; unsigned len, frame; bool start; } packet_t;
static void packet_push(packet_t *packets, unsigned *count, const uint8_t *data,
                        unsigned len, unsigned frame, bool start) {
    packets[*count]=(packet_t){data,len,frame,start}; ++*count;
}

static void make_iso(uint8_t *iso, uint8_t *raw, bool dst) {
    uint8_t *m=sec(iso,MASTER); memcpy(m,"SACDMTOC",8); m[8]=1; m[9]=20;
    be32(m+64,AREA); be16(m+84,4); m[138]=2;
    uint8_t *a=sec(iso,AREA); memcpy(a,"TWOCHTOC",8); be16(a+10,4);
    a[20]=4; a[21]=dst?0:2; a[32]=2; a[64]=0; a[65]=0; a[66]=FRAMES; a[69]=1;
    unsigned sector_count=dst?11:12;
    be32(a+72,AUDIO); be32(a+76,AUDIO+sector_count-1);
    uint8_t *l1=sec(iso,AREA+1); memcpy(l1,"SACDTRL1",8);
    be32(l1+8,AUDIO); be32(l1+8+255*4,sector_count);
    uint8_t *l2=sec(iso,AREA+2); memcpy(l2,"SACDTRL2",8);
    l2[8+255*4]=0; l2[9+255*4]=0; l2[10+255*4]=FRAMES;

    /* Frame zero ends and frame one begins in one sector. This exercises
       multiple audio packets and a pending frame start in the same sector. */
    packet_t packets[16]; unsigned count=0; size_t stride=dst?FRAME_BYTES+1:FRAME_BYTES;
    for(unsigned fr=0;fr<FRAMES;++fr) {
        size_t left=stride, at=0; size_t first=(fr==0?1800:1600);
        if(dst && fr==0) {
            for(unsigned i=0;i<4;++i) { packet_push(packets,&count,raw+fr*stride+at,1800,fr,i==0); at+=1800; left-=1800; }
            packet_push(packets,&count,raw+fr*stride+at,1800,fr,false); at+=1800; left-=1800;
            packet_push(packets,&count,raw+fr*stride+at,(unsigned)left,fr,false); continue;
        }
        while(left) { unsigned n=(unsigned)(left>first?first:left); packet_push(packets,&count,raw+fr*stride+at,n,fr,at==0); at+=n;left-=n; first=1900; }
    }
    /* For the raw DST stream, split the final 409 bytes of frame zero from
       the first 1600 bytes of frame one into one two-packet sector. */
    unsigned sector=AUDIO, i=0;
    while(i<count) {
        unsigned take=(dst && i==5)?2:1, infos=0; size_t offset=1+2*take;
        for(unsigned j=0;j<take;++j) if(packets[i+j].start) ++infos;
        uint8_t *s=sec(iso,sector++); s[0]=(uint8_t)((take<<5)|(infos<<2)|(dst?1:0));
        offset+=infos*(dst?4:3); size_t payload=offset;
        for(unsigned j=0;j<take;++j) {
            packet_t *q=&packets[i+j]; s[1+2*j]=(uint8_t)(0x10|(q->start?0x80:0)|((q->len>>8)&7)); s[2+2*j]=(uint8_t)q->len;
            if(q->start) { uint8_t *info=s+1+2*take+(infos-1)*(dst?4:3); info[0]=0;info[1]=0;info[2]=(uint8_t)q->frame; if(dst) info[3]=0; --infos; }
            memcpy(s+payload,q->data,q->len); payload+=q->len;
        }
        i+=take;
    }
}

static uint8_t *load_file(const char *path, size_t *size) {
    FILE *f=fopen(path,"rb"); if(!f) return NULL;
    if(fseeko(f,0,SEEK_END)!=0) { fclose(f); return NULL; }
    off_t n=ftello(f); if(n<0 || fseeko(f,0,SEEK_SET)!=0) { fclose(f); return NULL; }
    uint8_t *data=malloc((size_t)n); if(!data || fread(data,1,(size_t)n,f)!=(size_t)n) { free(data); fclose(f); return NULL; }
    fclose(f); *size=(size_t)n; return data;
}

static void run_cached_dst(const char *cache) {
    enum { N=10 }; size_t frame_size[N]; uint8_t *frames[N]; uint8_t *reference;
    char path[1024]; size_t ref_size=0;
    for(unsigned i=0;i<N;++i) { snprintf(path,sizeof(path),"%s/frame-%u.dst",cache,i); frames[i]=load_file(path,&frame_size[i]); assert(frames[i] && frame_size[i]>1 && frame_size[i]<=65536); }
    snprintf(path,sizeof(path),"%s/our.raw",cache); reference=load_file(path,&ref_size); assert(reference && ref_size==(size_t)N*FRAME_BYTES);
    unsigned packets=0;
    for(unsigned i=0;i<N;++i) packets+=(unsigned)((frame_size[i]+1799)/1800);
    unsigned sectors=AUDIO+packets; size_t iso_size=(size_t)sectors*SECTOR; uint8_t *iso=calloc(1,iso_size); assert(iso);
    uint8_t *m=sec(iso,MASTER); memcpy(m,"SACDMTOC",8);m[8]=1;m[9]=20;be32(m+64,AREA);be16(m+84,4);m[138]=2;
    uint8_t *a=sec(iso,AREA);memcpy(a,"TWOCHTOC",8);be16(a+10,4);a[20]=4;a[21]=0;a[32]=2;a[66]=N;a[69]=1;be32(a+72,AUDIO);be32(a+76,sectors-1);
    uint8_t *l1=sec(iso,AREA+1);memcpy(l1,"SACDTRL1",8);be32(l1+8,AUDIO);be32(l1+8+255*4,packets);
    uint8_t *l2=sec(iso,AREA+2);memcpy(l2,"SACDTRL2",8);l2[8+255*4]=0;l2[9+255*4]=0;l2[10+255*4]=N;
    unsigned sector=AUDIO;
    for(unsigned fr=0;fr<N;++fr) {
        size_t at=0;
        while(at<frame_size[fr]) {
            unsigned n=(unsigned)(frame_size[fr]-at>1800?1800:frame_size[fr]-at);bool start=at==0;
            uint8_t *s=sec(iso,sector++);s[0]=(uint8_t)((1<<5)|(start?1<<2:0)|1);
            s[1]=(uint8_t)(0x10|(start?0x80:0)|((n>>8)&7));s[2]=(uint8_t)n;
            if(start) {s[3]=0;s[4]=0;s[5]=(uint8_t)fr;s[6]=0;}
            memcpy(s+(start?7:3),frames[fr]+at,n);at+=n;
        }
    }
    snprintf(path,sizeof(path),"build_host/sacd-cached-compressed.iso");FILE *f=fopen(path,"wb");assert(f);assert(fwrite(iso,1,iso_size,f)==iso_size);fclose(f);free(iso);
    f=fopen(path,"rb");assert(f);uint64_t samples=0;sacd_reader_t *r=sacd_reader_open(f,&samples);assert(r&&samples==(uint64_t)N*DST_STEREO_CHANNEL_BYTES*8);
    uint8_t row[2];
    for(unsigned i=0;i<N*DST_STEREO_CHANNEL_BYTES;++i) {assert(read_row(r,row));assert(row[0]==reference[2*i]&&row[1]==reference[2*i+1]);}
    assert(!read_row(r,row));
    const uint64_t total=(uint64_t)N*DST_STEREO_CHANNEL_BYTES;uint32_t rng=0x51ac0deu;
    for(unsigned i=0;i<2048;++i) {
        uint64_t target;
        if(i<4) target=(uint64_t[]){0,DST_STEREO_CHANNEL_BYTES-1,DST_STEREO_CHANNEL_BYTES,total-1}[i];
        else {rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;target=rng%total;}
        assert(sacd_reader_seek(r,target));assert(read_row(r,row));assert(row[0]==reference[2*target]&&row[1]==reference[2*target+1]);
    }
    sacd_reader_close(r);fclose(f);unlink(path);free(reference);for(unsigned i=0;i<N;++i)free(frames[i]);
    puts("cached compressed DST ISO parity and 2048 seeks passed");
}

int main(int argc, char **argv) {
    /* DFF construction below uses a byte buffer so nested chunk sizes are exact. */
    char iso_path[]="build_host/sacd-reader-XXXXXX", raw_path[]="build_host/sacd-raw-XXXXXX", dff_path[]="build_host/sacd-dff-XXXXXX";
    int ifd=mkstemp(iso_path), rfd=mkstemp(raw_path), dfd=mkstemp(dff_path); assert(ifd>=0 && rfd>=0 && dfd>=0); close(ifd); close(rfd); close(dfd);
    uint8_t *raw=malloc(FRAMES*(FRAME_BYTES+1)); assert(raw);
    for(size_t i=0;i<FRAMES*(FRAME_BYTES+1);++i) raw[i]=(uint8_t)(i*37u+11u);
    for(unsigned f=0;f<FRAMES;++f) raw[f*(FRAME_BYTES+1)]=0;
    uint8_t *dff_data=malloc(FRAMES*FRAME_BYTES); assert(dff_data);
    for(unsigned i=0;i<FRAMES;++i) memcpy(dff_data+(size_t)i*FRAME_BYTES,raw+(size_t)i*(FRAME_BYTES+1)+1,FRAME_BYTES);
    size_t iso_size=(AUDIO+12)*SECTOR; uint8_t *iso=calloc(1,iso_size); assert(iso); make_iso(iso,raw,true);
    FILE *f=fopen(iso_path,"wb"); assert(f); assert(fwrite(iso,1,iso_size,f)==iso_size); fclose(f); free(iso);
    iso=calloc(1,iso_size); assert(iso); make_iso(iso,dff_data,false);
    f=fopen(raw_path,"wb"); assert(f); assert(fwrite(iso,1,iso_size,f)==iso_size); fclose(f); free(iso);
    /* Minimal DFF: one PROP/SND chunk with stream properties, then raw DSD chunk. */
    uint8_t *prop=calloc(1,4+12+4+12+4+12+2+12+4); assert(prop);
    size_t p=0; memcpy(prop+p,"SND ",4); p+=4;
    memcpy(prop+p,"FS  ",4); be64(prop+p+4,4); prop[p+12]=0; prop[p+13]=0x2b; prop[p+14]=0x11; prop[p+15]=0; p+=16;
    memcpy(prop+p,"CHNL",4); be64(prop+p+4,2); prop[p+12]=0; prop[p+13]=2; p+=14;
    memcpy(prop+p,"CMPR",4); be64(prop+p+4,4); memcpy(prop+p+12,"DSD ",4); p+=16;
    uint64_t dsd_size=FRAMES*FRAME_BYTES;
    uint64_t form_size=4+12+4+p+12+dsd_size;
    f=fopen(dff_path,"wb"); assert(f); uint8_t h[12]; memcpy(h,"FRM8",4); be64(h+4,form_size); fwrite(h,1,12,f); fwrite("DSD ",1,4,f);
    memcpy(h,"PROP",4); be64(h+4,p); fwrite(h,1,12,f); fwrite(prop,1,p,f);
    memcpy(h,"DSD ",4); be64(h+4,dsd_size); fwrite(h,1,12,f); fwrite(dff_data,1,dsd_size,f); fclose(f); free(prop); free(dff_data);

    f=fopen(iso_path,"rb"); assert(f); uint64_t samples=0; sacd_reader_t *r=sacd_reader_open(f,&samples); assert(r && samples==(uint64_t)ROWS*8);
    uint8_t batch_left[16],batch_right[16];assert(sacd_reader_bytes(r,batch_left,batch_right,16));
    for(unsigned i=0;i<16;++i) { size_t at=raw_at(i);assert(batch_left[i]==raw[at]&&batch_right[i]==raw[at+1]); }
    assert(sacd_reader_seek(r,0));
    for(unsigned i=0;i<ROWS;++i) { uint8_t row[2]; size_t at=raw_at(i); assert(read_row(r,row)); assert(row[0]==raw[at] && row[1]==raw[at+1]); }
    uint8_t row[2]; assert(!read_row(r,row));
    const unsigned seeks[]={0,1,DST_STEREO_CHANNEL_BYTES-1,DST_STEREO_CHANNEL_BYTES,DST_STEREO_CHANNEL_BYTES+17,ROWS-1};
    for(size_t k=0;k<sizeof(seeks)/sizeof(seeks[0]);++k) { size_t at=raw_at(seeks[k]); if(!sacd_reader_seek(r,seeks[k])) { fprintf(stderr,"seek failed at %u\\n",seeks[k]); abort(); } assert(read_row(r,row)); assert(row[0]==raw[at] && row[1]==raw[at+1]); }
    assert(sacd_reader_seek(r,ROWS)); assert(!read_row(r,row)); sacd_reader_close(r); fclose(f);

    f=fopen(raw_path,"rb"); assert(f); r=sacd_reader_open(f,&samples); assert(r && samples==(uint64_t)ROWS*8);
    for(unsigned i=0;i<ROWS;++i) { size_t at=raw_at(i); assert(read_row(r,row)); assert(row[0]==raw[at] && row[1]==raw[at+1]); }
    sacd_reader_close(r); fclose(f);

    dsd_decoder_t *a=dsd_open_file(iso_path), *b=dsd_open_file(dff_path); assert(a && b);
    uint64_t pcm=dsd_get_total_pcm_frame_count(a); assert(pcm==dsd_get_total_pcm_frame_count(b));
    int16_t *x=malloc((size_t)pcm*2*sizeof(*x)), *y=malloc((size_t)pcm*2*sizeof(*y)); assert(x&&y);
    assert(dsd_read_pcm_frames_s16(a,pcm,x).frames==pcm); assert(dsd_read_pcm_frames_s16(b,pcm,y).frames==pcm); assert(!memcmp(x,y,(size_t)pcm*4));
    assert(dsd_seek_to_pcm_frame(a,0)&&dsd_seek_to_pcm_frame(b,0)); int32_t *u=malloc((size_t)pcm*2*sizeof(*u)), *v=malloc((size_t)pcm*2*sizeof(*v)); assert(u&&v);
    assert(dsd_read_pcm_frames_s32(a,pcm,u).frames==pcm); assert(dsd_read_pcm_frames_s32(b,pcm,v).frames==pcm); assert(!memcmp(u,v,(size_t)pcm*8));
    uint64_t ignored; assert(dsd_set_dop(a,true,0,&ignored)&&dsd_set_dop(b,true,0,&ignored)); assert(ignored==0);
    uint32_t dop[128], dop2[128]; assert(dsd_read_pcm_frames_s32(a,64,(int32_t*)dop).frames==64); assert(dsd_read_pcm_frames_s32(b,64,(int32_t*)dop2).frames==64); assert(!memcmp(dop,dop2,sizeof(dop)));
    dsd_close(a); dsd_close(b); free(x);free(y);free(u);free(v);

    /* A TOC that declares one extra frame over two complete on-disc frames
       must shorten normally when the packet stream ends cleanly. */
    f=fopen(iso_path,"r+b");assert(f);assert(fseeko(f,(off_t)AREA*SECTOR+66,SEEK_SET)==0);fputc(3,f);
    assert(fseeko(f,(off_t)(AREA+2)*SECTOR+8+255*4+2,SEEK_SET)==0);fputc(3,f);fclose(f);
    a=dsd_open_file(iso_path);assert(a);uint64_t declared=dsd_get_total_pcm_frame_count(a);
    int16_t *short_read=calloc((size_t)(declared+1)*2,sizeof(*short_read));assert(short_read);
    decoder_read_result_t result=dsd_read_pcm_frames_s16(a,declared,short_read);assert(result.frames>0&&result.frames<declared);
    result=dsd_read_pcm_frames_s16(a,1,short_read);assert(result.frames==0&&result.status==DECODER_READ_EOF);
    dsd_close(a);free(short_read);

    /* Corruption in frame two must be fatal, and a later warmup seek must
       not hide the decoder error. The start packet is after the 409-byte
       tail packet in the shared sector. */
    f=fopen(iso_path,"r+b");assert(f);assert(fseeko(f,(off_t)(AUDIO+5)*SECTOR+9+409,SEEK_SET)==0);fputc(1,f);fclose(f);
    a=dsd_open_file(iso_path);assert(a);declared=dsd_get_total_pcm_frame_count(a);
    short_read=calloc((size_t)declared*2,sizeof(*short_read));assert(short_read);
    result=dsd_read_pcm_frames_s16(a,declared,short_read);assert(result.frames>0&&result.frames<declared);
    result=dsd_read_pcm_frames_s16(a,1,short_read);assert(result.frames==0&&result.status==DECODER_READ_FATAL_ERROR);
    assert(!dsd_seek_to_pcm_frame(a,declared-1));dsd_close(a);free(short_read);

    /* A malformed raw-DST frame marker and a packet claiming bytes beyond
       its sector are both rejected at open. */
    f=fopen(iso_path,"r+b"); assert(f); assert(fseeko(f,(off_t)AUDIO*SECTOR+7,SEEK_SET)==0);
    fputc(1,f); fclose(f);
    f=fopen(iso_path,"rb"); assert(f); assert(sacd_reader_open(f,&samples)==NULL); fclose(f);
    f=fopen(iso_path,"r+b"); assert(f); assert(fseeko(f,(off_t)AUDIO*SECTOR+7,SEEK_SET)==0); fputc(0,f);
    assert(fseeko(f,(off_t)AUDIO*SECTOR+1,SEEK_SET)==0);
    uint8_t bad[2]={7,0xff}; assert(fwrite(bad,1,2,f)==2); fclose(f);
    f=fopen(iso_path,"rb"); assert(f); assert(sacd_reader_open(f,&samples)==NULL); fclose(f);
    /* The TOC remains readable only when all declared audio sectors exist. */
    assert(truncate(raw_path,(off_t)AUDIO*SECTOR)==0);
    f=fopen(raw_path,"rb"); assert(f); assert(sacd_reader_open(f,&samples)==NULL); fclose(f);
    free(raw); unlink(iso_path);unlink(raw_path);unlink(dff_path);
    if(argc==3 && strcmp(argv[1],"--cached-dst")==0) run_cached_dst(argv[2]);
    puts("sacd_reader_spec passed"); return 0;
}
