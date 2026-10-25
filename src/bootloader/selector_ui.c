#define _POSIX_C_SOURCE 200809L

#include "selector_ui.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "board_config.h"
#include "selector_logos.h"

/* Reuse LVGL's vendored tiny_ttf rasterizer without initializing LVGL. The
 * font is opened once and stb's streaming callbacks seek/read that FILE for
 * each glyph; the target asset is local and capped below 6 MiB. */
static size_t ttf_stream_limit;
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

#define DEFAULT_FONT_PATH "/usr/resource/fonts/Montserrat-Medium.ttf"
#define DEFAULT_SETTINGS_PATH "/usr/data/.compas/settings.txt"
#define MAX_FONT_BYTES (6u * 1024u * 1024u)
#define MONTSERRAT_MEDIUM_BYTES 243180u
#define MONTSERRAT_MEDIUM_CRC32 0xE84615C2u
#define BG_RGB 0x100810u
#define CARD_RGB 0x201018u
#define PRIMARY_RGB 0xF1F3F5u
#define SECONDARY_RGB 0xA8B0BCu
#define BORDER_RGB 0x343B46u
#define DEFAULT_ACCENT_RGB 0xB0142Bu /* DEFAULT_ACCENT_COLOR */

typedef struct { int r, g, b; } rgb_t;

static uint16_t * canvas;
static int canvas_width;
static int canvas_height;
static size_t canvas_stride;
static int clip_top;
static int clip_bottom;
static uint32_t accent_rgb = DEFAULT_ACCENT_RGB;
static uint32_t selected_rgb;
static FILE * font_file;
static stbtt_fontinfo font_info;
static int font_ready;
static int font_ascent;
static int font_descent;
static int font_line_gap;

static void ttf_stream_seek(FILE * stream, long offset) {
    /* stb's parser trusts offsets inside the TTF. Clamp malformed offsets to
     * EOF so its stream callbacks can never read outside the validated file. */
    if (offset < 0 || (uintmax_t)offset > ttf_stream_limit)
        offset = (long)ttf_stream_limit;
    clearerr(stream);
    (void)fseek(stream, offset, SEEK_SET);
}

static void ttf_stream_read(FILE * stream, void * destination, size_t count) {
    memset(destination, 0, count);
    long position = ftell(stream);
    if (position < 0 || (uintmax_t)position >= ttf_stream_limit) return;
    size_t remaining = ttf_stream_limit - (size_t)position;
    if (count > remaining) count = remaining;
    (void)fread(destination, 1, count, stream);
}

static const char * const selector_title[] = { "Compás", "Rockbox", "Tools" };
static const char * const selector_detail[] = {
    "Compás player", "Rockbox player", "ADB, scripts and system options"
};
static const char * const tool_titles[] = {
    "ADB", "Run script", "Remount SD", "Repair SD card",
    "Restart", "Shutdown", "Recovery"
};

static rgb_t rgb(uint32_t value) {
    rgb_t c = { (int)((value >> 16) & 255), (int)((value >> 8) & 255), (int)(value & 255) };
    return c;
}

static uint16_t pack565(uint32_t value) {
    rgb_t c = rgb(value);
    return (uint16_t)(((unsigned)c.r >> 3 << 11) | ((unsigned)c.g >> 2 << 5) | ((unsigned)c.b >> 3));
}

static void put_pixel(int x, int y, uint32_t color, unsigned alpha) {
    if (!canvas || x < 0 || y < clip_top || x >= canvas_width || y >= canvas_height ||
        y >= clip_bottom || alpha == 0) return;
    if (alpha >= 255) {
        canvas[(size_t)y * canvas_stride + (size_t)x] = pack565(color);
        return;
    }
    uint16_t old = canvas[(size_t)y * canvas_stride + (size_t)x];
    rgb_t bg = { (int)(((old >> 11) & 31) * 255 / 31),
                 (int)(((old >> 5) & 63) * 255 / 63),
                 (int)((old & 31) * 255 / 31) };
    rgb_t fg = rgb(color);
    unsigned inv = 255 - alpha;
    uint32_t blended = (uint32_t)((fg.r * alpha + bg.r * inv + 127) / 255) << 16 |
                       (uint32_t)((fg.g * alpha + bg.g * inv + 127) / 255) << 8 |
                       (uint32_t)((fg.b * alpha + bg.b * inv + 127) / 255);
    canvas[(size_t)y * canvas_stride + (size_t)x] = pack565(blended);
}

