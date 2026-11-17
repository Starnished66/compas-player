/* Fake child lifetime; real pipe exercises nonblocking receiver-output drain. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/wait.h>
#include <unistd.h>
static pid_t route_waitpid(pid_t pid, int *status, int options);
#define waitpid route_waitpid
#include "../bluetooth_control.c"
#include "../bt_dac_info.c"
#include "../bt_control_daemon.c"
#undef waitpid
/* Only what bt_dac_info_refresh_event reaches when a PCM is removed. */
bool bt_pcm_path_mac(const char * path, char * mac) {
    const char * dev = strstr(path, "dev_");
    if (!dev || strlen(dev + 4) < 17) return false;
    for (int i = 0; i < 17; i++) mac[i] = dev[4 + i] == '_' ? ':' : dev[4 + i];
    mac[17] = '\0';
    return true;
}
bool bt_device_info_text(const char * mac, char * out, size_t size) { (void) mac; (void) out; (void) size; return false; }
void copy_info_field(char * dst, size_t size, const char * text, const char * key) { (void) dst; (void) size; (void) text; (void) key; }
void bluealsa_apply_soft_volume(const char * path) { (void) path; }
void bluealsa_clear_soft_volume_path(const char * path) { (void) path; }
bool subprocess_run(char * const argv[], char * out_buf, size_t out_buf_size) { (void) argv; (void) out_buf; (void) out_buf_size; return false; }
static bool cancelled_now, cancel_on_spawn, spawn_ok=true, exited, wait_error, already_reaped;
static int spawns, terminations, reaps;
bool subprocess_popen(char * const argv[], pid_t *pid, int *fd) {
    assert(!strcmp(argv[0],"bluealsa-aplay"));
    assert(!strcmp(argv[1],"--pcm-buffer-time=200000"));
    assert(!strcmp(argv[2],"--pcm-period-time=50000"));
    assert(!strcmp(argv[3],"AA:BB:CC:DD:EE:FF") && !argv[4]);
    spawns++;
    if(!spawn_ok) return false;
    int descriptors[2]; assert(pipe(descriptors)==0);
    assert(write(descriptors[1],"log",3)==3); close(descriptors[1]);
    *fd=descriptors[0]; *pid=1234;
    if(cancel_on_spawn) cancelled_now=true;
    return true;
}
void subprocess_terminate(pid_t pid) { assert(pid==1234); terminations++; exited=true; }
static pid_t route_waitpid(pid_t pid, int *status, int options) {
    (void)status; assert(pid==1234);
    if(wait_error) { errno=EIO; return -1; }
    if(already_reaped) { errno=ECHILD; return -1; }
    if(options==WNOHANG && !exited) return 0;
    reaps++; return pid;
}
static bool cancelled(void *ctx) { assert(ctx==&cancelled_now); return cancelled_now; }
static bool start(const bt_dac_stream_info_t *stream) {
    return bt_control_dac_route_start(stream,cancelled,&cancelled_now);
}
int main(void) {
    bt_dac_stream_info_t stream={.running=true,.available=true,.generation=7,
        .mac="AA:BB:CC:DD:EE:FF",.pcm_path="/org/bluealsa/owner"};
    stream_generation=7; bt_dac_info_store(&stream); last_applied_dac_mode_enabled=true;
    cancelled_now=true; assert(!start(&stream) && spawns==0);
    cancelled_now=false; stream.generation++; assert(!start(&stream) && spawns==0); stream.generation--;
    snprintf(bt_dac_info.pcm_path,sizeof(bt_dac_info.pcm_path),"/different");
    assert(!start(&stream) && spawns==0);
    snprintf(bt_dac_info.pcm_path,sizeof(bt_dac_info.pcm_path),"%s",stream.pcm_path);
    bt_dac_info.running=false; assert(!start(&stream) && spawns==0); bt_dac_info.running=true;
    last_applied_dac_mode_enabled=false; assert(!start(&stream) && spawns==0);
    last_applied_dac_mode_enabled=true;
    spawn_ok=false; assert(!start(&stream) && spawns==1); spawn_ok=true;
    cancel_on_spawn=true; assert(!start(&stream));
    assert(terminations==1 && reaps==1 && dac_route_pid<0);
    cancel_on_spawn=cancelled_now=exited=false;
    assert(start(&stream) && bt_control_dac_route_alive());
    assert(!start(&stream)); /* Never admit another child while this one is tracked. */
    int before=reaps; wait_error=true;
    assert(!bt_control_dac_route_stop() && dac_route_pid==1234 && dac_route_fd>=0 && reaps==before);
    wait_error=false; assert(bt_control_dac_route_stop());
    assert(reaps==before+1 && dac_route_pid<0 && dac_route_fd<0);
    exited=false; assert(start(&stream)); exited=true;
    before=reaps; assert(!bt_control_dac_route_alive());
    assert(reaps==before+1 && dac_route_pid<0 && dac_route_fd<0);
    assert(bt_control_dac_route_stop());
    exited=false; assert(start(&stream)); already_reaped=true;
    assert(bt_control_dac_route_stop() && dac_route_pid<0 && dac_route_fd<0);
    /* Removing the selected phone's PCM keeps another phone's PCM available. */
    memset(pcm_edges,0,sizeof(pcm_edges));
    const char *a="/org/bluealsa/hci0/dev_AA_BB_CC_DD_EE_FF/a2dpsnk/source";
    const char *b="/org/bluealsa/hci0/dev_11_22_33_44_55_66/a2dpsnk/source";
    snprintf(pcm_edges[0].path,sizeof(pcm_edges[0].path),"%s",a);
    snprintf(pcm_edges[1].path,sizeof(pcm_edges[1].path),"%s",b);
    stream=(bt_dac_stream_info_t){.available=true,.running=true};
    snprintf(stream.pcm_path,sizeof(stream.pcm_path),"%s",a); bt_dac_info_store(&stream);
    bt_dac_info_refresh_event(b,true,-1); /* Unselected phone leaves: nothing changes. */
    bt_control_get_dac_stream_info(&stream);
    assert(stream.available && !strcmp(stream.pcm_path,a));
    snprintf(pcm_edges[1].path,sizeof(pcm_edges[1].path),"%s",b);
    bt_dac_info_refresh_event(a,true,-1);
    bt_control_get_dac_stream_info(&stream);
    assert(stream.available && !strcmp(stream.pcm_path,b) && !strcmp(stream.mac,"11:22:33:44:55:66"));
    bt_dac_info_refresh_event(b,true,-1);
    bt_control_get_dac_stream_info(&stream);
    assert(!stream.available);
    puts("Bluetooth DAC tracked route tests passed");
}
