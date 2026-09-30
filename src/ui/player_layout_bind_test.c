/* Builds the real Player screen from each kind of layout and checks the
 * binder: role lookup, exported globals, hit targets, the lyrics view with
 * and without layout timelines, optional roles missing, and the fallback to
 * the built-in layout. Run from the repository root (the shipped example is
 * read from assets/theme2/player_layouts).
 *
 * Like player_lyrics_morph_test.c this includes gui_player.c itself.
 * Playback, network and database code is either discarded by section
 * garbage collection or, when only reachable through an event callback that
 * this test never fires, left unresolved at link time. */
#include "gui_player.c"
#include <assert.h>
#include <malloc.h>
#include <unistd.h>

player_settings_t current_settings;
lv_style_t style_theme_screen_bg, style_theme_text_primary, style_theme_text_muted, icon_press_style;
static lv_style_t accent_style, accent_knob_style, accent_outline_style;
lv_style_t * gui_theme_accent_style(void) { return &accent_style; }
lv_style_t * gui_theme_accent_knob_style(void) { return &accent_knob_style; }
lv_style_t * gui_theme_accent_outline_style(void) { return &accent_outline_style; }
lv_color_t accent_lv_color(void) { return lv_color_hex(0x2196f3); }
bool favorite_is_set;
player_layout_config_t player_layout_config;
lv_font_t app_font_16, app_font_20, app_font_22, app_font_28, app_font_lyrics;
lv_font_t app_font_player_title, app_font_player_meta;

const char * asset_path(const char * relative_path) {
    static char paths[64][128];
    static unsigned next;
    char * slot = paths[next++ % 64];
    snprintf(slot, 128, "S:assets/theme2/%s", relative_path);
    return slot;
}
const char * asset_stock_root(void) { return "assets/theme2/"; }
const char * asset_override_root(void) { return NULL; }
bool asset_decoded_image_open(asset_decoded_image_t * image, const char * path) { (void) image; (void) path; return false; }
void asset_decoded_image_close(asset_decoded_image_t * image) { (void) image; }
const void * asset_decoded_image_source(const asset_decoded_image_t * image) { (void) image; return NULL; }

bool audio_is_playing(void) { return false; }
float audio_get_volume(void) { return 0.5f; }
uint32_t gui_anim_ms(uint32_t base_ms) { return base_ms; }
void player_transition_mark_dirty(void) {}
void finalize_screen_navigation(lv_obj_t * screen) { lv_obj_add_flag(screen, LV_OBJ_FLAG_CLICKABLE); }
void register_swipe_dead_zone(lv_obj_t * obj) { (void) obj; }
void row_label_enable_marquee(lv_obj_t * label) { lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR); }
void gui_shell_update_quick_drawer_favorite(bool favorite) { (void) favorite; }
void gui_shell_update_quick_drawer_format(const char * text) { (void) text; }
void gui_shell_update_quick_drawer_play_state(bool playing) { (void) playing; }
lv_obj_t * build_header_back_button(lv_obj_t * screen, lv_event_cb_t cb) {
    lv_obj_t * button = lv_obj_create(screen);
    lv_obj_set_size(button, 64, 64);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_image_create(button);
    if (cb) lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, NULL);
    return button;
}

static int show_count, hide_count;
void gui_lyrics_prepare_layout(void) {}
void gui_lyrics_show_embedded(lv_obj_t * parent, int32_t top, int32_t height) {
    assert(parent == player_screen);
    assert(height > 0 && top + height <= BOARD_SCREEN_HEIGHT);
    ++show_count;
}
void gui_lyrics_hide_embedded(void) { ++hide_count; }

static void flush_test(lv_display_t * display, const lv_area_t * area, uint8_t * pixels) {
    (void) area; (void) pixels;
    lv_display_flush_ready(display);
}

static void run_ms(uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 10) {
        lv_tick_inc(10);
        lv_timer_handler();
    }
}

static void write_file(const char * path, const char * text) {
    FILE * f = fopen(path, "wb");
    assert(f);
    fputs(text, f);
    fclose(f);
}

static void build(void) {
    player_screen = build_player_screen(BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT);
    assert(player_screen);
    lv_screen_load(player_screen);
    lv_obj_update_layout(player_screen);
}

