#define _POSIX_C_SOURCE 200809L

#include "selector_ui.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "board_config.h"
#include "selector_font_pin.h"
#include "selector_internal.h"
#include "selector_logos.h"
#include "selector_style.h"

/* Reuse LVGL's vendored tiny_ttf rasterizer without initializing LVGL. The
 * font is opened once and stb's streaming callbacks seek/read that FILE for
 * each glyph; the target asset is local and capped below 6 MiB. */
static void ttf_stream_read(FILE * stream, void * destination, size_t count);
static void ttf_stream_seek(FILE * stream, long offset);
#define STBTT_STREAM_TYPE FILE*
#define STBTT_STREAM_READ(s, x, y) ttf_stream_read(s, x, y)
#define STBTT_STREAM_SEEK(s, x) ttf_stream_seek(s, x)
#define STBTT_STATIC
#define STBRP_STATIC
#define STB_RECT_PACK_IMPLEMENTATION
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_HEAP_FACTOR_SIZE_32 50
#define STBTT_HEAP_FACTOR_SIZE_128 20
#define STBTT_HEAP_FACTOR_SIZE_DEFAULT 10
#include "../../lvgl/src/libs/tiny_ttf/stb_rect_pack.h"
#include "../../lvgl/src/libs/tiny_ttf/stb_truetype_htcw.h"

#define DEFAULT_FONT_PATH "/usr/resource/fonts/NotoSans-Regular.ttf"
#define DEFAULT_MEDIUM_FONT_PATH "/usr/resource/fonts/NotoSans-Medium.ttf"
#define FALLBACK_FONT_PATH "/usr/resource/fonts/Montserrat-Medium.ttf"
#define MAX_FONT_BYTES (6u * 1024u * 1024u)
#define MONTSERRAT_MEDIUM_BYTES 243180u
#define MONTSERRAT_MEDIUM_CRC32 0xE84615C2u

typedef struct { int r, g, b; } rgb_t;

uint16_t * sel_canvas;
int sel_canvas_width;
int sel_canvas_height;
size_t sel_canvas_stride;
int sel_clip_top;
int sel_clip_bottom;
static FILE * font_file;
static FILE * font_medium_file;
static stbtt_fontinfo font_info;
static stbtt_fontinfo font_medium_info;
static int font_ready;
static int font_medium_ready;
static int font_ascent;
static int font_descent;
static int font_line_gap;
static int font_medium_ascent;
static int font_medium_descent;
static int font_medium_line_gap;

typedef struct { FILE * file; size_t limit; } ttf_file_slot_t;
static ttf_file_slot_t ttf_slots[2];

static void ttf_register(FILE * stream, size_t size) {
    if (!stream) return;
    size_t limit = size > MAX_FONT_BYTES ? MAX_FONT_BYTES : size;
    for (int i = 0; i < 2; i++) {
        if (ttf_slots[i].file == stream || ttf_slots[i].file == NULL) {
            ttf_slots[i].file = stream;
            ttf_slots[i].limit = limit;
            return;
        }
    }
}

static void ttf_unregister(FILE * stream) {
    for (int i = 0; i < 2; i++) {
        if (ttf_slots[i].file == stream) {
            ttf_slots[i].file = NULL;
            ttf_slots[i].limit = 0;
        }
    }
}

static void ttf_clear_slots(void) {
    ttf_slots[0].file = ttf_slots[1].file = NULL;
    ttf_slots[0].limit = ttf_slots[1].limit = 0;
}

static size_t ttf_file_limit(FILE * stream) {
    for (int i = 0; i < 2; i++)
        if (ttf_slots[i].file == stream) return ttf_slots[i].limit;
    return 0;
}

static void ttf_stream_seek(FILE * stream, long offset) {
    /* stb's parser trusts offsets inside the TTF. Clamp malformed offsets to
     * EOF so its stream callbacks can never read outside the validated file.
     * Limit is taken from the FILE so Regular and Medium can be open together. */
    size_t limit = ttf_file_limit(stream);
    if (offset < 0 || (uintmax_t)offset > limit)
        offset = (long)limit;
    clearerr(stream);
    (void)fseek(stream, offset, SEEK_SET);
}

static void ttf_stream_read(FILE * stream, void * destination, size_t count) {
    memset(destination, 0, count);
    size_t limit = ttf_file_limit(stream);
    long position = ftell(stream);
    if (position < 0 || (uintmax_t)position >= limit) return;
    size_t remaining = limit - (size_t)position;
    if (count > remaining) count = remaining;
    (void)fread(destination, 1, count, stream);
}

static rgb_t rgb(uint32_t value) {
    rgb_t c = { (int)((value >> 16) & 255), (int)((value >> 8) & 255), (int)(value & 255) };
    return c;
}

static uint16_t pack565(uint32_t value) {
    rgb_t c = rgb(value);
    return (uint16_t)(((unsigned)c.r >> 3 << 11) | ((unsigned)c.g >> 2 << 5) | ((unsigned)c.b >> 3));
}

