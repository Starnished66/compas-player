#include "remote_control.h"
#include "remote_control_mdns.h"
#include "remote_control_webapp.h"
#include "metadata.h"
#include "metadata_db.h"
#include "tagcache.h"
#include "albumart.h"
#include "artwork_coordinator.h"
#include "catalog_source_cache.h"
#include "playlist_files.h"
#include "file_browser.h"
#include "library_endian.h"
#include "favorite_writer.h"
#include "gui_library.h" /* sd_card_root_is_mounted() */
#include "settings.h"
#include "audio.h"
#ifndef HOST_BUILD
#include "audio_output.h" /* target-only, like audio.c's own audio_output_* calls */
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/time.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Port 8899 -- verified free on a real device; 4399 is already used by
 * Import via Wi-Fi's thttpd. */
#define REMOTE_CONTROL_PORT 8899

/* Same definition as gui.c's own MUSIC_ROOT_DIR/PLAYLISTS_DIR, duplicated
 * since gui.c doesn't expose either publicly. Must stay in sync. */
#ifdef HOST_BUILD
#define MUSIC_ROOT_DIR "./music"
#else
#define MUSIC_ROOT_DIR "/data/mnt/sd_0"
#endif
#define PLAYLISTS_DIR MUSIC_ROOT_DIR "/Playlists"

static pthread_mutex_t status_mutex = PTHREAD_MUTEX_INITIALIZER;
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
/* Source format is copied into this API snapshot before status_mutex is
 * acquired. Never call into audio.c while holding status_mutex: audio and UI
 * paths may acquire their own locks in the opposite order. */
static char status_codec[32] = {0};
static unsigned int status_source_bit_depth = 0;
static unsigned int status_source_sample_rate = 0;

static const char * remote_codec_name(audio_codec_t codec) {
    switch (codec) {
        case AUDIO_CODEC_FLAC: return "FLAC";
        case AUDIO_CODEC_MP3: return "MP3";
        case AUDIO_CODEC_PCM: return "PCM";
        case AUDIO_CODEC_DSD: return "DSD";
        case AUDIO_CODEC_AAC: return "AAC";
        case AUDIO_CODEC_ALAC: return "ALAC";
        case AUDIO_CODEC_APE: return "APE";
        case AUDIO_CODEC_WMA: return "WMA";
        case AUDIO_CODEC_OPUS: return "Opus";
        case AUDIO_CODEC_VORBIS: return "Vorbis";
        case AUDIO_CODEC_WAVPACK: return "WavPack";
        case AUDIO_CODEC_UNKNOWN: break;
    }
    return "";
}

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

static atomic_bool running = false;
static int listen_fd = -1;
static pthread_t listener_thread;
static pthread_mutex_t listener_mutex = PTHREAD_MUTEX_INITIALIZER;
static int active_client_fd = -1;

/* The PIN is shared by Wi-Fi and RFCOMM because both transports carry the
 * same HTTP request format. Keep reads and UI writes serialized. */
static pthread_mutex_t pin_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned int pin_failures = 0;
static char pin_failed_candidates[5][REMOTE_CONTROL_PIN_MAX_LENGTH + 1];
static time_t pin_failure_window = 0;
static time_t pin_blocked_until = 0;

#define REMOTE_CONTROL_PIN_MIN_LENGTH 4

static bool remote_control_pin_is_valid(const char * pin) {
    if (!pin) return false;
    size_t len = strlen(pin);
    if (len < REMOTE_CONTROL_PIN_MIN_LENGTH || len > REMOTE_CONTROL_PIN_MAX_LENGTH) return false;
    for (size_t i = 0; i < len; i++) if (pin[i] < '0' || pin[i] > '9') return false;
    return true;
}

/* The fixed PIN earlier builds seeded as a placeholder for Android app
 * testing. Every device shipped that same value, so a saved copy is treated
 * as unset and replaced with a random PIN once. */
#define REMOTE_CONTROL_LEGACY_PLACEHOLDER_PIN "0000"
#define REMOTE_CONTROL_GENERATED_PIN_DIGITS 6

/* Fills out[] with a uniformly random REMOTE_CONTROL_GENERATED_PIN_DIGITS
 * PIN from the kernel CSPRNG. Rejection sampling keeps every PIN equally
 * likely (a plain modulo of a 32-bit value would favor low PINs). */
static bool remote_control_random_pin(char * out, size_t out_size) {
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    const uint32_t range = 1000000u;
    const uint32_t limit = UINT32_MAX - (UINT32_MAX % range);
    bool ok = false;
    for (int attempt = 0; attempt < 16 && !ok; attempt++) {
        uint32_t value = 0;
        size_t got = 0;
        while (got < sizeof(value)) {
            ssize_t n = read(fd, (uint8_t *) &value + got, sizeof(value) - got);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            got += (size_t) n;
        }
        if (got != sizeof(value)) break;
        if (value >= limit) continue;
        snprintf(out, out_size, "%0*u", REMOTE_CONTROL_GENERATED_PIN_DIGITS, (unsigned) (value % range));
        ok = true;
    }
    close(fd);
    return ok;
}

/* Caller holds pin_mutex. A new PIN invalidates everything learned about the
 * old one, including an active lockout. */
static void remote_control_store_pin_locked(const char * pin) {
    snprintf(current_settings.remote_control_pin, sizeof(current_settings.remote_control_pin), "%s", pin);
    pin_failures = 0;
    memset(pin_failed_candidates, 0, sizeof(pin_failed_candidates));
    pin_failure_window = 0;
    pin_blocked_until = 0;
    settings_save_async(&current_settings);
}

/* Generates the PIN once on first use and reuses it after that. A saved
 * 4-12 digit PIN is kept, except the old shared placeholder. Returns false
 * only when no PIN exists and the CSPRNG cannot be read; callers then refuse
 * to serve rather than fall back to a guessable value. */
static bool remote_control_ensure_pin(void) {
    pthread_mutex_lock(&pin_mutex);
    const char * saved = current_settings.remote_control_pin;
    if (remote_control_pin_is_valid(saved) && strcmp(saved, REMOTE_CONTROL_LEGACY_PLACEHOLDER_PIN) != 0) {
        pthread_mutex_unlock(&pin_mutex);
        return true;
    }

    char pin[REMOTE_CONTROL_PIN_MAX_LENGTH + 1];
    bool ok = remote_control_random_pin(pin, sizeof(pin));
    if (ok) remote_control_store_pin_locked(pin);
    pthread_mutex_unlock(&pin_mutex);
    return ok;
}

bool remote_control_generate_new_pin(void) {
    char pin[REMOTE_CONTROL_PIN_MAX_LENGTH + 1];
    pthread_mutex_lock(&pin_mutex);
    bool ok = remote_control_random_pin(pin, sizeof(pin));
    /* Never "regenerate" to the same value, which would look like a no-op. */
    if (ok && strcmp(pin, current_settings.remote_control_pin) == 0)
        ok = remote_control_random_pin(pin, sizeof(pin));
    if (ok) remote_control_store_pin_locked(pin);
    pthread_mutex_unlock(&pin_mutex);
    return ok;
}

void remote_control_get_pin(char * out_pin, size_t out_pin_size) {
    if (!out_pin || out_pin_size == 0) return;
    /* The UI shows the PIN before the server is ever enabled, so create it
     * here too rather than displaying an empty or placeholder value. */
    remote_control_ensure_pin();
    pthread_mutex_lock(&pin_mutex);
    snprintf(out_pin, out_pin_size, "%s", current_settings.remote_control_pin);
    pthread_mutex_unlock(&pin_mutex);
}

typedef enum {
    REMOTE_CONTROL_PIN_INVALID = 0,
    REMOTE_CONTROL_PIN_VALID = 1,
    REMOTE_CONTROL_PIN_RATE_LIMITED = 2
} remote_control_pin_result_t;

static remote_control_pin_result_t remote_control_pin_check(const char * candidate) {
    bool matches = false;
    time_t now = time(NULL);
    pthread_mutex_lock(&pin_mutex);
    if (now < pin_blocked_until) {
        pthread_mutex_unlock(&pin_mutex);
        return REMOTE_CONTROL_PIN_RATE_LIMITED;
    }

    const char * expected = current_settings.remote_control_pin;
    size_t candidate_len = candidate ? strlen(candidate) : 0;
    size_t expected_len = strlen(expected);
    unsigned int diff = (unsigned int) (candidate_len ^ expected_len);
    size_t compare_len = candidate_len > expected_len ? candidate_len : expected_len;
    for (size_t i = 0; i < compare_len; i++) {
        unsigned char a = i < candidate_len ? (unsigned char) candidate[i] : 0;
        unsigned char b = i < expected_len ? (unsigned char) expected[i] : 0;
        diff |= (unsigned int) (a ^ b);
    }
    matches = diff == 0 && remote_control_pin_is_valid(candidate) &&
              remote_control_pin_is_valid(expected);
    if (matches) {
        pin_failures = 0;
        memset(pin_failed_candidates, 0, sizeof(pin_failed_candidates));
        pin_failure_window = 0;
    } else {
        if (now - pin_failure_window >= 60 || pin_failure_window == 0) {
            pin_failure_window = now;
            pin_failures = 0;
            memset(pin_failed_candidates, 0, sizeof(pin_failed_candidates));
        }
        bool already_counted = false;
        for (unsigned int i = 0; i < pin_failures; i++) {
            if (strcmp(pin_failed_candidates[i], candidate) == 0) {
                already_counted = true;
                break;
            }
        }
        if (!already_counted && pin_failures < 5) {
            snprintf(pin_failed_candidates[pin_failures], sizeof(pin_failed_candidates[pin_failures]), "%s", candidate);
            pin_failures++;
        }
        if (pin_failures >= 5) {
            pin_blocked_until = now + 60;
            pin_failures = 0;
            memset(pin_failed_candidates, 0, sizeof(pin_failed_candidates));
            pin_failure_window = 0;
        }
    }
    pthread_mutex_unlock(&pin_mutex);
    return matches ? REMOTE_CONTROL_PIN_VALID : REMOTE_CONTROL_PIN_INVALID;
}

void remote_control_notify_status(bool playing, bool paused, const char * title, const char * artist,
                                   const char * album, const char * path, int position_seconds,
                                   int duration_seconds, float volume, int play_mode, bool favorite) {
    /* Take the audio snapshot before status_mutex. Match its copied path
     * against this now-playing snapshot so a track transition cannot publish
     * the previous decoder's format beside the new track's metadata. */
    audio_current_format_info_t format = {0};
    bool format_matches_track = path && path[0] &&
        audio_get_current_format_info(&format) && format.valid && !format.is_stream &&
        strcmp(format.path, path) == 0;

    pthread_mutex_lock(&status_mutex);
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
    snprintf(status_codec, sizeof(status_codec), "%s",
             format_matches_track ? remote_codec_name(format.codec) : "");
    status_source_bit_depth = format_matches_track ? format.source_bit_depth : 0;
    status_source_sample_rate = format_matches_track ? format.source_sample_rate : 0;
    pthread_mutex_unlock(&status_mutex);
}

bool remote_control_consume_play_pause(void) {
    pthread_mutex_lock(&status_mutex);
    bool result = request_play_pause;
    request_play_pause = false;
    pthread_mutex_unlock(&status_mutex);
    return result;
}

bool remote_control_consume_screenshot(void) {
    pthread_mutex_lock(&status_mutex);
    bool result = request_screenshot;
    request_screenshot = false;
    pthread_mutex_unlock(&status_mutex);
    return result;
}

bool remote_control_consume_next(void) {
    pthread_mutex_lock(&status_mutex);
    bool result = request_next;
    request_next = false;
    pthread_mutex_unlock(&status_mutex);
    return result;
}

bool remote_control_consume_prev(void) {
    pthread_mutex_lock(&status_mutex);
    bool result = request_prev;
    request_prev = false;
    pthread_mutex_unlock(&status_mutex);
    return result;
}

bool remote_control_consume_mode_cycle(void) {
    pthread_mutex_lock(&status_mutex);
    bool result = request_mode_cycle;
    request_mode_cycle = false;
    pthread_mutex_unlock(&status_mutex);
    return result;
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
    pthread_mutex_lock(&status_mutex);
    bool result = request_favorite_changed;
    request_favorite_changed = false;
    pthread_mutex_unlock(&status_mutex);
    return result;
}

