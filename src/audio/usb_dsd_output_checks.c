/* Run with scripts/run_usb_dsd_output_spec.sh. */
#include "usb_dsd_output.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tinyalsa/pcm.h>

struct pcm_params { struct pcm_mask mask; };
struct pcm { enum pcm_format format; };
static struct pcm_params params;
static struct pcm fake_pcm;
static enum pcm_format only_format;
static unsigned int period_limit = UINT_MAX;
static unsigned int attempted_periods[32], open_attempts;
static unsigned long attempted_thresholds[32];
static uint8_t recorded_data[65536];
static size_t recorded_size;
static unsigned int writes;
static int scripted[32];
static unsigned int scripted_count, scripted_pos;

struct pcm_params *pcm_params_get(unsigned int c, unsigned int d, unsigned int f) { (void)c;(void)d;(void)f; return &params; }
void pcm_params_free(struct pcm_params *p) { (void)p; }
const struct pcm_mask *pcm_params_get_mask(const struct pcm_params *p, enum pcm_param n) { (void)n; return &p->mask; }
unsigned int pcm_params_get_min(const struct pcm_params *p, enum pcm_param n) { (void)p;(void)n; return 1; }
unsigned int pcm_params_get_max(const struct pcm_params *p, enum pcm_param n) { (void)p;(void)n; return 5000000; }
int pcm_params_format_test(struct pcm_params *p, enum pcm_format f) {
    unsigned int i = f == PCM_FORMAT_S24_LE ? 6 : f == PCM_FORMAT_S24_3LE ? 32 :
        f == PCM_FORMAT_S32_LE ? 10 : f == PCM_FORMAT_DSD_U8 ? 48 :
        f == PCM_FORMAT_DSD_U16_LE ? 49 : f == PCM_FORMAT_DSD_U32_LE ? 50 :
        f == PCM_FORMAT_DSD_U16_BE ? 51 : f == PCM_FORMAT_DSD_U32_BE ? 52 : 256;
    return i < 256 && (p->mask.bits[i/32] & (1u << (i%32)));
}
struct pcm *pcm_open(unsigned int c, unsigned int d, unsigned int f, const struct pcm_config *cfg) {
    (void)c;(void)d;(void)f;
    if (cfg->format != only_format) return NULL;
    if (open_attempts < 32) {
        attempted_periods[open_attempts] = cfg->period_size;
        attempted_thresholds[open_attempts] = cfg->start_threshold;
    }
    open_attempts++;
    if (cfg->period_size > period_limit) return NULL;
    fake_pcm.format=cfg->format; return &fake_pcm;
}
int pcm_close(struct pcm *p) { (void)p; return 0; }
int pcm_is_ready(const struct pcm *p) { return p != NULL; }
int pcm_writei(struct pcm *p, const void *data, unsigned int frames) {
    (void)p; writes++;
    int n = scripted_pos < scripted_count ? scripted[scripted_pos++] : (int)frames;
    if (n > (int)frames) n=(int)frames;
    unsigned int bpf = fake_pcm.format == PCM_FORMAT_S24_3LE ? 3 :
        fake_pcm.format == PCM_FORMAT_S24_LE || fake_pcm.format == PCM_FORMAT_S32_LE ? 4 :
        fake_pcm.format == PCM_FORMAT_DSD_U8 ? 1 :
        fake_pcm.format == PCM_FORMAT_DSD_U16_LE || fake_pcm.format == PCM_FORMAT_DSD_U16_BE ? 2 : 4;
    size_t bytes=(size_t)n*bpf;
    if (bytes > sizeof(recorded_data) - recorded_size) return -1;
    memcpy(recorded_data + recorded_size, data, bytes);
    recorded_size += bytes;
    return n;
}
static void reset_writes(void) { writes=0; recorded_size=0; scripted_count=scripted_pos=0; }
static void script(const int *values, unsigned int count) {
    memcpy(scripted, values, count * sizeof(*values)); scripted_count=count; scripted_pos=0;
}