void sel_put_pixel(int x, int y, uint32_t color, unsigned alpha) {
    if (!sel_canvas || x < 0 || y < sel_clip_top || x >= sel_canvas_width || y >= sel_canvas_height ||
        y >= sel_clip_bottom || alpha == 0) return;
    if (alpha >= 255) {
        sel_canvas[(size_t)y * sel_canvas_stride + (size_t)x] = pack565(color);
        return;
    }
    uint16_t old = sel_canvas[(size_t)y * sel_canvas_stride + (size_t)x];
    rgb_t bg = { (int)(((old >> 11) & 31) * 255 / 31),
                 (int)(((old >> 5) & 63) * 255 / 63),
                 (int)((old & 31) * 255 / 31) };
    rgb_t fg = rgb(color);
    unsigned inv = 255 - alpha;
    uint32_t blended = (uint32_t)((fg.r * alpha + bg.r * inv + 127) / 255) << 16 |
                       (uint32_t)((fg.g * alpha + bg.g * inv + 127) / 255) << 8 |
                       (uint32_t)((fg.b * alpha + bg.b * inv + 127) / 255);
    sel_canvas[(size_t)y * sel_canvas_stride + (size_t)x] = pack565(blended);
}

void sel_blit_a8(int x, int y, const uint8_t * pixels, int width, int height, uint32_t color) {
    if (!pixels || width <= 0 || height <= 0) return;
    for (int row = 0; row < height; row++) {
        const uint8_t * src = pixels + (size_t)row * (size_t)width;
        for (int col = 0; col < width; col++)
            sel_put_pixel(x + col, y + row, color, src[col]);
    }
}

void sel_blit_rgba(int x, int y, const uint8_t * pixels, int width, int height) {
    if (!pixels || width <= 0 || height <= 0) return;
    for (int row = 0; row < height; row++) {
        const uint8_t * src = pixels + (size_t)row * (size_t)width * 4u;
        for (int col = 0; col < width; col++) {
            const uint8_t * p = src + (size_t)col * 4u;
            uint32_t color = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
            sel_put_pixel(x + col, y + row, color, p[3]);
        }
    }
}

void sel_fill_rect(int x, int y, int w, int h, uint32_t color) {
    if (w <= 0 || h <= 0) return;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < sel_clip_top ? sel_clip_top : y;
    int x1 = x > sel_canvas_width - w ? sel_canvas_width : x + w;
    int y1 = y > sel_canvas_height - h ? sel_canvas_height : y + h;
    if (y1 > sel_clip_bottom) y1 = sel_clip_bottom;
    for (int py = y0; py < y1; py++)
        for (int px = x0; px < x1; px++)
            sel_canvas[(size_t)py * sel_canvas_stride + (size_t)px] = pack565(color);
}

static int integer_sqrt(int value) {
    int root = 0;
    while ((root + 1) * (root + 1) <= value) root++;
    return root;
}