void remote_control_sync_queue(const char * const * paths, int count, uint64_t revision) {
    /* metadata_db_get_songs_by_paths() takes its own METADATA_DB_GUARD lock
     * internally -- must not be called while already holding status_mutex,
     * or a UI-thread caller blocked on METADATA_DB_GUARD (e.g. a rescan)
     * while THIS thread holds status_mutex waiting on that same lock could
     * deadlock against gui.c's own status_mutex use elsewhere. Resolved
     * before, not during, the locked section below. Batched (one prepared
     * statement reused across all `count` lookups) rather than a fresh
     * metadata_db_get_song_by_path() prepare/finalize per song -- see that
     * function's own doc comment. */
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

/* Escape JSON string delimiters and control bytes so metadata round-trips as
 * the same string when a client parses the response. */
static void json_escape_append(char * out, size_t out_size, const char * in) {
    size_t len = strlen(out);
    for (const char * p = in; *p && len + 2 < out_size; p++) {
        unsigned char c = (unsigned char) *p;
        if (c == '"' || c == '\\') {
            if (len + 3 >= out_size) break;
            out[len++] = '\\';
            out[len++] = (char) c;
        } else if (c < 0x20) {
            static const char hex[] = "0123456789abcdef";
            if (len + 6 >= out_size) break;
            out[len++] = '\\';
            out[len++] = 'u';
            out[len++] = '0';
            out[len++] = '0';
            out[len++] = hex[c >> 4];
            out[len++] = hex[c & 0x0f];
        } else {
            out[len++] = (char) c;
        }
    }
    out[len] = '\0';
}

typedef struct {
    char * data;
    size_t capacity;
    size_t length;
    bool truncated;
} json_builder_t;

static void json_builder_init(json_builder_t * b, char * data, size_t capacity) {
    b->data = data;
    b->capacity = capacity;
    b->length = 0;
    b->truncated = capacity == 0;
    if (capacity > 0) data[0] = '\0';
}

static bool json_builder_appendf(json_builder_t * b, const char * format, ...) {
    if (b->truncated || b->length >= b->capacity) return false;
    size_t old_length = b->length;
    va_list ap;
    va_start(ap, format);
    int written = vsnprintf(b->data + b->length, b->capacity - b->length, format, ap);
    va_end(ap);
    if (written < 0 || (size_t) written >= b->capacity - b->length) {
        b->length = old_length;
        b->data[old_length] = '\0';
        b->truncated = true;
        return false;
    }
    b->length += (size_t) written;
    return true;
}

/* Decodes application/x-www-form-urlencoded query-string bytes (%XX and
 * "+" for space; both %20 and "+" are accepted for space). Bounded,
 * truncates rather than overflowing if the decoded result wouldn't fit. */
static void url_decode(const char * in, char * out, size_t out_size) {
    size_t len = 0;
    while (*in && len + 1 < out_size) {
        if (*in == '%' && in[1] && in[2]) {
            char hex[3] = { in[1], in[2], '\0' };
            char * end;
            long v = strtol(hex, &end, 16);
            if (end == hex + 2) {
                out[len++] = (char) v;
                in += 3;
                continue;
            }
        }
        out[len++] = (*in == '+') ? ' ' : *in;
        in++;
    }
    out[len] = '\0';
}

static bool url_decode_checked(const char * in, char * out, size_t out_size) {
    size_t len = 0;
    if (!out || out_size == 0) return false;
    while (*in) {
        if (len + 1 >= out_size) { out[0] = '\0'; return false; }
        if (*in == '%') {
            if (!in[1] || !in[2]) { out[0] = '\0'; return false; }
            bool first_is_hex = (in[1] >= '0' && in[1] <= '9') || (in[1] >= 'a' && in[1] <= 'f') ||
                                (in[1] >= 'A' && in[1] <= 'F');
            bool second_is_hex = (in[2] >= '0' && in[2] <= '9') || (in[2] >= 'a' && in[2] <= 'f') ||
                                 (in[2] >= 'A' && in[2] <= 'F');
            if (!first_is_hex || !second_is_hex) { out[0] = '\0'; return false; }
            char hex[3] = { in[1], in[2], '\0' };
            char * end;
            long v = strtol(hex, &end, 16);
            if (end != hex + 2 || v == 0) { out[0] = '\0'; return false; }
            out[len++] = (char)v;
            in += 3;
            continue;
        }
        out[len++] = (*in == '+') ? ' ' : *in;
        in++;
    }
    out[len] = '\0';
    return true;
}

static bool query_param_present(const char * path, const char * key) {
    const char * q = strchr(path, '?');
    if (!q) return false;
    q++;
    size_t key_len = strlen(key);
    while (*q) {
        if (strncmp(q, key, key_len) == 0 && q[key_len] == '=') return true;
        const char * amp = strchr(q, '&');
        if (!amp) break;
        q = amp + 1;
    }
    return false;
}

/* Pulls a raw (still percent-encoded) string query parameter's value.
 * Returns false (leaving out untouched) if absent or too long for out. */
static bool query_param_str(const char * path, const char * key, char * out, size_t out_size) {
    const char * q = strchr(path, '?');
    if (!q) return false;
    q++;

    size_t key_len = strlen(key);
    while (*q) {
        if (strncmp(q, key, key_len) == 0 && q[key_len] == '=') {
            const char * val = q + key_len + 1;
            const char * amp = strchr(val, '&');
            size_t val_len = amp ? (size_t) (amp - val) : strlen(val);
            if (val_len >= out_size) return false;
            memcpy(out, val, val_len);
            out[val_len] = '\0';
            return true;
        }
        const char * amp = strchr(q, '&');
        if (!amp) break;
        q = amp + 1;
    }
    return false;
}

/* Caps both the page size for build_library_json() and, indirectly, the
 * response size. */
#define LIBRARY_JSON_MAX_LIMIT 100

static void build_library_json(const char * query, const char * artist_filter, const char * album_artist_filter,
                                const char * album_filter, const char * genre_filter, int offset, int limit,
                                char * out, size_t out_size) {
    if (limit <= 0 || limit > LIBRARY_JSON_MAX_LIMIT) limit = LIBRARY_JSON_MAX_LIMIT;
    if (offset < 0) offset = 0;

    /* Query filtered songs directly from metadata_db. "index" in the response
     * is the persistent song id (song_row_t.id). */
    int64_t total_matches = metadata_db_count_songs_filtered(query, artist_filter, album_artist_filter, album_filter, genre_filter);

    json_builder_t json;
    json_builder_init(&json, out, out_size);
    json_builder_appendf(&json, "{\"total\":%lld,\"songs\":[", (long long) total_matches);

    /* Allocate rows on the heap to keep stack usage bounded. */
    song_row_t * rows = malloc(sizeof(song_row_t) * LIBRARY_JSON_MAX_LIMIT);
    int n = rows ? metadata_db_get_songs_filtered_page(query, artist_filter, album_artist_filter, album_filter,
                                                         genre_filter, offset, limit, rows)
                 : 0;
    for (int i = 0; i < n && !json.truncated; i++) {
        char display_title[128], title_esc[300] = {0}, artist_esc[300] = {0};
        metadata_db_song_display_title(&rows[i], display_title, sizeof(display_title));
        json_escape_append(title_esc, sizeof(title_esc), display_title);
        json_escape_append(artist_esc, sizeof(artist_esc), rows[i].tags.artist);
        if (!json_builder_appendf(&json, "%s{\"index\":%lld,\"title\":\"%s\",\"artist\":\"%s\"}", i > 0 ? "," : "",
                                  (long long) rows[i].id, title_esc, artist_esc)) break;
    }
    json.truncated = false; /* a rejected item left the prior JSON intact */
    json_builder_appendf(&json, "]}");
    free(rows);
}

/* Renders group_row_t entries into a JSON array {"name":..., "count":..., "index":..., "album_artist":...}.
 * index is a representative first song id from group_row_t.first_song_id. */
static void render_name_counts_json(const group_row_t * groups, int count, const char * json_key, char * out,
                                     size_t out_size) {
    json_builder_t json;
    json_builder_init(&json, out, out_size);
    json_builder_appendf(&json, "{\"%s\":[", json_key);
    size_t response_capacity = json.capacity;
    for (int i = 0; i < count && !json.truncated && json.capacity - json.length >= 3; i++) {
        char name_esc[METADATA_DB_TEXT_MAX * 6] = {0}, album_artist_esc[300] = {0};
        const char * name = strcmp(json_key, "genres") == 0 ? groups[i].genre_name : groups[i].name;
        json_escape_append(name_esc, sizeof(name_esc), name);
        /* album_artist is populated for album groups. */
        json_escape_append(album_artist_esc, sizeof(album_artist_esc), groups[i].album_artist);
        /* Keep room for the closing bracket, brace, and trailing NUL even if
         * the next full row does not fit in the response buffer. */
        json.capacity = response_capacity >= 2 ? response_capacity - 2 : response_capacity;
        bool appended = json_builder_appendf(&json,
            "%s{\"name\":\"%s\",\"count\":%d,\"index\":%lld,\"album_artist\":\"%s\"}",
            i > 0 ? "," : "", name_esc, groups[i].song_count,
            (long long) groups[i].first_song_id, album_artist_esc);
        json.capacity = response_capacity;
        if (!appended) break;
    }
    json.truncated = false;
    json_builder_appendf(&json, "]}");
}

/* Upper bound for groups queries. */
#define GROUPS_JSON_MAX 2000

static void build_artists_json(char * out, size_t out_size) {
    group_row_t * groups = malloc(sizeof(group_row_t) * GROUPS_JSON_MAX);
    int n = groups ? metadata_db_get_groups_page(METADATA_DB_GROUP_ARTIST, 0, GROUPS_JSON_MAX, groups) : 0;
    render_name_counts_json(groups, n, "artists", out, out_size);
    free(groups);
}

static void build_album_artists_json(char * out, size_t out_size) {
    group_row_t * groups = malloc(sizeof(group_row_t) * GROUPS_JSON_MAX);
    int n = groups ? metadata_db_get_groups_page(METADATA_DB_GROUP_ALBUM_ARTIST, 0, GROUPS_JSON_MAX, groups) : 0;
    render_name_counts_json(groups, n, "artists", out, out_size);
    free(groups);
}

static void build_genres_json(char * out, size_t out_size) {
    group_row_t * groups = malloc(sizeof(group_row_t) * GROUPS_JSON_MAX);
    int n = groups ? metadata_db_get_groups_page(METADATA_DB_GROUP_GENRE, 0, GROUPS_JSON_MAX, groups) : 0;
    render_name_counts_json(groups, n, "genres", out, out_size);
    free(groups);
}

/* Build album groups matching artist_filter or album_artist_filter. */
static void build_albums_json(const char * artist_filter, const char * album_artist_filter, char * out,
                               size_t out_size) {
    const char * filter = artist_filter[0] != '\0' ? artist_filter : album_artist_filter;
    group_row_t * groups = malloc(sizeof(group_row_t) * GROUPS_JSON_MAX);
    int n = groups ? metadata_db_get_albums_page_filtered(filter, 0, GROUPS_JSON_MAX, groups) : 0;
    render_name_counts_json(groups, n, "albums", out, out_size);
    free(groups);
}

static void build_status_json(char * out, size_t out_size) {
    char title_esc[512] = {0}, artist_esc[512] = {0}, album_esc[512] = {0};
    /* Query the route before taking status_mutex. Route selection has its own
     * lock and must not be nested with the playback-status snapshot lock. */
#ifndef HOST_BUILD
    bool bluetooth_audio_output = audio_output_is_bluetooth_requested();
#else
    bool bluetooth_audio_output = false; /* host builds have no Bluetooth route */
#endif
    pthread_mutex_lock(&status_mutex);
    json_escape_append(title_esc, sizeof(title_esc), status_title);
    json_escape_append(artist_esc, sizeof(artist_esc), status_artist);
    json_escape_append(album_esc, sizeof(album_esc), status_album);
    snprintf(out, out_size,
             "{\"playing\":%s,\"paused\":%s,\"title\":\"%s\",\"artist\":\"%s\",\"album\":\"%s\","
             "\"position_seconds\":%d,\"duration_seconds\":%d,\"volume\":%.2f,\"play_mode\":%d,"
             "\"codec\":\"%s\",\"source_bit_depth\":%u,\"source_sample_rate\":%u,"
             "\"bluetooth_audio_output\":%s,\"favorite\":%s}",
             status_playing ? "true" : "false", status_paused ? "true" : "false", title_esc, artist_esc, album_esc,
             status_position_seconds, status_duration_seconds, (double) status_volume, status_play_mode,
             status_codec, status_source_bit_depth, status_source_sample_rate,
             bluetooth_audio_output ? "true" : "false", status_favorite ? "true" : "false");
    pthread_mutex_unlock(&status_mutex);
}

/* The playlist `name` becomes a filesystem path component
 * (PLAYLISTS_DIR "/" name ".m3u", see playlist_files_create()) -- unlike
 * gui.c's own in-app equivalent (new_playlist_name_done_cb(), whose name
 * comes from a UI text field only the device's own user can type into),
 * this is reachable by authenticated remote clients, so it is validated
 * rather than trusted the way playlist_files_create()'s
 * own doc comment says its callers must. Rejects empty names, anything
 * containing a path separator, and anything containing ".." (defense in
 * depth against path traversal even though a bare ".." alone couldn't
 * escape PLAYLISTS_DIR without a "/" too). */
#define PLAYLIST_NAME_MAX_BYTES 127

static bool playlist_name_is_safe(const char * name) {
    if (!name || name[0] == '\0') return false;
    if (strchr(name, '/') || strchr(name, '\\')) return false;
    if (strstr(name, "..")) return false;
    return true;
}

static void build_playlists_json(char * out, size_t out_size) {
    char ** paths = NULL;
    int count = 0;
    playlist_files_scan(PLAYLISTS_DIR, &paths, &count);

    /* Build JSON array of playlists using json_builder_t with bounds checking. */
    json_builder_t json;
    json_builder_init(&json, out, out_size);
    json_builder_appendf(&json,
                            "{\"playlists\":[{\"name\":\"Favorites\",\"key\":\"@favorites\",\"internal\":true,\"writable\":false},"
                            "{\"name\":\"Most Played\",\"key\":\"@most_played\",\"internal\":true,\"writable\":false},"
                            "{\"name\":\"Recently Played\",\"key\":\"@recently_played\",\"internal\":true,\"writable\":false}");
    size_t playlists_dir_len = strlen(PLAYLISTS_DIR);
    for (int i = 0; i < count && !json.truncated; i++) {
        /* The API routes resolve user playlists as
         * PLAYLISTS_DIR/<key>.m3u. Advertise only files that exact lookup can
         * reach: direct children with the canonical lowercase extension. */
        if (strncmp(paths[i], PLAYLISTS_DIR, playlists_dir_len) != 0 ||
            paths[i][playlists_dir_len] != '/') continue;
        const char * base = paths[i] + playlists_dir_len + 1;
        if (strchr(base, '/') != NULL) continue;
        size_t base_len = strlen(base);
        if (base_len <= 4 || strcmp(base + base_len - 4, ".m3u") != 0) continue;

        size_t key_len = base_len - 4;
        char key[256] = {0};
        /* Playback context and playlist lookup use 128-byte decoded names. */
        if (key_len >= sizeof(key) || key_len > PLAYLIST_NAME_MAX_BYTES) continue;
        memcpy(key, base, key_len);
        if (!playlist_name_is_safe(key)) continue;

        char name_esc[1600] = {0}, key_esc[1600] = {0};
        json_escape_append(name_esc, sizeof(name_esc), base);
        json_escape_append(key_esc, sizeof(key_esc), key);
        if (!json_builder_appendf(&json, ",{\"name\":\"%s\",\"key\":\"%s\",\"internal\":false,\"writable\":true}",
                                  name_esc, key_esc)) break;
    }
    json.truncated = false;
    json_builder_appendf(&json, "]}");

    for (int i = 0; i < count; i++) free(paths[i]);
    free(paths);
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


static void send_all(int fd, const char * data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, data + sent, len - sent);
        if (n <= 0) return;
        sent += (size_t) n;
    }
}

/* Sends an HTTP response with no-cache headers. */
static void send_response(int fd, const char * status_line, const char * content_type, const char * body) {
    char header[320];
    int header_len = snprintf(header, sizeof(header),
                               "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                               "Cache-Control: no-store, no-cache, must-revalidate\r\nPragma: no-cache\r\n"
                               "Connection: close\r\n\r\n",
                               status_line, content_type, strlen(body));
    send_all(fd, header, (size_t) header_len);
    send_all(fd, body, strlen(body));
}

static void send_pin_rate_limited_response(int fd) {
    static const char body[] = "{\"error\":\"rate_limited\",\"code\":\"pin_rate_limited\",\"retryAfterSeconds\":60}";
    char header[320];
    int header_len = snprintf(header, sizeof(header),
                              "HTTP/1.1 429 Too Many Requests\r\nContent-Type: application/json\r\n"
                              "Content-Length: %zu\r\nRetry-After: 60\r\nConnection: close\r\n\r\n",
                              sizeof(body) - 1);
    send_all(fd, header, (size_t) header_len);
    send_all(fd, body, sizeof(body) - 1);
}

/* Pulls an integer query parameter from the request path. Returns false if
 * absent or invalid. */
static bool query_param_int(const char * path, const char * key, int * out_value) {
    const char * q = strchr(path, '?');
    if (!q) return false;
    q++;

    size_t key_len = strlen(key);
    while (*q) {
        if (strncmp(q, key, key_len) == 0 && q[key_len] == '=') {
            char * end;
            long v = strtol(q + key_len + 1, &end, 10);
            if (end == q + key_len + 1) return false; /* no digits at all */
            *out_value = (int) v;
            return true;
        }
        const char * amp = strchr(q, '&');
        if (!amp) break;
        q = amp + 1;
    }
    return false;
}

/* Pulls a 64-bit integer query parameter from the request path. */
static bool query_param_int64(const char * path, const char * key, int64_t * out_value) {
    const char * q = strchr(path, '?');
    if (!q) return false;
    q++;

    size_t key_len = strlen(key);
    while (*q) {
        if (strncmp(q, key, key_len) == 0 && q[key_len] == '=') {
            char * end;
            long long v = strtoll(q + key_len + 1, &end, 10);
            if (end == q + key_len + 1) return false; /* no digits at all */
            *out_value = (int64_t) v;
            return true;
        }
        const char * amp = strchr(q, '&');
        if (!amp) break;
        q = amp + 1;
    }
    return false;
}

/* Serves a song's embedded cover art as a raw image. Resolves the path from
 * song id or from the currently playing file if no index is provided. */
static bool resolve_art_source_path(bool have_index, int64_t index, char * out_path, size_t out_path_size) {
    if (have_index) {
        song_row_t row;
        if (!metadata_db_get_song_by_id(index, &row)) return false;
        snprintf(out_path, out_path_size, "%s", row.path);
        return true;
    }
    pthread_mutex_lock(&status_mutex);
    bool ok = false;
    if (status_path[0] != '\0') {
        snprintf(out_path, out_path_size, "%s", status_path);
        ok = true;
    }
    pthread_mutex_unlock(&status_mutex);
    return ok;
}

static void send_response_binary(int fd, const char * status_line, const char * content_type, const uint8_t * data,
                                  size_t len) {
    char header[320];
    int header_len = snprintf(header, sizeof(header),
                               "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                               "Cache-Control: no-store, no-cache, must-revalidate\r\nPragma: no-cache\r\n"
                               "Connection: close\r\n\r\n",
                               status_line, content_type, len);
    send_all(fd, header, (size_t) header_len);
    send_all(fd, (const char *) data, len);
}

/* Sniffs enough image magic to label either an original sidecar or embedded
 * art. The player can return BMP sidecars as well as embedded JPEG/PNG; an
 * unrecognized-but-real image keeps the common JPEG fallback. */
static const char * sniff_image_content_type(const uint8_t * data, uint32_t size) {
    if (size >= 8 && data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E && data[3] == 0x47) return "image/png";
    if (size >= 2 && data[0] == 'B' && data[1] == 'M') return "image/bmp";
    return "image/jpeg";
}

/* Tag text is capped at 127 bytes; JSON escaping can expand each byte to six.
 * The artist membership array duplicates at most that source text, then adds
 * up to three quote/comma bytes per split artist. Remaining row syntax,
 * numeric fields, and the key fit within 256 bytes. The page envelope is
 * bounded by its UUID/revision strings and signed numeric fields. */
