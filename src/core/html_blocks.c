#include "html_blocks.h"
#include "utf8_util.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef enum {
    DROP_NONE = 0,
    DROP_SCRIPT,
    DROP_STYLE,
    DROP_HEAD
} drop_kind_t;

typedef enum {
    BLOCK_NONE = 0,
    BLOCK_P,
    BLOCK_H
} cur_block_kind_t;

const char * html_status_reason(html_status status) {
    switch (status) {
        case HTML_OK: return "ok";
        case HTML_ERR_ARGS: return "bad_args";
        case HTML_ERR_TOO_LARGE: return "input_too_large";
        case HTML_ERR_NOMEM: return "nomem";
        default: return "bad_args";
    }
}

void html_blocks_free(html_blocks * out) {
    if (!out) return;
    if (out->blocks) {
        for (size_t i = 0; i < out->count; i++) {
            free(out->blocks[i].text);
            free(out->blocks[i].alt);
        }
        free(out->blocks);
    }
    memset(out, 0, sizeof(*out));
}

static char * alloc_empty_string(void) {
    char * s = (char *) malloc(1);
    if (s) {
        s[0] = '\0';
    }
    return s;
}

/* Bounds check: total emitted + current scratch + needed <= HTML_MAX_TEXT_BYTES */
static inline bool check_text_cap(size_t total, size_t scratch, size_t add) {
    if (total > HTML_MAX_TEXT_BYTES) return false;
    size_t rem = HTML_MAX_TEXT_BYTES - total;
    if (scratch > rem) return false;
    rem -= scratch;
    return add <= rem;
}

/* Bounds check: attribute value copy <= 64 KiB */
static inline bool check_attr_cap(size_t cur, size_t add) {
    if (cur >= 65536u) return false;
    return add <= (65536u - cur);
}

static bool add_block(html_blocks * out, size_t * capacity, const char * kind, int level, char * text, char * alt) {
    /* Hard cap at HTML_MAX_BLOCKS blocks */
    if (out->count >= HTML_MAX_BLOCKS) {
        free(text);
        free(alt);
        out->truncated = true;
        return true;
    }
    if (out->count == *capacity) {
        size_t new_cap = (*capacity == 0) ? 16u : (*capacity * 2u);
        if (new_cap > HTML_MAX_BLOCKS) {
            new_cap = HTML_MAX_BLOCKS;
        }
        html_block * new_blocks = (html_block *) realloc(out->blocks, new_cap * sizeof(html_block));
        if (!new_blocks) {
            free(text);
            free(alt);
            return false;
        }
        out->blocks = new_blocks;
        *capacity = new_cap;
    }
    out->blocks[out->count].kind = kind;
    out->blocks[out->count].level = level;
    out->blocks[out->count].text = text;
    out->blocks[out->count].alt = alt;
    out->count++;
    return true;
}

static bool flush_text_block(html_blocks * out, size_t * capacity, size_t * total_text_bytes,
                             cur_block_kind_t cur_kind, int cur_level,
                             char * scratch, size_t * scratch_len, bool * in_block) {
    if (!*in_block) {
        return true;
    }
    /* Drop one trailing space if present */
    if (*scratch_len > 0 && scratch[*scratch_len - 1] == ' ') {
        (*scratch_len)--;
    }
    /* Empty text blocks are not emitted */
    if (*scratch_len == 0) {
        *in_block = false;
        return true;
    }
    if (out->count >= HTML_MAX_BLOCKS) {
        out->truncated = true;
        *scratch_len = 0;
        *in_block = false;
        return true;
    }

    char * text_copy = (char *) malloc(*scratch_len + 1);
    if (!text_copy) {
        return false;
    }
    memcpy(text_copy, scratch, *scratch_len);
    text_copy[*scratch_len] = '\0';
    utf8_sanitize(text_copy);

    const char * kind = (cur_kind == BLOCK_H) ? "h" : "p";
    int level = (cur_kind == BLOCK_H) ? cur_level : 0;

    if (!add_block(out, capacity, kind, level, text_copy, NULL)) {
        return false;
    }

    *total_text_bytes += *scratch_len;
    *scratch_len = 0;
    *in_block = false;
    return true;
}

