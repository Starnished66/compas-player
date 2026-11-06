#ifndef DOWNLOAD_H
#define DOWNLOAD_H
#include "http/client/http_client.h"

typedef enum {
    DOWNLOAD_OK = 0, DOWNLOAD_ERR_ARGUMENT, DOWNLOAD_ERR_CANCELLED,
    DOWNLOAD_ERR_TRANSPORT, DOWNLOAD_ERR_HTTP_STATUS, DOWNLOAD_ERR_REDIRECT,
    DOWNLOAD_ERR_TOO_LARGE, DOWNLOAD_ERR_SIZE, DOWNLOAD_ERR_SHA256,
    DOWNLOAD_ERR_HASH, DOWNLOAD_ERR_OPEN, DOWNLOAD_ERR_WRITE, DOWNLOAD_ERR_SYNC,
    DOWNLOAD_ERR_CLOSE, DOWNLOAD_ERR_RENAME, DOWNLOAD_ERR_DIR_SYNC
} download_error_t;

typedef struct {
    const char *url;
    const char *dest_path;
    uint64_t max_bytes;
    bool has_expected_size;
    uint64_t expected_size;
    const char *expected_sha256;
    bool verify_tls;
    unsigned max_redirects;
    uint32_t connect_timeout_ms, read_timeout_ms, total_timeout_ms;
    int required_status;
} download_request_t;

typedef struct {
    download_error_t error;
    int http_status;
    int system_errno;
    const char *transport_error;
    uint64_t bytes_written;
    char content_type[128];
    bool committed;
    bool durable;
} download_result_t;

typedef bool (*download_progress_fn)(uint64_t received, uint64_t total, void *user);
/* Worker-only, synchronous; arguments and cancel token remain caller-owned.
 * Unsupported directory sync returns OK with committed=true, durable=false.
 * Real directory I/O errors return DIR_SYNC with committed=true, durable=false.
 * Media/Lua callers accept that commit with a log; store/previews require OK. */
download_error_t download_to_file(const download_request_t *request,
    http_cancel_token_t *cancel, download_progress_fn progress, void *user,
    download_result_t *result);
#endif