#define CATALOG_TAG_ESCAPED_MAX ((TAGCACHE_TAG_MAX - 1u) * 6u)
#define CATALOG_SONG_ROW_WORST_CASE (6u * CATALOG_TAG_ESCAPED_MAX + \
                                     3u * TAGCACHE_ARTIST_SPLIT_MAX + 256u)
#define CATALOG_COVER_ROW_WORST_CASE 256u
#define CATALOG_JSON_ENVELOPE_MAX 512u
/* Worst status JSON is 1,891 bytes: three 511-byte escaped strings, a
 * 43-byte formatted float, bounded integer/codec/boolean fields, and fixed syntax.
 * The extra byte count for NUL still fits comfortably in this 2 KiB buffer. */
#define STATUS_JSON_CAPACITY 2048u
#define CATALOG_ART_MAX_BYTES (8u * 1024u * 1024u)
#define CATALOG_ART_STREAM_CHUNK (16u * 1024u)

/* JSON error body shared by the catalog and later feature families:
 * {"error":"<family>_error","code":"<reason>"}. */
static void send_api_error(int cfd, const char * status, const char * error, const char * code) {
    char body[160];
    snprintf(body, sizeof(body), "{\"error\":\"%s\",\"code\":\"%s\"}", error, code);
    send_response(cfd, status, "application/json", body);
}

static void catalog_send_error(int cfd, const char * status, const char * code) {
    send_api_error(cfd, status, "catalog_error", code);
}

static bool catalog_query_number(const char * path, const char * key, uint64_t default_value,
                                 uint64_t max_value, uint64_t * out_value) {
    if (!query_param_present(path, key)) {
        *out_value = default_value;
        return true;
    }
    char raw[32] = {0};
    if (!query_param_str(path, key, raw, sizeof(raw)) || raw[0] == '\0') return false;
    for (const char * p = raw; *p; p++) if (*p < '0' || *p > '9') return false;
    errno = 0;
    char * end = NULL;
    unsigned long long value = strtoull(raw, &end, 10);
    if (errno == ERANGE || !end || *end != '\0' || value > max_value) return false;
    *out_value = (uint64_t) value;
    return true;
}

static bool catalog_query_revision_named(const char * path, const char * key, bool required,
                                         char out[METADATA_DB_CATALOG_REVISION_SIZE]) {
    out[0] = '\0';
    if (!query_param_present(path, key)) return !required;
    char raw[METADATA_DB_CATALOG_REVISION_SIZE * 3] = {0};
    if (!query_param_str(path, key, raw, sizeof(raw)) ||
        !url_decode_checked(raw, out, METADATA_DB_CATALOG_REVISION_SIZE) || out[0] == '\0') return false;
    return true;
}

static bool catalog_query_revision(const char * path, bool required,
                                   char out[METADATA_DB_CATALOG_REVISION_SIZE]) {
    return catalog_query_revision_named(path, "revision", required, out);
}

static void catalog_albumart_info(const song_row_t * row, albumart_info_t * out) {
    memset(out, 0, sizeof(*out));
    snprintf(out->path, sizeof(out->path), "%s", row->path);
    snprintf(out->artist, sizeof(out->artist), "%s", row->tags.artist);
    snprintf(out->album, sizeof(out->album), "%s", row->tags.album);
    snprintf(out->albumartist, sizeof(out->albumartist), "%s",
             row->tags.album_artist[0] ? row->tags.album_artist : row->tags.artist);
}

static uint64_t catalog_album_key_hash(const song_row_t * row) {
    albumart_info_t info;
    catalog_albumart_info(row, &info);
    return albumart_thumbnail_key(&info);
}

static bool catalog_build_songs_json(const metadata_db_catalog_page_t * page,
                                     const metadata_db_catalog_song_t * rows,
                                     int requested_offset, char * json, size_t capacity) {
    json_builder_t builder;
    json_builder_init(&builder, json, capacity);
    if (!json_builder_appendf(&builder,
        "{\"library_id\":\"%s\",\"revision\":\"%s\",\"total\":%lld,\"offset\":%d,\"songs\":[",
        page->library_id, page->revision, (long long) page->total, requested_offset)) return false;
    int emitted = 0;
    for (int i = 0; i < page->count; i++) {
        const song_row_t * row = &rows[i].song;
        char row_json[8192] = {0};
        json_builder_t row_builder;
        json_builder_init(&row_builder, row_json, sizeof(row_json));
        char title[128] = {0};
        char title_json[800] = {0}, artist_json[800] = {0}, album_json[800] = {0};
        char album_artist_json[800] = {0}, genre_json[800] = {0};
        char split_artists[TAGCACHE_ARTIST_SPLIT_MAX][TAGCACHE_TAG_MAX] = {{0}};
        metadata_db_song_display_title(row, title, sizeof(title));
        json_escape_append(title_json, sizeof(title_json), title);
        json_escape_append(artist_json, sizeof(artist_json), row->tags.artist);
        json_escape_append(album_json, sizeof(album_json), row->tags.album);
        json_escape_append(album_artist_json, sizeof(album_artist_json),
                           row->tags.album_artist[0] ? row->tags.album_artist : row->tags.artist);
        json_escape_append(genre_json, sizeof(genre_json), row->tags.genre);
        int artist_count = tagcache_artist_names(row->tags.artist, split_artists, TAGCACHE_ARTIST_SPLIT_MAX);
        bool appended = json_builder_appendf(&row_builder,
            "%s{\"id\":%lld,\"title\":\"%s\",\"artist\":\"%s\",\"artists\":[",
            emitted ? "," : "", (long long) row->id, title_json, artist_json);
        if (!appended) return false;
        for (int artist_i = 0; artist_i < artist_count; artist_i++) {
            char artist_name_json[800] = {0};
            json_escape_append(artist_name_json, sizeof(artist_name_json), split_artists[artist_i]);
            appended = json_builder_appendf(&row_builder, "%s\"%s\"", artist_i ? "," : "", artist_name_json);
            if (!appended) return false;
        }
        appended = json_builder_appendf(&row_builder,
            "],\"album\":\"%s\",\"album_artist\":\"%s\",\"genre\":\"%s\",\"track_number\":",
            album_json, album_artist_json, genre_json);
        if (!appended) return false;
        if (row->tags.track_number > 0)
            appended = json_builder_appendf(&row_builder, "%d,\"disc_number\":", row->tags.track_number);
        else
            appended = json_builder_appendf(&row_builder, "null,\"disc_number\":");
        if (!appended) return false;
        if (row->tags.disc_number > 0)
            appended = json_builder_appendf(&row_builder, "%d,\"album_key\":", row->tags.disc_number);
        else
            appended = json_builder_appendf(&row_builder, "null,\"album_key\":");
        if (!appended) return false;
        if (rows[i].album_representative_id > 0)
            appended = json_builder_appendf(&row_builder, "\"a2-%016llx\"}",
                (unsigned long long) catalog_album_key_hash(row));
        else
            appended = json_builder_appendf(&row_builder, "null}");
        if (!appended || row_builder.truncated ||
            !json_builder_appendf(&builder, "%s", row_json)) return false;
        emitted++;
    }
    return json_builder_appendf(&builder, "],\"next_offset\":%d}", requested_offset + emitted);
}

