#include "layout_crash_guard.h"
#include "player_layouts.h"
#include "../../../core/persistence/boot_marker.h"
#include "../../../core/persistence/storage_paths.h"
#include "../../../core/system/logging/db_log.h"
#include "lvgl/lvgl.h"

#include <stdbool.h>
#include <string.h>

#define LAYOUT_GUARD_MARKER INTERNAL_COMPAS_DIR "/boot_layout_guard"
#define LAYOUT_GUARD_SAFE_MS 5000

static bool decided;
static char tripped_id[PLAYER_LAYOUT_ID_MAX]; /* the id the last boot died building, if any */
static lv_timer_t * safe_timer;

/* Reads the marker once per boot, before anything can release or rewrite it. */
static void decide(void) {
    if (decided) return;
    decided = true;
    if (!boot_marker_read(LAYOUT_GUARD_MARKER, tripped_id, sizeof(tripped_id))) tripped_id[0] = '\0';
}

/* The build was safe or the app exited cleanly: drop the marker, except that
 * a tripped id stays recorded so every boot keeps using Default for it. */
static void release_marker(void) {
    decide();
    if (tripped_id[0]) boot_marker_set_text(LAYOUT_GUARD_MARKER, tripped_id);
    else boot_marker_clear(LAYOUT_GUARD_MARKER);
}

static void safe_timer_cb(lv_timer_t * timer) {
    (void) timer;
    safe_timer = NULL; /* one-shot: LVGL deletes it after this call */
    release_marker();
}

void layout_crash_guard_clear(void) {
    decided = true;
    tripped_id[0] = '\0';
    boot_marker_clear(LAYOUT_GUARD_MARKER);
}

bool layout_crash_guard_tripped(void) {
    decide();
    return tripped_id[0] != '\0';
}

void layout_crash_guard_clean_exit(void) {
    release_marker();
}

const char * layout_crash_guard_filter(const char * layout_id) {
    if (strcmp(layout_id, PLAYER_LAYOUT_ID_DEFAULT) == 0) return layout_id;
    decide();
    if (tripped_id[0] && strcmp(tripped_id, layout_id) == 0) {
        reload_diag_log("player layout '%s' skipped: the last boot died while building it", layout_id);
        return PLAYER_LAYOUT_ID_DEFAULT;
    }
    boot_marker_set_text(LAYOUT_GUARD_MARKER, layout_id);
    if (safe_timer) {
        lv_timer_reset(safe_timer);
    } else {
        safe_timer = lv_timer_create(safe_timer_cb, LAYOUT_GUARD_SAFE_MS, NULL);
        if (safe_timer) lv_timer_set_repeat_count(safe_timer, 1);
    }
    return layout_id;
}