/* Skip until unquoted '>' or EOF */
/* Returns true when the tag ended with "/>". */
static bool skip_to_unquoted_gt(const unsigned char ** pptr, size_t * prem) {
    const unsigned char * p = *pptr;
    size_t r = *prem;
    char quote = 0;
    char prev = 0;
    bool self_closing = false;
    while (r > 0) {
        char c = (char)*p++;
        r--;
        if (quote != 0) {
            if (c == quote) {
                quote = 0;
            }
        } else {
            if (c == '"' || c == '\'') {
                quote = c;
            } else if (c == '>') {
                self_closing = (prev == '/');
                break;
            }
        }
        prev = c;
    }
    *pptr = p;
    *prem = r;
    return self_closing;
}

/* Caller has consumed "<![CDATA[". Stops after "]]>" or at EOF. */
static void skip_until_cdata_end(const unsigned char ** pptr, size_t * prem) {
    const unsigned char * ptr = *pptr;
    size_t rem = *prem;
    while (rem > 0) {
        if (rem >= 3 && ptr[0] == ']' && ptr[1] == ']' && ptr[2] == '>') {
            ptr += 3;
            rem -= 3;
            break;
        }
        ptr++;
        rem--;
    }
    *pptr = ptr;
    *prem = rem;
}

/* script and style bodies are raw text. The matching end tag ends the body;
 * other '<' bytes, including comparisons, are not markup. A CDATA section
 * is markup, so an end tag inside it does not end the body. */
static void skip_rawtext_until_end(const unsigned char ** pptr, size_t * prem, const char * name) {
    const unsigned char * ptr = *pptr;
    size_t rem = *prem;
    size_t name_len = strlen(name);
    while (rem > 0) {
        if (rem >= 9 && strncasecmp((const char *)ptr, "<![CDATA[", 9) == 0) {
            ptr += 9;
            rem -= 9;
            skip_until_cdata_end(&ptr, &rem);
            continue;
        }
        if (rem >= name_len + 3 && ptr[0] == '<' && ptr[1] == '/') {
            if (strncasecmp((const char *)(ptr + 2), name, name_len) == 0) {
                unsigned char end = ptr[2 + name_len];
                if (end == ' ' || end == '\t' || end == '\n' || end == '\r' ||
                    end == '\f' || end == '/' || end == '>') {
                    ptr += 2 + name_len;
                    rem -= 2 + name_len;
                    *pptr = ptr;
                    *prem = rem;
                    skip_to_unquoted_gt(pptr, prem);
                    return;
                }
            }
        }
        ptr++;
        rem--;
    }
    *pptr = ptr;
    *prem = rem;
}

static void encode_codepoint(uint32_t cp, uint8_t out_buf[4], size_t * out_len, bool * is_space) {
    *is_space = false;
    if (cp == 0 || (cp >= 0xD800u && cp <= 0xDFFFu) || cp > 0x10FFFFu) {
        out_buf[0] = '?';
        *out_len = 1;
        return;
    }
    if (cp == 0x20u || cp == 0x09u || cp == 0x0Au || cp == 0x0Du || cp == 0x0Cu) {
        out_buf[0] = ' ';
        *out_len = 1;
        *is_space = true;
        return;
    }
    if (cp <= 0x7Fu) {
        out_buf[0] = (uint8_t)cp;
        *out_len = 1;
        return;
    }
    if (cp <= 0x7FFu) {
        out_buf[0] = (uint8_t)(0xC0u | (cp >> 6));
        out_buf[1] = (uint8_t)(0x80u | (cp & 0x3Fu));
        *out_len = 2;
        return;
    }
    if (cp <= 0xFFFFu) {
        out_buf[0] = (uint8_t)(0xE0u | (cp >> 12));
        out_buf[1] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
        out_buf[2] = (uint8_t)(0x80u | (cp & 0x3Fu));
        *out_len = 3;
        return;
    }
    out_buf[0] = (uint8_t)(0xF0u | (cp >> 18));
    out_buf[1] = (uint8_t)(0x80u | ((cp >> 12) & 0x3Fu));
    out_buf[2] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
    out_buf[3] = (uint8_t)(0x80u | (cp & 0x3Fu));
    *out_len = 4;
}

