/* Volume: UI curve, hardware register mapping, plugin curves and the
 * background request worker. Shared state is guarded by audio_mutex. */
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

float volume = 1.0f;      /* UI-facing 0.0-1.0 percent, what audio_get_volume() returns */
float volume_gain = 1.0f; /* actual linear PCM multiplier the playback thread applies -- see audio_set_volume() */
static atomic_uint volume_set_generation = 0;
static atomic_uint volume_applied_generation = 0;
static atomic_int max_volume_cap_percent = 100;
static pthread_once_t volume_request_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t volume_request_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t volume_request_cond = PTHREAD_COND_INITIALIZER;
static bool volume_request_worker_ready = false;
static bool volume_request_pending = false;
static float volume_requested_value = 1.0f;
static unsigned int volume_requested_generation = 0;

/* Maps UI volume (0-100%) to a logarithmic audio taper in dB (MIN_VOLUME_DB..0dB),
 * converted to a linear multiplier for apply_gain(). 0% is true silence. */
#define MIN_VOLUME_DB (-70.0)

/* Curve exponent applied before the equal-dB taper to calibrate volume progression
 * across the 0-100% range. */
#define VOLUME_CURVE_EXPONENT 0.53

#ifndef HOST_BUILD /* only the hardware volume path uses the taper helpers */
static double calibrated_taper_db(double percent) {
    if (percent <= 0.0) return MIN_VOLUME_DB;
    if (percent >= 1.0) return 0.0;
    return MIN_VOLUME_DB * (1.0 - pow(percent, VOLUME_CURVE_EXPONENT));
}

/* Hardware volume mapping: the codec's "Left"/"Right Playback Volume" ALSA controls
 * provide hardware attenuation. Raw 0 is maximum volume (0dB attenuation), with
 * higher raw values providing increasing attenuation up to full mute. */
#define HW_VOLUME_DB_PER_STEP 0.34

static int volume_db_to_hw_raw(double db) {
    if (db >= 0.0) return 0; /* hardware's loudest point -- can't go past it */
    int raw = (int) lround(-db / HW_VOLUME_DB_PER_STEP);
    if (raw > HW_VOLUME_MAX_RAW) raw = HW_VOLUME_MAX_RAW;
    return raw;
}
#endif

/* Plugin-supplied alternative to volume_db_to_hw_raw()/calibrated_taper_db()
 * allowing plugins to define custom UI-volume to hardware-register mappings.
 *
 * LIVE state read by compute_hw_raw(). Mutated only by audio_set_custom_hw_volume_curve()
 * and audio_commit_hw_volume_curve(). */
static bool custom_hw_curve_active = false;
static uint8_t custom_hw_curve[HW_VOLUME_CURVE_LEN];

/* STAGED state scratch space used during plugin (re)load transactions,
 * committed to LIVE state via audio_commit_hw_volume_curve(). */
static bool staged_hw_curve_active = false;
static uint8_t staged_hw_curve[HW_VOLUME_CURVE_LEN];

/* For local output, hardware attenuation is used and digital volume_gain remains 1.0.
 * For USB DAC output, hardware mixer controls do not apply to the USB stream, so
 * digital attenuation is applied instead. Bluetooth volume is managed separately
 * via AVRCP.
 *
 * Shared helper for volume calculation. Caller must hold audio_mutex. */
#ifndef HOST_BUILD /* only the hardware volume path uses it */
static int compute_hw_raw(float vol) {
    if (custom_hw_curve_active) {
        /* Use plugin curve index 0 for mute when vol <= 0.0f. */
        int idx = (vol <= 0.0f) ? 0 : (int) lround((double) vol * 100.0);
        if (idx < 0) idx = 0;
        if (idx > 100) idx = 100;
        return custom_hw_curve[idx];
    }
    return (vol <= 0.0f) ? HW_VOLUME_MAX_RAW : volume_db_to_hw_raw(calibrated_taper_db((double) vol));
}

static void request_hw_volume(float vol) {
    int raw = compute_hw_raw(vol);
    audio_output_request_hw_volume_raw(raw, raw);
}
#endif

