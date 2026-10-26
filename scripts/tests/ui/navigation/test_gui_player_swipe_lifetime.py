#!/usr/bin/env python3
"""Exercise the real Player teardown and swipe-zone registry across rebuilds."""
import pathlib
import re
import subprocess
import sys
import tempfile

root = pathlib.Path(__file__).resolve().parents[4]
player_source = (root / "src/ui/player/screen/gui_player.c").read_text()
shell_source = (root / "src/ui/shell/gui_shell.c").read_text()
volume_source = (root / "src/ui/player/transport/player_volume_popup.c").read_text()


def extract(source, name, return_type=None):
    prefix = r"^(?:static\s+)?"
    if return_type:
        prefix += re.escape(return_type) + r"\s+"
    match = re.search(prefix + re.escape(name) +
                      r"\([^;]*?\)\s*\{.*?^\}", source, re.M | re.S)
    if not match:
        raise RuntimeError("missing production function " + name)
    return match.group()


player_teardown = extract(player_source, "gui_player_teardown_internal", "void")
if "--baseline" in sys.argv:
    player_teardown = player_teardown.replace(
        "    if (progress_slider) unregister_swipe_dead_zone(progress_slider);\n", "")

shell_functions = "\n".join((
    extract(shell_source, "register_swipe_dead_zone", "void"),
    extract(shell_source, "unregister_swipe_dead_zone", "void"),
    extract(shell_source, "point_in_swipe_dead_zone", "bool"),
))
player_init = extract(player_source, "gui_player_init_internal", "void")
volume_teardown = extract(volume_source, "player_volume_popup_teardown", "void")

body = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

typedef struct lv_obj_t lv_obj_t;
typedef struct lv_timer_t { int id; } lv_timer_t;
typedef struct image_t { int id; } image_t;
typedef struct { bool drag_active; int pending; } volume_state_t;
typedef struct { int id; } settings_t;
typedef struct { unsigned token; bool probe, read_file; char path[256]; int metadata; } request_t;
typedef request_t track_metadata_request_t;
typedef struct { int x, y; } lv_point_t;
typedef struct { int x1, y1, x2, y2; } lv_area_t;
struct lv_obj_t {
    int id;
    unsigned flags;
    bool alive, arc;
    lv_obj_t *screen;
    lv_area_t click_area;
};
enum { TRACK_METADATA_CURRENT = 1, LV_OBJ_FLAG_HIDDEN = 1, LV_OBJ_FLAG_CLICKABLE = 2 };
static int lv_arc_class;
#define TR(s) (s)
#define SWIPE_DEAD_ZONE_MAX 72

static lv_obj_t *swipe_dead_zones[SWIPE_DEAD_ZONE_MAX];
static int swipe_dead_zone_count;
static lv_obj_t *active_screen;
static unsigned deleted_player_screens;
static unsigned registered_player_sliders;
static unsigned built_player_screens;
static lv_obj_t player_screens[128], player_sliders[128], native_screen, native_slider;
static bool seekbar_registered;
static lv_obj_t *progress_slider;
static lv_obj_t *player_screen, *song_title_label, *song_count_label, *cover_img;
static lv_obj_t *volume_popup, *volume_popup_track, *volume_popup_speaker_icon;
static lv_obj_t *volume_popup_value_label, *more_menu_popup, *more_menu_popup_backdrop;
static lv_timer_t *player_artwork_cache_timer, *volume_popup_hide_timer;
static image_t volume_popup_speaker_image;
static volume_state_t volume_popup_hv;
static settings_t current_settings;
static bool current_cover_bytes_present, current_cover_dsc_present;
static unsigned displayed_progress_percent, displayed_position_second, displayed_duration_second;
static unsigned deferred_resume_pending, playlist_index, playlist_count, metadata_ui_token;
static unsigned favorite_ui_revision, next_cover_prefetch_token, metadata_ui_audio_generation, metadata_favorite_revision;
static char metadata_ui_path[256];
static bool favorite_track_known, active_track;
static int delete_song_popup;
static atomic_uint cover_decode_generation;
static unsigned calls[32], metadata_requests;
static bool last_refresh_registry;
static lv_obj_t *stable_popup, *stable_menu, *stable_backdrop;
static lv_timer_t *stable_timer, *stable_hide_timer;
static unsigned stable_track_info_generation;

#define current_cover_bytes (current_cover_bytes_present ? (void *)1 : NULL)
#define current_cover_dsc ((struct { void *data; }){current_cover_dsc_present ? (void *)1 : NULL})

