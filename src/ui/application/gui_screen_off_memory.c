#include "gui_screen_off_memory.h"
#include "gui.h"
#include "../library/library_thumbnails.h"
#include "../player/screen/gui_lyrics.h"
#include "../../core/system/logging/db_log.h"
#include "lvgl/src/draw/sw/lv_draw_sw_private.h"

static bool released_this_period;
static uint32_t period_start_ms;

/* Frees only caches that are rebuilt on use. The visible cover stays. Lock
 * screen art is deliberately kept: the lock screen is shown on wake, so
 * freeing it would only delay the reveal. */
static void release_decoded_images(void) {
    library_thumbnails_release_memory();
    gui_player_release_memory_for_screen_off();
    gui_lyrics_release_memory_for_external_session();
    lv_draw_sw_image_cache_clear();
    /* Last, once no widget points at a descriptor that may be cached. */
    lv_image_cache_drop(NULL);
}

void gui_screen_off_memory_tick(bool screen_just_went_off, bool player_running, uint32_t now_ms) {
    if (screen_just_went_off) {
        released_this_period = false;
        period_start_ms = now_ms;
        return;
    }
    if (!gui_screen_off_memory_due(released_this_period, player_running, now_ms - period_start_ms)) return;
    released_this_period = true;
    long before = db_log_rss_kb();
    release_decoded_images();
    DB_LOG("MEM", "screen_off_release rss_kb=%ld->%ld", before, db_log_rss_kb());
}

void gui_screen_off_memory_restart_wait(uint32_t now_ms) {
    released_this_period = false;
    period_start_ms = now_ms;
}

void gui_screen_off_memory_wake(void) {
    gui_player_screen_off_release_wake();
    if (!released_this_period) return;
    DB_LOG("MEM", "screen_on_after_release rss_kb=%ld", db_log_rss_kb());
    released_this_period = false;
}