static void allow(enum pcm_format f) {
    memset(&params,0,sizeof(params)); only_format=f;
    unsigned int i = f == PCM_FORMAT_S24_LE ? 6 : f == PCM_FORMAT_S24_3LE ? 32 :
        f == PCM_FORMAT_S32_LE ? 10 : f == PCM_FORMAT_DSD_U8 ? 48 :
        f == PCM_FORMAT_DSD_U16_LE ? 49 : f == PCM_FORMAT_DSD_U32_LE ? 50 :
        f == PCM_FORMAT_DSD_U16_BE ? 51 : 52;
    params.mask.bits[i/32] |= 1u << (i%32);
}

static void period_sizing_and_fallback(void) {
    allow(PCM_FORMAT_DSD_U16_LE);
    period_limit=2048; open_attempts=0;
    usb_dsd_output_t *o=usb_dsd_output_open(3,0,true,2,176400); assert(o);
    assert(open_attempts==2 && attempted_periods[0]==4096 && attempted_periods[1]==2048);
    assert(attempted_thresholds[0]==16384 && attempted_thresholds[1]==8192);
    usb_dsd_output_close(o);

    allow(PCM_FORMAT_S24_3LE);
    period_limit=UINT_MAX; open_attempts=0;
    o=usb_dsd_output_open(3,0,false,2,8000); assert(o);
    assert(open_attempts==1 && attempted_periods[0]==1024 && attempted_thresholds[0]==4096);
    usb_dsd_output_close(o);
    period_limit=UINT_MAX;
}

static void native_formats(void) {
    const enum pcm_format fs[] = { PCM_FORMAT_DSD_U8, PCM_FORMAT_DSD_U16_LE,
        PCM_FORMAT_DSD_U16_BE, PCM_FORMAT_DSD_U32_LE, PCM_FORMAT_DSD_U32_BE };
    const uint8_t expected[][8] = {
        { 0x11,0x22,0x33,0x44 }, { 0x22,0x11,0x44,0x33 },
        { 0x11,0x22,0x33,0x44 }, { 0x44,0x33,0x22,0x11 },
        { 0x11,0x22,0x33,0x44 }
    };
    const size_t sizes[] = { 4,4,4,4,4 };
    int32_t words[] = { 0x051122, 0xfa3344 };
    for (size_t i=0;i<5;i++) {
        allow(fs[i]); usb_dsd_output_t *o=usb_dsd_output_open(3,0,true,1,176400); assert(o);
        reset_writes();
        assert(usb_dsd_output_hardware_rate(o) == (i == 0 ? 352800 : i < 3 ? 176400 : 88200));
        assert(usb_dsd_output_hardware_bits(o) == (i == 0 ? 8 : i < 3 ? 16 : 32));
        uint64_t wrote=0; assert(usb_dsd_output_write(o,words,2,&wrote) && wrote==2);
        assert(recorded_size==sizes[i]);
        if (memcmp(recorded_data,expected[i],recorded_size)) { fprintf(stderr,"native format %d mismatch\n",(int)fs[i]); abort(); }
        usb_dsd_output_close(o);
    }
}