static bool lv_obj_has_flag(lv_obj_t *o, unsigned f) { assert(o && o->alive); return (o->flags & f) != 0; }
static lv_obj_t *lv_obj_get_screen(lv_obj_t *o) { assert(o && o->alive); return o->screen; }
static lv_obj_t *lv_screen_active(void) { return active_screen; }
static bool lv_obj_check_type(lv_obj_t *o, const void *class_ptr) { assert(o && o->alive); return o->arc && class_ptr == &lv_arc_class; }
static bool lv_obj_hit_test(lv_obj_t *o, const lv_point_t *p) {
    assert(o && o->alive);
    return p->x >= o->click_area.x1 && p->x <= o->click_area.x2 &&
           p->y >= o->click_area.y1 && p->y <= o->click_area.y2;
}
static void lv_obj_get_click_area(lv_obj_t *o, lv_area_t *area) { assert(o && o->alive); *area = o->click_area; }

''' + shell_functions + r'''

static lv_obj_t *build_player_screen(uint32_t w, uint32_t h, bool refresh_registry) {
    assert(w == 480 && h == 800);
    unsigned i = built_player_screens++;
    assert(i < 128);
    lv_obj_t *screen = &player_screens[i];
    lv_obj_t *slider = &player_sliders[i];
    *screen = (lv_obj_t){ .id = 1000 + (int)i, .alive = true, .screen = screen };
    *slider = (lv_obj_t){ .id = 2000 + (int)i, .alive = true, .screen = screen,
                          .click_area = {10, 20, 110, 40} };
    progress_slider = slider;
    register_swipe_dead_zone(slider);
    registered_player_sliders++;
    last_refresh_registry = refresh_registry;
    return screen;
}
static void player_restore_reload_label_text(void) { calls[1]++; }
static int player_queue_model_current(void) { return (int)playlist_index; }
static int player_queue_model_count(void) { return (int)playlist_count; }
static bool gui_player_has_active_track(void) { return active_track; }
static void lv_label_set_text(lv_obj_t *o, const char *s) { (void)o; (void)s; }
static void player_set_song_count(unsigned i, unsigned n) { (void)i; (void)n; }
static void lv_obj_remove_flag(lv_obj_t *o, unsigned f) { (void)o; (void)f; }
static const char *playlist_path_at(unsigned i) { (void)i; return "/music/current.flac"; }
static bool seed_track_metadata(unsigned i, int *m) { (void)i; *m = 1; return true; }
static unsigned audio_get_playback_generation(void) { return 7; }
static void track_metadata_submit(int kind, request_t *r) { assert(kind == TRACK_METADATA_CURRENT && r->token); metadata_requests++; }
static void lv_image_set_src(lv_obj_t *o, void *d) { (void)o; (void)d; }
static void fit_cover_img_to_card(void) { calls[2]++; }
static void refresh_format_badge(void) { calls[3]++; }
static void gui_player_refresh_frosted_background(void) { calls[4]++; }
static void track_metadata_worker_start(void) { calls[5]++; }
static void favorite_writer_start(void) { calls[6]++; }
static void player_volume_popup_init(void) { calls[7]++; volume_popup=stable_popup; volume_popup_hide_timer=stable_hide_timer; }
static void build_delete_song_popup(void) { calls[8]++; }
static void build_more_menu_popup(void) { calls[9]++; more_menu_popup=stable_menu; more_menu_popup_backdrop=stable_backdrop; }
static void gui_track_info_init(void) { calls[10]++; stable_track_info_generation++; }
static void player_cover_prefetch_cancel(void) { calls[11]++; }
static void player_reflection_cache_clear(void) { calls[12]++; }
static void player_artwork_jobs_partial_clear(void) { calls[13]++; }
static unsigned player_artwork_jobs_advance_generation(void) {
    return atomic_fetch_add(&cover_decode_generation, 1u) + 1u;
}
static void player_inline_lyrics_cancel_timer(void) { calls[14]++; }
static void lv_timer_delete(lv_timer_t *t) { (void)t; calls[15]++; }
static void lv_draw_sw_image_cache_set_transition_active(bool b) { (void)b; }
static void lv_draw_sw_image_cache_set_intermediate_source(void *p) { (void)p; }
static void player_inline_lyrics_set_open(bool b, bool a) { (void)b; (void)a; }
static void player_timeline_labels_teardown(void) { calls[16]++; }
static void player_lyrics_backdrop_teardown(void) { }
static void player_inline_lyrics_generated_release(bool b) { (void)b; }
static int lv_slider_get_value(lv_obj_t *o) { (void)o; return 50; }
static void hw_volume_coalesce_drag_update(volume_state_t *s, int v) { (void)s; (void)v; }
static void hw_volume_coalesce_teardown(volume_state_t *s) { (void)s; calls[17]++; }
static void gui_player_remember_volume_percent(int v) { (void)v; }
static void settings_save(settings_t *s) { (void)s; }
static void lv_timer_del(lv_timer_t *t) { (void)t; calls[18]++; }
static void lv_obj_delete(lv_obj_t *o) {
    assert(o == player_screen && o->alive);
    for (int i = 0; i < swipe_dead_zone_count; ++i) assert(swipe_dead_zones[i] != progress_slider);
    o->alive = false;
    active_screen = &native_screen;
    deleted_player_screens++;
    calls[19]++;
}
static void asset_decoded_image_close(image_t *i) { (void)i; calls[20]++; }
static void gui_popup_teardown(void *p) { (void)p; calls[21]++; }
static void gui_track_info_teardown(void) { calls[22]++; stable_track_info_generation=0; }
static void player_capture_reload_label_text(void) { calls[23]++; }
static void player_inline_lyrics_invalidate_cover_source(void) { calls[24]++; }
static void player_reset_widget_globals(void) {
    calls[25]++;
    cover_img=NULL;
    song_title_label=NULL;
    song_count_label=NULL;
    progress_slider=NULL;
    seekbar_registered=false;
}
static void player_layouts_release(void) { calls[26]++; }
void player_volume_popup_teardown(void);

