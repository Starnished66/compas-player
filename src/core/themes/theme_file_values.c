#include "theme_file_values.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

void theme_file_trim_trailing(char *s) {
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r' || s[len - 1] == '\n')) {
        s[--len] = '\0';
    }
}

bool theme_file_copy_value(const char *value, char *out, size_t capacity) {
    size_t len = strlen(value);
    if (len >= capacity) return false;
    memcpy(out, value, len + 1);
    theme_file_trim_trailing(out);
    return true;
}

const char *theme_file_skip_leading(const char *s) {
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    return s;
}

bool theme_file_parse_uint32_color(const char *str, uint32_t *out_val) {
    str = theme_file_skip_leading(str);
    if (!*str) return false;

    char *endptr = NULL;
    errno = 0;
    unsigned long val = 0;
    if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
        val = strtoul(str, &endptr, 16);
    } else {
        val = strtoul(str, &endptr, 10);
    }

    if (errno != 0 || endptr == str) return false;
    while (*endptr == ' ' || *endptr == '\t' || *endptr == '\r' || *endptr == '\n') {
        endptr++;
    }
    if (*endptr != '\0') return false;
    if (val > 0xFFFFFF) return false;

    *out_val = (uint32_t)val;
    return true;
}

bool theme_file_parse_int32(const char *str, int32_t *out_val, bool allow_negative) {
    str = theme_file_skip_leading(str);
    if (!*str) return false;

    char *endptr = NULL;
    errno = 0;
    long val = 0;
    if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
        /* Hexadecimal integer (unsigned or positive) */
        unsigned long uval = strtoul(str, &endptr, 16);
        if (errno != 0 || endptr == str) return false;
        if (uval > (unsigned long)INT32_MAX) return false;
        val = (long)uval;
    } else {
        val = strtol(str, &endptr, 10);
        if (errno != 0 || endptr == str) return false;
    }

    while (*endptr == ' ' || *endptr == '\t' || *endptr == '\r' || *endptr == '\n') {
        endptr++;
    }
    if (*endptr != '\0') return false;
    if (!allow_negative && val < 0) return false;
    if (val < INT32_MIN || val > INT32_MAX) return false;

    *out_val = (int32_t)val;
    return true;
}

bool theme_file_parse_bool(const char *str, bool *out_val) {
    str = theme_file_skip_leading(str);
    if (strcmp(str, "true") == 0) {
        *out_val = true;
        return true;
    }
    if (strcmp(str, "false") == 0) {
        *out_val = false;
        return true;
    }
    return false;
}

bool theme_file_is_safe_relpath(const char *path) {
    if (!path || !path[0]) return false;
    if (path[0] == '/') return false;
    if (strstr(path, "..") != NULL) return false;
    if (strstr(path, "\\") != NULL) return false;

    for (const char *p = path; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 32 || c == 127) return false;
    }
    return true;
}

bool theme_file_is_safe_source_path(const char *path) {
    if (!path || !path[0]) return false;
    if (strstr(path, "..") != NULL) return false;
    if (strstr(path, "\\") != NULL) return false;

    for (const char *p = path; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 32 || c == 127) return false;
    }
    return true;
}

bool theme_file_is_valid_ident(const char *str) {
    if (!str || !*str) return false;
    if (!isalpha((unsigned char)*str)) return false;
    for (const char *p = str + 1; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '_') return false;
    }
    return true;
}

/* Plugin registration contract ID charset: [A-Za-z0-9._-]
 * Supports digits-leading, dots, hyphens, and underscores.
 * Must be non-empty and strictly < max_len (to fit destination without truncation). */
bool theme_file_is_valid_tile_id(const char *str, size_t max_len) {
    if (!str || !str[0]) return false;
    size_t len = 0;
    for (const char *p = str; *p; p++, len++) {
        unsigned char c = (unsigned char)*p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) {
            return false;
        }
    }
    if (len >= max_len) {
        return false;
    }
    return true;
}

bool theme_file_is_valid_align(const char *val) {
    return (strcmp(val, "left") == 0 ||
            strcmp(val, "center") == 0 ||
            strcmp(val, "right") == 0);
}

bool theme_file_is_valid_text_size(const char *val) {
    return (strcmp(val, "small") == 0 ||
            strcmp(val, "medium") == 0 ||
            strcmp(val, "large") == 0 ||
            strcmp(val, "mono") == 0);
}
