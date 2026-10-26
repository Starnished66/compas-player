#include "device_info.h"
#include "../../library/artwork/albumart/albumart.h"
#include "../../plugins/manager/plugin_api_version.h"
#include "../../library/tagcache/core/tagcache.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void read_file(const char *path, char *out, size_t size) {
    FILE *f = fopen(path, "rb");
    assert(f);
    size_t n = fread(out, 1, size - 1, f);
    out[n] = '\0';
    fclose(f);
}

static void test_format(void) {
    char text[512], expected[128];
    size_t length = device_info_format(text, sizeof(text));
    assert(length > 0 && text[length - 1] == '\n' && strlen(text) == length);
    assert(strncmp(text, "{\"schema\":1,\"model\":\"", 21) == 0);
    snprintf(expected, sizeof(expected), "\"plugin_api\":%d,\"database_format\":%u,\"art_cache\":\"%s\"}\n",
             PLUGIN_API_VERSION, (unsigned) TAGCACHE_DATABASE_FORMAT, ALBUMART_CACHE_VERSION);
    assert(strstr(text, expected));
    assert(strstr(text, "\"player_version\":\""));
    assert(device_info_format(text, 20) == 0);
}

static void test_publish(const char *root) {
    char dir[512], path[544], expected[512], actual[512];
    snprintf(dir, sizeof(dir), "%s/.compas", root);
    snprintf(path, sizeof(path), "%s/device.json", dir);
    device_info_format(expected, sizeof(expected));

    assert(device_info_publish(dir));
    read_file(path, actual, sizeof(actual));
    assert(strcmp(actual, expected) == 0);

    /* Unchanged content is not rewritten: a rename would give a new inode. */
    struct stat before, after;
    assert(stat(path, &before) == 0);
    assert(device_info_publish(dir));
    assert(stat(path, &after) == 0 && before.st_ino == after.st_ino && before.st_mtime == after.st_mtime);

    /* Stale content is replaced and no temp file is left behind. */
    FILE *f = fopen(path, "wb");
    assert(f);
    fputs("{\"schema\":0}\n", f);
    fclose(f);
    assert(device_info_publish(dir));
    read_file(path, actual, sizeof(actual));
    assert(strcmp(actual, expected) == 0);
    char temp[560];
    snprintf(temp, sizeof(temp), "%s.tmp", path);
    assert(access(temp, F_OK) != 0);
}

static void test_no_card(const char *root) {
    char dir[512];
    /* A missing mount point (no card) is a no-op, not an error to recover from. */
    snprintf(dir, sizeof(dir), "%s/absent/.compas", root);
    assert(!device_info_publish(dir));
    snprintf(dir, sizeof(dir), "%s/absent", root);
    assert(access(dir, F_OK) != 0);
}

static void test_read_only(const char *root) {
    if (geteuid() == 0) return; /* root ignores permissions */
    char dir[512], path[544];
    snprintf(dir, sizeof(dir), "%s/readonly", root);
    snprintf(path, sizeof(path), "%s/device.json", dir);
    assert(mkdir(dir, 0755) == 0);
    assert(chmod(dir, 0555) == 0);
    assert(!device_info_publish(dir));
    assert(access(path, F_OK) != 0);
    chmod(dir, 0755);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    test_format();
    test_publish(argv[1]);
    test_no_card(argv[1]);
    test_read_only(argv[1]);
    puts("device info: PASS");
    return 0;
}
