#include "fb_draw.h"
#include "fb_draw_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef FB_DRAW_HOST
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/fb.h>
#endif

#include "lvgl/src/libs/tjpgd/tjpgd.h"
#include "glyph5x7.h"
#include "startup_splash.h"

int fb_fd = -1;
uint16_t * fb_mem = NULL;
size_t fb_mem_len = 0;
int fb_stride_pixels = 0; /* line_length in PIXELS, not bytes -- see fb_open()'s own comment */
/* Pixel offset of the CURRENTLY DISPLAYED page's first pixel within
 * fb_mem, from vinfo.xoffset/yoffset -- see fb_open()'s own comment on why
 * this can be nonzero on this exact hardware. */
int fb_base_offset_pixels = 0;

/* Compact (no stride padding) heap copy of the decoded background, for
 * fb_restore_background()'s fast blit -- see fb_draw.h's own doc comment on
 * why this exists instead of re-decoding the JPEG on each redraw. NULL until
 * fb_draw_background_jpeg() succeeds at least
 * once. Freed by fb_close() -- this bootloader stays alive as the
 * fork/waitpid supervisor for however long the chosen player runs (see
 * main.c's own run_player_supervised()), so this 768000-byte allocation
 * must not just be left pinned for that entire lifetime once the
 * framebuffer itself is done with it; that matters on this 56 MiB
 * device even though it's a supervisor process doing nothing but
 * waitpid() by that point. */
uint16_t * bg_cache = NULL;

/* Off-screen back buffer used by drawing primitives to assemble frames before
 * blitting to the visible framebuffer in fb_flush(). */
uint16_t * back_buffer = NULL;

#ifndef FB_DRAW_HOST
/* RGB565 specifically, not just "some 16bpp format" -- bits_per_pixel==16
 * alone doesn't rule out RGB555/BGR565/ARGB4444, all also 16bpp. fb_rgb()
 * packs red at bits 15..11, green at 10..5, blue at 4..0; verifying the
 * driver reports that exact layout catches a wrong assumption here loudly
 * (fb_open() returning false) instead of silently drawing
 * correctly-shaped garbage in the wrong channels. */
static bool is_rgb565_layout(const struct fb_var_screeninfo * vinfo) {
    return vinfo->red.offset == 11 && vinfo->red.length == 5 &&
           vinfo->green.offset == 5 && vinfo->green.length == 6 &&
           vinfo->blue.offset == 0 && vinfo->blue.length == 5;
}

/* Same retry shape as src/main.c's own wait_for_fbdev_ready() -- the
 * device node can exist before the underlying driver has actually
 * settled, and a bootloader runs even earlier in boot than the main
 * player (which already needed this same retry). fb_draw.h's own doc
 * comment promises this; an earlier version of this function opened once
 * and gave up immediately on a transient, recoverable startup race. */
#define FB_READY_MAX_ATTEMPTS 50
#define FB_READY_DELAY_MS 100

