/* Read-only Linux framebuffer contract probe for the R1 display path.
 * This utility only opens the framebuffer read-only and issues FBIOGET_*.
 * It never maps framebuffer memory or changes/presents a display mode. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static int monotonic_us(uint64_t *out)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return -1;
    *out = (uint64_t)ts.tv_sec * UINT64_C(1000000) + (uint64_t)ts.tv_nsec / 1000U;
    return 0;
}

static int ioctl_timed(int fd, unsigned long request, void *arg,
                       uint64_t *elapsed_us, int *timing_valid)
{
    uint64_t start = 0, end = 0;
    *elapsed_us = 0;
    *timing_valid = 0;
    int have_start = monotonic_us(&start) == 0;
    int rc = ioctl(fd, request, arg);
    int saved_errno = errno;
    if (have_start && monotonic_us(&end) == 0) {
        *elapsed_us = end >= start ? end - start : 0;
        *timing_valid = 1;
    }
    errno = saved_errno;
    return rc;
}

static uint64_t min_u64(uint64_t a, uint64_t b)
{
    return a < b ? a : b;
}

static void print_bitfield(const char *name, const struct fb_bitfield *field)
{
    printf("\"%s\":{\"offset\":%u,\"length\":%u,\"msb_right\":%u}",
           name, field->offset, field->length, field->msb_right);
}

int main(int argc, char **argv)
{
    const char *device = "/dev/fb0";
    if (argc == 2) device = argv[1];
    else if (argc != 1) {
        fprintf(stderr, "Usage: %s [framebuffer-device]\n", argv[0]);
        return 2;
    }

    int fd = open(device, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "display_contract_probe: open failed: %s\n", strerror(errno));
        return 1;
    }

    struct fb_fix_screeninfo fix;
    struct fb_var_screeninfo var;
    memset(&fix, 0, sizeof(fix));
    memset(&var, 0, sizeof(var));
    uint64_t fix_ioctl_us = 0, var_ioctl_us = 0;
    int fix_timing_valid = 0, var_timing_valid = 0;
    if (ioctl_timed(fd, FBIOGET_FSCREENINFO, &fix, &fix_ioctl_us, &fix_timing_valid) != 0) {
        int saved_errno = errno;
        (void)close(fd);
        fprintf(stderr, "display_contract_probe: FBIOGET_FSCREENINFO failed: %s\n", strerror(saved_errno));
        return 1;
    }
    if (ioctl_timed(fd, FBIOGET_VSCREENINFO, &var, &var_ioctl_us, &var_timing_valid) != 0) {
        int saved_errno = errno;
        (void)close(fd);
        fprintf(stderr, "display_contract_probe: FBIOGET_VSCREENINFO failed: %s\n", strerror(saved_errno));
        return 1;
    }
    (void)close(fd);

    uint64_t row_bits = (uint64_t)var.xres * var.bits_per_pixel;
    uint64_t row_bytes = (row_bits + 7U) / 8U;
    uint64_t virtual_row_bits = (uint64_t)var.xres_virtual * var.bits_per_pixel;
    uint64_t virtual_row_bytes = (virtual_row_bits + 7U) / 8U;
    uint64_t page_bytes = (uint64_t)fix.line_length * var.yres;
    int stride_valid = var.xres > 0 && var.bits_per_pixel > 0 && row_bytes > 0 &&
                       fix.line_length >= virtual_row_bytes;
    uint64_t pages_by_virtual = var.yres > 0 ? var.yres_virtual / var.yres : 0;
    uint64_t pages_by_memory = page_bytes > 0 ? fix.smem_len / page_bytes : 0;
    /* This is deliberately conservative for FB_VMODE_YWRAP: circular ywrap
     * windows are not treated as a linear active viewport. */
    int offsets_valid = (uint64_t)var.xoffset + var.xres <= var.xres_virtual &&
                        (uint64_t)var.yoffset + var.yres <= var.yres_virtual;
    uint64_t virtual_memory_required = (uint64_t)fix.line_length * var.yres_virtual;
    int virtual_memory_bounds_valid = virtual_memory_required <= fix.smem_len;

    uint64_t active_row_bits = offsets_valid ?
        ((uint64_t)var.xoffset + var.xres) * var.bits_per_pixel : 0;
    uint64_t active_row_end_bytes = (active_row_bits + 7U) / 8U;
    uint64_t active_last_row = offsets_valid && var.yres > 0 ?
        (uint64_t)var.yoffset + var.yres - 1U : 0;
    uint64_t active_row_base = active_last_row * fix.line_length;
    int active_extent_valid = offsets_valid && var.yres > 0 && active_row_end_bytes <= fix.line_length &&
                              active_row_base <= UINT64_MAX - active_row_end_bytes;
    uint64_t active_viewport_end_bytes = active_extent_valid ? active_row_base + active_row_end_bytes : UINT64_MAX;
    int active_viewport_memory_valid = active_extent_valid && active_viewport_end_bytes <= fix.smem_len;
    int geometry_valid = var.xres > 0 && var.yres > 0 && var.bits_per_pixel > 0 &&
                         stride_valid && offsets_valid && active_viewport_memory_valid;
    /* Geometry says only that the active rectangle is addressable with the
     * reported stride. This stricter flag is the RGB565 format contract; it
     * still does not establish controller acceleration or scanout behavior. */
    int rgb565_supported = fix.type == FB_TYPE_PACKED_PIXELS &&
                           fix.visual == FB_VISUAL_TRUECOLOR &&
                           var.bits_per_pixel == 16U && var.grayscale == 0U && var.nonstd == 0U &&
                           var.red.offset == 11U && var.red.length == 5U && var.red.msb_right == 0U &&
                           var.green.offset == 5U && var.green.length == 6U && var.green.msb_right == 0U &&
                           var.blue.offset == 0U && var.blue.length == 5U && var.blue.msb_right == 0U &&
                           var.transp.offset == 0U && var.transp.length == 0U && var.transp.msb_right == 0U;
    uint64_t complete_pages = stride_valid && page_bytes > 0 ? min_u64(pages_by_virtual, pages_by_memory) : 0;

    /* accel_id is copied from fb_fix_screeninfo; it is not a GPU claim. */
    printf("{\"device\":\"fbdev\",\"fixed_ioctl_us\":%" PRIu64
           ",\"fixed_timing_valid\":%s,\"variable_ioctl_us\":%" PRIu64
           ",\"variable_timing_valid\":%s,\"geometry_valid\":%s,\"rgb565_supported\":%s,\"stride_valid\":%s"
           ",\"active_viewport_memory_valid\":%s"
           ",\"virtual_memory_bounds_valid\":%s,\"offsets_valid\":%s,\"xres\":%u,\"yres\":%u"
           ",\"xres_virtual\":%u,\"yres_virtual\":%u,\"xoffset\":%u,\"yoffset\":%u"
           ",\"bits_per_pixel\":%u,\"grayscale\":%u,\"nonstd\":%u,\"vmode\":%u"
           ",\"stride_bytes\":%u,\"visible_row_bytes\":%" PRIu64
           ",\"page_bytes\":%" PRIu64 ",\"active_viewport_end_bytes\":%" PRIu64
           ",\"virtual_memory_required_bytes\":%" PRIu64
           ",\"smem_len\":%u,\"mmio_len\":%u,\"complete_pages\":%" PRIu64
           ",\"panstep_x\":%u,\"panstep_y\":%u,\"visual\":%u,\"type\":%u,\"type_aux\":%u"
           ",\"accel_id\":%u,\"var_accel_flags\":%u,\"bitfields\":{",
           fix_ioctl_us, fix_timing_valid ? "true" : "false",
           var_ioctl_us, var_timing_valid ? "true" : "false",
           geometry_valid ? "true" : "false", rgb565_supported ? "true" : "false",
           stride_valid ? "true" : "false", active_viewport_memory_valid ? "true" : "false",
           virtual_memory_bounds_valid ? "true" : "false", offsets_valid ? "true" : "false",
           var.xres, var.yres, var.xres_virtual, var.yres_virtual, var.xoffset, var.yoffset,
           var.bits_per_pixel, var.grayscale, var.nonstd, var.vmode,
           fix.line_length, row_bytes, page_bytes, active_viewport_end_bytes,
           virtual_memory_required, fix.smem_len, fix.mmio_len,
           complete_pages, fix.xpanstep, fix.ypanstep, fix.visual, fix.type, fix.type_aux, fix.accel,
           var.accel_flags);
    print_bitfield("red", &var.red); printf(",");
    print_bitfield("green", &var.green); printf(",");
    print_bitfield("blue", &var.blue); printf(",");
    print_bitfield("transp", &var.transp);
    printf("}}\n");
    if (fflush(stdout) != 0 || ferror(stdout)) {
        int saved_errno = errno;
        fprintf(stderr, "display_contract_probe: output failed: %s\n",
                strerror(saved_errno ? saved_errno : EIO));
        return 4;
    }
    return geometry_valid ? 0 : 3;
}
