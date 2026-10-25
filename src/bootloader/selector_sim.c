#define _POSIX_C_SOURCE 200809L

#include "fb_draw.h"
#include "selector_ui.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Host selector snapshots. Draws the real selector_ui into the host
 * framebuffer backend and writes one PNG per selection row. */

#if defined(BOARD_R3PROII)
#define BOARD_SLUG "r3proii"
#elif defined(BOARD_R3II_2025)
#define BOARD_SLUG "r3ii_2025"
#else
#define BOARD_SLUG "r1"
#endif

#define DEFAULT_FONT_PATH "assets/fonts/Montserrat-Medium.ttf"
#define DEFAULT_OUT_DIR "scratch/selector-mockup"

static uint32_t crc32(const unsigned char * data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t) -(int32_t) (crc & 1u));
    }
    return crc ^ 0xFFFFFFFFu;
}

static uint32_t adler32(const unsigned char * data, size_t len) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; i++) {
        a += data[i];
        if (a >= 65521u) a -= 65521u;
        b += a;
        if (b >= 65521u) b -= 65521u;
    }
    return (b << 16) | a;
}

static void put_be32(unsigned char * p, uint32_t value) {
    p[0] = (unsigned char) (value >> 24);
    p[1] = (unsigned char) (value >> 16);
    p[2] = (unsigned char) (value >> 8);
    p[3] = (unsigned char) value;
}

static void put_le16(unsigned char * p, uint16_t value) {
    p[0] = (unsigned char) value;
    p[1] = (unsigned char) (value >> 8);
}

static int write_all(FILE * f, const void * data, size_t len) {
    return fwrite(data, 1, len, f) == len ? 0 : -1;
}

static int write_chunk(FILE * f, const char type[4], const unsigned char * data, size_t len) {
    unsigned char header[8];
    unsigned char crc_buf[4];
    size_t total = 4 + len;
    unsigned char * joined = malloc(total);
    if (!joined) return -1;
    memcpy(joined, type, 4);
    if (len) memcpy(joined + 4, data, len);
    put_be32(header, (uint32_t) len);
    memcpy(header + 4, type, 4);
    put_be32(crc_buf, crc32(joined, total));
    free(joined);
    if (write_all(f, header, sizeof(header)) != 0) return -1;
    if (len && write_all(f, data, len) != 0) return -1;
    return write_all(f, crc_buf, 4);
}

static int write_png(const char * path, const uint16_t * pixels, int width, int height) {
    if (!path || !pixels || width <= 0 || height <= 0) return -1;
    size_t raw_stride = 1u + (size_t) width * 3u;
    size_t raw_size = raw_stride * (size_t) height;
    unsigned char * raw = malloc(raw_size);
    if (!raw) return -1;
    for (int y = 0; y < height; y++) {
        unsigned char * row = raw + (size_t) y * raw_stride;
        row[0] = 0;
        for (int x = 0; x < width; x++) {
            uint16_t pixel = pixels[(size_t) y * (size_t) width + (size_t) x];
            unsigned red = (pixel >> 11) & 31u;
            unsigned green = (pixel >> 5) & 63u;
            unsigned blue = pixel & 31u;
            unsigned char * dst = row + 1 + (size_t) x * 3u;
            dst[0] = (unsigned char) ((red << 3) | (red >> 2));
            dst[1] = (unsigned char) ((green << 2) | (green >> 4));
            dst[2] = (unsigned char) ((blue << 3) | (blue >> 2));
        }
    }

    size_t max_stored = 65535u;
    size_t nblocks = (raw_size + max_stored - 1u) / max_stored;
    size_t idat_size = 2u + nblocks * 5u + raw_size + 4u;
    unsigned char * idat = malloc(idat_size);
    if (!idat) {
        free(raw);
        return -1;
    }
    idat[0] = 0x78;
    idat[1] = 0x01;
    size_t offset = 2;
    size_t remaining = raw_size;
    const unsigned char * src = raw;
    while (remaining) {
        size_t chunk = remaining > max_stored ? max_stored : remaining;
        idat[offset] = remaining == chunk ? 1u : 0u;
        put_le16(idat + offset + 1, (uint16_t) chunk);
        put_le16(idat + offset + 3, (uint16_t) ~((uint16_t) chunk));
        memcpy(idat + offset + 5, src, chunk);
        offset += 5 + chunk;
        src += chunk;
        remaining -= chunk;
    }
    put_be32(idat + offset, adler32(raw, raw_size));
    offset += 4;
    free(raw);

    FILE * f = fopen(path, "wb");
    if (!f) {
        perror(path);
        free(idat);
        return -1;
    }
    static const unsigned char signature[] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    unsigned char ihdr[13];
    put_be32(ihdr, (uint32_t) width);
    put_be32(ihdr + 4, (uint32_t) height);
    ihdr[8] = 8;
    ihdr[9] = 2;
    ihdr[10] = 0;
    ihdr[11] = 0;
    ihdr[12] = 0;
    int ok = write_all(f, signature, sizeof(signature)) == 0 &&
             write_chunk(f, "IHDR", ihdr, sizeof(ihdr)) == 0 &&
             write_chunk(f, "IDAT", idat, offset) == 0 &&
             write_chunk(f, "IEND", NULL, 0) == 0;
    free(idat);
    if (fclose(f) != 0) ok = 0;
    if (!ok) {
        fprintf(stderr, "selector_sim: failed to write %s\n", path);
        return -1;
    }
    printf("wrote %s\n", path);
    return 0;
}

