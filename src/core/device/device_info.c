#include "device_info.h"
#include "../../library/artwork/albumart/albumart.h"
#include "../system/runtime/app_version.h"
#include "../../library/artwork/support/library_endian.h"
#include "../../plugins/manager/plugin_api_version.h"
#include "../../library/tagcache/core/tagcache.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DEVICE_INFO_SCHEMA 1
#define DEVICE_INFO_FILE "device.json"

/* The version label is free text (a release title can hold quotes), so it is
 * escaped; the other values are fixed identifiers. */
static size_t escape_json(const char *text, char *out, size_t size) {
    size_t length = 0;
    for (const unsigned char *p = (const unsigned char *) text; *p; p++) {
        if (*p < 0x20) continue;
        size_t need = (*p == '"' || *p == '\\') ? 2 : 1;
        if (length + need >= size) return 0;
        if (need == 2) out[length++] = '\\';
        out[length++] = (char) *p;
    }
    out[length] = '\0';
    return length;
}

size_t device_info_format(char *out, size_t size) {
    char version[128];
    if (!escape_json(app_version_label(), version, sizeof(version))) return 0;
    int length = snprintf(out, size,
        "{\"schema\":%d,\"model\":\"%s\",\"player_version\":\"%s\",\"plugin_api\":%d,"
        "\"database_format\":%u,\"art_cache\":\"%s\"}\n",
        DEVICE_INFO_SCHEMA, app_board_model(), version, PLUGIN_API_VERSION,
        (unsigned) TAGCACHE_DATABASE_FORMAT, ALBUMART_CACHE_VERSION);
    return length > 0 && (size_t) length < size ? (size_t) length : 0;
}

static bool content_matches(const char *path, const char *content, size_t length) {
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    char current[512];
    size_t got = fread(current, 1, sizeof(current), file);
    bool same = !ferror(file) && got == length && memcmp(current, content, length) == 0;
    fclose(file);
    return same;
}

/* Temp file, flush to the card, rename over the old file, flush the folder:
 * the same sequence the player's other small state files use, so a pulled card
 * leaves either the old file or the new one. */
static bool write_atomically(const char *dir, const char *path, const char *content, size_t length) {
    char temp[600];
    if (snprintf(temp, sizeof(temp), "%s/" DEVICE_INFO_FILE ".tmp", dir) >= (int) sizeof(temp)) return false;
    int fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    bool ok = write(fd, content, length) == (ssize_t) length && fsync(fd) == 0;
    ok = close(fd) == 0 && ok;
    if (ok) ok = rename(temp, path) == 0;
    if (!ok) {
        unlink(temp);
        return false;
    }
    (void) library_fsync_dir(dir);
    return true;
}

bool device_info_publish(const char *compas_dir) {
    char content[512];
    char path[600];
    size_t length = device_info_format(content, sizeof(content));
    if (!length || snprintf(path, sizeof(path), "%s/" DEVICE_INFO_FILE, compas_dir) >= (int) sizeof(path))
        return false;
    if (content_matches(path, content, length)) return true;
    if (mkdir(compas_dir, 0755) != 0 && errno != EEXIST) return false;
    return write_atomically(compas_dir, path, content, length);
}
