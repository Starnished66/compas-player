#include "subsonic_client.h"
#include "../http/client/http_client.h"

#include "mbedtls/md5.h"
#include "cJSON.h"
#include "../../ui/text/rendering/fallback_font.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <math.h>
#include <pthread.h>
#include "../../library/remote/subsonic/subsonic_cache.h"
#include "../../library/text/lyrics/lyrics.h"

#define SUBSONIC_API_VERSION "1.16.1"
#define SUBSONIC_CLIENT_NAME "Compas"
#define SUBSONIC_CONNECT_TIMEOUT_MS 10000U
#define SUBSONIC_READ_TIMEOUT_MS 15000U
#define SUBSONIC_TOTAL_TIMEOUT_MS 30000U
#define SUBSONIC_MAX_API_RESPONSE (8U * 1024U * 1024U)
#define SUBSONIC_ALBUM_PAGE_SIZE 500
#define SUBSONIC_ALBUM_LIST_MAX 20000
#define SUBSONIC_PACK_TEXT_MAX 255
#define SUBSONIC_RANDOM_SONGS_DEFAULT 100
#define SUBSONIC_RANDOM_SONGS_MAX 500
/* Year 2100 in ms: asks getIndexes for "anything changed since", which servers
 * that support it answer with just the lastModified timestamp. */
#define SUBSONIC_FAR_FUTURE_MS "4102444800000"

static _Thread_local subsonic_error_t api_error;
static _Thread_local int api_error_code; /* the server's error code, 0 when none */

int subsonic_last_error_code(void) { return api_error_code; }

subsonic_error_t subsonic_last_error(void) { return api_error; }

static void api_error_set(subsonic_error_kind_t kind, int number, const char * detail) {
    api_error.kind = kind;
    api_error.number = number;
    snprintf(api_error.detail, sizeof(api_error.detail), "%s", detail ? detail : "");
}

/* False when out is too small for the whole encoding (out is then cut). */
static bool url_encode(const char * in, char * out, size_t out_size) {
    static const char hex[] = "0123456789ABCDEF";
    size_t pos = 0;
    for (const unsigned char * p = (const unsigned char *) in; *p; p++) {
        bool plain = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
                     *p == '-' || *p == '_' || *p == '.' || *p == '~';
        if (pos + (plain ? 1 : 3) + 1 > out_size) {
            out[pos] = '\0';
            return false;
        }
        if (plain) {
            out[pos++] = (char) *p;
        } else {
            out[pos++] = '%';
            out[pos++] = hex[*p >> 4];
            out[pos++] = hex[*p & 0xF];
        }
    }
    out[pos] = '\0';
    return true;
}

#define URL_ID_MAX 255                 /* the longest id the URL builders take */
#define URL_ID_ENC_MAX (3 * URL_ID_MAX + 1) /* every byte percent-encoded, plus the NUL */

/* Encodes an id for a URL. A too-long id empties out_url so no request goes out with a cut id. */
static bool encode_id_or_clear(const char * id, char * enc, char * out_url, size_t out_url_size) {
    if (strlen(id) <= URL_ID_MAX && url_encode(id, enc, URL_ID_ENC_MAX)) return true;
    if (out_url_size) out_url[0] = '\0';
    return false;
}

static void md5_hex(const char * data, size_t len, char out_hex[33]) {
    unsigned char digest[16];
    mbedtls_md5((const unsigned char *) data, len, digest);
    for (int i = 0; i < 16; i++) snprintf(out_hex + i * 2, 3, "%02x", digest[i]);
}

/* Generates random alphanumeric salt for Subsonic token generation. */
static void random_salt(char * out, size_t out_size) {
    static bool seeded = false;
    if (!seeded) { srand((unsigned int) time(NULL)); seeded = true; }

    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    size_t n = out_size - 1;
    for (size_t i = 0; i < n; i++) out[i] = alphabet[rand() % (sizeof(alphabet) - 1)];
    out[n] = '\0';
}

/* Builds query parameters for token-based authentication (u, t, s, v, c, f). */
static void build_auth_query(const subsonic_server_t * server, char * out, size_t out_size) {
    char salt[17];
    random_salt(salt, sizeof(salt));

    char token_input[128 + sizeof(salt)];
    snprintf(token_input, sizeof(token_input), "%s%s", server->password, salt);
    char token[33];
    md5_hex(token_input, strlen(token_input), token);

    char user_enc[384];
    url_encode(server->username, user_enc, sizeof(user_enc));

    snprintf(out, out_size, "u=%s&t=%s&s=%s&v=%s&c=%s&f=json",
              user_enc, token, salt, SUBSONIC_API_VERSION, SUBSONIC_CLIENT_NAME);
}

void subsonic_normalize_base_url(const char * input, char * out, size_t out_size) {
    while (*input && isspace((unsigned char)*input)) input++;
    size_t len = strlen(input);
    while (len && isspace((unsigned char)input[len - 1])) len--;
    while (len && input[len - 1] == '/') len--;
    if (len >= 5 && memcmp(input + len - 5, "/rest", 5) == 0) {
        len -= 5;
        while (len && input[len - 1] == '/') len--;
    }
    if (len >= out_size) len = out_size - 1;
    memcpy(out, input, len);
    out[len] = '\0';
}

static void api_error_clear(void) {
    memset(&api_error, 0, sizeof(api_error));
    api_error_code = 0;
}

static bool api_build_request(const subsonic_server_t * server, const char * endpoint,
                              const char * extra_params, http_request_t * request) {
    char auth[512];
    build_auth_query(server, auth, sizeof(auth));

    char base_url[sizeof(server->base_url)];
    subsonic_normalize_base_url(server->base_url, base_url, sizeof(base_url));
    int url_len = extra_params ? snprintf(request->url, sizeof(request->url), "%s/rest/%s?%s&%s", base_url, endpoint, auth, extra_params)
                               : snprintf(request->url, sizeof(request->url), "%s/rest/%s?%s", base_url, endpoint, auth);
    if (url_len < 0 || (size_t) url_len >= sizeof(request->url)) { /* never send a clipped URL */
        api_error_set(SUBSONIC_ERROR_REQUEST_TOO_LONG, 0, NULL);
        return false;
    }
    request->method = HTTP_METHOD_GET;
    request->verify_tls = server->verify_tls;
    request->connect_timeout_ms = SUBSONIC_CONNECT_TIMEOUT_MS;
    request->read_timeout_ms = SUBSONIC_READ_TIMEOUT_MS;
    request->total_timeout_ms = SUBSONIC_TOTAL_TIMEOUT_MS;
    request->max_response_bytes = SUBSONIC_MAX_API_RESPONSE;
    request->redirect_limit = 5;

    return true;
}