static size_t decode_entity(const unsigned char * ptr, size_t rem,
                            uint8_t out_buf[4], size_t * out_len, bool * is_space) {
    *is_space = false;
    if (rem < 2 || ptr[0] != '&') return 0;

    if (rem >= 5 && memcmp(ptr, "&amp;", 5) == 0) {
        out_buf[0] = '&'; *out_len = 1; return 5;
    }
    if (rem >= 4 && memcmp(ptr, "&lt;", 4) == 0) {
        out_buf[0] = '<'; *out_len = 1; return 4;
    }
    if (rem >= 4 && memcmp(ptr, "&gt;", 4) == 0) {
        out_buf[0] = '>'; *out_len = 1; return 4;
    }
    if (rem >= 6 && memcmp(ptr, "&quot;", 6) == 0) {
        out_buf[0] = '"'; *out_len = 1; return 6;
    }
    if (rem >= 6 && memcmp(ptr, "&apos;", 6) == 0) {
        out_buf[0] = '\''; *out_len = 1; return 6;
    }
    if (rem >= 6 && memcmp(ptr, "&nbsp;", 6) == 0) {
        out_buf[0] = ' '; *out_len = 1; *is_space = true; return 6;
    }
    if (rem >= 7 && memcmp(ptr, "&mdash;", 7) == 0) {
        out_buf[0] = 0xE2; out_buf[1] = 0x80; out_buf[2] = 0x94; *out_len = 3; return 7;
    }
    if (rem >= 7 && memcmp(ptr, "&ndash;", 7) == 0) {
        out_buf[0] = 0xE2; out_buf[1] = 0x80; out_buf[2] = 0x93; *out_len = 3; return 7;
    }
    if (rem >= 8 && memcmp(ptr, "&hellip;", 8) == 0) {
        out_buf[0] = 0xE2; out_buf[1] = 0x80; out_buf[2] = 0xA6; *out_len = 3; return 8;
    }
    if (rem >= 7 && memcmp(ptr, "&lsquo;", 7) == 0) {
        out_buf[0] = 0xE2; out_buf[1] = 0x80; out_buf[2] = 0x98; *out_len = 3; return 7;
    }
    if (rem >= 7 && memcmp(ptr, "&rsquo;", 7) == 0) {
        out_buf[0] = 0xE2; out_buf[1] = 0x80; out_buf[2] = 0x99; *out_len = 3; return 7;
    }
    if (rem >= 7 && memcmp(ptr, "&ldquo;", 7) == 0) {
        out_buf[0] = 0xE2; out_buf[1] = 0x80; out_buf[2] = 0x9C; *out_len = 3; return 7;
    }
    if (rem >= 7 && memcmp(ptr, "&rdquo;", 7) == 0) {
        out_buf[0] = 0xE2; out_buf[1] = 0x80; out_buf[2] = 0x9D; *out_len = 3; return 7;
    }

    if (ptr[1] == '#') {
        if (rem >= 4 && (ptr[2] == 'x' || ptr[2] == 'X')) {
            size_t k = 3;
            while (k < rem && k <= 12 && ptr[k] != ';') {
                k++;
            }
            if (k < rem && ptr[k] == ';' && k > 3) {
                uint32_t cp = 0;
                bool ok = true;
                for (size_t i = 3; i < k; i++) {
                    unsigned char hc = ptr[i];
                    uint32_t v;
                    if (hc >= '0' && hc <= '9') v = (uint32_t)(hc - '0');
                    else if (hc >= 'a' && hc <= 'f') v = (uint32_t)(hc - 'a' + 10);
                    else if (hc >= 'A' && hc <= 'F') v = (uint32_t)(hc - 'A' + 10);
                    else { ok = false; break; }
                    if (cp > (UINT32_MAX - v) / 16u) {
                        cp = UINT32_MAX;
                    } else {
                        cp = (cp * 16u) + v;
                    }
                }
                if (ok) {
                    encode_codepoint(cp, out_buf, out_len, is_space);
                    return k + 1;
                }
            }
        } else if (rem >= 3) {
            size_t k = 2;
            while (k < rem && k <= 12 && ptr[k] != ';') {
                k++;
            }
            if (k < rem && ptr[k] == ';' && k > 2) {
                uint32_t cp = 0;
                bool ok = true;
                for (size_t i = 2; i < k; i++) {
                    unsigned char dc = ptr[i];
                    if (dc < '0' || dc > '9') { ok = false; break; }
                    uint32_t v = (uint32_t)(dc - '0');
                    if (cp > (UINT32_MAX - v) / 10u) {
                        cp = UINT32_MAX;
                    } else {
                        cp = (cp * 10u) + v;
                    }
                }
                if (ok) {
                    encode_codepoint(cp, out_buf, out_len, is_space);
                    return k + 1;
                }
            }
        }
    }

    return 0;
}

