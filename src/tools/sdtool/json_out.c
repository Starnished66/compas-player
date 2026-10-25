#include "json_out.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static void append(json_line_t *line, const char *text, size_t length) {
    if (line->overflow || line->length + length >= sizeof(line->text)) {
        line->overflow = true;
        return;
    }
    memcpy(line->text + line->length, text, length);
    line->length += (int) length;
    line->text[line->length] = '\0';
}

static void append_escaped(json_line_t *line, const char *value) {
    append(line, "\"", 1);
    for (const unsigned char *p = (const unsigned char *) value; *p; p++) {
        char escape[8];
        switch (*p) {
            case '"': append(line, "\\\"", 2); break;
            case '\\': append(line, "\\\\", 2); break;
            case '\n': append(line, "\\n", 2); break;
            case '\r': append(line, "\\r", 2); break;
            case '\t': append(line, "\\t", 2); break;
            default:
                if (*p < 0x20) {
                    snprintf(escape, sizeof(escape), "\\u%04x", *p);
                    append(line, escape, 6);
                } else {
                    append(line, (const char *) p, 1);
                }
        }
    }
    append(line, "\"", 1);
}

static void append_key(json_line_t *line, const char *key) {
    append(line, line->length > 1 ? "," : "", line->length > 1 ? 1 : 0);
    append_escaped(line, key);
    append(line, ":", 1);
}

void json_begin(json_line_t *line) {
    line->text[0] = '{';
    line->text[1] = '\0';
    line->length = 1;
    line->overflow = false;
}

void json_string(json_line_t *line, const char *key, const char *value) {
    append_key(line, key);
    append_escaped(line, value ? value : "");
}

void json_integer(json_line_t *line, const char *key, long long value) {
    char digits[32];
    append_key(line, key);
    append(line, digits, (size_t) snprintf(digits, sizeof(digits), "%lld", value));
}

void json_boolean(json_line_t *line, const char *key, bool value) {
    append_key(line, key);
    append(line, value ? "true" : "false", value ? 4 : 5);
}

void json_emit(json_line_t *line) {
    printf("%s}\n", line->text);
    fflush(stdout);
}

void json_progress(const char *phase, long done, long total) {
    /* A scan can touch thousands of files a second; consumers need about one
     * update a second, plus the final one. */
    static time_t last_second;
    time_t now = time(NULL);
    bool final = total > 0 && done >= total;
    if (!final && now == last_second) return;
    last_second = now;
    json_line_t line;
    json_begin(&line);
    json_string(&line, "event", "progress");
    json_string(&line, "phase", phase);
    json_integer(&line, "done", done);
    json_integer(&line, "total", total);
    json_emit(&line);
}

void json_error(const char *code, const char *message) {
    json_line_t line;
    json_begin(&line);
    json_string(&line, "event", "error");
    json_string(&line, "code", code);
    json_string(&line, "message", message);
    json_emit(&line);
}

void sdtool_log(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
}
