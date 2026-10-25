#!/usr/bin/env python3
"""Compile the production Player init/teardown paths against lifecycle mocks."""
import pathlib
import re
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
source = (root / "src/ui/gui_player.c").read_text()
volume_source = (root / "src/ui/player/player_volume_popup.c").read_text()


def function(name):
    match = re.search(r"^static void " + name +
                      r"\([^;]*?\) \{.*?^\}", source, re.M | re.S)
    if not match:
        raise RuntimeError("missing production function " + name)
    return match.group()


production = "\n".join(function(name) for name in (
    "gui_player_init_internal", "gui_player_teardown_internal"))
volume_teardown = re.search(r"(?m)^void player_volume_popup_teardown\([^;]*?\) \{.*?^\}", volume_source, re.S)
if not volume_teardown:
    raise RuntimeError("missing production function player_volume_popup_teardown")
volume_teardown = volume_teardown.group()

body = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

typedef struct lv_obj_t { int id; } lv_obj_t;
typedef struct lv_timer_t { int id; } lv_timer_t;
typedef struct image_t { int id; } image_t;
typedef struct { bool drag_active; int pending; } volume_state_t;
typedef struct { int id; } settings_t;
typedef struct { unsigned token; bool probe, read_file; char path[256]; int metadata; } request_t;
typedef request_t track_metadata_request_t;
enum { TRACK_METADATA_CURRENT = 1, LV_OBJ_FLAG_HIDDEN = 1 };
#define TR(s) (s)

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
static lv_obj_t *stable_player, *stable_popup, *stable_menu, *stable_backdrop;
static lv_timer_t *stable_timer, *stable_hide_timer;
static unsigned stable_track_info_generation;

#define current_cover_bytes (current_cover_bytes_present ? (void *)1 : NULL)
#define current_cover_dsc ((struct { void *data; }){current_cover_dsc_present ? (void *)1 : NULL})
static lv_obj_t *build_player_screen(uint32_t w, uint32_t h, bool refresh_registry) { assert(w==480&&h==800); calls[0]++;last_refresh_registry=refresh_registry; return stable_player; }
static void player_restore_reload_label_text(void) { calls[1]++; }
static int player_queue_model_current(void) { return (int)playlist_index; }
static int player_queue_model_count(void) { return (int)playlist_count; }
static bool gui_player_has_active_track(void) { return active_track; }
static void lv_label_set_text(lv_obj_t *o, const char *s) { (void)o;(void)s; }
static void player_set_song_count(unsigned i, unsigned n) { (void)i;(void)n; }
static void lv_obj_remove_flag(lv_obj_t *o, unsigned f) { (void)o;(void)f; }
static const char *playlist_path_at(unsigned i) { (void)i; return "/music/current.flac"; }
static bool seed_track_metadata(unsigned i, int *m) { (void)i;*m=1;return true; }
static unsigned audio_get_playback_generation(void) { return 7; }
static void track_metadata_submit(int kind, request_t *r) { assert(kind==TRACK_METADATA_CURRENT&&r->token);metadata_requests++; }
static void lv_image_set_src(lv_obj_t *o, void *d) { (void)o;(void)d; }
static void fit_cover_img_to_card(void) { calls[2]++; }
static void refresh_format_badge(void) { calls[3]++; }
static void gui_player_refresh_frosted_background(void) { calls[4]++; }
static void track_metadata_worker_start(void) { calls[5]++; }
static void favorite_writer_start(void) { calls[6]++; }
static void player_volume_popup_init(void) { calls[7]++; volume_popup=stable_popup;volume_popup_hide_timer=stable_hide_timer; }
static void build_delete_song_popup(void) { calls[8]++; }
static void build_more_menu_popup(void) { calls[9]++;more_menu_popup=stable_menu;more_menu_popup_backdrop=stable_backdrop; }
static void gui_track_info_init(void) { calls[10]++;stable_track_info_generation++; }
static void player_cover_prefetch_cancel(void) { calls[11]++; }
static void player_reflection_cache_clear(void) { calls[12]++; }
static void player_artwork_jobs_partial_clear(void) { calls[13]++; }
static unsigned player_artwork_jobs_advance_generation(void) {
    return atomic_fetch_add(&cover_decode_generation, 1u) + 1u;
}
static void player_inline_lyrics_cancel_timer(void) { calls[14]++; }
static void lv_timer_delete(lv_timer_t *t) { (void)t;calls[15]++; }
static void lv_draw_sw_image_cache_set_transition_active(bool b) { (void)b; }
static void lv_draw_sw_image_cache_set_intermediate_source(void *p) { (void)p; }
static void player_inline_lyrics_set_open(bool b, bool a) { (void)b;(void)a; }
static void player_timeline_labels_teardown(void) { calls[16]++; }
static void player_lyrics_backdrop_teardown(void) { }
static void player_inline_lyrics_generated_release(bool b) { (void)b; }
static int lv_slider_get_value(lv_obj_t *o) { (void)o;return 50; }
static void hw_volume_coalesce_drag_update(volume_state_t *s, int v) { (void)s;(void)v; }
static void hw_volume_coalesce_teardown(volume_state_t *s) { (void)s;calls[17]++; }
static void gui_player_remember_volume_percent(int v) { (void)v; }
static void settings_save(settings_t *s) { (void)s; }
static void lv_timer_del(lv_timer_t *t) { (void)t;calls[18]++; }
static void unregister_swipe_dead_zone(lv_obj_t *o) { assert(o==progress_slider);seekbar_registered=false;calls[27]++; }
static void lv_obj_delete(lv_obj_t *o) { if(o==player_screen) assert(!seekbar_registered);calls[19]++; }
static void asset_decoded_image_close(image_t *i) { (void)i;calls[20]++; }
static void gui_popup_teardown(void *p) { (void)p;calls[21]++; }
static void gui_track_info_teardown(void) { calls[22]++;stable_track_info_generation=0; }
static void player_capture_reload_label_text(void) { calls[23]++; }
static void player_inline_lyrics_invalidate_cover_source(void) { calls[24]++; }
static void player_reset_widget_globals(void) { calls[25]++;cover_img=NULL;song_title_label=NULL;song_count_label=NULL; }
static void player_layouts_release(void) { calls[26]++; }
void player_volume_popup_teardown(void);

