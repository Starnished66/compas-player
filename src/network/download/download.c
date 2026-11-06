#include "download.h"
#include "files/file_integrity.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

typedef struct {
    int parent;
    FILE *file;
    char destination[NAME_MAX + 1];
    char temporary[64];
} download_file_t;

static bool request_valid(const download_request_t *r) {
    uint64_t offset_max = sizeof(off_t) >= 8 ? INT64_MAX : INT32_MAX;
    if (!r || !r->url || !r->url[0] || !r->dest_path || !r->dest_path[0] ||
        !r->max_bytes || r->max_bytes > offset_max || r->max_redirects > INT_MAX ||
        r->required_status < 0 || r->required_status > 599 ||
        (r->has_expected_size && r->expected_size > r->max_bytes)) return false;
    if (!r->expected_sha256) return true;
    if (strlen(r->expected_sha256) != 64) return false;
    for (int i = 0; i < 64; ++i)
        if (!isxdigit((unsigned char)r->expected_sha256[i])) return false;
    return true;
}

static bool open_parent(const char *path, download_file_t *f) {
    char parent[PATH_MAX];
    if (strlen(path) >= sizeof(parent)) { errno = ENAMETOOLONG; return false; }
    strcpy(parent, path);
    char *slash = strrchr(parent, '/');
    const char *name = slash ? slash + 1 : path;
    if (!name[0] || strlen(name) > NAME_MAX || !strcmp(name, ".") || !strcmp(name, "..")) {
        errno = EINVAL; return false;
    }
    strcpy(f->destination, name);
    if (!slash) strcpy(parent, ".");
    else if (slash == parent) slash[1] = '\0';
    else *slash = '\0';
    f->parent = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    return f->parent >= 0;
}

static bool open_temporary(download_file_t *f) {
    unsigned char random[12];
    int source = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (source < 0) return false;
    ssize_t count = read(source, random, sizeof(random));
    int saved = errno;
    close(source);
    if (count != sizeof(random)) { errno = count < 0 ? saved : EIO; return false; }
    strcpy(f->temporary, ".compas-download-");
    for (unsigned i = 0; i < sizeof(random); ++i)
        snprintf(f->temporary + 17 + i * 2, 3, "%02x", random[i]);
    int fd = openat(f->parent, f->temporary, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) { f->temporary[0] = '\0'; return false; }
    f->file = fdopen(fd, "w+b");
    if (!f->file) { saved = errno; close(fd); errno = saved; return false; }
    return true;
}

static download_error_t transport_error(const http_file_result_t *h) {
    const char *e = h->error ? h->error : HTTP_ERR_NONE;
    if (!strcmp(e, HTTP_ERR_CANCELLED)) return DOWNLOAD_ERR_CANCELLED;
    if (!strcmp(e, HTTP_ERR_WRITE)) return DOWNLOAD_ERR_WRITE;
    if (!strcmp(e, HTTP_ERR_RESPONSE_TOO_LARGE)) return DOWNLOAD_ERR_TOO_LARGE;
    if (!strcmp(e, HTTP_ERR_TOO_MANY_REDIRECTS) || !strcmp(e, HTTP_ERR_INSECURE_REDIRECT) ||
        !strcmp(e, HTTP_ERR_INVALID_REDIRECT))
        return DOWNLOAD_ERR_REDIRECT;
    return DOWNLOAD_ERR_TRANSPORT;
}

static download_error_t transfer(const download_request_t *r, download_file_t *f,
    http_cancel_token_t *cancel, download_progress_fn progress, void *user, download_result_t *out) {
    http_file_request_t request = {.url = r->url, .verify_tls = r->verify_tls,
        .max_bytes = r->max_bytes, .max_redirects = r->max_redirects,
        .connect_timeout_ms = r->connect_timeout_ms, .read_timeout_ms = r->read_timeout_ms,
        .total_timeout_ms = r->total_timeout_ms};
    http_file_result_t response = {0};
    bool ok = http_get_to_open_file(&request, f->file, cancel, progress, user, &response);
    out->http_status = response.status;
    out->transport_error = response.error;
    out->system_errno = response.system_errno;
    out->bytes_written = response.bytes_written;
    snprintf(out->content_type, sizeof(out->content_type), "%s", response.content_type);
    if (!ok) return transport_error(&response);
    if (response.status == 206 || (r->required_status ? response.status != r->required_status :
        response.status < 200 || response.status >= 300)) return DOWNLOAD_ERR_HTTP_STATUS;
    return DOWNLOAD_OK;
}

