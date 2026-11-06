#include "../../application/gui.h"
#include "../../text/i18n/i18n.h"
#include "gui_subsonic.h"
#include "gui_subsonic_album.h"
#include "gui_subsonic_internal.h"
#include "../../../core/persistence/settings.h"
#include "../../../library/remote/subsonic/subsonic_cache.h"
#include "../../../network/services/subsonic_client.h"
#include "../../widgets/builders/screen_builders.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <sys/stat.h>

extern player_settings_t current_settings;

subsonic_stream_song_meta_t * subsonic_stream_meta = NULL; /* parallel array, NULL when no Subsonic stream queue is loaded */
int subsonic_stream_meta_count = 0;

typedef enum { SUBSONIC_DOWNLOAD_PENDING_NONE, SUBSONIC_DOWNLOAD_PENDING_SONGS, SUBSONIC_DOWNLOAD_PENDING_ARTIST } subsonic_download_pending_t;

static void subsonic_fill_stream_queue_entry(const subsonic_server_t * server, const subsonic_song_t * song,
                                              const char * cover_id, int quality_kbps, char ** new_playlist,
                                              subsonic_stream_song_meta_t * new_meta, int slot) {
    char url[1536];
    subsonic_build_stream_url_quality(server, song->id, quality_kbps, url, sizeof(url));
    /* The "#.<suffix>" appended here is a local-only hint consumed by
     * audio_decoder_open.c's decoder_open() (stream_format_hint()); http_conn_parse_url()
     * strips it before it ever reaches the actual HTTP request, so it has
     * no effect on the server-facing URL. */
    size_t len = strlen(url);
    const char * stream_suffix = quality_kbps > 0 ? "mp3" : song->suffix;
    snprintf(url + len, sizeof(url) - len, "#.%s", stream_suffix);

    new_playlist[slot] = strdup(url);

    subsonic_stream_song_meta_t * m = &new_meta[slot];
    snprintf(m->song_id, sizeof(m->song_id), "%s", song->id);
    snprintf(m->url, sizeof(m->url), "%s", url);
    snprintf(m->title, sizeof(m->title), "%s", song->title);
    snprintf(m->artist, sizeof(m->artist), "%s", song->artist);
    snprintf(m->album, sizeof(m->album), "%s", song->album);
    snprintf(m->suffix, sizeof(m->suffix), "%s", stream_suffix);
    m->track = song->track;
    m->disc = song->disc;
    m->duration_seconds = song->duration_seconds;
    /* Source format metadata describes the library file, not a transcoded MP3. */
    m->sample_rate = quality_kbps > 0 ? 0 : song->sample_rate;
    m->bit_depth = quality_kbps > 0 ? 0 : song->bit_depth;
    m->channels = quality_kbps > 0 ? 0 : song->channels;
    m->bitrate_kbps = quality_kbps > 0 ? 0 : song->bitrate_kbps;
    /* cover_id is the album's own cover, or NULL for the song's. Never truncated. */
    const char * art = cover_id ? cover_id : song->cover_art;
    snprintf(m->cover_art, sizeof(m->cover_art), "%s", strlen(art) < sizeof(m->cover_art) ? art : "");
    subsonic_cache_server_key(server, m->server_key);
}

void subsonic_songs_album_info(const subsonic_server_t * server, gui_subsonic_album_info_t * info);

static int subsonic_stream_quality_kbps(void) {
    return current_settings.subsonic_stream_quality == 1 ? 96 :
           current_settings.subsonic_stream_quality == 2 ? 192 :
           current_settings.subsonic_stream_quality == 3 ? 320 : 0;
}

static bool subsonic_streams_directly(const subsonic_song_t * song, int quality_kbps) {
    return quality_kbps > 0 || strcasecmp(song->suffix, "mp3") == 0 || strcasecmp(song->suffix, "flac") == 0;
}

