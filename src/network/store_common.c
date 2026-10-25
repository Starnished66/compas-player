#include "store_common.h"
#include "i18n.h"

#include "mbedtls/sha256.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

void store_error_text(char * out, size_t size, const char * text) {
    if (out && size) {
        snprintf(out, size, "%s", text);
    }
}

bool store_start_thread(void * (*fn)(void *), void * arg) {
    pthread_attr_t a;
    pthread_t t;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&a, 256 * 1024);
    bool ok = pthread_create(&t, &a, fn, arg) == 0;
    pthread_attr_destroy(&a);
    return ok;
}

bool store_ascii_digit(char c) {
    return c >= '0' && c <= '9';
}

bool store_ascii_lower(char c) {
    return c >= 'a' && c <= 'z';
}

static bool ascii_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool store_ascii_alnum(char c) {
    return ascii_alpha(c) || store_ascii_digit(c);
}

bool store_token_valid(const char * s, size_t max, bool dot) {
    if (!s || !*s || strlen(s) > max) {
        return false;
    }
    for (; *s; s++) {
        if (!store_ascii_alnum(*s) && *s != '_' && *s != '-' && (!dot || *s != '.')) {
            return false;
        }
    }
    return true;
}

bool store_version_valid(const char * s) {
    if (!s || !*s || strlen(s) >= 32) {
        return false;
    }
    int dots = 0;
    bool digit = false;
    for (; *s; s++) {
        if (store_ascii_digit(*s)) {
            digit = true;
        } else if (*s == '.' && digit && dots < 3) {
            dots++;
            digit = false;
        } else {
            return false;
        }
    }
    return digit;
}

bool store_sha_valid(const char * s) {
    if (!s || strlen(s) != 64) {
        return false;
    }
    for (int i = 0; i < 64; i++) {
        if (!store_ascii_digit(s[i]) && !(s[i] >= 'a' && s[i] <= 'f')) {
            return false;
        }
    }
    return true;
}

bool store_safe_text(const char * s, size_t max, bool allow_empty) {
    if (!s || strlen(s) > max || (!allow_empty && !*s)) {
        return false;
    }
    for (; *s; s++) {
        if ((unsigned char) *s < 32 || (unsigned char) *s == 127) {
            return false;
        }
    }
    return true;
}

bool store_dest_path_valid(const char * dest) {
    if (!dest || !*dest || dest[0] == '/' || strlen(dest) > 255) {
        return false;
    }
    for (const char * p = dest; *p; p++) {
        if (!(store_ascii_alnum(*p) || *p == '.' || *p == '_' || *p == ' ' || *p == '/' || *p == '-')) {
            return false;
        }
    }
    const char * seg = dest;
    for (const char * p = dest; ; p++) {
        if (*p == '/' || *p == '\0') {
            size_t n = (size_t)(p - seg);
            if (!n || (n == 1 && seg[0] == '.') || (n == 2 && seg[0] == '.' && seg[1] == '.')) {
                return false;
            }
            if (!*p) {
                break;
            }
            seg = p + 1;
        }
    }
    return true;
}

/* Two destinations that cannot both exist: the same path, or one is a
 * folder the other lives in ("Data/a" and "Data/a/b"). */
bool store_dests_collide(const char * a, const char * b) {
    size_t la = strlen(a), lb = strlen(b);
    if (la == lb) {
        return strcasecmp(a, b) == 0;
    }
    const char * shorter = la < lb ? a : b;
    const char * longer = la < lb ? b : a;
    size_t n = la < lb ? la : lb;
    return strncasecmp(shorter, longer, n) == 0 && longer[n] == '/';
}

int store_version_compare(const char * a, const char * b) {
    while (*a || *b) {
        const char * a_end = a;
        const char * b_end = b;
        while (store_ascii_digit(*a_end)) {
            a_end++;
        }
        while (store_ascii_digit(*b_end)) {
            b_end++;
        }
        const char * a_num = a;
        const char * b_num = b;
        while (a_num < a_end && *a_num == '0') {
            a_num++;
        }
        while (b_num < b_end && *b_num == '0') {
            b_num++;
        }
        size_t a_len = (size_t) (a_end - a_num);
        size_t b_len = (size_t) (b_end - b_num);
        if (a_len != b_len) {
            return a_len < b_len ? -1 : 1;
        }
        int cmp = a_len ? memcmp(a_num, b_num, a_len) : 0;
        if (cmp) {
            return cmp < 0 ? -1 : 1;
        }
        a = *a_end == '.' ? a_end + 1 : a_end;
        b = *b_end == '.' ? b_end + 1 : b_end;
    }
    return 0;
}

