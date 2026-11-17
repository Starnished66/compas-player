#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define _FILE_OFFSET_BITS 64
#include "remote_import_files.h"
#include "../../../core/files/file_visibility.h"
#include "../control/remote_grow_buf.h"
#include "../control/remote_reply.h"
#include "../../../core/text/utf8_util.h"

#include "../../../hardware/usb/usb_mode_control.h"
#include "../../../ui/library/browse/gui_library.h"
#include "../../../library/browser/files/file_ops.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define IMPORT_ROOT "/data/mnt/sd_0"
#define IMPORT_PATH_MAX 1024
#define IMPORT_NAME_MAX 255
#define IMPORT_PAGE_DEFAULT 50
#define IMPORT_PAGE_MAX 100
#define IMPORT_LIST_WINDOW_MAX 1024u
#define IMPORT_BODY_BUFFER (64u * 1024u)
#define IMPORT_SYNC_BYTES (4u * 1024u * 1024u)
#define IMPORT_IDLE_SECONDS 12
#define IMPORT_TOTAL_SECONDS (6 * 60 * 60)

static pthread_mutex_t import_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t import_idle_cv = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t fileop_mu = PTHREAD_MUTEX_INITIALIZER;
static atomic_bool import_stopping = false;
static bool import_ready;
static unsigned active_requests;
static int import_root_fd = -1;
static int active_upload_fd = -1;
static bool fileop_active;
static bool request_stopped(void);

static const char *root_path(void) {
#ifdef HOST_BUILD
    return "./music";
#else
    return IMPORT_ROOT;
#endif
}

static bool mounted_root(void) {
    struct stat st;
#ifdef HOST_BUILD
    return stat(root_path(), &st) == 0 && S_ISDIR(st.st_mode);
#else
    return sd_card_root_is_mounted() && stat(root_path(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

bool remote_import_files_start(void) {
    pthread_mutex_lock(&import_mu);
    if (import_ready && import_root_fd >= 0 && !atomic_load(&import_stopping)) {
        pthread_mutex_unlock(&import_mu);
        return true;
    }
    if (!mounted_root()) {
        pthread_mutex_unlock(&import_mu);
        return false;
    }
    int fd = open(root_path(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        pthread_mutex_unlock(&import_mu);
        return false;
    }
    import_root_fd = fd;
    import_ready = true;
    atomic_store(&import_stopping, false);
    pthread_mutex_unlock(&import_mu);
    return true;
}

bool remote_import_files_is_ready(void) {
    pthread_mutex_lock(&import_mu);
    bool ready = import_ready && import_root_fd >= 0 && !atomic_load(&import_stopping);
    pthread_mutex_unlock(&import_mu);
    return ready;
}

static int root_dup(void) {
    pthread_mutex_lock(&import_mu);
    int fd = (import_ready && !atomic_load(&import_stopping) && import_root_fd >= 0)
                 ? fcntl(import_root_fd, F_DUPFD_CLOEXEC, 3) : -1;
    pthread_mutex_unlock(&import_mu);
    return fd;
}

void remote_import_files_stop(void) {
    pthread_mutex_lock(&import_mu);
    atomic_store(&import_stopping, true);
    import_ready = false;
    if (active_upload_fd >= 0) shutdown(active_upload_fd, SHUT_RDWR);
    pthread_mutex_unlock(&import_mu);

    pthread_mutex_lock(&import_mu);
    while (active_requests) pthread_cond_wait(&import_idle_cv, &import_mu);
    if (import_root_fd >= 0) close(import_root_fd);
    import_root_fd = -1;
    active_upload_fd = -1;
    pthread_mutex_unlock(&import_mu);
}

/* Ordinary hidden music remains accessible. Executable plugins and player
 * state belong to their authenticated APIs, including on case-insensitive SDs. */
static bool protected_component(const char *s, size_t n) {
    return (n == 8 && strncasecmp(s, ".plugins", n) == 0) ||
           (n == 7 && strncasecmp(s, ".compas", n) == 0);
}

static bool path_string_valid(const char *s, bool allow_empty) {
    size_t n = strlen(s);
    if (n > IMPORT_PATH_MAX || (!allow_empty && n == 0) || (n && s[n - 1] == '/')) return false;
    if (!utf8_is_valid(s, n)) return false;
    if (!n) return allow_empty;
    const char *part = s;
    for (const char *p = s;; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '\\' || (c != 0 && (c < 0x20 || c == 0x7f))) return false;
        if (c == '/' || c == 0) {
            size_t len = (size_t)(p - part);
            if (!len || len > IMPORT_NAME_MAX || protected_component(part, len) || (len == 1 && part[0] == '.') ||
                (len == 2 && part[0] == '.' && part[1] == '.')) return false;
            if (part[len - 1] == '.' || part[len - 1] == ' ') return false;
            if (c == 0) break;
            part = p + 1;
        }
    }
    return true;
}

static bool leaf_valid(const char *s) {
    size_t n = strlen(s);
    if (!n || n > IMPORT_NAME_MAX || protected_component(s, n) || (n == 1 && s[0] == '.') ||
        (n == 2 && s[0] == '.' && s[1] == '.') || s[n - 1] == '.' || s[n - 1] == ' ') return false;
    if (!utf8_is_valid(s, n)) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '/' || c == '\\' || c < 0x20 || c == 0x7f) return false;
    }
    return true;
}

static int decode_value(const char *in, char *out, size_t cap) {
    size_t n = 0;
    if (!cap) return -1;
    while (*in) {
        unsigned char c;
        if (*in == '%') {
            if (!in[1] || !in[2]) return -1;
            int hi = (in[1] >= '0' && in[1] <= '9') ? in[1]-'0' :
                     (in[1] >= 'a' && in[1] <= 'f') ? in[1]-'a'+10 :
                     (in[1] >= 'A' && in[1] <= 'F') ? in[1]-'A'+10 : -1;
            int lo = (in[2] >= '0' && in[2] <= '9') ? in[2]-'0' :
                     (in[2] >= 'a' && in[2] <= 'f') ? in[2]-'a'+10 :
                     (in[2] >= 'A' && in[2] <= 'F') ? in[2]-'A'+10 : -1;
            if (hi < 0 || lo < 0) return -1;
            c = (unsigned char)((hi << 4) | lo);
            in += 3;
        } else {
            c = (unsigned char)(*in == '+' ? ' ' : *in);
            in++;
        }
        if (!c || c < 0x20 || c == 0x7f || n + 1 >= cap) return -1;
        out[n++] = (char)c;
    }
    out[n] = '\0';
    return (int)n;
}

/* Accepts only known, unique parameters. Outputs decoded values. */
static bool query_parse(const char *path, const char *const *keys, char values[][IMPORT_PATH_MAX + 1],
                        bool *present, size_t count) {
    memset(present, 0, count * sizeof(*present));
    memset(values, 0, count * (IMPORT_PATH_MAX + 1));
    const char *q = strchr(path, '?');
    if (!q) return true;
    q++;
    if (!*q) return false;
    while (*q) {
        const char *eq = strchr(q, '='), *amp = strchr(q, '&');
        if (!eq || (amp && eq > amp)) return false;
        size_t kn = (size_t)(eq - q);
        if (!kn || kn > 32) return false;
        size_t i;
        for (i = 0; i < count; i++) if (strlen(keys[i]) == kn && memcmp(q, keys[i], kn) == 0) break;
        if (i == count || present[i]) return false;
        const char *v = eq + 1;
        size_t vn = amp ? (size_t)(amp - v) : strlen(v);
        if (vn > IMPORT_PATH_MAX * 3) return false;
        char raw[IMPORT_PATH_MAX * 3 + 1];
        memcpy(raw, v, vn); raw[vn] = '\0';
        if (decode_value(raw, values[i], IMPORT_PATH_MAX + 1) < 0) return false;
        present[i] = true;
        if (!amp) break;
        q = amp + 1;
        if (!*q) return false;
    }
    return true;
}

/* VFAT short names can alias a protected long name. Compare filesystem
 * identity as well as spelling; never guess how an 8.3 alias was generated. */
static bool unprotected_identity(int parent, const struct stat *candidate) {
    static const char *const names[] = { ".plugins", ".compas" };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        struct stat reserved;
        if (fstatat(parent, names[i], &reserved, AT_SYMLINK_NOFOLLOW) == 0) {
            if (reserved.st_dev == candidate->st_dev && reserved.st_ino == candidate->st_ino) {
                errno = EACCES;
                return false;
            }
        } else if (errno != ENOENT) return false;
    }
    return true;
}

