#define _GNU_SOURCE
#include "bt_cover_art.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <jerror.h>
#include <jpeglib.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef BT_COVER_ART_DIR
#define BT_COVER_ART_DIR "/tmp/compas-bt-art"
#endif
#define BT_COVER_ART_SIDE 200
#define BT_COVER_ART_MAX_FILES 4
#define BT_COVER_ART_MAX_BYTES (1024U * 1024U)
#define BT_COVER_ART_MAX_JPEG_BYTES (128U * 1024U)
#define BT_COVER_ART_JPEG_BUFFER_SIZE 4096

typedef struct {
    char name[64];
    size_t size;
    uint64_t last_used;
    uint64_t image_key;
} published_file_t;

typedef struct {
    struct jpeg_destination_mgr pub;
    JOCTET block[BT_COVER_ART_JPEG_BUFFER_SIZE];
    JOCTET * output;
    size_t used;
} bounded_destination_t;

typedef struct {
    struct jpeg_error_mgr pub;
    jmp_buf recovery;
} cover_jpeg_error_t;

static pthread_mutex_t cover_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint64_t current_serial;
static uint64_t use_clock;
static char * current_track_key;
static bool current_key_is_null = true;
static char published_url[128];
static int cover_dir_fd = -1;
static published_file_t published_files[BT_COVER_ART_MAX_FILES];
static size_t published_file_count;
static size_t published_file_bytes;

static void cover_jpeg_error_exit(j_common_ptr cinfo) {
    cover_jpeg_error_t * error = (cover_jpeg_error_t *) cinfo->err;
    longjmp(error->recovery, 1);
}

static void destination_init(j_compress_ptr cinfo) {
    bounded_destination_t * destination = (bounded_destination_t *) cinfo->dest;
    destination->pub.next_output_byte = destination->block;
    destination->pub.free_in_buffer = sizeof(destination->block);
}

static boolean destination_flush(j_compress_ptr cinfo) {
    bounded_destination_t * destination = (bounded_destination_t *) cinfo->dest;
    size_t chunk = sizeof(destination->block);
    if (chunk > BT_COVER_ART_MAX_JPEG_BYTES - destination->used)
        ERREXIT(cinfo, JERR_BUFFER_SIZE);
    memcpy(destination->output + destination->used, destination->block, chunk);
    destination->used += chunk;
    destination->pub.next_output_byte = destination->block;
    destination->pub.free_in_buffer = sizeof(destination->block);
    return TRUE;
}

static void destination_finish(j_compress_ptr cinfo) {
    bounded_destination_t * destination = (bounded_destination_t *) cinfo->dest;
    size_t chunk = sizeof(destination->block) - destination->pub.free_in_buffer;
    if (chunk > BT_COVER_ART_MAX_JPEG_BYTES - destination->used)
        ERREXIT(cinfo, JERR_BUFFER_SIZE);
    memcpy(destination->output + destination->used, destination->block, chunk);
    destination->used += chunk;
}

static void destination_setup(j_compress_ptr cinfo, bounded_destination_t * destination,
                              JOCTET * output) {
    destination->output = output;
    destination->used = 0;
    destination->pub.init_destination = destination_init;
    destination->pub.empty_output_buffer = destination_flush;
    destination->pub.term_destination = destination_finish;
    cinfo->dest = &destination->pub;
}

/* AVRCP requires one DQT and one DHT segment. IJG emits each table
 * separately, so consolidate the small header before publishing it. */
