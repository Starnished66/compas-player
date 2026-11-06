#include "subsonic_cache_internal.h"
#include "../../artwork/support/library_endian.h"
#include "../../metadata/scan/library_scan_storage.h"
#include "../../../core/persistence/storage_paths.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CACHE_SCHEMA "v2"
#define CACHE_META_FILE "meta.tsv"
#define CACHE_LINE_MAX 2048        /* above the widest row the client can produce */
#define CACHE_FIELD_MAX 255        /* the client cuts text at this length */
#define CACHE_ROWS_MAX 50000
#define CACHE_FILE_MAX (4L * 1024 * 1024)
#define CACHE_COLUMNS_MAX 16
#define SONG_ID_SIZE 128           /* ids are cut here in the first line, on save and on load */

/* A songs row is a fixed struct of char arrays and numbers: one column each,
 * in this order (size 0 marks a number). */
typedef struct {
    size_t offset;
    size_t size;
} song_col_t;

#define TEXT_COL(f) { offsetof(subsonic_song_t, f), sizeof(((subsonic_song_t *) 0)->f) }
#define NUM_COL(f) { offsetof(subsonic_song_t, f), 0 }
static const song_col_t song_cols[] = {
    TEXT_COL(id), TEXT_COL(title), TEXT_COL(artist), TEXT_COL(album), TEXT_COL(album_artist), TEXT_COL(suffix),
    TEXT_COL(cover_art), NUM_COL(track), NUM_COL(disc), NUM_COL(duration_seconds), NUM_COL(sample_rate),
    NUM_COL(bit_depth), NUM_COL(channels), NUM_COL(bitrate_kbps),
};
#define SONG_COLS ((int) (sizeof(song_cols) / sizeof(song_cols[0])))
_Static_assert(sizeof(unsigned int) == sizeof(int), "song numbers are stored as int");
_Static_assert(SONG_COLS <= CACHE_COLUMNS_MAX, "song columns fit one line split");

/* A list file has one row per line: the row struct's text members, then its
 * int members, in struct order. A songs kind uses song_cols instead, and
 * name is the folder of its files. */
typedef struct {
    const char * name;
    const char * file;
    size_t row_size;
    int text_fields;
    int int_fields;
    const song_col_t * cols;
} list_desc_t;

static const list_desc_t lists[SUBSONIC_LIST_ALL_KINDS] = {
    { "artists", "artists.tsv", sizeof(subsonic_artist_t), SUBSONIC_ARTIST_TEXT_FIELDS, 0, NULL },
    { "albums", "albums.tsv", sizeof(subsonic_album_t), SUBSONIC_ALBUM_TEXT_FIELDS, 3, NULL },
    { "playlists", "playlists.tsv", sizeof(subsonic_playlist_t), SUBSONIC_PLAYLIST_TEXT_FIELDS, 1, NULL },
    { "albums", NULL, sizeof(subsonic_song_t), 0, 0, song_cols },
    { "playlists", NULL, sizeof(subsonic_song_t), 0, 0, song_cols },
};

_Static_assert(sizeof(subsonic_artist_t) == SUBSONIC_ARTIST_TEXT_FIELDS * sizeof(const char *), "artists are text only");
_Static_assert(offsetof(subsonic_album_t, year) == SUBSONIC_ALBUM_TEXT_FIELDS * sizeof(const char *), "album ints follow the text");
_Static_assert(offsetof(subsonic_playlist_t, song_count) == SUBSONIC_PLAYLIST_TEXT_FIELDS * sizeof(const char *),
               "playlist ints follow the text");

static pthread_mutex_t save_mutex = PTHREAD_MUTEX_INITIALIZER;

