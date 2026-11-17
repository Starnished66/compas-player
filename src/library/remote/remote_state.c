#include "remote_state.h"
#include "../artwork/support/library_endian.h"
#include "../../core/persistence/storage_paths.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define REMOTE_STATE_DIR storage_sd_path(COMPAS_STORAGE_DIR)

#define REMOTE_STATE_FILE "remote_state.tsv"
#define REMOTE_STATE_PATH_MAX 600
#define REMOTE_STATE_MAX 4096

typedef struct {
    char * path;
    int32_t rating;
    int32_t playcount;
    int32_t last_played;
} rs_entry_t;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static rs_entry_t * entries;
static int entry_n;
static int entry_cap;
static bool loaded;
static atomic_uint revision;

void remote_state_note_rating_change(void) {
    atomic_fetch_add_explicit(&revision, 1, memory_order_relaxed);
}

unsigned remote_state_revision(void) {
    return atomic_load_explicit(&revision, memory_order_relaxed);
}

static int find_index(const char * path) {
    if (!path) return -1;
    for (int i = 0; i < entry_n; i++) {
        if (strcmp(entries[i].path, path) == 0) return i;
    }
    return -1;
}

static void free_all(void) {
    for (int i = 0; i < entry_n; i++) free(entries[i].path);
    free(entries);
    entries = NULL;
    entry_n = 0;
    entry_cap = 0;
}

static rs_entry_t * append_entry(const char * path, int32_t rating, int32_t playcount, int32_t last_played) {
    if (entry_n >= REMOTE_STATE_MAX) return NULL;
    if (entry_n >= entry_cap) {
        int cap = entry_cap ? entry_cap * 2 : 16;
        rs_entry_t * nent = realloc(entries, sizeof(*nent) * (size_t) cap);
        if (!nent) return NULL;
        entries = nent;
        entry_cap = cap;
    }
    entries[entry_n].path = strdup(path);
    if (!entries[entry_n].path) return NULL;
    entries[entry_n].rating = rating;
    entries[entry_n].playcount = playcount;
    entries[entry_n].last_played = last_played;
    return &entries[entry_n++];
}

static void load_file(void) {
    free_all();
    loaded = true;
    char path[640];
    snprintf(path, sizeof(path), "%s/%s", REMOTE_STATE_DIR, REMOTE_STATE_FILE);
    FILE * f = fopen(path, "r");
    if (!f) return;
    char line[REMOTE_STATE_PATH_MAX + 64];
    while (fgets(line, sizeof(line), f)) {
        size_t n = library_trim_eol(line);
        if (n == 0) continue;
        int32_t rating = 0, playcount = 0, last_played = 0;
        char * rest = line;
        rating = (int32_t) strtol(rest, &rest, 10);
        if (rest == line) continue;
        while (*rest == ' ' || *rest == '\t') rest++;
        char * next = rest;
        playcount = (int32_t) strtol(rest, &rest, 10);
        if (rest == next) continue;
        while (*rest == ' ' || *rest == '\t') rest++;
        next = rest;
        last_played = (int32_t) strtol(rest, &rest, 10);
        if (rest == next) continue;
        while (*rest == ' ' || *rest == '\t') rest++;
        if (!*rest || strlen(rest) >= REMOTE_STATE_PATH_MAX) continue;
        if (!append_entry(rest, rating, playcount, last_played)) break;
    }
    fclose(f);
}

static bool save_file(void);

static void migrate_album_keys(void) {
    bool changed = false;
    for (int i = 0; i < entry_n; i++) {
        char *path = entries[i].path;
        if (strncmp(path, "album://", 8) || !strchr(path + 8, '\t')) continue;
        size_t end = strlen(path), trimmed = end;
        while (trimmed && path[trimmed - 1] == ' ') trimmed--;
        if (trimmed == end) continue;
        path[trimmed] = 0;
        int collision = -1;
        for (int j = 0; j < entry_n; j++)
            if (j != i && !strcmp(entries[j].path, path)) { collision = j; break; }
        if (collision >= 0) {
            free(path);
            memmove(&entries[i], &entries[i + 1], sizeof(*entries) * (size_t)(entry_n - i - 1));
            entry_n--; i--;
        }
        changed = true;
    }
    if (changed) { (void)save_file(); remote_state_note_rating_change(); }
}

