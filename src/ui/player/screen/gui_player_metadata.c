#include "gui_player.h"
#include "gui_player_internal.h"
#include "gui_lyrics.h"
#include "gui_track_info.h"
#include "../queue/player_queue_model.h"
#include "../artwork/player_artwork_cache.h"
#include "../artwork/player_artwork_jobs.h"
#include "../artwork/player_cover.h"
#include "../../application/gui.h"
#include "../../settings/gui_settings.h"
#include "../../settings/lock/gui_lock_screen.h"
#include "../../shell/gui_notifications.h"
#include "../../shell/gui_shell.h"
#include "../../text/i18n/i18n.h"
#include "../../assets/assets.h"
#include "../../../audio/engine/audio.h"
#include "../../../core/persistence/settings.h"
#include "../../../library/artwork/prefetch/player_cover_prefetch.h"
#include "../../../library/metadata/db/metadata_db.h"
#include "../../../library/metadata/parsing/metadata.h"
#include "../../../library/playlists/favorite_writer.h"
#include "../../../library/playlists/playback_order.h"
#include "../../../library/remote/remote_track.h"
#include "../../../library/workers/track_metadata_worker.h"
#include "../../../network/bluetooth/media/bt_cover_art.h"
#include "../../../network/hiby/hiby_sys_server.h"
#include "../../../plugins/manager/plugin_manager.h"
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char now_playing_genre[128];
static int now_playing_track_number;
extern bool plugin_now_playing_loaded;
static uint64_t metadata_ui_token, metadata_next_token, metadata_next_queue_revision;
static uint64_t artwork_dispatched_metadata_token;
static uint64_t metadata_scalar_applied_token;
static char artwork_dispatched_artist[128], artwork_dispatched_album[128], artwork_dispatched_album_artist[128];
#ifdef UI_PERF_TRACE
static uint64_t metadata_request_perf_us;
#endif
static uint64_t metadata_ui_audio_generation, metadata_next_audio_generation;
static uint64_t favorite_ui_revision, metadata_favorite_revision;
static uint64_t favorite_request_token, favorite_request_revision;
static char favorite_request_path[PATH_MAX];
static bool favorite_track_known, favorite_toggle_pending;
static bool metadata_ui_notify, metadata_notify_ready;
static track_metadata_t metadata_notify_payload;
static bool selection_metadata_seed_valid;
static char selection_metadata_seed_path[PATH_MAX];
static track_metadata_t selection_metadata_seed;
static char metadata_ui_path[PATH_MAX], metadata_next_current_path[PATH_MAX];
static int metadata_next_gain_mode;
static bool metadata_next_gapless_enabled;
static char armed_next_path[PATH_MAX];
static uint64_t armed_next_generation;
static int armed_next_gain_mode;
static bool next_metadata_cached;
static char next_metadata_cached_path[PATH_MAX];
static track_metadata_t next_metadata_cache;
static uint64_t next_cover_prefetch_token;
static void notify_plugin_track_started(const track_metadata_t * meta, const char * path, double duration);
static track_metadata_request_t metadata_ui_request;

void player_reset_now_playing_extras(void) {
    now_playing_genre[0] = '\0';
    now_playing_track_number = 0;
}

/* NEXT tags are validated by the metadata poll. Start speculative work
 * only after current artwork has settled; it must never race the initial
 * foreground decode for the artwork coordinator or SD bandwidth. */
void maybe_prefetch_next_cover(void) {
    if (!gui_player_get_screen() || !next_metadata_cached || !metadata_ui_token ||
        artwork_dispatched_metadata_token != metadata_ui_token ||
        player_artwork_jobs_active() || player_artwork_jobs_pending() ||
        player_artwork_jobs_reload_active() ||
        next_cover_prefetch_token == metadata_next_token ||
        !gui_player_has_active_track() ||
        strcmp(metadata_next_current_path, gui_player_get_current_track_path()) != 0 ||
        strcmp(next_metadata_cached_path, gui_player_get_current_track_path()) == 0 ||
        is_http_url(next_metadata_cached_path) || remote_track_path_is_remote(next_metadata_cached_path)) return;
    next_cover_prefetch_token = metadata_next_token; /* At most one attempt per queue prediction. */
    player_cover_prefetch_submit(next_metadata_cached_path, next_metadata_cache.artist,
                                next_metadata_cache.album, next_metadata_cache.album_artist,
                                player_cover_raster_px(), player_cover_raster_px());
}

/* Drop any speculative next-track cover and allow a fresh attempt. */
void player_next_cover_prefetch_reset(void) {
    player_cover_prefetch_cancel();
    next_cover_prefetch_token = 0;
}

/* The now-playing heart: player icon, Quick Drawer, and the cached state
 * Remote Control reports in /status. */