static int open_dir_relative(int rootfd, const char *rel) {
    if (!path_string_valid(rel, true)) { errno = EINVAL; return -1; }
    if (!*rel) return openat(rootfd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    int current = fcntl(rootfd, F_DUPFD_CLOEXEC, 3);
    if (current < 0) return current;
    char part[IMPORT_NAME_MAX + 1];
    const char *p = rel;
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t n = slash ? (size_t)(slash - p) : strlen(p);
        memcpy(part, p, n); part[n] = '\0';
        int next = openat(current, part, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (next >= 0 && p == rel) {
            struct stat opened;
            if (fstat(next, &opened) != 0 || !unprotected_identity(current, &opened)) {
                int error = errno;
                close(next); close(current); errno = error;
                return -1;
            }
        }
        close(current);
        if (next < 0) return -1;
        current = next;
        if (!slash) break;
        p = slash + 1;
    }
    return current;
}

static int resolve_parent(int rootfd, const char *rel, char *leaf, size_t leafcap) {
    if (!path_string_valid(rel, false)) { errno = EINVAL; return -1; }
    const char *slash = strrchr(rel, '/');
    const char *name = slash ? slash + 1 : rel;
    if (!leaf_valid(name) || strlen(name) + 1 > leafcap) { errno = EINVAL; return -1; }
    strcpy(leaf, name);
    char parent[IMPORT_PATH_MAX + 1];
    size_t n = slash ? (size_t)(slash - rel) : 0;
    memcpy(parent, rel, n); parent[n] = '\0';
    int fd = open_dir_relative(rootfd, parent);
    if (fd >= 0 && !slash) {
        struct stat item;
        bool ok = fstatat(fd, leaf, &item, AT_SYMLINK_NOFOLLOW) == 0
            ? unprotected_identity(fd, &item) : errno == ENOENT;
        if (!ok) { int error = errno; close(fd); errno = error; return -1; }
    }
    return fd;
}

static int rename_noreplace(int olddir, const char *oldname, int newdir, const char *newname) {
#if defined(SYS_renameat2)
    int result = (int)syscall(SYS_renameat2, olddir, oldname, newdir, newname, 1u /* RENAME_NOREPLACE */);
    if (result == 0 || (errno != ENOSYS && errno != EINVAL && errno != EOPNOTSUPP && errno != ENOTSUP))
        return result;
#else
    errno = ENOSYS;
#endif

    /* Older filesystems such as the device's VFAT SD card reject
     * RENAME_NOREPLACE with EINVAL. Reserve the destination atomically,
     * then replace only that empty placeholder. The normal file-operation
     * gate and storage lease remain held by the caller across this sequence.
     * An unrelated writer which does not participate in those app-level
     * guards can still race the final rename; see the API limitation docs. */
    struct stat source, placeholder, current;
    if (fstatat(olddir, oldname, &source, AT_SYMLINK_NOFOLLOW) != 0) return -1;
    if (!S_ISREG(source.st_mode) && !S_ISDIR(source.st_mode)) { errno = EINVAL; return -1; }
    bool directory = S_ISDIR(source.st_mode);
    if (directory) {
        if (mkdirat(newdir, newname, 0700) != 0) return -1;
        if (fstatat(newdir, newname, &placeholder, AT_SYMLINK_NOFOLLOW) != 0) return -1;
        if (!S_ISDIR(placeholder.st_mode)) { errno = EIO; return -1; }
    } else {
        int fd = openat(newdir, newname, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0) return -1;
        int stat_result = fstat(fd, &placeholder);
        int stat_error = stat_result == 0 ? 0 : errno;
        bool stat_ok = stat_result == 0 && S_ISREG(placeholder.st_mode);
        if (stat_result == 0 && !stat_ok) stat_error = EIO;
        if (close(fd) != 0 && stat_ok) { stat_ok = false; stat_error = errno; }
        if (!stat_ok) {
            if (stat_result == 0 && fstatat(newdir, newname, &current, AT_SYMLINK_NOFOLLOW) == 0 &&
                current.st_dev == placeholder.st_dev && current.st_ino == placeholder.st_ino &&
                S_ISREG(current.st_mode))
                (void) unlinkat(newdir, newname, 0);
            errno = stat_error ? stat_error : EIO;
            return -1;
        }
    }

    int failure = 0;
    if (request_stopped()) failure = ECANCELED;
    else if (fstatat(newdir, newname, &current, AT_SYMLINK_NOFOLLOW) != 0) failure = errno;
    else if (current.st_dev != placeholder.st_dev || current.st_ino != placeholder.st_ino ||
             (directory ? !S_ISDIR(current.st_mode) : !S_ISREG(current.st_mode))) failure = EEXIST;
    else if (renameat(olddir, oldname, newdir, newname) != 0) failure = errno;
    if (failure != 0) {
        if (fstatat(newdir, newname, &current, AT_SYMLINK_NOFOLLOW) == 0 &&
            current.st_dev == placeholder.st_dev && current.st_ino == placeholder.st_ino &&
            (directory ? S_ISDIR(current.st_mode) : S_ISREG(current.st_mode)))
            (void) unlinkat(newdir, newname, directory ? AT_REMOVEDIR : 0);
        errno = failure;
        return -1;
    }
    return 0;
}

static bool send_all(int fd, const char *data, size_t size);

static void send_json(int fd, const char *status, const char *body) {
    char h[256];
    int n = snprintf(h, sizeof(h), "HTTP/1.1 %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n", status, strlen(body));
    if (n > 0 && send_all(fd, h, (size_t)n)) (void)send_all(fd, body, strlen(body));
}

static bool send_all(int fd, const char *data, size_t size) {
    while (size) {
        if (request_stopped()) return false;
        ssize_t n = send(fd, data, size, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        data += n; size -= (size_t)n;
    }
    return true;
}

static size_t json_escape(char *out, size_t cap, const char *s) {
    size_t n = 0;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        const char *esc = NULL;
        if (c == '"') esc = "\\\"";
        else if (c == '\\') esc = "\\\\";
        else if (c < 0x20) {
            if (n + 6 >= cap) break;
            int k = snprintf(out + n, cap - n, "\\u%04x", c); n += (size_t)k; continue;
        }
        if (esc) {
            size_t z = strlen(esc); if (n + z >= cap) break;
            memcpy(out + n, esc, z); n += z;
        } else { if (n + 1 >= cap) break; out[n++] = (char)c; }
    }
    if (cap) out[n < cap ? n : cap - 1] = '\0';
    return n;
}

static bool parse_uint(const char *s, unsigned long *out) {
    if (!*s) return false;
    unsigned long v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || v > (ULONG_MAX - (unsigned)(*s-'0')) / 10) return false;
        v = v * 10 + (unsigned)(*s-'0');
    }
    *out = v;
    return true;
}

