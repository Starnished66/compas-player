#!/usr/bin/env python3
"""Compile the production Player rebuild functions against a small LVGL mock."""
import pathlib
import re
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[5]
source = (root / "src/ui/player/screen/gui_player.c").read_text()


def function(name):
    match = re.search(r"^(?:static )?(?:int|void|bool) " + name +
                      r"\([^;]*?\) \{.*?^\}", source, re.M | re.S)
    if not match:
        raise RuntimeError("missing production function " + name)
    return match.group()


production = "\n".join(function(name) for name in (
    "player_layout_rebuild_ready", "gui_player_rebuild_layout"))

body = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct lv_obj_t { int id; unsigned flags; int value; const char *text; } lv_obj_t;
typedef struct lv_draw_buf_t { int id; } lv_draw_buf_t;
typedef struct lv_display_t { lv_obj_t *active; } lv_display_t;
typedef struct lv_indev_t { int state; struct lv_indev_t *next; } lv_indev_t;
typedef struct lv_timer_t { int unused; } lv_timer_t;
typedef int lv_color_t;
typedef struct { bool visible; } gui_popup_t;
#define LV_OBJ_FLAG_HIDDEN 1u
#define LV_OBJ_FLAG_SCROLLABLE 2u
#define LV_INDEV_STATE_PRESSED 1
#define LV_COLOR_FORMAT_RGB565 1
#define LV_OPA_COVER 255
#define LV_IMAGE_ALIGN_STRETCH 1
#define LV_ALIGN_CENTER 1
#define LV_ASSERT_NULL(x) assert((x) != NULL)

enum { E_SNAPSHOT = 1, E_CREATE_SCREEN, E_CREATE_IMAGE, E_LOAD_FREEZE, E_REFRESH,
       E_TEARDOWN, E_INIT, E_PLAY, E_FAVORITE, E_SLIDER, E_POSITION, E_DURATION,
       E_PROGRESS, E_FAVORITE_ASYNC, E_LYRICS, E_NAV, E_DELETE_FREEZE,
       E_CACHE_DROP, E_FRAME_DESTROY, E_TOPBAR };
static int events[128], event_count;
static void event(int e) { events[event_count++] = e; }
static lv_obj_t old_player = { .id = 1 }, rebuilt_player = { .id = 2 };
static lv_obj_t external_screen = { .id = 3 };
static lv_obj_t frozen_screen = { .id = 4 };
static lv_obj_t frozen_image = { .id = 5 };
static lv_obj_t slider_widget = { .id = 6, .value = 37 };
static lv_obj_t position_widget = { .id = 7, .text = "1:23" };
static lv_obj_t duration_widget = { .id = 8, .text = "4:56" };
static char position_text[32] = "1:23";
static char duration_text[32] = "4:56";
static lv_draw_buf_t frame = { .id = 9 };
static lv_display_t display;
static lv_indev_t indev;
static lv_obj_t *player_screen;
static lv_obj_t *progress_slider = &slider_widget;
static lv_obj_t *pos_label = &position_widget;
static lv_obj_t *dur_label = &duration_widget;
static lv_obj_t *loaded_screen;
static lv_obj_t *player_nav_entry;
static bool active, snapshot_ok, screen_alloc_ok, image_alloc_ok;
static bool cover_decode_active, cover_decode_pending_valid;
static atomic_bool cover_reload_active;
static bool player_artwork_jobs_active(void) { return cover_decode_active; }
static bool player_artwork_jobs_pending(void) { return cover_decode_pending_valid; }
static bool player_artwork_jobs_reload_active(void) { return atomic_load(&cover_reload_active); }
static bool queue_write_busy, queue_save_busy, nav_transition, lyrics_animating;
static bool user_seeking, progress_awaiting_seek;
static lv_timer_t *pending_progress_seek_timer;
static bool player_seek_input_progress_busy(void) {
    return user_seeking || progress_awaiting_seek || pending_progress_seek_timer != NULL;
}
static lv_obj_t *volume_popup, *more_menu_popup, *more_menu_popup_backdrop;
static bool player_lyrics_open, deferred_resume_pending, favorite_is_set, play_state;
static int displayed_progress_percent;

