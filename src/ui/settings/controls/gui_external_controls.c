#include "gui_external_controls.h"
#include <stdio.h>
#include <stdint.h>
#include "../../player/screen/gui_player.h"
#include "../../library/browse/gui_library.h"
#include "../../shell/gui_shell.h"
#include "../../appearance/gui_theme.h"
#include "../../shell/gui_notifications.h"
#include "../../../core/persistence/settings.h"
#include "../../../audio/engine/audio.h"
#include "../../../library/metadata/db/metadata_db.h"
#include "../../../network/remote/control/remote_control.h"
#include "../../../hardware/input/hw_buttons.h"
#include "../../../core/system/lifecycle/screenshot.h"
#include "../../../plugins/manager/plugin_manager.h"
#include "../../text/i18n/i18n.h"
#ifndef HOST_BUILD
#include "../../../network/bluetooth/media/bt_media_player.h"
#include "../../../network/bluetooth/media/bt_cover_art.h"
#endif

#ifndef HOST_BUILD
static void poll_bluetooth_controls(void) {
    /* Bluetooth accessory controls dispatch to UI/player state on this thread. */
    if (bt_media_player_consume_play_pause()) {
        toggle_play_pause();
    }
    if (bt_media_player_consume_next()) {
        gui_player_step_manual(1);
    }
    if (bt_media_player_consume_prev()) {
        gui_player_step_manual(-1);
    }
    /* Keeps PlaybackStatus accurate for whenever BlueZ/the accessory next
     * queries it -- cheap (no subprocess, just a mutex-protected bool),
     * safe to do unconditionally every tick rather than hunting down every
     * call site that can change play state. */
    bt_media_player_notify_playback_state(audio_is_playing());
    bool has_track = gui_player_has_active_track();
    char bt_art_url[256] = "";
    if (has_track) bt_cover_art_get_url(bt_art_url, sizeof(bt_art_url));
    else bt_cover_art_begin_track(NULL);
    bt_media_player_notify_track(has_track ? gui_player_get_now_playing_title() : "",
                                 has_track ? gui_player_get_now_playing_folder() : "",
                                 has_track ? gui_player_get_now_playing_album() : "",
                                 has_track ? gui_player_get_now_playing_genre() : "",
                                 has_track ? gui_player_get_now_playing_track_number() : 0,
                                 audio_get_position_seconds(), audio_get_duration_seconds(), bt_art_url);
}
#endif

static void poll_screenshot_completion(bool screenshot_request_pending) {
    /* Phone remote-control (Phase 2, remote_control.h): same "background
     * thread sets a flag, this is the one thread allowed to touch LVGL/
     * audio state" pattern as hw_buttons/bt_media_player just above --
     * reuses the exact same shuffle-aware stepping and volume-persistence
     * shape as those, rather than a separate implementation. */
    screenshot_request_pending = remote_control_consume_screenshot() || screenshot_request_pending;
    if (screenshot_request_pending) (void) screenshot_start();
    screenshot_completion_t screenshot_result;
    if (screenshot_poll_completion(&screenshot_result)) {
        if (screenshot_result.saved) {
            show_info_toast(TR("Screenshot saved"));
            plugin_manager_notify_screenshot_saved(screenshot_result.path);
        } else {
            char failed_msg[128];
            snprintf(failed_msg, sizeof(failed_msg), TR("Screenshot failed (%s)"),
                     screenshot_result.reason[0] ? screenshot_result.reason : TR("unknown"));
            show_error_toast(failed_msg);
            plugin_manager_notify_screenshot_failed(screenshot_result.reason);
        }
    }
}

static void poll_remote_playback_commands(void) {
    if (remote_control_consume_play_pause()) {
        toggle_play_pause();
    }
    if (remote_control_consume_next()) {
        gui_player_step_manual(1);
    }
    if (remote_control_consume_prev()) {
        gui_player_step_manual(-1);
    }
    if (remote_control_consume_mode_cycle()) {
        cycle_play_mode();
    }
    int remote_play_mode;
    if (remote_control_consume_play_mode(&remote_play_mode)) {
        gui_player_set_play_mode(remote_play_mode);
    }
}

static void poll_remote_library_commands(void) {
    int remote_sort_scope, remote_sort_mode;
    if (remote_control_consume_library_sorting(&remote_sort_scope, &remote_sort_mode)) {
        int *modes[] = { &current_settings.album_sort_mode, &current_settings.artist_albums_sort_mode,
                        &current_settings.album_artist_albums_sort_mode };
        const gui_library_sort_scope_t scopes[] = { GUI_LIBRARY_SORT_ALBUMS, GUI_LIBRARY_SORT_ARTIST_ALBUMS,
                                                    GUI_LIBRARY_SORT_ALBUM_ARTIST_ALBUMS };
        if (remote_sort_scope >= 0 && remote_sort_scope < 3 && remote_sort_mode >= 0 && remote_sort_mode <= 2) {
            *modes[remote_sort_scope] = remote_sort_mode;
            gui_library_apply_sorting(scopes[remote_sort_scope]);
            settings_save_async(&current_settings);
        }
    }
    remote_collection_play_request_t remote_collection;
    if (remote_control_consume_collection(&remote_collection) &&
        !gui_library_play_remote_collection(&remote_collection)) {
        remote_control_finish_collection(false, "Cannot prepare collection");
    }
}

