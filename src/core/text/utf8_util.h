#ifndef UTF8_UTIL_H
#define UTF8_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Truncates a null-terminated UTF-8 string at a safe boundary so no multibyte
 * character is split. Returns the length in bytes of the copied string. */
size_t utf8_truncate_safe(char * dst, const char * src, size_t max_bytes);

/* Length-bounded UTF-8 truncation: examines only up to min(src_len, dst_size - 1)
 * bytes of src without performing heap allocations. Safely truncates at valid
 * character boundaries and sanitizes invalid sequences. */
size_t utf8_truncate_safe_bounded(char * dst, size_t dst_size, const char * src, size_t src_len);

/* Validates and sanitizes a UTF-8 string in place, replacing overlong encodings,
 * UTF-16 surrogates (U+D800..U+DFFF), out-of-range codepoints (> U+10FFFF),
 * and malformed continuation bytes with '?'. */
void utf8_sanitize(char * str);

/* Strict UTF-8 validator for exactly size bytes. Embedded NUL bytes are treated
 * as ordinary ASCII characters and fully examined. Returns true if and only if
 * all size bytes form valid UTF-8. If text is NULL, returns true if and only
 * if size is 0. */
bool utf8_is_valid(const char * text, size_t size);

/* Locale-independent Latin-aware browse collation. Latin accents and common
 * ligatures fold to ASCII, leading punctuation/space/quotes are skipped, and
 * non-Latin codepoints sort after Latin. Invalid UTF-8 bytes have a stable
 * bytewise order after valid non-Latin text. */
int utf8_collate_compare(const char * a, const char * b);
int utf8_collate_identity_compare(const char * a, const char * b);
/* Frozen identity ordering used only to compare persisted v6 catalog keys. */
int utf8_collate_identity_compare_v6(const char * a, const char * b);
/* Writes a zero-terminated, zero-padded sortable byte key. Every emitted key
 * byte is nonzero, so memchr(out, 0, width) is non-NULL only when the full key
 * and terminator fit. Returns true in that case; otherwise callers compare
 * original strings when the prefixes tie. Identity key adds ASCII-folded raw
 * bytes after a nonzero separator. */
bool utf8_collate_prefix(const char * value, unsigned char * out, size_t width);
bool utf8_collate_identity_prefix(const char * value, unsigned char * out, size_t width);

#ifdef __cplusplus
}
#endif

#endif /* UTF8_UTIL_H */