static lv_display_t *lv_obj_get_display(lv_obj_t *obj) { (void)obj; return &display; }
static lv_obj_t *lv_display_get_screen_active(lv_display_t *d) { return d->active; }
static lv_indev_t *lv_indev_get_next(lv_indev_t *i) { return i ? i->next : (indev.state ? &indev : NULL); }
static int lv_indev_get_state(lv_indev_t *i) { return i->state; }
static bool gui_player_queue_write_busy(void) { return queue_write_busy; }
static bool player_queue_storage_save_active(void) { return queue_save_busy; }
static bool gui_navigation_transition_in_progress(void) { return nav_transition; }
static bool gui_player_lyrics_animation_in_progress(void) { return lyrics_animating; }
static bool gui_popup_is_visible(gui_popup_t *p) { return p->visible; }
static gui_popup_t delete_song_popup;
static bool lv_obj_has_flag(lv_obj_t *o, unsigned f) { return (o->flags & f) != 0; }
static lv_draw_buf_t *lv_snapshot_take(lv_obj_t *o, int format) {
    assert(o == &old_player && format == LV_COLOR_FORMAT_RGB565); event(E_SNAPSHOT);
    return snapshot_ok ? &frame : NULL;
}
static lv_obj_t *lv_obj_create(lv_obj_t *parent) {
    assert(parent == NULL); event(E_CREATE_SCREEN); return screen_alloc_ok ? &frozen_screen : NULL;
}
static lv_obj_t *lv_image_create(lv_obj_t *parent) {
    assert(parent == &frozen_screen); event(E_CREATE_IMAGE); return image_alloc_ok ? &frozen_image : NULL;
}
static void lv_obj_remove_style_all(lv_obj_t *o) { assert(o == &frozen_screen); }
static void lv_obj_set_style_bg_opa(lv_obj_t *o, int a, int s) { (void)o;(void)a;(void)s; }
static lv_color_t lv_color_black(void) { return 0; }
static void lv_obj_set_style_bg_color(lv_obj_t *o, lv_color_t c, int s) { (void)o;(void)c;(void)s; }
static void lv_obj_set_style_border_width(lv_obj_t *o, int w, int s) { (void)o;(void)w;(void)s; }
static void lv_obj_set_style_pad_all(lv_obj_t *o, int p, int s) { (void)o;(void)p;(void)s; }
static void lv_obj_remove_flag(lv_obj_t *o, unsigned f) { o->flags &= ~f; }
static void lv_obj_set_size(lv_obj_t *o, int w, int h) { (void)o;assert(w==480&&h==800); }
static void lv_image_set_src(lv_obj_t *o, lv_draw_buf_t *b) { assert(o==&frozen_image&&b==&frame); }
static void lv_image_set_inner_align(lv_obj_t *o, int a) { (void)o;(void)a; }
static void lv_obj_align(lv_obj_t *o, int a, int x, int y) { (void)o;(void)a;(void)x;(void)y; }
static void lv_screen_load(lv_obj_t *o) { player_lyrics_open=false;loaded_screen=o;display.active=o;event(E_LOAD_FREEZE); }
static void lv_obj_delete(lv_obj_t *o) { assert(o==&frozen_screen);event(E_DELETE_FREEZE); }
static void lv_image_cache_drop(const void *o) { assert(o==&frame);event(E_CACHE_DROP); }
static void lv_draw_buf_destroy(lv_draw_buf_t *b) { assert(b==&frame);event(E_FRAME_DESTROY); }
static int player_seekbar_get_value(lv_obj_t *o) { return o->value; }
static void player_seekbar_set_value(lv_obj_t *o, int v) { o->value=v;event(E_SLIDER); }
static const char *lv_label_get_text(lv_obj_t *o) { return o->text; }
static void lv_label_set_text(lv_obj_t *o, const char *s) {
    char *copy=NULL;
    if(o==pos_label) { event(E_POSITION); copy=position_text; }
    else if(o==dur_label) { event(E_DURATION); copy=duration_text; }
    if(copy) { snprintf(copy,32,"%s",s); o->text=copy; }
}
static bool audio_is_playing(void) { return true; }
static void set_play_button_state(bool b) { play_state=b;event(E_PLAY); }
static void show_favorite_state(bool b) { favorite_is_set=b;event(E_FAVORITE); }
static void gui_player_update_progress(void) { event(E_PROGRESS); }
static bool player_inline_lyrics_is_open(void) { return player_lyrics_open; }
static void player_inline_lyrics_set_open(bool b, bool animate) {
    assert(!animate);player_lyrics_open=b;event(E_LYRICS);
}
static void gui_player_teardown_internal(bool preserve) {
    assert(preserve);player_screen=NULL;event(E_TEARDOWN);
}
static void gui_player_init_internal(uint32_t w,uint32_t h,bool reuse) {
    assert(reuse);
    assert(w==480&&h==800);player_screen=&rebuilt_player;event(E_INIT);
}
static lv_obj_t *lv_screen_active(void) { return display.active; }
static void sync_player_topbar_visibility(lv_obj_t *screen) { assert(screen==display.active);event(E_TOPBAR); }
static void gui_navigation_replace_player_screen(lv_obj_t *old,lv_obj_t *fresh,bool was_active) {
    assert(old==&old_player&&fresh==&rebuilt_player);
    if(player_nav_entry==old) player_nav_entry=fresh;
    if(was_active) { display.active=fresh;loaded_screen=fresh; }
    event(E_NAV);
}
''' + production + r'''

