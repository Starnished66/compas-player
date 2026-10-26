#include "lvgl/lvgl.h"
#include "lvgl/src/core/lv_global.h"
#include <stdlib.h>
#include <string.h>

/* cover_decode.c decodes PNG covers with the LodePNG that ships inside LVGL.
 * That file allocates through LVGL's memory API and returns its pixels in an
 * LVGL draw buffer; these are the few entry points it needs, backed by the C
 * library, so the tool links no LVGL code. */

lv_global_t lv_global;

void *lv_malloc(size_t size) { return malloc(size); }
void *lv_realloc(void *data, size_t size) { return realloc(data, size); }
void lv_free(void *data) { free(data); }
void *lv_memcpy(void *dst, const void *src, size_t length) { return memcpy(dst, src, length); }
void lv_memset(void *dst, uint8_t value, size_t length) { memset(dst, value, length); }

lv_draw_buf_t *lv_draw_buf_create_ex(const lv_draw_buf_handlers_t *handlers, uint32_t width,
                                     uint32_t height, lv_color_format_t format, uint32_t stride) {
    (void) handlers;
    lv_draw_buf_t *buf = calloc(1, sizeof(*buf));
    size_t size = (size_t) stride * height;
    if (!buf || !(buf->data = malloc(size ? size : 1))) {
        free(buf);
        return NULL;
    }
    buf->unaligned_data = buf->data;
    buf->data_size = size;
    buf->header.magic = LV_IMAGE_HEADER_MAGIC;
    buf->header.cf = format;
    buf->header.w = width;
    buf->header.h = height;
    buf->header.stride = stride;
    return buf;
}

void lv_draw_buf_destroy(lv_draw_buf_t *buf) {
    if (!buf) return;
    free(buf->unaligned_data);
    free(buf);
}

/* LodePNG's file helpers are never used: covers arrive as memory buffers. */
lv_fs_res_t lv_fs_open(lv_fs_file_t *file, const char *path, lv_fs_mode_t mode) {
    (void) file; (void) path; (void) mode;
    return LV_FS_RES_NOT_IMP;
}
lv_fs_res_t lv_fs_close(lv_fs_file_t *file) { (void) file; return LV_FS_RES_NOT_IMP; }
lv_fs_res_t lv_fs_read(lv_fs_file_t *file, void *buf, uint32_t length, uint32_t *read) {
    (void) file; (void) buf; (void) length; (void) read;
    return LV_FS_RES_NOT_IMP;
}
lv_fs_res_t lv_fs_write(lv_fs_file_t *file, const void *buf, uint32_t length, uint32_t *written) {
    (void) file; (void) buf; (void) length; (void) written;
    return LV_FS_RES_NOT_IMP;
}
lv_fs_res_t lv_fs_seek(lv_fs_file_t *file, uint32_t pos, lv_fs_whence_t whence) {
    (void) file; (void) pos; (void) whence;
    return LV_FS_RES_NOT_IMP;
}
lv_fs_res_t lv_fs_tell(lv_fs_file_t *file, uint32_t *pos) { (void) file; (void) pos; return LV_FS_RES_NOT_IMP; }