static cJSON * api_fetch_json(const http_request_t * request, http_cancel_token_t * cancel) {
    http_response_t http_response;
    bool ok = http_request_ex(request, cancel, &http_response);
    if (!ok || http_response.status != 200 || !http_response.body) {
        if (!ok)
            api_error_set(SUBSONIC_ERROR_REQUEST_FAILED, 0, http_response.error);
        else if (http_response.status != 200)
            api_error_set(SUBSONIC_ERROR_HTTP_STATUS, http_response.status, NULL);
        else
            api_error_set(SUBSONIC_ERROR_EMPTY_RESPONSE, 0, NULL);
        http_response_free(&http_response);
        return NULL;
    }

    cJSON * root = cJSON_ParseWithLength((const char *) http_response.body, http_response.body_len);
    http_response_free(&http_response);
    if (!root) {
        api_error_set(SUBSONIC_ERROR_INVALID_JSON, 0, NULL);
        return NULL;
    }

    return root;
}

/* Deletes root when its response is invalid; the caller owns it on success. */
static cJSON * api_response_ok(cJSON * root) {
    cJSON * resp = cJSON_GetObjectItemCaseSensitive(root, "subsonic-response");
    if (!resp) {
        api_error_set(SUBSONIC_ERROR_NOT_SUBSONIC_JSON, 0, NULL);
        cJSON_Delete(root);
        return NULL;
    }

    cJSON * status_item = cJSON_GetObjectItemCaseSensitive(resp, "status");
    if (!cJSON_IsString(status_item) || strcmp(status_item->valuestring, "ok") != 0) {
        cJSON * error = cJSON_GetObjectItemCaseSensitive(resp, "error");
        cJSON * code = cJSON_GetObjectItemCaseSensitive(error, "code");
        if (cJSON_IsNumber(code)) {
            api_error_code = code->valueint;
            api_error_set(SUBSONIC_ERROR_SERVER, code->valueint, NULL);
        } else
            api_error_set(SUBSONIC_ERROR_INVALID_STATUS, 0, NULL);
        cJSON_Delete(root);
        return NULL;
    }

    return resp;
}

/* Caller owns *out_root upon success. */
static cJSON * api_request_limited(const subsonic_server_t * server, const char * endpoint, const char * extra_params,
                           cJSON ** out_root, http_cancel_token_t * cancel, size_t limit) {
    api_error_clear();
    http_request_t request = {0};
    if (!api_build_request(server, endpoint, extra_params, &request)) return NULL;
    request.max_response_bytes = limit;
    cJSON * root = api_fetch_json(&request, cancel);
    if (!root) return NULL;
    cJSON * resp = api_response_ok(root);
    if (resp) *out_root = root;
    return resp;
}

static cJSON * api_request(const subsonic_server_t * server, const char * endpoint, const char * extra_params,
                           cJSON ** out_root, http_cancel_token_t * cancel) {
    return api_request_limited(server, endpoint, extra_params, out_root, cancel, SUBSONIC_MAX_API_RESPONSE);
}

/* Preserve the original 256-byte encoded-id limit, but never send a cut id. */
static bool id_params(const char * id, char * out, size_t size) {
    char id_enc[256];
    api_error_clear();
    if (url_encode(id, id_enc, sizeof(id_enc))) {
        int n = snprintf(out, size, "id=%s", id_enc);
        if (n >= 0 && (size_t) n < size) return true;
    }
    if (size) out[0] = '\0';
    api_error_set(SUBSONIC_ERROR_REQUEST_TOO_LONG, 0, NULL);
    return false;
}

static const char * json_str(cJSON * obj, const char * key, const char * fallback) {
    cJSON * item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(item) ? item->valuestring : fallback;
}

static const char * json_nonempty_str(cJSON * obj, const char * key, const char * fallback) {
    const char * value = json_str(obj, key, fallback);
    return value[0] ? value : fallback;
}