void sel_line(int x0, int y0, int x1, int y1, uint32_t color, int thickness) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    int radius = thickness / 2;
    for (;;) {
        sel_fill_rect(x0 - radius, y0 - radius, thickness, thickness, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void sel_rounded_fill(int x, int y, int w, int h, int radius, uint32_t color) {
    if (w <= 0 || h <= 0) return;
    if (radius < 0) radius = 0;
    if (radius > w / 2) radius = w / 2;
    if (radius > h / 2) radius = h / 2;
    for (int row = 0; row < h; row++) {
        int inset = 0;
        if (row < radius) {
            int dy = radius - row - 1;
            inset = radius - integer_sqrt(radius * radius - dy * dy);
        } else if (row >= h - radius) {
            int dy = row - (h - radius);
            inset = radius - integer_sqrt(radius * radius - dy * dy);
        }
        sel_fill_rect(x + inset, y + row, w - 2 * inset, 1, color);
    }
}

/* Built-in 5x7 fallback, rendered at a size derived from the requested text
 * size. It intentionally covers the selector's Latin labels and digits; the
 * accented a in Compás maps to A when the optional external face is absent. */
static uint8_t fallback_glyph(uint32_t cp, int row) {
    static const uint8_t letters[26][7] = {
        {14,17,17,31,17,17,17},{30,17,17,30,17,17,30},{14,17,16,16,16,17,14},
        {30,17,17,17,17,17,30},{31,16,16,30,16,16,31},{31,16,16,30,16,16,16},
        {14,17,16,23,17,17,15},{17,17,17,31,17,17,17},{14,4,4,4,4,4,14},
        {7,2,2,2,18,18,12},{17,18,20,24,20,18,17},{16,16,16,16,16,16,31},
        {17,27,21,21,17,17,17},{17,25,21,19,17,17,17},{14,17,17,17,17,17,14},
        {30,17,17,30,16,16,16},{14,17,17,17,21,18,13},{30,17,17,30,20,18,17},
        {15,16,16,14,1,1,30},{31,4,4,4,4,4,4},{17,17,17,17,17,17,14},
        {17,17,17,17,17,10,4},{17,17,17,21,21,21,10},{17,17,10,4,10,17,17},
        {17,17,10,4,4,4,4},{31,1,2,4,8,16,31}
    };
    static const uint8_t digits[10][7] = {
        {14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},
        {30,1,1,14,1,1,30},{2,6,10,18,31,2,2},{31,16,16,30,1,1,30},
        {14,16,16,30,17,17,14},{31,1,2,4,8,8,8},{14,17,17,14,17,17,14},
        {14,17,17,15,1,1,14}
    };
    if (row < 0 || row >= 7) return 0;
    if (cp >= 'a' && cp <= 'z') cp -= ('a' - 'A');
    if (cp == 0x00e1) cp = 'A';
    if (cp >= 'A' && cp <= 'Z') return letters[cp - 'A'][row];
    if (cp >= '0' && cp <= '9') return digits[cp - '0'][row];
    switch (cp) {
        case ' ': return 0;
        case '-': return row == 3 ? 14 : 0;
        case ':': return row == 2 || row == 5 ? 4 : 0;
        case '.': return row == 6 ? 4 : 0;
        case ',': return row == 5 ? 4 : row == 6 ? 2 : 0;
        case '/': return (uint8_t)(1u << (row < 5 ? 4 - row : 0));
        case '+': return row == 3 ? 14 : row == 1 || row == 2 || row == 4 || row == 5 ? 4 : 0;
        default: return row == 0 || row == 6 ? 14 : row == 1 || row == 5 ? 17 : 2;
    }
}

static uint32_t decode_utf8(const unsigned char ** p) {
    const unsigned char *s = *p;
    if (*s < 0x80) { *p = s + 1; return *s; }
    if (s[1] && (*s & 0xe0) == 0xc0 && (s[1] & 0xc0) == 0x80) {
        *p = s + 2; return ((uint32_t)(s[0] & 0x1f) << 6) | (s[1] & 0x3f);
    }
    if (s[1] && s[2] && (*s & 0xf0) == 0xe0 && (s[1] & 0xc0) == 0x80 && (s[2] & 0xc0) == 0x80) {
        *p = s + 3; return ((uint32_t)(s[0] & 0x0f) << 12) | ((uint32_t)(s[1] & 0x3f) << 6) | (s[2] & 0x3f);
    }
    if (s[1] && s[2] && s[3] && (*s & 0xf8) == 0xf0 && (s[1] & 0xc0) == 0x80 && (s[2] & 0xc0) == 0x80 && (s[3] & 0xc0) == 0x80) {
        *p = s + 4; return ((uint32_t)(s[0] & 7) << 18) | ((uint32_t)(s[1] & 0x3f) << 12) |
                             ((uint32_t)(s[2] & 0x3f) << 6) | (s[3] & 0x3f);
    }
    *p = s + 1;
    return '?';
}

static int text_width_face(const stbtt_fontinfo * info, const char * text, int px) {
    if (!font_ready || !info) {
        int scale = px / 7;
        if (scale < 1) scale = 1;
        int count = 0;
        for (const unsigned char *p = (const unsigned char *)text; *p; ) {
            uint32_t cp = decode_utf8(&p);
            count += cp == 0x2026 ? 3 : 1;
        }
        return count * 6 * scale;
    }
    float scale = stbtt_ScaleForPixelHeight(info, (float)px);
    float advance = 0.0f;
    int previous = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; ) {
        uint32_t cp = decode_utf8(&p);
        int glyph = stbtt_FindGlyphIndex(info, (int)cp);
        int adv, lsb;
        if (previous) advance += stbtt_GetGlyphKernAdvance(info, previous, glyph) * scale;
        stbtt_GetGlyphHMetrics(info, glyph, &adv, &lsb);
        advance += adv * scale;
        previous = glyph;
    }
    return (int)(advance + 0.5f);
}

int sel_text_width(const char * text, int px) {
    return text_width_face(&font_info, text, px);
}

int sel_text_width_medium(const char * text, int px) {
    return text_width_face(font_medium_ready ? &font_medium_info : &font_info, text, px);
}

static void draw_text_face(const stbtt_fontinfo * info, int ascent, int x, int top,
                          const char * text, int px, uint32_t color) {
    if (!font_ready || !info) {
        int scale = px / 7;
        if (scale < 1) scale = 1;
        int cursor = x;
        for (const unsigned char *p = (const unsigned char *)text; *p; ) {
            uint32_t cp = decode_utf8(&p);
            int repeats = cp == 0x2026 ? 3 : 1;
            if (cp == 0x2026) cp = '.';
            for (int n = 0; n < repeats; n++) {
                for (int row = 0; row < 7; row++) {
                    uint8_t bits = fallback_glyph(cp, row);
                    for (int col = 0; col < 5; col++)
                        if (bits & (1u << (4 - col)))
                            sel_fill_rect(cursor + col * scale, top + row * scale, scale, scale, color);
                }
                cursor += 6 * scale;
            }
        }
        return;
    }

    float scale = stbtt_ScaleForPixelHeight(info, (float)px);
    int baseline = top + (int)(ascent * scale + 0.5f);
    int cursor = x;
    int previous = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; ) {
        uint32_t cp = decode_utf8(&p);
        int glyph = stbtt_FindGlyphIndex(info, (int)cp);
        int advance, lsb, x0, y0, x1, y1;
        if (previous) cursor += (int)(stbtt_GetGlyphKernAdvance(info, previous, glyph) * scale + 0.5f);
        stbtt_GetGlyphHMetrics(info, glyph, &advance, &lsb);
        stbtt_GetCodepointBitmapBox(info, (int)cp, scale, scale, &x0, &y0, &x1, &y1);
        int w = x1 - x0, h = y1 - y0;
        if (w > 0 && h > 0) {
            unsigned char * bitmap = stbtt_GetCodepointBitmap(info, scale, scale, (int)cp, NULL, NULL, NULL, NULL);
            if (bitmap) {
                for (int gy = 0; gy < h; gy++)
                    for (int gx = 0; gx < w; gx++)
                        sel_put_pixel(cursor + x0 + gx, baseline + y0 + gy, color, bitmap[(size_t)gy * (size_t)w + (size_t)gx]);
                stbtt_FreeBitmap(bitmap, NULL);
            }
        }
        cursor += (int)(advance * scale + 0.5f);
        previous = glyph;
    }
}