uint64_t subsonic_fnv64(uint64_t hash, const void * data, size_t len) {
    for (const unsigned char * p = data; len--; p++) {
        hash ^= *p;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

void subsonic_cache_server_key(const subsonic_server_t * server, char out[SUBSONIC_CACHE_KEY_SIZE]) {
    char url[sizeof(server->base_url)];
    subsonic_normalize_base_url(server->base_url, url, sizeof(url));
    uint64_t hash = subsonic_fnv64(SUBSONIC_FNV64_INIT, url, strlen(url));
    hash = subsonic_fnv64(hash, "\n", 1);
    hash = subsonic_fnv64(hash, server->username, strlen(server->username));
    snprintf(out, SUBSONIC_CACHE_KEY_SIZE, "%016llx", (unsigned long long) hash);
}

bool subsonic_cache_path(const subsonic_server_t * server, const char * file, char * out, size_t size) {
    char key[SUBSONIC_CACHE_KEY_SIZE];
    subsonic_cache_server_key(server, key);
    int n = file ? snprintf(out, size, "%s/%s/%s", storage_sd_path(".compas/subsonic"), key, file)
                 : snprintf(out, size, "%s/%s", storage_sd_path(".compas/subsonic"), key);
    return n > 0 && (size_t) n < size;
}

static bool ensure_server_dir(const subsonic_server_t * server) {
    char dir[512];
    if (!sd_card_root_is_mounted() || !subsonic_cache_path(server, NULL, dir, sizeof(dir))) return false;
    const char * parents[] = { storage_sd_path(".compas"), storage_sd_path(".compas/subsonic"), dir };
    for (size_t i = 0; i < sizeof(parents) / sizeof(parents[0]); i++)
        if (mkdir(parents[i], 0755) != 0 && errno != EEXIST) return false;
    return true;
}

/* Writes through a temp file so a reader (or a power cut) sees the old file or
 * the whole new one, never half of it. Each write has its own temp name, so
 * two threads writing the same file never share one; the last rename wins. */
bool subsonic_cache_write_atomic(const subsonic_server_t * server, const char * file,
                         bool (*fill)(FILE * f, const void * ctx), const void * ctx) {
    static atomic_uint tmp_serial;
    char path[512], tmp[540], dir[512];
    unsigned serial = atomic_fetch_add(&tmp_serial, 1);
    if (!ensure_server_dir(server) || !subsonic_cache_path(server, file, path, sizeof(path)) ||
        snprintf(tmp, sizeof(tmp), "%s.%u.%u.tmp", path, (unsigned) getpid(), serial) >= (int) sizeof(tmp))
        return false;
    snprintf(dir, sizeof(dir), "%s", path); /* the file's folder: the server's, or albums/ and playlists/ */
    *strrchr(dir, '/') = '\0';
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) return false;
    FILE * f = fopen(tmp, "w");
    if (!f) return false;
    bool ok = fill(f, ctx);
    if (fflush(f) != 0) ok = false;
    if (ok && fsync(fileno(f)) != 0) ok = false;
    if (fclose(f) != 0) ok = false;
    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        return false;
    }
    (void) library_fsync_dir(dir);
    return true;
}

/* Tabs and line breaks would break the file's framing: they become spaces. */
static char clean_char(char c) { return (c == '\t' || c == '\n' || c == '\r') ? ' ' : c; }

static void clean_copy(char * dst, size_t size, const char * s) {
    size_t i = 0;
    for (; s && s[i] && i + 1 < size; i++) dst[i] = clean_char(s[i]);
    dst[i] = '\0';
}

/* Appends s and a tab. */
static size_t put_text(char * line, size_t cap, size_t n, const char * s) {
    for (; s && *s && n + 2 < cap; s++) line[n++] = clean_char(*s);
    if (n + 1 < cap) line[n++] = '\t';
    return n;
}

static const int * row_ints(const list_desc_t * d, const void * row) {
    return (const int *) ((const char *) row + (size_t) d->text_fields * sizeof(const char *));
}