static int json_int(cJSON * obj, const char * key, int fallback) {
    cJSON * item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

static unsigned int json_positive_uint(cJSON * obj, const char * key) {
    int value = json_int(obj, key, 0);
    return value > 0 ? (unsigned int) value : 0;
}

struct subsonic_pack {
    size_t row_size;
    int text_fields;
    char * rows;
    int count;
    int cap;
    char * pool; /* text of every row; offset 0 is the empty string */
    size_t pool_len;
    size_t pool_cap;
};

subsonic_pack_t * subsonic_pack_new(size_t row_size, int text_fields) {
    subsonic_pack_t * pack = calloc(1, sizeof(*pack));
    if (!pack) return NULL;
    pack->row_size = row_size;
    pack->text_fields = text_fields;
    pack->pool = malloc(4096);
    if (!pack->pool) { free(pack); return NULL; }
    pack->pool_cap = 4096;
    pack->pool[pack->pool_len++] = '\0';
    return pack;
}

void subsonic_pack_discard(subsonic_pack_t * pack) {
    if (!pack) return;
    free(pack->rows);
    free(pack->pool);
    free(pack);
}

static bool pack_reserve(char ** buf, size_t elem, size_t * cap, size_t need) {
    if (need <= *cap) return true;
    size_t grown = *cap ? *cap : 16;
    while (grown < need) grown *= 2;
    char * bigger = realloc(*buf, grown * elem);
    if (!bigger) return false;
    *buf = bigger;
    *cap = grown;
    return true;
}

/* Copies s (cut at a character boundary past the text limit) into the pool and
 * returns its offset, or (size_t) -1 when memory ran out. */
static size_t pack_put(subsonic_pack_t * pack, const char * s) {
    size_t n = s ? strlen(s) : 0;
    if (n > SUBSONIC_PACK_TEXT_MAX) {
        n = SUBSONIC_PACK_TEXT_MAX;
        while (n > 0 && ((unsigned char) s[n] & 0xC0) == 0x80) n--;
    }
    if (n == 0) return 0;
    if (!pack_reserve(&pack->pool, 1, &pack->pool_cap, pack->pool_len + n + 1)) return (size_t) -1;
    size_t at = pack->pool_len;
    memcpy(pack->pool + at, s, n);
    pack->pool[at + n] = '\0';
    pack->pool_len += n + 1;
    return at;
}

/* Until finish, a row's text slots hold pool offsets. */
static const char ** pack_slots(const subsonic_pack_t * pack, int row) {
    return (const char **) (pack->rows + (size_t) row * pack->row_size);
}

bool subsonic_pack_add(subsonic_pack_t * pack, const void * row) {
    size_t cap = (size_t) pack->cap;
    if (!pack_reserve(&pack->rows, pack->row_size, &cap, (size_t) pack->count + 1)) return false;
    pack->cap = (int) cap;
    memcpy(pack->rows + (size_t) pack->count * pack->row_size, row, pack->row_size);
    const char ** slots = pack_slots(pack, pack->count);
    for (int i = 0; i < pack->text_fields; i++) {
        size_t at = pack_put(pack, ((const char * const *) row)[i]);
        if (at == (size_t) -1) return false;
        slots[i] = (const char *) (uintptr_t) at;
    }
    pack->count++;
    return true;
}

static int subsonic_pack_count(const subsonic_pack_t * pack) { return pack->count; }

static const char * subsonic_pack_text(const subsonic_pack_t * pack, int row, int field) {
    return pack->pool + (uintptr_t) pack_slots(pack, row)[field];
}

bool subsonic_pack_finish(subsonic_pack_t * pack, void ** out, int * out_count) {
    *out = NULL;
    *out_count = 0;
    bool ok = true;
    if (pack->count > 0) {
        size_t rows_bytes = (size_t) pack->count * pack->row_size;
        char * block = malloc(rows_bytes + pack->pool_len);
        ok = block != NULL;
        if (ok) {
            char * text = block + rows_bytes;
            memcpy(block, pack->rows, rows_bytes);
            memcpy(text, pack->pool, pack->pool_len);
            for (int i = 0; i < pack->count; i++) {
                const char ** slots = (const char **) (block + (size_t) i * pack->row_size);
                for (int j = 0; j < pack->text_fields; j++) slots[j] = text + (uintptr_t) slots[j];
            }
            *out = block;
            *out_count = pack->count;
        }
    }
    subsonic_pack_discard(pack);
    return ok;
}

/* Finishes a list: *out stays NULL for an empty one. */
static bool pack_finish_list(subsonic_pack_t * pack, void * out, int * out_count) {
    void * block;
    bool ok = subsonic_pack_finish(pack, &block, out_count);
    *(void **) out = block;
    return ok;
}

/* A cover id that would be cut by the pack is treated as no art. */
static const char * json_cover(cJSON * obj) {
    const char * id = json_str(obj, "coverArt", "");
    return strlen(id) <= SUBSONIC_PACK_TEXT_MAX ? id : "";
}

static bool pack_artist(subsonic_pack_t * pack, cJSON * item) {
    subsonic_artist_t artist = { json_str(item, "id", ""), json_str(item, "name", "(unknown)"),
                                 json_cover(item) };
    return subsonic_pack_add(pack, &artist);
}

static bool pack_album(subsonic_pack_t * pack, cJSON * item) {
    subsonic_album_t album = {
        .id = json_str(item, "id", ""),
        .name = json_str(item, "name", "(unknown)"),
        .artist = json_str(item, "artist", ""),
        .artist_id = json_str(item, "artistId", ""),
        .cover_art = json_cover(item),
        .genre = json_str(item, "genre", ""),
        .year = json_int(item, "year", 0),
        .song_count = json_int(item, "songCount", 0),
        .duration_seconds = json_int(item, "duration", 0),
    };
    return subsonic_pack_add(pack, &album);
}

/* Packs every item of a JSON array (NULL = none) with one row packer. */
static bool pack_list(cJSON * array, size_t row_size, int text_fields, bool (*pack_row)(subsonic_pack_t *, cJSON *),
                      void * out, int * out_count) {
    subsonic_pack_t * pack = subsonic_pack_new(row_size, text_fields);
    if (!pack) return false;
    cJSON * item;
    cJSON_ArrayForEach(item, array) {
        if (!pack_row(pack, item)) { subsonic_pack_discard(pack); return false; }
    }
    return pack_finish_list(pack, out, out_count);
}

/* A genre name that does not fit is skipped: a cut name would browse another genre. */
static bool pack_genre(subsonic_pack_t * pack, cJSON * item) {
    if (strlen(json_str(item, "value", "")) > SUBSONIC_PACK_TEXT_MAX) return true;
    subsonic_genre_t genre = { json_str(item, "value", ""), json_int(item, "albumCount", 0), json_int(item, "songCount", 0) };
    return subsonic_pack_add(pack, &genre);
}

static bool pack_albums(cJSON * array, subsonic_album_t ** out, int * out_count) {
    return pack_list(array, sizeof(subsonic_album_t), SUBSONIC_ALBUM_TEXT_FIELDS, pack_album, out, out_count);
}

static void parse_song(cJSON * item, const char * container_album_artist, subsonic_song_t * song) {
    snprintf(song->id, sizeof(song->id), "%s", json_str(item, "id", ""));
    const char * artist_id = json_str(item, "artistId", "");
    const char * album_id = json_str(item, "albumId", "");
    snprintf(song->artist_id, sizeof(song->artist_id), "%s", strlen(artist_id) < sizeof(song->artist_id) ? artist_id : "");
    snprintf(song->album_id, sizeof(song->album_id), "%s", strlen(album_id) < sizeof(song->album_id) ? album_id : "");
    utf8_truncate_safe(song->title, json_str(item, "title", "(unknown)"), sizeof(song->title));
    utf8_sanitize(song->title);
    utf8_truncate_safe(song->artist, json_str(item, "artist", ""), sizeof(song->artist));
    utf8_sanitize(song->artist);
    utf8_truncate_safe(song->album_artist,
                       json_nonempty_str(item, "albumArtist",
                                         container_album_artist && container_album_artist[0] ? container_album_artist : song->artist),
                       sizeof(song->album_artist));
    utf8_sanitize(song->album_artist);
    utf8_truncate_safe(song->album, json_str(item, "album", ""), sizeof(song->album));
    utf8_sanitize(song->album);
    snprintf(song->suffix, sizeof(song->suffix), "%s", json_str(item, "suffix", "mp3"));
    /* An id that does not fit is treated as no art, never cut short. */
    const char * art = json_str(item, "coverArt", "");
    snprintf(song->cover_art, sizeof(song->cover_art), "%s", strlen(art) < sizeof(song->cover_art) ? art : "");
    song->track = json_int(item, "track", 0);
    song->disc = json_int(item, "discNumber", 0);
    song->duration_seconds = json_int(item, "duration", 0);
    song->sample_rate = json_positive_uint(item, "samplingRate");
    song->bit_depth = json_positive_uint(item, "bitDepth");
    song->channels = json_positive_uint(item, "channelCount");
    song->bitrate_kbps = json_positive_uint(item, "bitRate");
}

bool subsonic_get_song(const subsonic_server_t * server, const char * song_id,
                       subsonic_song_t * song, http_cancel_token_t * cancel) {
    char params[272];
    if (!song || !id_params(song_id, params, sizeof(params))) return false;
    cJSON * root = NULL;
    cJSON * resp = api_request(server, "getSong.view", params, &root, cancel);
    cJSON * item = cJSON_GetObjectItemCaseSensitive(resp, "song");
    bool ok = cJSON_IsObject(item);
    if (ok) { memset(song, 0, sizeof(*song)); parse_song(item, NULL, song); }
    cJSON_Delete(root);
    return ok;
}

/* A member that may be missing but, when present, must be an array. */
static bool optional_array(cJSON * obj, const char * key, cJSON ** array) {
    *array = cJSON_GetObjectItemCaseSensitive(obj, key);
    return !*array || cJSON_IsArray(*array);
}

/* Parses a JSON array of songs (NULL = none) into a malloc'd list. */
static bool songs_from_array(cJSON * array, const char * container_album_artist, subsonic_song_t ** out, int * out_count) {
    int count = cJSON_IsArray(array) ? cJSON_GetArraySize(array) : 0;
    subsonic_song_t * songs = malloc(sizeof(subsonic_song_t) * (size_t) (count > 0 ? count : 1));
    if (!songs) return false;
    int i = 0;
    cJSON * item;
    cJSON_ArrayForEach(item, array) parse_song(item, container_album_artist, &songs[i++]);
    *out = songs;
    *out_count = count;
    return true;
}

bool subsonic_ping(const subsonic_server_t * server, http_cancel_token_t * cancel) {
    cJSON * root = NULL;
    cJSON * resp = api_request(server, "ping.view", NULL, &root, cancel);
    bool ok = (resp != NULL);
    if (root) cJSON_Delete(root);
    return ok;
}

bool subsonic_get_artists(const subsonic_server_t * server, subsonic_artist_t ** out_artists, int * out_count,
                           http_cancel_token_t * cancel) {
    *out_artists = NULL;
    *out_count = 0;

    cJSON * root = NULL;
    cJSON * resp = api_request(server, "getArtists.view", NULL, &root, cancel);
    if (!resp) return false;

    cJSON * artists_obj = cJSON_GetObjectItemCaseSensitive(resp, "artists");
    cJSON * index_arr = cJSON_GetObjectItemCaseSensitive(artists_obj, "index");
    if (!cJSON_IsArray(index_arr)) { cJSON_Delete(root); return false; }

    subsonic_pack_t * pack = subsonic_pack_new(sizeof(subsonic_artist_t), SUBSONIC_ARTIST_TEXT_FIELDS);
    if (!pack) { cJSON_Delete(root); return false; }

    cJSON * index_item;
    cJSON_ArrayForEach(index_item, index_arr) {
        cJSON * artist_arr = cJSON_GetObjectItemCaseSensitive(index_item, "artist");
        if (!cJSON_IsArray(artist_arr)) continue;

        cJSON * artist_item;
        cJSON_ArrayForEach(artist_item, artist_arr) {
            if (!pack_artist(pack, artist_item)) { subsonic_pack_discard(pack); cJSON_Delete(root); return false; }
        }
    }

    cJSON_Delete(root);
    /* A valid empty index is a successful response, not an authentication
     * failure. Navidrome can legitimately return no artists for a new user
     * or library while ping.view has already authenticated successfully. */
    return pack_finish_list(pack, out_artists, out_count);
}

bool subsonic_get_artist_albums(const subsonic_server_t * server, const char * artist_id,
                                 subsonic_album_t ** out_albums, int * out_count, http_cancel_token_t * cancel) {
    *out_albums = NULL;
    *out_count = 0;

    char params[300];
    if (!id_params(artist_id, params, sizeof(params))) return false;

    cJSON * root = NULL;
    cJSON * resp = api_request(server, "getArtist.view", params, &root, cancel);
    if (!resp) return false;

    cJSON * artist_obj = cJSON_GetObjectItemCaseSensitive(resp, "artist");
    cJSON * album_arr = cJSON_GetObjectItemCaseSensitive(artist_obj, "album");
    if (!cJSON_IsObject(artist_obj) || (album_arr && !cJSON_IsArray(album_arr))) { cJSON_Delete(root); return false; }

    bool ok = pack_albums(album_arr, out_albums, out_count);
    cJSON_Delete(root);
    return ok;
}

bool subsonic_get_album_songs(const subsonic_server_t * server, const char * album_id,
                               subsonic_song_t ** out_songs, int * out_count, http_cancel_token_t * cancel) {
    *out_songs = NULL;
    *out_count = 0;

    char params[300];
    if (!id_params(album_id, params, sizeof(params))) return false;

    cJSON * root = NULL;
    cJSON * resp = api_request(server, "getAlbum.view", params, &root, cancel);
    if (!resp) return false;

    cJSON * album_obj = cJSON_GetObjectItemCaseSensitive(resp, "album");
    const char * album_artist = json_nonempty_str(album_obj, "albumArtist", json_str(album_obj, "artist", ""));
    cJSON * song_arr = cJSON_GetObjectItemCaseSensitive(album_obj, "song");
    if (!cJSON_IsObject(album_obj) || (song_arr && !cJSON_IsArray(song_arr))) { cJSON_Delete(root); return false; }

    bool ok = songs_from_array(song_arr, album_artist, out_songs, out_count);
    cJSON_Delete(root);
    return ok;
}

/* Albums gathered while paging. slots is an open-addressing set of indices
 * into pack (-1 = empty) so an id that repeats across pages is kept once. */
typedef struct {
    subsonic_pack_t * pack;
    int * slots;
} album_set_t;

#define ALBUM_SLOTS 32768U /* power of two, comfortably above the cap */

static unsigned int album_slot(const char * id) {
    unsigned int h = 5381;
    for (const unsigned char * p = (const unsigned char *) id; *p; p++) h = h * 33 + *p;
    return h & (ALBUM_SLOTS - 1);
}

/* Appends the album unless its id was already seen: 1 added, 0 repeat, -1 no memory. */
static int album_set_add(album_set_t * set, cJSON * item) {
    const char * id = json_str(item, "id", "");
    unsigned int s = album_slot(id);
    while (set->slots[s] >= 0) {
        if (strcmp(subsonic_pack_text(set->pack, set->slots[s], 0), id) == 0) return 0;
        s = (s + 1) & (ALBUM_SLOTS - 1);
    }
    int index = subsonic_pack_count(set->pack);
    if (!pack_album(set->pack, item)) return -1;
    set->slots[s] = index;
    return 1;
}

/* Requests one page of getAlbumList2. On success *album_arr is the album array
 * (NULL for an empty page) and the caller frees *root. */
static bool request_album_page(const subsonic_server_t * server, const char * type, const char * genre, int offset,
                               cJSON ** root, cJSON ** album_arr, http_cancel_token_t * cancel) {
    char params[1024];
    int len = snprintf(params, sizeof(params), "type=%s&size=%d&offset=%d", type, SUBSONIC_ALBUM_PAGE_SIZE, offset);
    if (genre) {
        char genre_enc[URL_ID_ENC_MAX];
        if (strlen(genre) > URL_ID_MAX || !url_encode(genre, genre_enc, sizeof(genre_enc))) {
            api_error_clear();
            return false;
        }
        snprintf(params + len, sizeof(params) - (size_t) len, "&genre=%s", genre_enc);
    }
    cJSON * resp = api_request(server, "getAlbumList2.view", params, root, cancel);
    if (!resp) return false;

    /* An empty page may omit the album member entirely. */
    cJSON * list_obj = cJSON_GetObjectItemCaseSensitive(resp, "albumList2");
    if (cJSON_IsObject(list_obj) && optional_array(list_obj, "album", album_arr)) return true;
    cJSON_Delete(*root);
    return false;
}

/* Fetches one page of the alphabetical list at the given offset. Returns the
 * page size reported by the server (0 for an empty page) or -1 on error; *added
 * is how many new albums were kept, never past the cap. */
static int fetch_album_page(const subsonic_server_t * server, int offset, album_set_t * set, int * added,
                            http_cancel_token_t * cancel) {
    *added = 0;
    cJSON * root = NULL;
    cJSON * album_arr = NULL;
    if (!request_album_page(server, "alphabeticalByName", NULL, offset, &root, &album_arr, cancel)) return -1;

    int n = cJSON_IsArray(album_arr) ? cJSON_GetArraySize(album_arr) : 0;
    cJSON * item;
    cJSON_ArrayForEach(item, album_arr) {
        if (subsonic_pack_count(set->pack) >= SUBSONIC_ALBUM_LIST_MAX) break;
        int added_one = album_set_add(set, item);
        if (added_one < 0) { cJSON_Delete(root); return -1; }
        *added += added_one;
    }
    cJSON_Delete(root);
    return n;
}

bool subsonic_get_album_list(const subsonic_server_t * server, const char * type, const char * genre,
                             subsonic_album_t ** out_albums, int * out_count, http_cancel_token_t * cancel) {
    *out_albums = NULL;
    *out_count = 0;
    cJSON * root = NULL;
    cJSON * album_arr = NULL;
    if (!request_album_page(server, type, genre, 0, &root, &album_arr, cancel)) return false;
    bool ok = pack_albums(album_arr, out_albums, out_count);
    cJSON_Delete(root);
    return ok;
}

bool subsonic_get_all_albums(const subsonic_server_t * server, subsonic_album_t ** out_albums, int * out_count,
                              http_cancel_token_t * cancel) {
    *out_albums = NULL;
    *out_count = 0;

    album_set_t set = { subsonic_pack_new(sizeof(subsonic_album_t), SUBSONIC_ALBUM_TEXT_FIELDS), malloc(sizeof(int) * ALBUM_SLOTS) };
    if (!set.pack || !set.slots) { subsonic_pack_discard(set.pack); free(set.slots); return false; }
    memset(set.slots, 0xff, sizeof(int) * ALBUM_SLOTS);

    /* Page until a short page, a page with nothing new (the server ignores
     * the offset) or the cap; an empty library is a valid result. */
    int offset = 0;
    while (subsonic_pack_count(set.pack) < SUBSONIC_ALBUM_LIST_MAX) {
        int added = 0;
        int n = http_cancel_token_is_cancelled(cancel) ? -1 : fetch_album_page(server, offset, &set, &added, cancel);
        if (n < 0) { subsonic_pack_discard(set.pack); free(set.slots); return false; }
        offset += n;
        if (n < SUBSONIC_ALBUM_PAGE_SIZE || added == 0) break;
    }
    free(set.slots);
    return pack_finish_list(set.pack, out_albums, out_count);
}

bool subsonic_get_library_stamp(const subsonic_server_t * server, subsonic_library_stamp_t * out,
                                 http_cancel_token_t * cancel) {
    memset(out, 0, sizeof(*out));
    cJSON * root = NULL;
    cJSON * resp = api_request(server, "getIndexes.view", "ifModifiedSince=" SUBSONIC_FAR_FUTURE_MS, &root, cancel);
    bool answered = resp != NULL;
    if (resp) {
        cJSON * modified = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(resp, "indexes"), "lastModified");
        if (cJSON_IsNumber(modified) && modified->valuedouble > 0) out->last_modified = (long long) modified->valuedouble;
        cJSON_Delete(root);
    }
    if (http_cancel_token_is_cancelled(cancel)) return false;

    root = NULL;
    resp = api_request(server, "getScanStatus.view", NULL, &root, cancel); /* OpenSubsonic; older servers may refuse */
    if (resp) {
        answered = true;
        snprintf(out->last_scan, sizeof(out->last_scan), "%s",
                 json_str(cJSON_GetObjectItemCaseSensitive(resp, "scanStatus"), "lastScan", ""));
        cJSON_Delete(root);
    }
    return answered;
}

