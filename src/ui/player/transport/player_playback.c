#include "../screen/gui_player.h"
#include "../screen/gui_player_internal.h"
#include "../queue/player_queue_model.h"
#include "player_seek_input.h"
#include "../../application/gui.h"
#include "../../settings/gui_settings.h"
#include "../../shell/gui_notifications.h"
#include "../../shell/gui_shell.h"
#include "../../settings/lock/gui_lock_screen.h"
#include "../../widgets/builders/screen_builders.h"
#include "../../text/i18n/i18n.h"
#include "../../../audio/engine/audio.h"
#include "../../../core/persistence/settings.h"
#include "../../../hardware/usb/usb_mode_control.h"
#include "../../../library/metadata/parsing/metadata.h"
#include "../../../library/playlists/playback_order.h"
#include "../../../library/remote/remote_track.h"
#include "../../../library/workers/track_metadata_worker.h"
#include "../../../network/airplay/airplay_control.h"
#include "../../../plugins/manager/plugin_manager.h"
#ifndef HOST_BUILD
#include "../../../network/bluetooth/media/bt_media_player.h"
#endif
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Set by on_file_selected_stay(): the next explicit start plays without
 * opening the player screen (a plugin starting audio from its own screen). */
static bool stay_on_screen_once;
static char transport_skip_directory[PATH_MAX];
static int transport_skip_seconds;
static bool transport_skip_manual(int direction);

static int consecutive_decoder_failure_skips = 0;
#define MAX_FAILED_PHYSICAL_PATHS 5
static char failed_physical_paths[MAX_FAILED_PHYSICAL_PATHS][PATH_MAX];
static int failed_physical_paths_count = 0;

static uint64_t current_playback_generation = 0;

/* User playback intent and decoder failure history stay controller-owned. */
static uint64_t playback_intent_epoch = 1;
uint64_t gui_player_playback_intent_epoch(void) { return playback_intent_epoch; }
void playback_intent_bump(void) { playback_intent_epoch++; }
void player_playback_generation_reset(void) { current_playback_generation = 0; }

void reset_decoder_failure_tracking(void) {
    consecutive_decoder_failure_skips = 0;
    failed_physical_paths_count = 0;
}

static bool is_failed_physical_path(const char *path) {
    if (!path || !path[0]) return false;
    for (int i = 0; i < failed_physical_paths_count; i++)
        if (strcmp(failed_physical_paths[i], path) == 0) return true;
    return false;
}

static void record_failed_physical_path(const char *path) {
    if (!path || !path[0] || is_failed_physical_path(path) ||
        failed_physical_paths_count >= MAX_FAILED_PHYSICAL_PATHS) return;
    size_t len = strlen(path);
    if (len < sizeof(failed_physical_paths[failed_physical_paths_count])) {
        memcpy(failed_physical_paths[failed_physical_paths_count], path, len + 1);
        failed_physical_paths_count++;
    }
}

void gui_player_stop_for_external_session(void) {
    if (gui_player_has_active_track()) {
        /* A deferred resume has the authoritative checkpoint when no audio
         * is currently loaded; do not replace it with audio's idle position. */
        if (!(deferred_resume_pending && audio_is_idle())) {
            double pos = audio_get_resume_position_seconds();
            deferred_resume_position = (pos > 0.0) ? pos : 0.0;
        }
        deferred_resume_pending = true;
    }
    set_play_button_state(false);
    plugin_manager_notify_stopped();
    audio_stop();
}

/* A phone reserves output through handoff and receiver cleanup. */
static const char * external_dac_block_reason(void) {
    if (gui_shell_bt_dac_owns_output()) return TR("Pause Bluetooth audio or stop Bluetooth DAC to play music on this device");
    if (current_settings.usb_mode == USB_MODE_DAC) return TR("Exit USB DAC mode to play music on this device");
    return NULL;
}

bool gui_player_local_playback_allowed(void) {
    const char *reason = external_dac_block_reason();
    if (reason) show_error_toast(reason);
    return !reason;
}