void sel_draw_text(int x, int top, const char * text, int px, uint32_t color) {
    draw_text_face(&font_info, font_ascent, x, top, text, px, color);
}

void sel_draw_text_medium(int x, int top, const char * text, int px, uint32_t color) {
    if (font_medium_ready)
        draw_text_face(&font_medium_info, font_medium_ascent, x, top, text, px, color);
    else
        draw_text_face(&font_info, font_ascent, x, top, text, px, color);
}

void sel_draw_text_centered(int center_x, int top, const char * text, int px, uint32_t color) {
    sel_draw_text(center_x - sel_text_width(text, px) / 2, top, text, px, color);
}

void sel_fit_text(char out[128], const char * input, int max_width, int px, bool medium) {
    if (!input) input = "";
    const char * ell = font_ready ? "\xE2\x80\xA6" : "...";
    const stbtt_fontinfo * info = (medium && font_medium_ready) ? &font_medium_info : &font_info;
    int ell_w = text_width_face(info, ell, px);
    size_t last_fit = 0, used = 0;
    int full_width = 0;
    int finished = 1;
    const unsigned char * p = (const unsigned char *)input;

    if (font_ready && info) {
        float scale = stbtt_ScaleForPixelHeight(info, (float)px);
        float advance = 0.0f;
        int previous = 0;
        while (*p) {
            const unsigned char * before = p;
            uint32_t cp = decode_utf8(&p);
            size_t nbytes = (size_t)(p - before);
            if (used + nbytes > 124) { finished = 0; break; }
            int glyph = stbtt_FindGlyphIndex(info, (int)cp);
            int adv, lsb;
            if (previous) advance += stbtt_GetGlyphKernAdvance(info, previous, glyph) * scale;
            stbtt_GetGlyphHMetrics(info, glyph, &adv, &lsb);
            advance += adv * scale;
            int w = (int)(advance + 0.5f);
            memcpy(out + used, before, nbytes);
            used += nbytes;
            previous = glyph;
            full_width = w;
            if (w + ell_w <= max_width) last_fit = used;
            if (w > max_width) { finished = 0; break; }
        }
        if (*p) finished = 0;
    } else {
        int scale = px / 7;
        if (scale < 1) scale = 1;
        int cell = 6 * scale;
        while (*p) {
            const unsigned char * before = p;
            uint32_t cp = decode_utf8(&p);
            size_t nbytes = (size_t)(p - before);
            if (used + nbytes > 124) { finished = 0; break; }
            memcpy(out + used, before, nbytes);
            used += nbytes;
            full_width += cp == 0x2026 ? 3 * cell : cell;
            if (full_width + ell_w <= max_width) last_fit = used;
            if (full_width > max_width) { finished = 0; break; }
        }
        if (*p) finished = 0;
    }
    out[used] = '\0';
    if (finished && full_width <= max_width) return;
    out[last_fit] = '\0';
    size_t ell_len = strlen(ell);
    if (max_width >= ell_w && last_fit + ell_len <= 127)
        memcpy(out + last_fit, ell, ell_len + 1);
}

static int margin(void) { return selector_margin(sel_canvas_width); }
static int tool_list_top(void) { return selector_tool_list_top(sel_canvas_width); }
static int tool_row_h(void) { return selector_tool_row_h(sel_canvas_width); }
static int tool_gap(void) { return selector_tool_gap(sel_canvas_width); }
int sel_list_viewport_bottom(void) { return sel_canvas_height - BREEZE_LIST_NOTICE_BAND; }

void sel_draw_music_note(int x, int y, int size, uint32_t color) {
    int sw = size >= 36 ? 3 : 2;
    sel_line(x + size * 34 / 100, y + size * 72 / 100, x + size * 34 / 100, y + size * 25 / 100, color, sw);
    sel_line(x + size * 34 / 100, y + size * 25 / 100, x + size * 78 / 100, y + size * 16 / 100, color, sw);
    sel_line(x + size * 78 / 100, y + size * 16 / 100, x + size * 78 / 100, y + size * 61 / 100, color, sw);
    sel_rounded_fill(x + size * 10 / 100, y + size * 64 / 100, size * 25 / 100, size * 17 / 100, size / 12, color);
    sel_rounded_fill(x + size * 54 / 100, y + size * 53 / 100, size * 25 / 100, size * 17 / 100, size / 12, color);
}