/* What teardown does to the tree and what hangs off it. */
static void discard(void) {
    static lv_obj_t * other_screen;
    player_lyrics_set_open(false, false);
    if (!other_screen) other_screen = lv_obj_create(NULL);
    lv_screen_load(other_screen);
    lv_obj_delete(player_screen);
    player_screen = NULL;
    player_reset_widget_globals();
    player_layouts_release();
}

static void select_layout(const char * id) {
    snprintf(current_settings.player_layout, sizeof(current_settings.player_layout), "%s", id);
}

static bool has_hit_target_over(lv_obj_t * icon) {
    lv_area_t icon_area, hit_area;
    lv_obj_get_coords(icon, &icon_area);
    for (unsigned i = 0; i < player_hit_target_count; ++i) {
        lv_obj_get_coords(player_hit_targets[i], &hit_area);
        int32_t cx = (icon_area.x1 + icon_area.x2) / 2, cy = (icon_area.y1 + icon_area.y2) / 2;
        if (cx >= hit_area.x1 && cx <= hit_area.x2 && cy >= hit_area.y1 && cy <= hit_area.y2 &&
            lv_area_get_width(&hit_area) >= 44 && lv_area_get_height(&hit_area) >= 44) return true;
    }
    return false;
}

static void check_all_roles_bound(void) {
    assert(player_overlay_panel && player_background_img && cover_card && cover_img && song_title_label);
    assert(artist_label && album_label && favorite_circle && favorite_icon && quality_pill && format_badge_label);
    assert(pos_label && dur_label && song_count_label && order_icon && prev_btn && play_btn && next_btn);
    assert(progress_slider && player_dismiss_btn && volume_slider);
    assert(lv_obj_check_type(play_btn, &lv_image_class));
    assert(lv_slider_get_max_value(progress_slider) == 100);
    assert(player_hit_target_count == 6); /* order, prev, play, next, more, favorite */
    assert(has_hit_target_over(order_icon) && has_hit_target_over(prev_btn) && has_hit_target_over(play_btn));
    assert(has_hit_target_over(next_btn) && has_hit_target_over(favorite_circle));
    assert(lv_obj_has_flag(cover_img, LV_OBJ_FLAG_CLICKABLE));
}

static void check_builtin(void) {
    select_layout("");
    build();
    assert(player_layout_builtin && !player_layout_root);
    check_all_roles_bound();

    /* The C morph works with the optional metadata labels gone. */
    lv_obj_t * artist = artist_label, * album = album_label;
    artist_label = album_label = NULL;
    int shows = show_count;
    current_settings.lyrics_enabled = true;
    player_lyrics_set_open(true, true);
    run_ms(600);
    assert(player_lyrics_open && !player_lyrics_animating && show_count == shows + 1);
    assert(lv_obj_has_flag(play_btn, LV_OBJ_FLAG_HIDDEN) || lv_obj_has_flag(lv_obj_get_parent(play_btn), LV_OBJ_FLAG_HIDDEN));
    player_lyrics_set_open(false, true);
    run_ms(600);
    assert(!player_lyrics_open && !lv_obj_has_flag(lv_obj_get_parent(play_btn), LV_OBJ_FLAG_HIDDEN));
    artist_label = artist;
    album_label = album;
    discard();
}