void show_favorite_state(bool is_favorite) {
    favorite_is_set = is_favorite;
    if (favorite_icon) {
        lv_image_set_src(favorite_icon, asset_path(favorite_is_set ? "playing_plane/collect_in.png" : "playing_plane/collect_out.png"));
    }
}

/* Resolve a tap made while the initial Favorite read was pending against
 * the stored value, rather than treating the placeholder as authoritative. */
static void apply_loaded_favorite(bool value) {
    bool toggle = favorite_toggle_pending;
    favorite_toggle_pending = false;
    favorite_track_known = true;
    show_favorite_state(toggle ? !value : value);
    if (toggle) {
        ++favorite_ui_revision;
        favorite_writer_submit(gui_player_get_current_track_path(), favorite_is_set);
    }
}

/* Remote Control changed some favorite: reload the now-playing heart from
 * the writer, which also sees this player's own unsaved taps. */
void gui_player_refresh_favorite(void) {
    if (!gui_player_has_active_track()) return;
    const char * current = gui_player_get_current_track_path();
    if (!current || !current[0]) return;
    ++favorite_ui_revision;
    favorite_request_revision = favorite_ui_revision;
    track_metadata_request_t request = { .token = ++favorite_request_token };
    snprintf(request.path, sizeof(request.path), "%s", current);
    snprintf(favorite_request_path, sizeof(favorite_request_path), "%s", current);
    track_metadata_submit(TRACK_METADATA_FAVORITE, &request);
}

void favorite_icon_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!gui_player_has_active_track()) return;

    const char * path = gui_player_get_current_track_path();
    if (!path) return;

    if (!favorite_track_known) {
        favorite_toggle_pending = !favorite_toggle_pending;
        show_favorite_state(!favorite_is_set);
        return;
    }
    ++favorite_ui_revision;
    show_favorite_state(!favorite_is_set);

    favorite_writer_submit(path, favorite_is_set);
}

static void meta_set_text(char * dst, size_t size, bool * has, const char * text) {
    if (!text[0]) return;
    snprintf(dst, size, "%s", text);
    *has = true;
}

bool seed_track_metadata(int index, track_metadata_t * out_meta) {
    /* Resolved once -- this is player_queue_model_current()'s first real touch on every
     * track-start (play_track_at_from()/on_track_auto_advanced() both call
     * this immediately after setting player_queue_model_current()), so it's also where a
     * lazy All-Songs slot actually gets strdup'd. Every other playlist[index]
     * use below reuses this same resolved pointer rather than re-deriving
     * it (playlist_path_at() is idempotent/cheap on a re-call either way,
     * but there's no reason to). */
    const char * path = playlist_path_at(index);

    track_source_t src;
    track_source_resolve(index, path, &src);

    if (src.is_remote) {
        /* No file to open for a remote track -- unlike the Subsonic branch
         * below, this also fixes the ReplayGain gap Subsonic has today
         * (that branch never calls metadata_read(), so has_replaygain stays
         * false): the plugin declares replaygain_db up front from its own
         * catalog metadata, fed into resolve_replaygain() the same as any
         * local track's tag would be. */
        memset(out_meta, 0, sizeof(*out_meta));
        meta_set_text(out_meta->title, sizeof(out_meta->title), &out_meta->has_title, src.remote.title);
        meta_set_text(out_meta->artist, sizeof(out_meta->artist), &out_meta->has_artist, src.remote.artist);
        meta_set_text(out_meta->album, sizeof(out_meta->album), &out_meta->has_album, src.remote.album);
        if (src.remote.has_replaygain) {
            out_meta->has_replaygain = true;
            out_meta->replaygain_gain_db = src.remote.replaygain_db;
        }
    } else if (src.is_subsonic) {
        memset(out_meta, 0, sizeof(*out_meta));
        meta_set_text(out_meta->title, sizeof(out_meta->title), &out_meta->has_title, src.subsonic->title);
        meta_set_text(out_meta->artist, sizeof(out_meta->artist), &out_meta->has_artist, src.subsonic->artist);
        meta_set_text(out_meta->album, sizeof(out_meta->album), &out_meta->has_album, src.subsonic->album);
    } else {
        memset(out_meta, 0, sizeof(*out_meta));
        /* The tapped list row may have an exact owned snapshot even when the
         * opportunistic RAM display cache has evicted it. It is display-only;
         * audio still reads fresh tags for ReplayGain. */
        if (selection_metadata_seed_valid && path &&
            strcmp(path, selection_metadata_seed_path) == 0) {
            *out_meta = selection_metadata_seed;
        } else {
            /* Browsing already read these tags. Reuse an owned RAM snapshot:
             * no database lock or storage access is allowed on this UI path. */
            song_row_t row;
            if (!is_http_url(path) && metadata_db_try_get_cached_song_by_path(path, &row)) {
#define DISPLAY_TEXT(field) \
            snprintf(out_meta->field, sizeof(out_meta->field), "%s", row.tags.field); \
            out_meta->has_##field = out_meta->field[0] != '\0'
                DISPLAY_TEXT(title);
                DISPLAY_TEXT(artist);
                DISPLAY_TEXT(album);
                DISPLAY_TEXT(album_artist);
                DISPLAY_TEXT(genre);
#undef DISPLAY_TEXT
                out_meta->track_number = row.tags.track_number;
                out_meta->has_track_number = row.tags.track_number > 0;
                out_meta->disc_number = row.tags.disc_number;
                out_meta->has_disc_number = row.tags.disc_number > 0;
                out_meta->release_year = row.tags.release_year;
                out_meta->has_release_year = row.tags.release_year > 0;
                metadata_normalize_text_tags(out_meta);
            }
        }
    }
    /* Stream and remote catalog text reaches the rows too: same single-line,
     * non-empty rules as file tags. */
    if (src.is_remote || src.is_subsonic) metadata_normalize_text_tags(out_meta);

    return !src.is_remote && !src.is_subsonic && !is_http_url(path);
}

