#include "remote_control_internal.h"
#include "../../../core/system/runtime/app_version.h"
#include "../../../audio/engine/audio.h"
#ifndef HOST_BUILD
#include "../../../audio/output/audio_output.h" /* target-only, like audio.c's own audio_output_* calls */
#endif
#include "../../../core/persistence/settings_registry.h"
#include "../../bluetooth/control/bluetooth_control.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t status_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool status_bluetooth_dac_mode = false;
static bool status_playing = false;
static bool status_paused = false;
static char status_title[256] = {0};
static char status_artist[256] = {0};
static char status_album[256] = {0};
static char status_path[512] = {0};
static int status_position_seconds = 0;
static int status_duration_seconds = 0;
static float status_volume = 0;
static int status_play_mode = 0;
static bool status_favorite = false;
static bool status_accent_valid = false;
static uint32_t status_accent_rgb = 0;
static int status_library_sorting[3] = {0, 0, 0};
static bool request_has_library_sorting;
static int request_sort_scope, request_sort_mode;
static remote_collection_play_request_t request_collection;
static bool request_has_collection;
static unsigned collection_job_id;
static bool collection_job_busy;
static bool collection_job_failed;
static char collection_job_error[128];
static remote_import_state_t remote_import_state = REMOTE_IMPORT_STOPPED;
static char remote_import_error[128] = "";
static bool request_has_import;
static bool request_import_start;

/* Copies the path of the now-playing track; false (out untouched) when
 * nothing is playing. */
bool remote_state_now_playing_path(char * out, size_t size) {
    pthread_mutex_lock(&status_mutex);
    bool ok = status_path[0] != '\0';
    if (ok) snprintf(out, size, "%s", status_path);
    pthread_mutex_unlock(&status_mutex);
    return ok;
}

bool remote_control_consume_import_request(bool *out_start) {
    pthread_mutex_lock(&status_mutex);
    bool pending = request_has_import;
    if (pending) {
        if (out_start) *out_start = request_import_start;
        request_has_import = false;
    }
    pthread_mutex_unlock(&status_mutex);
    return pending;
}

void remote_control_set_import_state(remote_import_state_t state, const char *error) {
    pthread_mutex_lock(&status_mutex);
    remote_import_state = state;
    snprintf(remote_import_error, sizeof(remote_import_error), "%s", error ? error : "");
    pthread_mutex_unlock(&status_mutex);
}

void remote_control_get_import_state(remote_import_state_t *state, char *error, size_t error_size) {
    pthread_mutex_lock(&status_mutex);
    if (state) *state = remote_import_state;
    if (error && error_size) snprintf(error, error_size, "%s", remote_import_error);
    pthread_mutex_unlock(&status_mutex);
}

void remote_control_notify_library_sorting(int albums, int artists, int album_artists) {
    pthread_mutex_lock(&status_mutex);
    status_library_sorting[0] = albums;
    status_library_sorting[1] = artists;
    status_library_sorting[2] = album_artists;
    pthread_mutex_unlock(&status_mutex);
}

bool remote_control_consume_library_sorting(int *scope, int *mode) {
    pthread_mutex_lock(&status_mutex);
    bool pending = request_has_library_sorting;
    if (pending) { *scope = request_sort_scope; *mode = request_sort_mode; request_has_library_sorting = false; }
    pthread_mutex_unlock(&status_mutex);
    return pending;
}

bool remote_control_consume_collection(remote_collection_play_request_t *request) {
    pthread_mutex_lock(&status_mutex);
    bool pending = request_has_collection;
    if (pending) { *request = request_collection; request_has_collection = false; }
    pthread_mutex_unlock(&status_mutex);
    return pending;
}

void remote_control_finish_collection(bool success, const char *error) {
    pthread_mutex_lock(&status_mutex);
    collection_job_busy = false;
    collection_job_failed = !success;
    snprintf(collection_job_error, sizeof(collection_job_error), "%s", success ? "" : (error ? error : "Could not play collection"));
    pthread_mutex_unlock(&status_mutex);
}

/* Source format is copied into this API snapshot before status_mutex is
 * acquired. Never call into audio.c while holding status_mutex: audio and UI
 * paths may acquire their own locks in the opposite order. */
static char status_codec[32] = {0};
static unsigned int status_source_bit_depth = 0;
static unsigned int status_source_sample_rate = 0;

/* Control requests -- edge-triggered flags consumed by update_timer_cb.
 * Guarded by status_mutex. */
static bool request_play_pause = false;
static bool request_screenshot = false;
static bool request_next = false;
static bool request_prev = false;
static bool request_mode_cycle = false;
static bool request_has_play_mode = false;
static int request_play_mode = 0;
static bool request_has_seek = false;
static int request_seek_seconds = 0;
static bool request_has_volume = false;
static int request_volume_percent = 0;
static bool request_has_play_index = false;
static int64_t request_play_index = 0;
static char request_play_catalog_revision[METADATA_DB_CATALOG_REVISION_SIZE] = "";
static bool request_has_queue_index = false;
static int64_t request_queue_index = 0;
static char request_queue_catalog_revision[METADATA_DB_CATALOG_REVISION_SIZE] = "";
static bool request_has_queue_remove = false;
static int request_queue_remove_offset = 0;
static uint64_t request_queue_remove_revision = 0;
static bool request_queue_clear = false;
static uint64_t request_queue_clear_revision = 0;
static bool request_has_queue_move = false;
static int request_queue_move_from = 0;
static int request_queue_move_to = 0;
static uint64_t request_queue_move_revision = 0;
static bool request_has_queue_play = false;
static int request_queue_play_offset = 0;
static uint64_t request_queue_play_revision = 0;
static bool request_has_folder_play = false;
static char request_folder_play_path[REMOTE_CONTROL_PATH_MAX] = "";
static bool request_favorite_changed = false;
/* Scope filters echoed back by /api/playback/play to build the queue within
 * an album, artist, album-artist, or playlist view. Empty string means entire library. */
static char request_play_playlist_name[128] = "";
static char request_play_artist_filter[128] = "";
static char request_play_album_artist_filter[128] = "";
static char request_play_album_filter[128] = "";