/* Physical and touch transport share this routing; explicit play requests stay local. */
bool gui_player_transport_button(int direction) {
    if (!gui_shell_bt_dac_owns_output()) return false;
#ifndef HOST_BUILD
    bt_media_player_control_phone(direction);
#else
    (void) direction;
#endif
    return true;
}

static void play_track_at_from_internal(int index, double start_seconds, bool push_nav, bool skip_airplay_disconnect) {
    if (index < 0 || index >= player_queue_model_count()) {
        clear_selection_metadata_seed();
        return;
    }

    /* Symmetric with airplay_bridge.c's own side of this: an actual
     * incoming AirPlay stream starting stops local playback (airplay_
     * bridge_is_streaming() transitioning, see gui_network.c's gui_network_
     * poll_airplay_overlay()); starting local playback here while a stream
     * is already active should equally win and disconnect it, rather than
     * the two silently fighting over the shared output device or this call
     * failing/behaving oddly underneath a session it doesn't know about.
     * No-op when nothing is actively streaming. Must run before the block-
     * reason check below: that check no longer even considers AirPlay
     * (see external_dac_block_reason()'s own comment), but this call is
     * also what actually frees the output device before ensure_device()
     * further down tries to claim it.
     *
     * skip_airplay_disconnect exists solely for toggle_play_pause()'s own
     * resume-after-takeover path: it already called airplay_control_
     * disconnect_active_stream() itself to decide whether to resume at all,
     * and airplay_bridge_stop()/airplay_control_stop() are fire-and-forget
     * (see their own comments) -- is_streaming() can still read true for a
     * short window afterward. Calling disconnect a second time here could
     * see that stale true, and kill+respawn shairport a second time right
     * on top of the first respawn still settling. Every OTHER caller has
     * not already disconnected anything, so they must still run this call
     * normally. */
    if (!skip_airplay_disconnect) {
        airplay_control_disconnect_active_stream();
    }

    const char * block_reason = external_dac_block_reason();
    if (block_reason) {
        clear_selection_metadata_seed();
        show_error_toast(block_reason);
        return;
    }

    player_seek_input_progress_reset();
    deferred_resume_pending = false;
    deferred_resume_position = 0.0;
    player_queue_model_select_physical(index);

    const char *path = player_queue_model_path_at(index);
    track_metadata_t seed;
    bool local = seed_track_metadata(index, &seed);
    /* Present cached list metadata and begin artwork work before audio
     * startup. The fresh audio-tag request is promoted below after playback
     * has established the generation ReplayGain must belong to. */
    prepare_track_metadata(index, true, false, false, false);
    clear_selection_metadata_seed();
    track_metadata_count_play(path);
    if (local) audio_play_file_at_with_replaygain_mode(path, start_seconds, current_settings.replaygain_mode);
    else {
        bool has_gain, has_peak;
        double gain_db, peak;
        resolve_replaygain(&seed, &has_gain, &gain_db, &has_peak, &peak);
        audio_play_file_at(path, start_seconds, has_gain, gain_db, has_peak, peak);
    }
    if (push_nav) playback_intent_epoch++;
    current_playback_generation = audio_get_playback_generation();
    request_track_audio_metadata(index);
    arm_next_track_for_audio(index);

    set_play_button_state(true);
    /* Manual next/previous and a deferred startup resume can already be on
     * the player screen.  Do not stack a duplicate copy of the same screen
     * just because playback is being (re)started there. */
    if (push_nav && !stay_on_screen_once && lv_screen_active() != gui_player_get_screen()) nav_push(gui_player_get_screen());
    stay_on_screen_once = false;

    /* A remote track's stream_url can expire or be single-use -- resuming
     * into it blind on next launch can't work the way resuming a local
     * file (or even a Subsonic stream, which at least re-derives its own
     * salted URL from a stable server+song id) can. Leave last_track
     * untouched rather than saving a synthetic key that build_saved_
     * resume_playlist() has no way to turn back into a playable URL --
     * same "don't build expiring-URL-aware resume in this pass" scope
     * call as everywhere else remote tracks touch existing machinery. */
    if (path[0] && !remote_track_path_is_remote(path)) {
        snprintf(current_settings.last_track, sizeof(current_settings.last_track), "%s", path);
        current_settings.last_position = start_seconds;
    }
    settings_save_async(&current_settings);
}