static void blit_a8(int x, int y, const uint8_t * pixels, int width, int height, uint32_t color) {
    if (!pixels || width <= 0 || height <= 0) return;
    for (int row = 0; row < height; row++) {
        const uint8_t * src = pixels + (size_t)row * (size_t)width;
        for (int col = 0; col < width; col++)
            put_pixel(x + col, y + row, color, src[col]);
    }
}

static void blit_rgba(int x, int y, const uint8_t * pixels, int width, int height) {
    if (!pixels || width <= 0 || height <= 0) return;
    for (int row = 0; row < height; row++) {
        const uint8_t * src = pixels + (size_t)row * (size_t)width * 4u;
        for (int col = 0; col < width; col++) {
            const uint8_t * p = src + (size_t)col * 4u;
            uint32_t color = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
            put_pixel(x + col, y + row, color, p[3]);
        }
    }
}

static void fill_rect(int x, int y, int w, int h, uint32_t color) {
    if (w <= 0 || h <= 0) return;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < clip_top ? clip_top : y;
    int x1 = x > canvas_width - w ? canvas_width : x + w;
    int y1 = y > canvas_height - h ? canvas_height : y + h;
    if (y1 > clip_bottom) y1 = clip_bottom;
    for (int py = y0; py < y1; py++)
        for (int px = x0; px < x1; px++)
            canvas[(size_t)py * canvas_stride + (size_t)px] = pack565(color);
}

static int integer_sqrt(int value) {
    int root = 0;
    while ((root + 1) * (root + 1) <= value) root++;
    return root;
}

static void line(int x0, int y0, int x1, int y1, uint32_t color, int thickness) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    int radius = thickness / 2;
    for (;;) {
        fill_rect(x0 - radius, y0 - radius, thickness, thickness, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void rounded_fill(int x, int y, int w, int h, int radius, uint32_t color) {
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
        fill_rect(x + inset, y + row, w - 2 * inset, 1, color);
    }
}

static void rounded_card(int x, int y, int w, int h, bool selected) {
    uint32_t outer = selected ? accent_rgb : BORDER_RGB;
    uint32_t inner = selected ? selected_rgb : CARD_RGB;
    rounded_fill(x, y, w, h, canvas_width >= 400 ? 16 : 13, outer);
    rounded_fill(x + 1, y + 1, w - 2, h - 2, canvas_width >= 400 ? 15 : 12, inner);
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

static int text_width(const char * text, int px) {
    if (!font_ready) {
        int scale = px / 7;
        if (scale < 1) scale = 1;
        int count = 0;
        for (const unsigned char *p = (const unsigned char *)text; *p; ) { (void)decode_utf8(&p); count++; }
        return count * 6 * scale;
    }
    float scale = stbtt_ScaleForPixelHeight(&font_info, (float)px);
    float advance = 0.0f;
    int previous = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; ) {
        uint32_t cp = decode_utf8(&p);
        int glyph = stbtt_FindGlyphIndex(&font_info, (int)cp);
        int adv, lsb;
        if (previous) advance += stbtt_GetGlyphKernAdvance(&font_info, previous, glyph) * scale;
        stbtt_GetGlyphHMetrics(&font_info, glyph, &adv, &lsb);
        advance += adv * scale;
        previous = glyph;
    }
    return (int)(advance + 0.5f);
}

static void draw_text(int x, int top, const char * text, int px, uint32_t color) {
    if (!font_ready) {
        int scale = px / 7;
        if (scale < 1) scale = 1;
        int cursor = x;
        for (const unsigned char *p = (const unsigned char *)text; *p; ) {
            uint32_t cp = decode_utf8(&p);
            for (int row = 0; row < 7; row++) {
                uint8_t bits = fallback_glyph(cp, row);
                for (int col = 0; col < 5; col++)
                    if (bits & (1u << (4 - col))) fill_rect(cursor + col * scale, top + row * scale, scale, scale, color);
            }
            cursor += 6 * scale;
        }
        return;
    }

    float scale = stbtt_ScaleForPixelHeight(&font_info, (float)px);
    int baseline = top + (int)(font_ascent * scale + 0.5f);
    int cursor = x;
    int previous = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; ) {
        uint32_t cp = decode_utf8(&p);
        int glyph = stbtt_FindGlyphIndex(&font_info, (int)cp);
        int advance, lsb, x0, y0, x1, y1;
        if (previous) cursor += (int)(stbtt_GetGlyphKernAdvance(&font_info, previous, glyph) * scale + 0.5f);
        stbtt_GetGlyphHMetrics(&font_info, glyph, &advance, &lsb);
        stbtt_GetCodepointBitmapBox(&font_info, (int)cp, scale, scale, &x0, &y0, &x1, &y1);
        int w = x1 - x0, h = y1 - y0;
        if (w > 0 && h > 0) {
            unsigned char * bitmap = stbtt_GetCodepointBitmap(&font_info, scale, scale, (int)cp, NULL, NULL, NULL, NULL);
            if (bitmap) {
                for (int gy = 0; gy < h; gy++)
                    for (int gx = 0; gx < w; gx++)
                        put_pixel(cursor + x0 + gx, baseline + y0 + gy, color, bitmap[(size_t)gy * (size_t)w + (size_t)gx]);
                stbtt_FreeBitmap(bitmap, NULL);
            }
        }
        cursor += (int)(advance * scale + 0.5f);
        previous = glyph;
    }
}