/* Called by list selection handlers while they still own the visible row.
 * The copied snapshot is keyed by the exact path and contains display tags
 * only; it performs no database or filesystem work and cannot supply audio
 * ReplayGain. */
void gui_player_seed_selection_song(const char * path, const track_metadata_t * metadata) {
    selection_metadata_seed_valid = false;
    selection_metadata_seed_path[0] = '\0';
    memset(&selection_metadata_seed, 0, sizeof(selection_metadata_seed));
    if (!path || !path[0] || !metadata || strlen(path) >= sizeof(selection_metadata_seed_path)) return;
    snprintf(selection_metadata_seed_path, sizeof(selection_metadata_seed_path), "%s", path);
#define COPY_SELECTION_TEXT(field) do { \
    if (metadata->has_##field) { \
        snprintf(selection_metadata_seed.field, sizeof(selection_metadata_seed.field), "%s", metadata->field); \
        selection_metadata_seed.has_##field = selection_metadata_seed.field[0] != '\0'; \
    } \
} while (0)
    COPY_SELECTION_TEXT(title);
    COPY_SELECTION_TEXT(artist);
    COPY_SELECTION_TEXT(album);
    COPY_SELECTION_TEXT(album_artist);
    COPY_SELECTION_TEXT(genre);
#undef COPY_SELECTION_TEXT
    if (metadata->has_track_number && metadata->track_number > 0) {
        selection_metadata_seed.track_number = metadata->track_number;
        selection_metadata_seed.has_track_number = true;
    }
    if (metadata->has_disc_number && metadata->disc_number > 0) {
        selection_metadata_seed.disc_number = metadata->disc_number;
        selection_metadata_seed.has_disc_number = true;
    }
    if (metadata->has_release_year && metadata->release_year > 0) {
        selection_metadata_seed.release_year = metadata->release_year;
        selection_metadata_seed.has_release_year = true;
    }
    metadata_normalize_text_tags(&selection_metadata_seed);
    selection_metadata_seed_valid = true;
}

void clear_selection_metadata_seed(void) {
    selection_metadata_seed_valid = false;
    selection_metadata_seed_path[0] = '\0';
    memset(&selection_metadata_seed, 0, sizeof(selection_metadata_seed));
}

/* Only the server the track came from can serve its cover id. */
static void launch_subsonic_track_cover(int index, const subsonic_stream_song_meta_t * song) {
    subsonic_server_t server = subsonic_server_from_settings();
    char key[SUBSONIC_CACHE_KEY_SIZE];
    subsonic_cache_server_key(&server, key);
    if (strcmp(key, song->server_key) != 0) {
        cover_decode_request_t no_art = { .for_index = index };
        cover_decode_request_snapshot_frost(&no_art);
        launch_cover_decode_req(no_art);
        return;
    }
    launch_cover_decode_from_subsonic(index, &server, song->cover_art, gui_shell_wifi_connected());
}

/* Artwork is independent of favorite/lyrics completion. Dispatch once the
 * current track has enough identity and scalar tags to find its art. */
