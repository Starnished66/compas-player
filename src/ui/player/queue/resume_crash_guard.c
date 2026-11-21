#include "resume_crash_guard.h"
#include "../../../core/persistence/storage_paths.h"
#include "../../../core/system/logging/db_log.h"
#include "lvgl/lvgl.h"

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#define RESUME_GUARD_MARKER INTERNAL_COMPAS_DIR "/boot_resume_guard"
#define RESUME_GUARD_SAFE_MS 60000

static void guard_timer_cb(lv_timer_t * timer) {
    (void) timer;
    resume_crash_guard_clear();
}

void resume_crash_guard_clear(void) {
    (void) unlink(RESUME_GUARD_MARKER);
}

/* Decided once per boot: the SD queue restore can follow the last-track
 * resume in the same boot and must not read this boot's own marker as a
 * crash. A paused boot clears the marker so the next boot tries again. */
int resume_crash_guard_filter(int resume_mode) {
    static int decided;
    if (resume_mode != 1) return resume_mode;
    if (decided) return decided;
    if (access(RESUME_GUARD_MARKER, F_OK) == 0) {
        reload_diag_log("boot resume skipped: previous boot resume did not finish safely");
        resume_crash_guard_clear();
        return decided = 2;
    }
    int fd = open(RESUME_GUARD_MARKER, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
    lv_timer_t * timer = lv_timer_create(guard_timer_cb, RESUME_GUARD_SAFE_MS, NULL);
    if (timer) lv_timer_set_repeat_count(timer, 1);
    return decided = 1;
}