const cJSON * store_json_field(const cJSON * o, const char * name) {
    return cJSON_GetObjectItemCaseSensitive(o, name);
}

const char * store_json_string(const cJSON * o, const char * name) {
    const cJSON * v = store_json_field(o, name);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

bool store_json_uint(const cJSON * o, const char * name, uint32_t min, uint32_t max, uint32_t * out) {
    const cJSON * v = store_json_field(o, name);
    if (!cJSON_IsNumber(v) || v->valuedouble < min || v->valuedouble > max ||
        v->valuedouble != (double) (uint32_t) v->valuedouble) {
        return false;
    }
    *out = (uint32_t) v->valuedouble;
    return true;
}

bool store_hash_file(const char * path, uint64_t * size, char hex[65]) {
    FILE * f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    bool ok = mbedtls_sha256_starts(&c, 0) == 0;
    uint8_t buf[8192], dig[32];
    uint64_t total = 0;
    size_t n;
    while (ok && (n = fread(buf, 1, sizeof(buf), f)) > 0) {
        ok = mbedtls_sha256_update(&c, buf, n) == 0;
        total += n;
    }
    if (ferror(f)) {
        ok = false;
    }
    fclose(f);
    if (ok) {
        ok = mbedtls_sha256_finish(&c, dig) == 0;
    }
    mbedtls_sha256_free(&c);
    if (!ok) {
        return false;
    }
    for (int i = 0; i < 32; i++) {
        snprintf(hex + i * 2, 3, "%02x", dig[i]);
    }
    *size = total;
    return true;
}

bool store_download_verified(const char * url, const char * dest, uint32_t size, const char * sha256,
                             size_t max_bytes, http_progress_cb_t progress, void * context) {
    int status = 0;
    if (!http_get_to_file_redirects(url, true, dest, max_bytes, progress, context, STORE_TIMEOUT_CONNECT,
                                    STORE_TIMEOUT_READ, NULL, 5, &status)) {
        return false;
    }
    char sha[65];
    uint64_t got;
    return store_hash_file(dest, &got, sha) && got == size && strcmp(sha, sha256) == 0;
}

static const char * connection_error(const char * e, const char * generic) {
    if (e && strcmp(e, HTTP_ERR_TLS) == 0) {
        return TR("Secure connection failed. Check Wi-Fi and the date and time.");
    }
    if (e && (strcmp(e, HTTP_ERR_DNS) == 0 || strcmp(e, HTTP_ERR_CONNECT) == 0 ||
              strcmp(e, HTTP_ERR_CONNECT_TIMEOUT) == 0)) {
        return TR("Cannot reach GitHub. Check the Wi-Fi connection.");
    }
    if (e && strcmp(e, HTTP_ERR_TIMEOUT) == 0) {
        return TR("GitHub did not respond in time. Try again.");
    }
    return generic;
}

bool store_fetch_index(const char * url, size_t limit, const char * generic_error,
                       http_response_t * response, const char ** error) {
    http_request_t req;
    memset(&req, 0, sizeof(req));
    snprintf(req.url, sizeof(req.url), "%s", url);
    req.method = HTTP_METHOD_GET;
    req.verify_tls = true;
    req.connect_timeout_ms = STORE_TIMEOUT_CONNECT;
    req.read_timeout_ms = STORE_TIMEOUT_READ;
    req.total_timeout_ms = 45000;
    req.max_response_bytes = limit;
    req.redirect_limit = 5;
    bool ok = http_request_ex(&req, NULL, response);
    if (!ok || !response->body) {
        *error = connection_error(ok ? NULL : response->error, generic_error);
        http_response_free(response);
        return false;
    }
    if (response->status != 200) {
        *error = generic_error;
        http_response_free(response);
        return false;
    }
    return true;
}
