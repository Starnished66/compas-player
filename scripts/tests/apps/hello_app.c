/* Sample external app for testing the app protocol (never shipped). Fills the
 * framebuffer with a colour taken from the board id, draws the board id and
 * the screen size as text, writes a marker into COMPAS_APP_DATA, and exits on
 * any key or touch press or after 10 seconds. Pass "crash" as the first
 * argument to dereference NULL after drawing, "dark" to also blank the panel
 * and set the backlight to 0 first (to test that the player recovers).
 *
 * Build: mipsel-linux-musl-gcc -static -O2 -o hello hello_app.c
 * Install: copy hello and app.json to <SD>/.apps/hello/ */
#include <dirent.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "font8x12.inc"

static uint8_t * pixels;
static struct fb_var_screeninfo var;
static struct fb_fix_screeninfo fix;

static uint32_t pack(unsigned r, unsigned g, unsigned b) {
    return ((r >> (8 - var.red.length)) << var.red.offset) | ((g >> (8 - var.green.length)) << var.green.offset) |
           ((b >> (8 - var.blue.length)) << var.blue.offset);
}

static void put(int x, int y, uint32_t c) {
    if (x < 0 || y < 0 || x >= (int) var.xres || y >= (int) var.yres) return;
    uint8_t * p = pixels + (size_t) (y + var.yoffset) * fix.line_length + (size_t) x * (var.bits_per_pixel / 8);
    if (var.bits_per_pixel == 16) *(uint16_t *) p = (uint16_t) c;
    else *(uint32_t *) p = c;
}

static void rect(int x, int y, int w, int h, uint32_t c) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) put(x + i, y + j, c);
}

static void text(int x, int y, const char * s, int scale, uint32_t c) {
    for (; *s; s++, x += 8 * scale) {
        const char * at = strchr(GLYPH_CHARS, *s);
        if (!at || !*s) continue;
        const unsigned char * g = glyphs[at - GLYPH_CHARS];
        for (int row = 0; row < 12; row++)
            for (int col = 0; col < 8; col++)
                if (g[row] & (0x80 >> col)) rect(x + col * scale, y + row * scale, scale, scale, c);
    }
}

static int open_inputs(int * fds, int max) {
    int n = 0;
    for (int i = 0; i < 16 && n < max; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0) fds[n++] = fd;
    }
    return n;
}

static void write_text(const char * path, const char * value) {
    int fd = open(path, O_WRONLY);
    if (fd >= 0) {
        ssize_t ignored = write(fd, value, strlen(value));
        (void) ignored;
        close(fd);
    }
}

int main(int argc, char ** argv) {
    const char * board = getenv("COMPAS_BOARD");
    const char * w = getenv("COMPAS_SCREEN_WIDTH");
    const char * h = getenv("COMPAS_SCREEN_HEIGHT");
    const char * data = getenv("COMPAS_APP_DATA");
    printf("hello_app: protocol=%s id=%s board=%s screen=%sx%s data=%s file=%s\n", getenv("COMPAS_APP_PROTOCOL") ?: "?",
           getenv("COMPAS_APP_ID") ?: "?", board ?: "?", w ?: "?", h ?: "?", data ?: "?", argc > 1 ? argv[1] : "-");
    fflush(stdout);
    if (!board) board = "unknown";

    if (data) {
        char path[600];
        snprintf(path, sizeof(path), "%s/last_run.txt", data);
        FILE * f = fopen(path, "w");
        if (f) fprintf(f, "ran on %s\n", board), fclose(f);
    }

    int fb = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if (fb < 0 || ioctl(fb, FBIOGET_VSCREENINFO, &var) != 0 || ioctl(fb, FBIOGET_FSCREENINFO, &fix) != 0 ||
        (var.bits_per_pixel != 16 && var.bits_per_pixel != 32)) {
        perror("hello_app: framebuffer");
        return 1;
    }
    pixels = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);
    if (pixels == MAP_FAILED) {
        perror("hello_app: mmap");
        return 1;
    }

    unsigned hash = 0;
    for (const char * p = board; *p; p++) hash = hash * 31 + (unsigned char) *p;
    unsigned r = 40 + (hash & 0x3f), g = 40 + ((hash >> 6) & 0x3f), b = 80 + ((hash >> 12) & 0x7f);
    rect(0, 0, (int) var.xres, (int) var.yres, pack(r, g, b));
    uint32_t white = pack(255, 255, 255), yellow = pack(255, 220, 60);
    int scale = var.xres >= 400 ? 4 : var.xres >= 300 ? 3 : 2;
    char line[64];
    text(8, 16, "hello app", scale, white);
    text(8, 16 + 14 * scale, board, scale, yellow);
    snprintf(line, sizeof(line), "%ux%u", var.xres, var.yres);
    text(8, 16 + 28 * scale, line, scale, white);
    text(8, 16 + 42 * scale, "any key exits", scale > 2 ? scale - 1 : scale, white);
    /* Bars along the bottom: one per 10% of the 10 seconds left. */
    for (int i = 0; i < 10; i++) rect(8 + i * ((int) var.xres - 16) / 10, (int) var.yres - 40, 8, 24, white);

    if (argc > 1 && strcmp(argv[1], "dark") == 0) {
        write_text("/sys/class/graphics/fb0/blank", "4");
        write_text("/sys/class/backlight/backlight_pwm0/brightness", "0");
    }
    if (argc > 1 && (strcmp(argv[1], "crash") == 0 || strcmp(argv[1], "dark") == 0)) {
        sleep(2);
        *(volatile int *) 0 = 1;
    }

    int fds[16];
    int n = open_inputs(fds, 16);
    struct pollfd pfd[16];
    for (int i = 0; i < n; i++) pfd[i] = (struct pollfd) { .fd = fds[i], .events = POLLIN };
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int shown = 0;;) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        int elapsed = (int) (now.tv_sec - start.tv_sec);
        if (elapsed >= 10) break;
        for (; shown < elapsed; shown++) rect(8 + shown * ((int) var.xres - 16) / 10, (int) var.yres - 40, 8, 24, pack(r, g, b));
        if (poll(pfd, (nfds_t) n, 200) <= 0) continue;
        struct input_event ev;
        int pressed = 0;
        for (int i = 0; i < n; i++)
            while (read(fds[i], &ev, sizeof(ev)) == (ssize_t) sizeof(ev))
                if (ev.type == EV_KEY && ev.value == 1) pressed = 1;
        if (pressed) break;
    }
    return 0;
}