static void draw_text_centered(int center_x, int top, const char * text, int px, uint32_t color) {
    draw_text(center_x - text_width(text, px) / 2, top, text, px, color);
}

static int scaled_width(int ref) {
    return (ref * canvas_width + 240) / 480;
}

static int margin(void) { return canvas_width >= 400 ? 24 : 16; }
static int selector_list_top(void) { return canvas_width >= 400 ? 142 : 120; }
static int selector_card_h(void) { return canvas_width >= 400 ? 112 : 90; }
static int selector_gap(void) { return canvas_width >= 400 ? 14 : 10; }
static int tool_list_top(void) { return canvas_width >= 400 ? 128 : 104; }
static int tool_row_h(void) { return canvas_width >= 400 ? 76 : 64; }
static int tool_gap(void) { return canvas_width >= 400 ? 10 : 8; }
static int footer_top(void) { return canvas_height - scaled_width(64); }

static void draw_music_note(int x, int y, int size, uint32_t color) {
    int sw = size >= 36 ? 3 : 2;
    line(x + size * 34 / 100, y + size * 72 / 100, x + size * 34 / 100, y + size * 25 / 100, color, sw);
    line(x + size * 34 / 100, y + size * 25 / 100, x + size * 78 / 100, y + size * 16 / 100, color, sw);
    line(x + size * 78 / 100, y + size * 16 / 100, x + size * 78 / 100, y + size * 61 / 100, color, sw);
    rounded_fill(x + size * 10 / 100, y + size * 64 / 100, size * 25 / 100, size * 17 / 100, size / 12, color);
    rounded_fill(x + size * 54 / 100, y + size * 53 / 100, size * 25 / 100, size * 17 / 100, size / 12, color);
}

static void draw_mountain(int x, int y, int size, uint32_t color) {
    int sw = size >= 36 ? 3 : 2;
    line(x + 3, y + size - 5, x + size * 37 / 100, y + size * 35 / 100, color, sw);
    line(x + size * 37 / 100, y + size * 35 / 100, x + size * 52 / 100, y + size * 55 / 100, color, sw);
    line(x + size * 52 / 100, y + size * 55 / 100, x + size * 70 / 100, y + size * 28 / 100, color, sw);
    line(x + size * 70 / 100, y + size * 28 / 100, x + size - 3, y + size - 5, color, sw);
    line(x + 3, y + size - 5, x + size - 3, y + size - 5, color, sw);
    line(x + size * 22 / 100, y + size * 68 / 100, x + size * 34 / 100, y + size * 55 / 100, color, sw);
    line(x + size * 34 / 100, y + size * 55 / 100, x + size * 45 / 100, y + size * 68 / 100, color, sw);
}

static void draw_tools_icon(int x, int y, int size, uint32_t color) {
    int sw = size >= 36 ? 3 : 2;
    line(x + 8, y + 8, x + size - 8, y + size - 8, color, sw);
    line(x + size - 8, y + 8, x + 8, y + size - 8, color, sw);
    line(x + 4, y + 11, x + 12, y + 4, color, sw);
    line(x + size - 12, y + size - 4, x + size - 4, y + size - 12, color, sw);
}

static uint32_t mix_accent(uint32_t base, unsigned amount) {
    rgb_t a = rgb(accent_rgb), b = rgb(base);
    unsigned inv = 100 - amount;
    return (uint32_t)((a.r * amount + b.r * inv) / 100) << 16 |
           (uint32_t)((a.g * amount + b.g * inv) / 100) << 8 |
           (uint32_t)((a.b * amount + b.b * inv) / 100);
}