typedef struct {
    char name[IMPORT_NAME_MAX + 1];
    bool is_directory;
    off_t size;
} import_list_entry_t;

/* Directories sort before files, then strcasecmp orders names. In the C
 * locale that folds ASCII only; there is no Unicode-aware UTF-8 casefold
 * helper here. Raw strcmp makes any folded ties deterministic. */
static int import_list_entry_compare(const void *left, const void *right) {
    const import_list_entry_t *a = left;
    const import_list_entry_t *b = right;
    if (a->is_directory != b->is_directory) return a->is_directory ? -1 : 1;
    int by_folded_name = strcasecmp(a->name, b->name);
    return by_folded_name ? by_folded_name : strcmp(a->name, b->name);
}

/* Keep the smallest `count` entries in a max heap. */
static void import_list_heap_sift_down(import_list_entry_t *heap, size_t count, size_t root) {
    for (;;) {
        if (root >= count / 2) return;
        size_t largest = root;
        size_t left = root * 2 + 1;
        size_t right = left + 1;
        if (left < count && import_list_entry_compare(&heap[left], &heap[largest]) > 0) largest = left;
        if (right < count && import_list_entry_compare(&heap[right], &heap[largest]) > 0) largest = right;
        if (largest == root) return;
        import_list_entry_t tmp = heap[root];
        heap[root] = heap[largest];
        heap[largest] = tmp;
        root = largest;
    }
}

static void import_list_heap_add(import_list_entry_t *heap, size_t *count, size_t capacity,
                                 const import_list_entry_t *entry) {
    if (*count < capacity) {
        size_t i = (*count)++;
        heap[i] = *entry;
        while (i > 0) {
            size_t parent = (i - 1) / 2;
            if (import_list_entry_compare(&heap[i], &heap[parent]) <= 0) break;
            import_list_entry_t tmp = heap[i];
            heap[i] = heap[parent];
            heap[parent] = tmp;
            i = parent;
        }
    } else if (capacity && import_list_entry_compare(entry, &heap[0]) < 0) {
        heap[0] = *entry;
        import_list_heap_sift_down(heap, *count, 0);
    }
}