static uint64_t catalog_hash_text(uint64_t hash, const char * text) {
    for (const unsigned char * p = (const unsigned char *) text; *p; p++) {
        hash ^= *p;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static uint64_t catalog_source_fingerprint(const char * path, int64_t mtime, int64_t size) {
    uint64_t hash = UINT64_C(14695981039346656037);
    hash = catalog_hash_text(hash, path);
    hash ^= 0;
    hash *= UINT64_C(1099511628211);
    uint64_t values[2] = { (uint64_t) mtime, (uint64_t) size };
    for (size_t value = 0; value < 2; value++) {
        for (unsigned int byte = 0; byte < 8; byte++) {
            hash ^= (uint8_t) (values[value] >> (byte * 8));
            hash *= UINT64_C(1099511628211);
        }
    }
    return hash;
}

/* Source keys are independent of library generation and song id. A stable
 * album_key lets the client retain a cover if a new scan changes the chosen
 * representative but the original sidecar source remains the same. */
typedef struct {
    const metadata_db_catalog_cover_t * row;
} catalog_source_load_context_t;

static bool catalog_source_resolution_load(void * context,
                                           catalog_source_resolution_t * out_resolution) {
    catalog_source_load_context_t * load = context;
    const metadata_db_catalog_cover_t * row = load ? load->row : NULL;
    if (!row || !out_resolution) return false;

    albumart_info_t info;
    catalog_albumart_info(&row->representative, &info);
    char sidecar[CATALOG_SOURCE_CACHE_PATH_SIZE] = {0};
    struct stat st;
    if (albumart_search_source_files(&info, "", sidecar, sizeof(sidecar)) &&
        stat(sidecar, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 0) {
        uint64_t fingerprint = catalog_source_fingerprint(sidecar, (int64_t) st.st_mtime,
                                                          (int64_t) st.st_size);
        int n = snprintf(out_resolution->cover_key, sizeof(out_resolution->cover_key),
                         "s-%016llx", (unsigned long long) fingerprint);
        if (n <= 0 || (size_t) n >= sizeof(out_resolution->cover_key)) return false;
        n = snprintf(out_resolution->sidecar_path, sizeof(out_resolution->sidecar_path),
                     "%s", sidecar);
        if (n <= 0 || (size_t) n >= sizeof(out_resolution->sidecar_path)) return false;
        out_resolution->has_sidecar = true;
        out_resolution->stat_mtime = (int64_t) st.st_mtime;
        out_resolution->stat_size = (int64_t) st.st_size;
        return true;
    }

    int64_t mtime = row->scanned_mtime;
    int64_t size = row->scanned_size;
    if (stat(row->representative.path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 0) {
        mtime = (int64_t) st.st_mtime;
        size = (int64_t) st.st_size;
    }
    uint64_t fingerprint = catalog_source_fingerprint(row->representative.path, mtime, size);
    int n = snprintf(out_resolution->cover_key, sizeof(out_resolution->cover_key),
                     "e-%016llx", (unsigned long long) fingerprint);
    if (n <= 0 || (size_t) n >= sizeof(out_resolution->cover_key)) return false;
    out_resolution->stat_mtime = mtime;
    out_resolution->stat_size = size;
    return true;
}

static bool catalog_cover_resolution_for_row(const char * revision,
                                             const metadata_db_catalog_cover_t * row,
                                             catalog_source_resolution_t * out_resolution) {
    if (!row || !out_resolution) return false;
    catalog_source_load_context_t context = { .row = row };
    return catalog_source_cache_resolve(revision, row->representative_id,
        catalog_source_resolution_load, &context, out_resolution);
}

static bool catalog_build_covers_json(const metadata_db_catalog_page_t * page,
                                      const metadata_db_catalog_cover_t * rows,
                                      int requested_offset, char * json, size_t capacity) {
    json_builder_t builder;
    json_builder_init(&builder, json, capacity);
    if (!json_builder_appendf(&builder,
        "{\"library_id\":\"%s\",\"revision\":\"%s\",\"total\":%lld,\"offset\":%d,\"covers\":[",
        page->library_id, page->revision, (long long) page->total, requested_offset)) return false;
    int emitted = 0;
    for (int i = 0; i < page->count; i++) {
        catalog_source_resolution_t resolution;
        if (!catalog_cover_resolution_for_row(page->revision, &rows[i], &resolution)) return false;
        uint64_t album_hash = catalog_album_key_hash(&rows[i].representative);
        bool appended = json_builder_appendf(&builder,
            "%s{\"album_key\":\"a2-%016llx\",\"representative_id\":%lld,\"cover_key\":\"%s\",\"available\":%s}",
            emitted ? "," : "", (unsigned long long) album_hash,
            (long long) rows[i].representative_id, resolution.cover_key,
            resolution.has_sidecar ? "true" : "null");
        if (!appended) return false;
        emitted++;
    }
    return json_builder_appendf(&builder, "],\"next_offset\":%d}", requested_offset + emitted);
}

static void handle_catalog_page_request(int cfd, const char * path, bool covers) {
    uint64_t offset_u = 0, limit_u = 50;
    if (!catalog_query_number(path, "offset", 0, INT_MAX, &offset_u) ||
        !catalog_query_number(path, "limit", 50, METADATA_DB_CATALOG_PAGE_MAX, &limit_u) ||
        limit_u == 0) {
        catalog_send_error(cfd, "400 Bad Request", "invalid_paging");
        return;
    }
    char revision[METADATA_DB_CATALOG_REVISION_SIZE] = {0};
    if (!catalog_query_revision(path, covers, revision)) {
        catalog_send_error(cfd, "400 Bad Request", "invalid_revision");
        return;
    }
    int offset = (int) offset_u;
    int limit = (int) limit_u;
    size_t row_worst_case = covers ? CATALOG_COVER_ROW_WORST_CASE : CATALOG_SONG_ROW_WORST_CASE;
    size_t json_capacity = (size_t) limit * row_worst_case + CATALOG_JSON_ENVELOPE_MAX;
    char * json = malloc(json_capacity);
    if (!json) {
        catalog_send_error(cfd, "503 Service Unavailable", "temporary_unavailable");
        return;
    }
    metadata_db_catalog_page_t page;
    metadata_db_catalog_result_t result;
    if (covers) {
        metadata_db_catalog_cover_t * rows = calloc((size_t) limit, sizeof(*rows));
        if (!rows) {
            free(json);
            catalog_send_error(cfd, "503 Service Unavailable", "temporary_unavailable");
            return;
        }
        result = metadata_db_catalog_covers_page(revision[0] ? revision : NULL, offset, limit, &page, rows);
        if (result == METADATA_DB_CATALOG_OK) {
            if (page.count == 0 && offset < page.total) {
                result = METADATA_DB_CATALOG_UNAVAILABLE;
            } else if (catalog_build_covers_json(&page, rows, offset, json, json_capacity)) {
                send_response(cfd, "200 OK", "application/json", json);
            } else {
                result = METADATA_DB_CATALOG_UNAVAILABLE;
            }
        }
        free(rows);
    } else {
        metadata_db_catalog_song_t * rows = calloc((size_t) limit, sizeof(*rows));
        if (!rows) {
            free(json);
            catalog_send_error(cfd, "503 Service Unavailable", "temporary_unavailable");
            return;
        }
        result = metadata_db_catalog_songs_page(revision[0] ? revision : NULL, offset, limit, &page, rows);
        if (result == METADATA_DB_CATALOG_OK) {
            if (page.count == 0 && offset < page.total) {
                result = METADATA_DB_CATALOG_UNAVAILABLE;
            } else if (catalog_build_songs_json(&page, rows, offset, json, json_capacity)) {
                send_response(cfd, "200 OK", "application/json", json);
            } else {
                result = METADATA_DB_CATALOG_UNAVAILABLE;
            }
        }
        free(rows);
    }
    free(json);
    if (result == METADATA_DB_CATALOG_STALE)
        catalog_send_error(cfd, "409 Conflict", "revision_mismatch");
    else if (result == METADATA_DB_CATALOG_UNAVAILABLE)
        catalog_send_error(cfd, "503 Service Unavailable", "temporary_unavailable");
}

static bool catalog_parse_cover_key(const char * key, char * out_kind, uint64_t * out_fingerprint) {
    if (!key || (key[0] != 's' && key[0] != 'e') || key[1] != '-' || strlen(key + 2) != 16) return false;
    uint64_t value = 0;
    for (int i = 0; i < 16; i++) {
        char c = key[i + 2];
        unsigned int digit;
        if (c >= '0' && c <= '9') digit = (unsigned int) (c - '0');
        else if (c >= 'a' && c <= 'f') digit = (unsigned int) (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') digit = (unsigned int) (c - 'A' + 10);
        else return false;
        value = (value << 4) | digit;
    }
    *out_kind = key[0];
    *out_fingerprint = value;
    return true;
}

static void catalog_stream_header(int cfd, const char * content_type, size_t length) {
    char header[320];
    int n = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Cache-Control: no-store, no-cache, must-revalidate\r\nPragma: no-cache\r\n"
        "Connection: close\r\n\r\n", content_type, length);
    if (n > 0 && (size_t) n < sizeof(header)) send_all(cfd, header, (size_t) n);
}

static void catalog_stream_sidecar(int cfd, int fd, size_t length, const char * content_type) {
    catalog_stream_header(cfd, content_type, length);
    char buffer[CATALOG_ART_STREAM_CHUNK];
    size_t remaining = length;
    while (remaining > 0) {
        size_t wanted = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        ssize_t count = read(fd, buffer, wanted);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        send_all(cfd, buffer, (size_t) count);
        remaining -= (size_t) count;
    }
}

static void handle_catalog_art_request(int cfd, const char * path) {
    char revision[METADATA_DB_CATALOG_REVISION_SIZE] = {0};
    char raw_key[96] = {0}, cover_key[96] = {0};
    uint64_t representative_id_u = 0;
    if (!catalog_query_revision(path, true, revision) ||
        !catalog_query_number(path, "representative_id", 0, INT32_MAX, &representative_id_u) ||
        representative_id_u == 0 ||
        !query_param_str(path, "cover_key", raw_key, sizeof(raw_key)) ||
        !url_decode_checked(raw_key, cover_key, sizeof(cover_key))) {
        catalog_send_error(cfd, "400 Bad Request", "invalid_art_key");
        return;
    }
    char kind = 0;
    uint64_t requested_fingerprint = 0;
    if (!catalog_parse_cover_key(cover_key, &kind, &requested_fingerprint)) {
        catalog_send_error(cfd, "400 Bad Request", "invalid_art_key");
        return;
    }

    int64_t representative_id = (int64_t) representative_id_u;
    metadata_db_catalog_page_t page;
    metadata_db_catalog_cover_t source;
    metadata_db_catalog_result_t lookup = metadata_db_catalog_cover_source(
        revision, representative_id, &page, &source);
    if (lookup == METADATA_DB_CATALOG_STALE) {
        catalog_send_error(cfd, "409 Conflict", "revision_or_cover_stale");
        return;
    }
    if (lookup != METADATA_DB_CATALOG_OK) {
        catalog_send_error(cfd, "503 Service Unavailable", "temporary_unavailable");
        return;
    }

    catalog_source_resolution_t resolution;
    if (!catalog_cover_resolution_for_row(page.revision, &source, &resolution)) {
        catalog_send_error(cfd, "503 Service Unavailable", "source_unavailable");
        return;
    }
    if (strcmp(resolution.cover_key, cover_key) != 0 || (kind == 's') != resolution.has_sidecar) {
        catalog_send_error(cfd, "409 Conflict", "revision_or_cover_stale");
        return;
    }

    if (kind == 's') {
        int fd = open(resolution.sidecar_path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            catalog_send_error(cfd, "503 Service Unavailable", "source_unavailable");
            return;
        }
        struct stat st;
        if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
            close(fd);
            catalog_send_error(cfd, "503 Service Unavailable", "source_unavailable");
            return;
        }
        uint64_t actual_fingerprint = catalog_source_fingerprint(resolution.sidecar_path, (int64_t) st.st_mtime,
                                                                 (int64_t) st.st_size);
        if (actual_fingerprint != requested_fingerprint) {
            close(fd);
            catalog_send_error(cfd, "409 Conflict", "revision_or_cover_stale");
            return;
        }
        if ((uint64_t) st.st_size > CATALOG_ART_MAX_BYTES) {
            close(fd);
            catalog_send_error(cfd, "413 Content Too Large", "artwork_too_large");
            return;
        }
        if (st.st_size == 0) {
            close(fd);
            catalog_send_error(cfd, "404 Not Found", "artwork_missing");
            return;
        }
        metadata_db_catalog_page_t current_page;
        metadata_db_catalog_cover_t current_source;
        lookup = metadata_db_catalog_cover_source(revision, representative_id, &current_page, &current_source);
        if (lookup != METADATA_DB_CATALOG_OK ||
            strcmp(current_source.representative.path, source.representative.path) != 0) {
            close(fd);
            catalog_send_error(cfd, lookup == METADATA_DB_CATALOG_UNAVAILABLE ?
                               "503 Service Unavailable" : "409 Conflict",
                               lookup == METADATA_DB_CATALOG_UNAVAILABLE ?
                               "temporary_unavailable" : "revision_or_cover_stale");
            return;
        }
        uint8_t magic[16] = {0};
        ssize_t magic_size = pread(fd, magic, sizeof(magic), 0);
        if (magic_size < 0) magic_size = 0;
        const char * content_type = sniff_image_content_type(magic, (uint32_t) magic_size);
        if (lseek(fd, 0, SEEK_SET) < 0) {
            close(fd);
            catalog_send_error(cfd, "503 Service Unavailable", "source_unavailable");
            return;
        }
        catalog_stream_sidecar(cfd, fd, (size_t) st.st_size, content_type);
        close(fd);
        return;
    }

    struct stat before;
    if (stat(source.representative.path, &before) != 0 || !S_ISREG(before.st_mode)) {
        catalog_send_error(cfd, "503 Service Unavailable", "source_unavailable");
        return;
    }
    uint64_t actual_fingerprint = catalog_source_fingerprint(source.representative.path,
        (int64_t) before.st_mtime, (int64_t) before.st_size);
    if (actual_fingerprint != requested_fingerprint) {
        catalog_send_error(cfd, "409 Conflict", "revision_or_cover_stale");
        return;
    }

    artwork_acquire_result_t acquired = artwork_coordinator_acquire(ARTWORK_PRIO_WARMER,
        1024u * 1024u, 500, NULL, NULL);
    if (acquired != ARTWORK_ACQUIRE_OK) {
        catalog_send_error(cfd, "503 Service Unavailable", "artwork_busy");
        return;
    }
    track_metadata_t meta;
    memset(&meta, 0, sizeof(meta));
    metadata_artwork_result_t artwork = metadata_read_artwork_isolated(source.representative.path,
        &meta, 5000, ARTWORK_PRIO_WARMER);
    artwork_coordinator_release(ARTWORK_PRIO_WARMER);
    if (artwork == METADATA_ARTWORK_TEMPORARY_FAILURE) {
        free(meta.picture_data);
        free(meta.lyrics);
        catalog_send_error(cfd, "503 Service Unavailable", "artwork_unavailable");
        return;
    }
    if (artwork == METADATA_ARTWORK_TOO_LARGE) {
        free(meta.picture_data);
        free(meta.lyrics);
        catalog_send_error(cfd, "413 Content Too Large", "embedded_artwork_too_large");
        return;
    }
    if (artwork == METADATA_ARTWORK_INVALID) {
        free(meta.picture_data);
        free(meta.lyrics);
        catalog_send_error(cfd, "503 Service Unavailable", "embedded_artwork_invalid");
        return;
    }
    if (artwork != METADATA_ARTWORK_FOUND || !meta.picture_data || meta.picture_size == 0) {
        free(meta.picture_data);
        free(meta.lyrics);
        catalog_send_error(cfd, "404 Not Found", "artwork_missing");
        return;
    }
    if (meta.picture_size > CATALOG_ART_MAX_BYTES) {
        free(meta.picture_data);
        free(meta.lyrics);
        catalog_send_error(cfd, "413 Content Too Large", "artwork_too_large");
        return;
    }
    struct stat after;
    if (stat(source.representative.path, &after) != 0 || !S_ISREG(after.st_mode) ||
        after.st_mtime != before.st_mtime || after.st_size != before.st_size) {
        free(meta.picture_data);
        free(meta.lyrics);
        catalog_send_error(cfd, "409 Conflict", "revision_or_cover_stale");
        return;
    }
    metadata_db_catalog_page_t current_page;
    metadata_db_catalog_cover_t current_source;
    lookup = metadata_db_catalog_cover_source(revision, representative_id, &current_page, &current_source);
    if (lookup != METADATA_DB_CATALOG_OK ||
        strcmp(current_source.representative.path, source.representative.path) != 0) {
        free(meta.picture_data);
        free(meta.lyrics);
        catalog_send_error(cfd, lookup == METADATA_DB_CATALOG_UNAVAILABLE ?
                           "503 Service Unavailable" : "409 Conflict",
                           lookup == METADATA_DB_CATALOG_UNAVAILABLE ?
                           "temporary_unavailable" : "revision_or_cover_stale");
        return;
    }
    send_response_binary(cfd, "200 OK", sniff_image_content_type(meta.picture_data, meta.picture_size),
                         meta.picture_data, meta.picture_size);
    free(meta.picture_data);
    free(meta.lyrics);
}

static void handle_art_request(int cfd, const char * path) {
    int64_t index = -1;
    bool have_index = query_param_int64(path, "index", &index);

    char song_path[600] = {0};
    if (!resolve_art_source_path(have_index, index, song_path, sizeof(song_path))) {
        send_response(cfd, "404 Not Found", "text/plain", "No track");
        return;
    }

    track_metadata_t meta;
    memset(&meta, 0, sizeof(meta));
    metadata_read(song_path, &meta);

    if (meta.picture_data && meta.picture_size > 0) {
        send_response_binary(cfd, "200 OK", sniff_image_content_type(meta.picture_data, meta.picture_size),
                              meta.picture_data, meta.picture_size);
    } else {
        send_response(cfd, "404 Not Found", "text/plain", "No art");
    }
    free(meta.picture_data);
    free(meta.lyrics); /* only needed picture_data here */
}

/* Same definition as gui.c's own assets.c THEME_ROOT -- duplicated for the
 * same reason MUSIC_ROOT_DIR/PLAYLISTS_DIR are (assets.c doesn't expose it,
 * and this file already has its own copy of MUSIC_ROOT_DIR/PLAYLISTS_DIR
 * for the same reason). Only the stock, always-present theme2/category/
 * icons are served here -- no THEME_OVERRIDE_ROOT fallback, since none of
 * the whitelisted names below need one. */
#ifdef HOST_BUILD
#define THEME_ROOT "assets/theme2/"
#else
#define THEME_ROOT "/usr/resource/litegui/theme2/"
#endif

/* Fixed whitelist, not an arbitrary caller-supplied filename. Use the same bundled submenu icons
 * as build_music_screen() in gui_library.c. */
static const struct {
    const char * name;
    const char * relative_path;
} CATEGORY_ICONS[] = {
    { "all", "submenu/all_songs.png" },
    { "artist", "submenu/artists.png" },
    { "album_artist", "submenu/album_artist.png" },
    { "genre", "submenu/playlists.png" },
};

static uint8_t * read_whole_file(const char * path, size_t * out_size) {
    FILE * f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }

    uint8_t * buf = malloc((size_t) size);
    if (!buf) { fclose(f); return NULL; }
    size_t read_bytes = fread(buf, 1, (size_t) size, f);
    fclose(f);
    if (read_bytes != (size_t) size) {
        free(buf);
        return NULL;
    }
    *out_size = (size_t) size;
    return buf;
}

static void handle_icon_request(int cfd, const char * path) {
    char name[32] = {0};
    if (!query_param_str(path, "name", name, sizeof(name))) {
        send_response(cfd, "400 Bad Request", "text/plain", "Missing name");
        return;
    }

    const char * relative = NULL;
    for (size_t i = 0; i < sizeof(CATEGORY_ICONS) / sizeof(CATEGORY_ICONS[0]); i++) {
        if (strcmp(CATEGORY_ICONS[i].name, name) == 0) {
            relative = CATEGORY_ICONS[i].relative_path;
            break;
        }
    }
    if (!relative) {
        send_response(cfd, "404 Not Found", "text/plain", "Unknown icon");
        return;
    }

    char full_path[320];
    snprintf(full_path, sizeof(full_path), THEME_ROOT "%s", relative);
    size_t size;
    uint8_t * data = read_whole_file(full_path, &size);
    if (!data) {
        send_response(cfd, "404 Not Found", "text/plain", "Icon not found");
        return;
    }
    send_response_binary(cfd, "200 OK", "image/png", data, size);
    free(data);
}

/* Appends one prebuilt row while keeping `reserve` bytes for the closing
 * syntax, so a full page ends early instead of producing invalid JSON. */
static bool json_append_row(json_builder_t * json, const char * row, size_t reserve) {
    size_t len = strlen(row);
    if (json->length + len + reserve >= json->capacity) return false;
    memcpy(json->data + json->length, row, len + 1);
    json->length += len;
    return true;
}

/* Built-in playlists are derived from the library, never files, so every
 * mutation refuses them. The display-name aliases are the ones the original
 * routes already resolve to built-ins. */
static bool playlist_key_is_builtin(const char * key) {
    return key[0] == '@' || strcmp(key, "Favorites") == 0 || strcmp(key, "Most Played") == 0;
}

/* Opaque playlist revision: FNV-1a over the resolved entry sequence, the
 * same sequence positions index. Comment-only edits keep it; any entry
 * change, including a same-size reorder, changes it. */
static void playlist_revision_of(char * const * paths, int count, char out[20]) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (int i = 0; i < count; i++) {
        hash = catalog_hash_text(hash, paths[i]);
        hash ^= '\n';
        hash *= UINT64_C(1099511628211);
    }
    snprintf(out, 20, "p-%016llx", (unsigned long long) hash);
}

static void free_path_list(char ** paths, int count) {
    for (int i = 0; i < count; i++) free(paths[i]);
    free(paths);
}

/* GET /api/playlists/songs?name=NAME -- one playlist's songs, in file
 * order, in the same {"index":N,"title":...,"artist":...} shape
 * /api/library uses so the phone UI can render both with the same row
 * code. Entries whose path no longer matches anything in the synced
 * library are skipped, as on the player's own playlist screen; each row's
 * "position" is its entry offset in the file (skipped entries still count),
 * which the playlist edit routes take. User playlists also carry the
 * revision those routes check. */
static void handle_playlist_songs_request(int cfd, const char * path) {
    char raw_name[128] = {0}, name[128] = {0};
    if (!query_param_str(path, "name", raw_name, sizeof(raw_name))) {
        send_response(cfd, "400 Bad Request", "text/plain", "Bad Request");
        return;
    }
    url_decode(raw_name, name, sizeof(name));
    if (!playlist_name_is_safe(name)) {
        send_response(cfd, "400 Bad Request", "text/plain", "Bad Request");
        return;
    }

    char ** paths = NULL;
    int count = 0;
    bool loaded = false;
    bool user_playlist = false;
    if (strcmp(name, "@favorites") == 0 || strcmp(name, "Favorites") == 0) {
        metadata_db_load_favorite_songs(&paths, &count);
        loaded = true; /* an empty built-in playlist is valid */
    } else if (strcmp(name, "@most_played") == 0 || strcmp(name, "Most Played") == 0) {
        metadata_db_load_top_played_songs(20, &paths, &count);
        loaded = true;
    } else if (strcmp(name, "@recently_played") == 0) {
        metadata_db_load_recently_played_songs(METADATA_DB_RECENTLY_PLAYED_MAX, &paths, &count);
        loaded = true;
    } else {
        char m3u_path[512];
        snprintf(m3u_path, sizeof(m3u_path), "%s/%s.m3u", PLAYLISTS_DIR, name);
        loaded = playlist_files_read(m3u_path, &paths, &count);
        user_playlist = true;
    }
    if (!loaded) {
        send_response(cfd, "404 Not Found", "text/plain", "Playlist not found");
        return;
    }

    char * json_buf = malloc(65536);
    if (!json_buf) {
        free_path_list(paths, count);
        send_response(cfd, "500 Internal Server Error", "text/plain", "Out of memory");
        return;
    }

    /* Optional offset (an entry position) continues a list that did not fit
     * in one response; next_offset < total says there is more. */
    uint64_t start = 0;
    if (!catalog_query_number(path, "offset", 0, INT_MAX, &start)) {
        free(json_buf);
        free_path_list(paths, count);
        send_response(cfd, "400 Bad Request", "text/plain", "Bad Request");
        return;
    }
    char revision[20] = "";
    if (user_playlist) playlist_revision_of(paths, count, revision);
    /* Resolve playlist file paths against metadata_db and serialize to JSON
     * using json_builder_t with bounds checking. */
    json_builder_t json;
    json_builder_init(&json, json_buf, 65536);
    if (user_playlist)
        json_builder_appendf(&json, "{\"writable\":true,\"revision\":\"%s\",\"songs\":[", revision);
    else
        json_builder_appendf(&json, "{\"writable\":false,\"revision\":null,\"songs\":[");
    int emitted = 0;
    int next = start < (uint64_t) count ? (int) start : count;
    for (; next < count; next++) {
        song_row_t row;
        if (!metadata_db_get_song_by_path(paths[next], &row)) continue;

        char row_json[768], display_title[128], title_esc[300] = {0}, artist_esc[300] = {0};
        metadata_db_song_display_title(&row, display_title, sizeof(display_title));
        json_escape_append(title_esc, sizeof(title_esc), display_title);
        json_escape_append(artist_esc, sizeof(artist_esc), row.tags.artist);
        snprintf(row_json, sizeof(row_json), "%s{\"index\":%lld,\"title\":\"%s\",\"artist\":\"%s\",\"position\":%d}",
                 emitted > 0 ? "," : "", (long long) row.id, title_esc, artist_esc, next);
        if (!json_append_row(&json, row_json, 64)) break;
        emitted++;
    }
    json_builder_appendf(&json, "],\"total\":%d,\"next_offset\":%d}", count, next);

    send_response(cfd, "200 OK", "application/json", json_buf);

    free(json_buf);
    free_path_list(paths, count);
}

/* Reads and discards the request up to the blank line ending the headers
 * (this server never needs a request body -- see query_param_int()'s own
 * comment), keeping only the first line to parse the method/path out of.
 * Bounded read -- see REQUEST_LINE_MAX -- matches this project's general
 * "bound anything read from a socket" posture (http_client.c's own
 * response parsing does the same). */
#define REQUEST_LINE_MAX 4096
#define REQUEST_HEADERS_MAX 4096

/* ---- Remote Control v1 extensions: folders, recently played, favorites,
 * queue move/play, and playlist management. Each family reports errors as
 * {"error":"<family>_error","code":"<reason>"}; docs/REMOTE_CONTROL_API.md
 * lists every code. ---- */

/* Decodes one query value strictly (valid %XX escapes, no NUL).
 * Returns 0 when absent, 1 when decoded, -1 when too long or malformed. */
static int query_decoded(const char * path, const char * key, char * out, size_t out_size) {
    out[0] = '\0';
    if (!query_param_present(path, key)) return 0;
    char raw[REQUEST_LINE_MAX];
    if (!query_param_str(path, key, raw, sizeof(raw))) return -1;
    return url_decode_checked(raw, out, out_size) ? 1 : -1;
}

/* Required nonnegative integer no larger than max_value. */
static bool query_required_number(const char * path, const char * key, uint64_t max_value, uint64_t * out) {
    return query_param_present(path, key) && catalog_query_number(path, key, 0, max_value, out);
}

/* Offset/limit shared by the paged extension routes: offset defaults to 0,
 * limit to 50 and at most 100. */
#define EXT_PAGE_DEFAULT 50
#define EXT_PAGE_MAX 100

static bool query_paging(const char * path, int * out_offset, int * out_limit) {
    uint64_t offset = 0, limit = EXT_PAGE_DEFAULT;
    if (!catalog_query_number(path, "offset", 0, INT_MAX, &offset) ||
        !catalog_query_number(path, "limit", EXT_PAGE_DEFAULT, EXT_PAGE_MAX, &limit) || limit == 0) return false;
    *out_offset = (int) offset;
    *out_limit = (int) limit;
    return true;
}

/* ---- Folders ---- */

/* The player has a single music storage root, the SD card. */
#define FOLDER_ROOT_ID "sd"

/* A folder path is relative to the root: "" (or "/") is the root itself,
 * otherwise '/'-separated components that are not empty, ".", "..", or
 * hidden (leading '.', which the Files view never lists either), with no
 * backslash or control bytes. Writes the absolute path. */
static bool folder_resolve(const char * path, const char * key, char * out_rel, size_t rel_size,
                           char * out_abs, size_t abs_size, bool * out_valid) {
    *out_valid = false;
    char root_id[16];
    int root_state = query_decoded(path, "root", root_id, sizeof(root_id));
    if (root_state < 0 || (root_state == 1 && strcmp(root_id, FOLDER_ROOT_ID) != 0)) return false;
    char rel[REMOTE_CONTROL_PATH_MAX];
    if (query_decoded(path, key, rel, sizeof(rel)) < 0) return false;
    const char * start = rel[0] == '/' ? rel + 1 : rel;
    size_t len = strlen(start);
    if (len && start[len - 1] == '/') return false;
    const char * component = start;
    for (const char * p = start;; p++) {
        if (*p == '/' || *p == '\0') {
            size_t n = (size_t) (p - component);
            if (len && (n == 0 || component[0] == '.')) return false;
            if (*p == '\0') break;
            component = p + 1;
        } else if ((unsigned char) *p < 0x20 || *p == '\\') {
            return false;
        }
    }
    if (snprintf(out_rel, rel_size, "%s", start) >= (int) rel_size) return false;
    int n = len ? snprintf(out_abs, abs_size, "%s/%s", MUSIC_ROOT_DIR, start)
                : snprintf(out_abs, abs_size, "%s", MUSIC_ROOT_DIR);
    if (n < 0 || (size_t) n >= abs_size) return false;
    *out_valid = true;
    return true;
}

/* The mount point exists even with no card, so ask the player's own
 * mounted-card check rather than looking for the directory. */
static bool folder_root_available(void) {
    return sd_card_root_is_mounted();
}

/* An existing path must still resolve inside the root once symlinks are
 * followed (the SD card's FAT has none; this guards other mounts). */
static bool folder_within_root(const char * abs_path) {
    char root[PATH_MAX], real[PATH_MAX];
    if (!realpath(MUSIC_ROOT_DIR, root) || !realpath(abs_path, real)) return false;
    size_t n = strlen(root);
    return strncmp(real, root, n) == 0 && (real[n] == '\0' || real[n] == '/');
}

static void handle_folder_roots_request(int cfd) {
    char json[160];
    snprintf(json, sizeof(json),
             "{\"roots\":[{\"id\":\"" FOLDER_ROOT_ID "\",\"name\":\"SD card\",\"available\":%s}]}",
             folder_root_available() ? "true" : "false");
    send_response(cfd, "200 OK", "application/json", json);
}

/* Directories sort first in a file_browser index, so the folder count is
 * the first non-directory ordinal. */
static unsigned folder_count_of(const file_browser_index_t * index, unsigned count) {
    unsigned lo = 0, hi = count;
    char name[256];
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        bool is_dir = false;
        if (file_browser_index_entry_name(index, mid, name, sizeof(name), &is_dir) && is_dir) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

#define FOLDER_JSON_CAPACITY (128u * 1024u)

/* GET /api/folders?root=sd&path=REL&offset=N&limit=N -- one page of a
 * folder in the Files view's order (folders, then files by name; hidden
 * entries and symlinks omitted). Tracks carry their library song id, or -1
 * when the file is not in the library. */
static void handle_folder_list_request(int cfd, const char * path) {
    char rel[REMOTE_CONTROL_PATH_MAX], abs_path[REMOTE_CONTROL_PATH_MAX + sizeof(MUSIC_ROOT_DIR)];
    bool valid;
    int offset, limit;
    if (!folder_resolve(path, "path", rel, sizeof(rel), abs_path, sizeof(abs_path), &valid) || !valid) {
        send_api_error(cfd, "400 Bad Request", "folder_error", "invalid_path");
        return;
    }
    if (!query_paging(path, &offset, &limit)) {
        send_api_error(cfd, "400 Bad Request", "folder_error", "invalid_paging");
        return;
    }
    if (!folder_root_available()) {
        send_api_error(cfd, "503 Service Unavailable", "folder_error", "storage_unavailable");
        return;
    }
    struct stat st;
    if (stat(abs_path, &st) != 0 || !S_ISDIR(st.st_mode) || !folder_within_root(abs_path)) {
        send_api_error(cfd, "404 Not Found", "folder_error", "folder_not_found");
        return;
    }
    file_browser_index_t * index = NULL;
    unsigned count = 0;
    if (!file_browser_index_open(abs_path, &index, &count)) {
        send_api_error(cfd, "503 Service Unavailable", "folder_error",
                       errno == EFBIG ? "folder_too_large" : "folder_unavailable");
        return;
    }
    unsigned folders = folder_count_of(index, count);
    unsigned first = (unsigned) offset < count ? (unsigned) offset : count;
    unsigned page = count - first < (unsigned) limit ? count - first : (unsigned) limit;

    /* Resolve the page's track ids in one batched lookup. */
    char (*names)[256] = page ? calloc(page, sizeof(*names)) : NULL;
    bool * dirs = page ? calloc(page, sizeof(*dirs)) : NULL;
    char ** track_paths = page ? calloc(page, sizeof(*track_paths)) : NULL;
    song_row_t * rows = page ? calloc(page, sizeof(*rows)) : NULL;
    char * json_buf = malloc(FOLDER_JSON_CAPACITY);
    bool ok = json_buf && (!page || (names && dirs && track_paths && rows));
    int track_count = 0;
    int * track_at = page ? calloc(page, sizeof(*track_at)) : NULL;
    if (page && !track_at) ok = false;
    for (unsigned i = 0; ok && i < page; i++) {
        track_at[i] = -1;
        if (!file_browser_index_entry_name(index, first + i, names[i], sizeof(names[i]), &dirs[i])) { ok = false; break; }
        if (dirs[i] || library_is_m3u_file(names[i])) continue;
        char full[REMOTE_CONTROL_PATH_MAX + sizeof(MUSIC_ROOT_DIR) + 256];
        snprintf(full, sizeof(full), "%s/%s", abs_path, names[i]);
        track_paths[track_count] = strdup(full);
        if (!track_paths[track_count]) { ok = false; break; }
        track_at[i] = track_count++;
    }
    file_browser_index_close(index);
    if (ok && track_count) metadata_db_get_songs_by_paths((const char * const *) track_paths, track_count, rows);

    unsigned emitted = 0;
    if (ok) {
        json_builder_t json;
        json_builder_init(&json, json_buf, FOLDER_JSON_CAPACITY);
        char rel_esc[REMOTE_CONTROL_PATH_MAX * 6] = {0}, parent_esc[REMOTE_CONTROL_PATH_MAX * 6] = {0};
        json_escape_append(rel_esc, sizeof(rel_esc), rel);
        const char * slash = strrchr(rel, '/');
        if (slash) {
            char parent[REMOTE_CONTROL_PATH_MAX];
            snprintf(parent, sizeof(parent), "%.*s", (int) (slash - rel), rel);
            json_escape_append(parent_esc, sizeof(parent_esc), parent);
        }
        json_builder_appendf(&json,
            "{\"root\":\"" FOLDER_ROOT_ID "\",\"path\":\"%s\",\"parent\":%s%s%s,\"total\":%u,"
            "\"folder_count\":%u,\"offset\":%d,\"entries\":[",
            rel_esc, rel[0] ? "\"" : "null", rel[0] ? parent_esc : "", rel[0] ? "\"" : "",
            count, folders, offset);
        for (unsigned i = 0; i < page && !json.truncated; i++) {
            char row[4096], name_esc[256 * 6] = {0};
            json_escape_append(name_esc, sizeof(name_esc), names[i]);
            if (dirs[i] || track_at[i] < 0) {
                snprintf(row, sizeof(row), "%s{\"type\":\"%s\",\"name\":\"%s\"}", emitted ? "," : "",
                         dirs[i] ? "folder" : "playlist", name_esc);
            } else {
                const song_row_t * song = &rows[track_at[i]];
                bool known = song->id >= 0;
                char title[128] = {0}, title_esc[800] = {0}, artist_esc[800] = {0};
                if (known) metadata_db_song_display_title(song, title, sizeof(title));
                json_escape_append(title_esc, sizeof(title_esc), known ? title : names[i]);
                json_escape_append(artist_esc, sizeof(artist_esc), known ? song->tags.artist : "");
                snprintf(row, sizeof(row),
                         "%s{\"type\":\"track\",\"name\":\"%s\",\"index\":%lld,\"title\":\"%s\",\"artist\":\"%s\"}",
                         emitted ? "," : "", name_esc, known ? (long long) song->id : -1LL, title_esc, artist_esc);
            }
            if (!json_append_row(&json, row, 64)) break;
            emitted++;
        }
        json.truncated = false;
        json_builder_appendf(&json, "],\"next_offset\":%u}", first + emitted);
        send_response(cfd, "200 OK", "application/json", json_buf);
    } else {
        send_api_error(cfd, "503 Service Unavailable", "folder_error", "temporary_unavailable");
    }
    for (int i = 0; i < track_count; i++) free(track_paths[i]);
    free(track_paths);
    free(track_at);
    free(names);
    free(dirs);
    free(rows);
    free(json_buf);
}

/* POST /api/folders/play?root=sd&path=REL_FILE -- plays a file with its
 * folder as the queue, as tapping it in Files does. */
static void handle_folder_play_request(int cfd, const char * path) {
    char rel[REMOTE_CONTROL_PATH_MAX], abs_path[REMOTE_CONTROL_PATH_MAX + sizeof(MUSIC_ROOT_DIR)];
    bool valid;
    if (!folder_resolve(path, "path", rel, sizeof(rel), abs_path, sizeof(abs_path), &valid) || !valid || !rel[0] ||
        strlen(abs_path) >= REMOTE_CONTROL_PATH_MAX) {
        send_api_error(cfd, "400 Bad Request", "folder_error", "invalid_path");
        return;
    }
    const char * base = strrchr(abs_path, '/');
    struct stat st;
    if (!folder_root_available()) {
        send_api_error(cfd, "503 Service Unavailable", "folder_error", "storage_unavailable");
        return;
    }
    if (lstat(abs_path, &st) != 0 || !S_ISREG(st.st_mode) || !folder_within_root(abs_path)) {
        send_api_error(cfd, "404 Not Found", "folder_error", "track_not_found");
        return;
    }
    if (!file_browser_is_playable_name(base ? base + 1 : abs_path)) {
        send_api_error(cfd, "400 Bad Request", "folder_error", "not_playable");
        return;
    }
    pthread_mutex_lock(&status_mutex);
    request_has_folder_play = true;
    memcpy(request_folder_play_path, abs_path, strlen(abs_path) + 1); /* length checked above */
    pthread_mutex_unlock(&status_mutex);
    send_response(cfd, "200 OK", "text/plain", "OK");
}

/* ---- Recently played ---- */

static void handle_recent_request(int cfd, const char * path) {
    int offset, limit;
    if (!query_paging(path, &offset, &limit)) {
        send_api_error(cfd, "400 Bad Request", "recent_error", "invalid_paging");
        return;
    }
    song_row_t * rows = calloc((size_t) limit, sizeof(*rows));
    int64_t * played = calloc((size_t) limit, sizeof(*played));
    char * json_buf = malloc(65536);
    int total = 0;
    int n = rows && played && json_buf ? metadata_db_get_recently_played_page(offset, limit, rows, played, &total) : -1;
    if (n < 0) {
        free(rows); free(played); free(json_buf);
        send_api_error(cfd, "503 Service Unavailable", "recent_error", "temporary_unavailable");
        return;
    }
    json_builder_t json;
    json_builder_init(&json, json_buf, 65536);
    json_builder_appendf(&json, "{\"total\":%d,\"offset\":%d,\"songs\":[", total, offset);
    int emitted = 0;
    for (int i = 0; i < n; i++) {
        char row[1024], title[128], title_esc[300] = {0}, artist_esc[300] = {0};
        metadata_db_song_display_title(&rows[i], title, sizeof(title));
        json_escape_append(title_esc, sizeof(title_esc), title);
        json_escape_append(artist_esc, sizeof(artist_esc), rows[i].tags.artist);
        snprintf(row, sizeof(row), "%s{\"index\":%lld,\"title\":\"%s\",\"artist\":\"%s\",\"last_played\":%lld}",
                 emitted ? "," : "", (long long) rows[i].id, title_esc, artist_esc, (long long) played[i]);
        if (!json_append_row(&json, row, 64)) break;
        emitted++;
    }
    json_builder_appendf(&json, "],\"next_offset\":%d}", offset + emitted);
    send_response(cfd, "200 OK", "application/json", json_buf);
    free(rows); free(played); free(json_buf);
}

/* ---- Favorites ---- */

#define FAVORITE_STATE_MAX_IDS 100

/* GET /api/favorites/state?ids=1,2,3 */
static void handle_favorite_state_request(int cfd, const char * path) {
    char ids_text[FAVORITE_STATE_MAX_IDS * 12];
    int64_t ids[FAVORITE_STATE_MAX_IDS];
    int count = 0;
    if (query_decoded(path, "ids", ids_text, sizeof(ids_text)) != 1 || !ids_text[0]) {
        send_api_error(cfd, "400 Bad Request", "favorite_error", "invalid_ids");
        return;
    }
    for (char * p = ids_text; *p;) {
        char * end = NULL;
        errno = 0;
        long long v = (*p >= '0' && *p <= '9') ? strtoll(p, &end, 10) : -1;
        if (v <= 0 || errno == ERANGE || v > INT32_MAX || (*end != ',' && *end != '\0') ||
            count >= FAVORITE_STATE_MAX_IDS) {
            send_api_error(cfd, "400 Bad Request", "favorite_error", "invalid_ids");
            return;
        }
        ids[count++] = (int64_t) v;
        p = *end ? end + 1 : end;
        if (*end == ',' && !*p) {
            send_api_error(cfd, "400 Bad Request", "favorite_error", "invalid_ids");
            return;
        }
    }
    song_row_t * rows = calloc((size_t) count, sizeof(*rows));
    char * json_buf = malloc(8192);
    if (!rows || !json_buf) {
        free(rows); free(json_buf);
        send_api_error(cfd, "503 Service Unavailable", "favorite_error", "temporary_unavailable");
        return;
    }
    metadata_db_get_songs_by_ids(ids, count, rows);
    json_builder_t json;
    json_builder_init(&json, json_buf, 8192);
    json_builder_appendf(&json, "{\"songs\":[");
    for (int i = 0; i < count; i++) {
        bool found = rows[i].id >= 0;
        bool favorite = found && metadata_db_song_favorite_is_set(rows[i].path);
        json_builder_appendf(&json, "%s{\"index\":%lld,\"found\":%s,\"favorite\":%s}", i ? "," : "",
                             (long long) ids[i], found ? "true" : "false", favorite ? "true" : "false");
    }
    json_builder_appendf(&json, "]}");
    send_response(cfd, "200 OK", "application/json", json_buf);
    free(rows);
    free(json_buf);
}

/* POST /api/favorites/add or /remove with index=ID (optional
 * catalog_revision) or current=1 for the now-playing file. Favorites are the
 * library's rating flag, which also backs the @favorites playlist, so that
 * playlist follows without a separate write. */
static void handle_favorite_edit_request(int cfd, const char * path, bool is_favorite) {
    bool current = query_param_present(path, "current");
    bool have_index = query_param_present(path, "index");
    if (current == have_index) {
        send_api_error(cfd, "400 Bad Request", "favorite_error", "invalid_target");
        return;
    }
    song_row_t row;
    char song_path[REMOTE_CONTROL_PATH_MAX] = {0};
    int64_t index = -1;
    if (current) {
        uint64_t flag = 0;
        if (!catalog_query_number(path, "current", 0, 1, &flag) || flag != 1) {
            send_api_error(cfd, "400 Bad Request", "favorite_error", "invalid_target");
            return;
        }
        pthread_mutex_lock(&status_mutex);
        snprintf(song_path, sizeof(song_path), "%s", status_path);
        pthread_mutex_unlock(&status_mutex);
        if (!song_path[0]) {
            send_api_error(cfd, "404 Not Found", "favorite_error", "nothing_playing");
            return;
        }
        if (metadata_db_get_song_by_path(song_path, &row)) index = row.id;
    } else {
        uint64_t id = 0;
        char revision[METADATA_DB_CATALOG_REVISION_SIZE] = {0};
        if (!query_required_number(path, "index", INT32_MAX, &id) || id == 0 ||
            !catalog_query_revision_named(path, "catalog_revision", false, revision)) {
            send_api_error(cfd, "400 Bad Request", "favorite_error", "invalid_index");
            return;
        }
        metadata_db_catalog_result_t lookup = METADATA_DB_CATALOG_OK;
        bool found;
        if (revision[0]) {
            lookup = metadata_db_catalog_get_song_revision(revision, (int64_t) id, &row);
            found = lookup == METADATA_DB_CATALOG_OK;
        } else {
            found = metadata_db_get_song_by_id((int64_t) id, &row);
        }
        if (lookup == METADATA_DB_CATALOG_STALE) {
            send_api_error(cfd, "409 Conflict", "favorite_error", "revision_mismatch");
            return;
        }
        if (lookup == METADATA_DB_CATALOG_UNAVAILABLE) {
            send_api_error(cfd, "503 Service Unavailable", "favorite_error", "temporary_unavailable");
            return;
        }
        if (!found) {
            send_api_error(cfd, "404 Not Found", "favorite_error", "song_not_found");
            return;
        }
        index = row.id;
        snprintf(song_path, sizeof(song_path), "%s", row.path);
    }
    /* Ordered with the player's own heart taps, and confirmed on disk. */
    if (!favorite_writer_write_now(song_path, is_favorite)) {
        send_api_error(cfd, "503 Service Unavailable", "favorite_error", "write_failed");
        return;
    }
    pthread_mutex_lock(&status_mutex);
    request_favorite_changed = true; /* the UI reloads the now-playing heart */
    if (strcmp(status_path, song_path) == 0) status_favorite = is_favorite;
    pthread_mutex_unlock(&status_mutex);
    char json[96];
    snprintf(json, sizeof(json), "{\"index\":%lld,\"favorite\":%s}", (long long) index, is_favorite ? "true" : "false");
    send_response(cfd, "200 OK", "application/json", json);
}

/* ---- Queue move / play ---- */

/* POST /api/queue/move?from=N&to=N&revision=R and
 * POST /api/queue/play?offset=N&revision=R. Offsets index the Up Next list
 * of that revision. Accepted requests are applied on the UI thread, which
 * rechecks the revision and ignores the request if the queue changed. */
static void handle_queue_edit_request(int cfd, const char * path, bool move) {
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
    bool pending = request_has_queue_move || request_has_queue_play || request_has_queue_remove || request_queue_clear;
    if (revision != queue_snapshot_revision) code = "revision_mismatch";
    else if (pending) code = "edit_pending";
    else if (from >= (uint64_t) queue_song_id_count || (move && to >= (uint64_t) queue_song_id_count)) code = "invalid_offset";
    else if (move) {
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
    else if (strcmp(code, "invalid_offset") != 0) send_api_error(cfd, "409 Conflict", "queue_error", code);
    else send_api_error(cfd, "400 Bad Request", "queue_error", code);
}

/* ---- Playlist management ---- */

/* A new or renamed user playlist name: the playlist file rules, plus what
 * the API needs to address it afterwards as PLAYLISTS_DIR/<name>.m3u
 * (stem only, no "..", 127 bytes, not a built-in key or alias). */
static const char * playlist_new_name_error(const char * name) {
    if (!playlist_files_name_is_valid(name) || !playlist_name_is_safe(name) ||
        strlen(name) > PLAYLIST_NAME_MAX_BYTES || library_is_m3u_file(name)) return "invalid_name";
    if (playlist_key_is_builtin(name)) return "reserved_name";
    return NULL;
}

/* revision is NULL when the file could not be reread after a successful
 * change; the client then rereads the playlist. */
static void playlist_send_ok(int cfd, const char * key, const char * revision) {
    char key_esc[PLAYLIST_NAME_MAX_BYTES * 6 + 1] = {0}, json[PLAYLIST_NAME_MAX_BYTES * 12 + 96];
    json_escape_append(key_esc, sizeof(key_esc), key);
    snprintf(json, sizeof(json), "{\"key\":\"%s\",\"name\":\"%s.m3u\",\"revision\":%s%s%s}",
             key_esc, key_esc, revision ? "\"" : "", revision ? revision : "null", revision ? "\"" : "");
    send_response(cfd, "200 OK", "application/json", json);
}

/* Resolves the `name` key of an existing user playlist. Sends the error and
 * returns false for a malformed, built-in, or missing playlist. */
static bool playlist_existing_path(int cfd, const char * path, char * key, size_t key_size,
                                   char * m3u_path, size_t m3u_size) {
    int state = query_decoded(path, "name", key, key_size);
    if (state != 1 || !key[0] || !playlist_name_is_safe(key) || strlen(key) > PLAYLIST_NAME_MAX_BYTES) {
        send_api_error(cfd, "400 Bad Request", "playlist_error", "invalid_name");
        return false;
    }
    if (playlist_key_is_builtin(key)) {
        send_api_error(cfd, "403 Forbidden", "playlist_error", "read_only");
        return false;
    }
    struct stat st;
    if (snprintf(m3u_path, m3u_size, "%s/%s.m3u", PLAYLISTS_DIR, key) >= (int) m3u_size ||
        stat(m3u_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        send_api_error(cfd, "404 Not Found", "playlist_error", "playlist_not_found");
        return false;
    }
    return true;
}

static bool playlist_current_revision(const char * m3u_path, char revision[20], int * out_count) {
    char ** paths = NULL;
    int count = 0;
    playlist_read_status_t status = playlist_files_read_ex(m3u_path, &paths, &count);
    if (status != PLAYLIST_READ_OK && status != PLAYLIST_READ_EMPTY) return false;
    playlist_revision_of(paths, count, revision);
    free_path_list(paths, count);
    if (out_count) *out_count = count;
    return true;
}

/* POST /api/playlists/create?name=NAME -- a new empty user playlist. */
static void handle_playlist_create_request(int cfd, const char * path) {
    char name[PLAYLIST_NAME_MAX_BYTES + 2];
    const char * error = query_decoded(path, "name", name, sizeof(name)) == 1 ? playlist_new_name_error(name)
                                                                               : "invalid_name";
    if (error) {
        send_api_error(cfd, "400 Bad Request", "playlist_error", error);
        return;
    }
    char m3u_path[512];
    snprintf(m3u_path, sizeof(m3u_path), "%s/%s.m3u", PLAYLISTS_DIR, name);
    errno = 0;
    if (access(m3u_path, F_OK) == 0 ||
        !playlist_files_write_new(PLAYLISTS_DIR, name, NULL, 0, m3u_path, sizeof(m3u_path))) {
        bool exists = errno == EEXIST || access(m3u_path, F_OK) == 0;
        send_api_error(cfd, exists ? "409 Conflict" : "500 Internal Server Error", "playlist_error",
                       exists ? "name_exists" : "write_failed");
        return;
    }
    metadata_db_playlist_insert_one(m3u_path);
    char revision[20];
    playlist_revision_of(NULL, 0, revision);
    playlist_send_ok(cfd, name, revision);
}

/* POST /api/playlists/rename?name=KEY&new_name=NAME */
static void handle_playlist_rename_request(int cfd, const char * path) {
    char key[PLAYLIST_NAME_MAX_BYTES + 2], m3u_path[512], new_name[PLAYLIST_NAME_MAX_BYTES + 2];
    if (!playlist_existing_path(cfd, path, key, sizeof(key), m3u_path, sizeof(m3u_path))) return;
    const char * error = query_decoded(path, "new_name", new_name, sizeof(new_name)) == 1
                             ? playlist_new_name_error(new_name) : "invalid_name";
    if (error) {
        send_api_error(cfd, "400 Bad Request", "playlist_error", error);
        return;
    }
    char dest[512], revision[20];
    snprintf(dest, sizeof(dest), "%s/%s.m3u", PLAYLISTS_DIR, new_name);
    if (strcmp(dest, m3u_path) != 0 && access(dest, F_OK) == 0) {
        send_api_error(cfd, "409 Conflict", "playlist_error", "name_exists");
        return;
    }
    if (!playlist_files_rename(m3u_path, new_name, dest, sizeof(dest))) {
        bool exists = access(dest, F_OK) == 0 && strcmp(dest, m3u_path) != 0;
        send_api_error(cfd, exists ? "409 Conflict" : "500 Internal Server Error", "playlist_error",
                       exists ? "name_exists" : "write_failed");
        return;
    }
    metadata_db_playlist_delete_one(m3u_path);
    metadata_db_playlist_insert_one(dest);
    playlist_send_ok(cfd, new_name, playlist_current_revision(dest, revision, NULL) ? revision : NULL);
}

/* POST /api/playlists/delete?name=KEY */
static void handle_playlist_delete_request(int cfd, const char * path) {
    char key[PLAYLIST_NAME_MAX_BYTES + 2], m3u_path[512];
    if (!playlist_existing_path(cfd, path, key, sizeof(key), m3u_path, sizeof(m3u_path))) return;
    if (!playlist_files_delete(m3u_path)) {
        send_api_error(cfd, "500 Internal Server Error", "playlist_error", "write_failed");
        return;
    }
    metadata_db_playlist_delete_one(m3u_path);
    char key_esc[PLAYLIST_NAME_MAX_BYTES * 6 + 1] = {0}, json[PLAYLIST_NAME_MAX_BYTES * 6 + 48];
    json_escape_append(key_esc, sizeof(key_esc), key);
    snprintf(json, sizeof(json), "{\"key\":\"%s\",\"deleted\":true}", key_esc);
    send_response(cfd, "200 OK", "application/json", json);
}

typedef struct {
    const char * expected_revision;
    uint64_t from, to;
    bool move;
    const char * refusal;
} playlist_entry_check_t;

/* Runs under the playlist lock with the entries as they are right now. */
static bool playlist_entry_check(void * context, char * const * paths, int count) {
    playlist_entry_check_t * check = context;
    char revision[20];
    playlist_revision_of(paths, count, revision);
    if (strcmp(revision, check->expected_revision) != 0) check->refusal = "revision_mismatch";
    else if (check->from >= (uint64_t) count || (check->move && check->to >= (uint64_t) count))
        check->refusal = "invalid_position";
    return check->refusal == NULL;
}

/* POST /api/playlists/remove?name=KEY&position=N&revision=R and
 * POST /api/playlists/move?name=KEY&from=N&to=N&revision=R. Positions are
 * entry offsets from GET /playlists/songs of that revision; the revision
 * is checked under the same lock as the edit. */
static void handle_playlist_entry_request(int cfd, const char * path, bool move) {
    char key[PLAYLIST_NAME_MAX_BYTES + 2], m3u_path[512], expected[24], revision[20];
    if (!playlist_existing_path(cfd, path, key, sizeof(key), m3u_path, sizeof(m3u_path))) return;
    playlist_entry_check_t check = { .expected_revision = expected, .move = move };
    bool valid = query_decoded(path, "revision", expected, sizeof(expected)) == 1 &&
                 (move ? query_required_number(path, "from", INT_MAX, &check.from) &&
                         query_required_number(path, "to", INT_MAX, &check.to)
                       : query_required_number(path, "position", INT_MAX, &check.from));
    if (!valid) {
        send_api_error(cfd, "400 Bad Request", "playlist_error", "invalid_request");
        return;
    }
    if (move && check.from == check.to) {
        /* Nothing to write; still report a stale revision or bad position. */
        char ** paths = NULL;
        int count = 0;
        playlist_read_status_t read = playlist_files_read_ex(m3u_path, &paths, &count);
        bool readable = read == PLAYLIST_READ_OK || read == PLAYLIST_READ_EMPTY;
        bool accepted = readable && playlist_entry_check(&check, paths, count);
        free_path_list(paths, count);
        if (!readable) {
            send_api_error(cfd, "500 Internal Server Error", "playlist_error", "read_failed");
            return;
        }
        if (!accepted) {
            send_api_error(cfd, strcmp(check.refusal, "revision_mismatch") == 0 ? "409 Conflict" : "400 Bad Request",
                           "playlist_error", check.refusal);
            return;
        }
        playlist_send_ok(cfd, key, expected);
        return;
    }
    playlist_edit_status_t status = playlist_files_edit_entry_checked(
        m3u_path, (int) check.from, move ? (int) check.to : -1, playlist_entry_check, &check);
    if (status == PLAYLIST_EDIT_REFUSED) {
        send_api_error(cfd, strcmp(check.refusal, "revision_mismatch") == 0 ? "409 Conflict" : "400 Bad Request",
                       "playlist_error", check.refusal);
        return;
    }
    if (status != PLAYLIST_EDIT_OK) {
        send_api_error(cfd, "500 Internal Server Error", "playlist_error", "write_failed");
        return;
    }
    playlist_send_ok(cfd, key, playlist_current_revision(m3u_path, revision, NULL) ? revision : NULL);
}

/* The one request body in the API: an uploaded .m3u/.m3u8 file. */
#define PLAYLIST_IMPORT_MAX_BYTES (256u * 1024u)

typedef struct {
    int fd;
    size_t consumed;
} body_reader_t;

static ssize_t body_read(void * context, void * buffer, size_t size) {
    body_reader_t * body = context;
    for (;;) {
        struct pollfd pfd = { .fd = body->fd, .events = POLLIN, .revents = 0 };
        int ready = poll(&pfd, 1, 2000);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) return -1;
        ssize_t n = read(body->fd, buffer, size);
        if (n < 0 && errno == EINTR) continue;
        if (n > 0) body->consumed += (size_t) n;
        return n;
    }
}

/* Upper bounds for reading the rest of a rejected upload after replying. */
#define IMPORT_LINGER_MAX_BYTES (8u * 1024u * 1024u)
#define IMPORT_LINGER_MAX_SECONDS 10

/* Error reply for an upload whose body was not fully read. Closing a TCP
 * socket with unread data can reset the connection and discard the reply,
 * so send it, half-close, and read what is left of the body (at most
 * `remaining` bytes, bounded in bytes and time) before the caller closes. */
static void import_send_error(int cfd, const char * status, const char * code, size_t remaining) {
    send_api_error(cfd, status, "playlist_error", code);
    if (remaining == 0) return;
    shutdown(cfd, SHUT_WR);
    if (remaining > IMPORT_LINGER_MAX_BYTES) remaining = IMPORT_LINGER_MAX_BYTES;
    time_t give_up = time(NULL) + IMPORT_LINGER_MAX_SECONDS;
    body_reader_t body = { cfd, 0 };
    char buffer[1024];
    while (remaining > 0 && time(NULL) < give_up) {
        ssize_t n = body_read(&body, buffer, remaining < sizeof(buffer) ? remaining : sizeof(buffer));
        if (n <= 0) return;
        remaining -= (size_t) n;
    }
}

/* POST /api/playlists/import?name=NAME with the playlist file as the body
 * (Content-Length required). The bytes are stored unchanged as
 * PLAYLISTS_DIR/NAME.m3u; entries keep their own paths, so "matched" counts
 * those that resolve to library songs on this player. */
static void handle_playlist_import_request(int cfd, const char * path, long long content_length) {
    if (content_length < 0) {
        /* No length (or chunked): drain whatever follows, bounded. */
        import_send_error(cfd, "411 Length Required", "length_required", IMPORT_LINGER_MAX_BYTES);
        return;
    }
    size_t length = (unsigned long long) content_length > IMPORT_LINGER_MAX_BYTES ? IMPORT_LINGER_MAX_BYTES
                                                                                   : (size_t) content_length;
    if ((unsigned long long) content_length > PLAYLIST_IMPORT_MAX_BYTES) {
        import_send_error(cfd, "413 Content Too Large", "playlist_too_large", length);
        return;
    }
    char name[PLAYLIST_NAME_MAX_BYTES + 2];
    const char * error = query_decoded(path, "name", name, sizeof(name)) == 1 ? playlist_new_name_error(name)
                                                                               : "invalid_name";
    if (error) {
        import_send_error(cfd, "400 Bad Request", error, length);
        return;
    }
    body_reader_t body = { cfd, 0 };
    char m3u_path[512];
    char ** paths = NULL;
    int count = 0;
    playlist_import_status_t status = playlist_files_import_stream(PLAYLISTS_DIR, name, length,
        body_read, &body, m3u_path, sizeof(m3u_path), &paths, &count);
    size_t unread = length - body.consumed;
    switch (status) {
        case PLAYLIST_IMPORT_OK: break;
        case PLAYLIST_IMPORT_EXISTS:
            import_send_error(cfd, "409 Conflict", "name_exists", unread);
            return;
        case PLAYLIST_IMPORT_INVALID_NAME:
            import_send_error(cfd, "400 Bad Request", "invalid_name", unread);
            return;
        case PLAYLIST_IMPORT_INVALID_CONTENT:
            import_send_error(cfd, "400 Bad Request", "invalid_playlist", unread);
            return;
        case PLAYLIST_IMPORT_READ_ERROR:
            import_send_error(cfd, "400 Bad Request", "incomplete_body", unread);
            return;
        case PLAYLIST_IMPORT_IO_ERROR:
        default:
            import_send_error(cfd, "500 Internal Server Error", "write_failed", unread);
            return;
    }
    metadata_db_playlist_insert_one(m3u_path);
    char revision[20];
    playlist_revision_of(paths, count, revision);
    int matched = 0;
    enum { MATCH_BATCH = 32 };
    song_row_t * rows = malloc(sizeof(*rows) * MATCH_BATCH);
    for (int i = 0; rows && i < count; i += MATCH_BATCH) {
        int batch = count - i < MATCH_BATCH ? count - i : MATCH_BATCH;
        metadata_db_get_songs_by_paths((const char * const *) paths + i, batch, rows);
        for (int j = 0; j < batch; j++) if (rows[j].id >= 0) matched++;
    }
    free(rows);
    free_path_list(paths, count);
    char key_esc[PLAYLIST_NAME_MAX_BYTES * 6 + 1] = {0}, json[PLAYLIST_NAME_MAX_BYTES * 12 + 160];
    json_escape_append(key_esc, sizeof(key_esc), name);
    snprintf(json, sizeof(json),
             "{\"key\":\"%s\",\"name\":\"%s.m3u\",\"revision\":\"%s\",\"entries\":%d,\"matched\":%d}",
             key_esc, key_esc, revision, count, matched);
    send_response(cfd, "200 OK", "application/json", json);
}

/* Routes added after the original API. Returns false for any other path. */
static bool handle_extension_route(int cfd, const char * method, const char * path_only, const char * path,
                                   long long content_length) {
    bool get = strcmp(method, "GET") == 0, post = strcmp(method, "POST") == 0;
    if (get && strcmp(path_only, "/api/folders/roots") == 0) handle_folder_roots_request(cfd);
    else if (get && strcmp(path_only, "/api/folders") == 0) handle_folder_list_request(cfd, path);
    else if (post && strcmp(path_only, "/api/folders/play") == 0) handle_folder_play_request(cfd, path);
    else if (get && strcmp(path_only, "/api/recent") == 0) handle_recent_request(cfd, path);
    else if (get && strcmp(path_only, "/api/favorites/state") == 0) handle_favorite_state_request(cfd, path);
    else if (post && strcmp(path_only, "/api/favorites/add") == 0) handle_favorite_edit_request(cfd, path, true);
    else if (post && strcmp(path_only, "/api/favorites/remove") == 0) handle_favorite_edit_request(cfd, path, false);
    else if (post && strcmp(path_only, "/api/queue/move") == 0) handle_queue_edit_request(cfd, path, true);
    else if (post && strcmp(path_only, "/api/queue/play") == 0) handle_queue_edit_request(cfd, path, false);
    else if (post && strcmp(path_only, "/api/playlists/create") == 0) handle_playlist_create_request(cfd, path);
    else if (post && strcmp(path_only, "/api/playlists/rename") == 0) handle_playlist_rename_request(cfd, path);
    else if (post && strcmp(path_only, "/api/playlists/delete") == 0) handle_playlist_delete_request(cfd, path);
    else if (post && strcmp(path_only, "/api/playlists/remove") == 0) handle_playlist_entry_request(cfd, path, false);
    else if (post && strcmp(path_only, "/api/playlists/move") == 0) handle_playlist_entry_request(cfd, path, true);
    else if (post && strcmp(path_only, "/api/playlists/import") == 0)
        handle_playlist_import_request(cfd, path, content_length);
    else return false;
    return true;
}

static void handle_connection(int cfd) {
    if (!remote_control_ensure_pin()) {
        send_response(cfd, "503 Service Unavailable", "text/plain", "PIN generation unavailable");
        return;
    }
    char buf[REQUEST_LINE_MAX];
    size_t n = 0;
    bool line_complete = false;
    /* Stream transports (including RFCOMM) may deliver the request line in
     * several reads. Read through LF, bounded by REQUEST_LINE_MAX and an idle
     * timeout. */
    while (n + 1 < sizeof(buf)) {
        struct pollfd pfd = { .fd = cfd, .events = POLLIN, .revents = 0 };
        int ready = poll(&pfd, 1, 2000);
        if (ready <= 0) {
            if (ready < 0 && errno == EINTR) continue;
            if (ready == 0) send_response(cfd, "408 Request Timeout", "text/plain", "Request Timeout");
            return;
        }
        char c;
        ssize_t nr = read(cfd, &c, 1);
        if (nr <= 0) return; /* caller owns fd cleanup */
        buf[n++] = c;
        if (c == '\n') {
            line_complete = true;
            break;
        }
    }
    if (!line_complete) {
        send_response(cfd, "414 URI Too Long", "text/plain", "Request line too long");
        return;
    }
    buf[n] = '\0';

    /* Consume the headers before replying. Leaving them unread when closing a
     * TCP socket can reset the connection and discard the response. This API
     * has no request-body endpoints, so only the bounded header section is
     * consumed. Accept CRLF or LF line endings. */
    size_t header_bytes = 0;
    bool headers_complete = false;
    char pin_header[REMOTE_CONTROL_PIN_MAX_LENGTH + 1] = {0};
    bool pin_header_seen = false;
    bool pin_header_invalid = false;
    long long content_length = -1; /* only POST /playlists/import reads a body */
    bool content_length_invalid = false;
    char header_line[512];
    size_t header_line_len = 0;
    while (header_bytes < REQUEST_HEADERS_MAX) {
        struct pollfd pfd = { .fd = cfd, .events = POLLIN, .revents = 0 };
        int ready = poll(&pfd, 1, 2000);
        if (ready <= 0) {
            if (ready < 0 && errno == EINTR) continue;
            send_response(cfd, ready == 0 ? "408 Request Timeout" : "400 Bad Request",
                          "text/plain", ready == 0 ? "Request Timeout" : "Bad Request");
            return;
        }
        char c;
        ssize_t nr = read(cfd, &c, 1);
        if (nr <= 0) return;
        header_bytes++;
        if (c == '\n') {
            if (header_line_len && header_line[header_line_len - 1] == '\r') header_line_len--;
            header_line[header_line_len] = '\0';
            if (header_line_len == 0) {
                headers_complete = true;
                break;
            }
            static const char pin_name[] = "X-Compas-PIN:";
            if (header_line_len >= sizeof(pin_name) - 1 &&
                strncasecmp(header_line, pin_name, sizeof(pin_name) - 1) == 0) {
                const char * value = header_line + sizeof(pin_name) - 1;
                while (*value == ' ' || *value == '\t') value++;
                size_t value_len = strlen(value);
                while (value_len && (value[value_len - 1] == ' ' || value[value_len - 1] == '\t')) value_len--;
                if (pin_header_seen || value_len == 0 || value_len >= sizeof(pin_header)) {
                    pin_header_invalid = true;
                } else {
                    memcpy(pin_header, value, value_len);
                    pin_header[value_len] = '\0';
                    pin_header_seen = true;
                }
            }
            static const char length_name[] = "Content-Length:";
            if (header_line_len >= sizeof(length_name) - 1 &&
                strncasecmp(header_line, length_name, sizeof(length_name) - 1) == 0) {
                const char * value = header_line + sizeof(length_name) - 1;
                while (*value == ' ' || *value == '\t') value++;
                char * end = NULL;
                errno = 0;
                long long parsed = (*value >= '0' && *value <= '9') ? strtoll(value, &end, 10) : -1;
                while (end && (*end == ' ' || *end == '\t')) end++;
                if (parsed < 0 || errno == ERANGE || !end || *end != '\0' || content_length >= 0)
                    content_length_invalid = true;
                else
                    content_length = parsed;
            }
            header_line_len = 0;
        } else if (header_line_len + 1 < sizeof(header_line)) {
            header_line[header_line_len++] = c;
        } else {
            send_response(cfd, "431 Request Header Fields Too Large", "text/plain", "Request headers too large");
            return;
        }
    }
    if (!headers_complete) {
        send_response(cfd, "431 Request Header Fields Too Large", "text/plain", "Request headers too large");
        return;
    }

    char method[8] = {0};
    char path[REQUEST_LINE_MAX] = {0};
    if (sscanf(buf, "%7s %4095s", method, path) != 2) {
        send_response(cfd, "400 Bad Request", "text/plain", "Bad Request");
        return;
    }

    /* Path only, no query string, for the exact-match routes below --
     * every GET route is a fixed path, only the POST /api/playback routes
     * ever have a "?...". */
    char path_only[REQUEST_LINE_MAX];
    snprintf(path_only, sizeof(path_only), "%s", path);
    char * q = strchr(path_only, '?');
    if (q) *q = '\0';

    /* The v1 namespace is an alias layer over the original routes. Keep
     * path_only bounded and require the slash after the version so that
     * similarly named paths (for example /api/v10) never match. */
    bool api_v1 = strncmp(path_only, "/api/v1/", sizeof("/api/v1/") - 1) == 0;
    if (api_v1) {
        if (strcmp(path_only, "/api/v1/capabilities") == 0 && strcmp(method, "GET") == 0) {
            static const char capabilities[] =
                "{\"version\":1,\"authRequired\":true,\"authHeader\":\"X-Compas-PIN\","
                "\"transports\":[\"wifi\",\"bluetooth_classic_rfcomm\"],\"features\":["
                "\"status\",\"playback_controls\",\"queue\",\"library_browse\","
                "\"playlists\",\"album_art\",\"screenshots\",\"catalog_sync_v1\","
                "\"folder_browse\",\"recently_played\",\"favorites_edit\",\"queue_edit\","
                "\"playlist_edit\",\"playlist_import\"]}";
            send_response(cfd, "200 OK", "application/json", capabilities);
            return;
        }
        /* Remove only "/v1" so "/api/v1/status" routes as "/api/status". */
        memmove(path_only + sizeof("/api") - 1, path_only + sizeof("/api/v1") - 1,
                strlen(path_only + sizeof("/api/v1") - 1) + 1);
    }

    /* Only the shell and capabilities handshake are public. All library,
     * status, artwork, playback, screenshot, playlist, queue, and asset
     * requests require the same PIN header over both Wi-Fi and Bluetooth
     * RFCOMM. */
    bool public_request = strcmp(method, "GET") == 0 &&
                          (strcmp(path_only, "/") == 0 ||
                           strcmp(path, "/api/v1/capabilities") == 0 ||
                           strcmp(path, "/api/capabilities") == 0);
    if (!public_request) {
        remote_control_pin_result_t pin_result = REMOTE_CONTROL_PIN_INVALID;
        if (!pin_header_invalid && pin_header_seen) pin_result = remote_control_pin_check(pin_header);
        if (pin_result == REMOTE_CONTROL_PIN_RATE_LIMITED) {
            send_pin_rate_limited_response(cfd);
            return;
        }
        if (pin_result != REMOTE_CONTROL_PIN_VALID) {
            send_response(cfd, "401 Unauthorized", "application/json",
                          "{\"error\":\"unauthorized\",\"code\":\"pin_required\"}");
            return;
        }
    }

    if (content_length_invalid) {
        send_response(cfd, "400 Bad Request", "text/plain", "Bad Content-Length");
        return;
    }
    if (handle_extension_route(cfd, method, path_only, path, content_length)) return;

    if (strcmp(method, "GET") == 0) {
        if (strcmp(path_only, "/api/status") == 0) {
            char json[STATUS_JSON_CAPACITY];
            build_status_json(json, sizeof(json));
            send_response(cfd, "200 OK", "application/json", json);
        } else if (strcmp(path_only, "/api/catalog") == 0) {
            handle_catalog_page_request(cfd, path, false);
        } else if (strcmp(path_only, "/api/catalog/covers") == 0) {
            handle_catalog_page_request(cfd, path, true);
        } else if (strcmp(path_only, "/api/catalog/art") == 0) {
            handle_catalog_art_request(cfd, path);
        } else if (strcmp(path_only, "/api/library") == 0) {
            int offset = 0, limit = 50;
            char query_raw[128] = {0}, query[128] = {0};
            char artist_raw[128] = {0}, artist_filter[128] = {0};
            char album_artist_raw[128] = {0}, album_artist_filter[128] = {0};
            char album_raw[128] = {0}, album_filter[128] = {0};
            char genre_raw[METADATA_DB_TEXT_MAX * 3] = {0};
            char genre_filter[METADATA_DB_TEXT_MAX] = {0};
            query_param_int(path, "offset", &offset);
            query_param_int(path, "limit", &limit);
            bool query_ok = (!query_param_present(path, "q") || query_param_str(path, "q", query_raw, sizeof(query_raw))) &&
                            (!query_param_present(path, "artist") || query_param_str(path, "artist", artist_raw, sizeof(artist_raw))) &&
                            (!query_param_present(path, "album_artist") || query_param_str(path, "album_artist", album_artist_raw, sizeof(album_artist_raw))) &&
                            (!query_param_present(path, "album") || query_param_str(path, "album", album_raw, sizeof(album_raw)));
            if (!query_ok) {
                send_response(cfd, "400 Bad Request", "application/json",
                              "{\"error\":\"query_parameter_too_long\"}");
                return;
            }
            if (query_param_str(path, "q", query_raw, sizeof(query_raw))) {
                url_decode(query_raw, query, sizeof(query));
            }
            if (query_param_str(path, "artist", artist_raw, sizeof(artist_raw))) {
                url_decode(artist_raw, artist_filter, sizeof(artist_filter));
            }
            if (query_param_str(path, "album_artist", album_artist_raw, sizeof(album_artist_raw))) {
                url_decode(album_artist_raw, album_artist_filter, sizeof(album_artist_filter));
            }
            if (query_param_str(path, "album", album_raw, sizeof(album_raw))) {
                url_decode(album_raw, album_filter, sizeof(album_filter));
            }
            if (query_param_present(path, "genre") &&
                (!query_param_str(path, "genre", genre_raw, sizeof(genre_raw)) ||
                 !url_decode_checked(genre_raw, genre_filter, sizeof(genre_filter)))) {
                send_response(cfd, "400 Bad Request", "application/json",
                              "{\"error\":\"invalid_genre_filter\"}");
                return;
            }
            char * json = malloc(65536);
            if (json) {
                build_library_json(query, artist_filter, album_artist_filter, album_filter, genre_filter, offset, limit, json,
                                    65536);
                send_response(cfd, "200 OK", "application/json", json);
                free(json);
            } else {
                send_response(cfd, "500 Internal Server Error", "text/plain", "Out of memory");
            }
        } else if (strcmp(path_only, "/api/library/artists") == 0) {
            char * json = malloc(65536);
            if (json) {
                build_artists_json(json, 65536);
                send_response(cfd, "200 OK", "application/json", json);
                free(json);
            } else {
                send_response(cfd, "500 Internal Server Error", "text/plain", "Out of memory");
            }
        } else if (strcmp(path_only, "/api/library/album_artists") == 0) {
            char * json = malloc(65536);
            if (json) {
                build_album_artists_json(json, 65536);
                send_response(cfd, "200 OK", "application/json", json);
                free(json);
            } else {
                send_response(cfd, "500 Internal Server Error", "text/plain", "Out of memory");
            }
        } else if (strcmp(path_only, "/api/library/genres") == 0) {
            char * json = malloc(65536);
            if (json) {
                build_genres_json(json, 65536);
                send_response(cfd, "200 OK", "application/json", json);
                free(json);
            } else {
                send_response(cfd, "500 Internal Server Error", "text/plain", "Out of memory");
            }
        } else if (strcmp(path_only, "/api/library/albums") == 0) {
            char artist_raw[128] = {0}, artist_filter[128] = {0};
            char album_artist_raw[128] = {0}, album_artist_filter[128] = {0};
            bool query_ok = (!query_param_present(path, "artist") || query_param_str(path, "artist", artist_raw, sizeof(artist_raw))) &&
                            (!query_param_present(path, "album_artist") || query_param_str(path, "album_artist", album_artist_raw, sizeof(album_artist_raw)));
            if (!query_ok) {
                send_response(cfd, "400 Bad Request", "application/json",
                              "{\"error\":\"query_parameter_too_long\"}");
                return;
            }
            if (query_param_str(path, "artist", artist_raw, sizeof(artist_raw))) {
                url_decode(artist_raw, artist_filter, sizeof(artist_filter));
            }
            if (query_param_str(path, "album_artist", album_artist_raw, sizeof(album_artist_raw))) {
                url_decode(album_artist_raw, album_artist_filter, sizeof(album_artist_filter));
            }
            char * json = malloc(65536);
            if (json) {
                build_albums_json(artist_filter, album_artist_filter, json, 65536);
                send_response(cfd, "200 OK", "application/json", json);
                free(json);
            } else {
                send_response(cfd, "500 Internal Server Error", "text/plain", "Out of memory");
            }
        } else if (strcmp(path_only, "/api/art") == 0) {
            handle_art_request(cfd, path);
        } else if (strcmp(path_only, "/api/playlists") == 0) {
            /* Heap, not stack: this runs on the remote listener / RFCOMM worker
             * thread (default 128 KiB stack), and keeping large JSON buffers off
             * the frame keeps handle_connection well clear of stack exhaustion
             * on the deep metadata_db query paths below. */
            char * json = malloc(8192);
            if (json) {
                build_playlists_json(json, 8192);
                send_response(cfd, "200 OK", "application/json", json);
                free(json);
            } else {
                send_response(cfd, "500 Internal Server Error", "text/plain", "Out of memory");
            }
        } else if (strcmp(path_only, "/api/playlists/songs") == 0) {
            handle_playlist_songs_request(cfd, path);
        } else if (strcmp(path_only, "/api/queue") == 0) {
            int offset = 0, limit = 25;
            if (query_param_int(path, "offset", &offset) && offset < 0) offset = 0;
            if (query_param_int(path, "limit", &limit) && limit < 1) limit = 25;
            if (limit > 25) limit = 25;
            /* Heap, not stack: a 64 KiB frame here on a 128 KiB worker-thread
             * stack left too little headroom for the metadata_db query paths
             * build_queue_json() reaches. */
            char * json = malloc(65536);
            if (json) {
                build_queue_json(json, 65536, offset, limit);
                send_response(cfd, "200 OK", "application/json", json);
                free(json);
            } else {
                send_response(cfd, "500 Internal Server Error", "text/plain", "Out of memory");
            }
        } else if (strcmp(path_only, "/assets/icon") == 0) {
            handle_icon_request(cfd, path);
        } else if (strcmp(path_only, "/") == 0) {
            send_response(cfd, "200 OK", "text/html", REMOTE_CONTROL_APP_HTML);
        } else {
            send_response(cfd, "404 Not Found", "text/plain", "Not Found");
        }
    } else if (strcmp(method, "POST") == 0) {
        /* Playlist mutation (create / add-to) is handled separately below,
         * outside the mutex-guarded flag-setting block -- it does real file
         * I/O (playlist_files_create()/_append()/_contains(), potentially
         * slow SD card writes), which must never happen while holding
         * status_mutex or it would stall gui.c's own per-tick status-push/
         * consume calls on the main thread for the duration of that I/O.
         * Only the small "which song, by index" lookup needs the lock. */
        if (strcmp(path_only, "/api/playlists") == 0 || strcmp(path_only, "/api/playlists/add") == 0) {
            char raw_name[REQUEST_LINE_MAX] = {0};
            char name[REQUEST_LINE_MAX] = {0};
            int64_t index = -1;
            bool have_name = query_param_str(path, "name", raw_name, sizeof(raw_name));
            if (have_name) {
                url_decode(raw_name, name, sizeof(name));
                /* A decoded NUL would make validation see only a prefix. */
                for (const char * p = raw_name; p[0] && p[1] && p[2]; p++) {
                    if (p[0] == '%' && p[1] == '0' && p[2] == '0') {
                        have_name = false;
                        break;
                    }
                }
            }
            bool have_index = query_param_int64(path, "index", &index);

            char song_path[600] = {0};
            bool have_song = false;
            if (have_index) {
                song_row_t row;
                if (metadata_db_get_song_by_id(index, &row)) {
                    snprintf(song_path, sizeof(song_path), "%s", row.path);
                    have_song = true;
                }
            }

            if (have_name && playlist_key_is_builtin(name)) {
                /* Built-ins are library views; a file by that name would be unreachable. */
                send_response(cfd, "403 Forbidden", "text/plain", "Playlist is read-only");
            } else if (!have_name || strlen(name) > PLAYLIST_NAME_MAX_BYTES ||
                !playlist_name_is_safe(name) || !have_song) {
                send_response(cfd, "400 Bad Request", "text/plain", "Bad Request");
            } else if (strcmp(path_only, "/api/playlists") == 0) {
                char created_path[512];
                bool created = playlist_files_create(PLAYLISTS_DIR, name, song_path, created_path, sizeof(created_path));
                if (created) metadata_db_playlist_insert_one(created_path);
                send_response(cfd, created ? "200 OK" : "500 Internal Server Error", "text/plain",
                               created ? "OK" : "Failed to create playlist");
            } else {
                char m3u_path[512];
                int path_len = snprintf(m3u_path, sizeof(m3u_path), "%s/%s.m3u", PLAYLISTS_DIR, name);
                if (path_len < 0 || (size_t) path_len >= sizeof(m3u_path)) {
                    send_response(cfd, "400 Bad Request", "text/plain", "Bad Request");
                } else if (playlist_files_contains(m3u_path, song_path)) {
                    send_response(cfd, "200 OK", "text/plain", "Song already in playlist");
                } else {
                    bool added = playlist_files_append(m3u_path, song_path);
                    send_response(cfd, added ? "200 OK" : "500 Internal Server Error", "text/plain",
                                   added ? "OK" : "Failed to add to playlist");
                }
            }
        } else {
            bool is_catalog_id_action = strcmp(path_only, "/api/playback/play") == 0 ||
                                        strcmp(path_only, "/api/playback/queue") == 0;
            bool catalog_revision_present = false;
            char catalog_revision[METADATA_DB_CATALOG_REVISION_SIZE] = {0};
            if (is_catalog_id_action && query_param_present(path, "catalog_revision")) {
                catalog_revision_present = true;
                uint64_t index = 0;
                if (!catalog_query_revision_named(path, "catalog_revision", true, catalog_revision) ||
                    !catalog_query_number(path, "index", 0, INT32_MAX, &index) || index == 0) {
                    catalog_send_error(cfd, "400 Bad Request", "invalid_catalog_action");
                    return;
                }
                metadata_db_catalog_result_t validation = metadata_db_catalog_validate_song_revision(
                    catalog_revision, (int64_t) index);
                if (validation == METADATA_DB_CATALOG_STALE) {
                    catalog_send_error(cfd, "409 Conflict", "revision_mismatch");
                    return;
                }
                if (validation != METADATA_DB_CATALOG_OK) {
                    catalog_send_error(cfd, "503 Service Unavailable", "temporary_unavailable");
                    return;
                }
            }
            bool ok = true;
            bool conflict = false;
            pthread_mutex_lock(&status_mutex);
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
                    if (errno == 0 && end != mode_raw && *end == '\0' && mode >= 0 && mode <= 3) {
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
                    request_has_play_index = true;
                    request_play_index = index;
                    snprintf(request_play_catalog_revision, sizeof(request_play_catalog_revision), "%s",
                             catalog_revision_present ? catalog_revision : "");

                    char raw[128];
                    request_play_playlist_name[0] = '\0';
                    request_play_artist_filter[0] = '\0';
                    request_play_album_artist_filter[0] = '\0';
                    request_play_album_filter[0] = '\0';
                    if (query_param_str(path, "playlist", raw, sizeof(raw))) {
                        url_decode(raw, request_play_playlist_name, sizeof(request_play_playlist_name));
                    }
                    if (query_param_str(path, "artist", raw, sizeof(raw))) {
                        url_decode(raw, request_play_artist_filter, sizeof(request_play_artist_filter));
                    }
                    if (query_param_str(path, "album_artist", raw, sizeof(raw))) {
                        url_decode(raw, request_play_album_artist_filter, sizeof(request_play_album_artist_filter));
                    }
                    if (query_param_str(path, "album", raw, sizeof(raw))) {
                        url_decode(raw, request_play_album_filter, sizeof(request_play_album_filter));
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
                    if ((uint64_t) revision != queue_snapshot_revision || offset >= queue_song_id_count) {
                        ok = false;
                        conflict = true;
                    } else {
                        request_has_queue_remove = true;
                        request_queue_remove_offset = offset;
                        request_queue_remove_revision = (uint64_t) revision;
                    }
                } else ok = false;
            } else if (strcmp(path_only, "/api/queue/clear") == 0) {
                int64_t revision;
                if (query_param_int64(path, "revision", &revision) && revision >= 0) {
                    if ((uint64_t) revision != queue_snapshot_revision) {
                        ok = false;
                        conflict = true;
                    } else {
                        request_queue_clear = true;
                        request_queue_clear_revision = (uint64_t) revision;
                    }
                } else ok = false;
            } else {
                ok = false;
            }
            pthread_mutex_unlock(&status_mutex);

            if (ok) {
                send_response(cfd, "200 OK", "text/plain", "OK");
            } else if (conflict) {
                send_response(cfd, "409 Conflict", "text/plain", "Queue changed; reload it and retry");
            } else {
                send_response(cfd, "404 Not Found", "text/plain", "Not Found");
            }
        }
    } else {
        send_response(cfd, "405 Method Not Allowed", "text/plain", "Method Not Allowed");
    }

}

/* Handle one HTTP request on an already connected stream socket. The caller
 * retains ownership of fd and is responsible for closing it. */
void remote_control_handle_stream(int fd) {
    if (fd >= 0) handle_connection(fd);
}

/* Timeout for poll() on listen_fd so listener_thread_func periodically checks
 * running and terminates cleanly on stop. */
#define ACCEPT_POLL_TIMEOUT_MS 200

static void * listener_thread_func(void * arg) {
    (void) arg;
    while (atomic_load_explicit(&running, memory_order_acquire)) {
        pthread_mutex_lock(&listener_mutex);
        int fd = listen_fd;
        pthread_mutex_unlock(&listener_mutex);
        if (fd < 0) break;
        struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
        int pr = poll(&pfd, 1, ACCEPT_POLL_TIMEOUT_MS);
        if (pr <= 0) continue; /* Timeout or interrupted -- re-check running */
        if (!atomic_load_explicit(&running, memory_order_acquire)) break;

        int cfd = accept(fd, NULL, NULL);
        if (cfd < 0) continue;
        struct timeval timeout = { .tv_sec = 1, .tv_usec = 0 };
        setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        pthread_mutex_lock(&listener_mutex);
        if (!atomic_load_explicit(&running, memory_order_acquire)) {
            pthread_mutex_unlock(&listener_mutex);
            close(cfd);
            break;
        }
        active_client_fd = cfd;
        pthread_mutex_unlock(&listener_mutex);
        handle_connection(cfd);
        pthread_mutex_lock(&listener_mutex);
        if (active_client_fd == cfd) {
            active_client_fd = -1;
            close(cfd);
        }
        pthread_mutex_unlock(&listener_mutex);
    }
    return NULL;
}

void remote_control_start(void) {
    if (!remote_control_ensure_pin()) return;
    pthread_mutex_lock(&listener_mutex);
    if (atomic_load_explicit(&running, memory_order_acquire)) {
        pthread_mutex_unlock(&listener_mutex);
        return;
    }

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        pthread_mutex_unlock(&listener_mutex);
        return;
    }

    int reuse = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(REMOTE_CONTROL_PORT);

    if (bind(listen_fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        close(listen_fd);
        listen_fd = -1;
        pthread_mutex_unlock(&listener_mutex);
        return;
    }
    if (listen(listen_fd, 4) < 0) {
        close(listen_fd);
        listen_fd = -1;
        pthread_mutex_unlock(&listener_mutex);
        return;
    }

    atomic_store_explicit(&running, true, memory_order_release);
    if (pthread_create(&listener_thread, NULL, listener_thread_func, NULL) != 0) {
        atomic_store_explicit(&running, false, memory_order_release);
        close(listen_fd);
        listen_fd = -1;
    } else {
        /* DNS-SD runs on its own best-effort worker. A missing Wi-Fi link or
         * occupied multicast socket never affects the HTTP listener. */
        remote_control_mdns_start();
    }
    pthread_mutex_unlock(&listener_mutex);
}

void remote_control_stop(void) {
    remote_control_mdns_stop();
    pthread_mutex_lock(&listener_mutex);
    if (!atomic_load_explicit(&running, memory_order_acquire)) {
        pthread_mutex_unlock(&listener_mutex);
        return;
    }
    atomic_store_explicit(&running, false, memory_order_release);
    /* Interrupt poll(), accept(), and a client read before joining. The
     * client handler owns close(); shutdown only wakes a concurrent read. */
    int stopped_listen_fd = listen_fd;
    if (stopped_listen_fd >= 0) shutdown(stopped_listen_fd, SHUT_RDWR);
    if (active_client_fd >= 0) shutdown(active_client_fd, SHUT_RDWR);
    pthread_mutex_unlock(&listener_mutex);

    pthread_join(listener_thread, NULL);
    pthread_mutex_lock(&listener_mutex);
    if (listen_fd == stopped_listen_fd && listen_fd >= 0) {
        close(listen_fd);
        listen_fd = -1;
    }
    pthread_mutex_unlock(&listener_mutex);
}
