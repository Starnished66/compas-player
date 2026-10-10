#include <stdatomic.h>
#include "file_browser.h"
#include "i18n.h"
#include "assets.h"
#include "screen_builders.h" /* STATUS_BAR_CLEARANCE / TITLE_ROW_HEIGHT / LIST_ROW_* */
#include "playlist_files.h" /* playlist_files_resolve_path() -- shared M3U line resolution */
#include "library_endian.h"
#include "fallback_font.h"
#include "file_ops.h" /* file_op_path_in_root() */

#include <dirent.h>
#include <sys/stat.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>

typedef struct {
    char name[256];
    bool is_dir;
    bool is_playlist;
    bool is_cue;
    bool is_other; /* not media; listed only with LIST_ALL_FILES */
    int64_t mtime;
    bool mtime_known;
    int32_t playable_ordinal;
} dir_entry_t;

struct file_browser_index {
    int fd;
    int playable_fd;
    int directory_fd;
    dir_entry_t *memory_rows;
    dir_entry_t *memory_playable_rows;
    bool memory_backed;
    unsigned count;
    unsigned playable_count;
    char directory[PATH_MAX];
};

#ifndef INDEX_RUN_SIZE
#define INDEX_RUN_SIZE 256
#endif
#define INDEX_MEMORY_MAX_ROWS 4096


#define FILE_BROWSER_PAGE_SIZE 64
/* Where each ancestor's list was when a folder was opened from it, so going
 * back up returns to the same page and scroll offset instead of the top.
 * Deeper than the stack, back simply starts at the top. The listing loads
 * asynchronously, so the offset is applied once the parent's index arrives
 * (restore_generation ties it to that exact request). */
#define FILE_BROWSER_POSITION_STACK 32
typedef struct {
    int page_start;
    int32_t scroll_y;
} browser_position_t;

struct file_browser {
    file_browser_t * next; /* in the live `browsers` list */
    lv_obj_t * screen;
    lv_obj_t * path_label;
    lv_obj_t * list;
    lv_timer_t * index_poll_timer;
    char root_dir[PATH_MAX];
    char current_dir[PATH_MAX];
    unsigned list_flags;
    file_browser_options_t callbacks;

    /* The full directory stays in current_index; entries is the visible page. */
    file_browser_index_t * current_index;
    dir_entry_t entries[FILE_BROWSER_PAGE_SIZE];
    int entry_count; /* -1 when the folder could not be read */
    int page_start;

    /* Index worker state. The worker writes under index_worker_mu. */
    atomic_bool index_worker_running;
    atomic_bool index_worker_cancel;
    atomic_bool index_worker_error;
    atomic_bool index_worker_oversized;
    bool deleted; /* freed by the worker when it finishes after the screen went */
    unsigned index_request_generation;
    file_browser_index_t * index_worker_result;
    unsigned index_worker_result_generation;
    unsigned index_error_generation;
    unsigned index_error_rendered_generation;

    browser_position_t position_stack[FILE_BROWSER_POSITION_STACK];
    int position_depth;
    int position_overflow;
    bool restore_pending;
    int32_t restore_scroll_y;
    unsigned restore_generation;

    /* Selection mode: one bit per current_index ordinal. */
    bool selecting;
    uint8_t * selected;
    unsigned selected_count;
};

static file_browser_t * browsers;
static pthread_mutex_t index_worker_mu = PTHREAD_MUTEX_INITIALIZER;
static atomic_int current_sort_mode;

static bool is_cue_file(const char * name);
static bool index_entry_at(const file_browser_index_t *index, unsigned ordinal, dir_entry_t *out);
static bool index_row_playable(const dir_entry_t *row);

/* Snapshot of the directory + raw on-screen row (entries[] index, not the
 * file-only position select_cb receives) a file/playlist was last tapped
 * from -- set in entry_click_cb() right before invoking select_cb(), for
 * gui.c's player "List" option to later reopen the browser at that same
 * spot. */
static char last_selected_dir[PATH_MAX];
static int last_selected_row = -1;

static void rebuild_list(file_browser_t * fb);
static void scan_current_dir(file_browser_t * fb);

/* Kept in sync with audio.c's decoder dispatch. */
static const char * const PLAYABLE_EXTENSIONS[] = {
    ".flac", ".mp3", ".wav", ".rf64", ".w64", ".aiff", ".aif", ".aifc", ".dsf", ".dff", ".aac", ".m4a", ".m4b", ".mp4", ".alac", ".wv", ".caf", ".ape", ".wma", ".opus", ".ogg", ".oga",
};

bool file_browser_is_playable_name(const char * name) {
    const char * ext = strrchr(name, '.');
    if (!ext) return false;
    for (size_t i = 0; i < sizeof(PLAYABLE_EXTENSIONS) / sizeof(PLAYABLE_EXTENSIONS[0]); i++) {
        if (strcasecmp(ext, PLAYABLE_EXTENSIONS[i]) == 0) return true;
    }
    return false;
}


/* What a listing includes besides folders, playlists and playable files. */
enum {
    LIST_CUE = 1u << 0,       /* .cue sheets */
    LIST_ALL_FILES = 1u << 1, /* every other regular file */
    LIST_HIDDEN = 1u << 2,    /* names starting with a dot */
};

static bool skip_name(const char * name, unsigned flags) {
    if (name[0] != '.') return false;
    if (!(flags & LIST_HIDDEN)) return true;
    return strcmp(name, ".") == 0 || strcmp(name, "..") == 0;
}

static bool is_cue_file(const char * name) {
    const char * ext = strrchr(name, '.');
    if (!ext) return false;
    return strcasecmp(ext, ".cue") == 0;
}

static int compare_entries_mode(const void * a, const void * b, file_browser_sort_mode_t mode) {
    const dir_entry_t * ea = (const dir_entry_t *) a;
    const dir_entry_t * eb = (const dir_entry_t *) b;
    if (ea->is_dir != eb->is_dir) return ea->is_dir ? -1 : 1; /* directories before files */
    if (mode == FILE_BROWSER_SORT_NEWEST) {
        if (ea->mtime_known != eb->mtime_known) return ea->mtime_known ? -1 : 1;
        if (ea->mtime_known && ea->mtime != eb->mtime) return ea->mtime > eb->mtime ? -1 : 1;
    }
    int by_name = strcasecmp(ea->name, eb->name);
    return by_name ? by_name : strcmp(ea->name, eb->name);
}

static file_browser_sort_mode_t sort_mode_snapshot(void) {
    int mode = atomic_load_explicit(&current_sort_mode, memory_order_acquire);
    return mode == FILE_BROWSER_SORT_NEWEST ? FILE_BROWSER_SORT_NEWEST : FILE_BROWSER_SORT_NAME;
}

static int compare_entries_name(const void *a, const void *b) {
    return compare_entries_mode(a, b, FILE_BROWSER_SORT_NAME);
}
static int compare_entries_newest(const void *a, const void *b) {
    return compare_entries_mode(a, b, FILE_BROWSER_SORT_NEWEST);
}
static int (*sort_comparator(file_browser_sort_mode_t mode))(const void *, const void *) {
    return mode == FILE_BROWSER_SORT_NEWEST ? compare_entries_newest : compare_entries_name;
}

static int index_make_entry(int directory_fd, const char *name, unsigned char type,
                            dir_entry_t *out, unsigned flags) {
    struct stat st;
    bool mtime_known = fstatat(directory_fd, name, &st, AT_SYMLINK_NOFOLLOW) == 0;
    if (!mtime_known && errno == ENOENT) return 0;
    if (mtime_known && !S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) return 0;
    /* Preserve useful entries when the filesystem supplies a reliable dirent
     * type but cannot provide metadata. Their unknown timestamp sorts last. */
    bool dir;
    if (mtime_known) dir = S_ISDIR(st.st_mode);
    else if (type == DT_DIR) dir = true;
    else if (type == DT_REG) dir = false;
    else return -1;
    bool playlist = !dir && library_is_m3u_file(name);
    bool cue = !dir && (flags & LIST_CUE) && is_cue_file(name);
    bool other = !dir && !playlist && !cue && !file_browser_is_playable_name(name);
    if (other && !(flags & LIST_ALL_FILES)) return 0;
    memset(out, 0, sizeof(*out));
    utf8_truncate_safe(out->name, name, sizeof(out->name));
    out->is_dir = dir; out->is_playlist = playlist; out->is_cue = cue; out->is_other = other;
    out->mtime_known = mtime_known;
    if (mtime_known) out->mtime = (int64_t)st.st_mtime;
    return 1;
}