/* Hard cap for the paged listing body: at most IMPORT_PAGE_MAX entries, so this only
 * guards against pathological names. */
#define IMPORT_LIST_BODY_MAX (1024u * 1024u)

/* Appends one entry and counts it in *emitted. An entry whose joined path does not fit
 * is skipped without being counted. Returns false only when the body cannot grow. */
static bool import_body_entry(grow_buf_t *b, const char *rel, const import_list_entry_t *item,
                              unsigned long *emitted) {
    char escname[IMPORT_NAME_MAX * 6 + 1], full[IMPORT_PATH_MAX + 1], escfull[IMPORT_PATH_MAX * 6 + 1];
    int pn = snprintf(full, sizeof(full), "%s%s%s", rel, *rel ? "/" : "", item->name);
    if (pn < 0 || (size_t)pn >= sizeof(full)) return true;
    json_escape(escname, sizeof(escname), item->name);
    json_escape(escfull, sizeof(escfull), full);
    if (!grow_buf_appendf(b, "%s{\"name\":\"%s\",\"path\":\"%s\",\"isDirectory\":%s,\"size\":%llu}",
                          *emitted ? "," : "", escname, escfull, item->is_directory ? "true" : "false",
                          (unsigned long long)item->size))
        return false;
    (*emitted)++;
    return true;
}

/* Builds the whole page, then replies with a Content-Length like every other route. */
static void import_list_send(int fd, const char *rel, const import_list_entry_t *entries, size_t selected,
                             unsigned long offset, unsigned long limit, unsigned long total) {
    grow_buf_t b = {.max = IMPORT_LIST_BODY_MAX};
    char escpath[IMPORT_PATH_MAX * 6 + 1];
    json_escape(escpath, sizeof(escpath), rel);
    bool ok = grow_buf_appendf(&b, "{\"path\":\"%s\",\"entries\":[", escpath);
    unsigned long emitted = 0;
    for (size_t i = 0; ok && i < selected && emitted < limit; i++) {
        if (request_stopped()) { free(b.data); return; }
        ok = import_body_entry(&b, rel, &entries[i], &emitted);
    }
    if (ok && offset < total && limit < total - offset)
        ok = grow_buf_appendf(&b, "],\"offset\":%lu,\"next_offset\":%lu,\"total\":%lu}", offset, offset + limit, total);
    else if (ok)
        ok = grow_buf_appendf(&b, "],\"offset\":%lu,\"next_offset\":null,\"total\":%lu}", offset, total);
    if (ok) send_json(fd, "200 OK", b.data);
    else send_reason_error(fd, "500 Internal Server Error", "directory_read_failed");
    free(b.data);
}

typedef struct {
    char rel[IMPORT_PATH_MAX + 1];
    unsigned long offset, limit;
    bool show_hidden;
} import_list_request_t;

/* Parses and validates ?path&offset&limit; sends the error reply itself on failure. */
static bool import_list_parse(int fd, const char *path, import_list_request_t *req) {
    const char *keys[] = {"path", "offset", "limit", "show_hidden"};
    char values[4][IMPORT_PATH_MAX + 1]; bool have[4];
    if (!query_parse(path, keys, values, have, 4) || !have[0]) {
        send_reason_error(fd, "400 Bad Request", "invalid_query"); return false;
    }
    if (!path_string_valid(values[0], true)) { send_reason_error(fd, "400 Bad Request", "invalid_path"); return false; }
    snprintf(req->rel, sizeof(req->rel), "%s", values[0]);
    req->offset = 0; req->limit = IMPORT_PAGE_DEFAULT;
    if ((have[3] && strcmp(values[3], "0") != 0 && strcmp(values[3], "1") != 0) ||
        (have[1] && !parse_uint(values[1], &req->offset)) ||
        (have[2] && !parse_uint(values[2], &req->limit)) || req->limit == 0) {
        send_reason_error(fd, "400 Bad Request", "invalid_paging"); return false;
    }
    req->show_hidden = have[3] && strcmp(values[3], "1") == 0;
    if (req->limit > IMPORT_PAGE_MAX) req->limit = IMPORT_PAGE_MAX;
    return true;
}

/* True for a visible regular file or directory the listing should show. */
static bool import_list_visible(int dirfd, const struct dirent *de, struct stat *st, bool show_hidden) {
    if (file_name_is_hidden(de->d_name, show_hidden) || !leaf_valid(de->d_name)) return false;
    return fstatat(dirfd, de->d_name, st, AT_SYMLINK_NOFOLLOW) == 0 &&
           (S_ISDIR(st->st_mode) || S_ISREG(st->st_mode));
}

static unsigned long import_list_count(DIR *dir, int dirfd, bool show_hidden) {
    unsigned long total = 0;
    struct dirent *de;
    struct stat st;
    while (!request_stopped() && (de = readdir(dir)))
        if (import_list_visible(dirfd, de, &st, show_hidden)) total++;
    return total;
}

/* Collects the next sorted window of entries after `boundary` into `window`. Returns false if stopped. */
static bool import_list_fill_window(DIR *dir, int dirfd, import_list_entry_t *window, size_t *count,
                                    const import_list_entry_t *boundary, bool show_hidden) {
    struct dirent *de;
    struct stat st;
    *count = 0;
    rewinddir(dir);
    while ((de = readdir(dir))) {
        if (request_stopped()) return false;
        if (!import_list_visible(dirfd, de, &st, show_hidden)) continue;
        import_list_entry_t candidate;
        strcpy(candidate.name, de->d_name);
        candidate.is_directory = S_ISDIR(st.st_mode);
        candidate.size = S_ISREG(st.st_mode) ? st.st_size : 0;
        if (boundary && import_list_entry_compare(&candidate, boundary) <= 0) continue;
        import_list_heap_add(window, count, IMPORT_LIST_WINDOW_MAX, &candidate);
    }
    if (*count > 1) qsort(window, *count, sizeof(*window), import_list_entry_compare);
    return !request_stopped();
}

