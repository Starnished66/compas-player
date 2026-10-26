#ifndef SDTOOL_JSON_OUT_H
#define SDTOOL_JSON_OUT_H

#include <stdbool.h>

/* One JSON object per stdout line. Keys are added in order; values are
 * escaped. Human-readable logs go to stderr through sdtool_log(). */
typedef struct {
    char text[2048];
    int length;
    bool overflow;
} json_line_t;

void json_begin(json_line_t *line);
void json_string(json_line_t *line, const char *key, const char *value);
void json_integer(json_line_t *line, const char *key, long long value);
void json_boolean(json_line_t *line, const char *key, bool value);
/* Prints the finished line to stdout and flushes it. */
void json_emit(json_line_t *line);

void json_progress(const char *phase, long done, long total);
void json_error(const char *code, const char *message);

void sdtool_log(const char *format, ...) __attribute__((format(printf, 1, 2)));

#endif /* SDTOOL_JSON_OUT_H */
