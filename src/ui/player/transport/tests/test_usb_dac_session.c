#define GUI_SHELL_INTERNAL_H
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include "src/hardware/usb/usb_dac_bridge.h"

static bool armed, grant;
static int checkpoints;
static usb_dac_stream_info_t snapshot;
void usb_dac_bridge_get_stream_info(usb_dac_stream_info_t *out) { *out = snapshot; }
void usb_dac_bridge_set_output_allowed(bool allowed) { grant = allowed; }
static bool gui_network_usb_dac_active(void) { return armed; }
bool gui_shell_usb_dac_owns_output(void);
static void gui_player_stop_for_external_session(void) {
    assert(gui_shell_usb_dac_owns_output());
    assert(!grant);
    checkpoints++;
}
bool gui_shell_usb_dac_owns_output(void);
#include "src/ui/shell/gui_shell_usb_dac_session.c"

int main(void) {
    armed = true;
    snapshot.bridge_running = true;
    poll_usb_dac_session();
    assert(!gui_shell_usb_dac_owns_output() && !grant && checkpoints == 0);
    snapshot.streaming = true;
    poll_usb_dac_session();
    assert(gui_shell_usb_dac_owns_output() && grant && checkpoints == 1);
    poll_usb_dac_session();
    assert(checkpoints == 1);
    snapshot.owns_output = true;
    snapshot.streaming = false;
    snapshot.output_releasing = true;
    poll_usb_dac_session();
    assert(!grant && gui_shell_usb_dac_owns_output());
    /* A short host stop keeps the reservation and does not stop local twice. */
    snapshot.streaming = true;
    snapshot.output_releasing = false;
    poll_usb_dac_session();
    assert(grant && checkpoints == 1);
    snapshot.streaming = false;
    snapshot.owns_output = false;
    poll_usb_dac_session();
    assert(!grant && !gui_shell_usb_dac_owns_output());
    snapshot.streaming = true;
    poll_usb_dac_session();
    assert(grant && checkpoints == 2);
    /* Cable/sleep/Stop revoke a stream; stalled close must retain exclusion. */
    armed = false;
    snapshot.owns_output = true;
    poll_usb_dac_session();
    assert(!grant && gui_shell_usb_dac_owns_output());
    snapshot.bridge_running = false;
    poll_usb_dac_session();
    assert(gui_shell_usb_dac_owns_output());
    snapshot.owns_output = false;
    poll_usb_dac_session();
    assert(!gui_shell_usb_dac_owns_output());
    /* A stopped bridge cannot claim output from a stale streaming flag. */
    armed = true;
    poll_usb_dac_session();
    assert(!grant && checkpoints == 2);
    puts("USB DAC armed/owning session tests passed");
}