static void dispatch_track_artwork(int index, const char * path, track_metadata_t * meta,
                                   uint64_t metadata_token) {
    if (artwork_dispatched_metadata_token == metadata_token &&
        strcmp(artwork_dispatched_artist, meta->artist) == 0 &&
        strcmp(artwork_dispatched_album, meta->album) == 0 &&
        strcmp(artwork_dispatched_album_artist, meta->album_artist) == 0) return;
    track_source_t src;
    track_source_resolve(index, path, &src);
    if (src.is_remote && src.remote.artwork_url[0]) {
        launch_cover_decode_from_url(index, src.remote.artwork_url, src.remote.verify_tls);
    } else if (src.is_subsonic && src.subsonic->cover_art[0]) {
        launch_subsonic_track_cover(index, src.subsonic);
    } else if (src.is_remote) {
        /* Synthetic remote keys have no local file fallback. */
        cover_decode_request_t no_art = { .for_index = index };
        cover_decode_request_snapshot_frost(&no_art);
        launch_cover_decode_req(no_art);
    } else {
        free(meta->picture_data);
        meta->picture_data = NULL;
        launch_cover_decode_from_track(index, path, meta->artist, meta->album, meta->album_artist);
    }
    /* An allocation/thread-start failure leaves neither active nor pending
     * work. Let metadata completion retry instead of marking it dispatched. */
    if (player_artwork_jobs_active() || player_artwork_jobs_pending()) {
        artwork_dispatched_metadata_token = metadata_token;
        snprintf(artwork_dispatched_artist, sizeof(artwork_dispatched_artist), "%s", meta->artist);
        snprintf(artwork_dispatched_album, sizeof(artwork_dispatched_album), "%s", meta->album);
        snprintf(artwork_dispatched_album_artist, sizeof(artwork_dispatched_album_artist), "%s", meta->album_artist);
    }
}

static void apply_loaded_track_metadata(int index, track_metadata_t * out_meta, bool favorite, bool favorite_ready) {
    const char * path = playlist_path_at(index);
    char title[128], folder[128];
    get_display_names(path, title, sizeof(title), folder, sizeof(folder));
    track_source_t src;
    track_source_resolve(index, path, &src);
    gui_track_info_context_t info = {0};
    snprintf(info.path, sizeof(info.path), "%s", path);
    info.replaygain_mode = current_settings.replaygain_mode;
    info.has_replaygain_track = out_meta->has_replaygain;
    info.replaygain_track_db = out_meta->replaygain_gain_db;
    info.has_replaygain_album = out_meta->has_replaygain_album;
    info.replaygain_album_db = out_meta->replaygain_album_gain_db;
    info.has_track_number = out_meta->has_track_number;
    info.track_number = out_meta->track_number;
    info.has_disc_number = out_meta->has_disc_number;
    info.disc_number = out_meta->disc_number;

    const char * format_hint = info_path_hint(path);
    if (src.is_remote) {
        info.source = GUI_TRACK_SOURCE_PLUGIN;
        snprintf(info.provider, sizeof(info.provider), "%s", src.remote.provider);
        snprintf(info.track_id, sizeof(info.track_id), "%s", src.remote.track_id);
        format_hint = src.remote.codec;
        info.declared_codec = info_codec_from_hint(src.remote.codec);
        if (info.declared_codec == AUDIO_CODEC_UNKNOWN) format_hint = NULL;
        info.declared_sample_rate = src.remote.sample_rate;
        info.declared_bit_depth = src.remote.bit_depth;
        info.declared_channels = src.remote.channels;
        info.declared_bitrate_kbps = src.remote.bitrate_kbps;
        info.declared_duration_seconds = (double) src.remote.duration_ms / 1000.0;
    } else if (src.is_subsonic) {
        const subsonic_stream_song_meta_t * sm = src.subsonic;
        info.source = GUI_TRACK_SOURCE_SUBSONIC;
        format_hint = sm->suffix;
        info.declared_codec = info_codec_from_hint(sm->suffix);
        info.declared_sample_rate = sm->sample_rate;
        info.declared_bit_depth = sm->bit_depth;
        info.declared_channels = sm->channels;
        info.declared_bitrate_kbps = sm->bitrate_kbps;
        info.declared_duration_seconds = sm->duration_seconds;
        info.has_track_number = sm->track > 0;
        info.track_number = sm->track;
        info.has_disc_number = sm->disc > 0;
        info.disc_number = sm->disc;
    } else if (is_http_url(path)) {
        info.source = GUI_TRACK_SOURCE_RADIO;
        info.declared_codec = info_codec_from_hint(format_hint);
        if (info.declared_codec == AUDIO_CODEC_UNKNOWN) format_hint = NULL;
    } else {
        info.source = GUI_TRACK_SOURCE_LOCAL;
        info.declared_codec = info_codec_from_hint(format_hint);
    }
    info_container_from_hint(format_hint, info.container);
    gui_track_info_set_current(&info);

    const char * title_text = out_meta->has_title ? out_meta->title : title;
    const char * folder_text = out_meta->has_artist ? out_meta->artist : folder;
    const char * album_text = out_meta->has_album ? out_meta->album : "";
    snprintf(now_playing_genre, sizeof(now_playing_genre), "%s", out_meta->has_genre ? out_meta->genre : "");
    now_playing_track_number = out_meta->has_track_number && out_meta->track_number > 0 ? out_meta->track_number : 0;
    if (src.is_subsonic && src.subsonic->track > 0)
        now_playing_track_number = src.subsonic->track;

    player_set_track_display_text(title_text, folder_text, album_text);
    refresh_format_badge();
    dispatch_track_artwork(index, path, out_meta, metadata_ui_token);

    /* No local sidecar (or embedded tag) makes sense for a Subsonic stream
     * or remote-provider URL -- same reasoning cover art uses a server-
     * supplied cover URL instead of a local-file lookup for those. A
     * streamed track just never gets lyrics in this first release. */
    if (!src.is_subsonic && !src.is_remote) {
        gui_lyrics_load_track(index, path);
    } else {
        gui_lyrics_load_track(index, NULL);
    }
    /* Results contain only scalar tags; keep this defensive ownership guard
     * if the metadata loader later supplies optional blobs. */
    free(out_meta->lyrics);
    out_meta->lyrics = NULL;

    if (favorite_ready && metadata_favorite_revision == favorite_ui_revision) apply_loaded_favorite(favorite);
    player_transition_mark_dirty();
}

