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
    puts("Bluetooth DAC tracked route tests passed");
}