bool fb_open(void) {
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    bool ready = false;

    for (int attempt = 0; attempt < FB_READY_MAX_ATTEMPTS; attempt++) {
        fb_fd = open("/dev/fb0", O_RDWR);
        if (fb_fd >= 0) {
            if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) == 0 && ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo) == 0) {
                ready = true;
                break;
            }
            close(fb_fd);
            fb_fd = -1;
        }
        usleep(FB_READY_DELAY_MS * 1000);
    }
    if (!ready) {
        perror("fb_draw: /dev/fb0 not ready");
        return false;
    }

    /* Confirmed geometry/format (see fb_draw.h's own comment) -- refusing
     * to draw into anything else is deliberate: a bootloader guessing
     * wrong about pixel format would silently corrupt the display instead
     * of failing loudly. The player can still boot if this returns false. */
    if (vinfo.xres != FB_WIDTH || vinfo.yres != FB_HEIGHT || vinfo.bits_per_pixel != 16 || !is_rgb565_layout(&vinfo)) {
        fprintf(stderr, "fb_draw: unexpected fb format %ux%u @ %ubpp r%u:%u g%u:%u b%u:%u (expected %dx%d RGB565)\n",
                vinfo.xres, vinfo.yres, vinfo.bits_per_pixel, vinfo.red.offset, vinfo.red.length,
                vinfo.green.offset, vinfo.green.length, vinfo.blue.offset, vinfo.blue.length, FB_WIDTH, FB_HEIGHT);
        close(fb_fd);
        fb_fd = -1;
        return false;
    }

    fb_mem_len = finfo.smem_len;
    fb_mem = mmap(NULL, fb_mem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (fb_mem == MAP_FAILED) {
        perror("fb_draw: mmap");
        fb_mem = NULL;
        close(fb_fd);
        fb_fd = -1;
        return false;
    }

    /* line_length is in BYTES and may exceed xres*2 (row padding) --
     * everything below indexes fb_mem as uint16_t*, so this converts once
     * up front rather than dividing by 2 at every single pixel write. */
    fb_stride_pixels = (int) (finfo.line_length / sizeof(uint16_t));

    /* This panel supports panned double-buffering (confirmed by reading
     * lvgl/src/drivers/display/fb/lv_linux_fbdev.c directly: it toggles
     * vinfo.yoffset between 0 and vinfo.yres when yres_virtual >=
     * yres*2). Whichever page the player last left visible is whatever
     * vinfo.yoffset/xoffset already are right now -- writing at a fixed
     * offset 0 would silently draw into the CURRENTLY INVISIBLE page if
     * that happened to be page 1, with no error and nothing ever
     * appearing on screen. */
    fb_base_offset_pixels = (int) vinfo.yoffset * fb_stride_pixels + (int) vinfo.xoffset;

    /* Validate the full drawable range this offset implies actually fits
     * inside the mapped region before ever writing to it -- a driver
     * reporting a yoffset/xoffset that doesn't leave room for a full
     * FB_WIDTH x FB_HEIGHT image would otherwise write past fb_mem_len. */
    size_t last_pixel_index = (size_t) fb_base_offset_pixels + (size_t) (FB_HEIGHT - 1) * fb_stride_pixels + (FB_WIDTH - 1);
    if ((last_pixel_index + 1) * sizeof(uint16_t) > fb_mem_len) {
        fprintf(stderr, "fb_draw: current page offset (yoffset=%u xoffset=%u) leaves no room for a full frame "
                        "in a %zu-byte mapping\n",
                vinfo.yoffset, vinfo.xoffset, fb_mem_len);
        munmap(fb_mem, fb_mem_len);
        fb_mem = NULL;
        close(fb_fd);
        fb_fd = -1;
        return false;
    }

    /* See back_buffer's own doc comment -- everything drawn goes here
     * first, never straight to fb_mem. A failed allocation here means
     * nothing can be drawn at all, same severity as the geometry/mmap
     * failures above -- fail fb_open() itself rather than returning true
     * and having every draw call silently no-op against a NULL buffer. */
    back_buffer = malloc((size_t) FB_WIDTH * FB_HEIGHT * sizeof(uint16_t));
    if (!back_buffer) {
        perror("fb_draw: back buffer malloc");
        munmap(fb_mem, fb_mem_len);
        fb_mem = NULL;
        close(fb_fd);
        fb_fd = -1;
        return false;
    }

    return true;
}

/* Normalizes framebuffer offset to page zero before handoff to the next process.
 * Blits the current frame to page zero if opened on a different page, then pans. */
static void fb_normalize_to_page_zero(void) {
    if (!fb_mem || !back_buffer || fb_fd < 0) return;

    if (fb_base_offset_pixels != 0) {
        for (int y = 0; y < FB_HEIGHT; y++) {
            memcpy(fb_mem + (size_t) y * fb_stride_pixels,
                   back_buffer + (size_t) y * FB_WIDTH, (size_t) FB_WIDTH * sizeof(uint16_t));
        }
    }

    struct fb_var_screeninfo vinfo;
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) != 0) {
        perror("fb_draw: FBIOGET_VSCREENINFO before pan-to-page-0 failed");
        return;
    }
    vinfo.xoffset = 0;
    vinfo.yoffset = 0;
    if (ioctl(fb_fd, FBIOPAN_DISPLAY, &vinfo) != 0) {
        perror("fb_draw: FBIOPAN_DISPLAY to page 0 failed");
    }
}