/* Selects the entries [offset, offset+limit) in sorted order, window by window. False if stopped. */
static bool import_list_select(DIR *dir, int dirfd, unsigned long offset, unsigned long limit,
                               import_list_entry_t *window, import_list_entry_t *entries, size_t *selected,
                               bool show_hidden) {
    unsigned long skip = offset;
    import_list_entry_t boundary;
    bool have_boundary = false;
    *selected = 0;
    while (*selected < limit) {
        size_t count;
        if (request_stopped() || !import_list_fill_window(dir, dirfd, window, &count,
                                                          have_boundary ? &boundary : NULL, show_hidden))
            return false;
        if (!count) break;
        size_t start = skip >= count ? count : (size_t)skip;
        skip -= start;
        for (size_t i = start; i < count && *selected < limit; i++) entries[(*selected)++] = window[i];
        if (*selected == limit || count < IMPORT_LIST_WINDOW_MAX) break;
        boundary = window[count - 1];
        have_boundary = true;
    }
    return true;
}

static DIR *import_list_open(int fd, const char *rel) {
    int root = root_dup();
    if (root < 0) { send_reason_error(fd, "409 Conflict", "import_not_ready"); return NULL; }
    int dirfd = open_dir_relative(root, rel); close(root);
    if (dirfd < 0) { send_reason_error(fd, errno == ENOENT ? "404 Not Found" : "400 Bad Request", "directory_not_found"); return NULL; }
    DIR *dir = fdopendir(dirfd);
    if (!dir) { close(dirfd); send_reason_error(fd, "500 Internal Server Error", "directory_read_failed"); }
    return dir;
}

static bool handle_list(int fd, const char *path) {
    import_list_request_t req;
    if (!import_list_parse(fd, path, &req)) return true;
    DIR *dir = import_list_open(fd, req.rel);
    if (!dir) return true;
    int dfd = dirfd(dir);
    unsigned long total = import_list_count(dir, dfd, req.show_hidden);
    if (request_stopped()) { closedir(dir); return true; }
    import_list_entry_t *window = NULL, *entries = NULL;
    size_t selected = 0;
    bool ok = true;
    if (req.offset < total) {
        window = malloc(IMPORT_LIST_WINDOW_MAX * sizeof(*window));
        entries = malloc((size_t)req.limit * sizeof(*entries));
        if (!window || !entries) {
            free(window); free(entries); closedir(dir);
            send_reason_error(fd, "500 Internal Server Error", "directory_read_failed"); return true;
        }
        ok = import_list_select(dir, dfd, req.offset, req.limit, window, entries, &selected, req.show_hidden);
    }
    free(window);
    closedir(dir);
    if (ok) import_list_send(fd, req.rel, entries, selected, req.offset, req.limit, total);
    free(entries);
    return true;
}

static bool filename_attr_char(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || strchr("!#$&+-.^_`|~", c) != NULL;
}