static void reset(bool player_visible) {
    event_count=0;active=player_visible;display.active=player_visible?&old_player:&external_screen;
    player_screen=&old_player;player_nav_entry=&old_player;loaded_screen=NULL;
    snapshot_ok=screen_alloc_ok=image_alloc_ok=true;
    cover_decode_active=cover_decode_pending_valid=cover_reload_active=false;
    queue_write_busy=queue_save_busy=nav_transition=lyrics_animating=false;
    delete_song_popup.visible=false;user_seeking=progress_awaiting_seek=false;
    volume_popup=more_menu_popup=more_menu_popup_backdrop=NULL;
    pending_progress_seek_timer=NULL;indev.state=0;indev.next=NULL;
    player_lyrics_open=true;deferred_resume_pending=true;favorite_is_set=true;play_state=false;
    displayed_progress_percent=-1;slider_widget.value=37;
    snprintf(position_text,sizeof(position_text),"1:23");
    snprintf(duration_text,sizeof(duration_text),"4:56");
    position_widget.text=position_text;duration_widget.text=duration_text;
}

static int find_event(int wanted) {
    for(int i=0;i<event_count;i++) if(events[i]==wanted) return i;
    return -1;
}

int main(void) {
    reset(true);
    assert(gui_player_rebuild_layout(480,800));
    assert(loaded_screen==&rebuilt_player&&display.active==&rebuilt_player);
    assert(player_nav_entry==&rebuilt_player&&play_state&&favorite_is_set&&player_lyrics_open);
    assert(slider_widget.value==37&&displayed_progress_percent==37);
    assert(strcmp(position_widget.text,"1:23")==0&&strcmp(duration_widget.text,"4:56")==0);
    assert(find_event(E_LOAD_FREEZE)<find_event(E_TEARDOWN));
    assert(find_event(E_FAVORITE_ASYNC)==-1);
    assert(find_event(E_NAV)<find_event(E_TOPBAR));
    assert(find_event(E_PLAY)<find_event(E_NAV)&&find_event(E_FAVORITE)<find_event(E_NAV));
    assert(find_event(E_SLIDER)<find_event(E_NAV)&&find_event(E_POSITION)<find_event(E_NAV));
    assert(find_event(E_DURATION)<find_event(E_NAV)&&find_event(E_LYRICS)<find_event(E_NAV));
    assert(find_event(E_DELETE_FREEZE)<find_event(E_CACHE_DROP));
    assert(find_event(E_CACHE_DROP)<find_event(E_FRAME_DESTROY));

    /* A covered Player replaces its stack entry without touching the active
     * external screen, and unrelated input there does not block the rebuild. */
    reset(false);indev.state=LV_INDEV_STATE_PRESSED;
    assert(gui_player_rebuild_layout(480,800));
    assert(display.active==&external_screen&&loaded_screen==NULL);
    assert(player_nav_entry==&rebuilt_player&&find_event(E_SNAPSHOT)<0);

    reset(true);cover_decode_active=true;
    assert(!gui_player_rebuild_layout(480,800));
    assert(player_screen==&old_player&&event_count==0);
    reset(true);indev.state=LV_INDEV_STATE_PRESSED;
    assert(!gui_player_rebuild_layout(480,800));
    assert(player_screen==&old_player&&event_count==0);

    reset(true);snapshot_ok=false;
    assert(!gui_player_rebuild_layout(480,800));
    assert(player_screen==&old_player&&display.active==&old_player&&find_event(E_TEARDOWN)<0);
    reset(true);screen_alloc_ok=false;
    assert(!gui_player_rebuild_layout(480,800));
    assert(player_screen==&old_player&&find_event(E_FRAME_DESTROY)>=0&&find_event(E_TEARDOWN)<0);
    reset(true);image_alloc_ok=false;
    assert(!gui_player_rebuild_layout(480,800));
    assert(player_screen==&old_player&&find_event(E_DELETE_FREEZE)>=0&&find_event(E_FRAME_DESTROY)>=0);

    puts("gui player layout rebuild regression passed");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="compas-player-layout-rebuild-") as folder:
    folder = pathlib.Path(folder)
    c_file = folder / "test.c"
    binary = folder / "test"
    c_file.write_text(body)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    str(c_file), "-o", str(binary)], check=True, cwd=root)
    subprocess.run([str(binary)], check=True, cwd=root)