static void check_example_layout(void) {
    select_layout("example_minimal");
    build();
    assert(!player_layout_builtin && player_layout_root);
    check_all_roles_bound();
    assert(player_timeline_lyrics_open && player_timeline_lyrics_close);
    assert(player_timeline_track_change && player_timeline_screen_enter);
    assert(player_lyrics_control_count > 0);
    /* The transport row is part of the layout's own tree, not the screen's children. */
    assert(lv_obj_get_parent(play_btn) != player_screen);

    /* Layout timelines drive the lyrics view; the app hides the controls
     * once they have animated away and restores them on close. */
    lv_obj_t * card = cover_card;
    int32_t full = lv_obj_get_width(card);
    int shows = show_count, hides = hide_count;
    current_settings.lyrics_enabled = true;
    lv_obj_send_event(cover_img, LV_EVENT_CLICKED, NULL);
    assert(player_lyrics_animating);
    run_ms(800);
    assert(player_lyrics_open && !player_lyrics_animating && show_count == shows + 1);
    assert(lv_obj_get_width(card) < full);
    assert(lv_obj_has_flag(play_btn, LV_OBJ_FLAG_HIDDEN));
    for (unsigned i = 0; i < player_hit_target_count; ++i) assert(lv_obj_has_flag(player_hit_targets[i], LV_OBJ_FLAG_HIDDEN));
    lv_obj_send_event(cover_img, LV_EVENT_CLICKED, NULL);
    run_ms(800);
    assert(!player_lyrics_open && !player_lyrics_animating && hide_count == hides + 1);
    assert(lv_obj_get_width(card) == full);
    assert(!lv_obj_has_flag(play_btn, LV_OBJ_FLAG_HIDDEN));
    for (unsigned i = 0; i < player_hit_target_count; ++i) assert(!lv_obj_has_flag(player_hit_targets[i], LV_OBJ_FLAG_HIDDEN));

    /* Leaving the screen closes the lyrics without animation. */
    lv_obj_send_event(cover_img, LV_EVENT_CLICKED, NULL);
    run_ms(100);
    player_lyrics_set_open(false, false);
    assert(!player_lyrics_open && !player_lyrics_animating);
    assert(!lv_obj_has_flag(play_btn, LV_OBJ_FLAG_HIDDEN));

    /* Playing the other timelines does not disturb anything. */
    player_timeline_play(player_timeline_track_change);
    player_timeline_play(player_timeline_screen_enter);
    run_ms(500);
    discard();
}

static void check_minimal_layout_and_null_safety(const char * dir) {
    char path[256];
    snprintf(path, sizeof(path), "%s/bare.xml", dir);
    write_file(path,
        "<component><view extends=\"lv_obj\" width=\"100%\" height=\"100%\" style_bg_opa=\"0\">"
        "<lv_obj name=\"cover_card\" width=\"200\" height=\"200\"><lv_image name=\"cover_img\"/></lv_obj>"
        "<lv_label name=\"title\" y=\"210\"/>"
        "<lv_slider name=\"progress_slider\" y=\"240\" width=\"200\"/>"
        "<lv_image name=\"play_btn\" y=\"270\"/>"
        "</view></component>");
    assert(player_layouts_session_select_xml("bare", "Bare", path, false));
    build();
    assert(!player_layout_builtin);
    assert(cover_card && cover_img && song_title_label && progress_slider && play_btn);
    /* Optional roles are NULL; the ones the app assumes exist are created. */
    assert(!favorite_circle && !favorite_icon && !quality_pill);
    assert(!format_badge_label && !pos_label && !dur_label && !song_count_label && !order_icon);
    assert(!prev_btn && !next_btn && !player_dismiss_btn);
    assert(player_overlay_panel && player_background_img && volume_slider);
    /* The artist and album text lives in these labels, so a layout without
     * the roles still gets hidden ones that keep it. */
    assert(artist_label && album_label && player_artist_standin && player_album_standin);
    assert(lv_obj_has_flag(artist_label, LV_OBJ_FLAG_HIDDEN) && lv_obj_has_flag(album_label, LV_OBJ_FLAG_HIDDEN));
    lv_label_set_text(artist_label, "An Artist");
    lv_label_set_text(album_label, "An Album");
    assert(strcmp(gui_player_get_now_playing_folder(), "An Artist") == 0);
    assert(strcmp(gui_player_get_now_playing_album(), "An Album") == 0);
    assert(lv_label_get_long_mode(artist_label) != LV_LABEL_LONG_SCROLL_CIRCULAR); /* no marquee set up */
    assert(lv_obj_get_parent(player_background_img) == player_overlay_panel);
    assert(lv_obj_get_index(player_overlay_panel) == 0);
    assert(player_hit_target_count == 1); /* play_btn only */
    assert(player_timeline_lyrics_open == NULL && player_timeline_track_change == NULL);

    /* Update paths that run on every track and every poll. */
    refresh_format_badge();
    show_favorite_state(true);
    set_play_button_state(true);
    set_play_button_state(false);
    gui_player_refresh_frosted_background();
    gui_player_sync_topbar_visibility(player_screen);
    gui_player_refresh_font_geometry();
    fit_cover_img_to_card();
    assert(gui_player_get_volume_percent() == 50);

    /* Lyrics: the C morph with no artist, album or control roles. */
    current_settings.lyrics_enabled = true;
    int shows = show_count;
    player_lyrics_set_open(true, true);
    run_ms(600);
    assert(player_lyrics_open && show_count == shows + 1);
    assert(player_lyrics_object_count == 2); /* cover and title; the stand-ins do not move */
    assert(lv_obj_has_flag(artist_label, LV_OBJ_FLAG_HIDDEN));
    player_lyrics_set_open(false, true);
    run_ms(600);
    assert(!player_lyrics_open);
    assert(lv_obj_has_flag(artist_label, LV_OBJ_FLAG_HIDDEN));
    discard();
    player_layouts_session_reset();
    unlink(path);
}