bool subsonic_stamp_unchanged(const subsonic_library_stamp_t * known, const subsonic_library_stamp_t * now) {
    bool comparable = false;
    if (known->last_modified > 0 && now->last_modified > 0) {
        if (known->last_modified != now->last_modified) return false;
        comparable = true;
    }
    if (known->last_scan[0] && now->last_scan[0]) {
        if (strcmp(known->last_scan, now->last_scan) != 0) return false;
        comparable = true;
    }
    return comparable;
}

void subsonic_playlist_stamp(const subsonic_playlist_t * playlist, subsonic_library_stamp_t * out) {
    memset(out, 0, sizeof(*out));
    out->last_modified = (long long) playlist->song_count + 1;
    snprintf(out->last_scan, sizeof(out->last_scan), "%s", playlist->changed ? playlist->changed : "");
}

bool subsonic_playlist_stamp_unchanged(const subsonic_library_stamp_t * known, const subsonic_library_stamp_t * now) {
    return known->last_modified > 0 && now->last_modified > 0 && known->last_modified == now->last_modified &&
           strcmp(known->last_scan, now->last_scan) == 0;
}

static bool pack_playlist(subsonic_pack_t * pack, cJSON * item) {
    subsonic_playlist_t playlist = {
        .id = json_str(item, "id", ""),
        .name = json_str(item, "name", "(unknown)"),
        .changed = json_str(item, "changed", ""),
        .cover_art = json_str(item, "coverArt", ""),
        .song_count = json_int(item, "songCount", 0),
    };
    return subsonic_pack_add(pack, &playlist);
}