static bool handle_download(int fd, const char *path) {
    const char *keys[] = {"path"};
    char values[1][IMPORT_PATH_MAX + 1];
    bool have[1];
    if (!query_parse(path, keys, values, have, 1) || !have[0] ||
        !path_string_valid(values[0], false)) {
        send_reason_error(fd, "400 Bad Request", "invalid_path");
        return true;
    }

    int root = root_dup();
    if (root < 0) {
        send_reason_error(fd, "409 Conflict", "import_not_ready");
        return true;
    }
    char leaf[IMPORT_NAME_MAX + 1];
    int parent = resolve_parent(root, values[0], leaf, sizeof(leaf));
    close(root);
    if (parent < 0) {
        send_reason_error(fd, "404 Not Found", "item_not_found");
        return true;
    }
    int file = openat(parent, leaf, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    close(parent);
    if (file < 0) {
        send_reason_error(fd, errno == ENOENT ? "404 Not Found" : "400 Bad Request",
                          errno == ENOENT ? "item_not_found" : "download_failed");
        return true;
    }
    struct stat st;
    if (fstat(file, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
        close(file);
        send_reason_error(fd, "400 Bad Request", "download_failed");
        return true;
    }

    char disposition[IMPORT_NAME_MAX * 3 + 64];
    size_t used = (size_t)snprintf(disposition, sizeof(disposition),
                                   "Content-Disposition: attachment; filename*=UTF-8''");
    for (const unsigned char *p = (const unsigned char *)leaf; *p; p++) {
        int n = filename_attr_char(*p)
                    ? snprintf(disposition + used, sizeof(disposition) - used, "%c", *p)
                    : snprintf(disposition + used, sizeof(disposition) - used, "%%%02X", *p);
        if (n < 0 || (size_t)n >= sizeof(disposition) - used) {
            close(file);
            send_reason_error(fd, "400 Bad Request", "invalid_path");
            return true;
        }
        used += (size_t)n;
    }
    char header[IMPORT_NAME_MAX * 3 + 256];
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
        "Content-Length: %llu\r\n%s\r\nX-Content-Type-Options: nosniff\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
        (unsigned long long)st.st_size, disposition);
    bool ok = header_len > 0 && (size_t)header_len < sizeof(header) &&
              send_all(fd, header, (size_t)header_len);
    char buffer[8192];
    off_t remaining = st.st_size;
    while (ok && remaining > 0 && !request_stopped()) {
        size_t chunk = remaining < (off_t)sizeof(buffer) ? (size_t)remaining : sizeof(buffer);
        ssize_t n = read(file, buffer, chunk);
        if (n < 0 && errno == EINTR) continue;
        if (n == 0) break;
        if (n < 0) { ok = false; break; }
        ok = send_all(fd, buffer, (size_t)n);
        remaining -= n;
    }
    close(file);
    return true;
}

/* Error reply that also drains the `unread` request-body bytes the client
 * still sends (negative: unknown length), so closing does not reset the
 * connection and lose the reply. */
static void refuse(int fd, const char *status, const char *reason, long long unread) {
    size_t drain = unread < 0 || (unsigned long long)unread > REPLY_DRAIN_MAX_BYTES ? REPLY_DRAIN_MAX_BYTES : (size_t)unread;
    send_api_error_drain(fd, status, reason, reason, drain);
}

typedef struct { const char *status, *reason; } verdict_t;

static bool verdict_fail(verdict_t *v, const char *status, const char *reason) {
    v->status = status; v->reason = reason;
    return false;
}

/* Takes the storage gates, or says why not. Pair with end_mutation(). */
static bool mutation_try(verdict_t *v) {
    if (pthread_mutex_trylock(&fileop_mu) != 0) return verdict_fail(v, "409 Conflict", "storage_busy");
    if (fileop_active || request_stopped() || !file_op_external_begin()) {
        pthread_mutex_unlock(&fileop_mu);
        return verdict_fail(v, "409 Conflict", request_stopped() ? "import_stopping" : "storage_busy");
    }
    fileop_active = true;
    if (request_stopped() || !mounted_root() || !usb_mode_control_storage_write_begin()) {
        file_op_external_end();
        fileop_active = false; pthread_mutex_unlock(&fileop_mu);
        return verdict_fail(v, "503 Service Unavailable", "storage_busy");
    }
    return true;
}

static bool begin_mutation(int fd, long long unread) {
    verdict_t v;
    if (mutation_try(&v)) return true;
    refuse(fd, v.status, v.reason, unread);
    return false;
}

static void end_mutation(void) {
    usb_mode_control_storage_write_end();
    file_op_external_end();
    fileop_active = false;
    pthread_mutex_unlock(&fileop_mu);
}

static bool request_stopped(void) {
    return atomic_load_explicit(&import_stopping, memory_order_acquire);
}

static bool elapsed_at_least(const struct timespec *now, const struct timespec *then, time_t seconds) {
    time_t elapsed = now->tv_sec - then->tv_sec;
    return elapsed > seconds || (elapsed == seconds && now->tv_nsec >= then->tv_nsec);
}

static bool body_read_all(int fd, uint8_t *buf, size_t size, size_t *received,
                          const struct timespec *start, struct timespec *last) {
    *received = 0;
    while (*received < size) {
        if (request_stopped()) return false;
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        if (elapsed_at_least(&now, last, IMPORT_IDLE_SECONDS) ||
            elapsed_at_least(&now, start, IMPORT_TOTAL_SECONDS)) { errno = ETIMEDOUT; return false; }
        struct pollfd pfd = {.fd=fd,.events=POLLIN};
        int wait_ms = 1000;
        int pr = poll(&pfd, 1, wait_ms);
        if (pr < 0 && errno == EINTR) continue;
        if (pr <= 0) continue;
        ssize_t n = recv(fd, buf + *received, size - *received, 0);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (n <= 0) return false;
        *received += (size_t)n;
        clock_gettime(CLOCK_MONOTONIC, last);
    }
    return true;
}

static bool remove_owned_partial(int parent, const char *name, const struct stat *own) {
    struct stat current;
    return own && fstatat(parent, name, &current, AT_SYMLINK_NOFOLLOW) == 0 &&
           own->st_dev == current.st_dev && own->st_ino == current.st_ino && unlinkat(parent, name, 0) == 0;
}

/* Request checks that need no body and no storage. */
static bool upload_params(bool parsed, const bool *have, char values[][IMPORT_PATH_MAX + 1], long long size,
                          bool wifi, verdict_t *v) {
    if (!wifi) return verdict_fail(v, "403 Forbidden", "wifi_only");
    if (size < 0) return verdict_fail(v, "411 Length Required", "length_required");
    if ((unsigned long long)size > UINT32_MAX) return verdict_fail(v, "413 Content Too Large", "file_too_large");
    if (!parsed || !have[0] || !have[1] ||
        !path_string_valid(values[0], true) || !leaf_valid(values[1]) ||
        strlen(values[0]) + (values[0][0] ? 1u : 0u) + strlen(values[1]) > IMPORT_PATH_MAX)
        return verdict_fail(v, "400 Bad Request", "invalid_path_or_name");
    return true;
}

/* Opens the target directory with the storage gates held (end_mutation()
 * releases them); on failure nothing is held. */
static int upload_open_dir(const char *dirpath, verdict_t *v) {
    int root = root_dup();
    if (root < 0) { verdict_fail(v, "409 Conflict", "import_not_ready"); return -1; }
    if (!mutation_try(v)) { close(root); return -1; }
    int dirfd = open_dir_relative(root, dirpath); close(root);
    if (dirfd < 0) { end_mutation(); verdict_fail(v, "404 Not Found", "directory_not_found"); }
    return dirfd;
}

/* GET /api/import/files/upload-check?path=&name=&size=: the refusals an
 * upload would send, answered before any body is sent. */
static bool handle_upload_check(int fd, const char *path, bool wifi) {
    const char *keys[] = {"path", "name", "size"}; char values[3][IMPORT_PATH_MAX + 1], *end = NULL; bool have[3];
    bool parsed = query_parse(path, keys, values, have, 3);
    long long size = parsed && have[2] ? strtoll(values[2], &end, 10) : -1;
    if (end && (*end || end == values[2])) size = -1;
    verdict_t v;
    int dirfd = -1;
    struct stat st;
    if (!upload_params(parsed, have, values, size, wifi, &v) || (dirfd = upload_open_dir(values[0], &v)) < 0) {
        send_reason_error(fd, v.status, v.reason);
        return true;
    }
    bool exists = fstatat(dirfd, values[1], &st, AT_SYMLINK_NOFOLLOW) == 0;
    close(dirfd); end_mutation();
    if (exists) send_reason_error(fd, "409 Conflict", "name_exists");
    else send_json(fd, "200 OK", "{\"ok\":true}");
    return true;
}

static bool handle_upload(int fd, const char *path, long long content_length, bool wifi) {
    const char *keys[] = {"path", "name"}; char values[2][IMPORT_PATH_MAX + 1]; bool have[2];
    bool parsed = query_parse(path, keys, values, have, 2);
    verdict_t v;
    int dirfd = -1;
    if (!upload_params(parsed, have, values, content_length, wifi, &v) ||
        (dirfd = upload_open_dir(values[0], &v)) < 0) {
        refuse(fd, v.status, v.reason, content_length);
        return true;
    }
    int out = openat(dirfd, values[1], O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0666);
    if (out < 0) { int err = errno; close(dirfd); end_mutation(); refuse(fd, err == EEXIST ? "409 Conflict" : "400 Bad Request", err == EEXIST ? "name_exists" : "open_failed", content_length); return true; }
    pthread_mutex_lock(&import_mu); active_upload_fd = fd; pthread_mutex_unlock(&import_mu);
    uint8_t *buffer = malloc(IMPORT_BODY_BUFFER);
    struct stat own_stat;
    bool have_own_stat = fstat(out, &own_stat) == 0;
    uint64_t remaining = (uint64_t)content_length, total = 0;
    size_t unsynced = 0; bool ok = buffer != NULL;
    struct timespec upload_start, last_activity;
    clock_gettime(CLOCK_MONOTONIC, &upload_start); last_activity = upload_start;
    while (ok && remaining) {
        size_t chunk = remaining < IMPORT_BODY_BUFFER ? (size_t)remaining : IMPORT_BODY_BUFFER;
        size_t got = 0;
        if (!body_read_all(fd, buffer, chunk, &got, &upload_start, &last_activity)) { remaining -= got; ok = false; break; }
        size_t off = 0;
        while (off < got) {
            ssize_t n = write(out, buffer + off, got - off);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { ok = false; break; }
            off += (size_t)n; total += (uint64_t)n; unsynced += (size_t)n;
            clock_gettime(CLOCK_MONOTONIC, &last_activity);
            if (unsynced >= IMPORT_SYNC_BYTES) {
                if (fdatasync(out) != 0) { ok = false; break; }
                unsynced = 0;
                clock_gettime(CLOCK_MONOTONIC, &last_activity);
            }
        }
        remaining -= got;
    }
    if (ok && (request_stopped() || fdatasync(out) != 0)) ok = false;
    free(buffer);
    pthread_mutex_lock(&import_mu); if (active_upload_fd == fd) active_upload_fd = -1; pthread_mutex_unlock(&import_mu);
    if (!ok) (void)remove_owned_partial(dirfd, values[1], have_own_stat ? &own_stat : NULL);
    if (close(out) != 0 && ok) {
        ok = false;
        (void)remove_owned_partial(dirfd, values[1], have_own_stat ? &own_stat : NULL);
    }
    close(dirfd);
    end_mutation();
    if (!ok) { refuse(fd, request_stopped() ? "503 Service Unavailable" : "400 Bad Request", request_stopped() ? "import_stopped" : "upload_failed", (long long)remaining); return true; }
    char body[IMPORT_PATH_MAX * 6 + IMPORT_NAME_MAX * 6 + 256], escpath[IMPORT_PATH_MAX * 6 + 1], escname[IMPORT_NAME_MAX * 6 + 1], full[IMPORT_PATH_MAX + 1];
    snprintf(full, sizeof(full), "%s%s%s", values[0], values[0][0] ? "/" : "", values[1]);
    json_escape(escpath, sizeof(escpath), full); json_escape(escname, sizeof(escname), values[1]);
    snprintf(body, sizeof(body), "{\"ok\":true,\"path\":\"%s\",\"name\":\"%s\",\"bytes\":%llu}", escpath, escname, (unsigned long long)total);
    send_json(fd, "201 Created", body);
    return true;
}

static bool handle_rename(int fd, const char *path) {
    const char *keys[] = {"path", "name"}; char values[2][IMPORT_PATH_MAX + 1]; bool have[2];
    if (!query_parse(path, keys, values, have, 2) || !have[0] || !have[1] ||
        !path_string_valid(values[0], false) || !leaf_valid(values[1])) { send_reason_error(fd, "400 Bad Request", "invalid_path_or_name"); return true; }
    const char *last_slash = strrchr(values[0], '/');
    size_t parent_len = last_slash ? (size_t)(last_slash - values[0]) : 0;
    if (parent_len + (parent_len ? 1u : 0u) + strlen(values[1]) > IMPORT_PATH_MAX) { send_reason_error(fd, "400 Bad Request", "invalid_path_or_name"); return true; }
    int root = root_dup(); if (root < 0) { send_reason_error(fd, "409 Conflict", "import_not_ready"); return true; }
    if (!begin_mutation(fd, 0)) { close(root); return true; }
    char oldleaf[IMPORT_NAME_MAX + 1]; int parent = resolve_parent(root, values[0], oldleaf, sizeof(oldleaf)); close(root);
    if (parent < 0) { end_mutation(); send_reason_error(fd, "404 Not Found", "item_not_found"); return true; }
    struct stat src, dst;
    bool ok = fstatat(parent, oldleaf, &src, AT_SYMLINK_NOFOLLOW) == 0 &&
              (S_ISREG(src.st_mode) || S_ISDIR(src.st_mode));
    if (ok && strcmp(oldleaf, values[1]) != 0) {
        if (fstatat(parent, values[1], &dst, AT_SYMLINK_NOFOLLOW) == 0) { errno = EEXIST; ok = false; }
        else if (errno != ENOENT || rename_noreplace(parent, oldleaf, parent, values[1]) != 0) ok = false;
    }
    int err = errno; close(parent); end_mutation();
    if (!ok) {
        bool unsupported = err == ENOTSUP || err == ENOSYS || err == EINVAL || err == EOPNOTSUPP;
        const char *status = err == ECANCELED ? "503 Service Unavailable" : err == EEXIST ? "409 Conflict" : unsupported ? "501 Not Implemented" : "400 Bad Request";
        const char *code = err == ECANCELED ? "import_stopped" : err == EEXIST ? "name_exists" : unsupported ? "rename_not_supported" : "rename_failed";
        send_reason_error(fd, status, code); return true;
    }
    send_json(fd, "200 OK", "{\"ok\":true}"); return true;
}

static bool handle_delete(int fd, const char *path) {
    const char *keys[] = {"path"}; char values[1][IMPORT_PATH_MAX + 1]; bool have[1];
    if (!query_parse(path, keys, values, have, 1) || !have[0] || !path_string_valid(values[0], false)) { send_reason_error(fd, "400 Bad Request", "invalid_path"); return true; }
    int root = root_dup(); if (root < 0) { send_reason_error(fd, "409 Conflict", "import_not_ready"); return true; }
    if (!begin_mutation(fd, 0)) { close(root); return true; }
    char leaf[IMPORT_NAME_MAX + 1]; int parent = resolve_parent(root, values[0], leaf, sizeof(leaf)); close(root);
    if (parent < 0) { end_mutation(); send_reason_error(fd, "404 Not Found", "item_not_found"); return true; }
    struct stat st;
    bool ok = fstatat(parent, leaf, &st, AT_SYMLINK_NOFOLLOW) == 0 && (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode));
    if (ok) ok = unlinkat(parent, leaf, S_ISDIR(st.st_mode) ? AT_REMOVEDIR : 0) == 0;
    int err = errno; close(parent); end_mutation();
    if (!ok) { send_reason_error(fd, err == ENOTEMPTY ? "409 Conflict" : err == ENOENT ? "404 Not Found" : "400 Bad Request", err == ENOTEMPTY ? "directory_not_empty" : err == ENOENT ? "item_not_found" : "delete_failed"); return true; }
    send_json(fd, "200 OK", "{\"ok\":true}"); return true;
}