/* Queue of song IDs (metadata_db song_row_t.id). Guarded by status_mutex. */
static int64_t * queue_song_ids = NULL;
static int queue_song_id_count = 0;
static uint64_t queue_snapshot_revision = 0;

/* Publishes a request to play a file with its folder as the queue. The path
 * must fit REMOTE_CONTROL_PATH_MAX. */
void remote_state_post_folder_play(const char * abs_path) {
    pthread_mutex_lock(&status_mutex);
    request_has_folder_play = true;
    memcpy(request_folder_play_path, abs_path, strlen(abs_path) + 1);
    pthread_mutex_unlock(&status_mutex);
}

/* A favorite flag was written for song_path: the UI reloads its now-playing
 * heart, and the status snapshot follows when it is the playing track. */
void remote_state_favorite_written(const char * song_path, bool is_favorite) {
    pthread_mutex_lock(&status_mutex);
    request_favorite_changed = true;
    if (strcmp(status_path, song_path) == 0) status_favorite = is_favorite;
    pthread_mutex_unlock(&status_mutex);
}

void remote_control_notify_status(bool playing, bool paused, const char * title, const char * artist,
                                   const char * album, const char * path, int position_seconds,
                                   int duration_seconds, float volume, int play_mode, bool favorite,
                                   uint32_t accent_rgb, bool dac_mode) {
    /* Take the audio snapshot before status_mutex. Match its copied path
     * against this now-playing snapshot so a track transition cannot publish
     * the previous decoder's format beside the new track's metadata. */
    audio_current_format_info_t format = {0};
    bool format_matches_track = path && path[0] &&
        audio_get_current_format_info(&format) && format.valid && !format.is_stream &&
        strcmp(format.path, path) == 0;

    /* This runs on the UI thread, where receiver settings are owned. The
     * stream getter only copies its cached snapshot; no D-Bus work here. */
    bt_dac_stream_info_t stream = {0};
    if (dac_mode) {
        bt_control_get_dac_stream_info(&stream);
        playing = stream.running;
        paused = false;
        title = artist = album = path = "";
        position_seconds = duration_seconds = 0;
        favorite = false;
    }
    pthread_mutex_lock(&status_mutex);
    status_bluetooth_dac_mode = dac_mode;
    status_playing = playing;
    status_paused = paused;
    snprintf(status_title, sizeof(status_title), "%s", title ? title : "");
    snprintf(status_artist, sizeof(status_artist), "%s", artist ? artist : "");
    snprintf(status_album, sizeof(status_album), "%s", album ? album : "");
    snprintf(status_path, sizeof(status_path), "%s", path ? path : "");
    status_position_seconds = position_seconds;
    status_duration_seconds = duration_seconds;
    status_volume = volume;
    status_play_mode = play_mode;
    status_favorite = favorite;
    status_accent_rgb = accent_rgb & 0xFFFFFF;
    status_accent_valid = true;
    snprintf(status_codec, sizeof(status_codec), "%s",
             dac_mode ? stream.codec : format_matches_track ? audio_codec_name(format.codec) : "");
    status_source_bit_depth = dac_mode ? stream.bit_depth : format_matches_track ? format.source_bit_depth : 0;
    status_source_sample_rate = dac_mode ? stream.sample_rate : format_matches_track ? format.source_sample_rate : 0;
    pthread_mutex_unlock(&status_mutex);
}

/* Reads and clears one edge-triggered request flag under status_mutex. */
static bool consume_flag(bool * flag) {
    pthread_mutex_lock(&status_mutex);
    bool result = *flag;
    *flag = false;
    pthread_mutex_unlock(&status_mutex);
    return result;
}

bool remote_control_consume_play_pause(void) {
    return consume_flag(&request_play_pause);
}

bool remote_control_consume_screenshot(void) {
    return consume_flag(&request_screenshot);
}

bool remote_control_consume_next(void) {
    return consume_flag(&request_next);
}

bool remote_control_consume_prev(void) {
    return consume_flag(&request_prev);
}

bool remote_control_consume_mode_cycle(void) {
    return consume_flag(&request_mode_cycle);
}

bool remote_control_consume_play_mode(int * out_mode) {
    pthread_mutex_lock(&status_mutex);
    bool result = request_has_play_mode;
    if (result) {
        request_has_play_mode = false;
        if (out_mode) *out_mode = request_play_mode;
    }
    pthread_mutex_unlock(&status_mutex);
    return result;
}

bool remote_control_consume_seek(int * out_seconds) {
    pthread_mutex_lock(&status_mutex);
    bool result = request_has_seek;
    if (result) {
        request_has_seek = false;
        *out_seconds = request_seek_seconds;
    }
    pthread_mutex_unlock(&status_mutex);
    return result;
}

bool remote_control_consume_volume(int * out_percent) {
    pthread_mutex_lock(&status_mutex);
    bool result = request_has_volume;
    if (result) {
        request_has_volume = false;
        *out_percent = request_volume_percent;
    }
    pthread_mutex_unlock(&status_mutex);
    return result;
}

bool remote_control_consume_queue_index(int64_t * out_index, char * out_catalog_revision,
                                         size_t revision_size) {
    pthread_mutex_lock(&status_mutex);
    bool result = request_has_queue_index;
    if (result) {
        request_has_queue_index = false;
        *out_index = request_queue_index;
        if (out_catalog_revision && revision_size)
            snprintf(out_catalog_revision, revision_size, "%s", request_queue_catalog_revision);
    }
    pthread_mutex_unlock(&status_mutex);
    return result;
}

bool remote_control_consume_queue_remove(int * out_offset, uint64_t * out_revision) {
    pthread_mutex_lock(&status_mutex);
    bool result = request_has_queue_remove;
    if (result) {
        request_has_queue_remove = false;
        *out_offset = request_queue_remove_offset;
        *out_revision = request_queue_remove_revision;
    }
    pthread_mutex_unlock(&status_mutex);
    return result;
}

bool remote_control_consume_queue_clear(uint64_t * out_revision) {
    pthread_mutex_lock(&status_mutex);
    bool result = request_queue_clear;
    request_queue_clear = false;
    if (result) *out_revision = request_queue_clear_revision;
    pthread_mutex_unlock(&status_mutex);
    return result;
}