static size_t format_song_row(const void * row, char * line, size_t cap) {
    size_t n = 0;
    for (int i = 0; i < SONG_COLS; i++) {
        const char * field = (const char *) row + song_cols[i].offset;
        if (song_cols[i].size) {
            n = put_text(line, cap, n, field);
            continue;
        }
        int v;
        char num[16];
        memcpy(&v, field, sizeof(v));
        snprintf(num, sizeof(num), "%d", v < 0 ? 0 : v); /* the reader takes no negatives */
        n = put_text(line, cap, n, num);
    }
    line[n - 1] = '\n';
    line[n] = '\0';
    return n;
}

/* One row as a line ending in a newline; the same text feeds the hash. */
static size_t format_row(const list_desc_t * d, const void * row, char * line, size_t cap) {
    if (d->cols) return format_song_row(row, line, cap);
    size_t n = 0;
    for (int i = 0; i < d->text_fields; i++) n = put_text(line, cap, n, ((const char * const *) row)[i]);
    for (int i = 0; i < d->int_fields; i++) {
        char num[16];
        snprintf(num, sizeof(num), "%d", row_ints(d, row)[i]);
        n = put_text(line, cap, n, num);
    }
    line[n - 1] = '\n';
    line[n] = '\0';
    return n;
}

uint64_t subsonic_cache_hash(subsonic_list_kind_t kind, const void * items, int count) {
    const list_desc_t * d = &lists[kind];
    uint64_t hash = SUBSONIC_FNV64_INIT;
    char line[CACHE_LINE_MAX];
    for (int i = 0; i < count; i++) {
        size_t n = format_row(d, (const char *) items + (size_t) i * d->row_size, line, sizeof(line));
        hash = subsonic_fnv64(hash, line, n);
    }
    return hash;
}

typedef struct {
    const list_desc_t * desc;
    const void * items;
    int count;
    bool * oversized; /* set when the rows would not fit the load cap */
    const char * id;  /* songs files: the album or playlist, written in the first line */
    const subsonic_cache_entry_t * entry;
} list_ctx_t;

/* "#v2 <id> <fetched_at> <last_modified>:<last_scan> <hash> <rows>" */
static bool fill_header(FILE * f, const list_ctx_t * list) {
    char id[SONG_ID_SIZE], scan[sizeof(list->entry->stamp.last_scan)];
    clean_copy(id, sizeof(id), list->id);
    clean_copy(scan, sizeof(scan), list->entry->stamp.last_scan);
    return fprintf(f, "#%s\t%s\t%lld\t%lld:%s\t%016llx\t%d\n", CACHE_SCHEMA, id, list->entry->synced_at,
                   list->entry->stamp.last_modified, scan, (unsigned long long) list->entry->hash, list->count) >= 0;
}

static bool fill_list(FILE * f, const void * ctx) {
    const list_ctx_t * list = ctx;
    char line[CACHE_LINE_MAX];
    if (list->id && !fill_header(f, list)) return false;
    for (int i = 0; i < list->count; i++) {
        format_row(list->desc, (const char *) list->items + (size_t) i * list->desc->row_size, line, sizeof(line));
        if (fputs(line, f) == EOF) return false;
        if (ftell(f) > CACHE_FILE_MAX) { /* the loader would reject it: do not write it */
            *list->oversized = true;
            return false;
        }
    }
    return true;
}

typedef struct {
    const subsonic_server_t * server;
    const subsonic_cache_entry_t * entries;
} meta_ctx_t;

static bool fill_meta(FILE * f, const void * ctx) {
    const meta_ctx_t * meta = ctx;
    char base[sizeof(meta->server->base_url)], url[sizeof(base)], user[sizeof(meta->server->username)];
    subsonic_normalize_base_url(meta->server->base_url, base, sizeof(base));
    clean_copy(url, sizeof(url), base);
    clean_copy(user, sizeof(user), meta->server->username);
    if (fprintf(f, "%s\nurl\t%s\nusername\t%s\n", CACHE_SCHEMA, url, user) < 0) return false;
    for (int k = 0; k < SUBSONIC_LIST_KINDS; k++) {
        const subsonic_cache_entry_t * e = &meta->entries[k];
        char scan[sizeof(e->stamp.last_scan)];
        clean_copy(scan, sizeof(scan), e->stamp.last_scan);
        if (fprintf(f, "list\t%s\t%lld\t%016llx\t%d\t%lld\t%s\n", lists[k].name,
                    e->synced_at, (unsigned long long) e->hash, e->rows, e->stamp.last_modified, scan) < 0)
            return false;
    }
    return true;
}