void fb_close(void) {
    fb_normalize_to_page_zero();

    if (fb_mem) {
        munmap(fb_mem, fb_mem_len);
        fb_mem = NULL;
    }
    if (fb_fd >= 0) {
        close(fb_fd);
        fb_fd = -1;
    }
    /* See bg_cache's/back_buffer's own doc comments -- this process
     * outlives the framebuffer being open (it stays alive as the player
     * supervisor), so neither must just be left pinned once the fb itself
     * is closed. */
    free(bg_cache);
    bg_cache = NULL;
    free(back_buffer);
    back_buffer = NULL;
}
#endif /* FB_DRAW_HOST */

const uint16_t * fb_back_pixels(void) {
    return back_buffer;
}

fb_color_t fb_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return (fb_color_t) (((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/* Targets back_buffer -- see that variable's own doc comment for why
 * nothing draws straight to fb_mem anymore. back_buffer has no stride
 * padding (it's this bootloader's own private, always-(0,0)-based
 * layout), so indexing is a plain y*FB_WIDTH+x, no fb_base_offset_pixels
 * involved here -- that offset only matters at the actual fb_flush()
 * blit out to the real, possibly-panned mmap'd framebuffer. */
static inline void put_pixel(int x, int y, fb_color_t color) {
    if (!back_buffer || x < 0 || y < 0 || x >= FB_WIDTH || y >= FB_HEIGHT) return;
    back_buffer[(size_t) y * FB_WIDTH + x] = color;
}

void fb_fill(fb_color_t color) {
    fb_fill_rect(0, 0, FB_WIDTH, FB_HEIGHT, color);
}

bool fb_draw_blit_rgb565(const uint16_t * pixels, int width, int height, size_t stride_pixels) {
    if (!back_buffer || !pixels || width != FB_WIDTH || height != FB_HEIGHT ||
        stride_pixels < (size_t) FB_WIDTH ||
        stride_pixels > SIZE_MAX / sizeof(uint16_t) ||
        (size_t) FB_HEIGHT > SIZE_MAX / (stride_pixels * sizeof(uint16_t))) {
        return false;
    }
    for (int y = 0; y < FB_HEIGHT; y++) {
        memcpy(back_buffer + (size_t) y * FB_WIDTH,
               pixels + (size_t) y * stride_pixels,
               (size_t) FB_WIDTH * sizeof(uint16_t));
    }
    return true;
}

void fb_fill_rect(int x, int y, int w, int h, fb_color_t color) {
    if (w <= 0 || h <= 0) return;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > FB_WIDTH ? FB_WIDTH : x + w;
    int y1 = y + h > FB_HEIGHT ? FB_HEIGHT : y + h;
    for (int py = y0; py < y1; py++) {
        for (int px = x0; px < x1; px++) put_pixel(px, py, color);
    }
}

/* scale=3 keeps the status text readable on either supported panel without
 * needing anti-aliasing. */
#define GLYPH_SCALE (FB_WIDTH < 400 ? 2 : 3)

void fb_draw_text(int x, int y, const char * text, fb_color_t color) {
    int cursor_x = x;
    for (const char * p = text; *p; p++) {
        const uint8_t * rows = glyph5x7_rows(*p);
        for (int row = 0; row < GLYPH5X7_ROWS; row++) {
            uint8_t bits = rows[row];
            for (int col = 0; col < GLYPH5X7_COLS; col++) {
                if (bits & (1u << (GLYPH5X7_COLS - 1 - col))) {
                    fb_fill_rect(cursor_x + col * GLYPH_SCALE, y + row * GLYPH_SCALE,
                                GLYPH_SCALE, GLYPH_SCALE, color);
                }
            }
        }
        cursor_x += GLYPH5X7_CELL_W * GLYPH_SCALE;
    }
}

int fb_text_width(const char * text) {
    return (int) strlen(text) * GLYPH5X7_CELL_W * GLYPH_SCALE;
}

int fb_text_height(void) {
    return GLYPH5X7_ROWS * GLYPH_SCALE;
}

void fb_draw_text_centered(int y, const char * text, fb_color_t color) {
    int w = fb_text_width(text);
    fb_draw_text((FB_WIDTH - w) / 2, y, text, color);
}

void fb_flush(void) {
    /* The actual visible update -- one tight per-row copy from back_buffer
     * (no stride) into the real, possibly-panned fb_mem (see fb_open()'s
     * own comment on fb_base_offset_pixels/fb_stride_pixels). Everything
     * drawn since the last call to this landed only in back_buffer, never
     * visible on screen until now -- see back_buffer's own doc comment on
     * why every draw call site still calls this exactly where it always
     * did (no main.c changes needed for this fix). */
    if (!fb_mem || !back_buffer) return;
    for (int y = 0; y < FB_HEIGHT; y++) {
        memcpy(fb_mem + (size_t) fb_base_offset_pixels + (size_t) y * fb_stride_pixels,
              back_buffer + (size_t) y * FB_WIDTH, (size_t) FB_WIDTH * sizeof(uint16_t));
    }
}

typedef struct {
    FILE * f;
} jpeg_file_ctx_t;

/* tjpgd calls this with buff==NULL to mean "skip ndata bytes without
 * needing them" (confirmed by reading tjpgd.c directly -- it does this
 * for JPEG segments it has no use for), not just to fill a caller buffer
 * -- must fseek() rather than treat NULL as an error. */
static size_t jpeg_file_read(JDEC * jd, uint8_t * buff, size_t ndata) {
    jpeg_file_ctx_t * ctx = (jpeg_file_ctx_t *) jd->device;
    if (!buff) return fseek(ctx->f, (long) ndata, SEEK_CUR) == 0 ? ndata : 0;
    return fread(buff, 1, ndata, ctx->f);
}

/* JD_FORMAT is 0 in tjpgdcnf.h (shared with the main player's own cover-art
 * decode) -- bitmap here is already plain RGB888, 3 bytes/pixel, no BGR
 * swap needed (that swap in cover_decode.c's own output callback is for a
 * DIFFERENT reason specific to its own pipeline -- this one is required
 * regardless of any downstream pipeline: tjpgd's own conversion loop
 * writes bytes in B,G,R order despite JD_FORMAT's own comment calling it
 * "RGB888"; cover_decode.c's jpeg_mem_output() already swaps indices 0
 * and 2 for exactly this, and without the same swap here every baseline
 * JPEG this draws has its red and blue channels swapped). Writes straight
 * into the framebuffer via put_pixel() (which already accounts for
 * fb_base_offset_pixels) instead of an intermediate buffer -- there is no
 * resize/crop step to justify one, see fb_draw_background_jpeg()'s own
 * doc comment on why. */
static int jpeg_file_output_to_fb(JDEC * jd, void * bitmap, JRECT * rect) {
    (void) jd;
    int w = rect->right - rect->left + 1;
    int h = rect->bottom - rect->top + 1;
    const uint8_t * src = (const uint8_t *) bitmap;

    for (int y = 0; y < h; y++) {
        int dy = rect->top + y;
        for (int x = 0; x < w; x++) {
            int dx = rect->left + x;
            const uint8_t * p = src + (size_t) (y * w + x) * 3;
            put_pixel(dx, dy, fb_rgb(p[2], p[1], p[0])); /* R <- src[2], G <- src[1], B <- src[0] -- see this function's own comment */
        }
    }
    return 1;
}

bool fb_draw_background_jpeg(const char * path) {
    if (!back_buffer) return false;
    FILE * f = fopen(path, "rb");
    if (!f) {
        perror("fb_draw: fopen background jpeg");
        return false;
    }

    jpeg_file_ctx_t ctx = { .f = f };
    uint8_t workbuf[4096];
    JDEC jd;

    JRESULT prep_res = jd_prepare(&jd, jpeg_file_read, workbuf, sizeof(workbuf), &ctx);
    if (prep_res != JDR_OK) {
        fprintf(stderr, "fb_draw: jd_prepare(%s) failed: %d\n", path, (int) prep_res);
        fclose(f);
        return false;
    }
    /* Exact match only -- see this function's own doc comment on why this
     * deliberately doesn't scale/crop to fit instead of refusing. */
    if ((int) jd.width != FB_WIDTH || (int) jd.height != FB_HEIGHT) {
        fprintf(stderr, "fb_draw: background jpeg %s is %ux%u, expected %dx%d\n",
                path, jd.width, jd.height, FB_WIDTH, FB_HEIGHT);
        fclose(f);
        return false;
    }

    JRESULT decomp_res = jd_decomp(&jd, jpeg_file_output_to_fb, 0);
    bool ok = decomp_res == JDR_OK;
    if (!ok) fprintf(stderr, "fb_draw: jd_decomp(%s) failed: %d\n", path, (int) decomp_res);
    fclose(f);

    if (ok) {
        bool tint = false;
        uint32_t accent = 0xB0142B; /* DEFAULT_ACCENT_COLOR */
        startup_splash_get_tint(&tint, &accent);
        if (tint) {
            uint8_t accent_red = (uint8_t)(accent >> 16);
            uint8_t accent_green = (uint8_t)(accent >> 8);
            uint8_t accent_blue = (uint8_t)accent;
            size_t count = (size_t)FB_WIDTH * FB_HEIGHT;
            for (size_t i = 0; i < count; i++) {
                uint16_t pixel = back_buffer[i];
                unsigned red = ((pixel >> 11) & 31u) * 255u / 31u;
                unsigned green = ((pixel >> 5) & 63u) * 255u / 63u;
                unsigned blue = (pixel & 31u) * 255u / 31u;
                unsigned luminance = (299u * red + 587u * green + 114u * blue + 500u) / 1000u;
                back_buffer[i] = fb_rgb((uint8_t)(accent_red * luminance / 255u),
                                        (uint8_t)(accent_green * luminance / 255u),
                                        (uint8_t)(accent_blue * luminance / 255u));
            }
        }
        /* jpeg_file_output_to_fb() decoded into back_buffer (see put_pixel()'s
         * own doc comment) -- both it and bg_cache are the same compact,
         * no-stride layout now, so caching it is one plain memcpy, not a
         * per-row loop against fb_mem's own (possibly padded) stride. */
        free(bg_cache); /* replace any previous cache rather than leaking it -- harmless if this is ever called twice */
        bg_cache = malloc((size_t) FB_WIDTH * FB_HEIGHT * sizeof(uint16_t));
        if (bg_cache) memcpy(bg_cache, back_buffer, (size_t) FB_WIDTH * FB_HEIGHT * sizeof(uint16_t));
        /* A failed malloc here just means fb_restore_background() falls
         * back to a flat fill on every subsequent redraw -- this frame,
         * the one just decoded, still stays on screen until the next
         * fb_flush() actually happens (not blanked immediately), so this
         * isn't a visible regression even under memory pressure. */
    }
    return ok;
}

bool fb_draw_startup_background(void) {
    size_t count = (size_t)FB_WIDTH * FB_HEIGHT;
    uint16_t *pixels = malloc(count * sizeof(*pixels));
    if (pixels && startup_splash_read(STARTUP_SPLASH_PATH, FB_WIDTH, FB_HEIGHT, pixels)) {
        bool ok = fb_draw_blit_rgb565(pixels, FB_WIDTH, FB_HEIGHT, FB_WIDTH);
        if (ok) {
            free(bg_cache);
            bg_cache = pixels;
            return true;
        }
    }
    free(pixels);
    return fb_draw_background_jpeg(BOOTLOADER_BG_PATH);
}

void fb_restore_background(fb_color_t fallback_color) {
    if (!bg_cache) {
        fb_fill(fallback_color);
        return;
    }
    /* Same compact layout as back_buffer -- one plain memcpy, no stride
     * math (that only applies at the actual fb_flush() blit to fb_mem). */
    memcpy(back_buffer, bg_cache, (size_t) FB_WIDTH * FB_HEIGHT * sizeof(uint16_t));
}

void fb_draw_installer_warning(void) {
    fb_restore_background(fb_rgb(0x12, 0x12, 0x12));

    fb_color_t white = fb_rgb(0xFF, 0xFF, 0xFF);
    int text_height = fb_text_height();
    int first_y = FB_HEIGHT / 2 - text_height;
    int second_y = FB_HEIGHT / 2 + text_height / 2;
    int box_y = first_y - 8;
    int box_bottom = second_y + text_height + 8;
    int box_width = fb_text_width("DO NOT REMOVE SD CARD") + 16;
    if (box_width > FB_WIDTH) box_width = FB_WIDTH;
    int box_x = (FB_WIDTH - box_width) / 2;

    fb_fill_rect(box_x, box_y, box_width, box_bottom - box_y, fb_rgb(0x12, 0x12, 0x12));
    fb_draw_text_centered(first_y, "UPDATING PLAYER", white);
    fb_draw_text_centered(second_y, "DO NOT REMOVE SD CARD", white);
}