bool remote_control_consume_queue_move(int * out_from, int * out_to, uint64_t * out_revision) {
    pthread_mutex_lock(&status_mutex);
    bool result = request_has_queue_move;
    if (result) {
        request_has_queue_move = false;
        *out_from = request_queue_move_from;
        *out_to = request_queue_move_to;
        *out_revision = request_queue_move_revision;
    }
    pthread_mutex_unlock(&status_mutex);
    return result;
}

bool remote_control_consume_queue_play(int * out_offset, uint64_t * out_revision) {
    pthread_mutex_lock(&status_mutex);
    bool result = request_has_queue_play;
    if (result) {
        request_has_queue_play = false;
        *out_offset = request_queue_play_offset;
        *out_revision = request_queue_play_revision;
    }
    pthread_mutex_unlock(&status_mutex);
    return result;
}

bool remote_control_consume_folder_play(char * out_path, size_t path_size) {
    pthread_mutex_lock(&status_mutex);
    bool result = request_has_folder_play;
    if (result) {
        request_has_folder_play = false;
        snprintf(out_path, path_size, "%s", request_folder_play_path);
    }
    pthread_mutex_unlock(&status_mutex);
    return result;
}

bool remote_control_consume_favorite_changed(void) {
    return consume_flag(&request_favorite_changed);
}

void remote_control_sync_queue(const char * const * paths, int count, uint64_t revision) {
    /* metadata_db_get_songs_by_paths() takes its own METADATA_DB_GUARD lock
     * internally -- must not be called while already holding status_mutex,
     * or a UI-thread caller blocked on METADATA_DB_GUARD (e.g. a rescan)
     * while THIS thread holds status_mutex waiting on that same lock could
     * deadlock against gui.c's own status_mutex use elsewhere. Resolved
     * before, not during, the locked section below. Batched through one
     * metadata_db_get_songs_by_paths() call (one guard for all `count`
     * lookups) rather than a metadata_db_get_song_by_path() call per song. */
    int64_t * ids = count > 0 ? malloc(sizeof(int64_t) * (size_t) count) : NULL;
    int id_count = 0;
    if (ids) {
        song_row_t * rows = malloc(sizeof(song_row_t) * (size_t) count);
        if (rows) {
            metadata_db_get_songs_by_paths(paths, count, rows);
            for (int i = 0; i < count; i++) ids[id_count++] = rows[i].id;
            free(rows);
        }
    }

    pthread_mutex_lock(&status_mutex);
    free(queue_song_ids);
    queue_song_ids = ids;
    queue_song_id_count = id_count;
    queue_snapshot_revision = revision;
    pthread_mutex_unlock(&status_mutex);
}

bool remote_control_consume_play_index(int64_t * out_index, char * out_playlist, size_t playlist_size,
                                        char * out_artist, size_t artist_size, char * out_album_artist,
                                        size_t album_artist_size, char * out_album, size_t album_size,
                                        char * out_catalog_revision, size_t revision_size) {
    pthread_mutex_lock(&status_mutex);
    bool result = request_has_play_index;
    if (result) {
        request_has_play_index = false;
        *out_index = request_play_index;
        snprintf(out_playlist, playlist_size, "%s", request_play_playlist_name);
        snprintf(out_artist, artist_size, "%s", request_play_artist_filter);
        snprintf(out_album_artist, album_artist_size, "%s", request_play_album_artist_filter);
        snprintf(out_album, album_size, "%s", request_play_album_filter);
        if (out_catalog_revision && revision_size)
            snprintf(out_catalog_revision, revision_size, "%s", request_play_catalog_revision);
    }
    pthread_mutex_unlock(&status_mutex);
    return result;
}

/* Status JSON includes three 511-byte escaped metadata strings, a catalog
 * revision that can expand to 378 escaped bytes, and bounded player, accent,
 * model, release, sorting and collection-job fields. The error has at most 127
 * bytes (762 escaped); 5 KiB covers all bounded fields with room to spare. */
#define STATUS_JSON_CAPACITY 5120u

/* Catalog revision as a JSON value ("null" when there is none). The catalog lock must
 * be acquired before status_mutex. Library refresh paths publish status after metadata
 * work, so reversing this order could deadlock a status request against a rebuild. */
static void status_catalog_revision_json(char * out, size_t out_size) {
    char catalog_revision[METADATA_DB_CATALOG_REVISION_SIZE] = {0};
    char revision_esc[METADATA_DB_CATALOG_REVISION_SIZE * 6] = {0};
    snprintf(out, out_size, "null");
    if (!metadata_db_catalog_get_revision(catalog_revision, sizeof(catalog_revision))) return;
    json_escape_append(revision_esc, sizeof(revision_esc), catalog_revision);
    snprintf(out, out_size, "\"%s\"", revision_esc);
}

/* Formats the status JSON; takes status_mutex itself for the playback snapshot. */
static void status_format(char * out, size_t out_size, const char * revision_json, bool bluetooth_audio_output,
                          unsigned settings_revision, unsigned settings_epoch) {
    char title_esc[512] = {0}, artist_esc[512] = {0}, album_esc[512] = {0};
    char model_esc[128] = {0}, version_esc[128] = {0};
    char accent_color[12] = {0};
    char job_error[768] = {0};
    const char * accent_json = "null";
    json_escape_append(model_esc, sizeof(model_esc), app_board_display_name());
    json_escape_append(version_esc, sizeof(version_esc), app_version_label());
    pthread_mutex_lock(&status_mutex);
    json_escape_append(title_esc, sizeof(title_esc), status_title);
    json_escape_append(artist_esc, sizeof(artist_esc), status_artist);
    json_escape_append(album_esc, sizeof(album_esc), status_album);
    json_escape_append(job_error, sizeof(job_error), collection_job_error);
    if (status_accent_valid) {
        snprintf(accent_color, sizeof(accent_color), "\"#%06lX\"", (unsigned long) (status_accent_rgb & 0xFFFFFF));
        accent_json = accent_color;
    }
    snprintf(out, out_size,
             "{\"playing\":%s,\"paused\":%s,\"title\":\"%s\",\"artist\":\"%s\",\"album\":\"%s\","
             "\"position_seconds\":%d,\"duration_seconds\":%d,\"volume\":%.2f,\"play_mode\":%d,"
             "\"codec\":\"%s\",\"source_bit_depth\":%u,\"source_sample_rate\":%u,"
             "\"catalog_revision\":%s,\"features\":{\"plugin_store\":true,\"plugin_management\":true,\"player_layouts\":true},"
             "\"bluetooth_audio_output\":%s,\"bluetooth_dac_mode\":%s,\"favorite\":%s,\"accent_color\":%s,"
             "\"device_model\":\"%s\",\"player_version\":\"%s\",\"settingsRevision\":%u,\"settingsEpoch\":%u,"
             "\"sort_modes\":[%d,%d,%d],\"collection_job\":{\"id\":%u,\"state\":\"%s\",\"error\":\"%s\"}}",
             status_playing ? "true" : "false", status_paused ? "true" : "false", title_esc, artist_esc, album_esc,
             status_position_seconds, status_duration_seconds, (double) status_volume, status_play_mode,
             status_codec, status_source_bit_depth, status_source_sample_rate, revision_json,
             bluetooth_audio_output ? "true" : "false", status_bluetooth_dac_mode ? "true" : "false", status_favorite ? "true" : "false", accent_json,
             model_esc, version_esc, settings_revision, settings_epoch, status_library_sorting[0], status_library_sorting[1], status_library_sorting[2],
             collection_job_id, collection_job_busy ? "pending" : collection_job_failed ? "error" : collection_job_id ? "done" : "idle", job_error);
    pthread_mutex_unlock(&status_mutex);
}

