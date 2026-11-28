#pragma once

#include "../../core/process/audio_route_env.h"

#include <stdbool.h>

/* Audio hand-over to an external process (an emulator or an app). */

/* Stops playback and waits (up to 2 s) until the audio worker has released
 * the device. False when it is still busy. */
bool external_session_release_audio(void);

/* The player's audio route at launch time, from state that is already known:
 * the launch must not wait on bluealsactl. Wired, a USB DAC that is gone, or
 * Bluetooth whose negotiated rate is unknown (0 after a disconnect) leave
 * out->count at 0. */
void external_session_audio_route(audio_route_env_t * out);