void play_track_at_from(int index, double start_seconds) {
    if (!gui_player_local_playback_allowed()) return;
    reset_decoder_failure_tracking();
    play_track_at_from_internal(index, start_seconds, true, false);
}

/* Called from update_timer_cb when audio_consume_track_advanced() reports
 * the playback thread moved on to the queued next track by itself (gapless
 * handoff or a completed crossfade) -- audio is already playing it, so
 * unlike play_track_at_from() this must NOT call audio_play_file_at()
 * (that would hard-restart audio that's already mid-track). */
void on_track_auto_advanced(int index) {
    if (index < 0 || index >= player_queue_model_count()) return;

    player_seek_input_progress_reset();
    player_queue_model_select_physical(index);

    current_playback_generation = audio_get_playback_generation();
    track_metadata_count_play(playlist_path_at(index));
    request_track_metadata(index, true, false, false);
    arm_next_track_for_audio(index);

    set_play_button_state(true);

    /* See play_track_at_from()'s own comment on why a remote track's
     * synthetic key is never saved as last_track. */
    const char *advanced_path = playlist_path_at(index);
    if (advanced_path[0] && !remote_track_path_is_remote(advanced_path)) {
        snprintf(current_settings.last_track, sizeof(current_settings.last_track), "%s", advanced_path);
        current_settings.last_position = 0.0;
    }
    settings_save_async(&current_settings);
}

void play_track_at(int index) {
    play_track_at_from(index, 0.0);
}

/* on_file_selected() without opening the player screen. */
void on_file_selected_stay(char ** new_playlist, int count, int selected_index) {
    stay_on_screen_once = true;
    on_file_selected(new_playlist, count, selected_index);
    stay_on_screen_once = false;
}

void toggle_play_pause(void) {
    if (!gui_player_local_playback_allowed()) return;
    reset_decoder_failure_tracking();
    if (player_queue_model_current() < 0) return; /* nothing loaded yet */
    /* Same reasoning as play_track_at_from_internal()'s own call -- local
     * playback (resuming here) should win over an active AirPlay stream,
     * not silently fight it or no-op underneath a session it doesn't know
     * about. No-op when nothing is actively streaming. */
    if (airplay_control_disconnect_active_stream()) {
        /* AirPlay's own audio_stop() already tore down have_current back
         * when the stream first took over the output device (see airplay_
         * bridge.c's run_session()) -- audio_toggle_pause() further below
         * checks !have_current and would be a guaranteed no-op here, doing
         * nothing while set_play_button_state(false) claims it did.
         * Resume directly from the pending-aware checkpoint rather than
         * falling through to a no-op pause toggle.
         *
         * Calls play_track_at_from_internal() directly (skip_airplay_
         * disconnect=true) rather than the public play_track_at_from() --
         * that would call airplay_control_disconnect_active_stream() a
         * SECOND time. airplay_control_stop()/airplay_bridge_stop() are
         * fire-and-forget (see their own comments), so is_streaming() can
         * still read true for a short window after the call just above
         * returned; a second disconnect seeing that stale true would kill
         * and respawn shairport again right on top of the first respawn
         * still settling. reset_decoder_failure_tracking() was already
         * called at the top of this function, so skip play_track_at_from()'s
         * own redundant call to it too. */
        play_track_at_from_internal(player_queue_model_current(), audio_get_resume_position_seconds(), true, true);
        return;
    }
    if (deferred_resume_pending) {
        double start_seconds = deferred_resume_position;
        deferred_resume_pending = false;
        deferred_resume_position = 0.0;
        play_track_at_from(player_queue_model_current(), start_seconds);
        return;
    }
    /* Block local resume while a receiver owns output. Pausing stays available. */
    if (!audio_is_playing()) {
        const char * block_reason = external_dac_block_reason();
        if (block_reason) {
            show_error_toast(block_reason);
            return;
        }
    }
    audio_toggle_pause();
    bool now_playing = audio_is_playing();
    set_play_button_state(now_playing);

    if (now_playing) {
        plugin_manager_notify_resumed();
    } else {
        /* Checkpoint the resume position on pause -- a natural point to
         * persist, and far less write-heavy than saving on every tick.
         * Also urgently (re)writes the queue-checkpoint file (the file
         * startup restore actually reads for SD/internal tracks, separate
         * from settings.txt's own last_position field), so a pause
         * immediately followed by any kind of reboot does not resume from
         * a stale pre-pause position. Keep this settings save synchronous:
         * the urgent checkpoint is part of the same reboot-sensitive pause
         * persistence boundary. */
        current_settings.last_position = audio_get_resume_position_seconds();
        settings_save(&current_settings);
        gui_player_queue_checkpoint_urgent();
        plugin_manager_notify_paused();
    }
}