bool subsonic_get_playlists(const subsonic_server_t * server, subsonic_playlist_t ** out_playlists, int * out_count,
                             http_cancel_token_t * cancel) {
    *out_playlists = NULL;
    *out_count = 0;
    cJSON * root = NULL;
    cJSON * resp = api_request(server, "getPlaylists.view", NULL, &root, cancel);
    if (!resp) return false;

    cJSON * list_obj = cJSON_GetObjectItemCaseSensitive(resp, "playlists");
    cJSON * playlist_arr;
    /* Servers may omit the playlist member for a valid empty result. */
    bool ok = cJSON_IsObject(list_obj) && optional_array(list_obj, "playlist", &playlist_arr) &&
              pack_list(playlist_arr, sizeof(subsonic_playlist_t), SUBSONIC_PLAYLIST_TEXT_FIELDS,
                        pack_playlist, out_playlists, out_count);
    cJSON_Delete(root);
    return ok;
}

bool subsonic_get_playlist_songs(const subsonic_server_t * server, const char * playlist_id,
                                  subsonic_song_t ** out_songs, int * out_count, http_cancel_token_t * cancel) {
    *out_songs = NULL;
    *out_count = 0;

    char params[300];
    if (!id_params(playlist_id, params, sizeof(params))) return false;

    cJSON * root = NULL;
    cJSON * resp = api_request(server, "getPlaylist.view", params, &root, cancel);
    if (!resp) return false;

    cJSON * playlist_obj = cJSON_GetObjectItemCaseSensitive(resp, "playlist");
    cJSON * entry_arr = cJSON_GetObjectItemCaseSensitive(playlist_obj, "entry");
    if (!cJSON_IsObject(playlist_obj) || (entry_arr && !cJSON_IsArray(entry_arr))) { cJSON_Delete(root); return false; }

    bool ok = songs_from_array(entry_arr, NULL, out_songs, out_count);
    cJSON_Delete(root);
    return ok;
}

