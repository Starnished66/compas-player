#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define _FILE_OFFSET_BITS 64
#include "remote_import_files.h"

#include "usb_mode_control.h"
#include "gui_library.h"
#include "file_ops.h"

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
#define IMPORT_IDLE_SECONDS 60
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

static bool valid_utf8(const unsigned char *s) {
    while (*s) {
        if (*s < 0x80) { s++; continue; }
        if (*s >= 0xc2 && *s <= 0xdf) {
            if ((s[1] & 0xc0) != 0x80) return false;
            s += 2; continue;
        }
        if (*s == 0xe0) {
            if (s[1] < 0xa0 || s[1] > 0xbf || (s[2] & 0xc0) != 0x80) return false;
            s += 3; continue;
        }
        if ((*s >= 0xe1 && *s <= 0xec) || (*s >= 0xee && *s <= 0xef)) {
            if ((s[1] & 0xc0) != 0x80 || (s[2] & 0xc0) != 0x80) return false;
            s += 3; continue;
        }
        if (*s == 0xed) {
            if (s[1] < 0x80 || s[1] > 0x9f || (s[2] & 0xc0) != 0x80) return false;
            s += 3; continue;
        }
        if (*s == 0xf0) {
            if (s[1] < 0x90 || s[1] > 0xbf || (s[2] & 0xc0) != 0x80 || (s[3] & 0xc0) != 0x80) return false;
            s += 4; continue;
        }
        if (*s >= 0xf1 && *s <= 0xf3) {
            if ((s[1] & 0xc0) != 0x80 || (s[2] & 0xc0) != 0x80 || (s[3] & 0xc0) != 0x80) return false;
            s += 4; continue;
        }
        if (*s == 0xf4) {
            if (s[1] < 0x80 || s[1] > 0x8f || (s[2] & 0xc0) != 0x80 || (s[3] & 0xc0) != 0x80) return false;
            s += 4; continue;
        }
        return false;
    }
    return true;
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

static bool path_string_valid(const char *s, bool allow_empty) {
    size_t n = strlen(s);
    if (n > IMPORT_PATH_MAX || (!allow_empty && n == 0) || (n && s[n - 1] == '/')) return false;
    if (!valid_utf8((const unsigned char *)s)) return false;
    if (!n) return allow_empty;
    const char *part = s;
    for (const char *p = s;; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '\\' || (c != 0 && (c < 0x20 || c == 0x7f))) return false;
        if (c == '/' || c == 0) {
            size_t len = (size_t)(p - part);
            if (!len || len > IMPORT_NAME_MAX || (len == 1 && part[0] == '.') ||
                (len == 2 && part[0] == '.' && part[1] == '.') || part[0] == '.') return false;
            if (part[len - 1] == '.' || part[len - 1] == ' ') return false;
            if (c == 0) break;
            part = p + 1;
        }
    }
    return true;
}