static void subsonic_start_stream_queue(const subsonic_server_t * server, int index, int quality_kbps) {
    char ** new_playlist = malloc(sizeof(char *) * (size_t) subsonic_songs_count);
    subsonic_stream_song_meta_t * new_meta = malloc(sizeof(subsonic_stream_song_meta_t) * (size_t) subsonic_songs_count);
    int count = 0;
    int start_index = -1;
    /* An album's songs share its cover (Navidrome gives each file its own id);
     * a playlist mixes albums, so each song keeps its own. */
    gui_subsonic_album_info_t album = {0};
    bool is_album = subsonic_songs_kind == SUBSONIC_LIST_ALBUM_SONGS;
    if (is_album) subsonic_songs_album_info(server, &album);

    for (int i = 0; i < subsonic_songs_count; i++) {
        subsonic_song_t * s = &subsonic_songs_cache[i];
        if (!subsonic_streams_directly(s, quality_kbps)) continue;
        subsonic_fill_stream_queue_entry(server, s, is_album ? album.cover_id : NULL, quality_kbps, new_playlist, new_meta, count);
        if (i == index) start_index = count;
        count++;
    }

    if (start_index < 0) {
        /* Shouldn't happen -- the tapped song itself was mp3/flac, so it
         * must have been included above -- but fail safely rather than
         * play the wrong track if this invariant is ever violated. */
        for (int i = 0; i < count; i++) free(new_playlist[i]);
        free(new_playlist);
        free(new_meta);
        return;
    }

    free(subsonic_stream_meta);
    subsonic_stream_meta = new_meta;
    subsonic_stream_meta_count = count;

    clear_player_source(); /* a streamed queue has no on-device list to go back to */
    on_file_selected(new_playlist, count, start_index);
}

static void subsonic_download_then_play(const subsonic_server_t * server, const subsonic_song_t * song) {
    char url[1536];
    subsonic_build_stream_url(server, song->id, url, sizeof(url));
    mkdir(SUBSONIC_STREAM_CACHE_DIR, 0755); /* no-op (EEXIST) if it's already there */
    char dest[256];
    snprintf(dest, sizeof(dest), SUBSONIC_STREAM_CACHE_DIR "/stream.%s", song->suffix);

    gui_subsonic_download_track(url, server->verify_tls, dest, song->title);
}

/* mp3/flac plays directly off the stream URL -- no download, no wait --
 * and queues the rest of this album/playlist (subsonic_songs_cache is
 * already the full song list either way, see subsonic_songs_context_is_
 * playlist's own comment above) that's ALSO mp3/flac, in original order,
 * starting at the tapped song, so Prev/Next/auto-advance/Repeat/Shuffle
 * all work across the whole thing exactly like a local-library playlist.
 * A song in some other format is simply left out of this queue -- there's
 * no good way to background-download it without either stalling the
 * queue right there or building a much bigger hybrid stream+download
 * pipeline, and skipping it keeps every other song in the album/playlist
 * reachable instead of the queue silently dead-ending on it. See this
 * section's own top comment for why a non-streamable format tapped
 * directly still downloads first, as a single track, same as before. */
static void subsonic_play_song_and_queue_rest(int index) {
    if (!gui_subsonic_wifi_connected_guard()) return; /* cached lists open offline, playing needs the server */
    subsonic_song_t * song = &subsonic_songs_cache[index];
    subsonic_server_t server = subsonic_server_from_settings();
    int quality_kbps = subsonic_stream_quality_kbps();
    if (subsonic_streams_directly(song, quality_kbps)) {
        subsonic_start_stream_queue(&server, index, quality_kbps);
        return;
    }
    subsonic_download_then_play(&server, song);
}

void gui_subsonic_play_from(int index) {
    if (index < 0 || index >= subsonic_songs_count) return;
    subsonic_play_song_and_queue_rest(index);
}

static subsonic_download_pending_t subsonic_download_pending = SUBSONIC_DOWNLOAD_PENDING_NONE;

static gui_popup_t subsonic_download_confirm_popup;

static lv_obj_t * subsonic_download_confirm_title;

static void hide_subsonic_download_confirm_popup(void) {
    gui_popup_hide(&subsonic_download_confirm_popup);
    subsonic_download_pending = SUBSONIC_DOWNLOAD_PENDING_NONE;
}

