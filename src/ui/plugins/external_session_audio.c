#include "external_session_audio.h"

#include "../../audio/engine/audio.h"
#include "../../hardware/usb/usb_audio_output.h"
#include "../../network/bluetooth/control/bluetooth_control.h"
#include "../application/gui.h"
#include "../player/screen/gui_player.h"

#include <time.h>

bool external_session_release_audio(void) {
    /* Pausing only schedules a fade, so wait until the worker confirms. */
    gui_player_stop_for_external_session();
    const struct timespec audio_wait = { .tv_sec = 0, .tv_nsec = 20000000 };
    for (int waited_ms = 0; waited_ms < 2000; waited_ms += 20) {
        if (audio_is_idle()) return true;
        (void) nanosleep(&audio_wait, NULL);
    }
    return audio_is_idle();
}

#ifndef HOST_BUILD
void external_session_audio_route(audio_route_env_t * out) {
    audio_output_info_t info;
    audio_output_get_info(&info);
    char usb_device[32] = "";
    audio_route_t route = { .route = info.requested_route };
    if (route.route == AUDIO_OUTPUT_ROUTE_USB && usb_audio_output_is_connected(usb_device, sizeof(usb_device))) {
        route.device = usb_device;
        route.gain = audio_get_usb_gain();
    } else if (route.route == AUDIO_OUTPUT_ROUTE_BLUETOOTH) {
        route.device = bt_control_get_playback_pcm();
        route.rate = bt_connected_rate_cached;
    }
    audio_route_env_build(&route, out);
}
#else
void external_session_audio_route(audio_route_env_t * out) {
    out->count = 0;
}
#endif