/* GET /api/status */
void handle_status_get(int cfd) {
    char out[STATUS_JSON_CAPACITY];
    char revision_json[METADATA_DB_CATALOG_REVISION_SIZE * 6 + 3];
    status_catalog_revision_json(revision_json, sizeof(revision_json));
    /* Query the route before taking status_mutex. Route selection has its own
     * lock and must not be nested with the playback-status snapshot lock. */
#ifndef HOST_BUILD
    bool bluetooth_audio_output = audio_output_is_bluetooth_requested();
#else
    bool bluetooth_audio_output = false; /* host builds have no Bluetooth route */
#endif
    unsigned settings_epoch = settings_registry_epoch();
    unsigned settings_revision = settings_registry_revision(); /* own lock; not nested under status_mutex */
    status_format(out, sizeof(out), revision_json, bluetooth_audio_output, settings_revision, settings_epoch);
    send_response(cfd, "200 OK", "application/json", out);
}

static void build_queue_json(char * out, size_t out_size, int offset, int limit) {
    /* Snapshot queue_song_ids under status_mutex, then resolve IDs via
     * metadata_db_get_songs_by_ids() outside the lock. */
    pthread_mutex_lock(&status_mutex);
    int total = queue_song_id_count;
    uint64_t revision = queue_snapshot_revision;
    if (offset > total) offset = total;
    int count = total - offset;
    if (count > limit) count = limit;
    int64_t * ids = NULL;
    if (count > 0) {
        ids = malloc(sizeof(int64_t) * (size_t) count);
        if (ids) memcpy(ids, queue_song_ids + offset, sizeof(int64_t) * (size_t) count);
        else count = 0;
    }
    pthread_mutex_unlock(&status_mutex);

    song_row_t * rows = count > 0 ? malloc(sizeof(song_row_t) * (size_t) count) : NULL;
    if (count > 0 && !rows) count = 0;
    for (int i = 0; rows && i < count; i++) rows[i].id = -1;
    int resolved_count = 0;
    int64_t * resolved_ids = count > 0 ? malloc(sizeof(int64_t) * (size_t) count) : NULL;
    int * resolved_at = count > 0 ? malloc(sizeof(int) * (size_t) count) : NULL;
    if (resolved_ids && resolved_at) {
        for (int i = 0; i < count; i++) if (ids[i] >= 0) {
            resolved_ids[resolved_count] = ids[i];
            resolved_at[resolved_count++] = i;
        }
        if (resolved_count) {
            song_row_t * resolved = malloc(sizeof(song_row_t) * (size_t) resolved_count);
            if (resolved) {
                metadata_db_get_songs_by_ids(resolved_ids, resolved_count, resolved);
                for (int i = 0; i < resolved_count; i++) rows[resolved_at[i]] = resolved[i];
                free(resolved);
            }
        }
    }

    /* Build JSON array of queue songs using json_builder_t with bounds checking. */
    json_builder_t json;
    json_builder_init(&json, out, out_size);
    json_builder_appendf(&json, "{\"total\":%d,\"offset\":%d,\"revision\":%llu,\"songs\":[", total, offset,
                         (unsigned long long) revision);
    for (int i = 0; i < count && !json.truncated; i++) {
        if (ids[i] < 0 || !rows || rows[i].id == -1) {
            if (!json_builder_appendf(&json, "%s{\"offset\":%d,\"index\":-1,\"title\":\"Unknown track\",\"artist\":\"\"}", i ? "," : "", offset + i)) break;
            continue;
        }
        char display_title[128], title[512] = {0}, artist[512] = {0};
        metadata_db_song_display_title(&rows[i], display_title, sizeof(display_title));
        json_escape_append(title, sizeof(title), display_title);
        json_escape_append(artist, sizeof(artist), rows[i].tags.artist);
        if (!json_builder_appendf(&json, "%s{\"offset\":%d,\"index\":%lld,\"title\":\"%s\",\"artist\":\"%s\"}",
                                  i ? "," : "", offset + i, (long long) rows[i].id, title, artist)) break;
    }
    json.truncated = false;
    json_builder_appendf(&json, "]}");
    free(rows);
    free(ids);
    free(resolved_ids);
    free(resolved_at);
}

/* ---- Queue move / play ---- */

/* True while a queue edit is waiting for the UI thread. status_mutex held. */
static bool queue_edit_pending_locked(void) {
    return request_has_queue_move || request_has_queue_play || request_has_queue_remove || request_queue_clear;
}

/* Why a queue edit against `revision` cannot be accepted yet, or NULL.
 * status_mutex held. */
static const char * queue_edit_conflict_locked(uint64_t revision) {
    if (revision != queue_snapshot_revision) return "revision_mismatch";
    return queue_edit_pending_locked() ? "edit_pending" : NULL;
}