/* Splits line in place on tabs; returns the column count. */
static int split_columns(char * line, char ** cols, int max) {
    int n = 0;
    for (char * p = line; n < max; ) {
        cols[n++] = p;
        p = strchr(p, '\t');
        if (!p) break;
        *p++ = '\0';
    }
    return n;
}

static bool parse_int(const char * s, long long * out) {
    char * end;
    errno = 0;
    long long v = strtoll(s, &end, 10);
    if (end == s || *end || errno || v < 0) return false;
    *out = v;
    return true;
}

static bool parse_song_row(char * line, subsonic_song_t * song) {
    char * cols[CACHE_COLUMNS_MAX + 1];
    if (split_columns(line, cols, CACHE_COLUMNS_MAX + 1) != SONG_COLS || !cols[0][0]) return false;
    memset(song, 0, sizeof(*song));
    for (int i = 0; i < SONG_COLS; i++) {
        char * field = (char *) song + song_cols[i].offset;
        long long v;
        if (song_cols[i].size) {
            size_t len = strlen(cols[i]);
            if (len >= song_cols[i].size) return false;
            memcpy(field, cols[i], len + 1);
        } else if (parse_int(cols[i], &v) && v <= INT_MAX) {
            int n = (int) v;
            memcpy(field, &n, sizeof(n));
        } else {
            return false;
        }
    }
    return true;
}

/* Turns one line into a row; packed rows point their text into line. False for
 * any line with the wrong shape, an oversize field, a bad number or an empty id. */
static bool parse_row(const list_desc_t * d, char * line, void * row) {
    if (d->cols) return parse_song_row(line, row);
    char * cols[CACHE_COLUMNS_MAX + 1];
    int total = d->text_fields + d->int_fields;
    if (split_columns(line, cols, CACHE_COLUMNS_MAX + 1) != total || !cols[0][0]) return false;
    memset(row, 0, d->row_size);
    for (int i = 0; i < d->text_fields; i++) {
        if (strlen(cols[i]) > CACHE_FIELD_MAX) return false;
        ((const char **) row)[i] = cols[i];
    }
    int * ints = (int *) ((char *) row + (size_t) d->text_fields * sizeof(const char *));
    for (int i = 0; i < d->int_fields; i++) {
        long long v;
        if (!parse_int(cols[d->text_fields + i], &v) || v > INT_MAX) return false;
        ints[i] = (int) v;
    }
    return true;
}

/* Reads the next complete line (newline-terminated, under the limit). */
static bool next_line(FILE * f, char * line, size_t size, bool * eof) {
    if (!fgets(line, (int) size, f)) { *eof = true; line[0] = '\0'; return false; }
    size_t len = strlen(line);
    if (len > 0 && line[len - 1] == '\n') { library_trim_eol(line); return true; }
    if (len < size - 1) {
        *eof = true; /* the last line was cut short */
    } else {
        for (int c = fgetc(f); c != EOF && c != '\n'; c = fgetc(f)) {} /* drop the rest of an overlong line */
    }
    return false;
}

typedef bool (*row_sink_t)(void * ctx, const void * row);

/* Hands every valid row to sink; *clean turns false when any line had to be
 * skipped. False when sink refuses a row. */
static bool scan_rows(FILE * f, const list_desc_t * d, row_sink_t sink, void * ctx, bool * clean) {
    union { subsonic_artist_t artist; subsonic_album_t album; subsonic_playlist_t playlist; subsonic_song_t song; } row;
    char line[CACHE_LINE_MAX];
    bool eof = false;
    for (int rows = 0; !eof && rows < CACHE_ROWS_MAX; ) {
        bool got = next_line(f, line, sizeof(line), &eof);
        if (!got || !parse_row(d, line, &row)) {
            if (got || line[0]) *clean = false;
            continue;
        }
        if (!sink(ctx, &row)) return false;
        rows++;
    }
    return true;
}