/* Establish the new UI identity immediately; file/DB enrichment arrives
 * through an owned scalar snapshot and never resets live playback progress. */
void prepare_track_metadata(int index, bool notify, bool probe, bool audio_tags,
                            bool submit_request) {
    player_next_cover_prefetch_reset();
    const char * path = playlist_path_at(index);
    track_metadata_request_t request = { .token = ++metadata_ui_token, .probe = probe };
    snprintf(request.path, sizeof(request.path), "%s", path);
    request.read_file = seed_track_metadata(index, &request.metadata);
    if (request.read_file && notify) {
        if (next_metadata_cached && strcmp(path, next_metadata_cached_path) == 0) {
            request.metadata = next_metadata_cache;
            request.read_file = false;
        } else if (audio_tags) {
            request.read_audio = true;
            request.read_file = false;
            request.audio_generation = audio_get_playback_generation();
        }
    }
    /* Consume the prefetched NEXT snapshot only after this path-checked copy.
     * A later current request must not inherit a stale queue entry. */
    next_metadata_cached = false;
    snprintf(metadata_ui_path, sizeof(metadata_ui_path), "%s", path);
#ifdef UI_PERF_TRACE
    metadata_request_perf_us = ui_perf_now_us();
#endif
    metadata_ui_notify = notify;
    metadata_notify_ready = false;
    plugin_now_playing_loaded = false;
    metadata_ui_audio_generation = audio_get_playback_generation();
    metadata_favorite_revision = ++favorite_ui_revision;
    metadata_ui_request = request;
    if (submit_request) track_metadata_submit(TRACK_METADATA_CURRENT, &request);
    /* Cancel a cover result even if a new queue reuses the same slot. */
    player_artwork_jobs_advance_generation();
    player_artwork_jobs_clear_pending();
    gui_lyrics_load_track(index, NULL);
    bt_cover_art_begin_track(path);
    gui_track_info_context_t pending_info = {0};
    snprintf(pending_info.path, sizeof(pending_info.path), "%s", path);
    pending_info.replaygain_mode = current_settings.replaygain_mode;
    pending_info.source = remote_track_path_is_remote(path) ? GUI_TRACK_SOURCE_PLUGIN :
        subsonic_meta_for_path(index, path) ? GUI_TRACK_SOURCE_SUBSONIC :
        is_http_url(path) ? GUI_TRACK_SOURCE_RADIO : GUI_TRACK_SOURCE_LOCAL;
    pending_info.declared_codec = info_codec_from_hint(info_path_hint(path));
    gui_track_info_set_current(&pending_info);
    refresh_format_badge();
    snprintf(now_playing_path, sizeof(now_playing_path), "%s", path);
    refresh_now_playing_indicators();
    char title[128], folder[128];
    get_display_names(path, title, sizeof(title), folder, sizeof(folder));
    player_set_track_display_text(request.metadata.has_title ? request.metadata.title : title,
                                  request.metadata.has_artist ? request.metadata.artist : folder,
                                  request.metadata.has_album ? request.metadata.album : "");
    player_reset_now_playing_extras();
    favorite_track_known = false;
    favorite_toggle_pending = false;
    show_favorite_state(false);

    player_progress_display_reset();

    if (song_count_label) {
        player_set_song_count(index, player_queue_model_count());
        lv_obj_remove_flag(song_count_label, LV_OBJ_FLAG_HIDDEN);
    }
    /* Cached catalog tags can identify artwork immediately even while the
     * fresh audio tags/favorite are pending. Later changed album identity
     * cancels/replaces this request through the normal generation guard. */
    if ((!request.read_file && !request.read_audio) || request.metadata.has_title ||
        request.metadata.has_artist || request.metadata.has_album)
        dispatch_track_artwork(index, path, &request.metadata, request.token);
#ifdef UI_PERF_TRACE
    printf("PERF player_selection_seed token=%llu known_tags=%d audio_wait=%d artwork_started=%d\n",
           (unsigned long long)request.token, request.metadata.has_title || request.metadata.has_artist || request.metadata.has_album,
           request.read_audio, artwork_dispatched_metadata_token == request.token);
#endif
    player_transition_mark_dirty(); /* title/artist/format badge/progress reset above all just changed player_screen's own content -- see the cache's own doc comment */
    player_track_change_timeline_play();
}

