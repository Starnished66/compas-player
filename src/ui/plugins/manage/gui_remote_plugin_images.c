#define _POSIX_C_SOURCE 200809L
#include "gui_remote_plugin_images.h"

#include "cJSON.h"
#include "mbedtls/base64.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define REMOTE_PLUGIN_PREVIEW_MAX (512u * 1024u)
#define REMOTE_PLUGIN_PREVIEW_JSON_MAX (1024u * 1024u)

static bool read_preview_file(const char * path, bool png_only, unsigned char ** out,
                              size_t * out_size, const char ** mime) {
    if (!path || !path[0] || !out || !out_size || !mime) return false;
    *out = NULL;
    *out_size = 0;
    *mime = NULL;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return false;
    struct stat st;
    bool valid = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 &&
                 (uint64_t) st.st_size <= REMOTE_PLUGIN_PREVIEW_MAX;
    size_t length = valid ? (size_t) st.st_size : 0;
    unsigned char * bytes = valid ? malloc(length) : NULL;
    valid = valid && bytes != NULL;
    size_t used = 0;
    while (valid && used < length) {
        ssize_t n = read(fd, bytes + used, length - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { valid = false; break; }
        used += (size_t) n;
    }
    unsigned char extra;
    if (valid) {
        ssize_t n;
        do { n = read(fd, &extra, 1); } while (n < 0 && errno == EINTR);
        if (n != 0) valid = false;
    }
    close(fd);
    if (!valid || used != length) { free(bytes); return false; }

    static const unsigned char png_signature[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    bool png = length >= 24 && memcmp(bytes, png_signature, sizeof(png_signature)) == 0 &&
               bytes[8] == 0 && bytes[9] == 0 && bytes[10] == 0 && bytes[11] == 13 &&
               memcmp(bytes + 12, "IHDR", 4) == 0;
    if (png) {
        uint32_t width = ((uint32_t) bytes[16] << 24) | ((uint32_t) bytes[17] << 16) |
                         ((uint32_t) bytes[18] << 8) | bytes[19];
        uint32_t height = ((uint32_t) bytes[20] << 24) | ((uint32_t) bytes[21] << 16) |
                          ((uint32_t) bytes[22] << 8) | bytes[23];
        png = width > 0 && width <= 240 && height > 0 && height <= 400;
    }
    bool jpeg = length >= 4 && bytes[0] == 0xff && bytes[1] == 0xd8 &&
                bytes[length - 2] == 0xff && bytes[length - 1] == 0xd9;
    if ((!png && !jpeg) || (png_only && !png)) { free(bytes); return false; }
    *out = bytes;
    *out_size = length;
    *mime = png ? "image/png" : "image/jpeg";
    return true;
}

char * gui_remote_plugin_preview_json(const char * path, bool png_only) {
    unsigned char * bytes = NULL;
    size_t length = 0;
    const char * mime = NULL;
    if (!read_preview_file(path, png_only, &bytes, &length, &mime)) return NULL;
    size_t encoded_capacity = 4u * ((length + 2u) / 3u);
    char * encoded = malloc(encoded_capacity + 1u);
    if (!encoded) { free(bytes); return NULL; }
    size_t encoded_length = 0;
    int rc = mbedtls_base64_encode((unsigned char *) encoded, encoded_capacity + 1u,
                                   &encoded_length, bytes, length);
    free(bytes);
    if (rc != 0 || encoded_length > encoded_capacity) { free(encoded); return NULL; }
    encoded[encoded_length] = '\0';
    cJSON * root = cJSON_CreateObject();
    if (!root || !cJSON_AddStringToObject(root, "mime", mime) ||
        !cJSON_AddStringToObject(root, "data", encoded)) {
        cJSON_Delete(root);
        free(encoded);
        return NULL;
    }
    free(encoded);
    char * json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return NULL;
    if (strlen(json) > REMOTE_PLUGIN_PREVIEW_JSON_MAX) { free(json); return NULL; }
    return json;
}