''' + production + '\n' + volume_teardown + r'''

static void reset_state(void) {
    memset(calls,0,sizeof(calls));
    progress_slider=(lv_obj_t *)0x401;seekbar_registered=true;
    stable_player=(lv_obj_t *)0x101; stable_popup=(lv_obj_t *)0x102;
    stable_menu=(lv_obj_t *)0x103; stable_backdrop=(lv_obj_t *)0x104;
    stable_timer=(lv_timer_t *)0x201;stable_hide_timer=(lv_timer_t *)0x202;
    stable_track_info_generation=1; metadata_requests=0;
    player_screen=(lv_obj_t *)0x100; song_title_label=(lv_obj_t *)0x301;
    song_count_label=(lv_obj_t *)0x302;cover_img=(lv_obj_t *)0x303;
    volume_popup=stable_popup;more_menu_popup=stable_menu;more_menu_popup_backdrop=stable_backdrop;
    player_artwork_cache_timer=stable_timer;volume_popup_hide_timer=stable_hide_timer;
    volume_popup_track=(lv_obj_t *)0x304;volume_popup_speaker_icon=(lv_obj_t *)0x305;
    volume_popup_value_label=(lv_obj_t *)0x306;volume_popup_hv=(volume_state_t){.drag_active=true,.pending=42};
    current_cover_bytes_present=true;current_cover_dsc_present=true;
    displayed_progress_percent=13;displayed_position_second=4;displayed_duration_second=5;
    deferred_resume_pending=1;playlist_index=0;playlist_count=1;metadata_ui_token=8;favorite_ui_revision=9;
    next_cover_prefetch_token=10;favorite_track_known=true;active_track=true;
    atomic_store(&cover_decode_generation,11);
}

int main(void) {
    reset_state();
    gui_player_teardown_internal(true);
    assert(player_screen==NULL);
    assert(!seekbar_registered&&calls[27]==1);
    assert(volume_popup==stable_popup&&more_menu_popup==stable_menu&&more_menu_popup_backdrop==stable_backdrop);
    assert(volume_popup_hide_timer==stable_hide_timer&&player_artwork_cache_timer==NULL);
    assert(stable_track_info_generation==1);
    assert(metadata_ui_token==8&&favorite_ui_revision==9&&next_cover_prefetch_token==10);
    assert(atomic_load(&cover_decode_generation)==11);
    assert(calls[11]==0&&calls[12]==0&&calls[13]==0&&calls[17]==0&&calls[18]==0&&calls[22]==0);

    gui_player_init_internal(480,800,true);
    assert(player_screen==stable_player&&last_refresh_registry==false);
    assert(volume_popup==stable_popup&&more_menu_popup==stable_menu&&volume_popup_hide_timer==stable_hide_timer);
    assert(stable_track_info_generation==1&&metadata_requests==0);
    assert(calls[5]==0&&calls[6]==0&&calls[7]==0&&calls[8]==0&&calls[9]==0&&calls[10]==0);
    assert(metadata_ui_token==8&&favorite_ui_revision==9&&next_cover_prefetch_token==10);

    /* A full teardown still disposes shared popup/timer/track-info resources
     * and invalidates pending playback work; full init recreates them and
     * submits exactly one current-track metadata read. */
    reset_state();
    gui_player_teardown_internal(false);
    assert(!seekbar_registered&&calls[27]==1);
    assert(volume_popup==NULL&&more_menu_popup==NULL&&more_menu_popup_backdrop==NULL);
    assert(volume_popup_hide_timer==NULL&&stable_track_info_generation==0);
    assert(metadata_ui_token==9&&favorite_ui_revision==10&&next_cover_prefetch_token==0);
    assert(atomic_load(&cover_decode_generation)==12);
    gui_player_init_internal(480,800,false);
    assert(player_screen==stable_player&&last_refresh_registry==true&&metadata_requests==1);
    assert(calls[5]==1&&calls[6]==1&&calls[7]==1&&calls[8]==1&&calls[9]==1&&calls[10]==1);
    puts("gui player layout reuse lifecycle regression passed");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="compas-player-layout-reuse-") as folder:
    folder = pathlib.Path(folder)
    c_file = folder / "test.c"
    binary = folder / "test"
    c_file.write_text(body)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    str(c_file), "-o", str(binary)], check=True, cwd=root)
    subprocess.run([str(binary)], check=True, cwd=root)