/* Apply only if no newer synchronous or queued request has superseded this
 * generation. The check and state update share audio_mutex, so a worker
 * that races a release-time audio_set_volume() cannot restore an older
 * drag sample afterward. */
static float audio_clamp_volume_locked(float new_volume) {
    if (new_volume < 0.0f) new_volume = 0.0f;
    if (new_volume > 1.0f) new_volume = 1.0f;
    float cap = (float) atomic_load(&max_volume_cap_percent) / 100.0f;
    return new_volume > cap ? cap : new_volume;
}

static void audio_apply_volume_locked(float new_volume) {
    volume = new_volume;
    volume_gain = (new_volume <= 0.0f) ? 0.0f : 1.0f;
#ifndef HOST_BUILD
    request_hw_volume(new_volume);
    if (audio_output_is_usb_active()) {
        volume_gain = (new_volume <= 0.0f) ? 0.0f : (float) pow(10.0, calibrated_taper_db((double) new_volume) / 20.0);
    }
#endif
}

static void audio_apply_volume(float new_volume, unsigned int generation) {
    pthread_mutex_lock(&audio_mutex);
    if (atomic_load(&volume_set_generation) != generation) {
        pthread_mutex_unlock(&audio_mutex);
        return;
    }
    /* Read the cap only after taking audio_mutex: a cap reduction cannot
     * race a queued request that already sampled the old ceiling. */
    new_volume = audio_clamp_volume_locked(new_volume);
    audio_apply_volume_locked(new_volume);
    atomic_store_explicit(&volume_applied_generation, generation, memory_order_release);
    pthread_mutex_unlock(&audio_mutex);
}

void audio_set_volume(float new_volume) {
    unsigned int generation = atomic_fetch_add(&volume_set_generation, 1) + 1;
    audio_apply_volume(new_volume, generation);
}

unsigned int audio_get_volume_generation(void) {
    return atomic_load_explicit(&volume_set_generation, memory_order_acquire);
}

unsigned int audio_get_applied_volume_generation(void) {
    return atomic_load_explicit(&volume_applied_generation, memory_order_acquire);
}

bool audio_try_set_external_volume(float new_volume, unsigned int expected_generation,
                                   const atomic_uint * owner_generation,
                                   unsigned int expected_owner_generation,
                                   unsigned int * committed_generation) {
    if (!owner_generation || !committed_generation) return false;

    pthread_mutex_lock(&audio_mutex);
    if (atomic_load_explicit(owner_generation, memory_order_acquire) != expected_owner_generation) {
        pthread_mutex_unlock(&audio_mutex);
        return false;
    }

    unsigned int generation = expected_generation;
    unsigned int next_generation = expected_generation + 1;
    if (!atomic_compare_exchange_strong_explicit(&volume_set_generation, &generation,
                                                 next_generation, memory_order_acq_rel,
                                                 memory_order_acquire)) {
        pthread_mutex_unlock(&audio_mutex);
        return false;
    }

    new_volume = audio_clamp_volume_locked(new_volume);
    audio_apply_volume_locked(new_volume);
    atomic_store_explicit(&volume_applied_generation, next_generation, memory_order_release);
    *committed_generation = next_generation;
    pthread_mutex_unlock(&audio_mutex);
    return true;
}

int audio_clamp_volume_percent(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    int cap = atomic_load(&max_volume_cap_percent);
    return percent > cap ? cap : percent;
}

void audio_set_max_volume_percent(int percent) {
    if (percent < 1) percent = 1;
    if (percent > 100) percent = 100;
    pthread_mutex_lock(&audio_mutex);
    atomic_store(&max_volume_cap_percent, percent);
    /* Clamp current output atomically with the cap update. Queued requests
     * continue through their generation check and are clipped to this cap. */
    float cap = (float) percent / 100.0f;
    if (volume > cap) audio_apply_volume_locked(cap);
    pthread_mutex_unlock(&audio_mutex);
}

void audio_stage_custom_hw_volume_curve(bool active, const uint8_t * curve) {
    pthread_mutex_lock(&audio_mutex);
    staged_hw_curve_active = active;
    if (active) memcpy(staged_hw_curve, curve, sizeof(staged_hw_curve));
    pthread_mutex_unlock(&audio_mutex);
}