static bool consolidate_jpeg_tables(uint8_t * jpeg, size_t * size) {
    uint8_t header[2048], quant[260], huffman[1024];
    size_t pos = 2, used = 2, nq = 0, nh = 0;
    if (*size < 4 || jpeg[0] != 0xff || jpeg[1] != 0xd8) return false;
    memcpy(header, jpeg, 2);
    while (pos + 4 <= *size) {
        if (jpeg[pos] != 0xff) return false;
        uint8_t marker = jpeg[pos + 1];
        size_t length = ((size_t) jpeg[pos + 2] << 8) | jpeg[pos + 3];
        if (length < 2 || length > *size - pos - 2) return false;
        if (marker == 0xda) break;
        size_t payload = length - 2;
        if (marker == 0xdb) {
            if (payload > sizeof(quant) - nq) return false;
            memcpy(quant + nq, jpeg + pos + 4, payload); nq += payload;
        } else if (marker == 0xc4) {
            if (payload > sizeof(huffman) - nh) return false;
            memcpy(huffman + nh, jpeg + pos + 4, payload); nh += payload;
        } else {
            if (length + 2 > sizeof(header) - used) return false;
            memcpy(header + used, jpeg + pos, length + 2); used += length + 2;
        }
        pos += length + 2;
    }
    if (pos + 4 > *size || jpeg[pos + 1] != 0xda || !nq || !nh ||
        nq + nh + 8 > sizeof(header) - used) return false;
    header[used++] = 0xff; header[used++] = 0xdb;
    header[used++] = (uint8_t) ((nq + 2) >> 8);
    header[used++] = (uint8_t) (nq + 2);
    memcpy(header + used, quant, nq); used += nq;
    header[used++] = 0xff; header[used++] = 0xc4;
    header[used++] = (uint8_t) ((nh + 2) >> 8);
    header[used++] = (uint8_t) (nh + 2);
    memcpy(header + used, huffman, nh); used += nh;
    if (used > pos) return false;
    memmove(jpeg + used, jpeg + pos, *size - pos);
    memcpy(jpeg, header, used);
    *size -= pos - used;
    return true;
}