static bool handle_mkdir(int fd, const char *path) {
    const char *keys[] = {"path", "name"}; char values[2][IMPORT_PATH_MAX + 1]; bool have[2];
    if (!query_parse(path, keys, values, have, 2) || !have[0] || !have[1] || !path_string_valid(values[0], true) || !leaf_valid(values[1])) { send_reason_error(fd, "400 Bad Request", "invalid_path_or_name"); return true; }
    if (strlen(values[0]) + (values[0][0] ? 1u : 0u) + strlen(values[1]) > IMPORT_PATH_MAX) { send_reason_error(fd, "400 Bad Request", "invalid_path_or_name"); return true; }
    int root = root_dup(); if (root < 0) { send_reason_error(fd, "409 Conflict", "import_not_ready"); return true; }
    if (!begin_mutation(fd, 0)) { close(root); return true; }
    int parent = open_dir_relative(root, values[0]); close(root);
    bool ok = parent >= 0 && mkdirat(parent, values[1], 0777) == 0;
    int err = errno; if (parent >= 0) close(parent); end_mutation();
    if (!ok) { send_reason_error(fd, err == EEXIST ? "409 Conflict" : "400 Bad Request", err == EEXIST ? "name_exists" : "mkdir_failed"); return true; }
    send_json(fd, "201 Created", "{\"ok\":true}"); return true;
}

