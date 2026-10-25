#ifndef THEME_FILE_VALUES_H
#define THEME_FILE_VALUES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Private lexical helpers for the theme parser; no file or candidate ownership. */
void theme_file_trim_trailing(char *s);
bool theme_file_copy_value(const char *value, char *out, size_t capacity);
const char *theme_file_skip_leading(const char *s);
bool theme_file_parse_uint32_color(const char *str, uint32_t *out_val);
bool theme_file_parse_int32(const char *str, int32_t *out_val, bool allow_negative);
bool theme_file_parse_bool(const char *str, bool *out_val);
bool theme_file_is_safe_relpath(const char *path);
bool theme_file_is_safe_source_path(const char *path);
bool theme_file_is_valid_ident(const char *str);
bool theme_file_is_valid_tile_id(const char *str, size_t max_len);
bool theme_file_is_valid_align(const char *val);
bool theme_file_is_valid_text_size(const char *val);

#endif /* THEME_FILE_VALUES_H */
