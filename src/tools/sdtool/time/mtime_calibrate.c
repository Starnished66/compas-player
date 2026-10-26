#include "../sdtool.h"
#include "../../../core/persistence/storage_paths.h"
#include "../../../library/tagcache/core/tagcache.h"
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>

/* A database the player wrote holds file times in the player's frame. Compare
 * a sample of its rows with the files they name: when most files of the same
 * size differ by one whole zone offset, that offset is what the operating
 * system's conversion (mtime.c) missed, and is added to every card file time.
 * Only a difference this computer's own zone could explain is accepted. Anything
 * else is ignored, and a wrong guess can leave rows that look stale until the
 * player reads those files again at boot. */

#define CALIBRATION_SAMPLES 64

/* This computer's offset from UTC now. */
static long local_utc_offset(void) {
    time_t now = time(NULL);
    struct tm utc = *gmtime(&now);
    utc.tm_isdst = -1;
    return (long) (now - mktime(&utc));
}

/* A correction the card's own zone can explain: this computer's offset, give or
 * take a daylight-saving hour, in either direction. */
static bool plausible(long delta) {
    long magnitude = delta < 0 ? -delta : delta;
    long local = local_utc_offset();
    local = local < 0 ? -local : local;
    return magnitude == local || magnitude == local + 3600 || (local >= 3600 && magnitude == local - 3600);
}

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
    if (count >= 3 && best_count * 5 >= count * 3 && best != 0 && plausible(best))
        sdtool_mtime_set_calibrated(best);
}
