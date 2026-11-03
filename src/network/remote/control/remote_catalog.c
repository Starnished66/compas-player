#include "remote_control_internal.h"
#include "../../../library/metadata/parsing/metadata.h"
#include "../../../library/artwork/coordinator/artwork_coordinator.h"
#include "../../../library/tagcache/core/tagcache.h"
#include "../../../library/artwork/albumart/albumart.h"
#include "../../store/catalog/catalog_source_cache.h"
#include "../../../audio/engine/audio.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

#define CATALOG_ART_MAX_BYTES (8u * 1024u * 1024u)
#define CATALOG_ART_STREAM_CHUNK (16u * 1024u)

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
        char row_json[8192]; /* json_builder_init() terminates it */
        json_builder_t row_builder;
        json_builder_init(&row_builder, row_json, sizeof(row_json));
        char title[128] = {0};
        /* json_escape_append() only needs the leading NUL. */
        char title_json[800], artist_json[800], album_json[800];
        char album_artist_json[800], genre_json[800];
        title_json[0] = artist_json[0] = album_json[0] = album_artist_json[0] = genre_json[0] = '\0';
        char split_artists[TAGCACHE_ARTIST_SPLIT_MAX][TAGCACHE_TAG_MAX]; /* tagcache_artist_names() terminates each returned name */
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
            char artist_name_json[800];
            artist_name_json[0] = '\0';
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

