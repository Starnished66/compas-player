#include "remote_control_internal.h"

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Escape JSON string delimiters and control bytes so metadata round-trips as
 * the same string when a client parses the response. */
void json_escape_append(char * out, size_t out_size, const char * in) {
    size_t len = strlen(out);
    for (const char * p = in; *p && len + 2 < out_size; p++) {
        unsigned char c = (unsigned char) *p;
        if (c == '"' || c == '\\') {
            if (len + 3 >= out_size) break;
            out[len++] = '\\';
            out[len++] = (char) c;
        } else if (c < 0x20) {
            static const char hex[] = "0123456789abcdef";
            if (len + 6 >= out_size) break;
            out[len++] = '\\';
            out[len++] = 'u';
            out[len++] = '0';
            out[len++] = '0';
            out[len++] = hex[c >> 4];
            out[len++] = hex[c & 0x0f];
        } else {
            out[len++] = (char) c;
        }
    }
    out[len] = '\0';
}

void json_builder_init(json_builder_t * b, char * data, size_t capacity) {
    b->data = data;
    b->capacity = capacity;
    b->length = 0;
    b->truncated = capacity == 0;
    if (capacity > 0) data[0] = '\0';
}

bool json_builder_appendf(json_builder_t * b, const char * format, ...) {
    if (b->truncated || b->length >= b->capacity) return false;
    size_t old_length = b->length;
    va_list ap;
    va_start(ap, format);
    int written = vsnprintf(b->data + b->length, b->capacity - b->length, format, ap);
    va_end(ap);
    if (written < 0 || (size_t) written >= b->capacity - b->length) {
        b->length = old_length;
        b->data[old_length] = '\0';
        b->truncated = true;
        return false;
    }
    b->length += (size_t) written;
    return true;
}

/* Decodes application/x-www-form-urlencoded query-string bytes (%XX and
 * "+" for space; both %20 and "+" are accepted for space). Bounded,
 * truncates rather than overflowing if the decoded result wouldn't fit;
 * returns false when it truncated. */
bool url_decode(const char * in, char * out, size_t out_size) {
    size_t len = 0;
    while (*in && len + 1 < out_size) {
        if (*in == '%' && in[1] && in[2]) {
            char hex[3] = { in[1], in[2], '\0' };
            char * end;
            long v = strtol(hex, &end, 16);
            if (end == hex + 2) {
                out[len++] = (char) v;
                in += 3;
                continue;
            }
        }
        out[len++] = (*in == '+') ? ' ' : *in;
        in++;
    }
    out[len] = '\0';
    return *in == '\0';
}

bool url_decode_checked(const char * in, char * out, size_t out_size) {
    size_t len = 0;
    if (!out || out_size == 0) return false;
    while (*in) {
        if (len + 1 >= out_size) { out[0] = '\0'; return false; }
        if (*in == '%') {
            if (!in[1] || !in[2]) { out[0] = '\0'; return false; }
            bool first_is_hex = (in[1] >= '0' && in[1] <= '9') || (in[1] >= 'a' && in[1] <= 'f') ||
                                (in[1] >= 'A' && in[1] <= 'F');
            bool second_is_hex = (in[2] >= '0' && in[2] <= '9') || (in[2] >= 'a' && in[2] <= 'f') ||
                                 (in[2] >= 'A' && in[2] <= 'F');
            if (!first_is_hex || !second_is_hex) { out[0] = '\0'; return false; }
            char hex[3] = { in[1], in[2], '\0' };
            char * end;
            long v = strtol(hex, &end, 16);
            if (end != hex + 2 || v == 0) { out[0] = '\0'; return false; }
            out[len++] = (char)v;
            in += 3;
            continue;
        }
        out[len++] = (*in == '+') ? ' ' : *in;
        in++;
    }
    out[len] = '\0';
    return true;
}

/* Returns the text just past "key=" in the first query segment named key
 * (still percent-encoded, running to the next '&' or the end), or NULL. */
static const char * query_param_value(const char * path, const char * key) {
    const char * q = strchr(path, '?');
    if (!q) return NULL;
    q++;
    size_t key_len = strlen(key);
    while (*q) {
        if (strncmp(q, key, key_len) == 0 && q[key_len] == '=') return q + key_len + 1;
        const char * amp = strchr(q, '&');
        if (!amp) break;
        q = amp + 1;
    }
    return NULL;
}

bool query_param_present(const char * path, const char * key) {
    return query_param_value(path, key) != NULL;
}

/* Pulls a raw (still percent-encoded) string query parameter's value.
 * Returns false (leaving out untouched) if absent or too long for out. */
