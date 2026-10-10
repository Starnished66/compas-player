#include "file_ops.h"
#include "file_browser.h" /* file_browser_is_playable_name() */
#include "library_endian.h" /* library_is_m3u_file() */
#include "usb_mode_control.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define COPY_BUFFER_SIZE (64 * 1024)
/* Flush each copied file every few MiB. Unflushed writes to the slow SD
 * card pile up as page cache under writeback, which has OOM-killed the
 * player on this 56 MiB device; flushing keeps that bounded. */
#define COPY_SYNC_BYTES (4 * 1024 * 1024)
#define MAX_TREE_DEPTH 64
/* Tree walks keep their path buffers on the heap, so this is ample. */
#ifndef WORKER_STACK_SIZE
#define WORKER_STACK_SIZE (256 * 1024)
#endif
#define MAX_NAME_SUFFIX 999

typedef struct {
    file_op_kind_t kind;
    char ** paths;
    int count;
    char dest_dir[PATH_MAX];
} file_op_job_t;

static pthread_mutex_t status_mu = PTHREAD_MUTEX_INITIALIZER;
static file_op_status_t status;
static atomic_bool cancel_requested;

static bool cancelled(void) {
    return atomic_load(&cancel_requested);
}

static void set_current(const char * name) {
    pthread_mutex_lock(&status_mu);
    snprintf(status.current, sizeof(status.current), "%s", name);
    pthread_mutex_unlock(&status_mu);
}