static bool parse_img_tag(const unsigned char ** pptr, size_t * prem,
                          html_blocks * out, size_t * capacity, size_t * total_text_bytes,
                          char * scratch) {
    const unsigned char * ptr = *pptr;
    size_t rem = *prem;

    char * img_src = NULL;
    char * img_alt = NULL;
    bool src_truncated = false;
    bool alt_truncated = false;

    while (rem > 0) {
        while (rem > 0 && (*ptr == ' ' || *ptr == '\t' || *ptr == '\n' || *ptr == '\r' || *ptr == '\f')) {
            ptr++; rem--;
        }
        if (rem == 0) break;
        if (*ptr == '>') {
            ptr++; rem--;
            break;
        }
        if (*ptr == '/') {
            ptr++; rem--;
            continue;
        }

        char attr_name[17];
        size_t attr_len = 0;
        while (rem > 0) {
            unsigned char c = *ptr;
            if (c == '=' || c == '>' || c == '/' || c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') {
                break;
            }
            if (attr_len < 16) {
                attr_name[attr_len++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c;
            }
            ptr++; rem--;
        }
        attr_name[attr_len] = '\0';

        while (rem > 0 && (*ptr == ' ' || *ptr == '\t' || *ptr == '\n' || *ptr == '\r' || *ptr == '\f')) {
            ptr++; rem--;
        }

        bool has_val = false;
        char quote = 0;
        if (rem > 0 && *ptr == '=') {
            ptr++; rem--;
            has_val = true;
            while (rem > 0 && (*ptr == ' ' || *ptr == '\t' || *ptr == '\n' || *ptr == '\r' || *ptr == '\f')) {
                ptr++; rem--;
            }
            if (rem > 0 && (*ptr == '"' || *ptr == '\'')) {
                quote = (char)*ptr;
                ptr++; rem--;
            }
        }

        bool store_src = (strcmp(attr_name, "src") == 0 && img_src == NULL);
        bool store_alt = (strcmp(attr_name, "alt") == 0 && img_alt == NULL);

        if (has_val) {
            size_t val_len = 0;
            bool * tr_flag = store_src ? &src_truncated : &alt_truncated;

            while (rem > 0) {
                if (quote != 0) {
                    if ((char)*ptr == quote) {
                        ptr++; rem--;
                        break;
                    }
                } else {
                    if (*ptr == '>' || *ptr == ' ' || *ptr == '\t' || *ptr == '\n' || *ptr == '\r' || *ptr == '\f') {
                        break;
                    }
                }

                if (store_src || store_alt) {
                    if (*ptr == '&') {
                        uint8_t ebuf[4];
                        size_t elen = 0;
                        bool is_sp = false;
                        size_t cons = decode_entity(ptr, rem, ebuf, &elen, &is_sp);
                        if (cons > 0) {
                            if (check_attr_cap(val_len, elen) && check_text_cap(*total_text_bytes, val_len, elen)) {
                                memcpy(scratch + val_len, ebuf, elen);
                                val_len += elen;
                            } else {
                                *tr_flag = true;
                            }
                            ptr += cons; rem -= cons;
                            continue;
                        }
                    }
                    unsigned char rc = *ptr++; rem--;
                    if (rc == 0) rc = '?';
                    if (check_attr_cap(val_len, 1) && check_text_cap(*total_text_bytes, val_len, 1)) {
                        scratch[val_len++] = (char)rc;
                    } else {
                        *tr_flag = true;
                    }
                } else {
                    ptr++; rem--;
                }
            }

            if (store_src) {
                img_src = (char *) malloc(val_len + 1);
                if (!img_src) goto nomem;
                memcpy(img_src, scratch, val_len);
                img_src[val_len] = '\0';
                utf8_sanitize(img_src);
                *total_text_bytes += val_len;
            } else if (store_alt) {
                img_alt = (char *) malloc(val_len + 1);
                if (!img_alt) goto nomem;
                memcpy(img_alt, scratch, val_len);
                img_alt[val_len] = '\0';
                utf8_sanitize(img_alt);
                *total_text_bytes += val_len;
            }
        }
    }

    if (!img_src) {
        img_src = alloc_empty_string();
        if (!img_src) goto nomem;
    }
    if (!img_alt) {
        img_alt = alloc_empty_string();
        if (!img_alt) {
            free(img_src);
            img_src = NULL;
            goto nomem;
        }
    }

    if (src_truncated || alt_truncated) {
        out->truncated = true;
    }

    if (!add_block(out, capacity, "img", 0, img_src, img_alt)) {
        img_src = NULL;
        img_alt = NULL;
        goto nomem;
    }

    *pptr = ptr;
    *prem = rem;
    return true;

nomem:
    free(img_src);
    free(img_alt);
    *pptr = ptr;
    *prem = rem;
    return false;
}

html_status html_to_blocks(const void * data, size_t len, html_blocks * out) {
    if (!out) {
        return HTML_ERR_ARGS;
    }
    memset(out, 0, sizeof(*out));
    if (len > HTML_MAX_INPUT_BYTES) {
        return HTML_ERR_TOO_LARGE;
    }
    if (!data && len > 0) {
        return HTML_ERR_ARGS;
    }
    if (!data || len == 0) {
        return HTML_OK;
    }

    char * scratch = (char *) malloc(HTML_MAX_TEXT_BYTES);
    if (!scratch) {
        return HTML_ERR_NOMEM;
    }

    size_t capacity = 0;
    size_t total_text_bytes = 0;
    size_t scratch_len = 0;
    cur_block_kind_t cur_kind = BLOCK_NONE;
    int cur_level = 0;
    bool in_block = false;

    drop_kind_t drop_kind = DROP_NONE;
    uint32_t drop_depth = 0;

    const unsigned char * ptr = (const unsigned char *) data;
    size_t rem = len;

    /* Skip UTF-8 BOM if present */
    if (rem >= 3 && ptr[0] == 0xEF && ptr[1] == 0xBB && ptr[2] == 0xBF) {
        ptr += 3;
        rem -= 3;
    }

    while (rem > 0) {
        if (drop_depth > 0) {
            if (drop_kind == DROP_SCRIPT || drop_kind == DROP_STYLE) {
                /* Raw text until the matching end tag. Ordinary '<' is not markup. */
                skip_rawtext_until_end(&ptr, &rem, drop_kind == DROP_SCRIPT ? "script" : "style");
                drop_depth = 0;
                drop_kind = DROP_NONE;
                continue;
            }
            /* Drop region comments <!-- ... --> */
            if (rem >= 4 && memcmp(ptr, "<!--", 4) == 0) {
                ptr += 4; rem -= 4;
                while (rem > 0) {
                    if (rem >= 3 && ptr[0] == '-' && ptr[1] == '-' && ptr[2] == '>') {
                        ptr += 3; rem -= 3;
                        break;
                    }
                    ptr++; rem--;
                }
                continue;
            }
            /* Drop region CDATA <![CDATA[ ... ]]> */
            if (rem >= 9 && strncasecmp((const char *)ptr, "<![CDATA[", 9) == 0) {
                ptr += 9; rem -= 9;
                skip_until_cdata_end(&ptr, &rem);
                continue;
            }
            /* Drop region PI <? ... ?> */
            if (rem >= 2 && ptr[0] == '<' && ptr[1] == '?') {
                ptr += 2; rem -= 2;
                while (rem > 0) {
                    if (rem >= 2 && ptr[0] == '?' && ptr[1] == '>') {
                        ptr += 2; rem -= 2;
                        break;
                    }
                    ptr++; rem--;
                }
                continue;
            }
            /* Drop region other <! ... > */
            if (rem >= 2 && ptr[0] == '<' && ptr[1] == '!') {
                ptr += 2; rem -= 2;
                skip_to_unquoted_gt(&ptr, &rem);
                continue;
            }

            if (*ptr == '<') {
                if (rem == 1) {
                    ptr++; rem--;
                    continue;
                }
                char next = (char)ptr[1];
                if (next == '/') {
                    ptr += 2; rem -= 2;
                    char name[17];
                    size_t name_len = 0;
                    bool name_overflow = false;
                    while (rem > 0) {
                        unsigned char c = *ptr;
                        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '/' || c == '>') {
                            break;
                        }
                        if (name_len < 16) {
                            name[name_len++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c;
                        } else {
                            name_overflow = true;
                        }
                        ptr++; rem--;
                    }
                    name[name_len] = '\0';
                    if (!name_overflow && drop_kind == DROP_HEAD && strcmp(name, "head") == 0) {
                        drop_depth--;
                        if (drop_depth == 0) {
                            drop_kind = DROP_NONE;
                        }
                    }
                    skip_to_unquoted_gt(&ptr, &rem);
                    continue;
                } else if ((next >= 'a' && next <= 'z') || (next >= 'A' && next <= 'Z')) {
                    ptr++; rem--;
                    char name[17];
                    size_t name_len = 0;
                    bool name_overflow = false;
                    while (rem > 0) {
                        unsigned char c = *ptr;
                        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '/' || c == '>') {
                            break;
                        }
                        if (name_len < 16) {
                            name[name_len++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c;
                        } else {
                            name_overflow = true;
                        }
                        ptr++; rem--;
                    }
                    name[name_len] = '\0';
                    /* script/style inside head are still raw text. */
                    if (!name_overflow && (strcmp(name, "script") == 0 || strcmp(name, "style") == 0)) {
                        if (!skip_to_unquoted_gt(&ptr, &rem)) {
                            skip_rawtext_until_end(&ptr, &rem, name);
                        }
                        continue;
                    }
                    bool nests = !name_overflow && drop_kind == DROP_HEAD && strcmp(name, "head") == 0;
                    if (skip_to_unquoted_gt(&ptr, &rem)) {
                        nests = false; /* self-closing tag has no end tag */
                    }
                    if (nests) {
                        /* Trap: cap drop depth at 64 */
                        if (drop_depth >= 64u) {
                            out->truncated = true;
                            ptr += rem;
                            rem = 0;
                            break;
                        }
                        drop_depth++;
                    }
                    continue;
                } else {
                    ptr++; rem--;
                    continue;
                }
            } else {
                ptr++; rem--;
                continue;
            }
        }

        /* drop_depth == 0 */

        /* Comments <!-- ... --> */
        if (rem >= 4 && memcmp(ptr, "<!--", 4) == 0) {
            ptr += 4; rem -= 4;
            while (rem > 0) {
                if (rem >= 3 && ptr[0] == '-' && ptr[1] == '-' && ptr[2] == '>') {
                    ptr += 3; rem -= 3;
                    break;
                }
                ptr++; rem--;
            }
            continue;
        }

        /* CDATA <![CDATA[ ... ]]> */
        if (rem >= 9 && strncasecmp((const char *)ptr, "<![CDATA[", 9) == 0) {
            ptr += 9; rem -= 9;
            while (rem > 0) {
                if (rem >= 3 && ptr[0] == ']' && ptr[1] == ']' && ptr[2] == '>') {
                    ptr += 3; rem -= 3;
                    break;
                }
                unsigned char c = *ptr++; rem--;
                if (c == 0) c = '?';
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') {
                    if (in_block && scratch_len > 0 && scratch[scratch_len - 1] != ' ' && scratch[scratch_len - 1] != '\n') {
                        if (check_text_cap(total_text_bytes, scratch_len, 1)) {
                            scratch[scratch_len++] = ' ';
                        } else {
                            out->truncated = true;
                            ptr += rem; rem = 0;
                            break;
                        }
                    }
                } else {
                    if (!in_block) {
                        cur_kind = BLOCK_P;
                        cur_level = 0;
                        in_block = true;
                        scratch_len = 0;
                    }
                    if (check_text_cap(total_text_bytes, scratch_len, 1)) {
                        scratch[scratch_len++] = (char)c;
                    } else {
                        out->truncated = true;
                        ptr += rem; rem = 0;
                        break;
                    }
                }
            }
            continue;
        }

        /* PI <? ... ?> */
        if (rem >= 2 && ptr[0] == '<' && ptr[1] == '?') {
            ptr += 2; rem -= 2;
            while (rem > 0) {
                if (rem >= 2 && ptr[0] == '?' && ptr[1] == '>') {
                    ptr += 2; rem -= 2;
                    break;
                }
                ptr++; rem--;
            }
            continue;
        }

        /* Other <! ... > */
        if (rem >= 2 && ptr[0] == '<' && ptr[1] == '!') {
            ptr += 2; rem -= 2;
            skip_to_unquoted_gt(&ptr, &rem);
            continue;
        }

        if (*ptr == '<') {
            if (rem == 1) {
                if (!in_block) {
                    cur_kind = BLOCK_P; cur_level = 0; in_block = true; scratch_len = 0;
                }
                if (check_text_cap(total_text_bytes, scratch_len, 1)) {
                    scratch[scratch_len++] = '<';
                } else {
                    out->truncated = true;
                }
                ptr++; rem--;
                continue;
            }
            char next = (char)ptr[1];
            if (next == '/') {
                ptr += 2; rem -= 2;
                char name[17];
                size_t name_len = 0;
                bool name_overflow = false;
                while (rem > 0) {
                    unsigned char c = *ptr;
                    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '/' || c == '>') {
                        break;
                    }
                    if (name_len < 16) {
                        name[name_len++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c;
                    } else {
                        name_overflow = true;
                    }
                    ptr++; rem--;
                }
                name[name_len] = '\0';
                if (!name_overflow) {
                    if (strcmp(name, "p") == 0 || strcmp(name, "div") == 0 ||
                        strcmp(name, "li") == 0 || strcmp(name, "blockquote") == 0 ||
                        (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && name[2] == '\0')) {
                        if (!flush_text_block(out, &capacity, &total_text_bytes, cur_kind, cur_level, scratch, &scratch_len, &in_block)) {
                            goto nomem;
                        }
                    }
                }
                skip_to_unquoted_gt(&ptr, &rem);
                continue;
            } else if ((next >= 'a' && next <= 'z') || (next >= 'A' && next <= 'Z')) {
                ptr++; rem--;
                char name[17];
                size_t name_len = 0;
                bool name_overflow = false;
                while (rem > 0) {
                    unsigned char c = *ptr;
                    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '/' || c == '>') {
                        break;
                    }
                    if (name_len < 16) {
                        name[name_len++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c;
                    } else {
                        name_overflow = true;
                    }
                    ptr++; rem--;
                }
                name[name_len] = '\0';
                if (name_overflow) {
                    skip_to_unquoted_gt(&ptr, &rem);
                    continue;
                }

                if (strcmp(name, "script") == 0) {
                    if (!flush_text_block(out, &capacity, &total_text_bytes, cur_kind, cur_level, scratch, &scratch_len, &in_block)) goto nomem;
                    if (!skip_to_unquoted_gt(&ptr, &rem)) {
                        drop_kind = DROP_SCRIPT;
                        drop_depth = 1;
                    }
                    continue;
                }
                if (strcmp(name, "style") == 0) {
                    if (!flush_text_block(out, &capacity, &total_text_bytes, cur_kind, cur_level, scratch, &scratch_len, &in_block)) goto nomem;
                    if (!skip_to_unquoted_gt(&ptr, &rem)) {
                        drop_kind = DROP_STYLE;
                        drop_depth = 1;
                    }
                    continue;
                }
                if (strcmp(name, "head") == 0) {
                    if (!flush_text_block(out, &capacity, &total_text_bytes, cur_kind, cur_level, scratch, &scratch_len, &in_block)) goto nomem;
                    if (!skip_to_unquoted_gt(&ptr, &rem)) {
                        drop_kind = DROP_HEAD;
                        drop_depth = 1;
                    }
                    continue;
                }

                if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && name[2] == '\0') {
                    if (!flush_text_block(out, &capacity, &total_text_bytes, cur_kind, cur_level, scratch, &scratch_len, &in_block)) goto nomem;
                    cur_kind = BLOCK_H;
                    cur_level = name[1] - '0';
                    in_block = true;
                    scratch_len = 0;
                    skip_to_unquoted_gt(&ptr, &rem);
                    continue;
                }
                if (strcmp(name, "p") == 0 || strcmp(name, "div") == 0 ||
                    strcmp(name, "li") == 0 || strcmp(name, "blockquote") == 0) {
                    if (!flush_text_block(out, &capacity, &total_text_bytes, cur_kind, cur_level, scratch, &scratch_len, &in_block)) goto nomem;
                    cur_kind = BLOCK_P;
                    cur_level = 0;
                    in_block = true;
                    scratch_len = 0;
                    skip_to_unquoted_gt(&ptr, &rem);
                    continue;
                }
                if (strcmp(name, "br") == 0) {
                    if (!in_block) {
                        cur_kind = BLOCK_P;
                        cur_level = 0;
                        in_block = true;
                        scratch_len = 0;
                    }
                    if (scratch_len > 0 && scratch[scratch_len - 1] == ' ') {
                        scratch_len--;
                    }
                    if (check_text_cap(total_text_bytes, scratch_len, 1)) {
                        scratch[scratch_len++] = '\n';
                    } else {
                        out->truncated = true;
                        ptr += rem; rem = 0;
                        break;
                    }
                    skip_to_unquoted_gt(&ptr, &rem);
                    continue;
                }
                if (strcmp(name, "hr") == 0) {
                    if (!flush_text_block(out, &capacity, &total_text_bytes, cur_kind, cur_level, scratch, &scratch_len, &in_block)) goto nomem;
                    char * hr_text = alloc_empty_string();
                    if (!hr_text) goto nomem;
                    if (!add_block(out, &capacity, "hr", 0, hr_text, NULL)) goto nomem;
                    skip_to_unquoted_gt(&ptr, &rem);
                    continue;
                }
                if (strcmp(name, "img") == 0) {
                    if (!flush_text_block(out, &capacity, &total_text_bytes, cur_kind, cur_level, scratch, &scratch_len, &in_block)) goto nomem;
                    if (!parse_img_tag(&ptr, &rem, out, &capacity, &total_text_bytes, scratch)) goto nomem;
                    continue;
                }

                /* Unknown tag */
                skip_to_unquoted_gt(&ptr, &rem);
                continue;
            } else {
                /* Literal '<' (e.g. '<3') */
                if (!in_block) {
                    cur_kind = BLOCK_P; cur_level = 0; in_block = true; scratch_len = 0;
                }
                if (check_text_cap(total_text_bytes, scratch_len, 1)) {
                    scratch[scratch_len++] = '<';
                } else {
                    out->truncated = true;
                    ptr += rem; rem = 0;
                    break;
                }
                ptr++; rem--;
                continue;
            }
        }

        /* Check entities */
        if (*ptr == '&') {
            uint8_t ebuf[4];
            size_t elen = 0;
            bool is_sp = false;
            size_t cons = decode_entity(ptr, rem, ebuf, &elen, &is_sp);
            if (cons > 0) {
                if (is_sp) {
                    if (in_block && scratch_len > 0 && scratch[scratch_len - 1] != ' ' && scratch[scratch_len - 1] != '\n') {
                        if (check_text_cap(total_text_bytes, scratch_len, 1)) {
                            scratch[scratch_len++] = ' ';
                        } else {
                            out->truncated = true;
                            ptr += rem; rem = 0;
                            break;
                        }
                    }
                } else {
                    if (!in_block) {
                        cur_kind = BLOCK_P; cur_level = 0; in_block = true; scratch_len = 0;
                    }
                    if (check_text_cap(total_text_bytes, scratch_len, elen)) {
                        memcpy(scratch + scratch_len, ebuf, elen);
                        scratch_len += elen;
                    } else {
                        out->truncated = true;
                        ptr += rem; rem = 0;
                        break;
                    }
                }
                ptr += cons; rem -= cons;
                continue;
            }
        }

        /* Normal text character */
        unsigned char c = *ptr++; rem--;
        if (c == 0) c = '?';
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') {
            if (in_block && scratch_len > 0 && scratch[scratch_len - 1] != ' ' && scratch[scratch_len - 1] != '\n') {
                if (check_text_cap(total_text_bytes, scratch_len, 1)) {
                    scratch[scratch_len++] = ' ';
                } else {
                    out->truncated = true;
                    ptr += rem; rem = 0;
                    break;
                }
            }
        } else {
            if (!in_block) {
                cur_kind = BLOCK_P; cur_level = 0; in_block = true; scratch_len = 0;
            }
            if (check_text_cap(total_text_bytes, scratch_len, 1)) {
                scratch[scratch_len++] = (char)c;
            } else {
                out->truncated = true;
                ptr += rem; rem = 0;
                break;
            }
        }
    }

    if (!flush_text_block(out, &capacity, &total_text_bytes, cur_kind, cur_level, scratch, &scratch_len, &in_block)) {
        goto nomem;
    }

    free(scratch);
    if (out->count == 0 && out->blocks) {
        free(out->blocks);
        out->blocks = NULL;
    }
    return HTML_OK;

nomem:
    free(scratch);
    html_blocks_free(out);
    return HTML_ERR_NOMEM;
}