static bool parse_hex6(const char * p, uint32_t * out) {
    char digits[7];
    size_t n = 0;
    while (isspace((unsigned char)*p)) p++;
    if (*p == '#') p++;
    while (isxdigit((unsigned char)*p) && n < 6) digits[n++] = *p++;
    while (isspace((unsigned char)*p)) p++;
    if (n != 6 || *p != '\0') return false;
    digits[6] = '\0';
    char * end = NULL;
    unsigned long value = strtoul(digits, &end, 16);
    if (!end || *end || value > 0xFFFFFFul) return false;
    *out = (uint32_t)value;
    return true;
}

static void load_accent(const char * path) {
    accent_rgb = DEFAULT_ACCENT_RGB;
    FILE *f = fopen(path ? path : DEFAULT_SETTINGS_PATH, "r");
    if (!f) return;
    char linebuf[256];
    while (fgets(linebuf, sizeof(linebuf), f)) {
        char * eq = strchr(linebuf, '=');
        if (!eq) continue;
        *eq++ = '\0';
        char * key = linebuf;
        while (isspace((unsigned char)*key)) key++;
        char * end = key + strlen(key);
        while (end > key && isspace((unsigned char)end[-1])) *--end = '\0';
        if (strcmp(key, "accent_color") == 0) {
            uint32_t parsed;
            if (parse_hex6(eq, &parsed)) accent_rgb = parsed;
            break;
        }
    }
    fclose(f);
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

/* The renderer uses one shipped font asset. Pinning its content avoids
 * sending malformed composite-glyph graphs into stb_truetype, whose public
 * API does not expose a recursion/cycle limit. Any changed or damaged font
 * simply falls back to the small builtin face. */
static bool is_shipped_montserrat(FILE * f, size_t size) {
    if (size != MONTSERRAT_MEDIUM_BYTES) return false;
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
    return (crc ^ 0xFFFFFFFFu) == MONTSERRAT_MEDIUM_CRC32;
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

static void load_font(const char * path) {
    font_ready = 0;
    font_file = fopen(path ? path : DEFAULT_FONT_PATH, "rb");
    if (!font_file) return;
    struct stat st;
    if (fstat(fileno(font_file), &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_size < 12 || (uintmax_t)st.st_size > MAX_FONT_BYTES) {
        fclose(font_file);
        font_file = NULL;
        ttf_stream_limit = 0;
        return;
    }
    ttf_stream_limit = (size_t)st.st_size;
    if (!is_shipped_montserrat(font_file, ttf_stream_limit) ||
        !validate_font(font_file, ttf_stream_limit) || !stbtt_InitFont(&font_info, font_file, 0)) {
        fclose(font_file);
        font_file = NULL;
        ttf_stream_limit = 0;
        return;
    }
    stbtt_GetFontVMetrics(&font_info, &font_ascent, &font_descent, &font_line_gap);
    if (font_ascent <= 0 || font_descent >= 0 ||
        (int64_t)font_ascent - font_descent > 10000) {
        fclose(font_file);
        font_file = NULL;
        ttf_stream_limit = 0;
        return;
    }
    /* The optional tables need separate complex offset validation. Skipping
     * kerning avoids parsing those tables on this compact fixed menu face. */
    font_info.kern = 0;
    font_info.gpos = 0;
    font_info.svg = 0;
    font_ready = 1;
}

bool selector_ui_init(int width, int height, const char * font_path, const char * settings_path) {
    if (width <= 0 || height <= 0 || (size_t)width > SIZE_MAX / (size_t)height ||
        (size_t)width * (size_t)height > SIZE_MAX / sizeof(uint16_t)) return false;
    selector_ui_shutdown();
    canvas = calloc((size_t)width * (size_t)height, sizeof(*canvas));
    if (!canvas) return false;
    canvas_width = width;
    canvas_height = height;
    canvas_stride = (size_t)width;
    clip_top = 0;
    clip_bottom = height;
    load_accent(settings_path);
    selected_rgb = mix_accent(BG_RGB, 22);
    load_font(font_path);
    return true;
}

void selector_ui_shutdown(void) {
    if (font_file) fclose(font_file);
    font_file = NULL;
    font_ready = 0;
    ttf_stream_limit = 0;
    free(canvas);
    canvas = NULL;
    canvas_width = canvas_height = 0;
    canvas_stride = 0;
    clip_top = clip_bottom = 0;
}

const uint16_t * selector_ui_pixels(size_t * stride_pixels) {
    if (stride_pixels) *stride_pixels = canvas_stride;
    return canvas;
}
int selector_ui_width(void) { return canvas_width; }
int selector_ui_height(void) { return canvas_height; }

void selector_ui_draw_selector(int selected, bool rockbox_available,
                               unsigned countdown_seconds,
                               bool countdown_cancelled, bool adb_enabled) {
    (void)adb_enabled; /* Selector painter does not own the Tools toggle. */
    if (!canvas) return;
    int row_count = rockbox_available ? 3 : 2;
    if (selected < 0 || selected >= row_count) selected = 0;
    fill_rect(0, 0, canvas_width, canvas_height, BG_RGB);
    int m = margin();
    int compact = canvas_width < 400;
    draw_text(m, compact ? 24 : 38, "Choose a player", compact ? 24 : 28, PRIMARY_RGB);
    draw_text(m, compact ? 64 : 80, "Select what to start", 16, SECONDARY_RGB);

    int list_top = selector_list_top(), card_h = selector_card_h(), gap = selector_gap();
    int x = m, w = canvas_width - 2 * m;
    for (int row = 0; row < row_count; row++) {
        int i = rockbox_available || row == 0 ? row : 2;
        int y = list_top + row * (card_h + gap);
        bool is_selected = row == selected;
        rounded_card(x, y, w, card_h, is_selected);
        if (is_selected) rounded_fill(x + 1, y + 17, 4, card_h - 34, 2, accent_rgb);
        int icon_size = compact ? 32 : 40;
        int icon_x = x + (compact ? 12 : 18);
        int icon_y = y + (card_h - icon_size) / 2;
        uint32_t icon_color = is_selected ? accent_rgb : SECONDARY_RGB;
        int logo_x = x + (compact ? 16 : 22);
        int drew_logo = 0;
        if (i == 0) {
            const selector_logo_a8_t * mark = selector_logo_wordmark();
            if (mark && mark->pixels && mark->width > 0 && mark->height > 0) {
                blit_a8(logo_x, y + (card_h - mark->height) / 2,
                        mark->pixels, mark->width, mark->height, PRIMARY_RGB);
                drew_logo = 1;
            }
        } else if (i == 1) {
            const selector_logo_rgba_t * mark = selector_logo_rockbox();
            if (mark && mark->pixels && mark->width > 0 && mark->height > 0) {
                blit_rgba(logo_x, y + (card_h - mark->height) / 2,
                          mark->pixels, mark->width, mark->height);
                drew_logo = 1;
            }
        }
        if (!drew_logo) {
            if (i == 0) draw_music_note(icon_x, icon_y, icon_size, icon_color);
            else if (i == 1) draw_mountain(icon_x, icon_y, icon_size, icon_color);
            else draw_tools_icon(icon_x, icon_y, icon_size, icon_color);
            int tx = icon_x + icon_size + (compact ? 11 : 16);
            draw_text(tx, y + (card_h - 45) / 2, selector_title[i], 22, PRIMARY_RGB);
            draw_text(tx, y + (card_h - 45) / 2 + 27, selector_detail[i], 16, SECONDARY_RGB);
        }
        if (is_selected) {
            int cx = x + w - 20, cy = y + card_h / 2;
            line(cx - 4, cy - 6, cx + 2, cy, accent_rgb, 2);
            line(cx + 2, cy, cx - 4, cy + 6, accent_rgb, 2);
        }
    }

    if (!countdown_cancelled && selected == 0) {
        char label[48];
        snprintf(label, sizeof(label), "Auto-start in %u sec", countdown_seconds);
        draw_text_centered(canvas_width / 2, canvas_height - 34, label, 14, SECONDARY_RGB);
    }
}

void selector_ui_draw_starting(bool rockbox) {
    if (!canvas) return;
    fill_rect(0, 0, canvas_width, canvas_height, BG_RGB);

    int compact = canvas_width < 400;
    int icon_size = compact ? 48 : 56;
    int font_px = compact ? 20 : 24;
    const char * label = rockbox ? "Starting Rockbox…" : "Starting Compás…";
    int group_height = icon_size + 26 + font_px + 8;
    int top = (canvas_height - group_height) / 2;
    int icon_x = (canvas_width - icon_size) / 2;
    int icon_y = top;

    if (rockbox) draw_mountain(icon_x, icon_y, icon_size, accent_rgb);
    else draw_music_note(icon_x, icon_y, icon_size, accent_rgb);
    draw_text_centered(canvas_width / 2, icon_y + icon_size + 26,
                       label, font_px, PRIMARY_RGB);
}

int selector_ui_tools_max_scroll(void) {
    if (!canvas) return 0;
    int total = 7 * tool_row_h() + 6 * tool_gap();
    int viewport = footer_top() - tool_list_top();
    return total > viewport ? total - viewport : 0;
}

static int ensure_visible(int count, int selected, int scroll_pixels,
                          int list_top, int row_height, int gap) {
    int max_scroll;
    int viewport = footer_top() - list_top;
    int content = count > 0 ? count * (row_height + gap) - gap : 0;
    max_scroll = content > viewport ? content - viewport : 0;
    if (scroll_pixels < 0) scroll_pixels = 0;
    if (scroll_pixels > max_scroll) scroll_pixels = max_scroll;
    /* Back is a focusable header target, not a row. */
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
    if (!canvas) return 0;
    return ensure_visible(7, selected, scroll_pixels, tool_list_top(),
                          tool_row_h(), tool_gap());
}

static void draw_back_header(const char * title, bool selected);

void selector_ui_draw_tools(int selected, int scroll_pixels, bool adb_enabled) {
    if (!canvas) return;
    if (selected != SELECTOR_UI_HIT_BACK && (selected < 0 || selected > 6)) selected = 0;
    int max_scroll = selector_ui_tools_max_scroll();
    if (scroll_pixels < 0) scroll_pixels = 0;
    if (scroll_pixels > max_scroll) scroll_pixels = max_scroll;
    fill_rect(0, 0, canvas_width, canvas_height, BG_RGB);
    int m = margin(), compact = canvas_width < 400;
    draw_back_header("Tools", selected == SELECTOR_UI_HIT_BACK);

    int list_top = tool_list_top(), row_h = tool_row_h(), gap = tool_gap();
    int viewport_bottom = footer_top();
    int x = m, w = canvas_width - 2 * m;
    int previous_clip_top = clip_top, previous_clip_bottom = clip_bottom;
    clip_top = list_top;
    clip_bottom = viewport_bottom;
    /* Clip each card to the actual scrolling viewport; one or more may be
     * partial at the top/bottom on smaller panels. */
    for (int i = 0; i < 7; i++) {
        int content_y = i * (row_h + gap) - scroll_pixels;
        int y = list_top + content_y;
        if (y + row_h <= list_top || y >= viewport_bottom) continue;
        bool is_selected = i == selected;
        rounded_card(x, y, w, row_h, is_selected);
        if (is_selected) rounded_fill(x + 1, y + (row_h > 24 ? 12 : 8), 4,
                                      row_h - (row_h > 24 ? 24 : 16), 2, accent_rgb);
        draw_text(x + (compact ? 14 : 20), y + (row_h - 22) / 2, tool_titles[i], compact ? 20 : 22, PRIMARY_RGB);
        if (i == 0) {
            int track_w = compact ? 40 : 46, track_h = compact ? 22 : 24;
            int track_x = x + w - (compact ? 14 : 20) - track_w;
            int track_y = y + (row_h - track_h) / 2;
            rounded_fill(track_x, track_y, track_w, track_h, track_h / 2,
                         adb_enabled ? accent_rgb : 0x4A454Du);
            int knob = compact ? 14 : 16, pad = 4;
            int knob_x = adb_enabled ? track_x + track_w - knob - pad : track_x + pad;
            rounded_fill(knob_x, track_y + (track_h - knob) / 2, knob, knob, knob / 2,
                         adb_enabled ? 0xFFFFFFu : 0xD7D4D9u);
        } else if (is_selected) {
            int cx = x + w - 20, cy = y + row_h / 2;
            line(cx - 4, cy - 6, cx + 2, cy, accent_rgb, 2);
            line(cx + 2, cy, cx - 4, cy + 6, accent_rgb, 2);
        }
    }
    clip_top = previous_clip_top;
    clip_bottom = previous_clip_bottom;
    if (selector_ui_tools_max_scroll() > 0) {
        fill_rect(m, list_top, w, 1, BORDER_RGB);
        fill_rect(m, viewport_bottom - 1, w, 1, BORDER_RGB);
    }
}

static int hit_row(int x, int y, int list_top, int row_height, int gap,
                   int count, int scroll, int viewport_bottom) {
    int m = margin();
    if (x < m || x >= canvas_width - m || y < list_top || y >= viewport_bottom) return SELECTOR_UI_HIT_NONE;
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
    if (!canvas) return SELECTOR_UI_HIT_NONE;
    int count = rockbox_available ? 3 : 2;
    return hit_row(x, y, selector_list_top(), selector_card_h(), selector_gap(), count, 0,
                   footer_top());
}

int selector_ui_hit_test_tools(int x, int y, int scroll_pixels) {
    if (!canvas) return SELECTOR_UI_HIT_NONE;
    int m = margin(), header_y = canvas_width < 400 ? 24 : 38;
    int back_size = canvas_width < 400 ? 32 : 44;
    if (x >= m && x <= m + back_size + 8 && y >= header_y - 8 && y <= header_y + back_size + 8)
        return SELECTOR_UI_HIT_BACK;
    int max_scroll = selector_ui_tools_max_scroll();
    if (scroll_pixels < 0) scroll_pixels = 0;
    if (scroll_pixels > max_scroll) scroll_pixels = max_scroll;
    return hit_row(x, y, tool_list_top(), tool_row_h(), tool_gap(), 7, scroll_pixels, footer_top());
}

static bool bounded_text(char out[128], const char * input) {
    if (!input) input = "";
    size_t n = 0;
    while (n < 124 && input[n]) { out[n] = input[n]; n++; }
    bool truncated = input[n] != '\0';
    if (truncated) {
        out[n++] = '.'; out[n++] = '.'; out[n++] = '.';
    }
    out[n] = '\0';
    return truncated;
}

static void remove_last_codepoint(char text[128]) {
    size_t n = strlen(text);
    if (!n) return;
    size_t start = n - 1;
    while (start > 0 && (((unsigned char)text[start] & 0xC0u) == 0x80u)) start--;
    text[start] = '\0';
}

static void fit_list_text(char out[128], const char * input, int max_width, int px) {
    bool truncated = bounded_text(out, input);
    if (!truncated && text_width(out, px) <= max_width) return;
    truncated = true;
    size_t length = strlen(out);
    if (length >= 3 && memcmp(out + length - 3, "...", 3) == 0)
        out[length - 3] = '\0';
    while (*out && text_width(out, px) + text_width("...", px) > max_width)
        remove_last_codepoint(out);
    if (max_width >= text_width("...", px)) {
        length = strlen(out);
        if (length <= 124) memcpy(out + length, "...", 4);
    }
}

static void draw_back_header(const char * title, bool selected) {
    int m = margin(), compact = canvas_width < 400;
    int header_y = compact ? 24 : 38;
    int back_size = compact ? 32 : 44;
    char safe_title[128];
    bounded_text(safe_title, title ? title : "Tools");
    int title_px = compact ? 24 : 28;
    int card_h = compact ? 48 : 56;
    int card_w = back_size + 8 + text_width(safe_title, title_px) + 20;
    if (selected) rounded_card(m, header_y - (card_h - back_size) / 2,
                               card_w, card_h, true);
    uint32_t color = selected ? accent_rgb : SECONDARY_RGB;
    line(m + back_size - 10, header_y - 2, m + back_size - 20,
         header_y + back_size / 2 - 2, color, 3);
    line(m + back_size - 20, header_y + back_size / 2 - 2,
         m + back_size - 10, header_y + back_size - 2, color, 3);
    draw_text(m + back_size + 8, header_y, safe_title, title_px, selected ? accent_rgb : PRIMARY_RGB);
}

int selector_ui_list_max_scroll(size_t count) {
    if (!canvas) return 0;
    if (count > SELECTOR_UI_MAX_LIST_ITEMS) count = SELECTOR_UI_MAX_LIST_ITEMS;
    int total = (int)count * (tool_row_h() + tool_gap());
    if (count > 0) total -= tool_gap();
    int viewport = footer_top() - tool_list_top();
    return total > viewport ? total - viewport : 0;
}

int selector_ui_ensure_visible_list(size_t count, int selected, int scroll_pixels) {
    if (!canvas) return 0;
    if (count > SELECTOR_UI_MAX_LIST_ITEMS) count = SELECTOR_UI_MAX_LIST_ITEMS;
    return ensure_visible((int)count, selected, scroll_pixels, tool_list_top(),
                          tool_row_h(), tool_gap());
}

void selector_ui_draw_list(const char * title, const char * const * items,
                           size_t count, int selected, int scroll_pixels,
                           const char * notice) {
    if (!canvas) return;
    if (count > SELECTOR_UI_MAX_LIST_ITEMS) count = SELECTOR_UI_MAX_LIST_ITEMS;
    if (selected != SELECTOR_UI_HIT_BACK) {
        if (count == 0) selected = -1;
        else if (selected < 0 || (size_t)selected >= count) selected = 0;
    }
    int max_scroll = selector_ui_list_max_scroll(count);
    if (scroll_pixels < 0) scroll_pixels = 0;
    if (scroll_pixels > max_scroll) scroll_pixels = max_scroll;
    fill_rect(0, 0, canvas_width, canvas_height, BG_RGB);
    draw_back_header(title ? title : "Select a file", selected == SELECTOR_UI_HIT_BACK);
    int list_top = tool_list_top(), row_h = tool_row_h(), gap = tool_gap();
    int bottom = footer_top(), m = margin(), w = canvas_width - 2 * m;
    int previous_clip_top = clip_top, previous_clip_bottom = clip_bottom;
    clip_top = list_top;
    clip_bottom = bottom;
    for (size_t i = 0; i < count; i++) {
        int y = list_top + (int)i * (row_h + gap) - scroll_pixels;
        if (y + row_h <= list_top || y >= bottom) continue;
        bool is_selected = (int)i == selected;
        rounded_card(m, y, w, row_h, is_selected);
        if (is_selected)
            rounded_fill(m + 1, y + (row_h > 24 ? 12 : 8), 4,
                         row_h - (row_h > 24 ? 24 : 16), 2, accent_rgb);
        char safe_item[128];
        int item_px = canvas_width < 400 ? 20 : 22;
        int item_x = m + (canvas_width < 400 ? 14 : 20);
        int max_text_width = w - (item_x - m) - 34;
        fit_list_text(safe_item, items ? items[i] : "", max_text_width, item_px);
        draw_text(item_x, y + (row_h - 22) / 2, safe_item, item_px, PRIMARY_RGB);
        if (is_selected) {
            int cx = m + w - 20, cy = y + row_h / 2;
            line(cx - 4, cy - 6, cx + 2, cy, accent_rgb, 2);
            line(cx + 2, cy, cx - 4, cy + 6, accent_rgb, 2);
        }
    }
    clip_top = previous_clip_top;
    clip_bottom = previous_clip_bottom;
    if (max_scroll > 0) {
        fill_rect(m, list_top, w, 1, BORDER_RGB);
        fill_rect(m, bottom - 1, w, 1, BORDER_RGB);
    }
    if (notice && *notice) {
        char safe_notice[128];
        bounded_text(safe_notice, notice);
        draw_text_centered(canvas_width / 2, canvas_height - 30, safe_notice, 14, SECONDARY_RGB);
    }
}

int selector_ui_hit_test_list(int x, int y, size_t count, int scroll_pixels) {
    if (!canvas) return SELECTOR_UI_HIT_NONE;
    if (count > SELECTOR_UI_MAX_LIST_ITEMS) count = SELECTOR_UI_MAX_LIST_ITEMS;
    int m = margin(), header_y = canvas_width < 400 ? 24 : 38;
    int back_size = canvas_width < 400 ? 32 : 44;
    if (x >= m && x <= m + back_size + 8 && y >= header_y - 8 && y <= header_y + back_size + 8)
        return SELECTOR_UI_HIT_BACK;
    int max_scroll = selector_ui_list_max_scroll(count);
    if (scroll_pixels < 0) scroll_pixels = 0;
    if (scroll_pixels > max_scroll) scroll_pixels = max_scroll;
    return hit_row(x, y, tool_list_top(), tool_row_h(), tool_gap(), (int)count,
                   scroll_pixels, footer_top());
}

void selector_ui_draw_notice(const char * title, const char * message) {
    if (!canvas) return;
    fill_rect(0, 0, canvas_width, canvas_height, BG_RGB);
    draw_back_header(title ? title : "Notice", false);
    char safe_message[128];
    bounded_text(safe_message, message ? message : "");
    draw_text_centered(canvas_width / 2, canvas_height / 2 - 14,
                       safe_message, 18, PRIMARY_RGB);
}

void selector_ui_draw_progress(const char * title, const char * message, int percent) {
    if (!canvas) return;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    fill_rect(0, 0, canvas_width, canvas_height, BG_RGB);
    draw_back_header(title ? title : "Working", false);
    char safe_message[128];
    bounded_text(safe_message, message ? message : "");
    draw_text_centered(canvas_width / 2, canvas_height / 2 - 54,
                       safe_message, 18, PRIMARY_RGB);
    int m = margin(), bar_w = canvas_width - 2 * m, bar_h = 20;
    int bar_x = m, bar_y = canvas_height / 2 - bar_h / 2;
    rounded_fill(bar_x, bar_y, bar_w, bar_h, bar_h / 2, BORDER_RGB);
    if (percent > 0) {
        int fill_w = (bar_w - 4) * percent / 100;
        if (fill_w < bar_h - 4 && percent > 0) fill_w = bar_h - 4;
        rounded_fill(bar_x + 2, bar_y + 2, fill_w, bar_h - 4,
                     (bar_h - 4) / 2, accent_rgb);
    }
}