void sel_draw_mountain(int x, int y, int size, uint32_t color) {
    int sw = size >= 36 ? 3 : 2;
    sel_line(x + 3, y + size - 5, x + size * 37 / 100, y + size * 35 / 100, color, sw);
    sel_line(x + size * 37 / 100, y + size * 35 / 100, x + size * 52 / 100, y + size * 55 / 100, color, sw);
    sel_line(x + size * 52 / 100, y + size * 55 / 100, x + size * 70 / 100, y + size * 28 / 100, color, sw);
    sel_line(x + size * 70 / 100, y + size * 28 / 100, x + size - 3, y + size - 5, color, sw);
    sel_line(x + 3, y + size - 5, x + size - 3, y + size - 5, color, sw);
    sel_line(x + size * 22 / 100, y + size * 68 / 100, x + size * 34 / 100, y + size * 55 / 100, color, sw);
    sel_line(x + size * 34 / 100, y + size * 55 / 100, x + size * 45 / 100, y + size * 68 / 100, color, sw);
}

void sel_draw_tools_icon(int x, int y, int size, uint32_t color) {
    int sw = size >= 36 ? 3 : 2;
    sel_line(x + 8, y + 8, x + size - 8, y + size - 8, color, sw);
    sel_line(x + size - 8, y + 8, x + 8, y + size - 8, color, sw);
    sel_line(x + 4, y + 11, x + 12, y + 4, color, sw);
    sel_line(x + size - 12, y + size - 4, x + size - 4, y + size - 12, color, sw);
}

typedef struct {
    uint32_t offset;
    uint32_t length;
    bool found;
} validated_table_t;

static uint16_t read_be16(const unsigned char * p) {
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

static uint32_t read_be32(const unsigned char * p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
           (uint32_t)p[2] << 8 | p[3];
}

static void ttf_read_at(FILE * f, size_t offset, void * data, size_t length) {
    ttf_stream_seek(f, (long)offset);
    ttf_stream_read(f, data, length);
}

/* The renderer uses shipped font assets. Pinning content avoids sending
 * malformed composite-glyph graphs into stb_truetype, whose public API does
 * not expose a recursion/cycle limit. Noto Sans Regular is tried first,
 * then Noto Sans Medium for headings, then Montserrat-Medium. A changed or
 * damaged file falls back to the next pin and finally the builtin face. */
static bool matches_font_pin(FILE * f, size_t size, size_t expected_size, uint32_t expected_crc) {
    if (size != expected_size) return false;
    uint32_t crc = 0xFFFFFFFFu;
    unsigned char chunk[1024];
    for (size_t offset = 0; offset < size;) {
        size_t count = size - offset;
        if (count > sizeof(chunk)) count = sizeof(chunk);
        ttf_read_at(f, offset, chunk, count);
        for (size_t i = 0; i < count; i++) {
            crc ^= chunk[i];
            for (unsigned bit = 0; bit < 8; bit++)
                crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1u));
        }
        offset += count;
    }
    return (crc ^ 0xFFFFFFFFu) == expected_crc;
}

static bool is_shipped_font(FILE * f, size_t size) {
    return matches_font_pin(f, size, NOTO_SANS_REGULAR_BYTES, NOTO_SANS_REGULAR_CRC32) ||
           matches_font_pin(f, size, NOTO_SANS_MEDIUM_BYTES, NOTO_SANS_MEDIUM_CRC32) ||
           matches_font_pin(f, size, MONTSERRAT_MEDIUM_BYTES, MONTSERRAT_MEDIUM_CRC32);
}

static bool is_shipped_noto_regular(FILE * f, size_t size) {
    return matches_font_pin(f, size, NOTO_SANS_REGULAR_BYTES, NOTO_SANS_REGULAR_CRC32);
}

static bool validate_cmap(FILE * f, const validated_table_t * cmap) {
    unsigned char header[4];
    if (cmap->length < sizeof(header)) return false;
    ttf_read_at(f, cmap->offset, header, sizeof(header));
    if (read_be16(header) != 0) return false;
    uint16_t records = read_be16(header + 2);
    if (records == 0 || records > 256 || 4u + (uint32_t)records * 8u > cmap->length) return false;
    bool has_supported = false;
    for (uint16_t i = 0; i < records; i++) {
        unsigned char entry[8], sub[16];
        size_t entry_at = (size_t)cmap->offset + 4u + (size_t)i * 8u;
        ttf_read_at(f, entry_at, entry, sizeof(entry));
        uint32_t sub_offset = read_be32(entry + 4);
        if (sub_offset > cmap->length || cmap->length - sub_offset < 2) return false;
        size_t at = (size_t)cmap->offset + sub_offset;
        ttf_read_at(f, at, sub, 2);
        uint16_t format = read_be16(sub);
        uint32_t sub_length = 0;
        if (format == 2) return false; /* This face does not need legacy cmap format 2. */
        if (format == 0 || format == 4 || format == 6) {
            if (cmap->length - sub_offset < 4) return false;
            ttf_read_at(f, at + 2, sub + 2, 2);
            sub_length = read_be16(sub + 2);
            if ((format == 0 && sub_length < 262) ||
                (format == 4 && sub_length < 16) ||
                (format == 6 && sub_length < 10)) return false;
            if (format == 4) {
                ttf_read_at(f, at + 6, sub + 6, 2);
                uint16_t seg_count = read_be16(sub + 6) / 2;
                if (seg_count == 0 || seg_count > 2048 ||
                    16u + (uint32_t)seg_count * 8u > sub_length) return false;
            }
            if (format == 6) {
                ttf_read_at(f, at + 8, sub + 8, 2);
                uint16_t entries = read_be16(sub + 8);
                if (10u + (uint32_t)entries * 2u > sub_length) return false;
            }
            if (format == 0 || format == 4 || format == 6) has_supported = true;
        } else if (format == 12 || format == 13) {
            if (cmap->length - sub_offset < 16) return false;
            ttf_read_at(f, at + 4, sub + 4, 4);
            sub_length = read_be32(sub + 4);
            if (sub_length < 16 || sub_length > cmap->length - sub_offset) return false;
            ttf_read_at(f, at + 12, sub + 12, 4);
            uint32_t groups = read_be32(sub + 12);
            if (groups > (sub_length - 16) / 12) return false;
            has_supported = true;
        }
        if (sub_length > cmap->length - sub_offset) return false;
    }
    return has_supported;
}