static const char * base_name(const char * path) {
    const char * slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static bool join_path(char * out, size_t size, const char * dir, const char * name) {
    int n = snprintf(out, size, "%s/%s", dir, name);
    return n > 0 && (size_t) n < size;
}

/* True when path is dir or inside it. */
static bool path_within(const char * path, const char * dir) {
    size_t len = strlen(dir);
    /* SD cards use FAT, whose path lookup is case-insensitive. Treat case
     * variants as the same path here so a copy cannot descend into itself. */
    return strncasecmp(path, dir, len) == 0 && (path[len] == '\0' || path[len] == '/');
}

/* dest_dir/name, or "stem (2).ext", "stem (3).ext"... when taken. Folders
 * keep any dot in their name. */
static bool free_dest_path(char * out, size_t size, const char * dest_dir, const char * name, bool is_dir) {
    if (!join_path(out, size, dest_dir, name)) return false;
    struct stat st;
    if (lstat(out, &st) != 0) return errno == ENOENT;
    const char * dot = is_dir ? NULL : strrchr(name, '.');
    if (dot == name) dot = NULL; /* ".hidden" has no extension */
    int stem_len = dot ? (int) (dot - name) : (int) strlen(name);
    for (int n = 2; n <= MAX_NAME_SUFFIX; n++) {
        int written = snprintf(out, size, "%s/%.*s (%d)%s", dest_dir, stem_len, name, n, dot ? dot : "");
        if (written <= 0 || (size_t) written >= size) return false;
        if (lstat(out, &st) != 0) return errno == ENOENT;
    }
    return false;
}

static bool write_all(int fd, const char * data, size_t size) {
    while (size) {
        ssize_t n = write(fd, data, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        data += n;
        size -= (size_t) n;
    }
    return true;
}

static bool copy_file(const char * src, const char * dst, const struct stat * src_st, char * buffer) {
    int in = open(src, O_RDONLY);
    if (in < 0) return errno == ENOENT; /* gone since it was listed */
    int out = open(dst, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (out < 0) { close(in); return false; }
    bool ok = true;
    size_t unsynced = 0;
    for (;;) {
        if (cancelled()) { ok = false; break; }
        ssize_t n = read(in, buffer, COPY_BUFFER_SIZE);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { ok = false; break; }
        if (n == 0) break;
        if (!write_all(out, buffer, (size_t) n)) { ok = false; break; }
        unsynced += (size_t) n;
        if (unsynced >= COPY_SYNC_BYTES) {
            if (fdatasync(out) != 0) { ok = false; break; }
            unsynced = 0;
        }
    }
    close(in);
    if (ok) {
        /* Keep the modification time so "Newest first" sorting still holds. */
        const struct timespec times[2] = { src_st->st_atim, src_st->st_mtim };
        futimens(out, times);
        ok = fdatasync(out) == 0;
    }
    if (close(out) != 0) ok = false;
    if (!ok) unlink(dst);
    return ok;
}

/* The tree walks extend one PATH_MAX heap buffer per path with "/name" and
 * cut it back afterwards, so each level of a deep tree costs a few bytes of
 * the worker's stack instead of a path buffer. Returns the length to cut
 * back to, or SIZE_MAX (path unchanged) when the name does not fit. */
static size_t path_push(char * path, const char * name) {
    size_t len = strlen(path);
    int n = snprintf(path + len, PATH_MAX - len, "/%s", name);
    if (n <= 0 || (size_t) n >= PATH_MAX - len) {
        path[len] = '\0';
        return SIZE_MAX;
    }
    return len;
}

static bool is_dot_entry(const char * name) {
    return strcmp(name, ".") == 0 || strcmp(name, "..") == 0;
}

/* path is a PATH_MAX buffer, restored before returning. */
static bool remove_tree(char * path, int depth) {
    if (cancelled() || depth > MAX_TREE_DEPTH) return false;
    struct stat st;
    if (lstat(path, &st) != 0) return errno == ENOENT;
    if (!S_ISDIR(st.st_mode)) return unlink(path) == 0 || errno == ENOENT;
    DIR * dir = opendir(path);
    if (!dir) return false;
    bool ok = true;
    struct dirent * de;
    for (;;) {
        errno = 0;
        de = readdir(dir);
        if (!de) {
            if (errno != 0) ok = false;
            break;
        }
        if (is_dot_entry(de->d_name)) continue;
        size_t len = path_push(path, de->d_name);
        bool child_ok = len != SIZE_MAX && remove_tree(path, depth + 1);
        if (len != SIZE_MAX) path[len] = '\0';
        if (!child_ok) {
            ok = false;
            if (cancelled()) break;
        }
    }
    if (closedir(dir) != 0) ok = false;
    return ok && rmdir(path) == 0;
}

/* src and dst are PATH_MAX buffers, restored before returning; dst must not
 * exist yet. A failed folder copy keeps what was copied. */
static bool copy_tree(char * src, char * dst, int depth, char * buffer) {
    if (cancelled() || depth > MAX_TREE_DEPTH) return false;
    struct stat st;
    if (lstat(src, &st) != 0) return errno == ENOENT; /* gone since it was listed */
    if (S_ISREG(st.st_mode)) return copy_file(src, dst, &st, buffer);
    if (!S_ISDIR(st.st_mode)) return false; /* links and devices are not copied */
    if (mkdir(dst, 0777) != 0) return false;
    DIR * dir = opendir(src);
    if (!dir) return false;
    bool ok = true;
    struct dirent * de;
    for (;;) {
        errno = 0;
        de = readdir(dir);
        if (!de) {
            if (errno != 0) ok = false;
            break;
        }
        if (is_dot_entry(de->d_name)) continue;
        size_t src_len = path_push(src, de->d_name);
        size_t dst_len = src_len == SIZE_MAX ? SIZE_MAX : path_push(dst, de->d_name);
        bool child_ok = dst_len != SIZE_MAX && copy_tree(src, dst, depth + 1, buffer);
        if (src_len != SIZE_MAX) src[src_len] = '\0';
        if (dst_len != SIZE_MAX) dst[dst_len] = '\0';
        if (!child_ok) {
            ok = false;
            if (cancelled()) break;
        }
    }
    if (closedir(dir) != 0) ok = false;
    return ok;
}

typedef struct {
    char src[PATH_MAX];
    char dst[PATH_MAX];
    char data[COPY_BUFFER_SIZE];
} work_buffers_t;

static bool touches_library(const char * path, const struct stat * st) {
    return S_ISDIR(st->st_mode) || file_browser_is_playable_name(path) || library_is_m3u_file(path);
}

static bool run_one(const file_op_job_t * job, const char * path, work_buffers_t * work) {
    struct stat st;
    if (lstat(path, &st) != 0) return errno == ENOENT && job->kind == FILE_OP_DELETE;
    if (touches_library(path, &st)) {
        pthread_mutex_lock(&status_mu);
        status.touched_library = true;
        pthread_mutex_unlock(&status_mu);
    }
    snprintf(work->src, sizeof(work->src), "%s", path);
    if (job->kind == FILE_OP_DELETE) return remove_tree(work->src, 0);

    /* A folder cannot go inside itself. */
    if (S_ISDIR(st.st_mode) && path_within(job->dest_dir, path)) return false;
    if (job->kind == FILE_OP_MOVE) {
        /* Moving into the folder it is already in changes nothing. */
        size_t parent_len = (size_t) (base_name(path) - path) - 1;
        if (strlen(job->dest_dir) == parent_len && strncmp(path, job->dest_dir, parent_len) == 0) return true;
    }
    if (!free_dest_path(work->dst, sizeof(work->dst), job->dest_dir, base_name(path), S_ISDIR(st.st_mode)))
        return false;
    if (job->kind == FILE_OP_COPY) return copy_tree(work->src, work->dst, 0, work->data);
    if (rename(path, work->dst) == 0) return true;
    /* Another filesystem: copy, then delete the original only if the copy
     * is complete. */
    return errno == EXDEV && copy_tree(work->src, work->dst, 0, work->data) && remove_tree(work->src, 0);
}

static void * file_op_worker(void * arg) {
    file_op_job_t * job = arg;
    work_buffers_t * work = malloc(sizeof(*work));
    /* Keeps USB mass storage from exposing the card while it is written. */
    bool leased = usb_mode_control_storage_write_begin();
    for (int i = 0; i < job->count; i++) {
        bool ok = false;
        if (work && leased && !cancelled()) {
            set_current(base_name(job->paths[i]));
            ok = run_one(job, job->paths[i], work);
        }
        pthread_mutex_lock(&status_mu);
        status.done++;
        if (!ok) status.failed++;
        pthread_mutex_unlock(&status_mu);
    }
    if (leased) usb_mode_control_storage_write_end();
    pthread_mutex_lock(&status_mu);
    status.storage_busy = !leased;
    pthread_mutex_unlock(&status_mu);
    free(work);
    for (int i = 0; i < job->count; i++) free(job->paths[i]);
    free(job->paths);
    free(job);
    pthread_mutex_lock(&status_mu);
    status.running = false;
    status.stopped = cancelled() && status.failed;
    status.current[0] = '\0';
    pthread_mutex_unlock(&status_mu);
    return NULL;
}

bool file_op_start(file_op_kind_t kind, char * const * paths, int count, const char * dest_dir) {
    if (count <= 0) return false;
    file_op_job_t * job = calloc(1, sizeof(*job));
    if (!job) return false;
    job->kind = kind;
    job->paths = calloc((size_t) count, sizeof(*job->paths));
    snprintf(job->dest_dir, sizeof(job->dest_dir), "%s", dest_dir ? dest_dir : "");
    for (int i = 0; job->paths && i < count; i++) {
        if (!(job->paths[i] = strdup(paths[i]))) break;
        job->count++;
    }
    pthread_mutex_lock(&status_mu);
    bool busy = status.running;
    if (!busy && job->count == count) {
        unsigned id = status.id + 1;
        memset(&status, 0, sizeof(status));
        status.id = id;
        status.running = true;
        status.total = (unsigned) count;
    }
    pthread_mutex_unlock(&status_mu);
    if (busy || job->count != count) goto fail;

    atomic_store(&cancel_requested, false);
    pthread_attr_t attr;
    bool attr_ok = pthread_attr_init(&attr) == 0;
    if (attr_ok) pthread_attr_setstacksize(&attr, WORKER_STACK_SIZE);
    pthread_t thread;
    int created = pthread_create(&thread, attr_ok ? &attr : NULL, file_op_worker, job);
    if (attr_ok) pthread_attr_destroy(&attr);
    if (created != 0) {
        pthread_mutex_lock(&status_mu);
        status.running = false;
        pthread_mutex_unlock(&status_mu);
        goto fail;
    }
    pthread_detach(thread);
    return true;
fail:
    for (int i = 0; i < job->count; i++) free(job->paths[i]);
    free(job->paths);
    free(job);
    return false;
}

void file_op_get_status(file_op_status_t * out) {
    pthread_mutex_lock(&status_mu);
    *out = status;
    pthread_mutex_unlock(&status_mu);
}

bool file_op_running(void) {
    pthread_mutex_lock(&status_mu);
    bool running = status.running;
    pthread_mutex_unlock(&status_mu);
    return running;
}

void file_op_cancel(void) {
    atomic_store(&cancel_requested, true);
}

/* Names on the FAT card match case-insensitively. */
bool file_op_is_player_folder(const char * path, const char * sd_root) {
    static const char * const folders[] = { ".compas", ".plugins" };
    size_t root_len = strlen(sd_root);
    if (strncmp(path, sd_root, root_len) != 0 || path[root_len] != '/') return false;
    const char * name = path + root_len + 1;
    for (size_t i = 0; i < sizeof(folders) / sizeof(folders[0]); i++)
        if (strcasecmp(name, folders[i]) == 0) return true;
    return false;
}

bool file_op_path_in_root(const char * path, const char * root) {
    size_t root_len = strlen(root);
    if (strncmp(path, root, root_len) != 0) return false;
    if (path[root_len] == '\0') return true;
    if (path[root_len] != '/') return false;
    for (const char * part = path + root_len + 1;;) {
        const char * end = strchr(part, '/');
        size_t len = end ? (size_t) (end - part) : strlen(part);
        if (len == 0 || (len == 1 && part[0] == '.') || (len == 2 && part[0] == '.' && part[1] == '.'))
            return false;
        if (!end) return true;
        part = end + 1;
    }
}