static void subsonic_download_confirm_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_subsonic_download_confirm_popup();
}

static void subsonic_download_confirm_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_subsonic_download_confirm_popup();
}

static void show_subsonic_download_confirm_popup(subsonic_download_pending_t kind, const char * msg) {
    subsonic_download_pending = kind;
    lv_label_set_text(subsonic_download_confirm_title, msg);
    gui_popup_show(&subsonic_download_confirm_popup);
}

static void subsonic_download_songs_now(void) {
    if (subsonic_songs_count == 0) return;

    subsonic_song_t * songs_copy = malloc(sizeof(subsonic_song_t) * (size_t) subsonic_songs_count);
    if (!songs_copy) { show_error_toast(TR("Not enough memory to start download")); return; }
    memcpy(songs_copy, subsonic_songs_cache, sizeof(subsonic_song_t) * (size_t) subsonic_songs_count);

    const char * playlist_name = subsonic_songs_context_is_playlist ? subsonic_songs_context_playlist_name : NULL;
    char label[192];
    snprintf(label, sizeof(label), TR("Downloading\n%s..."), lv_label_get_text(subsonic_songs_title_label));
    gui_subsonic_download_library(songs_copy, subsonic_songs_count, NULL, 0, playlist_name, label);
}

void gui_subsonic_download_songs_btn_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!gui_subsonic_wifi_connected_guard()) return;
    if (subsonic_songs_count == 0) return;

    char msg[224];
    snprintf(msg, sizeof(msg), TR("Download \"%s\"?"), lv_label_get_text(subsonic_songs_title_label));
    show_subsonic_download_confirm_popup(SUBSONIC_DOWNLOAD_PENDING_SONGS, msg);
}

static void subsonic_download_artist_now(void) {
    if (subsonic_albums_context_artist[0] == '\0' || subsonic_albums_count == 0) return;

    char ** album_ids = gui_subsonic_copy_album_ids(subsonic_albums_cache, subsonic_albums_count);
    if (!album_ids) { show_error_toast(TR("Not enough memory to start download")); return; }

    char label[192];
    snprintf(label, sizeof(label), TR("Downloading\n%s..."), subsonic_albums_context_artist);
    gui_subsonic_download_library(NULL, 0, album_ids, subsonic_albums_count, NULL, label);
}

void gui_subsonic_download_artist_btn_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!gui_subsonic_wifi_connected_guard()) return;
    if (subsonic_albums_context_artist[0] == '\0' || subsonic_albums_count == 0) return;

    char msg[224];
    snprintf(msg, sizeof(msg), TR("Download every album from \"%s\"?"), subsonic_albums_context_artist);
    show_subsonic_download_confirm_popup(SUBSONIC_DOWNLOAD_PENDING_ARTIST, msg);
}

static void subsonic_download_confirm_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    subsonic_download_pending_t kind = subsonic_download_pending;
    hide_subsonic_download_confirm_popup();
    if (!gui_subsonic_wifi_connected_guard()) return; /* Wi-Fi may have dropped while the popup was open */
    if (kind == SUBSONIC_DOWNLOAD_PENDING_SONGS) subsonic_download_songs_now();
    else if (kind == SUBSONIC_DOWNLOAD_PENDING_ARTIST) subsonic_download_artist_now();
}

void gui_subsonic_download_confirm_build(void) {
    subsonic_download_confirm_popup.popup = build_confirm_popup(
        "", LV_LABEL_LONG_WRAP, &subsonic_download_confirm_title, NULL, TR("Download"), accent_lv_color(),
        subsonic_download_confirm_cb, NULL, TR("Cancel"), lv_color_make(160, 160, 160), subsonic_download_confirm_cancel_cb,
        NULL, subsonic_download_confirm_backdrop_cb, &subsonic_download_confirm_popup.backdrop);
}


void gui_subsonic_download_confirm_teardown(void) {
    gui_popup_teardown(&subsonic_download_confirm_popup);
}