static void ensure_loaded(void) {
    if (!loaded) { load_file(); migrate_album_keys(); }
}

static bool save_file(void) {
    mkdir(REMOTE_STATE_DIR, 0755);
    char path[640], tmp[640];
    snprintf(path, sizeof(path), "%s/%s", REMOTE_STATE_DIR, REMOTE_STATE_FILE);
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int) sizeof(tmp)) return false;
    FILE * f = fopen(tmp, "w");
    if (!f) return false;
    bool ok = true;
    for (int i = 0; i < entry_n; i++) {
        if (fprintf(f, "%d %d %d %s\n", entries[i].rating, entries[i].playcount, entries[i].last_played,
                    entries[i].path) < 0) {
            ok = false;
            break;
        }
    }
    if (fflush(f) != 0) ok = false;
    if (ok && fsync(fileno(f)) != 0) ok = false;
    if (fclose(f) != 0) ok = false;
    if (!ok) {
        unlink(tmp);
        return false;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return false;
    }
    /* The rename is durable only once the directory is. */
    return library_fsync_dir(REMOTE_STATE_DIR);
}

static rs_entry_t * ensure_entry(const char * path) {
    int i = find_index(path);
    if (i >= 0) return &entries[i];
    if (!path || !path[0] || strlen(path) >= REMOTE_STATE_PATH_MAX) return NULL;
    return append_entry(path, 0, 0, 0);
}

void remote_state_drop(void) {
    pthread_mutex_lock(&mu);
    free_all();
    loaded = false;
    remote_state_note_rating_change();
    pthread_mutex_unlock(&mu);
}

bool remote_state_get(const char * path, int32_t * rating, int32_t * playcount, int32_t * last_played) {
    pthread_mutex_lock(&mu);
    ensure_loaded();
    int i = find_index(path);
    bool ok = i >= 0;
    if (ok) {
        if (rating) *rating = entries[i].rating;
        if (playcount) *playcount = entries[i].playcount;
        if (last_played) *last_played = entries[i].last_played;
    }
    pthread_mutex_unlock(&mu);
    return ok;
}

bool remote_state_set_rating(const char * path, int32_t rating) {
    pthread_mutex_lock(&mu);
    ensure_loaded();
    rs_entry_t * e = ensure_entry(path);
    bool saved = false;
    if (e) {
        e->rating = rating;
        saved = save_file();
        remote_state_note_rating_change();
    }
    pthread_mutex_unlock(&mu);
    return saved;
}

void remote_state_add_play(const char * path, int32_t now) {
    pthread_mutex_lock(&mu);
    ensure_loaded();
    rs_entry_t * e = ensure_entry(path);
    if (e) {
        if (e->playcount < INT32_MAX) e->playcount++;
        e->last_played = now;
        save_file();
    }
    pthread_mutex_unlock(&mu);
}

void remote_state_remove_matches(const remote_state_rating_t *rows, const int32_t (*tuples)[3],
                                  const bool *migrated, int count) {
    pthread_mutex_lock(&mu);
    ensure_loaded();
    bool changed = false;
    for (int n = 0; n < count; n++) {
        if (!migrated[n]) continue;
        int i = find_index(rows[n].path);
        if (i < 0 || entries[i].rating != tuples[n][0] || entries[i].playcount != tuples[n][1] ||
            entries[i].last_played != tuples[n][2]) continue;
        free(entries[i].path);
        memmove(&entries[i], &entries[i + 1], sizeof(entries[0]) * (size_t)(entry_n - i - 1));
        entry_n--;
        changed = true;
    }
    if (changed) save_file();
    pthread_mutex_unlock(&mu);
}

remote_state_rating_t *remote_state_list_prefix(const char *prefix, int *count) {
    *count = 0;
    if (!prefix) return NULL;
    pthread_mutex_lock(&mu);
    ensure_loaded();
    remote_state_rating_t *rows = entry_n ? calloc((size_t)entry_n, sizeof(*rows)) : NULL;
    for (int i = 0; rows && i < entry_n; i++) {
        if (strncmp(entries[i].path, prefix, strlen(prefix))) continue;
        char *path = strdup(entries[i].path);
        if (path) rows[(*count)++] = (remote_state_rating_t){path, entries[i].rating};
    }
    pthread_mutex_unlock(&mu);
    return rows;
}