/* Structural checks keep the pinned SFNT tables within the shipped file. */
static bool validate_font(FILE * f, size_t size) {
    unsigned char header[12];
    if (size < sizeof(header)) return false;
    ttf_read_at(f, 0, header, sizeof(header));
    if (!(header[0] == 0 && header[1] == 1 && header[2] == 0 && header[3] == 0) &&
        memcmp(header, "true", 4) != 0) return false;
    uint16_t count = read_be16(header + 4);
    if (count == 0 || count > 128 || 12u + (uint32_t)count * 16u > size) return false;
    validated_table_t head = {0}, hhea = {0}, hmtx = {0}, maxp = {0},
                      loca = {0}, glyf = {0}, cmap = {0};
    for (uint16_t i = 0; i < count; i++) {
        unsigned char record[16];
        ttf_read_at(f, 12u + (size_t)i * 16u, record, sizeof(record));
        uint32_t offset = read_be32(record + 8), length = read_be32(record + 12);
        if ((uint64_t)offset + length > size) return false;
        validated_table_t * table = NULL;
        if (!memcmp(record, "head", 4)) table = &head;
        else if (!memcmp(record, "hhea", 4)) table = &hhea;
        else if (!memcmp(record, "hmtx", 4)) table = &hmtx;
        else if (!memcmp(record, "maxp", 4)) table = &maxp;
        else if (!memcmp(record, "loca", 4)) table = &loca;
        else if (!memcmp(record, "glyf", 4)) table = &glyf;
        else if (!memcmp(record, "cmap", 4)) table = &cmap;
        if (table) { table->offset = offset; table->length = length; table->found = true; }
    }
    if (!head.found || !hhea.found || !hmtx.found || !maxp.found ||
        !loca.found || !glyf.found || !cmap.found || head.length < 54 ||
        hhea.length < 36 || hmtx.length < 4 || maxp.length < 6 ||
        glyf.length == 0) return false;
    unsigned char values[4];
    ttf_read_at(f, (size_t)maxp.offset + 4, values, 2);
    uint16_t glyph_count = read_be16(values);
    ttf_read_at(f, (size_t)hhea.offset + 34, values, 2);
    uint16_t metric_count = read_be16(values);
    ttf_read_at(f, (size_t)head.offset + 50, values, 2);
    int16_t loca_format = (int16_t)read_be16(values);
    if (glyph_count == 0 || metric_count == 0 || metric_count > glyph_count ||
        (loca_format != 0 && loca_format != 1)) return false;
    uint64_t required_hmtx = (uint64_t)metric_count * 4u +
                             (uint64_t)(glyph_count - metric_count) * 2u;
    uint64_t required_loca = (uint64_t)(glyph_count + 1u) * (loca_format ? 4u : 2u);
    if (required_hmtx > hmtx.length || required_loca > loca.length || !validate_cmap(f, &cmap))
        return false;
    uint32_t previous = 0;
    for (uint32_t i = 0; i <= glyph_count; i++) {
        unsigned char raw[4];
        size_t bytes = loca_format ? 4u : 2u;
        ttf_read_at(f, (size_t)loca.offset + (size_t)i * bytes, raw, bytes);
        uint32_t current = loca_format ? read_be32(raw) : (uint32_t)read_be16(raw) * 2u;
        if (current < previous || current > glyf.length) return false;
        previous = current;
    }
    return true;
}