bool subsonic_get_genres(const subsonic_server_t * server, subsonic_genre_t ** out_genres, int * out_count,
                         http_cancel_token_t * cancel) {
    *out_genres = NULL;
    *out_count = 0;
    cJSON * root = NULL;
    cJSON * resp = api_request(server, "getGenres.view", NULL, &root, cancel);
    if (!resp) return false;

    cJSON * list_obj = cJSON_GetObjectItemCaseSensitive(resp, "genres");
    cJSON * genre_arr;
    bool ok = cJSON_IsObject(list_obj) && optional_array(list_obj, "genre", &genre_arr) &&
              pack_list(genre_arr, sizeof(subsonic_genre_t), SUBSONIC_GENRE_TEXT_FIELDS, pack_genre, out_genres, out_count);
    cJSON_Delete(root);
    return ok;
}

bool subsonic_get_random_songs(const subsonic_server_t * server, int size, subsonic_song_t ** out_songs,
                               int * out_count, http_cancel_token_t * cancel) {
    *out_songs = NULL;
    *out_count = 0;
    if (size <= 0) size = SUBSONIC_RANDOM_SONGS_DEFAULT;
    if (size > SUBSONIC_RANDOM_SONGS_MAX) size = SUBSONIC_RANDOM_SONGS_MAX;
    char params[32];
    snprintf(params, sizeof(params), "size=%d", size);

    cJSON * root = NULL;
    cJSON * resp = api_request(server, "getRandomSongs.view", params, &root, cancel);
    if (!resp) return false;

    cJSON * list_obj = cJSON_GetObjectItemCaseSensitive(resp, "randomSongs");
    cJSON * song_arr;
    bool ok = cJSON_IsObject(list_obj) && optional_array(list_obj, "song", &song_arr) &&
              songs_from_array(song_arr, NULL, out_songs, out_count);
    cJSON_Delete(root);
    return ok;
}

bool subsonic_search3(const subsonic_server_t * server, const char * query, int limit,
                      subsonic_artist_t ** out_artists, int * out_artist_count,
                      subsonic_album_t ** out_albums, int * out_album_count,
                      subsonic_song_t ** out_songs, int * out_song_count, http_cancel_token_t * cancel) {
    *out_artists = NULL; *out_artist_count = 0;
    *out_albums = NULL; *out_album_count = 0;
    *out_songs = NULL; *out_song_count = 0;
    if (!query || !query[0] || limit <= 0 || limit > 100) return false;
    char encoded[URL_ID_ENC_MAX], params[URL_ID_ENC_MAX + 128];
    if (strlen(query) > URL_ID_MAX || !url_encode(query, encoded, sizeof(encoded))) return false;
    snprintf(params, sizeof(params), "query=%s&artistCount=%d&artistOffset=0&albumCount=%d&albumOffset=0&songCount=%d&songOffset=0",
             encoded, limit, limit, limit);
    cJSON * root = NULL;
    cJSON * resp = api_request(server, "search3.view", params, &root, cancel);
    if (!resp) return false;
    cJSON * result = cJSON_GetObjectItemCaseSensitive(resp, "searchResult3");
    cJSON * artists, * albums, * songs;
    bool ok = cJSON_IsObject(result) && optional_array(result, "artist", &artists) &&
              optional_array(result, "album", &albums) && optional_array(result, "song", &songs);
    if (ok) ok = pack_list(artists, sizeof(subsonic_artist_t), SUBSONIC_ARTIST_TEXT_FIELDS,
                           pack_artist, (void **) out_artists, out_artist_count) &&
                 pack_albums(albums, out_albums, out_album_count) &&
                 songs_from_array(songs, NULL, out_songs, out_song_count);
    cJSON_Delete(root);
    if (!ok) {
        free(*out_artists); free(*out_albums); free(*out_songs);
        *out_artists = NULL; *out_artist_count = 0;
        *out_albums = NULL; *out_album_count = 0;
        *out_songs = NULL; *out_song_count = 0;
    }
    return ok;
}