static bool encode_rgb565(const uint16_t * pixels, int width, int height,
                          uint8_t ** output, size_t * output_size) {
    if (!pixels || width <= 0 || height <= 0 || !output || !output_size) return false;
    *output = NULL;
    *output_size = 0;
    if ((size_t) width > SIZE_MAX / (size_t) height / sizeof(uint16_t)) return false;

    uint8_t * jpeg = malloc(BT_COVER_ART_MAX_JPEG_BYTES);
    struct jpeg_compress_struct * cinfo = calloc(1, sizeof(*cinfo));
    cover_jpeg_error_t * error = calloc(1, sizeof(*error));
    bounded_destination_t * destination = calloc(1, sizeof(*destination));
    if (!jpeg || !cinfo || !error || !destination) {
        free(jpeg); free(cinfo); free(error); free(destination);
        return false;
    }

    cinfo->err = jpeg_std_error(&error->pub);
    error->pub.error_exit = cover_jpeg_error_exit;
    if (setjmp(error->recovery)) {
        jpeg_destroy_compress(cinfo);
        free(jpeg); free(cinfo); free(error); free(destination);
        return false;
    }

    jpeg_create_compress(cinfo);
    destination_setup(cinfo, destination, jpeg);
    cinfo->image_width = BT_COVER_ART_SIDE;
    cinfo->image_height = BT_COVER_ART_SIDE;
    cinfo->input_components = 3;
    cinfo->in_color_space = JCS_RGB;
    jpeg_set_defaults(cinfo);
    jpeg_set_quality(cinfo, 75, TRUE);
    /* AVRCP 1.6 section 5.14.2.2.1 mandates YCC422 and an EXIF image
     * whose APP1 container has no separate embedded thumbnail. */
    cinfo->comp_info[0].h_samp_factor = 2;
    cinfo->comp_info[0].v_samp_factor = 1;
    cinfo->comp_info[1].h_samp_factor = cinfo->comp_info[2].h_samp_factor = 1;
    cinfo->comp_info[1].v_samp_factor = cinfo->comp_info[2].v_samp_factor = 1;
    cinfo->write_JFIF_header = FALSE;
    static const JOCTET exif[] = {
        0x45, 0x78, 0x69, 0x66, 0x00, 0x00, 0x49, 0x49, 0x2a, 0x00, 0x08, 0x00,
        0x00, 0x00, 0x05, 0x00, 0x1a, 0x01, 0x05, 0x00, 0x01, 0x00, 0x00, 0x00,
        0x98, 0x00, 0x00, 0x00, 0x1b, 0x01, 0x05, 0x00, 0x01, 0x00, 0x00, 0x00,
        0xa0, 0x00, 0x00, 0x00, 0x28, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x00, 0x00, 0x13, 0x02, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x69, 0x87, 0x04, 0x00, 0x01, 0x00, 0x00, 0x00,
        0x4a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x90,
        0x07, 0x00, 0x04, 0x00, 0x00, 0x00, 0x30, 0x32, 0x33, 0x32, 0x01, 0x91,
        0x07, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x00, 0x00, 0xa0,
        0x07, 0x00, 0x04, 0x00, 0x00, 0x00, 0x30, 0x31, 0x30, 0x30, 0x01, 0xa0,
        0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0xa0,
        0x04, 0x00, 0x01, 0x00, 0x00, 0x00, 0xc8, 0x00, 0x00, 0x00, 0x03, 0xa0,
        0x04, 0x00, 0x01, 0x00, 0x00, 0x00, 0xc8, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x48, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x48, 0x00,
        0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    }; /* TIFF: required Exif fields, sRGB 200x200, centered chroma, no IFD1. */
    jpeg_start_compress(cinfo, TRUE);
    jpeg_write_marker(cinfo, JPEG_APP0 + 1, exif, sizeof(exif));
    uint8_t rgb_row[BT_COVER_ART_SIDE * 3];
    while (cinfo->next_scanline < cinfo->image_height) {
        int y = (int) cinfo->next_scanline;
        int source_y = (int) ((int64_t) y * height / BT_COVER_ART_SIDE);
        for (int x = 0; x < BT_COVER_ART_SIDE; x++) {
            int source_x = (int) ((int64_t) x * width / BT_COVER_ART_SIDE);
            uint16_t pixel = pixels[(size_t) source_y * (size_t) width + (size_t) source_x];
            size_t offset = (size_t) x * 3U;
            unsigned red = (pixel >> 11) & 0x1fU;
            unsigned green = (pixel >> 5) & 0x3fU;
            unsigned blue = pixel & 0x1fU;
            rgb_row[offset] = (uint8_t) ((red << 3) | (red >> 2));
            rgb_row[offset + 1] = (uint8_t) ((green << 2) | (green >> 4));
            rgb_row[offset + 2] = (uint8_t) ((blue << 3) | (blue >> 2));
        }
        JSAMPROW row = rgb_row;
        if (jpeg_write_scanlines(cinfo, &row, 1) != 1) {
            jpeg_destroy_compress(cinfo);
            free(jpeg); free(cinfo); free(error); free(destination);
            return false;
        }
    }
    jpeg_finish_compress(cinfo);
    size_t encoded_size = destination->used;
    jpeg_destroy_compress(cinfo);
    free(cinfo); free(error); free(destination);
    if (encoded_size == 0 || encoded_size > BT_COVER_ART_MAX_JPEG_BYTES ||
        !consolidate_jpeg_tables(jpeg, &encoded_size)) {
        free(jpeg);
        return false;
    }
    *output = jpeg;
    *output_size = encoded_size;
    return true;
}

