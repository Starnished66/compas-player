#include "sdtool.h"
#include "storage_paths.h"
#include "tagcache.h"
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>

/* A database the player wrote holds file times in the player's frame. Compare
 * a sample of its rows with the files they name: when most files of the same
 * size differ by one whole zone offset, that offset is what the operating
 * system's conversion (mtime.c) missed, and is added to every card file time.
 * Anything less clear is ignored, so a wrong guess can only cost a rescan. */

#define CALIBRATION_SAMPLES 64
#define ZONE_STEP_SECONDS 900
#define ZONE_SPAN_SECONDS (26 * 3600)

void sdtool_mtime_calibrate(void) {
    long deltas[CALIBRATION_SAMPLES];
    int count = 0;
    int32_t slots = tagcache_slot_count();
    int32_t step = slots / CALIBRATION_SAMPLES + 1;
    sdtool_mtime_set_calibrated(0);
    for (int32_t slot = 0; slot < slots && count < CALIBRATION_SAMPLES; slot += step) {
        tagcache_song_t song;
        char host[PATH_MAX];
        struct stat st;
        if (!tagcache_song_at_slot(slot, &song) ||
            stat(storage_stored_path(song.path, host, sizeof(host)), &st) != 0 ||
            (int64_t) st.st_size != song.size)
            continue;
        deltas[count++] = (long) song.mtime - (long) st.st_mtime;
    }

    long best = 0;
    int best_count = 0;
    for (int i = 0; i < count; i++) {
        int same = 0;
        for (int j = 0; j < count; j++) same += deltas[j] == deltas[i];
        if (same > best_count) {
            best_count = same;
            best = deltas[i];
        }
    }
    long magnitude = best < 0 ? -best : best;
    if (count >= 3 && best_count * 5 >= count * 3 && best != 0 && magnitude <= ZONE_SPAN_SECONDS &&
        magnitude % ZONE_STEP_SECONDS == 0)
        sdtool_mtime_set_calibrated(best);
}