static void dop_formats(void) {
    enum pcm_format fs[] = { PCM_FORMAT_S24_LE, PCM_FORMAT_S24_3LE, PCM_FORMAT_S32_LE };
    int32_t words[] = { 0x00051122, (int32_t)0x00fa3344 };
    const uint8_t packed3[] = { 0x22,0x11,0x05, 0x44,0x33,0xfa };
    for (size_t i=0;i<3;i++) {
        allow(fs[i]); usb_dsd_output_t *o=usb_dsd_output_open(3,0,false,1,176400); assert(o);
        reset_writes();
        assert(usb_dsd_output_hardware_rate(o)==176400);
        assert(usb_dsd_output_hardware_bits(o)==(fs[i]==PCM_FORMAT_S32_LE?32:24));
        uint64_t wrote=0; assert(usb_dsd_output_write(o,words,2,&wrote) && wrote==2);
        if (fs[i]==PCM_FORMAT_S24_3LE) assert(recorded_size==6 && !memcmp(recorded_data,packed3,6));
        if (fs[i]==PCM_FORMAT_S24_LE) assert(recorded_size==8 && !memcmp(recorded_data,words,8));
        if (fs[i]==PCM_FORMAT_S32_LE) { uint32_t x[2]={0x05112200,0xfa334400}; assert(recorded_size==8 && !memcmp(recorded_data,x,8)); }
        usb_dsd_output_close(o);
    }
}

static void unsupported_and_partial(void) {
    allow(PCM_FORMAT_S16_LE); assert(!usb_dsd_output_open(3,0,true,2,176400));
    allow(PCM_FORMAT_DSD_U16_LE); usb_dsd_output_t *o=usb_dsd_output_open(3,0,true,1,176400); assert(o);
    int32_t words[]={0x051122,0xfa3344}; uint64_t wrote=0;
    int one_then_zero[] = { 1, 0 }; script(one_then_zero,2);
    assert(!usb_dsd_output_write(o,words,2,&wrote) && wrote==1);
    assert(!usb_dsd_output_write(o,words+1,1,&wrote) && wrote==0); usb_dsd_output_close(o);

    allow(PCM_FORMAT_DSD_U8); o=usb_dsd_output_open(3,0,true,1,176400); assert(o); reset_writes();
    int short_then_full[] = { 1 }; script(short_then_full,1);
    assert(usb_dsd_output_write(o,words,2,&wrote) && wrote==2 && writes > 1);
    usb_dsd_output_close(o);

    allow(PCM_FORMAT_DSD_U32_LE); o=usb_dsd_output_open(3,0,true,1,176400); assert(o); reset_writes();
    int negative[] = { -5 }; script(negative,1);
    assert(!usb_dsd_output_write(o,words,2,&wrote) && wrote==0);
    assert(!usb_dsd_output_write(o,words,2,&wrote) && wrote==0); usb_dsd_output_close(o);

    allow(PCM_FORMAT_DSD_U16_LE); o=usb_dsd_output_open(3,0,true,1,176400); assert(o); reset_writes();
    size_t count=5000; int32_t *many=malloc(count*sizeof(*many)); assert(many);
    for (size_t i=0;i<count;i++) many[i]=(int32_t)(0x051122u + (uint32_t)i);
    int multistage[] = { 17, 13 }; script(multistage,2);
    assert(usb_dsd_output_write(o,many,count,&wrote) && wrote==count && writes > 3);
    assert(recorded_size==count*2); free(many); usb_dsd_output_close(o);
}

static void native_odd_frame_carry(void) {
    allow(PCM_FORMAT_DSD_U32_LE);
    usb_dsd_output_t *o=usb_dsd_output_open(3,0,true,1,176400); assert(o);
    int32_t words[]={0x051122,0xfa3344}; uint64_t wrote=0; reset_writes();
    assert(usb_dsd_output_write(o,words,1,&wrote) && wrote==1 && writes==0);
    assert(usb_dsd_output_write(o,words+1,1,&wrote) && wrote==1 && writes==1);
    const uint8_t pair[]={0x44,0x33,0x22,0x11}; assert(recorded_size==4 && !memcmp(recorded_data,pair,4));
    assert(usb_dsd_output_write(o,words,1,&wrote) && wrote==1 && writes==1);
    usb_dsd_output_close(o);
    assert(recorded_size==4); /* Closing discards one incomplete U32 native word. */
}

int main(void) { native_formats(); dop_formats(); unsupported_and_partial(); period_sizing_and_fallback(); native_odd_frame_carry(); puts("usb_dsd_output_selftest: PASS"); return 0; }
