/* Run queued jobs explicitly to exercise cancellation before worker entry. */
#define GUI_SHELL_INTERNAL_H
#define HOST_BUILD
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "src/network/bluetooth/control/bluetooth_control.h"
typedef struct { atomic_bool done; bool active; } shell_job_t;
enum { USB_MODE_DAC = 1 };
static struct { bool bt_dac_mode_enabled, wifi_dac_mode_enabled; int usb_mode; } current_settings;
static bool bt_is_powered_cached, idle, start_ok, stop_ok;
static bt_dac_stream_info_t snapshot;
static int starts, stops, checkpoints, waits;
static bool cancel_wait, cancel_spawn, stop_timeout;
static void *(*pending)(void *);
static void *pending_arg;
static void gui_shell_bt_dac_release(void);
static bool shell_job_start(shell_job_t *job, void *(*fn)(void *), void *arg) {
    job->active=true; atomic_store(&job->done,false); pending=fn; pending_arg=arg; return true;
}
static bool shell_job_reap(shell_job_t *job) {
    if(!job->active || !atomic_load(&job->done)) return false;
    job->active=false; return true;
}
static void shell_job_cancel(shell_job_t *job) { (void)job; }
void bt_control_get_dac_stream_info(bt_dac_stream_info_t *out) { *out=snapshot; }
static bool audio_is_idle(void) { return idle; }
static bool gui_shell_bt_dac_busy(void);
static bool gui_shell_bt_dac_profile_failed(void) { return false; }
static bool gui_shell_bt_dac_profile_ready(void) { return current_settings.bt_dac_mode_enabled && bt_is_powered_cached; }
static void gui_shell_bt_dac_finish_profiles(void) { }
static bool bt_workers_active(void) { return false; }
static bool gui_network_toggle_bt_dac(void) { return current_settings.bt_dac_mode_enabled = true; }
static bool gui_network_airplay_enable_pending(void) { return false; }
static bool gui_network_usb_dac_pending(void) { return false; }
static void gui_player_stop_for_external_session(void) { checkpoints++; }
void bt_control_dac_set_reserved(bool reserved) { (void)reserved; }
static void fake_sleep(unsigned int delay);
#define usleep fake_sleep
bool bt_control_dac_route_start(const bt_dac_stream_info_t *info, bt_control_cancel_callback_t cancelled, void *ctx) {
    assert(idle && info->running && !cancelled(ctx)); starts++;
    if(cancel_spawn) gui_shell_bt_dac_release();
    return start_ok;
}
bool bt_control_dac_route_alive(void) { return true; }
bool bt_control_dac_route_stop(void);
#include "src/ui/shell/gui_shell_bt_dac_session.c"
static bool gui_shell_bt_dac_busy(void) { return owns_output; }
static void fake_sleep(unsigned int delay) {
    waits++;
    if(delay==10000) { if(cancel_wait) gui_shell_bt_dac_release(); else if (!stop_timeout) idle=true; }
    else gui_shell_bt_dac_release();
}
bool bt_control_dac_route_stop(void) {
    assert(gui_shell_bt_dac_owns_output()); stops++; return stop_ok;
}
static void reset(void) {
    memset(&session_job,0,sizeof(session_job)); owns_output=false; session_error=false;
    consumed_generation=0; atomic_store(&request_generation,0); atomic_store(&receiving,false);
    current_settings.bt_dac_mode_enabled=true; bt_is_powered_cached=true;
    idle=false; start_ok=stop_ok=true; cancel_wait=cancel_spawn=stop_timeout=false; starts=stops=checkpoints=waits=0;
    snapshot=(bt_dac_stream_info_t){.available=true,.running=true,.generation=1,
        .mac="AA:BB:CC:DD:EE:FF",.pcm_path="/owner"};
}
int main(void) {
    reset(); poll_bt_dac_session(); assert(owns_output && checkpoints==1 && starts==0);
    pending(pending_arg); assert(waits && starts==1 && stops==1 && owns_output);
    poll_bt_dac_session(); assert(!owns_output); /* Only reaping releases reservation. */
    poll_bt_dac_session(); assert(starts==1 && checkpoints==1); /* No retry of same edge. */
    reset(); poll_bt_dac_session(); gui_shell_bt_dac_release();
    pending(pending_arg); assert(starts==0 && stops==1);
    poll_bt_dac_session(); assert(!owns_output);
    reset(); cancel_wait=true; poll_bt_dac_session(); pending(pending_arg);
    assert(starts==0 && stops==1 && owns_output); poll_bt_dac_session(); assert(!owns_output);
    reset(); stop_timeout=true; poll_bt_dac_session(); pending(pending_arg);
    assert(starts==0 && stops==1 && owns_output && worker_failed);
    poll_bt_dac_session(); assert(!owns_output && session_error);
    reset(); cancel_spawn=true; poll_bt_dac_session(); pending(pending_arg);
    assert(starts==1 && stops==1 && !gui_shell_bt_dac_receiving());
    poll_bt_dac_session(); assert(!owns_output);
    reset(); poll_bt_dac_session();
    snapshot.generation=2; snprintf(snapshot.mac,sizeof(snapshot.mac),"11:22:33:44:55:66");
    snprintf(snapshot.pcm_path,sizeof(snapshot.pcm_path),"/second");
    poll_bt_dac_session(); pending(pending_arg); poll_bt_dac_session();
    assert(!owns_output && starts==0 && checkpoints==1); /* No queued second-phone takeover. */
    reset(); poll_bt_dac_session(); snapshot.generation=2; snprintf(snapshot.pcm_path,sizeof(snapshot.pcm_path),"/resumed");
    poll_bt_dac_session(); pending(pending_arg); poll_bt_dac_session();
    assert(owns_output && owner.generation==2 && checkpoints==2); /* Same phone resuming before the reap. */
    reset(); start_ok=false; poll_bt_dac_session(); pending(pending_arg);
    poll_bt_dac_session(); assert(!owns_output && session_error && starts==1);
    poll_bt_dac_session(); assert(starts==1 && checkpoints==1);
    reset(); stop_ok=false; poll_bt_dac_session(); pending(pending_arg);
    poll_bt_dac_session(); assert(owns_output && session_error); /* Unsafe closure keeps lock. */
    reset(); current_settings.bt_dac_mode_enabled = false; snapshot.running = false; gui_shell_bt_dac_remember(true);
    poll_bt_dac_session(); assert(!current_settings.bt_dac_mode_enabled); /* Still on: power-off pending. */
    bt_is_powered_cached = false; poll_bt_dac_session(); assert(!current_settings.bt_dac_mode_enabled);
    bt_is_powered_cached = true; poll_bt_dac_session(); assert(current_settings.bt_dac_mode_enabled && !restore_on_power);
    reset(); current_settings.bt_dac_mode_enabled = false; snapshot.running = false; gui_shell_bt_dac_remember(true);
    bt_is_powered_cached = false; poll_bt_dac_session(); current_settings.wifi_dac_mode_enabled = true;
    bt_is_powered_cached = true; poll_bt_dac_session(); /* AirPlay chosen meanwhile wins. */
    assert(!current_settings.bt_dac_mode_enabled && !restore_on_power);
    puts("Bluetooth DAC session ordering tests passed");
}