static bool pack_sink(void * pack, const void * row) { return subsonic_pack_add(pack, row); }

static bool read_rows(FILE * f, const list_desc_t * d, void ** items, int * count, bool * clean) {
    subsonic_pack_t * pack = subsonic_pack_new(d->row_size, d->text_fields);
    if (!pack) return false;
    if (!scan_rows(f, d, pack_sink, pack, clean)) { subsonic_pack_discard(pack); return false; }
    return subsonic_pack_finish(pack, items, count);
}

typedef struct {
    subsonic_song_t * songs;
    int count;
    int cap;
} song_buf_t;

static bool song_sink(void * ctx, const void * row) {
    song_buf_t * buf = ctx;
    if (buf->count == buf->cap) {
        int cap = buf->cap ? buf->cap * 2 : 64;
        subsonic_song_t * grown = realloc(buf->songs, sizeof(*grown) * (size_t) cap);
        if (!grown) return false;
        buf->songs = grown;
        buf->cap = cap;
    }
    buf->songs[buf->count++] = *(const subsonic_song_t *) row;
    return true;
}

/* Parses meta.tsv from f (may be NULL); kinds it does not list stay zero. Closes f. */
static void parse_meta(FILE * f, subsonic_cache_entry_t * entries) {
    memset(entries, 0, sizeof(*entries) * SUBSONIC_LIST_KINDS);
    if (!f) return;
    char line[CACHE_LINE_MAX];
    bool eof = false;
    if (next_line(f, line, sizeof(line), &eof) && strcmp(line, CACHE_SCHEMA) == 0) {
        while (!eof) {
            char * cols[8];
            if (!next_line(f, line, sizeof(line), &eof) || split_columns(line, cols, 8) != 7 || strcmp(cols[0], "list")) continue;
            long long synced, rows, modified;
            for (int k = 0; k < SUBSONIC_LIST_KINDS; k++) {
                if (strcmp(cols[1], lists[k].name) != 0 || !parse_int(cols[2], &synced) || !parse_int(cols[4], &rows) ||
                    rows > INT_MAX || !parse_int(cols[5], &modified))
                    continue;
                entries[k].synced_at = synced;
                entries[k].hash = strtoull(cols[3], NULL, 16);
                entries[k].rows = (int) rows;
                entries[k].stamp.last_modified = modified;
                snprintf(entries[k].stamp.last_scan, sizeof(entries[k].stamp.last_scan), "%s", cols[6]);
            }
        }
    }
    fclose(f);
}

static void read_meta(const subsonic_server_t * server, subsonic_cache_entry_t * entries) {
    char path[512];
    parse_meta(subsonic_cache_path(server, CACHE_META_FILE, path, sizeof(path)) ? fopen(path, "r") : NULL, entries);
}

static bool write_meta(const subsonic_server_t * server, const subsonic_cache_entry_t * entries) {
    meta_ctx_t meta = { server, entries };
    return subsonic_cache_write_atomic(server, CACHE_META_FILE, fill_meta, &meta);
}

bool subsonic_cache_server_known(const subsonic_server_t * server) {
    char path[512];
    struct stat st;
    return subsonic_cache_path(server, CACHE_META_FILE, path, sizeof(path)) && stat(path, &st) == 0;
}

bool subsonic_cache_list_exists(const subsonic_server_t * server, subsonic_list_kind_t kind) {
    char path[512];
    struct stat st;
    return subsonic_cache_path(server, lists[kind].file, path, sizeof(path)) && stat(path, &st) == 0;
}

/* Opens a cache file if it is within the size cap. Saves replace files by
 * rename, so the handle keeps the version that was current when it was opened. */