/* One JSON error body for every rejected queue edit: 400 for a bad offset,
 * 409 for a conflict the client resolves by rereading the queue. */
static void send_queue_edit_error(int cfd, const char * code) {
    send_api_error(cfd, strcmp(code, "invalid_offset") == 0 ? "400 Bad Request" : "409 Conflict", "queue_error", code);
}

/* POST /api/queue/move?from=N&to=N&revision=R and
 * POST /api/queue/play?offset=N&revision=R. Offsets index the Up Next list
 * of that revision. Accepted requests are applied on the UI thread, which
 * rechecks the revision and ignores the request if the queue changed. */
void handle_queue_edit_request(int cfd, const char * path, bool move) {
    uint64_t from = 0, to = 0, revision = 0;
    bool valid = query_required_number(path, "revision", UINT64_MAX, &revision) &&
                 (move ? query_required_number(path, "from", INT_MAX, &from) &&
                         query_required_number(path, "to", INT_MAX, &to)
                       : query_required_number(path, "offset", INT_MAX, &from));
    if (!valid) {
        send_api_error(cfd, "400 Bad Request", "queue_error", "invalid_request");
        return;
    }
    const char * code = NULL;
    pthread_mutex_lock(&status_mutex);
    /* One edit per published revision: a second one would overwrite the
     * first before the UI applies it. The client rereads /queue. */
    code = queue_edit_conflict_locked(revision);
    if (!code && (from >= (uint64_t) queue_song_id_count || (move && to >= (uint64_t) queue_song_id_count)))
        code = "invalid_offset";
    if (code) {
        /* rejected below */
    } else if (move) {
        request_has_queue_move = true;
        request_queue_move_from = (int) from;
        request_queue_move_to = (int) to;
        request_queue_move_revision = revision;
    } else {
        request_has_queue_play = true;
        request_queue_play_offset = (int) from;
        request_queue_play_revision = revision;
    }
    pthread_mutex_unlock(&status_mutex);
    if (!code) send_response(cfd, "200 OK", "text/plain", "OK");
    else send_queue_edit_error(cfd, code);
}

/* Scoped album ordering stays on the player so cached phone catalogs need no
 * timestamp/schema migration. UI-owned settings reach this worker by snapshot. */
static int remote_sort_scope(const char *name) {
    if (strcmp(name, "albums") == 0) return 0;
    if (strcmp(name, "artists") == 0) return 1;
    if (strcmp(name, "album_artists") == 0) return 2;
    return -1;
}

static void build_library_sorting_json(char *out, size_t size) {
    int configured[3];
    pthread_mutex_lock(&status_mutex);
    memcpy(configured, status_library_sorting, sizeof(configured));
    pthread_mutex_unlock(&status_mutex);
    bool recent = metadata_db_album_sort_available(METADATA_DB_ALBUM_SORT_RECENT);
    bool year = metadata_db_album_sort_available(METADATA_DB_ALBUM_SORT_YEAR);
    const char *names[] = {"albums", "artists", "album_artists"};
    json_builder_t json;
    json_builder_init(&json, out, size);
    json_builder_appendf(&json, "{");
    for (int i = 0; i < 3; ++i) {
        int mode = configured[i];
        if (mode < 0 || mode > 7 || (mode & ~METADATA_DB_ALBUM_SORT_REVERSE) > (i == 0 ? 3 : 2)) mode = 0;
        int effective = metadata_db_album_sort_available((metadata_db_album_sort_t) mode)
                            ? mode : (mode & METADATA_DB_ALBUM_SORT_REVERSE);
        json_builder_appendf(&json, "%s\"%s\":{\"configured\":%d,\"effective\":%d,\"reverse\":%s,\"available\":[true,%s,%s]}",
                             i ? "," : "", names[i], mode & ~METADATA_DB_ALBUM_SORT_REVERSE,
                             effective & ~METADATA_DB_ALBUM_SORT_REVERSE,
                             mode & METADATA_DB_ALBUM_SORT_REVERSE ? "true" : "false",
                             recent ? "true" : "false", year ? "true" : "false");
    }
    json_builder_appendf(&json, "}");
}

/* Copies src into dst when it fits including the NUL; false otherwise. */
static bool copy_fitting(char * dst, size_t size, const char * src) {
    size_t len = strlen(src);
    if (len >= size) return false;
    memcpy(dst, src, len + 1);
    return true;
}

/* GET/POST /api/library/sorting */
void handle_library_sorting_route(int fd, const char *method, const char *path) {
    if (strcmp(method, "GET") == 0) {
        char json[512]; build_library_sorting_json(json, sizeof(json));
        send_response(fd, "200 OK", "application/json", json);
    } else if (strcmp(method, "POST") == 0) {
        char scope_name[32]; uint64_t mode, reverse = 0;
        bool has_reverse = query_param_present(path, "reverse");
        int scope = query_decoded(path, "scope", scope_name, sizeof(scope_name)) == 1 ? remote_sort_scope(scope_name) : -1;
        if (scope < 0 || !query_required_number(path, "mode", 2, &mode) ||
            (has_reverse && !query_required_number(path, "reverse", 1, &reverse))) {
            catalog_send_error(fd, "400 Bad Request", "invalid_sorting"); return;
        }
        pthread_mutex_lock(&status_mutex);
        if (!has_reverse) reverse = (status_library_sorting[scope] & METADATA_DB_ALBUM_SORT_REVERSE) != 0;
        pthread_mutex_unlock(&status_mutex);
        if (reverse) mode |= METADATA_DB_ALBUM_SORT_REVERSE;
        if (!metadata_db_album_sort_available((metadata_db_album_sort_t) mode)) {
            catalog_send_error(fd, "409 Conflict", "sort_unavailable"); return;
        }
        pthread_mutex_lock(&status_mutex);
        bool busy = request_has_library_sorting;
        if (!busy) { request_sort_scope = scope; request_sort_mode = (int) mode; request_has_library_sorting = true; }
        pthread_mutex_unlock(&status_mutex);
        if (busy) send_reason_error(fd, "409 Conflict", "sorting_busy");
        else send_response(fd, "202 Accepted", "application/json", "{\"accepted\":true}");
    } else send_response(fd, "405 Method Not Allowed", "text/plain", "Method not allowed");
}