void request_track_metadata(int index, bool notify, bool probe, bool audio_tags) {
    prepare_track_metadata(index, notify, probe, audio_tags, true);
}

/* The manual selection path seeds the UI before audio startup can block.
 * Once audio has its new generation, submit the prepared request with
 * fresh audio-tag reading while retaining its token and display snapshot.
 * Preparing the display does not submit a worker request or parse tags. */
void request_track_audio_metadata(int index) {
    if (index < 0 || index >= player_queue_model_count() || index != player_queue_model_current() ||
        !metadata_ui_notify || metadata_ui_request.token != metadata_ui_token) return;
    const char * path = playlist_path_at(index);
    if (!path || strcmp(path, metadata_ui_path) != 0 ||
        strcmp(path, metadata_ui_request.path) != 0) return;
    if (metadata_ui_request.read_file && metadata_ui_notify) {
        metadata_ui_request.read_file = false;
        metadata_ui_request.read_audio = true;
        metadata_ui_request.audio_generation = audio_get_playback_generation();
    }
    metadata_ui_audio_generation = audio_get_playback_generation();
    track_metadata_submit(TRACK_METADATA_CURRENT, &metadata_ui_request);
}

/* Tells audio.c what comes after `index` (per the current play mode -- see
 * compute_auto_advance_index()) so its playback thread can gapless-handoff
 * or crossfade into it near `index`'s natural end without a GUI round-trip.
 * Must be re-called (from on_track_auto_advanced) every time the thread
 * advances on its own, or the chain of automatic transitions stops after
 * one hop. */
void arm_next_track_for_audio(int index) {
    player_next_cover_prefetch_reset();
    ++metadata_next_token;
    next_metadata_cached = false;
    next_metadata_cached_path[0] = '\0';
    int next_index = -1;
    uint64_t revision = 0;
    const char *next_path = player_queue_model_note_next_armed(
        current_settings.play_mode, index, &next_index, &revision);
    if (next_index < 0) {
        armed_next_path[0] = '\0';
        audio_set_next_track(NULL, false, 0.0, false, 0.0);
        return;
    }
    uint64_t generation = audio_get_playback_generation();
    if (!current_settings.gapless_enabled || armed_next_generation != generation ||
        armed_next_gain_mode != current_settings.replaygain_mode || strcmp(armed_next_path, next_path) != 0) {
        armed_next_path[0] = '\0';
        audio_set_next_track(NULL, false, 0.0, false, 0.0);
    }
    track_metadata_request_t request = { .token = ++metadata_next_token };
    snprintf(request.path, sizeof(request.path), "%s", next_path);
    request.read_file = seed_track_metadata(next_index, &request.metadata);
    metadata_next_queue_revision = revision;
    metadata_next_audio_generation = audio_get_playback_generation();
    snprintf(metadata_next_current_path, sizeof(metadata_next_current_path), "%s",
             player_queue_model_path_at(index));
    metadata_next_gain_mode = current_settings.replaygain_mode;
    metadata_next_gapless_enabled = current_settings.gapless_enabled;
    track_metadata_submit(TRACK_METADATA_NEXT, &request);
}

static void poll_track_started_notification(void) {
    if (!metadata_ui_notify || !metadata_notify_ready) return;
    if (audio_get_playback_generation() != metadata_ui_audio_generation) {
        metadata_ui_notify = false;
        metadata_notify_ready = false;
        return;
    }
    audio_playback_progress_t progress;
    if (!audio_get_playback_progress(metadata_ui_path, &progress) ||
        progress.generation != metadata_ui_audio_generation ||
        progress.state != AUDIO_PLAYBACK_PROGRESS_ACTIVE ||
        (!progress.is_playing && !progress.is_paused)) return;
#ifndef HOST_BUILD
    hiby_sys_server_report_metadata(metadata_notify_payload.title, metadata_notify_payload.artist,
        metadata_notify_payload.album, metadata_notify_payload.genre,
        (long) (progress.duration_seconds * 1000.0));
    hiby_sys_server_report_position((long) (progress.position_seconds * 1000.0));
#endif
    notify_plugin_track_started(&metadata_notify_payload, metadata_ui_path, progress.duration_seconds);
    metadata_ui_notify = false;
    metadata_notify_ready = false;
}

