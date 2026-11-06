#include "subsonic_covers.h"
#include "subsonic_cache_internal.h"
#include "../../artwork/decode/cover_decode.h"

#include "cJSON.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define COVER_DIR "covers"
#define THUMB_MAGIC UINT32_C(0x31545643) /* "CVT1" */
#define THUMB_VERSION 1U
#define COVER_CONNECT_TIMEOUT_MS 10000U
#define COVER_READ_TIMEOUT_MS 15000U
#define COVER_TOTAL_TIMEOUT_MS 30000U
#define ERROR_DOC_HEAD 512 /* how much of an XML answer is searched for the error element */

typedef struct {
    uint32_t magic, version, w, h;
} thumb_header_t;

typedef enum { DOWNLOAD_OK, DOWNLOAD_FAILED, DOWNLOAD_NO_ART } download_t;

uint64_t subsonic_cover_key(const char * srvkey, const char * cover_id) {
    uint64_t hash = subsonic_fnv64(SUBSONIC_FNV64_INIT, srvkey, strlen(srvkey));
    hash = subsonic_fnv64(hash, "\n", 1);
    return subsonic_fnv64(hash, cover_id, strlen(cover_id));
}

/* covers/<fnv64(id)>.t<px>.rgb or covers/<fnv64(id)>.<px>.img, relative to the server folder. */
static bool cover_file(const char * id, bool thumb, int px, char * out, size_t size) {
    if (!id || !id[0] || px <= 0 || px > SUBSONIC_COVER_LARGE_PX) return false;
    unsigned long long hash = subsonic_fnv64(SUBSONIC_FNV64_INIT, id, strlen(id));
    int n = snprintf(out, size, COVER_DIR "/%016llx.%s%d.%s", hash, thumb ? "t" : "", px, thumb ? "rgb" : "img");
    return n > 0 && (size_t) n < size;
}

typedef struct {
    const void * head;
    size_t head_len;
    const void * data;
    size_t data_len;
} blob_t;

static bool fill_blob(FILE * f, const void * ctx) {
    const blob_t * blob = ctx;
    return (!blob->head_len || fwrite(blob->head, blob->head_len, 1, f) == 1) &&
           fwrite(blob->data, blob->data_len, 1, f) == 1;
}

static atomic_int saves_since_prune;

static bool write_blob(const subsonic_server_t * server, const char * file, const blob_t * blob) {
    bool ok = subsonic_cache_write_atomic(server, file, fill_blob, blob);
    if (ok) atomic_fetch_add(&saves_since_prune, 1);
    return ok;
}

bool subsonic_covers_prune_due(void) {
    int saved = atomic_load(&saves_since_prune);
    if (saved < SUBSONIC_COVER_SAVES_PER_PRUNE) return false;
    atomic_fetch_sub(&saves_since_prune, saved);
    return true;
}

bool subsonic_cover_thumb_load(const subsonic_server_t * server, const char * cover_id, int px, uint16_t ** px_out) {
    *px_out = NULL;
    char file[64];
    if (!cover_file(cover_id, true, px, file, sizeof(file))) return false;
    size_t bytes = (size_t) px * (size_t) px * sizeof(uint16_t);
    FILE * f = subsonic_cache_open(server, file, (long) (sizeof(thumb_header_t) + bytes));
    if (!f) return false;
    thumb_header_t head;
    uint16_t * pixels = malloc(bytes);
    bool ok = pixels && fread(&head, sizeof(head), 1, f) == 1 && head.magic == THUMB_MAGIC &&
              head.version == THUMB_VERSION && head.w == (uint32_t) px && head.h == (uint32_t) px &&
              fread(pixels, 1, bytes, f) == bytes && fgetc(f) == EOF; /* the size must match exactly */
    fclose(f);
    if (!ok) {
        free(pixels);
        return false;
    }
    *px_out = pixels;
    return true;
}

static bool read_blob(const subsonic_server_t * server, const char * file, uint8_t ** bytes, size_t * n) {
    FILE * f = subsonic_cache_open(server, file, SUBSONIC_COVER_LARGE_MAX);
    long size = f && fseek(f, 0, SEEK_END) == 0 ? ftell(f) : -1;
    uint8_t * data = size > 0 && fseek(f, 0, SEEK_SET) == 0 ? malloc((size_t) size) : NULL;
    bool ok = data && fread(data, 1, (size_t) size, f) == (size_t) size;
    if (f) fclose(f);
    if (!ok) {
        free(data);
        return false;
    }
    *bytes = data;
    *n = (size_t) size;
    return true;
}