/* GET /api/library/albums?scope=...: albums of one scope in the configured sort order. */
void handle_scoped_albums_route(int fd, const char *method, const char *path) {
    if (strcmp(method, "GET") != 0) { send_response(fd, "405 Method Not Allowed", "text/plain", "Method not allowed"); return; }
    char scope_name[32], artist[128], album_artist[128];
    int scope = query_decoded(path, "scope", scope_name, sizeof(scope_name)) == 1 ? remote_sort_scope(scope_name) : -1;
    int artist_present = query_decoded(path, "artist", artist, sizeof(artist));
    int album_artist_present = query_decoded(path, "album_artist", album_artist, sizeof(album_artist));
    uint64_t offset = 0, limit = 25;
    if (scope < 0 || artist_present < 0 || album_artist_present < 0 ||
        (scope == 0 && (artist_present || album_artist_present)) ||
        (scope == 1 && (artist_present != 1 || !artist[0] || album_artist_present)) ||
        (scope == 2 && (album_artist_present != 1 || !album_artist[0] || artist_present)) ||
        (query_param_present(path, "offset") && !query_required_number(path, "offset", INT_MAX, &offset)) ||
        (query_param_present(path, "limit") && (!query_required_number(path, "limit", 100, &limit) || !limit))) {
        catalog_send_error(fd, "400 Bad Request", "invalid_album_query"); return;
    }
    if (limit > 25) limit = 25;
    pthread_mutex_lock(&status_mutex);
    int configured = status_library_sorting[scope];
    pthread_mutex_unlock(&status_mutex);
    if (configured < 0 || configured > 7 ||
        (configured & ~METADATA_DB_ALBUM_SORT_REVERSE) > (scope == 0 ? 3 : 2)) configured = 0;
    int mode = metadata_db_album_sort_available((metadata_db_album_sort_t) configured)
                   ? configured : (configured & METADATA_DB_ALBUM_SORT_REVERSE);
    metadata_db_catalog_page_t identity = {0};
    metadata_db_catalog_song_t sample;
    if (metadata_db_catalog_songs_page(NULL, 0, 1, &identity, &sample) != METADATA_DB_CATALOG_OK) {
        catalog_send_error(fd, "503 Service Unavailable", "library_unavailable"); return;
    }
    group_row_t *groups = calloc((size_t) limit, sizeof(*groups));
    size_t capacity = (size_t) limit * 2048 + 512;
    char *out = malloc(capacity);
    if (!groups || !out) { free(groups); free(out); catalog_send_error(fd, "503 Service Unavailable", "allocation_failed"); return; }
    int64_t total;
    int count;
    if (scope == 0) {
        int artists = 0, album_artists = 0, albums = 0;
        metadata_db_get_group_counts(&artists, &album_artists, &albums);
        total = albums;
        count = metadata_db_get_albums_page_sorted(NULL, (metadata_db_album_sort_t) mode, (int) offset, (int) limit, groups);
    } else {
        metadata_db_group_kind_t kind = scope == 1 ? METADATA_DB_GROUP_ARTIST : METADATA_DB_GROUP_ALBUM_ARTIST;
        const char *name = scope == 1 ? artist : album_artist;
        total = metadata_db_count_albums_for_group(kind, name);
        count = metadata_db_get_albums_for_group_sorted(kind, name, (metadata_db_album_sort_t) mode, (int) offset, (int) limit, groups);
    }
    if (!metadata_db_catalog_revision_matches(identity.revision) || count < 0 || total < 0) {
        free(groups); free(out); catalog_send_error(fd, "409 Conflict", "library_changed"); return;
    }
    if (offset > (uint64_t) total) {
        free(groups); free(out); catalog_send_error(fd, "400 Bad Request", "invalid_offset"); return;
    }
    json_builder_t json; json_builder_init(&json, out, capacity);
    json_builder_appendf(&json, "{\"total\":%lld,\"offset\":%llu,\"next_offset\":%llu,\"revision\":\"%s:%d:%d:%d\",\"groups\":[",
                         (long long) total, (unsigned long long) offset, (unsigned long long) (offset + (unsigned) count), identity.revision, scope, configured, mode);
    for (int i = 0; i < count; ++i) {
        char name[768] = {0}, aa[768] = {0};
        json_escape_append(name, sizeof(name), groups[i].name);
        json_escape_append(aa, sizeof(aa), groups[i].album_artist);
        json_builder_appendf(&json, "%s{\"name\":\"%s\",\"count\":%d,\"index\":%lld,\"album_artist\":\"%s\"}",
                             i ? "," : "", name, groups[i].song_count, (long long) groups[i].first_song_id, aa);
    }
    json_builder_appendf(&json, "]}");
    if (json.truncated) catalog_send_error(fd, "503 Service Unavailable", "response_too_large");
    else send_response(fd, "200 OK", "application/json", out);
    free(groups); free(out);
}

