/* Additional lifecycle characterization around the real Player binder. The
 * runner can point this include at the frozen Batch04 fixture; the fixture
 * includes its neighboring gui_player.c, so these assertions inspect the
 * production implementation without copying its transition code. */
#ifndef PLAYER_INLINE_LYRICS_BIND_TEST_SOURCE
#define PLAYER_INLINE_LYRICS_BIND_TEST_SOURCE "player_layout_bind_test.c"
#endif
#define main player_layout_bind_test_main
#include PLAYER_INLINE_LYRICS_BIND_TEST_SOURCE
#undef main

bool gui_display_refresh_allowed(void) { return true; }

static void check_cancel_before_player_bind(void) {
#ifdef PLAYER_INLINE_LYRICS_BASELINE_MODE
    player_lyrics_timeline_cancel_timer();
#else
    player_inline_lyrics_cancel_timer();
#endif
}

static void write_snapshot_layout(const char * path) {
    write_file(path,
        "<component><animations>"
        "<timeline name=\"lyrics_open\"><animation prop=\"width\" target=\"cover_card\" start=\"120\" end=\"100\" duration=\"200\"/></timeline>"
        "<timeline name=\"lyrics_close\"><animation prop=\"width\" target=\"cover_card\" start=\"100\" end=\"120\" duration=\"200\"/></timeline>"
        "</animations><view extends=\"lv_obj\" width=\"100%\" height=\"100%\" scrollable=\"false\">"
        "<lv_obj name=\"cover_card\" x=\"20\" y=\"80\" width=\"120\" height=\"120\" style_pad_all=\"0\" style_border_width=\"0\" style_outline_width=\"0\" style_shadow_width=\"0\" style_radius=\"0\">"
        "<lv_image name=\"cover_img\"/></lv_obj>"
        "<lv_label name=\"title\" x=\"20\" y=\"220\" width=\"280\"/>"
        "<lv_slider name=\"progress_slider\" x=\"20\" y=\"300\" width=\"280\"/>"
        "<lv_image name=\"play_btn\" x=\"20\" y=\"350\" width=\"60\" height=\"60\"/>"
        "</view></component>");
}

static void install_cover(void) {
    free(current_cover_bytes);
    current_cover_bytes = malloc(120U * 120U * 2U);
    assert(current_cover_bytes);
    memset(current_cover_bytes, 0x5a, 120U * 120U * 2U);
    memset(&current_cover_dsc, 0, sizeof(current_cover_dsc));
    current_cover_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    current_cover_dsc.header.w = 120;
    current_cover_dsc.header.h = 120;
    current_cover_dsc.header.stride = 240;
    current_cover_dsc.data_size = 120U * 120U * 2U;
    current_cover_dsc.data = current_cover_bytes;
    ++current_cover_generation;
    lv_image_set_src(cover_img, &current_cover_dsc);
}

static void check_interruption_and_source_replacement(const char * dir) {
    char path[256];
    snprintf(path, sizeof(path), "%s/inline_lifecycle.xml", dir);
    write_snapshot_layout(path);
    assert(player_layouts_session_select_xml("inline_lifecycle", "Inline lifecycle", path, false));
    current_settings.lyrics_enabled = true;
    build();
    install_cover();
    player_lyrics_set_open(true, true);
    assert(player_lyrics_animating && player_lyrics_timeline_timer);
    assert(player_timeline_cover_frame);
    /* Immediate teardown cancels the deferred start timer and restores its
     * temporary source before returning. */
    player_lyrics_set_open(false, false);
    assert(!player_lyrics_animating && !player_lyrics_timeline_timer);
    assert(!player_timeline_cover_frame && !player_timeline_cover_source);
    assert(lv_image_get_src(cover_img) == &current_cover_dsc);

    /* Exercise the same pre-free invalidation seam used by cover replacement
     * while a transition frame is installed. */
    player_lyrics_set_open(true, true);
    assert(player_lyrics_animating && player_timeline_cover_frame);
    player_timeline_cover_cache_invalidate();
    assert(!player_timeline_cover_frame && !player_timeline_cover_cache);
    assert(lv_image_get_src(cover_img) == &current_cover_dsc);
    uint8_t * old_cover = current_cover_bytes;
    free(old_cover);
    current_cover_bytes = NULL;
    install_cover();
    assert(current_cover_bytes);
    assert(current_cover_dsc.data == current_cover_bytes);
    assert(lv_image_get_src(cover_img) == &current_cover_dsc);

    /* Immediate close cancels the replacement timer and leaves no active
     * frame/cache allocation behind. */
    player_lyrics_set_open(false, false);
    assert(!player_lyrics_animating && !player_lyrics_timeline_timer);
    assert(!player_timeline_cover_frame && !player_timeline_cover_cache);
    discard();
    assert(!player_timeline_cover_frame && !player_timeline_cover_cache);
    free(current_cover_bytes);
    current_cover_bytes = NULL;
    current_cover_dsc.data = NULL;
    player_layouts_session_reset();
    unlink(path);
}

static void check_display_teardown_observer(const char * dir) {
    char path[256];
    snprintf(path, sizeof(path), "%s/display_lifecycle.xml", dir);
    write_snapshot_layout(path);
    assert(player_layouts_session_select_xml("display_lifecycle", "Display lifecycle", path, false));
    build();
    install_cover();
    lv_display_t * display = lv_obj_get_display(player_screen);
    assert(display && player_timeline_cover_cache_display == display);
    player_lyrics_set_open(true, true);
    assert(player_lyrics_timeline_timer);
    discard();
    assert(!player_lyrics_timeline_timer && !player_timeline_cover_frame && !player_timeline_cover_cache);
    free(current_cover_bytes);
    current_cover_bytes = NULL;
    current_cover_dsc.data = NULL;
    player_layouts_session_reset();
    unlink(path);

    /* The display callback is still registered after screen teardown and
     * owns the final observer-pointer cleanup. This must be the final LVGL
     * operation in this process. */
    lv_display_delete(display);
    assert(!player_timeline_cover_cache_display);
}

int main(int argc, char ** argv) {
    check_cancel_before_player_bind();
    int result = player_layout_bind_test_main(argc, argv);
    assert(result == 0);
    char dir[] = "build/player_inline_lyrics_lifecycle/fixture_XXXXXX";
    assert(mkdtemp(dir));
    check_interruption_and_source_replacement(dir);
    check_display_teardown_observer(dir);
    rmdir(dir);
    puts("Player inline lyrics interruption, cover replacement and display teardown: passed");
    return 0;
}