/* The image types the decoder takes: JPEG, PNG and BMP. */
static bool looks_like_image(const uint8_t * b, size_t n) {
    return n > 4 && ((b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) || memcmp(b, "\x89PNG", 4) == 0 ||
                     (b[0] == 'B' && b[1] == 'M'));
}

/* JSON: subsonic-response.error.code. */
static bool json_error_is_not_found(const uint8_t * body, size_t n) {
    cJSON * root = cJSON_ParseWithLength((const char *) body, n);
    const cJSON * resp = cJSON_GetObjectItemCaseSensitive(root, "subsonic-response");
    const cJSON * err = cJSON_GetObjectItemCaseSensitive(resp, "error");
    const cJSON * code = cJSON_GetObjectItemCaseSensitive(err, "code");
    bool found = cJSON_IsNumber(code) && code->valueint == SUBSONIC_ERROR_NOT_FOUND;
    cJSON_Delete(root);
    return found;
}

/* XML: the code attribute of the error element, as in <error code="70" .../>. */
static bool xml_error_is_not_found(const uint8_t * body, size_t n) {
    char head[ERROR_DOC_HEAD];
    size_t len = n < sizeof(head) - 1 ? n : sizeof(head) - 1;
    memcpy(head, body, len);
    head[len] = '\0';
    const char * tag = strstr(head, "<error ");
    const char * end = tag ? strchr(tag, '>') : NULL;
    const char * attr = end ? strstr(tag, " code=\"") : NULL;
    return attr && attr < end && strtol(attr + 7, NULL, 10) == SUBSONIC_ERROR_NOT_FOUND;
}

/* Servers answer a missing cover with an error document, often under HTTP 200. */
static bool is_not_found_doc(const uint8_t * body, size_t n) {
    while (n && (*body == ' ' || *body == '\n' || *body == '\r' || *body == '\t')) body++, n--;
    if (!n) return false;
    return *body == '{' ? json_error_is_not_found(body, n) : xml_error_is_not_found(body, n);
}

static download_t classify(const http_response_t * resp, bool ok) {
    if (!ok) return DOWNLOAD_FAILED;
    if (resp->status == 404) return DOWNLOAD_NO_ART;
    if (resp->status != 200 || !resp->body) return DOWNLOAD_FAILED;
    if (looks_like_image(resp->body, resp->body_len)) return DOWNLOAD_OK;
    return is_not_found_doc(resp->body, resp->body_len) ? DOWNLOAD_NO_ART : DOWNLOAD_FAILED;
}

/* Fetches the cover scaled by the server to size_px. The URL exists only here. */
static download_t download(const subsonic_server_t * server, const char * id, int size_px,
                           http_cancel_token_t * cancel, uint8_t ** body, size_t * n) {
    http_request_t request = {0};
    subsonic_build_cover_art_url(server, id, size_px, request.url, sizeof(request.url));
    request.method = HTTP_METHOD_GET;
    request.verify_tls = server->verify_tls;
    request.connect_timeout_ms = COVER_CONNECT_TIMEOUT_MS;
    request.read_timeout_ms = COVER_READ_TIMEOUT_MS;
    request.total_timeout_ms = COVER_TOTAL_TIMEOUT_MS;
    request.max_response_bytes = (size_t) SUBSONIC_COVER_LARGE_MAX;
    request.redirect_limit = 5;
    http_response_t resp;
    download_t result = classify(&resp, http_request_ex(&request, cancel, &resp));
    if (result == DOWNLOAD_OK) {
        *body = resp.body;
        *n = resp.body_len;
        resp.body = NULL; /* the caller owns it now */
    }
    http_response_free(&resp);
    return result;
}

bool subsonic_cover_large_bytes(const subsonic_server_t * server, const char * cover_id, bool allow_net,
                                http_cancel_token_t * cancel, uint8_t ** bytes, size_t * n) {
    *bytes = NULL;
    *n = 0;
    char file[64];
    if (!cover_file(cover_id, false, SUBSONIC_COVER_LARGE_PX, file, sizeof(file))) return false;
    if (read_blob(server, file, bytes, n)) return true;
    if (!allow_net) return false;
    if (download(server, cover_id, SUBSONIC_COVER_LARGE_PX, cancel, bytes, n) != DOWNLOAD_OK) return false;
    blob_t blob = { NULL, 0, *bytes, *n };
    (void) write_blob(server, file, &blob); /* not kept without a card; still returned */
    return true;
}