static int index_temp_fd(int directory_fd) {
    char name[64];
    for (unsigned attempt = 0; attempt < 100; attempt++) {
        snprintf(name, sizeof(name), ".compas_files_%ld_%u", (long)getpid(), attempt);
        int fd = openat(directory_fd, name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) { if (unlinkat(directory_fd, name, 0) != 0) { close(fd); return -1; } return fd; }
        if (errno != EEXIST) return -1;
    }
    return -1;
}

static int index_read_record(int fd, dir_entry_t *row) {
    size_t got = 0;
    while (got < sizeof(*row)) {
        ssize_t n = read(fd, (char *)row + got, sizeof(*row) - got);
        if (n > 0) { got += (size_t)n; continue; }
        if (n == 0 && got == 0) return 1;
        if (n < 0 && errno == EINTR) continue;
        return -1;
    }
    return 0;
}

static bool index_write_full(int fd, const void *data, size_t size) {
    const char *p = data;
    while (size) {
        ssize_t n = write(fd, p, size);
        if (n > 0) { p += n; size -= (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

static int index_write_run(int directory_fd, dir_entry_t *rows, size_t count, file_browser_sort_mode_t mode) {
    qsort(rows, count, sizeof(*rows), sort_comparator(mode));
    int fd = index_temp_fd(directory_fd);
    if (fd < 0 || !index_write_full(fd, rows, count * sizeof(*rows))) {
        if (fd >= 0) close(fd); return -1;
    }
    lseek(fd, 0, SEEK_SET); return fd;
}

static int index_merge_runs(int left, int right, int directory_fd, atomic_bool *cancel, file_browser_sort_mode_t mode) {
    int out = index_temp_fd(directory_fd);
    if (out < 0) { close(left); close(right); return -1; }
    dir_entry_t a, b;
    int na = index_read_record(left, &a), nb = index_read_record(right, &b);
    if (na < 0 || nb < 0) { close(left); close(right); close(out); return -1; }
    while (na == 0 || nb == 0) {
        if (cancel && atomic_load(cancel)) { close(left); close(right); close(out); return -1; }
        dir_entry_t *pick;
        if (nb != 0 || (na == 0 && compare_entries_mode(&a, &b, mode) <= 0)) pick = &a;
        else pick = &b;
        if (!index_write_full(out, pick, sizeof(*pick))) { close(left); close(right); close(out); return -1; }
        if (pick == &a) na = index_read_record(left, &a); else nb = index_read_record(right, &b);
        if (na < 0 || nb < 0) { close(left); close(right); close(out); return -1; }
    }
    close(left); close(right); lseek(out, 0, SEEK_SET); return out;
}

static bool index_cancelled(atomic_bool *cancel) {
    return cancel && atomic_load_explicit(cancel, memory_order_acquire);
}

static bool index_open_memory(const char *directory, DIR *dir, int dfd,
                              file_browser_index_t **out,
                              unsigned *out_count, unsigned flags,
                              atomic_bool *cancel, file_browser_sort_mode_t mode) {
    dir_entry_t *rows = malloc(sizeof(*rows) * INDEX_MEMORY_MAX_ROWS);
    if (!rows) { close(dfd); closedir(dir); return false; }
    unsigned count = 0;
    bool ok = true;
    for (;;) {
        errno = 0;
        struct dirent *de = readdir(dir);
        if (!de) { if (errno) ok = false; break; }
        if (index_cancelled(cancel)) { ok = false; break; }
        if (skip_name(de->d_name, flags)) continue;
        dir_entry_t candidate;
        int status = index_make_entry(dfd, de->d_name, de->d_type, &candidate, flags);
        if (status < 0) { ok = false; break; }
        if (status > 0) {
            if (count >= INDEX_MEMORY_MAX_ROWS) { errno = EFBIG; ok = false; break; }
            rows[count] = candidate;
            count++;
        }
    }
    int failure_errno = errno;
    closedir(dir);
    if (!ok) { free(rows); close(dfd); errno = failure_errno; return false; }
    qsort(rows, count, sizeof(*rows), sort_comparator(mode));
    file_browser_index_t *idx = calloc(1, sizeof(*idx));
    if (!idx) { free(rows); close(dfd); return false; }
    idx->fd = -1; idx->playable_fd = -1; idx->directory_fd = dfd;
    idx->memory_rows = rows;
    idx->memory_playable_rows = count ? malloc(sizeof(*rows) * count) : NULL;
    idx->memory_backed = true; idx->count = count;
    if (count && !idx->memory_playable_rows) {
        free(idx->memory_rows); free(idx); close(dfd); return false;
    }
    for (unsigned i = 0; i < count; i++) {
        idx->memory_rows[i].playable_ordinal = -1;
        if (index_row_playable(&idx->memory_rows[i])) {
            idx->memory_rows[i].playable_ordinal = (int32_t)idx->playable_count;
            idx->memory_playable_rows[idx->playable_count++] = idx->memory_rows[i];
        }
    }
    snprintf(idx->directory, sizeof(idx->directory), "%s", directory);
    *out = idx; if (out_count) *out_count = count;
    return true;
}

static bool file_browser_index_open_ex(const char *directory, file_browser_index_t **out,
                                       unsigned *out_count, unsigned flags,
                                       atomic_bool *cancel, file_browser_sort_mode_t mode) {
    if (!directory || !out) return false;
    *out = NULL; if (out_count) *out_count = 0;
    DIR *dir = opendir(directory); if (!dir) return false;
    int dfd = dup(dirfd(dir)), levels[32];
    if (dfd < 0) { closedir(dir); return false; }
#ifdef FILE_BROWSER_TEST_FORCE_MEMORY_INDEX
    return index_open_memory(directory, dir, dfd, out, out_count, flags, cancel, mode);
#endif
    memset(levels, -1, sizeof(levels));
    errno = 0;
    int probe = index_temp_fd(dfd);
    if (probe < 0 && (errno == EROFS || errno == EACCES || errno == EPERM)) {
        return index_open_memory(directory, dir, dfd, out, out_count, flags, cancel, mode);
    }
    if (probe < 0) goto fail;
    close(probe);
    size_t row_count = 0; unsigned total = 0;
    dir_entry_t rows[INDEX_RUN_SIZE]; struct dirent *de;
    for (;;) {
        errno = 0;
        de = readdir(dir);
        if (!de) { if (errno) goto fail; break; }
        if (cancel && atomic_load(cancel)) goto fail;
        if (skip_name(de->d_name, flags)) continue;
        int entry_status = index_make_entry(dfd, de->d_name, de->d_type, &rows[row_count], flags);
        if (entry_status < 0) goto fail;
        if (entry_status == 0) continue;
        if (++row_count == INDEX_RUN_SIZE) {
            int fd = index_write_run(dfd, rows, row_count, mode); if (fd < 0) goto fail;
            total += (unsigned)row_count; row_count = 0;
            for (unsigned level = 0; ; level++) { if (level >= 32) { close(fd); goto fail; } if (levels[level] < 0) { levels[level] = fd; break; } int prior = levels[level]; levels[level] = -1; fd = index_merge_runs(prior, fd, dfd, cancel, mode); if (fd < 0) goto fail; }
        }
    }
    if (row_count) { int fd = index_write_run(dfd, rows, row_count, mode); if (fd < 0) goto fail; total += (unsigned)row_count; for (unsigned level = 0; ; level++) { if (level >= 32) { close(fd); goto fail; } if (levels[level] < 0) { levels[level] = fd; break; } int prior = levels[level]; levels[level] = -1; fd = index_merge_runs(prior, fd, dfd, cancel, mode); if (fd < 0) goto fail; } }
    closedir(dir);
    int final = -1; for (unsigned level = 0; level < 32; level++) if (levels[level] >= 0) { int prior = levels[level]; levels[level] = -1; if (final < 0) final = prior; else { final = index_merge_runs(prior, final, dfd, cancel, mode); if (final < 0) goto fail_closed; } }
    file_browser_index_t *idx = calloc(1, sizeof(*idx)); if (!idx) { if (final >= 0) close(final); goto fail_closed; }
    idx->fd = final >= 0 ? final : index_temp_fd(dfd); idx->count = total; idx->playable_fd = index_temp_fd(dfd); idx->directory_fd = dup(dfd); idx->playable_count = 0;
    if (idx->fd < 0 || idx->playable_fd < 0 || idx->directory_fd < 0) { if (idx->fd >= 0) close(idx->fd); if (idx->playable_fd >= 0) close(idx->playable_fd); if (idx->directory_fd >= 0) close(idx->directory_fd); free(idx); goto fail_closed; }
    dir_entry_t row;
    for (unsigned i = 0; i < idx->count; i++) {
        if (pread(idx->fd, &row, sizeof(row), (off_t)i * sizeof(row)) != (ssize_t)sizeof(row)) { close(idx->fd); close(idx->playable_fd); close(idx->directory_fd); free(idx); goto fail_closed; }
        row.playable_ordinal = -1;
        if (index_row_playable(&row)) {
            row.playable_ordinal = (int32_t)idx->playable_count;
            if (pwrite(idx->fd, &row, sizeof(row), (off_t)i * sizeof(row)) != (ssize_t)sizeof(row) ||
                !index_write_full(idx->playable_fd, &row, sizeof(row))) { close(idx->fd); close(idx->playable_fd); close(idx->directory_fd); free(idx); goto fail_closed; }
            idx->playable_count++;
        }
    }
    lseek(idx->playable_fd, 0, SEEK_SET);
    snprintf(idx->directory, sizeof(idx->directory), "%s", directory);
    close(dfd); *out = idx; if (out_count) *out_count = idx->count; return true;
fail:
    closedir(dir);
fail_closed:
    close(dfd); for (unsigned i = 0; i < 32; i++) if (levels[i] >= 0) close(levels[i]); return false;
}

bool file_browser_index_open(const char *directory, file_browser_index_t **out, unsigned *out_count) {
    return file_browser_index_open_ex(directory, out, out_count, 0, NULL, sort_mode_snapshot());
}

bool file_browser_index_retain(const file_browser_index_t *source, file_browser_index_t **out) {
    if (!source || !out) return false; file_browser_index_t *copy = calloc(1, sizeof(*copy)); if (!copy) return false;
    if (source->memory_backed) {
        copy->fd = -1; copy->playable_fd = -1;
        copy->directory_fd = dup(source->directory_fd);
        copy->count = source->count; copy->playable_count = source->playable_count;
        copy->memory_backed = true;
        copy->memory_rows = malloc(sizeof(*copy->memory_rows) * copy->count);
        copy->memory_playable_rows = malloc(sizeof(*copy->memory_playable_rows) * copy->playable_count);
        if (copy->directory_fd < 0 || (copy->count && !copy->memory_rows) ||
            (copy->playable_count && !copy->memory_playable_rows)) {
            if (copy->directory_fd >= 0) close(copy->directory_fd);
            free(copy->memory_rows); free(copy->memory_playable_rows); free(copy); return false;
        }
        memcpy(copy->memory_rows, source->memory_rows, sizeof(*copy->memory_rows) * copy->count);
        memcpy(copy->memory_playable_rows, source->memory_playable_rows,
               sizeof(*copy->memory_playable_rows) * copy->playable_count);
        snprintf(copy->directory, sizeof(copy->directory), "%s", source->directory);
        *out = copy; return true;
    }
    copy->fd = dup(source->fd); copy->playable_fd = dup(source->playable_fd); copy->directory_fd = dup(source->directory_fd);
    if (copy->fd < 0 || copy->playable_fd < 0 || copy->directory_fd < 0) { if (copy->fd >= 0) close(copy->fd); if (copy->playable_fd >= 0) close(copy->playable_fd); if (copy->directory_fd >= 0) close(copy->directory_fd); free(copy); return false; }
    copy->count = source->count; copy->playable_count = source->playable_count; snprintf(copy->directory, sizeof(copy->directory), "%s", source->directory); *out = copy; return true;
}

bool file_browser_index_path_at(const file_browser_index_t *index, unsigned ordinal, char *out_path, size_t out_size) {
    if (!index || ordinal >= index->count || !out_path || !out_size) return false;
    dir_entry_t row;
    if (index->memory_backed) row = index->memory_rows[ordinal];
    else if (pread(index->fd, &row, sizeof(row), (off_t)ordinal * sizeof(row)) != (ssize_t)sizeof(row)) return false;
    return snprintf(out_path, out_size, "%s/%s", index->directory, row.name) < (int)out_size;
}

static bool index_row_playable(const dir_entry_t *row) {
    return row && !row->is_dir && !row->is_playlist && !row->is_cue && !row->is_other;
}

unsigned file_browser_index_playable_count(const file_browser_index_t *index) {
    if (!index) return 0;
    return index->playable_count;
}

bool file_browser_index_playable_path_at(const file_browser_index_t *index, unsigned ordinal,
                                         char *out_path, size_t out_size) {
    if (!index || !out_path || !out_size) return false;
    if (ordinal >= index->playable_count) return false;
    dir_entry_t row;
    if (index->memory_backed) row = index->memory_playable_rows[ordinal];
    else if (pread(index->playable_fd, &row, sizeof(row), (off_t)ordinal * sizeof(row)) != (ssize_t)sizeof(row)) return false;
    return snprintf(out_path, out_size, "%s/%s", index->directory, row.name) < (int)out_size;
}

int file_browser_index_dup_directory_fd(const file_browser_index_t *index) {
    return index ? dup(index->directory_fd) : -1;
}

const char *file_browser_index_directory(const file_browser_index_t *index) {
    return index ? index->directory : NULL;
}

void file_browser_index_close(file_browser_index_t *index) {
    if (!index) return;
    if (index->fd >= 0) close(index->fd);
    if (index->playable_fd >= 0) close(index->playable_fd);
    if (index->directory_fd >= 0) close(index->directory_fd);
    free(index->memory_rows); free(index->memory_playable_rows); free(index);
}

static bool index_entry_at(const file_browser_index_t *index, unsigned ordinal, dir_entry_t *out) {
    if (!index || ordinal >= index->count || !out) return false;
    if (index->memory_backed) { *out = index->memory_rows[ordinal]; return true; }
    return pread(index->fd, out, sizeof(*out), (off_t)ordinal * sizeof(*out)) == (ssize_t)sizeof(*out);
}

bool file_browser_index_entry_name(const file_browser_index_t *index, unsigned ordinal,
                                   char *name, size_t name_size, bool *is_dir) {
    dir_entry_t row;
    if (!name || name_size == 0 || !index_entry_at(index, ordinal, &row)) return false;
    if (strlen(row.name) >= name_size) return false;
    memcpy(name, row.name, strlen(row.name) + 1);
    if (is_dir) *is_dir = row.is_dir;
    return true;
}

/* Copy one screen of an already-open index. Does not rescan or allocate. */
static bool load_visible_entries(file_browser_t * fb) {
    if (fb->entry_count < 0) return false;
    if (fb->page_start < 0) fb->page_start = 0;
    if (fb->page_start >= fb->entry_count)
        fb->page_start = fb->entry_count > 0 ? ((fb->entry_count - 1) / FILE_BROWSER_PAGE_SIZE) * FILE_BROWSER_PAGE_SIZE : 0;
    if (!fb->current_index) return fb->entry_count == 0;
    int end = fb->page_start + FILE_BROWSER_PAGE_SIZE;
    if (end > fb->entry_count) end = fb->entry_count;
    for (int i = fb->page_start; i < end; i++) {
        if (!index_entry_at(fb->current_index, (unsigned) i, &fb->entries[i - fb->page_start])) return false;
    }
    return true;
}

static void notify_selection(file_browser_t * fb) {
    if (fb->callbacks.on_selection) fb->callbacks.on_selection(fb);
}

/* The selection names ordinals of current_index, so it ends with it. */
static void drop_selection(file_browser_t * fb) {
    bool had = fb->selecting || fb->selected_count;
    free(fb->selected);
    fb->selected = NULL;
    fb->selected_count = 0;
    fb->selecting = false;
    if (had) notify_selection(fb);
}

static void free_entries(file_browser_t * fb) {
    if (fb->current_index) { file_browser_index_close(fb->current_index); fb->current_index = NULL; }
    memset(fb->entries, 0, sizeof(fb->entries));
    fb->entry_count = 0;
    drop_selection(fb);
}

/* Scans dir_path into a freshly malloc'd, sorted (dirs-first, then alpha)
 * array of playable entries. Caller owns the result (free() it). Used both
 * for the interactive browser's current directory (via scan_current_dir)
 * and for one-shot lookups that don't touch the browser's own state (e.g.
 * resuming a track without ever having opened the browser screen). */
static int scan_entry_limit(void) {
    const char *limit = getenv("FILE_BROWSER_TEST_ENTRY_LIMIT");
    if (!limit || !limit[0]) return INT_MAX;
    char *end = NULL;
    long value = strtol(limit, &end, 10);
    if (end == limit || value < 0 || value > INT_MAX) return INT_MAX;
    return (int) value;
}

/* Returns the entry count, 0 when the directory cannot be opened, and -1
 * when memory cannot hold the whole directory. A short list is never
 * reported as success. */
static int scan_directory(const char * dir_path, dir_entry_t ** out_entries,
                          file_browser_sort_mode_t mode, unsigned flags) {
    DIR * dir = opendir(dir_path);
    if (!dir) {
        fprintf(stderr, "file_browser: failed to open '%s'\n", dir_path);
        *out_entries = NULL;
        return 0;
    }

    int capacity = 32;
    int count = 0;
    int limit = scan_entry_limit();
    dir_entry_t * result = malloc(sizeof(dir_entry_t) * (size_t) capacity);
    if (!result) {
        closedir(dir);
        *out_entries = NULL;
        return -1;
    }

    struct dirent * de;
    while ((de = readdir(dir)) != NULL) {
        if (skip_name(de->d_name, flags)) continue;

        dir_entry_t candidate;
        int status = index_make_entry(dirfd(dir), de->d_name, de->d_type,
                                      &candidate, flags);
        if (status < 0) {
            free(result);
            closedir(dir);
            *out_entries = NULL;
            return -1;
        }
        if (status == 0) continue;

        if (count >= limit) {
            free(result);
            closedir(dir);
            *out_entries = NULL;
            return -1;
        }
        if (count == capacity) {
            dir_entry_t * grown = realloc(result, sizeof(dir_entry_t) * (size_t) (capacity * 2));
            if (!grown) {
                free(result);
                closedir(dir);
                *out_entries = NULL;
                return -1;
            }
            result = grown;
            capacity *= 2;
        }

        result[count] = candidate;
        count++;
    }

    closedir(dir);

    qsort(result, (size_t) count, sizeof(dir_entry_t), sort_comparator(mode));
    *out_entries = result;
    return count;
}

typedef struct {
    file_browser_t * fb;
    unsigned generation;
    char directory[PATH_MAX];
    unsigned list_flags;
    file_browser_sort_mode_t sort_mode;
} index_worker_request_t;

static void *index_worker_main(void *arg) {
    index_worker_request_t request = *(index_worker_request_t *)arg;
    free(arg);
    file_browser_t * fb = request.fb;
    unsigned generation = request.generation;
    file_browser_index_t *built = NULL; unsigned count = 0;
    bool ok = file_browser_index_open_ex(request.directory, &built, &count, request.list_flags,
                                         &fb->index_worker_cancel, request.sort_mode);
    bool oversized = !ok && errno == EFBIG;
    pthread_mutex_lock(&index_worker_mu);
    if (fb->deleted) {
        pthread_mutex_unlock(&index_worker_mu);
        if (built) file_browser_index_close(built);
        free(fb);
        return NULL;
    }
    if (ok && !atomic_load(&fb->index_worker_cancel) && generation == fb->index_request_generation) {
        if (fb->index_worker_result) file_browser_index_close(fb->index_worker_result);
        fb->index_worker_result = built; fb->index_worker_result_generation = generation;
    } else {
        if (built) file_browser_index_close(built);
        if (!ok && generation == fb->index_request_generation) {
            atomic_store(&fb->index_worker_error, true);
            atomic_store(&fb->index_worker_oversized, oversized);
            fb->index_error_generation = generation;
        }
    }
    atomic_store(&fb->index_worker_running, false);
    pthread_mutex_unlock(&index_worker_mu);
    return NULL;
}

/* Called with index_worker_mu held; all mutable inputs are copied into this
 * request before the worker starts, so a newer scan cannot change its order. */
static bool index_worker_launch_locked(file_browser_t * fb, unsigned generation) {
    index_worker_request_t *request = calloc(1, sizeof(*request));
    if (!request) return false;
    request->fb = fb;
    request->generation = generation;
    snprintf(request->directory, sizeof(request->directory), "%s", fb->current_dir);
    request->list_flags = fb->list_flags;
    request->sort_mode = sort_mode_snapshot();
    atomic_store(&fb->index_worker_cancel, false);
    atomic_store(&fb->index_worker_running, true);
    pthread_t thread;
    if (pthread_create(&thread, NULL, index_worker_main, request) != 0) {
        free(request);
        atomic_store(&fb->index_worker_running, false);
        return false;
    }
    pthread_detach(thread);
    return true;
}

static void index_poll_cb(lv_timer_t *timer) {
    file_browser_t * fb = lv_timer_get_user_data(timer);
    pthread_mutex_lock(&index_worker_mu);
    file_browser_index_t *ready = fb->index_worker_result;
    unsigned generation = fb->index_worker_result_generation;
    fb->index_worker_result = NULL;
    bool running = atomic_load(&fb->index_worker_running);
    pthread_mutex_unlock(&index_worker_mu);
    if (!ready || generation != fb->index_request_generation) {
        if (ready) file_browser_index_close(ready);
        if (!running && atomic_load(&fb->index_worker_error) &&
            fb->index_error_generation == fb->index_request_generation &&
            fb->index_error_rendered_generation != fb->index_request_generation) {
            fb->index_error_rendered_generation = fb->index_request_generation;
            free_entries(fb);
            fb->entry_count = -1;
            rebuild_list(fb);
        }
        else if (!running && !atomic_load(&fb->index_worker_error) && !fb->current_index && fb->entry_count == 0) {
            pthread_mutex_lock(&index_worker_mu);
            if (!index_worker_launch_locked(fb, fb->index_request_generation)) {
                atomic_store(&fb->index_worker_error, true);
                fb->index_error_generation = fb->index_request_generation;
            }
            pthread_mutex_unlock(&index_worker_mu);
        }
        return;
    }
    if (fb->current_index) file_browser_index_close(fb->current_index);
    fb->current_index = ready; fb->entry_count = (int)ready->count;
    free(fb->selected);
    fb->selected = NULL;
    fb->selected_count = 0;
    if (fb->callbacks.on_selection && ready->count)
        fb->selected = calloc((ready->count + 7) / 8, 1); /* NULL: selection unavailable */
    if (!load_visible_entries(fb)) {
        atomic_store(&fb->index_worker_error, true);
        fb->entry_count = -1;
    }
    rebuild_list(fb);
    if (fb->restore_pending && fb->restore_generation == generation) {
        fb->restore_pending = false;
        if (fb->entry_count > 0) {
            /* Clamped by LVGL if the folder shrank since. */
            lv_obj_update_layout(fb->list);
            lv_obj_scroll_to_y(fb->list, fb->restore_scroll_y, LV_ANIM_OFF);
        }
    }
}

static void scan_current_dir(file_browser_t * fb) {
    free_entries(fb);
    pthread_mutex_lock(&index_worker_mu);
    unsigned generation = ++fb->index_request_generation;
    atomic_store(&fb->index_worker_error, false);
    atomic_store(&fb->index_worker_oversized, false);
    atomic_store(&fb->index_worker_cancel, true);
    if (fb->index_worker_result) { file_browser_index_close(fb->index_worker_result); fb->index_worker_result = NULL; }
    if (!atomic_load(&fb->index_worker_running) && !index_worker_launch_locked(fb, generation)) {
        atomic_store(&fb->index_worker_error, true);
        fb->index_error_generation = generation;
    }
    pthread_mutex_unlock(&index_worker_mu);
    fb->entry_count = 0;
}

/* A running worker still uses fb (its cancel flag), so it frees fb itself. */
static void browser_delete_cb(lv_event_t *event) {
    file_browser_t * fb = lv_event_get_user_data(event);
    for (file_browser_t ** link = &browsers; *link; link = &(*link)->next) {
        if (*link == fb) { *link = fb->next; break; }
    }
    if (fb->index_poll_timer) lv_timer_delete(fb->index_poll_timer);
    free(fb->selected);
    pthread_mutex_lock(&index_worker_mu);
    ++fb->index_request_generation;
    atomic_store(&fb->index_worker_cancel, true);
    if (fb->index_worker_result) { file_browser_index_close(fb->index_worker_result); fb->index_worker_result = NULL; }
    if (fb->current_index) { file_browser_index_close(fb->current_index); fb->current_index = NULL; }
    bool worker_frees = atomic_load(&fb->index_worker_running);
    fb->deleted = worker_frees;
    pthread_mutex_unlock(&index_worker_mu);
    if (!worker_frees) free(fb);
}

/* Builds the playlist from every playable file in the current directory
 * (in the same sorted order they're displayed) and reports which position
 * within that file-only list corresponds to `file_display_index`. */
static void build_playlist_and_select(file_browser_t * fb, int file_display_index) {
    dir_entry_t selected_entry;
    if (!index_entry_at(fb->current_index, (unsigned)file_display_index, &selected_entry)) return;
    if (fb->callbacks.on_index_select) {
        unsigned playable = fb->current_index->playable_count;
        unsigned selected = selected_entry.playable_ordinal >= 0 ? (unsigned)selected_entry.playable_ordinal : 0;
        file_browser_index_t *retained = NULL;
        if (file_browser_index_retain(fb->current_index, &retained))
            fb->callbacks.on_index_select(retained, playable, selected);
        return;
    }
    dir_entry_t *all = NULL;
    int all_count = scan_directory(fb->current_dir, &all, sort_mode_snapshot(), fb->list_flags & LIST_CUE);
    if (all_count <= 0) { free(all); return; }
    char ** playlist = malloc(sizeof(char *) * (size_t) all_count);
    if (!playlist) { free(all); return; }
    int count = 0;
    int selected = -1;

    for (int i = 0; i < all_count; i++) {
        if (all[i].is_dir || all[i].is_playlist || all[i].is_cue) continue;
        if (strcmp(all[i].name, selected_entry.name) == 0) selected = count;

        char full_path[PATH_MAX];
        snprintf(full_path, sizeof(full_path), "%s/%s", fb->current_dir, all[i].name);
        playlist[count] = strdup(full_path);
        if (!playlist[count]) { for (int j = 0; j < count; j++) free(playlist[j]); free(playlist); free(all); return; }
        count++;
    }
    free(all);

    if (selected < 0) {
        for (int i = 0; i < count; i++) free(playlist[i]);
        free(playlist);
        return;
    }

    fb->callbacks.on_select(playlist, count, selected);
}

file_browser_sort_mode_t file_browser_get_sort_mode(void) {
    return sort_mode_snapshot();
}

void file_browser_set_sort_mode(file_browser_sort_mode_t mode) {
    if (mode != FILE_BROWSER_SORT_NEWEST) mode = FILE_BROWSER_SORT_NAME;
    file_browser_sort_mode_t previous = sort_mode_snapshot();
    if (previous == mode) return;
    atomic_store_explicit(&current_sort_mode, mode, memory_order_release);
    last_selected_row = -1;
    /* Ordering changed, so previously saved row and scroll offsets no longer
     * identify the same entries. Keep the current path and restart there. */
    for (file_browser_t * fb = browsers; fb; fb = fb->next) {
        fb->page_start = 0;
        fb->position_depth = 0;
        fb->position_overflow = 0;
        fb->restore_pending = false;
        fb->restore_scroll_y = 0;
        scan_current_dir(fb);
        rebuild_list(fb);
    }
}

bool file_browser_build_playlist_from_m3u(const char * m3u_path, char *** out_playlist, int * out_count) {
    char ** paths = NULL;
    int count = 0;
    *out_playlist = NULL;
    *out_count = 0;
    if (!playlist_files_read(m3u_path, &paths, &count)) return false;
    int kept = 0;
    for (int i = 0; i < count; i++) {
        if (file_browser_is_playable_name(paths[i])) paths[kept++] = paths[i];
        else free(paths[i]);
    }
    if (!kept) { free(paths); return false; }
    *out_playlist = paths;
    *out_count = kept;
    return true;
}

static bool at_root(const file_browser_t * fb) {
    return strlen(fb->current_dir) <= strlen(fb->root_dir);
}

/* Steps current_dir up one level toward root_dir and rebuilds the list.
 * No-op if already at root. */
static void go_up(file_browser_t * fb) {
    char * last_slash = strrchr(fb->current_dir, '/');
    if (last_slash && !at_root(fb)) {
        *last_slash = '\0';
        if (strlen(fb->current_dir) < strlen(fb->root_dir)) {
            memcpy(fb->current_dir, fb->root_dir, sizeof(fb->current_dir));
        }
        bool restore = false;
        if (fb->position_overflow > 0) {
            fb->position_overflow--;
            fb->page_start = 0;
        } else if (fb->position_depth > 0) {
            fb->position_depth--;
            fb->page_start = fb->position_stack[fb->position_depth].page_start;
            fb->restore_scroll_y = fb->position_stack[fb->position_depth].scroll_y;
            restore = true;
        } else {
            fb->page_start = 0;
        }
        scan_current_dir(fb);
        fb->restore_pending = restore;
        fb->restore_generation = fb->index_request_generation;
        rebuild_list(fb);
    }
}

bool file_browser_go_up_for_screen(lv_obj_t * screen) {
    for (file_browser_t * fb = browsers; fb; fb = fb->next) {
        if (fb->screen != screen) continue;
        if (fb->selecting) { file_browser_set_selecting(fb, false); return true; }
        if (at_root(fb)) return false;
        go_up(fb);
        return true;
    }
    return false;
}

/* Rows are children of the list, whose user data is its browser. */
static file_browser_t * browser_of(lv_event_t * e) {
    return lv_obj_get_user_data(lv_obj_get_parent(lv_event_get_current_target_obj(e)));
}

static void up_click_cb(lv_event_t * e) {
    go_up(browser_of(e));
}

static void page_click_cb(lv_event_t * e) {
    file_browser_t * fb = browser_of(e);
    int delta = (int) (intptr_t) lv_event_get_user_data(e);
    int next = fb->page_start + delta;
    if (next < 0) next = 0;
    if (next >= fb->entry_count) next = fb->entry_count > 0
        ? ((fb->entry_count - 1) / FILE_BROWSER_PAGE_SIZE) * FILE_BROWSER_PAGE_SIZE : 0;
    if (next == fb->page_start) return;
    fb->page_start = next;
    if (!load_visible_entries(fb)) {
        atomic_store(&fb->index_worker_error, true);
        fb->entry_count = -1;
    }
    rebuild_list(fb);
}

/* Entries in the folder's index; entry_count is -1 after a read error. */
static unsigned listed_count(const file_browser_t * fb) {
    return fb->current_index ? fb->current_index->count : 0;
}

static bool is_selected(const file_browser_t * fb, unsigned ordinal) {
    return fb->selected && (fb->selected[ordinal / 8] >> (ordinal % 8)) & 1u;
}

static void set_selected(file_browser_t * fb, unsigned ordinal, bool on) {
    if (!fb->selected || is_selected(fb, ordinal) == on) return;
    fb->selected[ordinal / 8] ^= (uint8_t) (1u << (ordinal % 8));
    if (on) fb->selected_count++;
    else fb->selected_count--;
}

/* Full rebuild for a mode change, at the same scroll offset. */
static void rebuild_list_in_place(file_browser_t * fb) {
    int32_t scroll_y = lv_obj_get_scroll_y(fb->list);
    rebuild_list(fb);
    lv_obj_update_layout(fb->list);
    lv_obj_scroll_to_y(fb->list, scroll_y, LV_ANIM_OFF);
}

static void entry_click_cb(lv_event_t * e) {
    file_browser_t * fb = browser_of(e);
    int index = (int) (intptr_t) lv_event_get_user_data(e);
    dir_entry_t clicked;
    if (!index_entry_at(fb->current_index, (unsigned)index, &clicked)) return;

    if (fb->selecting) {
        set_selected(fb, (unsigned) index, !is_selected(fb, (unsigned) index));
        /* The check box is the row's last child. */
        lv_obj_t * row = lv_event_get_current_target_obj(e);
        lv_obj_delete(lv_obj_get_child(row, -1));
        lv_obj_align(build_check_box(row, is_selected(fb, (unsigned) index)), LV_ALIGN_RIGHT_MID,
                     -LIST_ROW_LABEL_INSET, 0);
        notify_selection(fb);
        return;
    }

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", fb->current_dir, clicked.name);
    if (clicked.is_dir) {
        snprintf(fb->current_dir, sizeof(fb->current_dir), "%s", path);
        if (fb->position_depth < FILE_BROWSER_POSITION_STACK) {
            fb->position_stack[fb->position_depth].page_start = fb->page_start;
            fb->position_stack[fb->position_depth].scroll_y = lv_obj_get_scroll_y(fb->list);
            fb->position_depth++;
        } else {
            fb->position_overflow++;
        }
        fb->restore_pending = false;
        fb->page_start = 0;
        scan_current_dir(fb);
        rebuild_list(fb);
    } else if (clicked.is_playlist) {
        char ** playlist;
        int count;
        if (file_browser_build_playlist_from_m3u(path, &playlist, &count)) {
            snprintf(last_selected_dir, sizeof(last_selected_dir), "%s", fb->current_dir);
            last_selected_row = index;
            fb->callbacks.on_select(playlist, count, 0);
        }
    } else if (clicked.is_cue) {
        snprintf(last_selected_dir, sizeof(last_selected_dir), "%s", fb->current_dir);
        last_selected_row = index;
        fb->callbacks.on_cue_select(path);
    } else if (clicked.is_other) {
        fb->callbacks.on_open_other(path);
    } else {
        snprintf(last_selected_dir, sizeof(last_selected_dir), "%s", fb->current_dir);
        last_selected_row = index;
        build_playlist_and_select(fb, index);
    }
}

/* A long press starts selection mode with the pressed row selected. */
static void entry_long_press_cb(lv_event_t * e) {
    file_browser_t * fb = browser_of(e);
    if (fb->selecting || !fb->selected) return;
    set_selected(fb, (unsigned) (intptr_t) lv_event_get_user_data(e), true);
    fb->selecting = true;
    /* The rebuild deletes the pressed row; its release must not click
     * whatever row takes its place. */
    lv_indev_wait_release(lv_indev_active());
    rebuild_list_in_place(fb);
    notify_selection(fb);
}

/* A single touch-list row, shared geometry/style with every other row list
 * in the app (LIST_ROW_* in screen_builders.h). `icon_asset` is NULL for a
 * plain file (just an indented label); directories and playlists each get
 * their own real icon. In selection mode entry rows end in a check box. */
static lv_obj_t * add_file_row(file_browser_t * fb, const char * label_text, const char * icon_asset,
                               lv_event_cb_t cb, void * user_data, bool check_box, bool checked) {
    lv_obj_t * row = lv_obj_create(fb->list);
    /* Files uses the same font-aware ordinary row height as other native
     * lists, including the music browser. */
    lv_obj_set_size(row, LIST_ROW_WIDTH_WIDE, ui_list_row_height());
    lv_obj_add_style(row, &native_row_min_style, 0);
    lv_obj_add_style(row, &pill_row_bg_style, 0);
    lv_obj_add_style(row, &list_row_pressed_style, LV_STATE_PRESSED);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * label = lv_label_create(row);
    lv_label_set_text(label, label_text);
    lv_obj_add_style(label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(label, &LIST_ROW_FONT, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);

    int32_t check_space = check_box ? BOARD_SCALE_PX(40) : 0;
    if (icon_asset) {
        lv_obj_t * icon = lv_image_create(row);
        lv_image_set_src(icon, asset_path(icon_asset));
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 16, 0);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 72, 0);
        lv_obj_set_width(label, LIST_ROW_WIDTH_WIDE - BOARD_SCALE_PX(88) - check_space);
    } else {
        lv_obj_align(label, LV_ALIGN_LEFT_MID, LIST_ROW_LABEL_INSET, 0);
        lv_obj_set_width(label, LIST_ROW_WIDTH_WIDE - 2 * LIST_ROW_LABEL_INSET - check_space);
    }
    if (check_box)
        lv_obj_align(build_check_box(row, checked), LV_ALIGN_RIGHT_MID, -LIST_ROW_LABEL_INSET, 0);

    if (cb) {
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, user_data);
    }
    return row;
}

static void rebuild_list(file_browser_t * fb) {
    lv_obj_clean(fb->list);
    lv_label_set_text(fb->path_label, fb->current_dir);
    if (fb->entry_count < 0) {
        add_file_row(fb, atomic_load(&fb->index_worker_oversized)
                     ? TR("Folder too large to index (tap Back)")
                     : TR("Unable to read folder (tap Back and retry)"), NULL, NULL, NULL, false, false);
        return;
    }

    if (!at_root(fb)) {
        add_file_row(fb, TR("Back"), "sub_back/btn_back.png", up_click_cb, NULL, false, false);
    }

    if (fb->page_start > 0) {
        add_file_row(fb, TR("Previous"), "sub_back/btn_back.png", page_click_cb,
                     (void *) (intptr_t) -FILE_BROWSER_PAGE_SIZE, false, false);
    }

    if (fb->entry_count == 0) {
        if (fb->list_flags & LIST_ALL_FILES)
            build_list_message(fb->list, TR("This folder is empty"), NULL);
        else
            build_list_message(fb->list,
                               !at_root(fb)
                                   ? TR("No playable files here")
                                   : TR("No playable audio files found"),
                               TR("Open a folder containing supported audio files."));
        return;
    }

    int page_end = fb->page_start + FILE_BROWSER_PAGE_SIZE;
    if (page_end > fb->entry_count) page_end = fb->entry_count;
    for (int i = fb->page_start; i < page_end; i++) {
        const char * icon_asset = NULL;
        dir_entry_t *entry = &fb->entries[i - fb->page_start];
        if (entry->is_dir) icon_asset = "touch_list/list_folder.png";
        else if (entry->is_playlist) icon_asset = "sub_back/btn_playlist.png";
        /* No dedicated cue-sheet icon asset exists in this theme -- reuses
         * the playlist one, the closest existing match semantically (both
         * represent "tap to see a list of tracks", not a single song). */
        else if (entry->is_cue) icon_asset = "sub_back/btn_playlist.png";
        lv_obj_t * row = add_file_row(fb, entry->name, icon_asset, entry_click_cb, (void *) (intptr_t) i,
                                      fb->selecting, is_selected(fb, (unsigned) i));
        if (fb->callbacks.on_selection)
            lv_obj_add_event_cb(row, entry_long_press_cb, LV_EVENT_LONG_PRESSED, (void *) (intptr_t) i);
    }

    if (page_end < fb->entry_count) {
        add_file_row(fb, TR("Next"), "playing_plane/btn_next.png", page_click_cb,
                     (void *) (intptr_t) FILE_BROWSER_PAGE_SIZE, false, false);
    }
}

/* Maximum directory recursion depth when walking song directories. */
#define SCAN_ALL_SONGS_MAX_DEPTH 64


/* Bounded-memory variant used by the database scanner. Does not sort:
 * ordering belongs in the on-disk DB, not in the discovery pass.
 * False means the scan is void. Unreadable entries and subtrees are stepped
 * over and counted in *skipped, which marks the walk incomplete. add_files is
 * inherited from the parent and flipped by database.ignore/database.unignore;
 * subdirectories are descended either way, so a nested database.unignore can
 * re-include a subtree. */
static bool walk_all_songs_recursive(const char * dir_path, file_browser_song_visit_cb_t cb, void * user,
                                     int * count, int depth, atomic_int * progress,
                                     const char * excluded_top_level_dir, int * skipped, bool add_files) {
    if (depth > SCAN_ALL_SONGS_MAX_DEPTH) { (*skipped)++; return true; }

    DIR * dir = opendir(dir_path);
    if (!dir) { (*skipped)++; return true; }

    /* One path buffer for the whole frame: this function recurses to
     * SCAN_ALL_SONGS_MAX_DEPTH on a fixed-size thread stack. */
    char path_buf[PATH_MAX];
    int probe_len = snprintf(path_buf, sizeof(path_buf), "%s/database.ignore", dir_path);
    bool has_ignore = (probe_len > 0 && (size_t) probe_len < sizeof(path_buf) && access(path_buf, F_OK) == 0);

    probe_len = snprintf(path_buf, sizeof(path_buf), "%s/database.unignore", dir_path);
    bool has_unignore = (probe_len > 0 && (size_t) probe_len < sizeof(path_buf) && access(path_buf, F_OK) == 0);

    if (has_ignore != has_unignore) add_files = has_unignore;

    bool fatal = false;
    struct dirent * de;
    for (;;) {
        errno = 0;
        de = readdir(dir);
        if (!de) {
            /* NULL is end-of-directory or a read error; errno separates them.
             * An error hides the rest of this directory, so it counts as an
             * omission rather than a clean end. */
            if (errno) (*skipped)++;
            break;
        }
        if (de->d_name[0] == '.') continue;

        int length = snprintf(path_buf, sizeof(path_buf), "%s/%s", dir_path, de->d_name);
        if (length < 0 || (size_t) length >= sizeof(path_buf)) { (*skipped)++; continue; }

        struct stat st;
        bool stat_ok = lstat(path_buf, &st) == 0;
        if (progress) atomic_fetch_add_explicit(progress, 1, memory_order_relaxed);
        if (!stat_ok) { (*skipped)++; continue; }
        /* Reject symlinks to prevent path traversal outside the music root. */
        if (S_ISLNK(st.st_mode)) continue;

        if (S_ISDIR(st.st_mode)) {
            /* Skip the excluded top-level directory before recursion.
             * depth==0 ensures subdirectories with the same name deeper
             * in the tree are still discovered. */
            if (depth == 0 && excluded_top_level_dir &&
                strcasecmp(de->d_name, excluded_top_level_dir) == 0)
                continue;
            if (!walk_all_songs_recursive(path_buf, cb, user, count, depth + 1, progress,
                                          excluded_top_level_dir, skipped, add_files)) {
                fatal = true;
                break;
            }
            continue;
        }
        if (!file_browser_is_playable_name(de->d_name)) continue;

        if (add_files) {
            (*count)++;
            if (cb && !cb(path_buf, user)) { fatal = true; break; }
        }
    }

    closedir(dir);
    return !fatal;
}

bool file_browser_walk_all_songs_excluding_top_level(const char * root, const char * excluded_dir,
                                                     file_browser_song_visit_cb_t cb, void * user,
                                                     int * out_count, atomic_int * progress,
                                                     int * out_skipped) {
    int count = 0;
    int skipped = 0;
    /* A root that cannot be opened is a void scan, not an empty library. */
    DIR * root_dir = opendir(root);
    if (!root_dir) {
        if (out_count) *out_count = 0;
        if (out_skipped) *out_skipped = 0;
        return false;
    }
    closedir(root_dir);
    bool completed = walk_all_songs_recursive(root, cb, user, &count, 0, progress, excluded_dir, &skipped, true);
    if (out_count) *out_count = count;
    if (out_skipped) *out_skipped = skipped;
    return completed;
}

file_browser_t * file_browser_create(lv_obj_t * parent, const char * root,
                                     const file_browser_options_t * options) {
    file_browser_t * fb = calloc(1, sizeof(*fb));
    if (!fb) return NULL;
    fb->screen = parent;
    fb->callbacks = *options;
    fb->list_flags = (options->on_cue_select ? LIST_CUE : 0u) | (options->on_open_other ? LIST_ALL_FILES : 0u);
    snprintf(fb->root_dir, sizeof(fb->root_dir), "%s", root);
    snprintf(fb->current_dir, sizeof(fb->current_dir), "%s", root);
    fb->next = browsers;
    browsers = fb;
    lv_obj_add_event_cb(parent, browser_delete_cb, LV_EVENT_DELETE, fb);

    fb->path_label = lv_label_create(parent);
    lv_obj_set_style_text_color(fb->path_label, lv_color_make(180, 180, 180), 0);
    lv_obj_set_width(fb->path_label, lv_display_get_horizontal_resolution(lv_display_get_default()) -
                                     BOARD_SCALE_PX(20));
    lv_label_set_long_mode(fb->path_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(fb->path_label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_align(fb->path_label, LV_ALIGN_TOP_LEFT, 10, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + 4);
    lv_label_set_text(fb->path_label, fb->current_dir);

    /* Plain flex-column container, not lv_list -- rows are hand-built pill
     * shapes (add_file_row), not lv_list's own button/text item API. */
    lv_obj_t * list = lv_obj_create(parent);
    fb->list = list;
    lv_obj_set_user_data(list, fb);
    lv_obj_set_size(list, lv_pct(100),
                    lv_display_get_vertical_resolution(lv_display_get_default()) - STATUS_BAR_CLEARANCE -
                        TITLE_ROW_HEIGHT - (lv_font_get_line_height(gui_theme_font(GUI_FONT_ROLE_SUBTEXT)) +
                        BOARD_SCALE_PX(10)) - HOME_INDICATOR_CONTENT_INSET);
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, -HOME_INDICATOR_CONTENT_INSET);
    lv_obj_set_style_bg_opa(list, 0, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    /* Vertical-only scrolling so horizontal back-swipe gestures can escalate
     * to LV_EVENT_GESTURE instead of being consumed as scroll events. */
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_width(list, BOARD_SCALE_PX(3), LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(list, BOARD_SCALE_PX(2), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(list, LV_OPA_50, LV_PART_SCROLLBAR);
    lv_obj_add_style(list, gui_theme_accent_style(), LV_PART_SCROLLBAR);
    /* Clear default theme padding so rows center properly without edge clipping. */
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(list, GUI_ROW_GAP, 0);
    lv_obj_set_style_pad_top(list, GUI_ROW_GAP, 0);
    lv_obj_set_style_pad_bottom(list, BOARD_SCALE_PX(8), 0);
    /* Rows follow the live display width. Explicit cross-axis centering also
     * keeps this correct if a future parent is narrower than the display. */
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);

    fb->index_poll_timer = lv_timer_create(index_poll_cb, 100, fb);
    scan_current_dir(fb);
    rebuild_list(fb);
    return fb;
}

/* Resets each browser to root and refreshes the directory listing on SD card
 * hotplug events (mount/unmount) to avoid displaying stale or removed files. */
void file_browser_reset_to_root(void) {
    for (file_browser_t * fb = browsers; fb; fb = fb->next) {
        snprintf(fb->current_dir, sizeof(fb->current_dir), "%s", fb->root_dir);
        fb->page_start = 0;
        fb->position_depth = fb->position_overflow = 0;
        fb->restore_pending = false;
        scan_current_dir(fb);
        rebuild_list(fb);
    }
}

void file_browser_refresh(file_browser_t * fb) {
    int page_start = fb->page_start;
    int32_t scroll_y = lv_obj_get_scroll_y(fb->list);
    scan_current_dir(fb);
    fb->page_start = page_start;
    fb->restore_pending = true;
    fb->restore_scroll_y = scroll_y;
    fb->restore_generation = fb->index_request_generation;
    rebuild_list(fb);
}

bool file_browser_open_dir(file_browser_t * fb, const char * dir) {
    size_t root_len = strlen(fb->root_dir);
    size_t len = strlen(dir);
    while (len > root_len && dir[len - 1] == '/') len--;
    struct stat st;
    char path[PATH_MAX];
    if (len >= sizeof(path)) return false;
    memcpy(path, dir, len);
    path[len] = '\0';
    if (!file_op_path_in_root(path, fb->root_dir) || stat(path, &st) != 0 || !S_ISDIR(st.st_mode))
        return false;
    memcpy(fb->current_dir, path, len + 1);
    fb->page_start = 0;
    fb->position_depth = fb->position_overflow = 0;
    fb->restore_pending = false;
    scan_current_dir(fb);
    rebuild_list(fb);
    return true;
}

void file_browser_refresh_all(void) {
    for (file_browser_t * fb = browsers; fb; fb = fb->next) file_browser_refresh(fb);
}

const char * file_browser_current_dir(const file_browser_t * fb) {
    return fb->current_dir;
}

void file_browser_set_show_hidden(file_browser_t * fb, bool show) {
    if (show == file_browser_shows_hidden(fb)) return;
    fb->list_flags ^= LIST_HIDDEN;
    file_browser_refresh(fb);
}

bool file_browser_shows_hidden(const file_browser_t * fb) {
    return fb->list_flags & LIST_HIDDEN;
}

bool file_browser_is_selecting(const file_browser_t * fb) {
    return fb->selecting;
}

void file_browser_set_selecting(file_browser_t * fb, bool selecting) {
    if (!fb->selected) selecting = false;
    if (!selecting && fb->selected) {
        memset(fb->selected, 0, ((size_t) listed_count(fb) + 7) / 8);
        fb->selected_count = 0;
    }
    if (fb->selecting == selecting) return;
    fb->selecting = selecting;
    rebuild_list_in_place(fb);
    notify_selection(fb);
}

void file_browser_select_all(file_browser_t * fb) {
    if (!fb->selected) return;
    for (unsigned i = 0; i < listed_count(fb); i++) set_selected(fb, i, true);
    fb->selecting = true;
    rebuild_list_in_place(fb);
    notify_selection(fb);
}

unsigned file_browser_selected_count(const file_browser_t * fb) {
    return fb->selected_count;
}

bool file_browser_selected_paths(const file_browser_t * fb, char *** out_paths, int * out_count) {
    *out_paths = NULL;
    *out_count = 0;
    if (!fb->selected_count) return false;
    char ** paths = calloc(fb->selected_count, sizeof(*paths));
    if (!paths) return false;
    int count = 0;
    for (unsigned i = 0; i < listed_count(fb) && count < (int) fb->selected_count; i++) {
        if (!is_selected(fb, i)) continue;
        char path[PATH_MAX];
        if (!file_browser_index_path_at(fb->current_index, i, path, sizeof(path)) ||
            !(paths[count] = strdup(path))) {
            for (int j = 0; j < count; j++) free(paths[j]);
            free(paths);
            return false;
        }
        count++;
    }
    *out_paths = paths;
    *out_count = count;
    return true;
}

const char * file_browser_get_last_selected_dir(void) {
    return last_selected_dir;
}

int file_browser_get_last_selected_row(void) {
    return last_selected_row;
}


bool file_browser_build_playlist_for_path(const char * path, char *** out_playlist, int * out_count, int * out_selected_index) {
    const char * slash = strrchr(path, '/');
    if (!slash) return false;

    char dir_path[PATH_MAX];
    size_t dir_len = (size_t) (slash - path);
    if (dir_len >= sizeof(dir_path)) dir_len = sizeof(dir_path) - 1;
    memcpy(dir_path, path, dir_len);
    dir_path[dir_len] = '\0';

    dir_entry_t * scanned = NULL;
    int scanned_count = scan_directory(dir_path, &scanned, sort_mode_snapshot(), LIST_CUE);
    if (scanned_count < 0) return false;

    char ** playlist = malloc(sizeof(char *) * (size_t) (scanned_count > 0 ? scanned_count : 1));
    if (!playlist) {
        free(scanned);
        return false;
    }
    int count = 0;
    int selected = -1;

    for (int i = 0; i < scanned_count; i++) {
        if (scanned[i].is_dir || scanned[i].is_playlist || scanned[i].is_cue) continue;

        char full_path[PATH_MAX];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, scanned[i].name);
        if (strcmp(full_path, path) == 0) selected = count;

        playlist[count] = strdup(full_path);
        if (!playlist[count]) {
            for (int j = 0; j < count; j++) free(playlist[j]);
            free(playlist);
            free(scanned);
            return false;
        }
        count++;
    }

    free(scanned);

    if (selected < 0) {
        for (int i = 0; i < count; i++) free(playlist[i]);
        free(playlist);
        return false;
    }

    *out_playlist = playlist;
    *out_count = count;
    *out_selected_index = selected;
    return true;
}

bool file_browser_open_lazy_directory(const char *track_path, file_browser_index_t **out_index,
                                      unsigned *out_playable, unsigned *out_selected) {
    if (out_index) *out_index = NULL;
    if (out_playable) *out_playable = 0;
    if (out_selected) *out_selected = 0;
    if (!track_path || !out_index || !out_playable || !out_selected) return false;
    const char *slash = strrchr(track_path, '/');
    if (!slash || slash == track_path) return false;
    char dir_path[PATH_MAX];
    size_t dir_len = (size_t) (slash - track_path);
    if (dir_len >= sizeof(dir_path)) return false;
    memcpy(dir_path, track_path, dir_len);
    dir_path[dir_len] = '\0';
    file_browser_index_t *index = NULL;
    unsigned count = 0;
    if (!file_browser_index_open(dir_path, &index, &count)) return false;
    unsigned playable = file_browser_index_playable_count(index);
    for (unsigned i = 0; i < playable; i++) {
        char path[PATH_MAX];
        if (!file_browser_index_playable_path_at(index, i, path, sizeof(path))) {
            file_browser_index_close(index);
            return false;
        }
        if (strcmp(path, track_path) == 0) {
            *out_index = index;
            *out_playable = playable;
            *out_selected = i;
            return true;
        }
    }
    file_browser_index_close(index);
    return false;
}