void audio_get_staged_hw_volume_curve_state(bool * active_out, uint8_t * curve_out) {
    pthread_mutex_lock(&audio_mutex);
    *active_out = staged_hw_curve_active;
    memcpy(curve_out, staged_hw_curve, sizeof(staged_hw_curve));
    pthread_mutex_unlock(&audio_mutex);
}

/* Installs staged as the new live state and writes hardware once to
 * match, both under the same lock -- see this function's own doc comment
 * in audio.h for why the install and the write must be atomic together,
 * not two separate locked steps. */
void audio_commit_hw_volume_curve(void) {
    pthread_mutex_lock(&audio_mutex);
    custom_hw_curve_active = staged_hw_curve_active;
    if (staged_hw_curve_active) memcpy(custom_hw_curve, staged_hw_curve, sizeof(custom_hw_curve));
#ifndef HOST_BUILD
    request_hw_volume(volume);
#endif
    pthread_mutex_unlock(&audio_mutex);
}

/* Reapplies immediately at whatever volume is currently authoritative, so
 * a curve switch is audible right away rather than waiting for the next
 * slider touch -- for the normal "an already-loaded, running plugin
 * changes its curve" case only, see this function's own doc comment in
 * audio.h. Mutates the LIVE pair directly, under one lock alongside the
 * write -- NOT via audio_stage_custom_hw_volume_curve()/
 * audio_commit_hw_volume_curve(), which operate on the separate STAGED
 * pair a plugin (re)load transaction uses; this path runs outside any
 * such transaction; touching STAGED here would leave it holding a value
 * that doesn't belong to (and could leak into) whatever transaction
 * starts next. Deliberately NOT implemented via audio_set_volume():
 * that bumps volume_set_generation as a brand-new synchronous command,
 * which would silently invalidate a newer audio_request_volume() (an
 * in-flight slider-drag sample) already queued on the worker thread by
 * the time it runs -- this only ever touches the hardware register, not
 * that generation counter. */
void audio_set_custom_hw_volume_curve(const uint8_t * curve) {
    pthread_mutex_lock(&audio_mutex);
    custom_hw_curve_active = (curve != NULL);
    if (curve) memcpy(custom_hw_curve, curve, sizeof(custom_hw_curve));
#ifndef HOST_BUILD
    request_hw_volume(volume);
#endif
    pthread_mutex_unlock(&audio_mutex);
}

static void * volume_request_worker_main(void * unused) {
    (void) unused;
    for (;;) {
        pthread_mutex_lock(&volume_request_mutex);
        while (!volume_request_pending)
            pthread_cond_wait(&volume_request_cond, &volume_request_mutex);
        float requested = volume_requested_value;
        unsigned int generation = volume_requested_generation;
        volume_request_pending = false;
        pthread_mutex_unlock(&volume_request_mutex);
        audio_apply_volume(requested, generation);
    }
    return NULL;
}

static void start_volume_request_worker(void) {
    pthread_t thread;
    if (pthread_create(&thread, NULL, volume_request_worker_main, NULL) == 0) {
        pthread_detach(thread);
        volume_request_worker_ready = true;
    }
}

void audio_request_volume(float new_volume) {
    if (new_volume < 0.0f) new_volume = 0.0f;
    if (new_volume > 1.0f) new_volume = 1.0f;
    unsigned int generation = atomic_fetch_add(&volume_set_generation, 1) + 1;
    pthread_once(&volume_request_once, start_volume_request_worker);
    if (!volume_request_worker_ready) {
        audio_apply_volume(new_volume, generation);
        return;
    }
    pthread_mutex_lock(&volume_request_mutex);
    volume_requested_value = new_volume;
    volume_requested_generation = generation;
    volume_request_pending = true;
    pthread_cond_signal(&volume_request_cond);
    pthread_mutex_unlock(&volume_request_mutex);
}

float audio_get_volume(void) {
    pthread_mutex_lock(&audio_mutex);
    float result = volume;
    pthread_mutex_unlock(&audio_mutex);
    return result;
}