static bool decode_cancelled(void * token) { return token && http_cancel_token_is_cancelled(token); }

static cover_decode_result_t thumb_decode(const uint8_t * data, size_t n, int px, http_cancel_token_t * cancel,
                                          uint16_t ** px_out) {
    return cover_decode_to_rgb565_ex(data, (uint32_t) n, px, px, ARTWORK_PRIO_THUMBNAIL, decode_cancelled, cancel,
                                     px_out);
}

void subsonic_cover_large_evict(const subsonic_server_t * server, const char * cover_id) {
    char file[64], path[512];
    if (cover_file(cover_id, false, SUBSONIC_COVER_LARGE_PX, file, sizeof(file)) &&
        subsonic_cache_path(server, file, path, sizeof(path)))
        unlink(path);
}

uint16_t * subsonic_cover_decode_large(const subsonic_server_t * server, const char * cover_id,
                                       uint8_t * bytes, size_t n, int px, bool (*cancelled)(void *), void * user) {
    uint16_t * pixels = NULL;
    cover_decode_result_t res = cover_decode_to_rgb565_ex(bytes, (uint32_t) n, px, px, ARTWORK_PRIO_THUMBNAIL,
                                                          cancelled, user, &pixels);
    free(bytes);
    if (res == COVER_DECODE_OK) return pixels;
    free(pixels);
    if (cover_decode_result_is_permanent(res)) subsonic_cover_large_evict(server, cover_id);
    return NULL;
}

/* Scales a stored large cover. One that cannot be decoded at all is deleted. */
static bool thumb_from_large(const subsonic_server_t * server, const char * id, int px,
                             http_cancel_token_t * cancel, uint16_t ** px_out) {
    uint8_t * data;
    size_t n;
    if (!subsonic_cover_large_bytes(server, id, false, cancel, &data, &n)) return false;
    *px_out = subsonic_cover_decode_large(server, id, data, n, px, decode_cancelled, cancel);
    return *px_out != NULL;
}

static bool thumb_store(const subsonic_server_t * server, const char * file, const uint16_t * pixels, int px) {
    thumb_header_t head = { THUMB_MAGIC, THUMB_VERSION, (uint32_t) px, (uint32_t) px };
    blob_t blob = { &head, sizeof(head), pixels, (size_t) px * (size_t) px * sizeof(uint16_t) };
    return write_blob(server, file, &blob);
}

static bool thumb_fetch_work(const subsonic_server_t * server, const char * id, const char * file, int px,
                             http_cancel_token_t * cancel, uint16_t ** px_out, bool * no_art) {
    if (subsonic_cover_thumb_load(server, id, px, px_out)) return true;
    if (!thumb_from_large(server, id, px, cancel, px_out)) {
        uint8_t * data = NULL;
        size_t n = 0;
        download_t got = download(server, id, px, cancel, &data, &n);
        *no_art = got == DOWNLOAD_NO_ART;
        if (got != DOWNLOAD_OK) return false;
        cover_decode_result_t res = thumb_decode(data, n, px, cancel, px_out);
        free(data);
        if (res != COVER_DECODE_OK) {
            *px_out = NULL;
            return false;
        }
    }
    (void) thumb_store(server, file, *px_out, px);
    return true;
}

bool subsonic_cover_thumb_fetch(const subsonic_server_t * server, const char * cover_id, int px,
                                http_cancel_token_t * cancel, uint16_t ** px_out, bool * no_art) {
    *px_out = NULL;
    *no_art = !cover_id || !cover_id[0];
    char file[64];
    if (*no_art || !cover_file(cover_id, true, px, file, sizeof(file))) return false;
    return thumb_fetch_work(server, cover_id, file, px, cancel, px_out, no_art);
}

void subsonic_covers_prune(const subsonic_server_t * server, int keep_thumbs, int keep_large) {
    const char * dirs[] = { COVER_DIR };
    subsonic_cache_prune_files(server, dirs, 1, ".rgb", true, keep_thumbs);
    subsonic_cache_prune_files(server, dirs, 1, ".img", true, keep_large);
}