static void clean_old_artifacts_locked(int dir_fd) {
    int scan_fd = dup(dir_fd);
    if (scan_fd < 0) return;
    DIR * directory = fdopendir(scan_fd);
    if (!directory) {
        close(scan_fd);
        return;
    }
    struct dirent * entry;
    while ((entry = readdir(directory)) != NULL) {
        if (strncmp(entry->d_name, "cover-", 6) != 0 &&
            strncmp(entry->d_name, ".tmp-", 5) != 0) continue;
        struct stat st;
        if (fstatat(dir_fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0 &&
            S_ISREG(st.st_mode) && st.st_uid == geteuid())
            (void) unlinkat(dir_fd, entry->d_name, 0);
    }
    closedir(directory);
}

static bool ensure_directory_locked(void) {
    if (cover_dir_fd >= 0) return true;
    if (mkdir(BT_COVER_ART_DIR, 0700) != 0 && errno != EEXIST) return false;
    int fd = open(BT_COVER_ART_DIR, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() ||
        fchmod(fd, 0700) != 0) {
        close(fd);
        return false;
    }
    clean_old_artifacts_locked(fd);
    cover_dir_fd = fd;
    return true;
}

uint64_t bt_cover_art_begin_track(const char * track_key) {
    pthread_mutex_lock(&cover_mutex);
    bool is_null = track_key == NULL;
    bool same = (is_null == current_key_is_null) &&
        (is_null || (current_track_key && strcmp(current_track_key, track_key) == 0));
    if (!same) {
        char * key_copy = track_key ? strdup(track_key) : NULL;
        free(current_track_key);
        current_track_key = key_copy;
        current_key_is_null = is_null;
        current_serial++;
        published_url[0] = '\0';
    }
    uint64_t serial = current_serial;
    pthread_mutex_unlock(&cover_mutex);
    return serial;
}

static bool write_all(int fd, const uint8_t * data, size_t size) {
    size_t offset = 0;
    while (offset < size) {
        ssize_t written = write(fd, data + offset, size - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return false;
        offset += (size_t) written;
    }
    return true;
}

/* Allocate unique seven-digit handles across player restarts in this boot. */
static uint64_t allocate_handle_locked(void) {
    int lock_fd = openat(cover_dir_fd, ".handle-lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (lock_fd < 0) return 0;
    struct stat st;
    bool ok = fstat(lock_fd, &st) == 0 && S_ISREG(st.st_mode) &&
              st.st_uid == geteuid() && !(st.st_mode & 0077) &&
              flock(lock_fd, LOCK_EX | LOCK_NB) == 0;
    char number[17];
    uint64_t value = 0;
    if (ok) {
        int fd = openat(cover_dir_fd, ".handle-sequence", O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
        if (fd < 0) {
            ok = errno == ENOENT;
        } else {
            ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == geteuid() &&
                 !(st.st_mode & 0077) && st.st_size > 0 && st.st_size < (off_t) sizeof(number);
            if (ok) {
                ssize_t n = read(fd, number, sizeof(number) - 1);
                ok = n == st.st_size;
                if (ok) {
                    char *end;
                    number[n] = '\0'; errno = 0;
                    value = strtoull(number, &end, 10);
                    ok = !errno && end != number && *end == '\n' && end[1] == '\0' &&
                         number[0] >= '0' && number[0] <= '9';
                }
            }
            close(fd);
        }
    }
    if (ok && value < 9999999) {
        value++;
        char temp[64];
        snprintf(temp, sizeof(temp), ".tmp-sequence-%ld", (long) getpid());
        int fd = openat(cover_dir_fd, temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        int n = snprintf(number, sizeof(number), "%llu\n", (unsigned long long) value);
        ok = fd >= 0 && write_all(fd, (const uint8_t *) number, (size_t) n) && fsync(fd) == 0;
        if (fd >= 0 && close(fd) != 0) ok = false;
        if (ok) ok = renameat(cover_dir_fd, temp, cover_dir_fd, ".handle-sequence") == 0;
        if (!ok && fd >= 0) (void) unlinkat(cover_dir_fd, temp, 0);
    } else { ok = false; }
    close(lock_fd);
    return ok ? value : 0;
}

static bool remove_oldest_locked(void) {
    if (published_file_count == 0) return false;
    size_t oldest = 0;
    for (size_t i = 1; i < published_file_count; i++)
        if (published_files[i].last_used < published_files[oldest].last_used) oldest = i;
    if (unlinkat(cover_dir_fd, published_files[oldest].name, 0) != 0 && errno != ENOENT)
        return false;
    published_file_bytes -= published_files[oldest].size;
    if (published_url[0]) {
        char removed_url[384];
        snprintf(removed_url, sizeof(removed_url), "file://%s/%s", BT_COVER_ART_DIR,
                 published_files[oldest].name);
        if (strcmp(published_url, removed_url) == 0) published_url[0] = '\0';
    }
    published_files[oldest] = published_files[published_file_count - 1];
    published_file_count--;
    return true;
}

bool bt_cover_art_publish_rgb565(uint64_t token, const uint16_t * pixels,
                                 int width, int height, uint64_t image_key) {
    uint8_t * jpeg = NULL;
    size_t jpeg_size = 0;
    if (!encode_rgb565(pixels, width, height, &jpeg, &jpeg_size)) return false;

    pthread_mutex_lock(&cover_mutex);
    if (current_key_is_null || token != current_serial || !ensure_directory_locked()) {
        pthread_mutex_unlock(&cover_mutex);
        free(jpeg);
        return false;
    }
    uint64_t id = allocate_handle_locked();
    if (id == 0) { pthread_mutex_unlock(&cover_mutex); free(jpeg); return false; }
    int dir_fd = cover_dir_fd;
    pthread_mutex_unlock(&cover_mutex);

    char temp_name[64], final_name[64];
    int temp_len = snprintf(temp_name, sizeof(temp_name), ".tmp-%ld-%016llx",
                            (long) getpid(), (unsigned long long) id);
    int final_len = snprintf(final_name, sizeof(final_name), "cover-%07llu.jpg",
                             (unsigned long long) id);
    if (temp_len < 0 || (size_t) temp_len >= sizeof(temp_name) ||
        final_len < 0 || (size_t) final_len >= sizeof(final_name)) {
        free(jpeg);
        return false;
    }

    int fd = openat(dir_fd, temp_name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    bool written = fd >= 0 && write_all(fd, jpeg, jpeg_size) && fsync(fd) == 0;
    if (fd >= 0 && close(fd) != 0) written = false;
    free(jpeg);
    if (!written) {
        if (fd >= 0) (void) unlinkat(dir_fd, temp_name, 0);
        return false;
    }

    pthread_mutex_lock(&cover_mutex);
    if (current_key_is_null || token != current_serial || dir_fd != cover_dir_fd) {
        (void) unlinkat(dir_fd, temp_name, 0);
        pthread_mutex_unlock(&cover_mutex);
        return false;
    }
    while (published_file_count >= BT_COVER_ART_MAX_FILES ||
           jpeg_size > BT_COVER_ART_MAX_BYTES - published_file_bytes) {
        if (!remove_oldest_locked()) {
            (void) unlinkat(dir_fd, temp_name, 0);
            pthread_mutex_unlock(&cover_mutex);
            return false;
        }
    }

    if (renameat(dir_fd, temp_name, dir_fd, final_name) != 0) {
        (void) unlinkat(dir_fd, temp_name, 0);
        pthread_mutex_unlock(&cover_mutex);
        return false;
    }
    published_file_t * record = &published_files[published_file_count++];
    snprintf(record->name, sizeof(record->name), "%s", final_name);
    record->size = jpeg_size;
    record->last_used = ++use_clock;
    record->image_key = image_key;
    published_file_bytes += jpeg_size;

    char url[128];
    int url_len = snprintf(url, sizeof(url), "file://%s/%s", BT_COVER_ART_DIR, final_name);
    if (url_len < 0 || (size_t) url_len >= sizeof(url)) {
        (void) unlinkat(dir_fd, final_name, 0);
        published_file_bytes -= jpeg_size;
        published_file_count--;
        pthread_mutex_unlock(&cover_mutex);
        return false;
    }
    snprintf(published_url, sizeof(published_url), "%s", url);
    pthread_mutex_unlock(&cover_mutex);
    return true;
}

void bt_cover_art_clear(uint64_t token) {
    pthread_mutex_lock(&cover_mutex);
    if (token == current_serial) published_url[0] = '\0';
    pthread_mutex_unlock(&cover_mutex);
}

bool bt_cover_art_reuse(uint64_t token, uint64_t image_key) {
    if (image_key == 0) return false;
    pthread_mutex_lock(&cover_mutex);
    bool reused = false;
    if (!current_key_is_null && token == current_serial) {
        for (size_t i = 0; i < published_file_count; i++) {
            if (published_files[i].image_key != image_key) continue;
            snprintf(published_url, sizeof(published_url), "file://%s/%s",
                     BT_COVER_ART_DIR, published_files[i].name);
            published_files[i].last_used = ++use_clock;
            reused = true;
            break;
        }
    }
    pthread_mutex_unlock(&cover_mutex);
    return reused;
}

bool bt_cover_art_get_url(char * buffer, size_t size) {
    if (!buffer || size == 0) return false;
    pthread_mutex_lock(&cover_mutex);
    size_t length = strlen(published_url);
    bool available = length > 0 && length < size;
    if (available) memcpy(buffer, published_url, length + 1);
    else buffer[0] = '\0';
    pthread_mutex_unlock(&cover_mutex);
    return available;
}