static uint64_t catalog_source_fingerprint(const char * path, int64_t mtime, int64_t size) {
    uint64_t hash = catalog_hash_text(FNV_OFFSET_BASIS, path);
    hash = fnv1a_byte(hash, 0); /* separator */
    uint64_t values[2] = { (uint64_t) mtime, (uint64_t) size };
    for (size_t value = 0; value < 2; value++)
        for (unsigned int byte = 0; byte < 8; byte++)
            hash = fnv1a_byte(hash, (uint8_t) (values[value] >> (byte * 8)));
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

void handle_catalog_page_request(int cfd, const char * path, bool covers) {
    int offset, limit;
    if (!query_paging(path, METADATA_DB_CATALOG_PAGE_MAX, &offset, &limit)) {
        catalog_send_error(cfd, "400 Bad Request", "invalid_paging");
        return;
    }
    char revision[METADATA_DB_CATALOG_REVISION_SIZE] = {0};
    if (!catalog_query_revision_named(path, "revision", covers, revision)) {
        catalog_send_error(cfd, "400 Bad Request", "invalid_revision");
        return;
    }
    size_t row_worst_case = covers ? CATALOG_COVER_ROW_WORST_CASE : CATALOG_SONG_ROW_WORST_CASE;
    size_t json_capacity = (size_t) limit * row_worst_case + CATALOG_JSON_ENVELOPE_MAX;
    char * json = malloc(json_capacity);
    if (!json) {
        catalog_send_error(cfd, "503 Service Unavailable", "temporary_unavailable");
        return;
    }
    metadata_db_catalog_page_t page;
    metadata_db_catalog_result_t result;
    void * rows = calloc((size_t) limit, covers ? sizeof(metadata_db_catalog_cover_t)
                                                : sizeof(metadata_db_catalog_song_t));
    if (!rows) {
        free(json);
        catalog_send_error(cfd, "503 Service Unavailable", "temporary_unavailable");
        return;
    }
    const char * page_revision = revision[0] ? revision : NULL;
    result = covers ? metadata_db_catalog_covers_page(page_revision, offset, limit, &page, rows)
                    : metadata_db_catalog_songs_page(page_revision, offset, limit, &page, rows);
    if (result == METADATA_DB_CATALOG_OK) {
        if (page.count == 0 && offset < page.total) {
            result = METADATA_DB_CATALOG_UNAVAILABLE;
        } else if (covers ? catalog_build_covers_json(&page, rows, offset, json, json_capacity)
                          : catalog_build_songs_json(&page, rows, offset, json, json_capacity)) {
            send_response(cfd, "200 OK", "application/json", json);
        } else {
            result = METADATA_DB_CATALOG_UNAVAILABLE;
        }
    }
    free(rows);
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

static void catalog_stream_sidecar(int cfd, int fd, size_t length, const char * content_type) {
    send_header(cfd, "200 OK", content_type, length);
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

/* Re-reads the cover's catalog row after the artwork was read. Returns true
 * when it still names the same file; otherwise sets the error to send. */
static bool catalog_cover_source_unchanged(const char * revision, int64_t representative_id,
                                           const char * source_path,
                                           const char ** out_status, const char ** out_code) {
    metadata_db_catalog_page_t current_page;
    metadata_db_catalog_cover_t current_source;
    metadata_db_catalog_result_t lookup = metadata_db_catalog_cover_source(
        revision, representative_id, &current_page, &current_source);
    if (lookup == METADATA_DB_CATALOG_OK && strcmp(current_source.representative.path, source_path) == 0)
        return true;
    bool unavailable = lookup == METADATA_DB_CATALOG_UNAVAILABLE;
    *out_status = unavailable ? "503 Service Unavailable" : "409 Conflict";
    *out_code = unavailable ? "temporary_unavailable" : "revision_or_cover_stale";
    return false;
}

/* Closes the sidecar and sends the catalog error. */
static void catalog_art_fail_fd(int cfd, int fd, const char * status, const char * code) {
    close(fd);
    catalog_send_error(cfd, status, code);
}

/* Frees the embedded-artwork buffers and sends the catalog error. */
static void catalog_art_fail_meta(int cfd, track_metadata_t * meta, const char * status, const char * code) {
    free(meta->picture_data);
    free(meta->lyrics);
    catalog_send_error(cfd, status, code);
}

/* Maps a failed embedded-artwork read, or an empty or oversized result, to
 * its catalog error; false when the artwork is usable. The order of the
 * checks decides which code a client sees. */
static bool catalog_embedded_art_error(metadata_artwork_result_t artwork, const track_metadata_t * meta,
                                       const char ** out_status, const char ** out_code) {
    if (artwork == METADATA_ARTWORK_TEMPORARY_FAILURE) {
        *out_status = "503 Service Unavailable";
        *out_code = "artwork_unavailable";
    } else if (artwork == METADATA_ARTWORK_TOO_LARGE) {
        *out_status = "413 Content Too Large";
        *out_code = "embedded_artwork_too_large";
    } else if (artwork == METADATA_ARTWORK_INVALID) {
        *out_status = "503 Service Unavailable";
        *out_code = "embedded_artwork_invalid";
    } else if (artwork != METADATA_ARTWORK_FOUND || !meta->picture_data || meta->picture_size == 0) {
        *out_status = "404 Not Found";
        *out_code = "artwork_missing";
    } else if (meta->picture_size > CATALOG_ART_MAX_BYTES) {
        *out_status = "413 Content Too Large";
        *out_code = "artwork_too_large";
    } else {
        return false;
    }
    return true;
}

void handle_catalog_art_request(int cfd, const char * path) {
    char revision[METADATA_DB_CATALOG_REVISION_SIZE] = {0};
    char raw_key[96] = {0}, cover_key[96] = {0};
    uint64_t representative_id_u = 0;
    if (!catalog_query_revision_named(path, "revision", true, revision) ||
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
            catalog_art_fail_fd(cfd, fd, "503 Service Unavailable", "source_unavailable");
            return;
        }
        uint64_t actual_fingerprint = catalog_source_fingerprint(resolution.sidecar_path, (int64_t) st.st_mtime,
                                                                 (int64_t) st.st_size);
        const char * error_status = NULL, * error_code = NULL;
        if (actual_fingerprint != requested_fingerprint) {
            catalog_art_fail_fd(cfd, fd, "409 Conflict", "revision_or_cover_stale");
            return;
        }
        if ((uint64_t) st.st_size > CATALOG_ART_MAX_BYTES) {
            catalog_art_fail_fd(cfd, fd, "413 Content Too Large", "artwork_too_large");
            return;
        }
        if (st.st_size == 0) {
            catalog_art_fail_fd(cfd, fd, "404 Not Found", "artwork_missing");
            return;
        }
        if (!catalog_cover_source_unchanged(revision, representative_id, source.representative.path,
                                            &error_status, &error_code)) {
            catalog_art_fail_fd(cfd, fd, error_status, error_code);
            return;
        }
        uint8_t magic[16] = {0};
        ssize_t magic_size = pread(fd, magic, sizeof(magic), 0);
        if (magic_size < 0) magic_size = 0;
        const char * content_type = sniff_image_content_type(magic, (uint32_t) magic_size);
        if (lseek(fd, 0, SEEK_SET) < 0) {
            catalog_art_fail_fd(cfd, fd, "503 Service Unavailable", "source_unavailable");
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
    const char * error_status = NULL, * error_code = NULL;
    if (catalog_embedded_art_error(artwork, &meta, &error_status, &error_code)) {
        catalog_art_fail_meta(cfd, &meta, error_status, error_code);
        return;
    }
    struct stat after;
    if (stat(source.representative.path, &after) != 0 || !S_ISREG(after.st_mode) ||
        after.st_mtime != before.st_mtime || after.st_size != before.st_size) {
        catalog_art_fail_meta(cfd, &meta, "409 Conflict", "revision_or_cover_stale");
        return;
    }
    if (!catalog_cover_source_unchanged(revision, representative_id, source.representative.path,
                                        &error_status, &error_code)) {
        catalog_art_fail_meta(cfd, &meta, error_status, error_code);
        return;
    }
    send_response_binary(cfd, "200 OK", sniff_image_content_type(meta.picture_data, meta.picture_size),
                         meta.picture_data, meta.picture_size);
    free(meta.picture_data);
    free(meta.lyrics);
}