static bool arm_cached_next_track(const char * path, const track_metadata_t * tags,
                                  uint64_t generation, int gain_mode) {
    if (!path || !tags || generation != audio_get_playback_generation() ||
        !current_settings.gapless_enabled) return false;
    if (remote_track_path_is_remote(path) && !remote_track_stream_is_fresh(path)) return false;
    if (armed_next_generation == generation && armed_next_gain_mode == gain_mode &&
        strcmp(armed_next_path, path) == 0) return true;

    bool gain, peak;
    double gain_db, peak_value;
    replaygain_for_mode(gain_mode, tags, &gain, &gain_db, &peak, &peak_value);
    if (!audio_set_next_track_for_generation(path, gain, gain_db, peak, peak_value, generation)) return false;
    snprintf(armed_next_path, sizeof(armed_next_path), "%s", path);
    armed_next_generation = generation;
    armed_next_gain_mode = gain_mode;
    return true;
}

/* When Wi-Fi comes back, a Subsonic cover that missed offline is asked for
 * again. The new launch advances the cover generation, so an older result
 * cannot land over it. */
static void retry_subsonic_cover_on_wifi(void) {
    static bool was_connected;
    bool connected = gui_shell_wifi_connected();
    bool came_back = connected && !was_connected;
    was_connected = connected;
    if (!came_back || !gui_player_has_active_track() || player_cover_bytes()) return;
    int index = player_queue_model_current();
    track_source_t src;
    track_source_resolve(index, playlist_path_at(index), &src);
    if (src.is_subsonic && src.subsonic->cover_art[0]) launch_subsonic_track_cover(index, src.subsonic);
}

void gui_player_poll_track_metadata(void) {
    poll_track_started_notification();
    retry_subsonic_cover_on_wifi();
    track_metadata_result_t result;
    if (track_metadata_take(TRACK_METADATA_FAVORITE, &result) &&
        result.request.token == favorite_request_token && favorite_request_revision == favorite_ui_revision &&
        gui_player_has_active_track() &&
        strcmp(result.request.path, gui_player_get_current_track_path()) == 0 &&
        strcmp(result.request.path, favorite_request_path) == 0)
        apply_loaded_favorite(result.favorite);
    if (track_metadata_take(TRACK_METADATA_NEXT, &result) &&
        result.request.token == metadata_next_token && player_queue_model_revision() == metadata_next_queue_revision &&
        audio_get_playback_generation() == metadata_next_audio_generation &&
        gui_player_has_active_track() &&
        strcmp(gui_player_get_current_track_path(), metadata_next_current_path) == 0) {
        next_metadata_cached = true;
        snprintf(next_metadata_cached_path, sizeof(next_metadata_cached_path), "%s", result.request.path);
        next_metadata_cache = result.request.metadata;
        if (metadata_next_gapless_enabled && current_settings.gapless_enabled)
            arm_cached_next_track(result.request.path, &result.request.metadata,
                                  metadata_next_audio_generation, metadata_next_gain_mode);
    }
    /* Resolve a signed provider URL in the background shortly before the
     * current track ends. This runs from the existing UI poll, and never
     * waits for the plugin response. */
    if (gui_player_has_active_track() &&
        audio_get_playback_generation() == metadata_next_audio_generation &&
        player_queue_model_revision() == metadata_next_queue_revision &&
        strcmp(gui_player_get_current_track_path(), metadata_next_current_path) == 0 &&
        next_metadata_cached) {
        int next_index = compute_auto_advance_index(player_queue_model_current());
        if (next_index < 0 || strcmp(next_metadata_cached_path, playlist_path_at(next_index)) != 0)
            next_metadata_cached = false;
    }
    if (gui_player_has_active_track() &&
        audio_get_playback_generation() == metadata_next_audio_generation &&
        player_queue_model_revision() == metadata_next_queue_revision &&
        strcmp(gui_player_get_current_track_path(), metadata_next_current_path) == 0 &&
        next_metadata_cached) {
        audio_playback_progress_t progress;
        if (audio_get_playback_progress(metadata_ui_path, &progress) &&
            progress.generation == metadata_next_audio_generation &&
            progress.state == AUDIO_PLAYBACK_PROGRESS_ACTIVE && progress.is_playing &&
            progress.duration_seconds > 0.0 &&
            progress.duration_seconds - progress.position_seconds <= 30.0) {
            if (remote_track_path_is_remote(next_metadata_cached_path))
                remote_track_stream_prefetch(next_metadata_cached_path, metadata_next_audio_generation);
            if (metadata_next_gapless_enabled && current_settings.gapless_enabled)
                arm_cached_next_track(next_metadata_cached_path, &next_metadata_cache,
                                      metadata_next_audio_generation, metadata_next_gain_mode);
        }
    }
    if (!track_metadata_take(TRACK_METADATA_CURRENT, &result)) return;
    if (result.request.token != metadata_ui_token ||
        !gui_player_has_active_track() || !gui_player_get_screen() ||
        strcmp(result.request.path, gui_player_get_current_track_path()) != 0 ||
        strcmp(result.request.path, metadata_ui_path) != 0) return;
    if (result.display_hint) {
        /* A catalog snapshot can reach us before audio startup on a RAM
         * cache miss. Show it without finalizing lyrics, favorites, format
         * or plugin notifications; fresh file tags still own those. */
        char title[128], folder[128];
        get_display_names(result.request.path, title, sizeof(title), folder, sizeof(folder));
        track_metadata_t * hint = &result.request.metadata;
        player_set_track_display_text(hint->has_title ? hint->title : title,
                                      hint->has_artist ? hint->artist : folder,
                                      hint->has_album ? hint->album : "");
        dispatch_track_artwork(player_queue_model_current(), result.request.path, hint, result.request.token);
        player_transition_mark_dirty();
        return;
    }
#ifdef UI_PERF_TRACE
    printf("PERF player_metadata_request_to_result_ms elapsed=%u token=%llu\n",
           (unsigned)((ui_perf_now_us() - metadata_request_perf_us) / 1000),
           (unsigned long long) result.request.token);
#endif
    if (metadata_scalar_applied_token != result.request.token) {
        apply_loaded_track_metadata(player_queue_model_current(), &result.request.metadata, result.favorite,
                                     !result.enrichment_pending);
        metadata_scalar_applied_token = result.request.token;
    } else if (!result.enrichment_pending && metadata_favorite_revision == favorite_ui_revision) {
        apply_loaded_favorite(result.favorite);
    }
    if (metadata_ui_notify) {
        metadata_notify_payload = result.request.metadata;
        metadata_notify_ready = true;
        poll_track_started_notification();
    }
    if (deferred_resume_pending && result.has_probe && result.probe.duration_seconds > 0.0) {
        double position = isfinite(deferred_resume_position) ? deferred_resume_position : 0.0;
        if (position < 0.0) position = 0.0;
        if (position > result.probe.duration_seconds) position = result.probe.duration_seconds;

        player_progress_display_set_deferred(position, result.probe.duration_seconds);
    }
}