static download_error_t verify(const download_request_t *r, FILE *file, download_result_t *out) {
    if (fflush(file) != 0) return DOWNLOAD_ERR_WRITE;
    if (r->has_expected_size && out->bytes_written != r->expected_size) return DOWNLOAD_ERR_SIZE;
    if (!r->expected_sha256) return DOWNLOAD_OK;
    if (fseeko(file, 0, SEEK_SET) != 0) return DOWNLOAD_ERR_HASH;
    char hash[65];
    uint64_t size;
    if (!file_sha256_stream(file, &size, hash)) return DOWNLOAD_ERR_HASH;
    if (size != out->bytes_written) return DOWNLOAD_ERR_SIZE;
    return strcasecmp(hash, r->expected_sha256) ? DOWNLOAD_ERR_SHA256 : DOWNLOAD_OK;
}

static download_error_t publish(download_file_t *f, http_cancel_token_t *cancel,
    download_progress_fn progress, void *user, download_result_t *out) {
    if (fsync(fileno(f->file)) != 0) return DOWNLOAD_ERR_SYNC;
    int closed = fclose(f->file);
    f->file = NULL;
    if (closed != 0) return DOWNLOAD_ERR_CLOSE;
    if ((progress && !progress(out->bytes_written, out->bytes_written, user)) ||
        (cancel && http_cancel_token_is_cancelled(cancel))) return DOWNLOAD_ERR_CANCELLED;
    if (renameat(f->parent, f->temporary, f->parent, f->destination) != 0) return DOWNLOAD_ERR_RENAME;
    out->committed = true;
    f->temporary[0] = '\0';
    if (!filesystem_sync_directory(f->parent, &out->durable)) return DOWNLOAD_ERR_DIR_SYNC;
    return DOWNLOAD_OK;
}

static void cleanup(download_file_t *f) {
    if (f->file) fclose(f->file);
    if (f->parent >= 0) {
        if (f->temporary[0]) unlinkat(f->parent, f->temporary, 0);
        close(f->parent);
    }
}

download_error_t download_to_file(const download_request_t *request,
    http_cancel_token_t *cancel, download_progress_fn progress, void *user, download_result_t *result) {
    download_result_t local = {0};
    download_result_t *out = result ? result : &local;
    memset(out, 0, sizeof(*out));
    download_file_t file = {.parent = -1};
    download_error_t error = DOWNLOAD_ERR_ARGUMENT;
    if (!request_valid(request)) goto done;
    error = DOWNLOAD_ERR_CANCELLED;
    if (cancel && http_cancel_token_is_cancelled(cancel)) goto done;
    error = DOWNLOAD_ERR_OPEN;
    if (!open_parent(request->dest_path, &file) || !open_temporary(&file)) goto done;
    error = transfer(request, &file, cancel, progress, user, out);
    if (error == DOWNLOAD_OK) error = verify(request, file.file, out);
    if (error == DOWNLOAD_OK) error = publish(&file, cancel, progress, user, out);
done:
    if (!out->system_errno && (error == DOWNLOAD_ERR_OPEN || error == DOWNLOAD_ERR_WRITE ||
        error == DOWNLOAD_ERR_HASH || error == DOWNLOAD_ERR_SYNC || error == DOWNLOAD_ERR_CLOSE ||
        error == DOWNLOAD_ERR_RENAME || error == DOWNLOAD_ERR_DIR_SYNC)) out->system_errno = errno;
    cleanup(&file);
    out->error = error;
    return error;
}