void play_btn_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!gui_player_transport_button(0)) toggle_play_pause();
}

/* Standard CD-player/iPod convention: a tap partway into a track restarts
 * it; only a tap already near the start (first press rewinds, second press
 * goes to previous song) moves to the actual previous track -- no separate
 * double-tap timer needed, since "already near the start" is naturally true
 * right after the first tap rewound it. */
#define PREV_BUTTON_REWIND_THRESHOLD_SECONDS 3.0
void prev_btn_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    /* Consume LVGL's CLICKED after a long press; PRESS_LOST cleanup is in the seek controller. */
    if (player_seek_input_consume_transport_click(-1)) return;
    if (gui_player_transport_button(-1)) return;
    if (!gui_player_local_playback_allowed()) return;
    reset_decoder_failure_tracking();
    if (player_queue_model_current() < 0) return;
    if (transport_skip_manual(-1)) return;
    if (audio_get_position_seconds() > PREV_BUTTON_REWIND_THRESHOLD_SECONDS) {
        audio_seek(0.0);
        return;
    }
    gui_player_step_manual(-1);
}

void next_btn_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (player_seek_input_consume_transport_click(1)) return;
    if (!gui_player_transport_button(1)) gui_player_step_manual(1);
}

/* The one place crossfade is applied, for both entry points (the Settings
 * switch below and the quick drawer's own tile) -- the gapless implication
 * here is easy to apply in one and forget in the other.
 *
 * Crossfade blends into an ARMED next track, so it cannot engage at all
 * while gapless is off, because then nothing gets armed (see
 * arm_next_track_for_audio()). Enabling crossfade therefore switches
 * gapless back on rather than leaving a control that reads On and silently
 * does nothing. */
void gui_player_set_crossfade_enabled(bool enabled) {
    current_settings.crossfade_enabled = enabled;
    if (enabled) current_settings.gapless_enabled = true;
    audio_set_crossfade_enabled(enabled);
    settings_save_async(&current_settings);
    /* Apply to the next transition, not the one after it. */
    if (gui_player_has_active_track()) arm_next_track_for_audio(player_queue_model_current());
    refresh_quick_drawer_crossfade_icon(); /* see its own comment -- keeps the drawer icon in sync */
    /* Both of gapless's own surfaces, since enabling crossfade may have just
     * switched it back on. Done here rather than in each caller so no entry
     * point can forget one of them. */
    gui_settings_sync_gapless_toggle();
    gui_shell_refresh_quick_drawer_expansion_toggles();
}