bool remote_import_files_handle(int fd, const char *method, const char *path,
                                long long content_length, bool wifi_transport) {
    const char *q = strchr(path, '?');
    size_t n = q ? (size_t)(q - path) : strlen(path);
    char route[64];
    if (n >= sizeof(route)) return false;
    memcpy(route, path, n); route[n] = '\0';
    bool matched = (!strcmp(route, "/api/import/files") && !strcmp(method, "GET")) ||
                   (!strcmp(route, "/api/import/files/download") && !strcmp(method, "GET")) ||
                   (!strcmp(route, "/api/import/files/upload") && !strcmp(method, "POST")) ||
                   (!strcmp(route, "/api/import/files/upload-check") && !strcmp(method, "GET")) ||
                   (!strcmp(route, "/api/import/files/rename") && !strcmp(method, "POST")) ||
                   (!strcmp(route, "/api/import/files/delete") && !strcmp(method, "POST")) ||
                   (!strcmp(route, "/api/import/files/mkdir") && !strcmp(method, "POST"));
    if (!matched) return false;
    pthread_mutex_lock(&import_mu);
    bool active = import_ready && !atomic_load(&import_stopping);
    if (active) active_requests++;
    pthread_mutex_unlock(&import_mu);
    if (!active) {
        refuse(fd, "409 Conflict", "import_not_ready", !strcmp(route, "/api/import/files/upload") ? content_length : 0);
        return true;
    }
    if (!strcmp(route, "/api/import/files") && !strcmp(method, "GET")) (void)handle_list(fd, path);
    else if (!strcmp(route, "/api/import/files/download")) (void)handle_download(fd, path);
    else if (!strcmp(route, "/api/import/files/upload")) (void)handle_upload(fd, path, content_length, wifi_transport);
    else if (!strcmp(route, "/api/import/files/upload-check")) (void)handle_upload_check(fd, path, wifi_transport);
    else if (!strcmp(route, "/api/import/files/rename")) (void)handle_rename(fd, path);
    else if (!strcmp(route, "/api/import/files/delete")) (void)handle_delete(fd, path);
    else (void)handle_mkdir(fd, path);
    pthread_mutex_lock(&import_mu);
    if (active_requests) active_requests--;
    if (!active_requests) pthread_cond_broadcast(&import_idle_cv);
    pthread_mutex_unlock(&import_mu);
    return true;
}