bool query_param_str(const char * path, const char * key, char * out, size_t out_size) {
    const char * val = query_param_value(path, key);
    if (!val) return false;
    const char * amp = strchr(val, '&');
    size_t val_len = amp ? (size_t) (amp - val) : strlen(val);
    if (val_len >= out_size) return false;
    memcpy(out, val, val_len);
    out[val_len] = '\0';
    return true;
}

/* Raw (percent-encoded) size limit for the loosely decoded filter and name
 * parameters read by query_loose(): every byte of a 127-byte name may be
 * sent as %XX. */
#define QUERY_LOOSE_RAW_MAX (128 * 3)

/* Optional filter/name parameter decoded with the lenient url_decode().
 * Clears out first. Returns 0 when absent, -1 when the raw value does not
 * fit QUERY_LOOSE_RAW_MAX bytes or the decoded value does not fit out_size,
 * 1 when decoded. */
int query_loose(const char * path, const char * key, char * out, size_t out_size) {
    out[0] = '\0';
    if (!query_param_present(path, key)) return 0;
    char raw[QUERY_LOOSE_RAW_MAX];
    if (!query_param_str(path, key, raw, sizeof(raw))) return -1;
    if (!url_decode(raw, out, out_size)) {
        out[0] = '\0';
        return -1;
    }
    return 1;
}

void send_all(int fd, const char * data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, data + sent, len - sent);
        if (n <= 0) return;
        sent += (size_t) n;
    }
}

/* Sends the status line and no-cache headers of a close-delimited response
 * with a known body length. */
void send_header(int fd, const char * status_line, const char * content_type, size_t len) {
    char header[320];
    int header_len = snprintf(header, sizeof(header),
                              "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                              "Cache-Control: no-store, no-cache, must-revalidate\r\nPragma: no-cache\r\n"
                              "Connection: close\r\n\r\n",
                              status_line, content_type, len);
    if (header_len > 0 && (size_t) header_len < sizeof(header)) send_all(fd, header, (size_t) header_len);
}

void send_response_binary(int fd, const char * status_line, const char * content_type, const uint8_t * data,
                                 size_t len) {
    send_header(fd, status_line, content_type, len);
    send_all(fd, (const char *) data, len);
}

/* Sends an HTTP response with no-cache headers. */
void send_response(int fd, const char * status_line, const char * content_type, const char * body) {
    send_response_binary(fd, status_line, content_type, (const uint8_t *) body, strlen(body));
}

/* Large JSON goes in a heap buffer to keep request frames small on the
 * worker threads. Sends 500 and returns NULL when it cannot be allocated. */
char * json_alloc_or_500(int cfd, size_t capacity) {
    char * json = malloc(capacity);
    if (!json) send_response(cfd, "500 Internal Server Error", "text/plain", "Out of memory");
    return json;
}

void send_json_free(int cfd, char * json) {
    send_response(cfd, "200 OK", "application/json", json);
    free(json);
}

/* Pulls an integer query parameter from the request path. Returns false if
 * absent, invalid or out of int range. */
bool query_param_int(const char * path, const char * key, int * out_value) {
    const char * val = query_param_value(path, key);
    if (!val) return false;
    char * end;
    errno = 0;
    long v = strtol(val, &end, 10);
    if (end == val) return false; /* no digits at all */
    if (errno == ERANGE || v < INT_MIN || v > INT_MAX) return false;
    *out_value = (int) v;
    return true;
}

/* Like query_param_int() but a value outside int range clamps to INT_MIN or
 * INT_MAX instead of failing, for paging parameters where "far past the end"
 * is a valid request. */
bool query_param_int_saturating(const char * path, const char * key, int * out_value) {
    const char * val = query_param_value(path, key);
    if (!val) return false;
    char * end;
    long v = strtol(val, &end, 10);
    if (end == val) return false; /* no digits at all */
    *out_value = v < INT_MIN ? INT_MIN : v > INT_MAX ? INT_MAX : (int) v;
    return true;
}

/* Pulls a 64-bit integer query parameter from the request path. Returns
 * false if absent, invalid or out of range. */
bool query_param_int64(const char * path, const char * key, int64_t * out_value) {
    const char * val = query_param_value(path, key);
    if (!val) return false;
    char * end;
    errno = 0;
    long long v = strtoll(val, &end, 10);
    if (end == val) return false; /* no digits at all */
    if (errno == ERANGE) return false;
    *out_value = (int64_t) v;
    return true;
}

