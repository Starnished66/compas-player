#include "resume_crash_guard.h"
#include "../../../core/persistence/boot_marker.h"
#include "../../../core/persistence/storage_paths.h"
#include "../../../core/system/logging/db_log.h"
#include "lvgl/lvgl.h"

#define RESUME_GUARD_MARKER INTERNAL_COMPAS_DIR "/boot_resume_guard"
#define RESUME_GUARD_PAUSED_MARKER INTERNAL_COMPAS_DIR "/boot_resume_paused"
#define RESUME_GUARD_SAFE_MS 300000

static bool tripped;
static bool skip_decided;
static bool skip;
static bool paused_armed; /* this boot left the second-stage marker */
static int decided; /* this boot's guarded mode, 0 until the filter runs */

void resume_crash_guard_clear(void) {
    boot_marker_clear(RESUME_GUARD_MARKER);
    boot_marker_clear(RESUME_GUARD_PAUSED_MARKER);
}

static void guard_timer_cb(lv_timer_t * timer) {
    (void) timer;
    resume_crash_guard_clear();
}

static void start_safe_timer(void) {
    lv_timer_t * timer = lv_timer_create(guard_timer_cb, RESUME_GUARD_SAFE_MS, NULL);
    if (timer) lv_timer_set_repeat_count(timer, 1);
}

/* Second stage: a boot that the guard paused leaves its own marker. If that
 * boot dies too, the crash is in showing the restored track, so the next boot
 * restores nothing. Decided once per boot, before the first stage can set the
 * marker it reads. */
bool resume_crash_guard_skip_restore(void) {
    if (skip_decided) return skip;
    skip_decided = true;
    if (boot_marker_exists(RESUME_GUARD_PAUSED_MARKER)) {
        reload_diag_log("boot restore skipped: the last paused boot did not finish safely");
        boot_marker_clear(RESUME_GUARD_PAUSED_MARKER);
        skip = true;
    }
    return skip;
}

/* Before the filter has run (card not mounted at startup) an existing marker
 * still means the last boot resume crashed. */
bool resume_crash_guard_tripped(void) {
    return tripped || (!decided && boot_marker_exists(RESUME_GUARD_MARKER));
}

/* Decided once per boot: the SD queue restore can follow the last-track
 * resume in the same boot and must not read this boot's own marker as a
 * crash. A paused boot clears the first marker so the next boot tries again,
 * and leaves the second one. */
int resume_crash_guard_filter(int resume_mode) {
    (void) resume_crash_guard_skip_restore();
    if (resume_mode != 1) {
        /* Shows the track without playing it: only the second stage applies. */
        if (!decided && !paused_armed) {
            paused_armed = true;
            boot_marker_set(RESUME_GUARD_PAUSED_MARKER);
            start_safe_timer();
        }
        return resume_mode;
    }
    if (decided) return decided;
    if (boot_marker_exists(RESUME_GUARD_MARKER)) {
        reload_diag_log("boot resume skipped: previous boot resume did not finish safely");
        boot_marker_clear(RESUME_GUARD_MARKER);
        boot_marker_set(RESUME_GUARD_PAUSED_MARKER);
        tripped = true;
        start_safe_timer();
        return decided = 2;
    }
    boot_marker_set(RESUME_GUARD_MARKER);
    start_safe_timer();
    return decided = 1;
}
