#include "gui_shell_internal.h"

/* UI reservation precedes the bridge grant. Keep it until the writer has
 * acknowledged revocation and closed its output, including a stalled close. */
static bool usb_output_reserved;

bool gui_shell_usb_dac_owns_output(void) {
    usb_dac_stream_info_t info;
    usb_dac_bridge_get_stream_info(&info);
    return usb_output_reserved || info.owns_output || info.output_releasing;
}

void gui_shell_usb_dac_release(void) {
    usb_dac_bridge_set_output_allowed(false);
}

void poll_usb_dac_session(void) {
    usb_dac_stream_info_t info;
    usb_dac_bridge_get_stream_info(&info);
    bool requesting = gui_network_usb_dac_active() && info.bridge_running && info.streaming && !info.output_failed;
    if (!requesting) {
        gui_shell_usb_dac_release();
        usb_dac_bridge_get_stream_info(&info);
        if (!info.owns_output && !info.output_releasing) usb_output_reserved = false;
        return;
    }
    if (!usb_output_reserved) {
        usb_output_reserved = true;
        gui_player_stop_for_external_session();
    }
    usb_dac_bridge_set_output_allowed(true);
}