bool subsonic_get_starred(const subsonic_server_t * server, subsonic_starred_t * out, http_cancel_token_t * cancel) {
    memset(out, 0, sizeof(*out));
    cJSON * root = NULL;
    cJSON * resp = api_request(server, "getStarred2.view", NULL, &root, cancel);
    if (!resp) return false;

    cJSON * starred = cJSON_GetObjectItemCaseSensitive(resp, "starred2");
    cJSON * artist_arr, * album_arr, * song_arr;
    bool ok = cJSON_IsObject(starred) && optional_array(starred, "artist", &artist_arr) &&
              optional_array(starred, "album", &album_arr) && optional_array(starred, "song", &song_arr) &&
              pack_list(artist_arr, sizeof(subsonic_artist_t), SUBSONIC_ARTIST_TEXT_FIELDS, pack_artist,
                        &out->artists, &out->artist_count) &&
              pack_albums(album_arr, &out->albums, &out->album_count) &&
              songs_from_array(song_arr, NULL, &out->songs, &out->song_count);
    cJSON_Delete(root);
    if (!ok) {
        free(out->artists);
        free(out->albums);
        free(out->songs);
        memset(out, 0, sizeof(*out));
    }
    return ok;
}

/* Keep authentication (and salt generation) before id validation. */
static bool url_prelude(const subsonic_server_t * server, const char * id, char auth[512],
                         char base[sizeof(server->base_url)], char enc[URL_ID_ENC_MAX], char * out_url, size_t out_url_size) {
    build_auth_query(server, auth, 512);
    subsonic_normalize_base_url(server->base_url, base, sizeof(server->base_url));
    return encode_id_or_clear(id, enc, out_url, out_url_size);
}

/* Empties out_url when snprintf cut the URL (n is its return value). */
static void clear_if_cut(int n, char * out_url, size_t out_url_size) {
    if (out_url_size && (n < 0 || (size_t) n >= out_url_size)) out_url[0] = '\0';
}

void subsonic_build_stream_url(const subsonic_server_t * server, const char * song_id, char * out_url, size_t out_url_size) {
    char auth[512], base[sizeof(server->base_url)], id_enc[URL_ID_ENC_MAX];
    if (!url_prelude(server, song_id, auth, base, id_enc, out_url, out_url_size)) return;

    clear_if_cut(snprintf(out_url, out_url_size, "%s/rest/stream.view?%s&id=%s&format=raw", base, auth, id_enc),
                 out_url, out_url_size);
}

void subsonic_build_stream_url_quality(const subsonic_server_t * server, const char * song_id,
                                       int max_bitrate_kbps, char * out_url, size_t out_url_size) {
    if (max_bitrate_kbps <= 0) {
        subsonic_build_stream_url(server, song_id, out_url, out_url_size);
        return;
    }
    char auth[512], base[sizeof(server->base_url)], id_enc[URL_ID_ENC_MAX];
    if (!url_prelude(server, song_id, auth, base, id_enc, out_url, out_url_size)) return;
    clear_if_cut(snprintf(out_url, out_url_size, "%s/rest/stream.view?%s&id=%s&format=mp3&maxBitRate=%d",
                          base, auth, id_enc, max_bitrate_kbps), out_url, out_url_size);
}

void subsonic_build_cover_art_url(const subsonic_server_t * server, const char * cover_art_id, int size_px,
                                  char * out_url, size_t out_url_size) {
    char auth[512], base[sizeof(server->base_url)], id_enc[URL_ID_ENC_MAX];
    if (!url_prelude(server, cover_art_id, auth, base, id_enc, out_url, out_url_size)) return;

    int len = snprintf(out_url, out_url_size, "%s/rest/getCoverArt.view?%s&id=%s", base, auth, id_enc);
    clear_if_cut(len, out_url, out_url_size);
    if (size_px > 0 && out_url[0])
        clear_if_cut(len + snprintf(out_url + len, out_url_size - (size_t) len, "&size=%d", size_px), out_url, out_url_size);
}

static uint32_t lyrics_time_ms(double start, double offset) {
    double shifted = start - offset;
    if (shifted <= 0) return 0;
    if (shifted >= LYRICS_MAX_TIME_MS) return LYRICS_MAX_TIME_MS;
    return (uint32_t)shifted;
}

static char * structured_lyrics_text(cJSON * doc, bool synced) {
    cJSON * lines = cJSON_GetObjectItemCaseSensitive(doc, "line");
    if (!cJSON_IsArray(lines) || cJSON_GetArraySize(lines) > LYRICS_MAX_LINES) return NULL;
    char * text = malloc(LYRICS_MAX_FILE_BYTES + 1);
    if (!text) return NULL;
    size_t used = 0;
    double offset = 0;
    cJSON * bias = cJSON_GetObjectItemCaseSensitive(doc, "offset");
    if (cJSON_IsNumber(bias) && isfinite(bias->valuedouble)) offset = bias->valuedouble;
    cJSON * line;
    cJSON_ArrayForEach(line, lines) {
        const char * value = json_str(line, "value", "");
        size_t len = strlen(value);
        if (len > LYRICS_MAX_LINE_BYTES || used + len + 40 > LYRICS_MAX_FILE_BYTES) { free(text); return NULL; }
        if (synced) {
            cJSON * start = cJSON_GetObjectItemCaseSensitive(line, "start");
            if (!cJSON_IsNumber(start) || !isfinite(start->valuedouble) || start->valuedouble < 0) continue;
            uint32_t ms = lyrics_time_ms(start->valuedouble, offset);
            /* A space prevents bracketed lyric text becoming another LRC tag. */
            used += (size_t)snprintf(text + used, 40, "[%02u:%02u.%03u] ", ms / 60000, ms / 1000 % 60, ms % 1000);
        }
        for (size_t i = 0; i < len; ++i)
            text[used++] = synced && (value[i] == '\n' || value[i] == '\r') ? ' ' : value[i];
        text[used++] = '\n';
    }
    if (!used) { free(text); return NULL; }
    text[used] = '\0';
    char * small = realloc(text, used + 1);
    return small ? small : text;
}

/* One server capability is cached for the current session. */
static char lyrics_capability_key[SUBSONIC_CACHE_KEY_SIZE];
static bool lyrics_capability_supported;
static pthread_mutex_t lyrics_capability_mutex = PTHREAD_MUTEX_INITIALIZER;

