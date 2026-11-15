#include "icy_metadata.h"
#include "../../../core/text/utf8_util.h"
#include <string.h>
#include <strings.h>
#include <ctype.h>

static bool image_url(const char * url) {
    /* The artwork worker validates the downloaded image, including extensionless URLs. */
    return !strncasecmp(url, "https://", 8) || !strncasecmp(url, "http://", 7);
}

bool icy_metadata_parse(const char * block, icy_metadata_t * state) {
    if (!block || !state) return false;
    icy_metadata_t next = *state;
    next.artwork_url[0] = '\0';
    const char * p = block;
    while (*p) {
        while (*p == ';' || isspace((unsigned char)*p)) ++p;
        const char * key = p;
        const char * equal = strchr(p, '=');
        if (!equal) break;
        p = equal + 1;
        if (*p != '\'' && *p != '"') break;
        char quote = *p++;
        const char * value = p;
        while (*p && !(*p == quote && (!p[1] || p[1] == ';'))) ++p;
        if (!*p) break;
        size_t key_len = (size_t)(equal - key), len = (size_t)(p - value);
        char text[4096];
        if (len >= sizeof(text)) break;
        memcpy(text, value, len); text[len] = '\0';
        if (key_len == 11 && !strncasecmp(key, "StreamTitle", 11) ) {
            utf8_sanitize(text);
            char * split = strstr(text, " - ");
            if (split) {
                *split = '\0';
                utf8_truncate_safe(next.artist, text, sizeof(next.artist));
                utf8_truncate_safe(next.title, split + 3, sizeof(next.title));
            } else {
                next.artist[0] = '\0';
                utf8_truncate_safe(next.title, text, sizeof(next.title));
            }
        } else if (key_len == 9 && !strncasecmp(key, "StreamUrl", 9) && image_url(text) && len < sizeof(next.artwork_url)) {
            memcpy(next.artwork_url, text, len + 1);
        }
        ++p;
    }
    if (!strcmp(next.title, state->title) && !strcmp(next.artist, state->artist) &&
        !strcmp(next.artwork_url, state->artwork_url)) return false;
    next.revision++;
    *state = next;
    return true;
}