/* POST /api/playback/collection */
void handle_collection_play_route(int fd, const char *method, const char *path) {
    if (strcmp(method, "POST") != 0) { send_response(fd, "405 Method Not Allowed", "text/plain", "Method not allowed"); return; }
    remote_collection_play_request_t request = {0};
    const char *scopes[] = {"all", "album", "artist", "album_artist", "genre", "playlist", "favorites", "recently_added", "recently_played"};
    char scope_name[32]; uint64_t mode; int scope = -1;
    if (query_decoded(path, "scope", scope_name, sizeof(scope_name)) == 1) {
        for (unsigned i = 0; i < sizeof(scopes) / sizeof(scopes[0]); ++i) if (strcmp(scopes[i], scope_name) == 0) scope = (int) i;
    }
    if (scope < 0 || !query_required_number(path, "mode", 4, &mode) ||
        query_decoded(path, "name", request.name, sizeof(request.name)) < 0 ||
        query_decoded(path, "artist", request.artist, sizeof(request.artist)) < 0 ||
        query_decoded(path, "album_artist", request.album_artist, sizeof(request.album_artist)) < 0 ||
        !catalog_query_revision_named(path, "catalog_revision", true, request.expected_revision) ||
        (scope >= 1 && scope <= 5 && !request.name[0]) ||
        (scope == REMOTE_COLLECTION_ALBUM && !request.artist[0] && !query_param_present(path, "album_artist")) ||
        (request.artist[0] && query_param_present(path, "album_artist")) ||
        (scope == 5 && (!playlist_name_is_safe(request.name) || strlen(request.name) > PLAYLIST_NAME_MAX_BYTES))) {
        catalog_send_error(fd, "400 Bad Request", "invalid_collection"); return;
    }
    request.scope = (remote_collection_scope_t) scope; request.play_mode = (int) mode;
    bool name_fits = true;
    if (scope == REMOTE_COLLECTION_ALBUM) name_fits = copy_fitting(request.album, sizeof(request.album), request.name);
    else if (scope == REMOTE_COLLECTION_ARTIST) name_fits = copy_fitting(request.artist, sizeof(request.artist), request.name);
    else if (scope == REMOTE_COLLECTION_ALBUM_ARTIST) name_fits = copy_fitting(request.album_artist, sizeof(request.album_artist), request.name);
    else if (scope == REMOTE_COLLECTION_GENRE) memcpy(request.genre, request.name, strlen(request.name) + 1);
    if (!name_fits) { catalog_send_error(fd, "400 Bad Request", "invalid_collection"); return; }
    if (request.artist[0] && request.album_artist[0]) { catalog_send_error(fd, "400 Bad Request", "conflicting_filters"); return; }
    pthread_mutex_lock(&status_mutex);
    bool busy = collection_job_busy;
    if (!busy) {
        request_collection = request; request_has_collection = true; collection_job_busy = true; collection_job_failed = false; collection_job_error[0] = '\0';
        collection_job_id = collection_job_id >= INT_MAX ? 1 : collection_job_id + 1;
    }
    unsigned id = collection_job_id;
    pthread_mutex_unlock(&status_mutex);
    char body[64]; snprintf(body, sizeof(body), "{\"id\":%u}", id);
    if (busy) send_reason_error(fd, "409 Conflict", "collection_busy");
    else send_response(fd, "202 Accepted", "application/json", body);
}

/* POST /api/import/start and /stop. Returns false for any other request. */
bool handle_import_lifecycle_route(int cfd, const char * method, const char * path_only) {
    if (strcmp(method, "POST") != 0 ||
        (strcmp(path_only, "/api/import/start") != 0 && strcmp(path_only, "/api/import/stop") != 0)) return false;
    bool start = strcmp(path_only, "/api/import/start") == 0;
    bool conflict = false;
    pthread_mutex_lock(&status_mutex);
    if (request_has_import || remote_import_state == REMOTE_IMPORT_STARTING ||
        remote_import_state == REMOTE_IMPORT_STOPPING) {
        conflict = true;
    } else if ((start && remote_import_state == REMOTE_IMPORT_READY) ||
               (!start && remote_import_state == REMOTE_IMPORT_STOPPED)) {
        /* Repeated requests for the already-achieved state are safe. */
    } else {
        request_has_import = true;
        request_import_start = start;
        remote_import_state = start ? REMOTE_IMPORT_STARTING : REMOTE_IMPORT_STOPPING;
        remote_import_error[0] = '\0';
    }
    pthread_mutex_unlock(&status_mutex);
    if (conflict) {
        send_api_error(cfd, "409 Conflict", "import_service_busy", "busy");
    } else {
        send_response(cfd, "202 Accepted", "application/json", "{\"accepted\":true}");
    }
    return true;
}

/* GET /api/import */
void handle_import_get(int cfd) {
    remote_import_state_t state;
    char error[sizeof(remote_import_error)];
    remote_control_get_import_state(&state, error, sizeof(error));
    const char *state_name = "stopped";
    switch (state) {
        case REMOTE_IMPORT_STARTING: state_name = "starting"; break;
        case REMOTE_IMPORT_READY: state_name = "ready"; break;
        case REMOTE_IMPORT_STOPPING: state_name = "stopping"; break;
        case REMOTE_IMPORT_ERROR: state_name = "error"; break;
        case REMOTE_IMPORT_STOPPED: break;
    }
    char error_escaped[sizeof(error) * 6] = {0};
    json_escape_append(error_escaped, sizeof(error_escaped), error);
    char json[sizeof(error_escaped) + 128];
    snprintf(json, sizeof(json), "{\"state\":\"%s\",\"error\":%s%s%s,\"port\":8899}",
             state_name, error[0] ? "\"" : "null", error[0] ? error_escaped : "",
             error[0] ? "\"" : "");
    send_response(cfd, "200 OK", "application/json", json);
}

/* GET /api/queue */
void handle_queue_get(int cfd, const char * path) {
    int offset = 0, limit = 25;
    if (query_param_int_saturating(path, "offset", &offset) && offset < 0) offset = 0;
    if (query_param_int_saturating(path, "limit", &limit) && limit < 1) limit = 25;
    if (limit > 25) limit = 25;
    /* Heap, not stack: a 64 KiB frame here would leave too little
     * headroom on the worker-thread stacks for the metadata_db query
     * paths build_queue_json() reaches. */
    char * json = json_alloc_or_500(cfd, 65536);
    if (json) {
        build_queue_json(json, 65536, offset, limit);
        send_json_free(cfd, json);
    }
}

/* Validates the optional catalog_revision of POST /api/playback/play and
 * /api/playback/queue. Returns false when the request was answered with an
 * error; *out_present says whether a revision was supplied. */
static bool validate_catalog_action(int cfd, const char * path_only, const char * path,
                                    char out_revision[METADATA_DB_CATALOG_REVISION_SIZE], bool * out_present) {
    bool is_catalog_id_action = strcmp(path_only, "/api/playback/play") == 0 ||
                                strcmp(path_only, "/api/playback/queue") == 0;
    *out_present = false;
    if (!is_catalog_id_action || !query_param_present(path, "catalog_revision")) return true;
    *out_present = true;
    uint64_t index = 0;
    if (!catalog_query_revision_named(path, "catalog_revision", true, out_revision) ||
        !catalog_query_number(path, "index", 0, INT32_MAX, &index) || index == 0) {
        catalog_send_error(cfd, "400 Bad Request", "invalid_catalog_action");
        return false;
    }
    metadata_db_catalog_result_t validation = metadata_db_catalog_validate_song_revision(
        out_revision, (int64_t) index);
    if (validation == METADATA_DB_CATALOG_STALE) {
        catalog_send_error(cfd, "409 Conflict", "revision_mismatch");
        return false;
    }
    if (validation != METADATA_DB_CATALOG_OK) {
        catalog_send_error(cfd, "503 Service Unavailable", "temporary_unavailable");
        return false;
    }
    return true;
}