static int mkdir_p(const char * path) {
    char buf[512];
    if (!path || !*path) return -1;
    if (strlen(path) >= sizeof(buf)) return -1;
    memcpy(buf, path, strlen(path) + 1);
    for (char * p = buf + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(buf, 0755) != 0 && errno != EEXIST) return -1;
        *p = '/';
    }
    if (mkdir(buf, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int present_and_save(const char * path) {
    size_t stride = 0;
    const uint16_t * pixels = selector_ui_pixels(&stride);
    if (!pixels || !fb_draw_blit_rgb565(pixels, FB_WIDTH, FB_HEIGHT, stride)) {
        fprintf(stderr, "selector_sim: blit failed\n");
        return -1;
    }
    fb_flush();
    const uint16_t * frame = fb_back_pixels();
    if (!frame) {
        fprintf(stderr, "selector_sim: empty back buffer\n");
        return -1;
    }
    return write_png(path, frame, FB_WIDTH, FB_HEIGHT);
}

int main(int argc, char ** argv) {
    const char * out_dir = argc > 1 ? argv[1] : DEFAULT_OUT_DIR;
    const char * font_path = argc > 2 ? argv[2] : DEFAULT_FONT_PATH;
    if (mkdir_p(out_dir) != 0) {
        fprintf(stderr, "selector_sim: mkdir %s: %s\n", out_dir, strerror(errno));
        return 1;
    }
    if (!selector_ui_init(FB_WIDTH, FB_HEIGHT, font_path, NULL)) {
        fprintf(stderr, "selector_sim: selector_ui_init failed\n");
        return 1;
    }
    if (!fb_open()) {
        fprintf(stderr, "selector_sim: fb_open failed\n");
        selector_ui_shutdown();
        return 1;
    }

    static const char * names[] = { "compas", "rockbox", "tools" };
    int failed = 0;
    for (int selected = 0; selected < 3; selected++) {
        selector_ui_draw_selector(selected, true, 5, false, false);
        char path[512];
        if (snprintf(path, sizeof(path), "%s/before-%s-%s.png",
                     out_dir, BOARD_SLUG, names[selected]) >= (int) sizeof(path)) {
            fprintf(stderr, "selector_sim: output path too long\n");
            failed = 1;
            break;
        }
        if (present_and_save(path) != 0) failed = 1;
    }

    fb_close();
    selector_ui_shutdown();
    return failed ? 1 : 0;
}
