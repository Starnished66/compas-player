#include "src/ui/application/gui.h"
#include "src/ui/player/screen/gui_lyrics.h"
#include "src/library/text/lyrics/lyrics.h"
#include "src/ui/text/fonts/ui_fonts.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
static pthread_t ui;
static atomic_bool hold, entered, cancelled, fail, exited, synced_result;
static atomic_int calls;
bool __wrap_subsonic_get_lyrics(const subsonic_server_t * server, const char * id,
                               const char * artist, const char * title, char ** text,
                               bool * synced, http_cancel_token_t * cancel) {
    (void)server; (void)artist; (void)title;
    assert(!pthread_equal(ui, pthread_self()));
    atomic_store(&exited, false);
    atomic_fetch_add(&calls, 1); atomic_store(&entered, true);
    while (atomic_load(&hold) && !http_cancel_token_is_cancelled(cancel)) usleep(1000);
    if (http_cancel_token_is_cancelled(cancel)) {
        atomic_store(&cancelled, true);
        usleep(20000); /* Teardown must wait for worker cleanup after cancellation. */
        atomic_store(&exited, true);
        *text = NULL; return false;
    }
    atomic_store(&exited, true);
    if (atomic_load(&fail)) { *text = NULL; return false; }
    if (!strcmp(id, "Oversized lyric")) {
        *text = malloc(LYRICS_MAX_FILE_BYTES + 2U);
        assert(*text);
        memset(*text, 'x', LYRICS_MAX_FILE_BYTES + 1U);
        (*text)[LYRICS_MAX_FILE_BYTES + 1U] = 0;
    } else {
        *text = strdup(id);
    }
    *synced = atomic_load(&synced_result);
    return true;
}
static bool has_text(lv_obj_t * obj, const char * text) {
    if (lv_obj_check_type(obj, &lv_label_class) && !strcmp(lv_label_get_text(obj), text)) return true;
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i)
        if (has_text(lv_obj_get_child(obj, i), text)) return true;
    return false;
}
static void tick(void) { gui_lyrics_poll_load(); lv_tick_inc(160); lv_timer_handler(); usleep(1000); }
static void wait_text(const char * text) {
    for (int i = 0; i < 2000 && !has_text(gui_lyrics_get_screen(), text); ++i) tick();
    assert(has_text(gui_lyrics_get_screen(), text));
}
int main(void) {
    lv_init(); assert(ui_fonts_init());
    assert(lv_sdl_window_create(BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT));
    settings_load(&current_settings); audio_init(); gui_init(BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT);
    ui = pthread_self();
    subsonic_server_t server = { .base_url = "https://example.invalid" };
    atomic_store(&hold, true);
    gui_lyrics_load_subsonic(0, &server, "Obsolete lyric", "Artist", "Title");
    for (int i = 0; i < 20; ++i) tick();
    assert(!atomic_load(&calls));
    gui_lyrics_open_screen();
    for (int i = 0; i < 2000 && !atomic_load(&entered); ++i) tick();
    assert(atomic_load(&entered));
    gui_lyrics_load_subsonic(0, &server, "Current lyric", "Artist", "Title");
    atomic_store(&hold, false);
    wait_text("Current lyric");
    assert(atomic_load(&cancelled));
    assert(!has_text(gui_lyrics_get_screen(), "Obsolete lyric"));
    int before = atomic_load(&calls);
    gui_lyrics_load_subsonic(0, &server, "Current lyric", "Artist", "Title");
    wait_text("Current lyric");
    assert(atomic_load(&calls) == before);
    atomic_store(&fail, true);
    gui_lyrics_load_subsonic(0, &server, "Retry lyric", "Artist", "Title");
    for (int i = 0; i < 100; ++i) tick();
    before = atomic_load(&calls);
    for (int i = 0; i < 100; ++i) tick();
    assert(atomic_load(&calls) == before);
    lv_obj_t * other = lv_obj_create(NULL);
    lv_screen_load(other); tick();
    atomic_store(&fail, false); atomic_store(&synced_result, true);
    gui_lyrics_open_screen();
    wait_text("Retry lyric"); /* Invalid synced text falls back to plain. */
    assert(atomic_load(&calls) == before + 1);
    for (int i = 0; i < 2; ++i) {
        before = atomic_load(&calls);
        gui_lyrics_load_subsonic(0, &server, "Oversized lyric", "Artist", "Title");
        for (int n = 0; n < 2000 && atomic_load(&calls) == before; ++n) tick();
        for (int n = 0; n < 2000 && gui_lyrics_has_background_work(); ++n) tick();
        assert(!gui_lyrics_has_background_work());
        assert(atomic_load(&calls) == before + 1);
        assert(has_text(gui_lyrics_get_screen(), "No lyrics for this song"));
    }
    atomic_store(&hold, true); atomic_store(&entered, false);
    gui_lyrics_load_subsonic(0, &server, "Cancelled at exit", "Artist", "Title");
    for (int i = 0; i < 2000 && !atomic_load(&entered); ++i) tick();
    assert(atomic_load(&entered));
    gui_lyrics_cancel_background_work();
    assert(atomic_load(&exited));
    assert(!gui_lyrics_has_background_work());
    gui_lyrics_cancel_background_work();
    puts("Subsonic lyrics demand loading, cache and same-slot cancellation passed");
}