''' + player_init + "\n" + player_teardown + "\n" + volume_teardown + r'''

static void fixture_init(void) {
    memset(calls, 0, sizeof(calls));
    built_player_screens = registered_player_sliders = deleted_player_screens = 0;
    swipe_dead_zone_count = 0;
    memset(player_screens, 0, sizeof(player_screens));
    memset(player_sliders, 0, sizeof(player_sliders));
    native_screen = (lv_obj_t){ .id=1, .alive=true };
    native_screen.screen = &native_screen;
    active_screen = &native_screen;
    native_slider = (lv_obj_t){ .id=2, .alive=true, .screen=&native_screen,
                                .click_area={200, 20, 260, 40} };
    register_swipe_dead_zone(&native_slider);
    stable_popup=(lv_obj_t *)0x102;
    stable_menu=(lv_obj_t *)0x103;
    stable_backdrop=(lv_obj_t *)0x104;
    stable_timer=(lv_timer_t *)0x201;
    stable_hide_timer=(lv_timer_t *)0x202;
    stable_track_info_generation=1;
    metadata_requests=0;
    player_screen=build_player_screen(480, 800, false);
    active_screen=player_screen;
    song_title_label=(lv_obj_t *)0x301;
    song_count_label=(lv_obj_t *)0x302;
    cover_img=(lv_obj_t *)0x303;
    volume_popup=stable_popup;
    more_menu_popup=stable_menu;
    more_menu_popup_backdrop=stable_backdrop;
    player_artwork_cache_timer=stable_timer;
    volume_popup_hide_timer=stable_hide_timer;
    volume_popup_track=(lv_obj_t *)0x304;
    volume_popup_speaker_icon=(lv_obj_t *)0x305;
    volume_popup_value_label=(lv_obj_t *)0x306;
    volume_popup_hv=(volume_state_t){ .drag_active=true, .pending=42 };
    current_cover_bytes_present=current_cover_dsc_present=true;
    deferred_resume_pending=playlist_index=0;
    playlist_count=1;
    metadata_ui_token=8;
    favorite_ui_revision=9;
    next_cover_prefetch_token=10;
    favorite_track_known=active_track=true;
    atomic_store(&cover_decode_generation, 11);
}

int main(void) {
    fixture_init();
    assert(swipe_dead_zone_count == 2);
    assert(point_in_swipe_dead_zone((lv_point_t){20, 30}));
    assert(!point_in_swipe_dead_zone((lv_point_t){220, 30}));

    for (unsigned cycle = 0; cycle < 100; ++cycle) {
        gui_player_teardown_internal(true);
        assert(player_screen == NULL);
        assert(swipe_dead_zone_count == 1);
        assert(swipe_dead_zones[0] == &native_slider);
        assert(!point_in_swipe_dead_zone((lv_point_t){20, 30}));
        assert(point_in_swipe_dead_zone((lv_point_t){220, 30}));

        gui_player_init_internal(480, 800, true);
        assert(player_screen && player_screen->alive);
        active_screen = player_screen; /* gui_navigation_replace_player_screen() restores the active Player */
        assert(progress_slider && progress_slider->alive);
        assert(swipe_dead_zone_count == 2);
        assert(point_in_swipe_dead_zone((lv_point_t){20, 30}));
        assert(!point_in_swipe_dead_zone((lv_point_t){220, 30}));
    }

    gui_player_teardown_internal(true);
    assert(swipe_dead_zone_count == 1);
    assert(swipe_dead_zones[0] == &native_slider);
    assert(registered_player_sliders == 101);
    assert(deleted_player_screens == 101);
    assert(point_in_swipe_dead_zone((lv_point_t){220, 30}));
    puts("gui player swipe-zone lifetime regression passed (100 rebuild cycles)");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="compas-player-swipe-lifetime-") as folder:
    folder = pathlib.Path(folder)
    c_file = folder / "test.c"
    binary = folder / "test"
    c_file.write_text(body)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(c_file), "-o", str(binary)],
                   check=True, cwd=root)
    subprocess.run([str(binary)], check=True, cwd=root)
