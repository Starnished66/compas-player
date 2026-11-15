#include "src/ui/application/gui.h"
#include "src/ui/player/screen/gui_track_info.h"
#include "src/ui/text/fonts/ui_fonts.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static pthread_t ui_thread;
static atomic_bool hold_probe, entered;
bool __real_audio_probe_file_format(const char *, audio_current_format_info_t *);
bool __wrap_audio_probe_file_format(const char *path, audio_current_format_info_t *out) {
    assert(!pthread_equal(ui_thread, pthread_self()));
    assert(!strstr(path, "#compas-cue="));
    atomic_store(&entered, true);
    while (atomic_load(&hold_probe)) usleep(1000);
    return __real_audio_probe_file_format(path, out);
}
static bool has_text(lv_obj_t *obj, const char *text) {
    if (lv_obj_check_type(obj, &lv_label_class) && !strcmp(lv_label_get_text(obj), text)) return true;
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++)
        if (has_text(lv_obj_get_child(obj, i), text)) return true;
    return false;
}
static void wait_for(const char *text) {
    for (int i = 0; i < 2000 && !has_text(gui_track_info_get_screen(), text); i++) {
        gui_track_info_poll(); usleep(1000);
    }
    assert(has_text(gui_track_info_get_screen(), text));
}
int main(void) {
    lv_init(); assert(ui_fonts_init());
    assert(lv_sdl_window_create(BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT));
    settings_load(&current_settings); audio_init();
    gui_init(BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT);
    ui_thread = pthread_self();
    gui_track_info_context_t playing = { .source = GUI_TRACK_SOURCE_LOCAL };
    strcpy(playing.path, "./music/current.wav");
    gui_track_info_set_current(&playing);
    gui_track_info_context_t selected = { .source = GUI_TRACK_SOURCE_LOCAL };
    strcpy(selected.path, "./music/selected.wav");
    atomic_store(&hold_probe, true);
    gui_track_info_open_for_context(&selected);
    for (int i = 0; i < 2000 && !atomic_load(&entered); i++) usleep(1000);
    assert(atomic_load(&entered));
    gui_track_info_poll(); /* Must return even while the file worker is held. */
    assert(!has_text(gui_track_info_get_screen(), "16-bit / 48 kHz"));
    atomic_store(&hold_probe, false);
    wait_for("16-bit / 48 kHz");
    assert(has_text(gui_track_info_get_screen(), "Stereo (2 channels)"));
    assert(has_text(gui_track_info_get_screen(), "~1536 kbps"));
    assert(has_text(gui_track_info_get_screen(), "0:02"));
    nav_pop_immediate();
    strcpy(selected.path, "./music/selected.cue#compas-cue=2");
    gui_track_info_open_for_context(&selected);
    wait_for("16-bit / 48 kHz");
    assert(has_text(gui_track_info_get_screen(), "wav"));
    assert(has_text(gui_track_info_get_screen(), "375.0 KB"));
    assert(has_text(gui_track_info_get_screen(), "0:01"));
    assert(has_text(gui_track_info_get_screen(), "~1536 kbps"));
    /* A late result after leaving must not replace the playing context. */
    nav_pop_immediate();
    atomic_store(&hold_probe, true); atomic_store(&entered, false);
    gui_track_info_open_for_context(&selected);
    for (int i = 0; i < 2000 && !atomic_load(&entered); i++) usleep(1000);
    assert(atomic_load(&entered));
    nav_pop_immediate();
    atomic_store(&hold_probe, false);
    gui_track_info_open();
    for (int i = 0; i < 100; i++) { gui_track_info_poll(); usleep(1000); }
    assert(!has_text(gui_track_info_get_screen(), "16-bit / 48 kHz"));
    puts("Information metadata, CUE facts and stale completion passed");
}