static void check_fallback(const char * dir) {
    char missing_role[256], broken[256];
    snprintf(missing_role, sizeof(missing_role), "%s/no_play.xml", dir);
    snprintf(broken, sizeof(broken), "%s/broken.xml", dir);
    /* play_btn is required, and must be an image. */
    write_file(missing_role,
        "<component><view extends=\"lv_obj\">"
        "<lv_obj name=\"cover_card\"><lv_image name=\"cover_img\"/></lv_obj>"
        "<lv_label name=\"title\"/><lv_slider name=\"progress_slider\"/>"
        "<lv_label name=\"play_btn\"/></view></component>");
    write_file(broken, "<component><view extends=\"lv_obj\"><lv_label");
    assert(player_layouts_session_select_xml("no_play", "No play", missing_role, false));
    build();
    assert(player_layout_builtin); /* fell back, with a complete screen */
    check_all_roles_bound();
    assert(lv_obj_get_parent(lv_obj_find_by_name(player_screen, "play_btn")) != player_screen);
    discard();

    assert(player_layouts_session_select_xml("broken", "Broken", broken, false));
    build();
    assert(player_layout_builtin);
    check_all_roles_bound();
    discard();
    player_layouts_session_reset();
    unlink(missing_role);
    unlink(broken);
}

/* A soft UI reload builds and deletes the player screen. Nothing the binder
 * allocates (transport contexts, XML components) may outlive it. LVGL's own
 * lists grow in steps early on, hence the warm-up and the margin. */
static void check_no_heap_growth(void) {
    const char * ids[] = { "", "example_minimal" };
    for (int round = 0; round < 2; ++round) {
        /* LVGL's own caches settle within the first builds. */
        for (int i = 0; i < 400; ++i) { select_layout(ids[round]); build(); discard(); }
        size_t before = mallinfo2().uordblks;
        for (int i = 0; i < 400; ++i) { select_layout(ids[round]); build(); discard(); }
        size_t after = mallinfo2().uordblks;
        if (after > before + 4096) {
            fprintf(stderr, "layout '%s': heap grew by %zu bytes over 400 screen builds\n", ids[round], after - before);
            abort();
        }
    }
}

int main(void) {
    lv_init();
    lv_display_t * display = lv_display_create(BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT);
    static uint8_t buffer[BOARD_SCREEN_WIDTH * 40 * 4];
    lv_display_set_buffers(display, buffer, NULL, sizeof(buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush_test);
    lv_style_init(&style_theme_screen_bg);
    lv_style_set_bg_color(&style_theme_screen_bg, lv_color_hex(0x101010));
    lv_style_init(&style_theme_text_primary);
    lv_style_set_text_color(&style_theme_text_primary, lv_color_white());
    lv_style_init(&style_theme_text_muted);
    lv_style_set_text_color(&style_theme_text_muted, lv_color_hex(0x888888));
    lv_style_init(&icon_press_style);
    lv_style_set_opa(&icon_press_style, LV_OPA_60);
    lv_style_init(&accent_style);
    lv_style_set_bg_color(&accent_style, lv_color_hex(0x2196f3));
    lv_style_init(&accent_knob_style);
    lv_style_init(&accent_outline_style);
    app_font_16 = app_font_20 = app_font_22 = app_font_28 = lv_font_montserrat_16;
    app_font_player_title = app_font_player_meta = lv_font_montserrat_20;

    char dir[] = "/tmp/player_bind_test_XXXXXX";
    assert(mkdtemp(dir));
    check_builtin();
    check_example_layout();
    check_minimal_layout_and_null_safety(dir);
    check_fallback(dir);
    check_no_heap_growth();
    rmdir(dir);
    printf("Player layout binder: builtin, XML, optional roles, fallback and reload passed\n");
    return 0;
}