static void poll_remote_position_volume(void) {
    int remote_seek_seconds;
    if (remote_control_consume_seek(&remote_seek_seconds)) {
        audio_seek((double) remote_seek_seconds);
    }
    int remote_volume_percent;
    if (remote_control_consume_volume(&remote_volume_percent) &&
        !gui_player_volume_is_being_adjusted()) {
        remote_volume_percent = audio_clamp_volume_percent(remote_volume_percent);
        gui_player_set_volume_percent(remote_volume_percent);
        audio_set_volume((float) remote_volume_percent / 100.0f);
        gui_player_remember_volume_percent(remote_volume_percent);
        settings_save_async(&current_settings);
        show_volume_popup(remote_volume_percent);
        refresh_volume_topbar(remote_volume_percent);
    }
}

static void poll_remote_queue_commands(void) {
    int64_t remote_queue_id;
    char remote_queue_catalog_revision[METADATA_DB_CATALOG_REVISION_SIZE] = {0};
    if (remote_control_consume_queue_index(&remote_queue_id, remote_queue_catalog_revision,
                                            sizeof(remote_queue_catalog_revision))) {
        song_row_t remote_queue_row;
        bool found = remote_queue_catalog_revision[0]
            ? metadata_db_catalog_get_song_revision(remote_queue_catalog_revision, remote_queue_id,
                                                    &remote_queue_row) == METADATA_DB_CATALOG_OK
            : metadata_db_get_song_by_id(remote_queue_id, &remote_queue_row);
        if (found) queue_add_song(remote_queue_row.path);
    }
    int remote_queue_remove_offset;
    uint64_t remote_queue_revision;
    if (remote_control_consume_queue_remove(&remote_queue_remove_offset, &remote_queue_revision))
        gui_player_remote_queue_remove(remote_queue_remove_offset, remote_queue_revision);
    if (remote_control_consume_queue_clear(&remote_queue_revision))
        gui_player_remote_queue_clear(remote_queue_revision);
    int remote_queue_from, remote_queue_to;
    if (remote_control_consume_queue_move(&remote_queue_from, &remote_queue_to, &remote_queue_revision))
        gui_player_remote_queue_move(remote_queue_from, remote_queue_to, remote_queue_revision);
    int remote_queue_play_offset;
    if (remote_control_consume_queue_play(&remote_queue_play_offset, &remote_queue_revision))
        gui_player_remote_queue_play(remote_queue_play_offset, remote_queue_revision);
    if (remote_control_consume_favorite_changed()) gui_player_refresh_favorite();
    char remote_folder_track[REMOTE_CONTROL_PATH_MAX];
    if (remote_control_consume_folder_play(remote_folder_track, sizeof(remote_folder_track)) &&
        !gui_player_play_folder_track(remote_folder_track))
        show_error_toast(TR("Cannot play folder"));
}

static void poll_remote_track(void) {
    int64_t remote_play_id;
    char remote_play_playlist[128], remote_play_artist[128], remote_play_album_artist[128], remote_play_album[128];
    char remote_play_catalog_revision[METADATA_DB_CATALOG_REVISION_SIZE] = {0};
    if (remote_control_consume_play_index(&remote_play_id, remote_play_playlist, sizeof(remote_play_playlist),
                                           remote_play_artist, sizeof(remote_play_artist), remote_play_album_artist,
                                           sizeof(remote_play_album_artist), remote_play_album,
                                           sizeof(remote_play_album), remote_play_catalog_revision,
                                           sizeof(remote_play_catalog_revision))) {
        /* remote_play_id is a song id (metadata_db.c's rowid-based
         * song_row_t.id) -- resolve it to a path, then hand that straight to
         * play_remote_control_song() (defined with the rest of the
         * remote-control scoped-play machinery, much further down), which
         * resolves the path plus the playlist/artist/album context into the
         * right playlist and position via its own DB queries, falling back
         * to the whole library (All Songs, by title offset) when no scope
         * applies. */
        song_row_t remote_play_row;
        bool found = remote_play_catalog_revision[0]
            ? metadata_db_catalog_get_song_revision(remote_play_catalog_revision, remote_play_id,
                                                    &remote_play_row) == METADATA_DB_CATALOG_OK
            : metadata_db_get_song_by_id(remote_play_id, &remote_play_row);
        if (found) {
            play_remote_control_song(remote_play_row.path, remote_play_playlist, remote_play_artist,
                                      remote_play_album_artist, remote_play_album);
        }
    }
}

void gui_external_controls_poll(void) {
    bool screenshot_request_pending = hw_buttons_consume_screenshot();
#ifndef HOST_BUILD
    poll_bluetooth_controls();
#endif
    poll_screenshot_completion(screenshot_request_pending);
    poll_remote_playback_commands();
    poll_remote_library_commands();
    poll_remote_position_volume();
    poll_remote_queue_commands();
    poll_remote_track();
}

void gui_external_controls_publish_status(bool favorite) {
    /* No separate now-playing metadata cache exists in this app beyond
     * what's already on screen -- song_title_label/artist_label
     * are this app's own single source of truth for title/artist (see
     * apply_track_metadata_to_ui()), so read them back rather than
     * standing up a second copy of the same state just for this.
     * album_label is read back the same way below. */
    const char * now_playing_path = gui_player_has_active_track()
            ? gui_player_get_current_track_path()
            : NULL;
    remote_control_notify_status(audio_is_playing(), audio_is_paused(), gui_player_get_now_playing_title(),
                                  gui_player_get_now_playing_folder(), gui_player_get_now_playing_album(), now_playing_path,
                                  (int) audio_get_position_seconds(), (int) audio_get_duration_seconds(),
                                  audio_get_volume(), current_settings.play_mode,
                                  now_playing_path && favorite, gui_theme_accent_rgb());
    remote_control_notify_library_sorting(current_settings.album_sort_mode,
                                          current_settings.artist_albums_sort_mode,
                                          current_settings.album_artist_albums_sort_mode);
}