static bool leaf_valid(const char *s) {
    size_t n = strlen(s);
    if (!n || n > IMPORT_NAME_MAX || s[0] == '.' || s[n - 1] == '.' || s[n - 1] == ' ') return false;
    if (!valid_utf8((const unsigned char *)s)) return false;
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
    if (!slash) return fcntl(rootfd, F_DUPFD_CLOEXEC, 3);
    char parent[IMPORT_PATH_MAX + 1];
    size_t n = (size_t)(slash - rel);
    memcpy(parent, rel, n); parent[n] = '\0';
    return open_dir_relative(rootfd, parent);
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

static void send_error(int fd, const char *status, const char *code) {
    char body[160];
    snprintf(body, sizeof(body), "{\"error\":\"%s\",\"code\":\"%s\"}", code, code);
    send_json(fd, status, body);
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

static bool send_chunk(int fd, const char *data, size_t size) {
    char h[32];
    int n = snprintf(h, sizeof(h), "%zx\r\n", size);
    return n > 0 && send_all(fd, h, (size_t)n) && send_all(fd, data, size) && send_all(fd, "\r\n", 2);
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

static bool handle_list(int fd, const char *path) {
    const char *keys[] = {"path", "offset", "limit"};
    char values[3][IMPORT_PATH_MAX + 1]; bool have[3];
    if (!query_parse(path, keys, values, have, 3) || !have[0]) {
        send_error(fd, "400 Bad Request", "invalid_query"); return true;
    }
    const char *rel = have[0] ? values[0] : "";
    if (!path_string_valid(rel, true)) { send_error(fd, "400 Bad Request", "invalid_path"); return true; }
    unsigned long offset = 0, limit = IMPORT_PAGE_DEFAULT;
    if ((have[1] && !parse_uint(values[1], &offset)) ||
        (have[2] && !parse_uint(values[2], &limit)) || limit == 0) {
        send_error(fd, "400 Bad Request", "invalid_paging"); return true;
    }
    if (limit > IMPORT_PAGE_MAX) limit = IMPORT_PAGE_MAX;
    int root = root_dup();
    if (root < 0) { send_error(fd, "409 Conflict", "import_not_ready"); return true; }
    int dirfd = open_dir_relative(root, rel); close(root);
    if (dirfd < 0) { send_error(fd, errno == ENOENT ? "404 Not Found" : "400 Bad Request", "directory_not_found"); return true; }
    DIR *dir = fdopendir(dirfd);
    if (!dir) { close(dirfd); send_error(fd, "500 Internal Server Error", "directory_read_failed"); return true; }
    unsigned long total = 0;
    struct dirent *de;
    while ((de = readdir(dir))) {
        if (request_stopped()) break;
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..") || de->d_name[0] == '.' || !leaf_valid(de->d_name)) continue;
        struct stat st;
        if (fstatat(dirfd, de->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0 && (S_ISDIR(st.st_mode) || S_ISREG(st.st_mode))) total++;
    }
    if (request_stopped()) { closedir(dir); return true; }
    import_list_entry_t *window = NULL;
    import_list_entry_t *entries = NULL;
    size_t selected = 0;
    if (offset < total) {
        window = malloc(IMPORT_LIST_WINDOW_MAX * sizeof(*window));
        entries = malloc((size_t)limit * sizeof(*entries));
        if (!window || !entries) {
            free(window); free(entries); closedir(dir);
            send_error(fd, "500 Internal Server Error", "directory_read_failed"); return true;
        }
        unsigned long skip = offset;
        import_list_entry_t boundary;
        bool have_boundary = false;
        while (selected < limit) {
            if (request_stopped()) { free(window); free(entries); closedir(dir); return true; }
            rewinddir(dir);
            size_t count = 0;
            while ((de = readdir(dir))) {
                if (request_stopped()) { free(window); free(entries); closedir(dir); return true; }
                if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..") || de->d_name[0] == '.' || !leaf_valid(de->d_name)) continue;
                struct stat st;
                if (fstatat(dirfd, de->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0 || (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode))) continue;
                import_list_entry_t candidate;
                strcpy(candidate.name, de->d_name);
                candidate.is_directory = S_ISDIR(st.st_mode);
                candidate.size = S_ISREG(st.st_mode) ? st.st_size : 0;
                if (have_boundary && import_list_entry_compare(&candidate, &boundary) <= 0) continue;
                import_list_heap_add(window, &count, IMPORT_LIST_WINDOW_MAX, &candidate);
            }
            if (request_stopped()) { free(window); free(entries); closedir(dir); return true; }
            if (!count) break;
            if (count > 1) qsort(window, count, sizeof(*window), import_list_entry_compare);
            if (skip >= count) {
                skip -= (unsigned long)count;
            } else {
                size_t start = (size_t)skip;
                skip = 0;
                for (size_t i = start; i < count && selected < limit; i++)
                    entries[selected++] = window[i];
            }
            if (selected == limit || count < IMPORT_LIST_WINDOW_MAX) break;
            boundary = window[count - 1];
            have_boundary = true;
        }
    }
    char hdr[256];
    int hn = snprintf(hdr, sizeof(hdr), "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nTransfer-Encoding: chunked\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n");
    if (hn <= 0 || !send_all(fd, hdr, (size_t)hn)) { free(window); free(entries); closedir(dir); return true; }
    char prefix[IMPORT_PATH_MAX * 6 + 160], escpath[IMPORT_PATH_MAX * 6 + 1];
    json_escape(escpath, sizeof(escpath), rel);
    bool more = offset < total && limit < total - offset;
    unsigned long next = more ? offset + limit : total;
    hn = snprintf(prefix, sizeof(prefix), "{\"path\":\"%s\",\"entries\":[", escpath);
    if (hn > 0) (void)send_chunk(fd, prefix, (size_t)hn);
    unsigned long emitted = 0;
    char entry[IMPORT_PATH_MAX * 6 + IMPORT_NAME_MAX * 6 + 512];
    char escname[IMPORT_NAME_MAX * 6 + 1], full[IMPORT_PATH_MAX + 1], escfull[IMPORT_PATH_MAX * 6 + 1];
    for (size_t i = 0; i < (size_t)selected && emitted < limit; i++) {
        if (request_stopped()) break;
        import_list_entry_t *item = &entries[i];
        int pn = snprintf(full, sizeof(full), "%s%s%s", rel, *rel ? "/" : "", item->name);
        if (pn < 0 || (size_t)pn >= sizeof(full)) continue;
        json_escape(escname, sizeof(escname), item->name);
        json_escape(escfull, sizeof(escfull), full);
        int en = snprintf(entry, sizeof(entry), "%s{\"name\":\"%s\",\"path\":\"%s\",\"isDirectory\":%s,\"size\":%llu}",
                          emitted ? "," : "", escname, escfull, item->is_directory ? "true" : "false",
                          (unsigned long long)item->size);
        if (en <= 0 || (size_t)en >= sizeof(entry) || !send_chunk(fd, entry, (size_t)en)) break;
        emitted++;
    }
    free(window);
    free(entries);
    char suffix[128];
    int sn = more ? snprintf(suffix, sizeof(suffix), "],\"offset\":%lu,\"next_offset\":%lu,\"total\":%lu}", offset, next, total)
                  : snprintf(suffix, sizeof(suffix), "],\"offset\":%lu,\"next_offset\":null,\"total\":%lu}", offset, total);
    if (sn > 0) (void)send_chunk(fd, suffix, (size_t)sn);
    (void)send_all(fd, "0\r\n\r\n", 5);
    closedir(dir);
    return true;
}

static bool begin_mutation(int fd) {
    if (pthread_mutex_trylock(&fileop_mu) != 0) {
        send_error(fd, "409 Conflict", "storage_busy"); return false;
    }
    if (fileop_active || request_stopped() || !file_op_external_begin()) {
        pthread_mutex_unlock(&fileop_mu); send_error(fd, "409 Conflict", request_stopped() ? "import_stopping" : "storage_busy"); return false;
    }
    fileop_active = true;
    if (request_stopped() || !mounted_root() || !usb_mode_control_storage_write_begin()) {
        file_op_external_end();
        fileop_active = false; pthread_mutex_unlock(&fileop_mu);
        send_error(fd, "503 Service Unavailable", "storage_busy"); return false;
    }
    return true;
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

static bool body_read_all(int fd, uint8_t *buf, size_t size, size_t *received,
                          const struct timespec *start, struct timespec *last) {
    *received = 0;
    while (*received < size) {
        if (request_stopped()) return false;
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed_idle = now.tv_sec - last->tv_sec;
        long elapsed_total = now.tv_sec - start->tv_sec;
        if (elapsed_idle >= IMPORT_IDLE_SECONDS || elapsed_total >= IMPORT_TOTAL_SECONDS) { errno = ETIMEDOUT; return false; }
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

static bool handle_upload(int fd, const char *path, long long content_length, bool wifi) {
    if (!wifi) { send_error(fd, "403 Forbidden", "wifi_only"); return true; }
    if (content_length < 0) { send_error(fd, "411 Length Required", "length_required"); return true; }
    if ((unsigned long long)content_length > UINT32_MAX) { send_error(fd, "413 Content Too Large", "file_too_large"); return true; }
    const char *keys[] = {"path", "name"}; char values[2][IMPORT_PATH_MAX + 1]; bool have[2];
    if (!query_parse(path, keys, values, have, 2) || !have[0] || !have[1] ||
        !path_string_valid(values[0], true) || !leaf_valid(values[1])) { send_error(fd, "400 Bad Request", "invalid_path_or_name"); return true; }
    if (strlen(values[0]) + (values[0][0] ? 1u : 0u) + strlen(values[1]) > IMPORT_PATH_MAX) { send_error(fd, "400 Bad Request", "invalid_path_or_name"); return true; }
    int root = root_dup();
    if (root < 0) { send_error(fd, "409 Conflict", "import_not_ready"); return true; }
    if (!begin_mutation(fd)) { close(root); return true; }
    int dirfd = open_dir_relative(root, values[0]); close(root);
    if (dirfd < 0) { end_mutation(); send_error(fd, "404 Not Found", "directory_not_found"); return true; }
    int out = openat(dirfd, values[1], O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0666);
    if (out < 0) { close(dirfd); end_mutation(); send_error(fd, errno == EEXIST ? "409 Conflict" : "400 Bad Request", errno == EEXIST ? "name_exists" : "open_failed"); return true; }
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
        if (!body_read_all(fd, buffer, chunk, &got, &upload_start, &last_activity)) { ok = false; break; }
        size_t off = 0;
        while (off < got) {
            ssize_t n = write(out, buffer + off, got - off);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { ok = false; break; }
            off += (size_t)n; total += (uint64_t)n; unsynced += (size_t)n;
            if (unsynced >= IMPORT_SYNC_BYTES) {
                if (fdatasync(out) != 0) { ok = false; break; }
                unsynced = 0;
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
    if (!ok) { send_error(fd, request_stopped() ? "503 Service Unavailable" : "400 Bad Request", request_stopped() ? "import_stopped" : "upload_failed"); return true; }
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
        !path_string_valid(values[0], false) || !leaf_valid(values[1])) { send_error(fd, "400 Bad Request", "invalid_path_or_name"); return true; }
    const char *last_slash = strrchr(values[0], '/');
    size_t parent_len = last_slash ? (size_t)(last_slash - values[0]) : 0;
    if (parent_len + (parent_len ? 1u : 0u) + strlen(values[1]) > IMPORT_PATH_MAX) { send_error(fd, "400 Bad Request", "invalid_path_or_name"); return true; }
    int root = root_dup(); if (root < 0) { send_error(fd, "409 Conflict", "import_not_ready"); return true; }
    if (!begin_mutation(fd)) { close(root); return true; }
    char oldleaf[IMPORT_NAME_MAX + 1]; int parent = resolve_parent(root, values[0], oldleaf, sizeof(oldleaf)); close(root);
    if (parent < 0) { end_mutation(); send_error(fd, "404 Not Found", "item_not_found"); return true; }
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
        send_error(fd, status, code); return true;
    }
    send_json(fd, "200 OK", "{\"ok\":true}"); return true;
}

static bool handle_delete(int fd, const char *path) {
    const char *keys[] = {"path"}; char values[1][IMPORT_PATH_MAX + 1]; bool have[1];
    if (!query_parse(path, keys, values, have, 1) || !have[0] || !path_string_valid(values[0], false)) { send_error(fd, "400 Bad Request", "invalid_path"); return true; }
    int root = root_dup(); if (root < 0) { send_error(fd, "409 Conflict", "import_not_ready"); return true; }
    if (!begin_mutation(fd)) { close(root); return true; }
    char leaf[IMPORT_NAME_MAX + 1]; int parent = resolve_parent(root, values[0], leaf, sizeof(leaf)); close(root);
    if (parent < 0) { end_mutation(); send_error(fd, "404 Not Found", "item_not_found"); return true; }
    struct stat st;
    bool ok = fstatat(parent, leaf, &st, AT_SYMLINK_NOFOLLOW) == 0 && (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode));
    if (ok) ok = unlinkat(parent, leaf, S_ISDIR(st.st_mode) ? AT_REMOVEDIR : 0) == 0;
    int err = errno; close(parent); end_mutation();
    if (!ok) { send_error(fd, err == ENOTEMPTY ? "409 Conflict" : err == ENOENT ? "404 Not Found" : "400 Bad Request", err == ENOTEMPTY ? "directory_not_empty" : err == ENOENT ? "item_not_found" : "delete_failed"); return true; }
    send_json(fd, "200 OK", "{\"ok\":true}"); return true;
}

static bool handle_mkdir(int fd, const char *path) {
    const char *keys[] = {"path", "name"}; char values[2][IMPORT_PATH_MAX + 1]; bool have[2];
    if (!query_parse(path, keys, values, have, 2) || !have[0] || !have[1] || !path_string_valid(values[0], true) || !leaf_valid(values[1])) { send_error(fd, "400 Bad Request", "invalid_path_or_name"); return true; }
    if (strlen(values[0]) + (values[0][0] ? 1u : 0u) + strlen(values[1]) > IMPORT_PATH_MAX) { send_error(fd, "400 Bad Request", "invalid_path_or_name"); return true; }
    int root = root_dup(); if (root < 0) { send_error(fd, "409 Conflict", "import_not_ready"); return true; }
    if (!begin_mutation(fd)) { close(root); return true; }
    int parent = open_dir_relative(root, values[0]); close(root);
    bool ok = parent >= 0 && mkdirat(parent, values[1], 0777) == 0;
    int err = errno; if (parent >= 0) close(parent); end_mutation();
    if (!ok) { send_error(fd, err == EEXIST ? "409 Conflict" : "400 Bad Request", err == EEXIST ? "name_exists" : "mkdir_failed"); return true; }
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
                   (!strcmp(route, "/api/import/files/upload") && !strcmp(method, "POST")) ||
                   (!strcmp(route, "/api/import/files/rename") && !strcmp(method, "POST")) ||
                   (!strcmp(route, "/api/import/files/delete") && !strcmp(method, "POST")) ||
                   (!strcmp(route, "/api/import/files/mkdir") && !strcmp(method, "POST"));
    if (!matched) return false;
    pthread_mutex_lock(&import_mu);
    bool active = import_ready && !atomic_load(&import_stopping);
    if (active) active_requests++;
    pthread_mutex_unlock(&import_mu);
    if (!active) { send_error(fd, "409 Conflict", "import_not_ready"); return true; }
    if (!strcmp(route, "/api/import/files") && !strcmp(method, "GET")) (void)handle_list(fd, path);
    else if (!strcmp(route, "/api/import/files/upload")) (void)handle_upload(fd, path, content_length, wifi_transport);
    else if (!strcmp(route, "/api/import/files/rename")) (void)handle_rename(fd, path);
    else if (!strcmp(route, "/api/import/files/delete")) (void)handle_delete(fd, path);
    else (void)handle_mkdir(fd, path);
    pthread_mutex_lock(&import_mu);
    if (active_requests) active_requests--;
    if (!active_requests) pthread_cond_broadcast(&import_idle_cv);
    pthread_mutex_unlock(&import_mu);
    return true;
}