FILE * subsonic_cache_open(const subsonic_server_t * server, const char * file, long max_bytes) {
    char path[512];
    struct stat st;
    FILE * f = subsonic_cache_path(server, file, path, sizeof(path)) ? fopen(path, "r") : NULL;
    if (f && (fstat(fileno(f), &st) != 0 || st.st_size > max_bytes)) {
        fclose(f);
        f = NULL;
    }
    return f;
}

/* Lost, damaged or edited rows: the stored hash and stamp no longer describe the file. */
static void distrust_if_altered(subsonic_list_kind_t kind, const void * items, int count, bool clean,
                                subsonic_cache_entry_t * entry) {
    if (clean && count == entry->rows && subsonic_cache_hash(kind, items, count) == entry->hash) return;
    entry->hash = 0;
    memset(&entry->stamp, 0, sizeof(entry->stamp));
}

bool subsonic_cache_load(const subsonic_server_t * server, subsonic_list_kind_t kind, void ** items, int * count,
                         subsonic_cache_entry_t * entry) {
    *items = NULL;
    *count = 0;
    subsonic_cache_entry_t entries[SUBSONIC_LIST_KINDS];
    pthread_mutex_lock(&save_mutex); /* rows and meta must come from the same save */
    FILE * rows = subsonic_cache_open(server, lists[kind].file, CACHE_FILE_MAX);
    FILE * meta = rows ? subsonic_cache_open(server, CACHE_META_FILE, CACHE_FILE_MAX) : NULL;
    pthread_mutex_unlock(&save_mutex);
    bool clean = true;
    bool ok = rows && read_rows(rows, &lists[kind], items, count, &clean);
    if (rows) fclose(rows);
    if (ok && !clean && *count == 0) ok = false; /* lines but no row: a file of another version, not a list */
    parse_meta(meta, entries);
    if (!ok) return false;
    *entry = entries[kind];
    distrust_if_altered(kind, *items, *count, clean, entry);
    return true;
}

/* Stores the entry; rows < 0 keeps the row count already on file. Caller holds save_mutex. */
static bool update_entry_locked(const subsonic_server_t * server, subsonic_list_kind_t kind,
                                const subsonic_cache_entry_t * entry, int rows) {
    subsonic_cache_entry_t entries[SUBSONIC_LIST_KINDS];
    read_meta(server, entries);
    int kept = rows < 0 ? entries[kind].rows : rows;
    entries[kind] = *entry;
    entries[kind].rows = kept;
    return write_meta(server, entries);
}

bool subsonic_cache_save_entry(const subsonic_server_t * server, subsonic_list_kind_t kind,
                               const subsonic_cache_entry_t * entry) {
    pthread_mutex_lock(&save_mutex);
    bool ok = update_entry_locked(server, kind, entry, -1);
    pthread_mutex_unlock(&save_mutex);
    return ok;
}

/* A list too big to cache is left absent, not stale. Caller holds save_mutex. */
static void drop_list_locked(const subsonic_server_t * server, subsonic_list_kind_t kind) {
    char path[512];
    if (subsonic_cache_path(server, lists[kind].file, path, sizeof(path))) unlink(path);
    subsonic_cache_entry_t none = {0};
    (void) update_entry_locked(server, kind, &none, 0);
}

bool subsonic_cache_save(const subsonic_server_t * server, subsonic_list_kind_t kind, const void * items, int count,
                         const subsonic_cache_entry_t * entry) {
    bool oversized = count > CACHE_ROWS_MAX; /* the loader would truncate it */
    list_ctx_t list = { &lists[kind], items, count, &oversized, NULL, NULL };
    pthread_mutex_lock(&save_mutex);
    bool ok = !oversized && subsonic_cache_write_atomic(server, lists[kind].file, fill_list, &list) &&
              update_entry_locked(server, kind, entry, count);
    if (oversized) drop_list_locked(server, kind);
    pthread_mutex_unlock(&save_mutex);
    return ok;
}