static FILE * open_font_file(const char * path, size_t * size_out) {
    FILE * f = fopen(path, "rb");
    if (!f) return NULL;
    struct stat st;
    if (fstat(fileno(f), &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_size < 12 || (uintmax_t)st.st_size > MAX_FONT_BYTES) {
        fclose(f);
        return NULL;
    }
    *size_out = (size_t)st.st_size;
    return f;
}

static bool init_face(FILE * f, size_t size, stbtt_fontinfo * info,
                      int * ascent, int * descent, int * line_gap) {
    if (!is_shipped_font(f, size) || !validate_font(f, size) ||
        !stbtt_InitFont(info, f, 0)) return false;
    stbtt_GetFontVMetrics(info, ascent, descent, line_gap);
    if (*ascent <= 0 || *descent >= 0 ||
        (int64_t)*ascent - *descent > 10000) return false;
    /* The optional tables need separate complex offset validation. Skipping
     * kerning avoids parsing those tables on this compact fixed menu face. */
    info->kern = 0;
    info->gpos = 0;
    info->svg = 0;
    return true;
}

static void sibling_medium_path(const char * regular, char * out, size_t out_size) {
    const char * slash = strrchr(regular, '/');
    const char * name = slash ? slash + 1 : regular;
    size_t dir_len = (size_t)(name - regular);
    if (strcmp(name, "NotoSans-Regular.ttf") == 0 && dir_len + 21 < out_size) {
        memcpy(out, regular, dir_len);
        memcpy(out + dir_len, "NotoSans-Medium.ttf", 20);
        return;
    }
    snprintf(out, out_size, "%s", DEFAULT_MEDIUM_FONT_PATH);
}

static int try_load_face(const char * path, FILE ** slot, stbtt_fontinfo * info,
                         int * ascent, int * descent, int * line_gap) {
    size_t size = 0;
    FILE * f = open_font_file(path, &size);
    if (!f) return 0;
    ttf_register(f, size);
    if (!init_face(f, size, info, ascent, descent, line_gap)) {
        ttf_unregister(f);
        fclose(f);
        return 0;
    }
    *slot = f;
    return is_shipped_noto_regular(f, size) ? 2 : 1;
}

static void load_font(const char * path) {
    font_ready = 0;
    font_medium_ready = 0;
    const char * primary = path && path[0] ? path : DEFAULT_FONT_PATH;
    int kind = try_load_face(primary, &font_file, &font_info,
                             &font_ascent, &font_descent, &font_line_gap);
    if (!kind && strcmp(primary, DEFAULT_FONT_PATH) != 0)
        kind = try_load_face(DEFAULT_FONT_PATH, &font_file, &font_info,
                             &font_ascent, &font_descent, &font_line_gap);
    if (!kind)
        kind = try_load_face(FALLBACK_FONT_PATH, &font_file, &font_info,
                             &font_ascent, &font_descent, &font_line_gap);
    if (!kind) return;
    font_ready = 1;
    if (kind != 2) return;
    char medium_path[512];
    sibling_medium_path(primary, medium_path, sizeof(medium_path));
    if (try_load_face(medium_path, &font_medium_file, &font_medium_info,
                      &font_medium_ascent, &font_medium_descent,
                      &font_medium_line_gap))
        font_medium_ready = 1;
    else if (try_load_face(DEFAULT_MEDIUM_FONT_PATH, &font_medium_file, &font_medium_info,
                          &font_medium_ascent, &font_medium_descent,
                          &font_medium_line_gap))
        font_medium_ready = 1;
}

bool selector_ui_init(int width, int height, const char * font_path, const char * settings_path) {
    (void)settings_path;
    if (width <= 0 || height <= 0 || (size_t)width > SIZE_MAX / (size_t)height ||
        (size_t)width * (size_t)height > SIZE_MAX / sizeof(uint16_t)) return false;
    selector_ui_shutdown();
    sel_canvas = calloc((size_t)width * (size_t)height, sizeof(*sel_canvas));
    if (!sel_canvas) return false;
    sel_canvas_width = width;
    sel_canvas_height = height;
    sel_canvas_stride = (size_t)width;
    sel_clip_top = 0;
    sel_clip_bottom = height;
    load_font(font_path);
    return true;
}

void selector_ui_shutdown(void) {
    if (font_file) fclose(font_file);
    if (font_medium_file) fclose(font_medium_file);
    font_file = NULL;
    font_medium_file = NULL;
    ttf_clear_slots();
    font_ready = 0;
    font_medium_ready = 0;
    free(sel_canvas);
    sel_canvas = NULL;
    sel_canvas_width = sel_canvas_height = 0;
    sel_canvas_stride = 0;
    sel_clip_top = sel_clip_bottom = 0;
}

const uint16_t * selector_ui_pixels(size_t * stride_pixels) {
    if (stride_pixels) *stride_pixels = sel_canvas_stride;
    return sel_canvas;
}
int selector_ui_width(void) { return sel_canvas_width; }
int selector_ui_height(void) { return sel_canvas_height; }

void selector_ui_draw_selector(int selected, bool rockbox_available,
                               unsigned countdown_seconds,
                               bool countdown_cancelled, bool adb_enabled) {
    (void)adb_enabled;
    selector_paint_selector(selected, rockbox_available, countdown_seconds,
                            countdown_cancelled);
}

void selector_ui_draw_starting(bool rockbox) {
    selector_paint_starting(rockbox);
}

int selector_ui_tools_max_scroll(void) {
    if (!sel_canvas) return 0;
    int total = 7 * tool_row_h() + 6 * tool_gap();
    int viewport = sel_canvas_height - tool_list_top();
    return total > viewport ? total - viewport : 0;
}

static int ensure_visible(int count, int selected, int scroll_pixels,
                          int list_top, int row_height, int gap, int viewport_bottom) {
    int viewport = viewport_bottom - list_top;
    int content = count > 0 ? count * (row_height + gap) - gap : 0;
    int max_scroll = content > viewport ? content - viewport : 0;
    if (scroll_pixels < 0) scroll_pixels = 0;
    if (scroll_pixels > max_scroll) scroll_pixels = max_scroll;
    if (selected >= 0 && selected < count && viewport > 0) {
        int row_top = selected * (row_height + gap);
        int row_bottom = row_top + row_height;
        if (row_top < scroll_pixels) scroll_pixels = row_top;
        else if (row_bottom > scroll_pixels + viewport)
            scroll_pixels = row_bottom - viewport;
        if (scroll_pixels < 0) scroll_pixels = 0;
        if (scroll_pixels > max_scroll) scroll_pixels = max_scroll;
    }
    return scroll_pixels;
}

int selector_ui_ensure_visible_tools(int selected, int scroll_pixels) {
    if (!sel_canvas) return 0;
    return ensure_visible(7, selected, scroll_pixels, tool_list_top(),
                          tool_row_h(), tool_gap(), sel_canvas_height);
}

void selector_ui_draw_tools(int selected, int scroll_pixels, bool adb_enabled) {
    if (!sel_canvas) return;
    int max_scroll = selector_ui_tools_max_scroll();
    if (scroll_pixels < 0) scroll_pixels = 0;
    if (scroll_pixels > max_scroll) scroll_pixels = max_scroll;
    selector_paint_tools(selected, scroll_pixels, adb_enabled);
}

static int hit_row(int x, int y, int list_top, int row_height, int gap,
                   int count, int scroll, int viewport_bottom) {
    int m = margin();
    if (x < m || x >= sel_canvas_width - m || y < list_top || y >= viewport_bottom) return SELECTOR_UI_HIT_NONE;
    int local_y = y - list_top + scroll;
    int row = local_y / (row_height + gap);
    int within = local_y % (row_height + gap);
    if (row < 0 || row >= count || within >= row_height) return SELECTOR_UI_HIT_NONE;
    return row;
}

int selector_ui_hit_test_selector(int x, int y) {
    return selector_ui_hit_test_selector_available(x, y, true);
}

int selector_ui_hit_test_selector_available(int x, int y, bool rockbox_available) {
    if (!sel_canvas) return SELECTOR_UI_HIT_NONE;
    int count = rockbox_available ? 3 : 2;
    return hit_row(x, y, selector_list_top(sel_canvas_width), selector_card_h(sel_canvas_width),
                   selector_gap(sel_canvas_width), count, 0, sel_canvas_height);
}

static int hit_test_back(int x, int y) {
    int m = margin();
    int header_y = selector_header_title_y(sel_canvas_width);
    int back_size = selector_back_icon(sel_canvas_width);
    if (x >= m && x <= m + back_size + 8 && y >= header_y - 8 && y <= header_y + back_size + 8)
        return SELECTOR_UI_HIT_BACK;
    return SELECTOR_UI_HIT_NONE;
}

int selector_ui_hit_test_tools(int x, int y, int scroll_pixels) {
    if (!sel_canvas) return SELECTOR_UI_HIT_NONE;
    if (hit_test_back(x, y) == SELECTOR_UI_HIT_BACK) return SELECTOR_UI_HIT_BACK;
    int max_scroll = selector_ui_tools_max_scroll();
    if (scroll_pixels < 0) scroll_pixels = 0;
    if (scroll_pixels > max_scroll) scroll_pixels = max_scroll;
    return hit_row(x, y, tool_list_top(), tool_row_h(), tool_gap(), 7, scroll_pixels,
                   sel_canvas_height);
}

int selector_ui_list_max_scroll(size_t count) {
    if (!sel_canvas) return 0;
    if (count > SELECTOR_UI_MAX_LIST_ITEMS) count = SELECTOR_UI_MAX_LIST_ITEMS;
    int total = (int)count * (tool_row_h() + tool_gap());
    if (count > 0) total -= tool_gap();
    int viewport = sel_list_viewport_bottom() - tool_list_top();
    return total > viewport ? total - viewport : 0;
}

int selector_ui_ensure_visible_list(size_t count, int selected, int scroll_pixels) {
    if (!sel_canvas) return 0;
    if (count > SELECTOR_UI_MAX_LIST_ITEMS) count = SELECTOR_UI_MAX_LIST_ITEMS;
    return ensure_visible((int)count, selected, scroll_pixels, tool_list_top(),
                          tool_row_h(), tool_gap(), sel_list_viewport_bottom());
}

void selector_ui_draw_list(const char * title, const char * const * items,
                           size_t count, int selected, int scroll_pixels,
                           const char * notice) {
    if (!sel_canvas) return;
    int max_scroll = selector_ui_list_max_scroll(count);
    if (scroll_pixels < 0) scroll_pixels = 0;
    if (scroll_pixels > max_scroll) scroll_pixels = max_scroll;
    selector_paint_list(title, items, count, selected, scroll_pixels, notice);
}

int selector_ui_hit_test_list(int x, int y, size_t count, int scroll_pixels) {
    if (!sel_canvas) return SELECTOR_UI_HIT_NONE;
    if (count > SELECTOR_UI_MAX_LIST_ITEMS) count = SELECTOR_UI_MAX_LIST_ITEMS;
    if (hit_test_back(x, y) == SELECTOR_UI_HIT_BACK) return SELECTOR_UI_HIT_BACK;
    int max_scroll = selector_ui_list_max_scroll(count);
    if (scroll_pixels < 0) scroll_pixels = 0;
    if (scroll_pixels > max_scroll) scroll_pixels = max_scroll;
    return hit_row(x, y, tool_list_top(), tool_row_h(), tool_gap(), (int)count,
                   scroll_pixels, sel_list_viewport_bottom());
}

void selector_ui_draw_notice(const char * title, const char * message) {
    selector_paint_notice(title, message);
}

void selector_ui_draw_progress(const char * title, const char * message, int percent) {
    selector_paint_progress(title, message, percent);
}