static bool server_song_lyrics(const subsonic_server_t * server, http_cancel_token_t * cancel) {
    char key[SUBSONIC_CACHE_KEY_SIZE];
    subsonic_cache_server_key(server, key);
    pthread_mutex_lock(&lyrics_capability_mutex);
    if (lyrics_capability_key[0] && !strcmp(lyrics_capability_key, key)) {
        bool supported = lyrics_capability_supported;
        pthread_mutex_unlock(&lyrics_capability_mutex);
        return supported;
    }
    pthread_mutex_unlock(&lyrics_capability_mutex);
    cJSON * root = NULL;
    cJSON * resp = api_request_limited(server, "getOpenSubsonicExtensions.view", NULL, &root, cancel, 64 * 1024);
    bool song_lyrics = false;
    cJSON * extensions = cJSON_GetObjectItemCaseSensitive(resp, "openSubsonicExtensions");
    if (cJSON_IsArray(extensions)) {
        cJSON * extension;
        cJSON_ArrayForEach(extension, extensions) {
            if (!strcmp(json_str(extension, "name", ""), "songLyrics")) song_lyrics = true;
        }
    }
    bool known = resp != NULL || api_error_code == SUBSONIC_ERROR_NOT_FOUND ||
                 (api_error.kind == SUBSONIC_ERROR_HTTP_STATUS && api_error.number == 404);
    cJSON_Delete(root);
    if (known && !http_cancel_token_is_cancelled(cancel)) {
        pthread_mutex_lock(&lyrics_capability_mutex);
        snprintf(lyrics_capability_key, sizeof(lyrics_capability_key), "%s", key);
        lyrics_capability_supported = song_lyrics;
        pthread_mutex_unlock(&lyrics_capability_mutex);
    }
    return song_lyrics;
}

static char * find_structured_lyrics(cJSON * docs, bool synced) {
    if (!cJSON_IsArray(docs)) return NULL;
    cJSON * doc;
    cJSON_ArrayForEach(doc, docs) {
        bool doc_synced = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(doc, "synced"));
        if (doc_synced != synced) continue;
        char * text = structured_lyrics_text(doc, synced);
        if (text) return text;
    }
    return NULL;
}

static bool request_structured_lyrics(const subsonic_server_t * server, const char * song_id,
                                      cJSON ** root, char ** text, bool * synced,
                                      http_cancel_token_t * cancel) {
    char params[1600];
    if (!id_params(song_id, params, sizeof(params))) return false;
    cJSON * resp = api_request_limited(server, "getLyricsBySongId.view", params, root, cancel,
                                       LYRICS_MAX_FILE_BYTES * 2);
    cJSON * list = cJSON_GetObjectItemCaseSensitive(resp, "lyricsList");
    cJSON * docs = cJSON_GetObjectItemCaseSensitive(list, "structuredLyrics");
    *text = find_structured_lyrics(docs, true);
    if (*text) {
        *synced = true;
    } else {
        *text = find_structured_lyrics(docs, false);
    }
    return resp != NULL;
}

static bool request_classic_lyrics(const subsonic_server_t * server, const char * artist,
                                   const char * title, cJSON ** root, char ** text,
                                   http_cancel_token_t * cancel) {
    char artist_enc[512], title_enc[512], params[1600];
    if (!url_encode(artist ? artist : "", artist_enc, sizeof(artist_enc)) ||
        !url_encode(title ? title : "", title_enc, sizeof(title_enc))) return false;
    snprintf(params, sizeof(params), "artist=%s&title=%s", artist_enc, title_enc);
    cJSON * resp = api_request_limited(server, "getLyrics.view", params, root, cancel,
                                       LYRICS_MAX_FILE_BYTES * 2);
    cJSON * lyrics = cJSON_GetObjectItemCaseSensitive(resp, "lyrics");
    const char * value = json_str(lyrics, "value", "");
    size_t len = strlen(value);
    if (len && len <= LYRICS_MAX_FILE_BYTES) *text = strdup(value);
    return resp != NULL;
}

bool subsonic_get_lyrics(const subsonic_server_t * server, const char * song_id,
                         const char * artist, const char * title, char ** text,
                         bool * synced, http_cancel_token_t * cancel) {
    *text = NULL;
    *synced = false;
    bool song_lyrics = server_song_lyrics(server, cancel);
    if (http_cancel_token_is_cancelled(cancel)) return false;
    cJSON * root = NULL;
    bool ok = song_lyrics ? request_structured_lyrics(server, song_id, &root, text, synced, cancel)
                          : request_classic_lyrics(server, artist, title, &root, text, cancel);
    cJSON_Delete(root);
    if (cancel && http_cancel_token_is_cancelled(cancel)) { free(*text); *text = NULL; return false; }
    return ok;
}

/* Snapshot the selected endpoint policy independently of UI metadata. */
static pthread_mutex_t stream_policy_mutex = PTHREAD_MUTEX_INITIALIZER;
typedef struct stream_policy {
    char prefix[sizeof(((subsonic_server_t *)0)->base_url) + 32];
    struct stream_policy *next;
} stream_policy_t;
static stream_policy_t *stream_tls_exceptions;

void subsonic_stream_policy_set(const char *base_url, bool verify_tls) {
    char base[sizeof(((subsonic_server_t *)0)->base_url)] = "";
    if (base_url && strlen(base_url) < sizeof(base))
        subsonic_normalize_base_url(base_url, base, sizeof(base));
    if (strncmp(base, "https://", 8)) return;
    char prefix[sizeof(base) + 32];
    snprintf(prefix, sizeof(prefix), "%s/rest/stream.view?", base);
    pthread_mutex_lock(&stream_policy_mutex);
    stream_policy_t **link = &stream_tls_exceptions;
    while (*link && strcmp((*link)->prefix, prefix)) link = &(*link)->next;
    if (verify_tls && *link) {
        stream_policy_t *old = *link;
        *link = old->next;
        free(old);
    } else if (!verify_tls && !*link) {
        *link = calloc(1, sizeof(**link));
        if (*link) snprintf((*link)->prefix, sizeof((*link)->prefix), "%s", prefix);
    }
    pthread_mutex_unlock(&stream_policy_mutex);
}

bool subsonic_stream_verify_tls(const char *url) {
    pthread_mutex_lock(&stream_policy_mutex);
    bool verify = true;
    for (stream_policy_t *p = stream_tls_exceptions; url && p; p = p->next)
        if (!strncmp(url, p->prefix, strlen(p->prefix))) { verify = false; break; }
    pthread_mutex_unlock(&stream_policy_mutex);
    return verify;
}