/* Sniffs enough image magic to label either an original sidecar or embedded
 * art. The player can return BMP sidecars as well as embedded JPEG/PNG; an
 * unrecognized-but-real image keeps the common JPEG fallback. */
const char * sniff_image_content_type(const uint8_t * data, uint32_t size) {
    if (size >= 8 && data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E && data[3] == 0x47) return "image/png";
    if (size >= 2 && data[0] == 'B' && data[1] == 'M') return "image/bmp";
    return "image/jpeg";
}

/* JSON error body shared by the catalog and later feature families:
 * {"error":"<family>_error","code":"<reason>"}. */
void send_api_error(int cfd, const char * status, const char * error, const char * code) {
    char body[160];
    snprintf(body, sizeof(body), "{\"error\":\"%s\",\"code\":\"%s\"}", error, code);
    send_response(cfd, status, "application/json", body);
}

/* Error whose "error" and "code" are the same reason string (the legacy routes'
 * original "error" values stay as they were; "code" is additive). */
void send_reason_error(int cfd, const char * status, const char * reason) {
    send_api_error(cfd, status, reason, reason);
}

void catalog_send_error(int cfd, const char * status, const char * code) {
    send_api_error(cfd, status, "catalog_error", code);
}

bool catalog_query_number(const char * path, const char * key, uint64_t default_value,
                                 uint64_t max_value, uint64_t * out_value) {
    if (!query_param_present(path, key)) {
        *out_value = default_value;
        return true;
    }
    char raw[32] = {0};
    if (!query_param_str(path, key, raw, sizeof(raw)) || raw[0] == '\0') return false;
    for (const char * p = raw; *p; p++) if (*p < '0' || *p > '9') return false;
    errno = 0;
    char * end = NULL;
    unsigned long long value = strtoull(raw, &end, 10);
    if (errno == ERANGE || !end || *end != '\0' || value > max_value) return false;
    *out_value = (uint64_t) value;
    return true;
}

/* Offset/limit for paged routes: offset defaults to 0 and is at most
 * INT_MAX, limit defaults to 50 and is 1..max_limit. */
#define PAGE_DEFAULT_LIMIT 50

bool query_paging(const char * path, uint64_t max_limit, int * out_offset, int * out_limit) {
    uint64_t offset = 0, limit = PAGE_DEFAULT_LIMIT;
    if (!catalog_query_number(path, "offset", 0, INT_MAX, &offset) ||
        !catalog_query_number(path, "limit", PAGE_DEFAULT_LIMIT, max_limit, &limit) || limit == 0) return false;
    *out_offset = (int) offset;
    *out_limit = (int) limit;
    return true;
}

bool catalog_query_revision_named(const char * path, const char * key, bool required,
                                         char out[METADATA_DB_CATALOG_REVISION_SIZE]) {
    out[0] = '\0';
    if (!query_param_present(path, key)) return !required;
    char raw[METADATA_DB_CATALOG_REVISION_SIZE * 3] = {0};
    if (!query_param_str(path, key, raw, sizeof(raw)) ||
        !url_decode_checked(raw, out, METADATA_DB_CATALOG_REVISION_SIZE) || out[0] == '\0') return false;
    return true;
}

uint64_t catalog_hash_text(uint64_t hash, const char * text) {
    for (const unsigned char * p = (const unsigned char *) text; *p; p++) hash = fnv1a_byte(hash, *p);
    return hash;
}

/* Appends one prebuilt row while keeping `reserve` bytes for the closing
 * syntax, so a full page ends early instead of producing invalid JSON. */
bool json_append_row(json_builder_t * json, const char * row, size_t reserve) {
    size_t len = strlen(row);
    if (json->length + len + reserve >= json->capacity) return false;
    memcpy(json->data + json->length, row, len + 1);
    json->length += len;
    return true;
}

/* Decodes one query value strictly (valid %XX escapes, no NUL).
 * Returns 0 when absent, 1 when decoded, -1 when too long or malformed. */
int query_decoded(const char * path, const char * key, char * out, size_t out_size) {
    out[0] = '\0';
    if (!query_param_present(path, key)) return 0;
    char raw[REQUEST_LINE_MAX];
    if (!query_param_str(path, key, raw, sizeof(raw))) return -1;
    return url_decode_checked(raw, out, out_size) ? 1 : -1;
}

/* Required nonnegative integer no larger than max_value. */
bool query_required_number(const char * path, const char * key, uint64_t max_value, uint64_t * out) {
    return query_param_present(path, key) && catalog_query_number(path, key, 0, max_value, out);
}