/* albums/<fnv64(id)>.tsv or playlists/<fnv64(id)>.tsv, relative to the server folder. */
static bool song_file(subsonic_list_kind_t kind, const char * id, char * out, size_t size) {
    if (!subsonic_list_is_songs(kind) || !id || !id[0]) return false;
    uint64_t hash = subsonic_fnv64(SUBSONIC_FNV64_INIT, id, strlen(id));
    int n = snprintf(out, size, "%s/%016llx.tsv", lists[kind].name, (unsigned long long) hash);
    return n > 0 && (size_t) n < size;
}

bool subsonic_cache_songs_exists(const subsonic_server_t * server, subsonic_list_kind_t kind, const char * id) {
    char file[48], path[512];
    struct stat st;
    return song_file(kind, id, file, sizeof(file)) && subsonic_cache_path(server, file, path, sizeof(path)) &&
           stat(path, &st) == 0;
}

/* Reads the first line into entry. False when it is not the line of this id. */
static bool parse_song_header(FILE * f, const char * id, subsonic_cache_entry_t * entry) {
    char line[CACHE_LINE_MAX], want[SONG_ID_SIZE], * cols[8], * colon;
    bool eof = false;
    clean_copy(want, sizeof(want), id);
    if (!next_line(f, line, sizeof(line), &eof) || split_columns(line, cols, 8) != 6 ||
        strcmp(cols[0], "#" CACHE_SCHEMA) != 0 || strcmp(cols[1], want) != 0 || !(colon = strchr(cols[3], ':')))
        return false;
    *colon++ = '\0';
    long long fetched, modified, rows;
    if (!parse_int(cols[2], &fetched) || !parse_int(cols[3], &modified) || !parse_int(cols[5], &rows) || rows > INT_MAX)
        return false;
    memset(entry, 0, sizeof(*entry));
    entry->synced_at = fetched;
    entry->hash = strtoull(cols[4], NULL, 16);
    entry->rows = (int) rows;
    entry->stamp.last_modified = modified;
    snprintf(entry->stamp.last_scan, sizeof(entry->stamp.last_scan), "%s", colon);
    return true;
}

bool subsonic_cache_songs_load(const subsonic_server_t * server, subsonic_list_kind_t kind, const char * id,
                               void ** items, int * count, subsonic_cache_entry_t * entry) {
    *items = NULL;
    *count = 0;
    char file[48];
    if (!song_file(kind, id, file, sizeof(file))) return false;
    pthread_mutex_lock(&save_mutex);
    FILE * f = subsonic_cache_open(server, file, CACHE_FILE_MAX);
    pthread_mutex_unlock(&save_mutex);
    if (!f) return false;
    song_buf_t buf = {0};
    bool clean = true;
    bool ok = parse_song_header(f, id, entry) && scan_rows(f, &lists[kind], song_sink, &buf, &clean);
    fclose(f);
    if (!ok) {
        free(buf.songs);
        return false;
    }
    distrust_if_altered(kind, buf.songs, buf.count, clean, entry);
    *items = buf.songs;
    *count = buf.count;
    return true;
}

/* Caller holds save_mutex. */
static void remove_file_locked(const subsonic_server_t * server, const char * file) {
    char path[512];
    if (subsonic_cache_path(server, file, path, sizeof(path))) unlink(path);
}

bool subsonic_cache_songs_save(const subsonic_server_t * server, subsonic_list_kind_t kind, const char * id,
                               const void * items, int count, const subsonic_cache_entry_t * entry) {
    char file[48];
    if (!song_file(kind, id, file, sizeof(file))) return false;
    bool oversized = count > CACHE_ROWS_MAX; /* the loader would truncate it */
    list_ctx_t list = { &lists[kind], items, count, &oversized, id, entry };
    pthread_mutex_lock(&save_mutex);
    bool ok = !oversized && subsonic_cache_write_atomic(server, file, fill_list, &list);
    if (oversized) remove_file_locked(server, file); /* absent, not stale */
    pthread_mutex_unlock(&save_mutex);
    return ok;
}