/* Cached now-playing metadata for plugin.get_now_playing() -- populated at
 * the same two call sites that fire the "track_started" plugin event
 * (notify_plugin_track_started() below), so there's no separate tracking
 * needed. plugin_now_playing_loaded distinguishes "nothing has ever played
 * this session" from "a track is playing with an empty title" -- the title
 * buffer alone can't tell those apart. */
char plugin_now_playing_title[128];
char plugin_now_playing_artist[128];
char plugin_now_playing_album[128];
double plugin_now_playing_duration;
bool plugin_now_playing_loaded = false;

/* Shared by play_track_at_from()/on_track_auto_advanced() below -- caches
 * meta into the plugin_now_playing_* globals above and fires the
 * "track_started" plugin event. Called after audio_play_file_at()/the
 * gapless handoff has already happened in both call sites, so
 * audio_get_duration_seconds() reflects the new track. */
static void notify_plugin_track_started(const track_metadata_t * meta, const char * path, double duration) {
    snprintf(plugin_now_playing_title, sizeof(plugin_now_playing_title), "%s", meta->title);
    snprintf(plugin_now_playing_artist, sizeof(plugin_now_playing_artist), "%s", meta->artist);
    snprintf(plugin_now_playing_album, sizeof(plugin_now_playing_album), "%s", meta->album);
    plugin_now_playing_duration = duration;
    plugin_now_playing_loaded = true;
    gui_lock_screen_refresh(false);

    remote_track_meta_t remote_meta;
    bool is_remote_track = remote_track_meta_copy_for_path(path, &remote_meta);
    plugin_manager_notify_track_started(meta->title, meta->artist, meta->album, plugin_now_playing_duration,
                                         is_remote_track ? remote_meta.provider : "", is_remote_track ? remote_meta.track_id : "");
}

const char * gui_player_get_now_playing_genre(void) {
    return now_playing_genre;
}

int gui_player_get_now_playing_track_number(void) {
    return now_playing_track_number;
}

/* Seeds the first current-track metadata request after the initial boot. */
void player_metadata_request_current_for_init(void) {
    track_metadata_request_t request = { .token = ++metadata_ui_token, .probe = deferred_resume_pending };
    snprintf(request.path, sizeof(request.path), "%s", gui_player_get_current_track_path());
    request.read_file = seed_track_metadata(player_queue_model_current(), &request.metadata);
    snprintf(metadata_ui_path, sizeof(metadata_ui_path), "%s", request.path);
    metadata_ui_audio_generation = audio_get_playback_generation();
    metadata_favorite_revision = favorite_ui_revision;
    track_metadata_submit(TRACK_METADATA_CURRENT, &request);
}

/* The queue was cleared: drop every in-flight metadata and favorite result. */
void player_metadata_invalidate_queue(void) {
    ++metadata_ui_token;
    ++metadata_next_token;
    next_metadata_cached = false;
    next_metadata_cached_path[0] = '\0';
    ++favorite_ui_revision;
}

/* The UI is being torn down: drop in-flight current and favorite results. */
void player_metadata_invalidate_ui(void) {
    ++metadata_ui_token;
    ++favorite_ui_revision;
}