/* Publishes one playback or queue request for the UI thread to consume.
 * Caller holds status_mutex. Returns false for an unknown route or invalid
 * parameters; *reject receives the error code when a queue request is rejected
 * or offset. */
static bool set_playback_request_locked(const char * path_only, const char * path, bool catalog_revision_present,
                                        const char * catalog_revision, const char ** reject) {
    bool ok = true;
    if (strcmp(path_only, "/api/playback/toggle") == 0) {
        request_play_pause = true;
    } else if (strcmp(path_only, "/api/screenshot") == 0) {
        request_screenshot = true;
    } else if (strcmp(path_only, "/api/playback/next") == 0) {
        request_next = true;
    } else if (strcmp(path_only, "/api/playback/prev") == 0) {
        request_prev = true;
    } else if (strcmp(path_only, "/api/playback/mode") == 0) {
        char mode_raw[32] = {0};
        bool mode_present = query_param_str(path, "mode", mode_raw, sizeof(mode_raw));
        if (!mode_present) {
            request_mode_cycle = true;
        } else {
            char * end = NULL;
            errno = 0;
            long mode = strtol(mode_raw, &end, 10);
            if (errno == 0 && end != mode_raw && *end == '\0' && mode >= 0 && mode <= 4) {
                request_has_play_mode = true;
                request_play_mode = (int) mode;
            } else {
                ok = false;
            }
        }
    } else if (strcmp(path_only, "/api/playback/seek") == 0) {
        int seconds;
        if (query_param_int(path, "seconds", &seconds) && seconds >= 0) {
            request_has_seek = true;
            request_seek_seconds = seconds;
        } else {
            ok = false;
        }
    } else if (strcmp(path_only, "/api/playback/volume") == 0) {
        int percent;
        if (query_param_int(path, "percent", &percent) && percent >= 0 && percent <= 100) {
            request_has_volume = true;
            request_volume_percent = percent;
        } else {
            ok = false;
        }
    } else if (strcmp(path_only, "/api/playback/play") == 0) {
        int64_t index;
        if (query_param_int64(path, "index", &index) && index >= 0) {
            /* A scope value that does not fit rejects the request: playing
             * without it would queue more than the client asked for. Parsed
             * aside so a rejected request leaves a pending one intact. */
            char playlist[sizeof(request_play_playlist_name)], artist[sizeof(request_play_artist_filter)];
            char album_artist[sizeof(request_play_album_artist_filter)], album[sizeof(request_play_album_filter)];
            if (query_loose(path, "playlist", playlist, sizeof(playlist)) < 0 ||
                query_loose(path, "artist", artist, sizeof(artist)) < 0 ||
                query_loose(path, "album_artist", album_artist, sizeof(album_artist)) < 0 ||
                query_loose(path, "album", album, sizeof(album)) < 0) {
                ok = false;
            } else {
                request_has_play_index = true;
                request_play_index = index;
                snprintf(request_play_catalog_revision, sizeof(request_play_catalog_revision), "%s",
                         catalog_revision_present ? catalog_revision : "");
                snprintf(request_play_playlist_name, sizeof(request_play_playlist_name), "%s", playlist);
                snprintf(request_play_artist_filter, sizeof(request_play_artist_filter), "%s", artist);
                snprintf(request_play_album_artist_filter, sizeof(request_play_album_artist_filter), "%s", album_artist);
                snprintf(request_play_album_filter, sizeof(request_play_album_filter), "%s", album);
            }
        } else {
            ok = false;
        }
    } else if (strcmp(path_only, "/api/playback/queue") == 0) {
        int64_t index;
        if (query_param_int64(path, "index", &index) && index >= 0) {
            request_has_queue_index = true;
            request_queue_index = index;
            snprintf(request_queue_catalog_revision, sizeof(request_queue_catalog_revision), "%s",
                     catalog_revision_present ? catalog_revision : "");
        } else {
            ok = false;
        }
    } else if (strcmp(path_only, "/api/queue/remove") == 0) {
        int offset;
        int64_t revision;
        if (query_param_int(path, "offset", &offset) && offset >= 0 &&
            query_param_int64(path, "revision", &revision) && revision >= 0) {
            const char * code = queue_edit_conflict_locked((uint64_t) revision);
            if (!code && offset >= queue_song_id_count) code = "invalid_offset";
            if (code) {
                ok = false;
                *reject = code;
            } else {
                request_has_queue_remove = true;
                request_queue_remove_offset = offset;
                request_queue_remove_revision = (uint64_t) revision;
            }
        } else ok = false;
    } else if (strcmp(path_only, "/api/queue/clear") == 0) {
        int64_t revision;
        if (query_param_int64(path, "revision", &revision) && revision >= 0) {
            const char * code = queue_edit_conflict_locked((uint64_t) revision);
            if (code) {
                ok = false;
                *reject = code;
            } else {
                request_queue_clear = true;
                request_queue_clear_revision = (uint64_t) revision;
            }
        } else ok = false;
    } else {
        ok = false;
    }
    return ok;
}

/* The original POST playback and queue routes; any other POST is a 404. */
void handle_legacy_playback_post(int cfd, const char * path_only, const char * path) {
    bool catalog_revision_present = false;
    char catalog_revision[METADATA_DB_CATALOG_REVISION_SIZE] = {0};
    if (!validate_catalog_action(cfd, path_only, path, catalog_revision, &catalog_revision_present)) return;
    const char * reject = NULL;
    pthread_mutex_lock(&status_mutex);
    bool ok = set_playback_request_locked(path_only, path, catalog_revision_present, catalog_revision, &reject);
    pthread_mutex_unlock(&status_mutex);

    if (ok) {
        send_response(cfd, "200 OK", "text/plain", "OK");
    } else if (reject) {
        send_queue_edit_error(cfd, reject);
    } else {
        send_response(cfd, "404 Not Found", "text/plain", "Not Found");
    }
}
