/* Output device layer: SDL device on the host build; tinyalsa-backed audio_output
 * (with bounded close+reopen retries) on the target. */
#include "audio_internal.h"
#include "audio_helpers.h"
#include "../../ui/text/i18n/i18n.h"
#include "../demux/mp4/mp4_demux.h"
#include "../demux/ape/ape_demux.h"
#include "../decoders/ogg/ogg_probe.h"
#include "../effects/peq.h"
#include "../timing/audio_tempo.h"
#include "../../network/http/stream/http_stream_file.h"
#include "../../library/remote/remote_track.h"
#include "../../library/metadata/parsing/metadata.h"
#include "../../library/parsers/cue/cue_parser.h"
#include "../../core/system/logging/debug_log.h"
#include "../../core/persistence/storage_paths.h"

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <unistd.h>
#include <limits.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>

#ifdef HOST_BUILD
  #include <SDL2/SDL.h>
#else
  #include "../output/audio_output.h"
#endif

#ifdef HOST_BUILD
SDL_AudioDeviceID sdl_dev = 0;
unsigned int device_channels = 0;
unsigned int device_sample_rate = 0;
#endif

#ifdef HOST_BUILD
static bool open_device(unsigned int channels, unsigned int sample_rate) {
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = (int) sample_rate;
    want.format = AUDIO_S16SYS;
    want.channels = (Uint8) channels;
    want.samples = NORMAL_CHUNK_FRAMES;

    sdl_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (sdl_dev == 0) {
        fprintf(stderr, "audio: SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_PauseAudioDevice(sdl_dev, 0);
    device_channels = channels;
    device_sample_rate = sample_rate;
    return true;
}

void close_device(void) {
    if (sdl_dev != 0) { SDL_CloseAudioDevice(sdl_dev); sdl_dev = 0; }
    device_channels = 0;
    device_sample_rate = 0;
    atomic_store_explicit(&local_output_active, false, memory_order_release);
}

bool ensure_device(unsigned int channels, unsigned int sample_rate) {
    atomic_store_explicit(&local_output_active, true, memory_order_release);
    bool device_open = (sdl_dev != 0);
    if (device_open && device_channels == channels && device_sample_rate == sample_rate) return true;
    close_device();
    atomic_store_explicit(&local_output_active, true, memory_order_release);
    return open_device(channels, sample_rate);
}

void write_device(const int16_t * buf, uint64_t frames, unsigned int channels) {
    SDL_QueueAudio(sdl_dev, buf, (Uint32) (frames * channels * sizeof(int16_t)));

    /* Throttle so the decode loop doesn't race ahead and queue the whole file at once */
    Uint32 max_queued_bytes = device_sample_rate * channels * sizeof(int16_t);
    while (SDL_GetQueuedAudioSize(sdl_dev) > max_queued_bytes) {
        pthread_mutex_lock(&audio_mutex);
        bool stop_now = stop_requested || restart_requested || paused || seek_pending;
        pthread_mutex_unlock(&audio_mutex);
        if (stop_now) break;
        usleep(10000);
    }
}
#else
/* Target build: thin wrappers around the shared audio_output module (see
 * audio_output.h) for output routing and format configuration. */
bool ensure_device_format(unsigned int channels, unsigned int sample_rate, bool want_s24) {
    /* Decoder-fed local playback requests standard battery-tuned local buffer. */
    atomic_store_explicit(&local_output_active, true, memory_order_release);
    bool ok = audio_output_ensure(channels, sample_rate, false, want_s24);
    if (ok) {
        /* Reapply volume if USB output state changed so digital volume taper
         * is correctly activated or deactivated. */
        static bool last_gain_for_usb = false;
        bool now_usb = audio_output_is_usb_active();
        if (now_usb != last_gain_for_usb) {
            audio_set_volume(audio_get_volume());
            last_gain_for_usb = now_usb;
        }
    }
    return ok;
}

bool ensure_device(unsigned int channels, unsigned int sample_rate) {
    return ensure_device_format(channels, sample_rate, false);
}

bool ensure_device_dop(unsigned int channels, unsigned int sample_rate) {
    atomic_store_explicit(&local_output_active, true, memory_order_release);
    return audio_output_ensure_dop(channels, sample_rate);
}

void close_device(void) {
    audio_output_close();
    atomic_store_explicit(&local_output_active, false, memory_order_release);
}

/* Writes a PCM batch to the output device with bounded close+reopen
 * retries. Returns true when all frames were successfully delivered.
 * Checks stop/restart between retries so the playback thread stays
 * controllable, unless allow_during_stop_restart is true for controlled
 * transition ramps (e.g. pause/stop/seek ramp-downs before powering down).
 * Tracks exact delivered frame count to avoid replaying audio on retry.
 *
 * Target build only: the HOST_BUILD SDL path uses write_device() directly
 * (SDL never needs close+reopen recovery the way tinyalsa/aplay can). */
#define OUTPUT_WRITE_RETRIES 3
#define OUTPUT_WRITE_RETRY_SLEEP_US 20000  /* 20 ms */

static bool write_abort_pending(bool allow_during_stop_restart) {
    pthread_mutex_lock(&audio_mutex);
    bool abort = should_abort_write_retry(allow_during_stop_restart, stop_requested, restart_requested);
    pthread_mutex_unlock(&audio_mutex);
    return abort;
}

/* Initial output acquisition has its own bounded recovery budget. In
 * particular, audio_output_ensure() can fail transiently while another route
 * is handing ownership back; do not turn that into a permanent track failure
 * before the first sample is written. This helper never closes an existing
 * device: audio_output_close() is owner-guarded, and an ensure failure must
 * not tear down another producer's output. */
write_result_t ensure_device_with_retry(unsigned int channels,
                                        unsigned int sample_rate,
                                        bool want_s24,
                                        bool dop,
                                        const char * path,
                                        bool allow_during_stop_restart) {
    for (int attempt = 0; attempt <= OUTPUT_WRITE_RETRIES; attempt++) {
        bool abort = write_abort_pending(allow_during_stop_restart);
        if (abort) return WRITE_RESULT_ABORTED;

        bool ok = dop ? ensure_device_dop(channels, sample_rate)
            : want_s24 ? ensure_device_format(channels, sample_rate, true)
            : ensure_device(channels, sample_rate);
        if (ok) return WRITE_RESULT_OK;
        /* A refused DoP rate will not change on retry. */
        if (dop) break;

        if (attempt == OUTPUT_WRITE_RETRIES) break;
        DBG_LOG("audio: output ensure failed attempt %d/%d (%s)\n",
                attempt + 1, OUTPUT_WRITE_RETRIES + 1, safe_path_tail(path));
        usleep(OUTPUT_WRITE_RETRY_SLEEP_US);
    }
    return WRITE_RESULT_FAILED;
}

write_result_t write_device_with_retry_ex(const int16_t * buf, uint64_t frames,
                                                unsigned int channels,
                                                unsigned int sample_rate,
                                                const char * path,
                                                uint64_t * out_delivered_frames,
                                                bool allow_during_stop_restart) {
    uint64_t delivered = 0;
    if (out_delivered_frames) *out_delivered_frames = 0;

    write_result_t ensured = ensure_device_with_retry(channels, sample_rate, false, false,
                                                      path, allow_during_stop_restart);
    if (ensured != WRITE_RESULT_OK) return ensured;

    for (int attempt = 0; attempt <= OUTPUT_WRITE_RETRIES; attempt++) {
        bool abort = write_abort_pending(allow_during_stop_restart);
        if (abort) {
            if (out_delivered_frames) *out_delivered_frames = delivered;
            return WRITE_RESULT_ABORTED;
        }

        if (attempt > 0) {
            close_device();
            usleep(OUTPUT_WRITE_RETRY_SLEEP_US);
            abort = write_abort_pending(allow_during_stop_restart);
            if (abort) {
                if (out_delivered_frames) *out_delivered_frames = delivered;
                return WRITE_RESULT_ABORTED;
            }
            if (!ensure_device(channels, sample_rate)) {
                DBG_LOG("audio: output reopen failed on retry %d (%s)\n",
                        attempt, safe_path_tail(path));
                continue;
            }
            DBG_LOG("audio: output reopened for retry %d (%s)\n",
                    attempt, safe_path_tail(path));
        }

        const int16_t * cur_buf = buf + (size_t) (delivered * channels);
        uint64_t remaining_frames = frames - delivered;
        uint64_t written_this_call = 0;
        bool ok = audio_output_write(cur_buf, remaining_frames, channels, &written_this_call);
        delivered += written_this_call;
        if (out_delivered_frames) *out_delivered_frames = delivered;

        if (ok || delivered >= frames) return WRITE_RESULT_OK;

        DBG_LOG("audio: output write failed attempt %d/%d (delivered %" PRIu64 "/%" PRIu64 " frames) (%s)\n",
                attempt + 1, OUTPUT_WRITE_RETRIES + 1, delivered, frames, safe_path_tail(path));
    }
    DBG_LOG("audio: output recovery exhausted (%s)\n", safe_path_tail(path));
    if (out_delivered_frames) *out_delivered_frames = delivered;
    return WRITE_RESULT_FAILED;
}

/* fallback_buf must have room for at least frames*channels int16_t samples
 * (buf_cur, already allocated at MAX_CHUNK_FRAMES*MAX_CHANNELS and otherwise
 * idle while a chunk is being decoded/written through this s32 path, is what
 * every real caller passes -- see write_device_with_retry_s32()'s own
 * caller). Used only if the device turns out not to actually be S24_LE
 * when it's time to write -- see the ground-truth check inside the loop.
 */
write_result_t write_device_with_retry_s32_ex(const int32_t * buf, uint64_t frames,
                                                    unsigned int channels,
                                                    unsigned int sample_rate,
                                                    const char * path,
                                                    int16_t * fallback_buf,
                                                    uint64_t * out_delivered_frames,
                                                    bool allow_during_stop_restart,
                                                    bool dop) {
    uint64_t delivered = 0;
    if (out_delivered_frames) *out_delivered_frames = 0;

    write_result_t ensured = ensure_device_with_retry(channels, sample_rate, true, dop,
                                                      path, allow_during_stop_restart);
    if (ensured != WRITE_RESULT_OK) return ensured;

    for (int attempt = 0; attempt <= OUTPUT_WRITE_RETRIES; attempt++) {
        bool abort = write_abort_pending(allow_during_stop_restart);
        if (abort) {
            if (out_delivered_frames) *out_delivered_frames = delivered;
            return WRITE_RESULT_ABORTED;
        }

        if (attempt > 0) {
            close_device();
            usleep(OUTPUT_WRITE_RETRY_SLEEP_US);
            abort = write_abort_pending(allow_during_stop_restart);
            if (abort) {
                if (out_delivered_frames) *out_delivered_frames = delivered;
                return WRITE_RESULT_ABORTED;
            }
            if (dop ? !ensure_device_dop(channels, sample_rate)
                    : !ensure_device_format(channels, sample_rate, true)) {
                DBG_LOG("audio: output reopen (s32) failed on retry %d (%s)\n",
                        attempt, safe_path_tail(path));
                continue;
            }
            DBG_LOG("audio: output reopened (s32) for retry %d (%s)\n",
                    attempt, safe_path_tail(path));
        }

        const int32_t * cur_buf = buf + (size_t) (delivered * channels);
        uint64_t remaining_frames = frames - delivered;
        uint64_t written_this_call = 0;
        bool ok;
        if (dop && !audio_output_is_dop_active()) {
            /* Never narrowed or written as PCM: the caller converts instead. */
            break;
        } else if (audio_output_is_s24_active()) {
            ok = audio_output_write_s24(cur_buf, remaining_frames, channels, &written_this_call);
        } else {
            /* ensure_device_format() above only REQUESTED S24_LE -- this is the
             * ground truth for what actually opened. An S24_LE hw_params
             * negotiation failure that fell back to S16_LE (open_device()'s own
             * fallback) can leave this false even though the request above
             * just returned true. audio_output_write_s24() itself now refuses
             * this combination rather than corrupting samples (see its own
             * guard), so retrying it here would only exhaust the budget and
             * fail the whole track for no reason. Narrow this remaining portion
             * to S16 and deliver it through the ordinary, already-correct path
             * instead -- checked fresh on every attempt, not just once, so a
             * route/format change mid-retry is caught too. */
            size_t sample_count = (size_t) remaining_frames * channels;
            for (size_t i = 0; i < sample_count; i++) {
                fallback_buf[i] = (int16_t) (cur_buf[i] >> 8);
            }
            ok = audio_output_write(fallback_buf, remaining_frames, channels, &written_this_call);
        }
        delivered += written_this_call;
        if (out_delivered_frames) *out_delivered_frames = delivered;

        if (ok || delivered >= frames) return WRITE_RESULT_OK;

        DBG_LOG("audio: output write (s32) failed attempt %d/%d (delivered %" PRIu64 "/%" PRIu64 " frames) (%s)\n",
                attempt + 1, OUTPUT_WRITE_RETRIES + 1, delivered, frames, safe_path_tail(path));
    }
    DBG_LOG("audio: output recovery (s32) exhausted (%s)\n", safe_path_tail(path));
    if (out_delivered_frames) *out_delivered_frames = delivered;
    return WRITE_RESULT_FAILED;
}

#endif
