/* compas-sdtool maps stored player paths to a card mounted on the computer.
 * When that mapping fails, the exclusion check must answer "unknown" so the
 * library keeps the song, whichever way the mapper reports the failure. */
#include "src/library/browser/paths/library_ignore.h"
#include <assert.h>
#include <stdlib.h>

static enum { MAP_OK, MAP_UNCHANGED, MAP_EMPTY } mode;
static char card[64];

const char *storage_sd_path(const char *relative) { (void) relative; return ""; }

const char *storage_stored_path(const char *stored, char *out, size_t size) {
    if (mode == MAP_UNCHANGED) return stored;
    if (mode == MAP_EMPTY) { out[0] = '\0'; return out; }
    snprintf(out, size, "%s%s", card, stored + strlen(SD_DEVICE_MOUNT));
    return out;
}

static void touch(const char *path) { FILE *f = fopen(path, "w"); assert(f); fclose(f); }

int main(void) {
    snprintf(card, sizeof(card), "/tmp/compas-map-XXXXXX");
    assert(mkdtemp(card));
    char path[160];
    snprintf(path, sizeof(path), "%s/real", card);
    assert(mkdir(path, 0755) == 0);
    snprintf(path, sizeof(path), "%s/real/song.mp3", card);
    touch(path);
    snprintf(path, sizeof(path), "%s/album", card);
    assert(symlink("real", path) == 0);

    const char *song = SD_DEVICE_MOUNT "/album/song.mp3";
    char out[160];
    mode = MAP_OK;
    assert(library_host_path(song, out, sizeof(out)) != NULL);
    assert(library_path_under_symlink(LIBRARY_STORED_ROOT, song) == LIBRARY_IGNORED);
    mode = MAP_UNCHANGED;
    assert(library_host_path(song, out, sizeof(out)) == NULL);
    assert(library_path_under_symlink(LIBRARY_STORED_ROOT, song) == LIBRARY_IGNORE_UNKNOWN);
    mode = MAP_EMPTY;
    assert(library_host_path(song, out, sizeof(out)) == NULL);
    assert(library_path_under_symlink(LIBRARY_STORED_ROOT, song) == LIBRARY_IGNORE_UNKNOWN);

    char command[96];
    snprintf(command, sizeof(command), "rm -rf '%s'", card);
    assert(system(command) == 0);
    return 0;
}