void subsonic_cache_songs_remove(const subsonic_server_t * server, subsonic_list_kind_t kind, const char * id) {
    char file[48];
    if (!song_file(kind, id, file, sizeof(file))) return;
    pthread_mutex_lock(&save_mutex);
    remove_file_locked(server, file);
    pthread_mutex_unlock(&save_mutex);
}

#define PRUNE_SCAN_MAX 8192 /* a bigger pile is pruned over several runs */

typedef struct {
    char file[48];
    long long age; /* fetched time or mtime; bigger is newer */
} prune_file_t;

/* Lists the files ending in suffix in the folders. */
static int collect_files(const subsonic_server_t * server, const char * const * dirs, int dir_count,
                         const char * suffix, prune_file_t * out, int max) {
    int n = 0;
    size_t suffix_len = strlen(suffix);
    for (int i = 0; i < dir_count; i++) {
        char dir[512];
        DIR * d = subsonic_cache_path(server, dirs[i], dir, sizeof(dir)) ? opendir(dir) : NULL;
        for (struct dirent * e; d && n < max && (e = readdir(d)); ) {
            size_t len = strlen(e->d_name);
            if (len <= suffix_len || strcmp(e->d_name + len - suffix_len, suffix) != 0) continue; /* skips .tmp leftovers */
            if (snprintf(out[n].file, sizeof(out[n].file), "%s/%s", dirs[i], e->d_name) >= (int) sizeof(out[n].file)) continue;
            out[n++].age = 0;
        }
        if (d) closedir(d);
    }
    return n;
}

/* The fetched time in a songs file's first line. */
static long long song_file_fetched_at(const subsonic_server_t * server, const char * file) {
    FILE * f = subsonic_cache_open(server, file, CACHE_FILE_MAX);
    char line[CACHE_LINE_MAX], * cols[8];
    long long fetched = 0;
    bool eof = false;
    if (f && next_line(f, line, sizeof(line), &eof) && split_columns(line, cols, 8) == 6)
        (void) parse_int(cols[2], &fetched); /* stays 0 when it is not a number */
    if (f) fclose(f);
    return fetched;
}

/* An unreadable file counts as the oldest. */
static long long file_age(const subsonic_server_t * server, const char * file, bool by_mtime) {
    char path[512];
    struct stat st;
    if (!by_mtime) return song_file_fetched_at(server, file);
    return subsonic_cache_path(server, file, path, sizeof(path)) && stat(path, &st) == 0 ? (long long) st.st_mtime : 0;
}

static int older_first(const void * a, const void * b) {
    long long x = ((const prune_file_t *) a)->age, y = ((const prune_file_t *) b)->age;
    return (x > y) - (x < y);
}

void subsonic_cache_prune_files(const subsonic_server_t * server, const char * const * dirs, int dir_count,
                                const char * suffix, bool by_mtime, int keep) {
    if (keep < 0) return; /* a bad budget deletes nothing */
    prune_file_t * files = malloc(sizeof(*files) * PRUNE_SCAN_MAX);
    if (!files) return;
    int n = collect_files(server, dirs, dir_count, suffix, files, PRUNE_SCAN_MAX);
    if (n > keep) {
        for (int i = 0; i < n; i++) files[i].age = file_age(server, files[i].file, by_mtime);
        qsort(files, (size_t) n, sizeof(*files), older_first);
        pthread_mutex_lock(&save_mutex);
        for (int i = 0; i < n - keep; i++) remove_file_locked(server, files[i].file);
        pthread_mutex_unlock(&save_mutex);
    }
    free(files);
}

void subsonic_cache_songs_prune(const subsonic_server_t * server, int keep) {
    const char * dirs[] = { lists[SUBSONIC_LIST_ALBUM_SONGS].name, lists[SUBSONIC_LIST_PLAYLIST_SONGS].name };
    subsonic_cache_prune_files(server, dirs, 2, ".tsv", false, keep);
}