void crossfade_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    gui_player_set_crossfade_enabled(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

/* gui_player_set_crossfade_enabled()'s counterpart, and for the same reason:
 * two entry points (the Settings switch and the quick drawer tile) share
 * one rule, so it cannot be applied in one and missed in the other.
 *
 * Turning gapless off turns crossfade off with it -- crossfade has nothing
 * to blend into once no next track is armed. */
void gui_player_set_gapless_enabled(bool enabled) {
    current_settings.gapless_enabled = enabled;
    if (!enabled && current_settings.crossfade_enabled) {
        /* Sets crossfade false and saves; deliberately called before this
         * function's own save so the two flags persist together. */
        gui_player_set_crossfade_enabled(false);
        gui_settings_sync_crossfade_toggle();
    }
    settings_save_async(&current_settings);
    /* Apply to the next transition, not the one after it: an already-armed
     * next track would otherwise still hand off gaplessly once more. */
    if (gui_player_has_active_track()) arm_next_track_for_audio(player_queue_model_current());
    gui_settings_sync_gapless_toggle();
    gui_shell_refresh_quick_drawer_expansion_toggles();
}

void gui_player_set_replaygain_mode(int mode) {
    if (mode < 0 || mode > 2 || current_settings.replaygain_mode == mode) return;
    current_settings.replaygain_mode = mode;
    settings_save_async(&current_settings);
    /* The already prepared successor captured the old gain mode. Rebuild it
     * now so the new setting takes effect at the next transition. */
    if (player_queue_model_current() >= 0) arm_next_track_for_audio(player_queue_model_current());
}

void gapless_switch_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    gui_player_set_gapless_enabled(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

void gui_player_poll_confirmed_playback(void) {
    if (consecutive_decoder_failure_skips > 0 && audio_is_playing()) {
        if (audio_get_position_seconds() >= 3.0) {
            reset_decoder_failure_tracking();
        }
    }
}

void gui_player_step_manual(int direction) {
    if (!gui_player_local_playback_allowed()) return;
    if (transport_skip_manual(direction)) return;
    reset_decoder_failure_tracking();
    if (player_queue_model_current() < 0) return;
    int next_idx = compute_manual_step_index(player_queue_model_current(), direction);
    if (next_idx >= 0) play_track_at(next_idx);
    else plugin_manager_notify_queue_exhausted(direction);
}

void gui_player_set_transport_skip(const char * directory, int seconds) {
    snprintf(transport_skip_directory, sizeof(transport_skip_directory), "%s", directory ? directory : "");
    size_t n = strlen(transport_skip_directory);
    while (n > 1 && transport_skip_directory[n - 1] == '/') transport_skip_directory[--n] = '\0';
    transport_skip_seconds = seconds;
}

static bool transport_skip_manual(int direction) {
    const char * path = gui_player_get_current_track_path();
    size_t n = strlen(transport_skip_directory);
    if (!transport_skip_seconds || !n || strncmp(path, transport_skip_directory, n) != 0 ||
        path[n] != '/' || (!audio_is_playing() && !audio_is_paused())) return false;
    /* Consume a press during decoder startup as well, so it cannot change files. */
    double duration = audio_get_duration_seconds();
    double position = audio_get_resume_position_seconds();
    if (!isfinite(duration) || duration <= 0 || !isfinite(position) || deferred_resume_pending) return true;
    double target = position + (direction < 0 ? -transport_skip_seconds : transport_skip_seconds);
    if (target < 0) target = 0;
    double max_target = duration > TRANSPORT_SEEK_EOF_GUARD_SECONDS ? duration - TRANSPORT_SEEK_EOF_GUARD_SECONDS : 0;
    if (target > max_target) target = max_target;
    audio_seek(target);
    return true;
}

void gui_player_handle_auto_advance(void) {
    if (player_queue_model_current() < 0) return;
    reset_decoder_failure_tracking();
    int advanced_index = compute_auto_advance_index(player_queue_model_current());
    if (advanced_index >= 0) {
        commit_auto_advance();
        on_track_auto_advanced(advanced_index);
    }
}

void gui_player_handle_track_finished(void) {
    if (player_queue_model_current() < 0) return;
    reset_decoder_failure_tracking();
    int finished_index = compute_auto_advance_index(player_queue_model_current());
    if (finished_index >= 0) {
        commit_auto_advance();
        play_track_at_from_internal(finished_index, 0.0, false, false);
    } else {
        set_play_button_state(false);
    }
}

static void stop_after_playback_failure(const char * error_text) {
    char message[384];
    set_play_button_state(false);
    deferred_resume_pending = false;
    deferred_resume_position = 0.0;
    reset_decoder_failure_tracking();
    snprintf(message, sizeof(message), TR("Playback stopped: %s"), error_text);
    show_error_toast(message);
}

void gui_player_handle_playback_error_ex(audio_error_t err, uint64_t err_generation) {
    if (err == AUDIO_ERROR_NONE) return;
    if (player_queue_model_current() < 0) return;

    /* Stale-vs-race generation check: only reject an error OLDER than what
     * this function has last synced to -- playback_generation only ever
     * increases, so err_generation < current_playback_generation
     * unambiguously means this error is about a track already moved past.
     *
     * A plain != check is wrong here: the audio thread also bumps
     * playback_generation on its OWN via a crossfade/gapless promotion
     * (audio.c's track_advanced=true sites), which the GUI only learns
     * about by consuming audio_consume_track_advanced() -- and gui.c's
     * poll checks for an error FIRST, in a single if/else-if chain, so
     * that consume never runs this tick if an error is also pending. If
     * the newly-promoted track then fails within the same ~500ms poll
     * gap (easily possible -- the audio thread runs far more iterations
     * than that per poll), the error's generation is NEWER than what this
     * function has seen, not stale -- but a plain != check can't tell
     * "newer" from "older" and would silently drop a real failure while
     * audio_consume_error_ex() has already cleared it for good, leaving
     * the UI believing a track that never actually played is still
     * playing. Catch up to that pending promotion first (the same call
     * gui.c's own else-if branch would have made this tick, had the error
     * not taken priority) so the failure is attributed to the track it
     * actually happened on. */
    if (err_generation != 0 && current_playback_generation != 0 &&
        err_generation != current_playback_generation) {
        if (err_generation < current_playback_generation) return;
        if (!audio_consume_track_advanced()) return;
        gui_player_handle_auto_advance();
        if (current_playback_generation != err_generation) return;
    }

    if (err == AUDIO_ERROR_OUTPUT_FAILED) {
        /* Checkpoint confirmed position so pressing Play can retry cleanly */
        double pos = audio_get_resume_position_seconds();
        deferred_resume_position = (pos > 0.0) ? pos : 0.0;
        deferred_resume_pending = true;
        set_play_button_state(false);
        show_error_toast(TR("Playback error: audio output failed"));
        return;
    }

    if (err == AUDIO_ERROR_DECODER_FAILED || err == AUDIO_ERROR_FILE_UNAVAILABLE ||
        err == AUDIO_ERROR_UNSUPPORTED_FORMAT || err == AUDIO_ERROR_UNSUPPORTED_CHANNELS) {
        char failure_message[384];
        /* Record the failed track's physical path */
        const char * failed_path = gui_player_get_current_track_path();
        if (failed_path) record_failed_physical_path(failed_path);
        char remote_error[256] = {0};
        const char * error_text = TR(audio_error_description(err));
        if (failed_path && remote_track_path_is_remote(failed_path) &&
            remote_track_stream_error_for_path(failed_path, err_generation,
                                                remote_error, sizeof(remote_error)) &&
            remote_error[0]) error_text = TR(remote_error);

        consecutive_decoder_failure_skips++;
        int queue_count = player_queue_model_count();
        int max_skips = (queue_count < 5) ? queue_count : 5;
        if (consecutive_decoder_failure_skips >= max_skips) {
            stop_after_playback_failure(error_text);
            return;
        }

        const char *failed_paths[MAX_FAILED_PHYSICAL_PATHS];
        for (int i = 0; i < failed_physical_paths_count; i++)
            failed_paths[i] = failed_physical_paths[i];
        int target_index = player_queue_model_advance_past_failed(
            current_settings.play_mode, failed_paths, failed_physical_paths_count);

        if (target_index < 0) {
            stop_after_playback_failure(error_text);
            return;
        }

        deferred_resume_pending = false;
        deferred_resume_position = 0.0;
        snprintf(failure_message, sizeof(failure_message), TR("Skipped: %s"), error_text);
        show_error_toast(failure_message);
        play_track_at_from_internal(target_index, 0.0, false, false);
    }
}
