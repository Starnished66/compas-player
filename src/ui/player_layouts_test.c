/* Headless tests for the Player layout registry and its XML loader, using the
 * real LVGL and the vendored XML engine. Run from the repository root: the
 * shipped example is read from assets/theme2/player_layouts. */
#include "player_layouts.h"
#include "board_config.h"
#include "fallback_font.h"
#include "assets.h"
#include "settings.h"
#include <assert.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

player_settings_t current_settings;
lv_font_t app_font_16, app_font_20, app_font_22, app_font_28, app_font_lyrics;
lv_font_t app_font_player_title, app_font_player_meta;

/* assets.c is not linked: resolve every asset to a stable file-style string. */
const char * asset_path(const char * relative_path) {
    static char paths[64][128];
    static unsigned next;
    char * slot = paths[next++ % 64];
    snprintf(slot, 128, "S:assets/theme2/%s", relative_path);
    return slot;
}
uint32_t gui_anim_ms(uint32_t base_ms) { return base_ms; } /* lv_conf.h's scroll animation hook */
const char * asset_stock_root(void) { return "assets/theme2/"; }
const char * asset_override_root(void) { return NULL; }

/* Every role of docs/PLAYER_LAYOUTS.md and the widget type it must have. */
typedef struct {
    const char * name;
    const lv_obj_class_t * type; /* NULL: any widget */
} role_t;

static const role_t roles[] = {
    { "cover_card", NULL }, { "cover_img", &lv_image_class }, { "title", &lv_label_class },
    { "play_btn", &lv_image_class }, { "progress_slider", &lv_slider_class },
    { "overlay_panel", NULL }, { "background_img", &lv_image_class }, { "artist", &lv_label_class },
    { "album", &lv_label_class }, { "favorite_circle", NULL }, { "favorite_icon", &lv_image_class },
    { "quality_pill", NULL }, { "format_badge", &lv_label_class }, { "pos_label", &lv_label_class },
    { "dur_label", &lv_label_class }, { "song_count", &lv_label_class }, { "order_btn", &lv_image_class },
    { "prev_btn", &lv_image_class }, { "next_btn", &lv_image_class }, { "more_btn", &lv_image_class },
    { "dismiss_btn", NULL }, { "volume_slider", &lv_slider_class },
};

#if LV_USE_LOG
static void log_cb(lv_log_level_t level, const char * buf) {
    (void) level;
    /* The stock image files are not in the repository; their absence is not news. */
    if (strstr(buf, ".png")) return;
    fputs(buf, stderr);
}
#endif

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

static lv_obj_t * builtin_stub(lv_obj_t * parent) {
    return parent;
}

static lv_obj_t * exported_stub(lv_obj_t * parent) {
    lv_obj_t * root = lv_obj_create(parent);
    lv_obj_set_name_static(root, "exported_root");
    return root;
}

static lv_anim_timeline_t * exported_timeline;
static lv_anim_timeline_t * exported_timeline_lookup(lv_obj_t * root, const char * name) {
    (void) root;
    return strcmp(name, "track_change") == 0 ? exported_timeline : NULL;
}

static void write_file(const char * path, const char * text) {
    FILE * f = fopen(path, "wb");
    assert(f);
    fputs(text, f);
    fclose(f);
}

static void check_example_layout(void) {
    int index = player_layouts_find("example_minimal");
    assert(index >= 0);
    assert(player_layouts_get(index)->kind == PLAYER_LAYOUT_XML_FILE);

    lv_obj_t * scr = lv_obj_create(NULL);
    lv_screen_load(scr);
    player_layout_kind_t kind;
    lv_obj_t * root = player_layouts_create("example_minimal", scr, &kind);
    assert(root && kind == PLAYER_LAYOUT_XML_FILE);
    assert(lv_obj_get_parent(root) == scr);

    for (size_t i = 0; i < sizeof(roles) / sizeof(roles[0]); ++i) {
        lv_obj_t * obj = lv_obj_find_by_name(scr, roles[i].name);
        if (!obj) { fprintf(stderr, "missing role %s\n", roles[i].name); abort(); }
        if (roles[i].type && !lv_obj_check_type(obj, roles[i].type)) {
            fprintf(stderr, "role %s has the wrong type\n", roles[i].name);
            abort();
        }
    }

    /* The flex arrangement has to fit the screen. */
    lv_obj_update_layout(scr);
    lv_obj_t * pill = lv_obj_find_by_name(scr, "quality_pill");
    lv_obj_t * transport = lv_obj_find_by_name(scr, "transport");
    assert(lv_obj_get_y(pill) > 0);
    lv_area_t area;
    lv_obj_get_coords(transport, &area);
    assert(area.y2 < BOARD_SCREEN_HEIGHT);
    lv_obj_get_coords(lv_obj_find_by_name(scr, "cover_card"), &area);
    assert(area.x1 >= 0 && area.x2 < BOARD_SCREEN_WIDTH);
    lv_obj_get_coords(lv_obj_find_by_name(scr, "progress_slider"), &area);
    assert(area.x1 >= 0 && area.x2 < BOARD_SCREEN_WIDTH);

    const char * timelines[] = { "lyrics_open", "lyrics_close", "track_change", "screen_enter" };
    lv_anim_timeline_t * found[4];
    for (int i = 0; i < 4; ++i) {
        found[i] = player_layouts_find_timeline(root, timelines[i]);
        if (!found[i]) { fprintf(stderr, "missing timeline %s\n", timelines[i]); abort(); }
    }
    assert(!player_layouts_find_timeline(root, "no_such_timeline"));

    /* Play both lyrics timelines; the cover card follows them. */
    lv_obj_t * card = lv_obj_find_by_name(scr, "cover_card");
    int32_t full = lv_obj_get_width(card);
    lv_anim_timeline_set_progress(found[0], 0);
    uint32_t play_ms = lv_anim_timeline_start(found[0]);
    assert(play_ms > 0);
    run_ms(play_ms + 100);
    assert(lv_obj_get_width(card) < full);
    lv_anim_timeline_set_progress(found[1], 0);
    play_ms = lv_anim_timeline_start(found[1]);
    run_ms(play_ms + 100);
    assert(lv_obj_get_width(card) == full);

    /* Tree first, then its component: the tree uses the component's styles. */
    lv_screen_load(lv_obj_create(NULL));
    lv_obj_delete(scr);
    player_layouts_release();
    player_layouts_release(); /* idempotent */
}

static void check_switching_and_failures(void) {
    char dir[] = "/tmp/player_layouts_test_XXXXXX";
    assert(mkdtemp(dir));
    char good[256], variant[256], bad[256], missing[256];
    snprintf(good, sizeof(good), "%s/variant_demo.xml", dir);
    snprintf(variant, sizeof(variant), "%s/variant_demo@%dx%d.xml", dir, BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT);
    snprintf(bad, sizeof(bad), "%s/bad.xml", dir);
    snprintf(missing, sizeof(missing), "%s/missing.xml", dir);
    write_file(good, "<component><view extends=\"lv_obj\" width=\"111\" height=\"50\"/></component>");
    write_file(variant, "<component><view extends=\"lv_obj\" width=\"222\" height=\"50\"/></component>");
    write_file(bad, "<component><view extends=\"lv_obj\" width=\"10\"><lv_label");

    lv_obj_t * scr = lv_obj_create(NULL);
    player_layout_kind_t kind;

    /* Board variant preferred over the plain file. */
    assert(player_layouts_session_select_xml("variant_demo", "Variant", good, false));
    assert(strcmp(player_layouts_effective_id(), "variant_demo") == 0);
    lv_obj_t * root = player_layouts_create("variant_demo", scr, &kind);
    assert(root && lv_obj_get_style_width(root, 0) == 222);
    lv_obj_clean(scr);
    player_layouts_release();

    /* Switching to another XML layout replaces the registration. */
    unlink(variant);
    root = player_layouts_create("variant_demo", scr, &kind);
    assert(root && lv_obj_get_style_width(root, 0) == 111);
    lv_obj_clean(scr);
    player_layouts_release();

    /* Malformed and missing files fail cleanly and leave the parent empty. */
    assert(player_layouts_session_select_xml("broken", "Broken", bad, false));
    assert(!player_layouts_create("broken", scr, &kind));
    assert(lv_obj_get_child_count(scr) == 0);
    assert(player_layouts_session_select_xml("gone", "Gone", missing, false));
    assert(!player_layouts_create("gone", scr, &kind));
    assert(lv_obj_get_child_count(scr) == 0);
    assert(!player_layouts_create("not_registered", scr, &kind));

    /* The component registered for a good layout survives a failed one. */
    root = player_layouts_create("variant_demo", scr, &kind);
    assert(root && lv_obj_get_style_width(root, 0) == 111);
    lv_obj_clean(scr);
    player_layouts_release();

    /* Layouts registered while a plugin loads, and their selection, go away on
     * a plugin reset. One registered from a callback survives it: no code
     * would register it again. */
    player_layouts_session_reset();
    assert(player_layouts_find("variant_demo") < 0);
    assert(strcmp(player_layouts_effective_id(), PLAYER_LAYOUT_ID_DEFAULT) == 0);
    assert(player_layouts_session_select_xml("from_load", "Load", good, false));
    assert(player_layouts_session_select_xml("from_callback", "Callback", good, true));
    player_layouts_session_reset();
    assert(player_layouts_find("from_load") < 0 && player_layouts_find("from_callback") >= 0);
    assert(strcmp(player_layouts_effective_id(), "from_callback") == 0);
    assert(player_layouts_session_clear_selection());
    assert(strcmp(player_layouts_effective_id(), PLAYER_LAYOUT_ID_DEFAULT) == 0);

    /* A layout registered while a plugin loads is only offered: it does not
     * change the effective id, the user's pick makes it effective, and a reload
     * (reset, then register again) keeps it so without duplicating the entry. */
    player_layouts_session_reset();
    assert(!strcmp(player_layouts_effective_id(), PLAYER_LAYOUT_ID_DEFAULT));
    int count_before = player_layouts_count();
    assert(player_layouts_session_register_xml("plugin.clean", "Clean", good, false));
    assert(player_layouts_count() == count_before + 1);
    assert(player_layouts_find("plugin.clean") >= 0);
    assert(!strcmp(player_layouts_effective_id(), PLAYER_LAYOUT_ID_DEFAULT));
    snprintf(current_settings.player_layout, sizeof(current_settings.player_layout), "plugin.clean");
    assert(!strcmp(player_layouts_effective_id(), "plugin.clean"));
    player_layouts_session_reset();
    assert(player_layouts_find("plugin.clean") < 0);
    assert(!strcmp(player_layouts_effective_id(), PLAYER_LAYOUT_ID_DEFAULT));
    assert(player_layouts_session_register_xml("plugin.clean", "Clean", good, false));
    assert(player_layouts_session_register_xml("plugin.clean", "Clean", good, false));
    assert(player_layouts_count() == count_before + 1);
    assert(!strcmp(player_layouts_effective_id(), "plugin.clean"));
    root = player_layouts_create("plugin.clean", scr, &kind);
    assert(root && lv_obj_get_style_width(root, 0) == 111);
    lv_obj_clean(scr);
    player_layouts_release();

    /* A callback selection overrides the saved choice for the session, and
     * clearing it brings the saved choice back. */
    assert(player_layouts_session_select_xml("from_callback2", "Callback", good, true));
    assert(!strcmp(player_layouts_effective_id(), "from_callback2"));
    assert(player_layouts_session_clear_selection());
    assert(!strcmp(player_layouts_effective_id(), "plugin.clean"));
    current_settings.player_layout[0] = '\0';
    player_layouts_session_reset();
    assert(player_layouts_find("plugin.clean") < 0);

    lv_obj_delete(scr);
    unlink(good); unlink(bad); unlink(variant);
    rmdir(dir);
}

/* Building and deleting layouts repeatedly, including broken ones, must not
 * grow the heap: a soft UI reload does exactly this. */
static size_t heap_in_use(void) {
    return mallinfo2().uordblks;
}

static void layout_cycle(const char * id, bool expect_ok) {
    lv_obj_t * scr = lv_obj_create(NULL);
    player_layout_kind_t kind;
    lv_obj_t * root = player_layouts_create(id, scr, &kind);
    assert((root != NULL) == expect_ok);
    if (root) {
        lv_obj_update_layout(scr);
        lv_anim_timeline_t * timeline = player_layouts_find_timeline(root, "lyrics_open");
        if (timeline) {
            lv_anim_timeline_set_progress(timeline, 0);
            run_ms(lv_anim_timeline_start(timeline) + 50);
        }
    }
    lv_obj_delete(scr);
    player_layouts_release();
}

static void check_no_heap_growth(void) {
    char dir[] = "/tmp/player_layouts_leak_XXXXXX";
    assert(mkdtemp(dir));
    char bad[256];
    snprintf(bad, sizeof(bad), "%s/bad.xml", dir);
    write_file(bad, "<component><styles><style name=\"s\" bg_opa=\"0\"/></styles>"
                    "<view extends=\"lv_obj\"><lv_label");
    assert(player_layouts_session_select_xml("leak_bad", "Bad", bad, false));

    /* LVGL's own lists and caches grow in steps early on, so warm up first and
     * allow for a little noise. A leaked component or scope is hundreds of
     * bytes per cycle. */
    for (int i = 0; i < 400; ++i) { layout_cycle("example_minimal", true); layout_cycle("leak_bad", false); }
    size_t before = heap_in_use();
    for (int i = 0; i < 400; ++i) { layout_cycle("example_minimal", true); layout_cycle("leak_bad", false); }
    size_t after = heap_in_use();
    if (after > before + 4096) {
        fprintf(stderr, "heap grew by %zu bytes over 400 layout cycles\n", after - before);
        abort();
    }
    player_layouts_session_reset();
    unlink(bad);
    rmdir(dir);
}

static void check_registry(void) {
    assert(player_layouts_count() >= 1);
    assert(strcmp(player_layouts_get(0)->id, PLAYER_LAYOUT_ID_DEFAULT) == 0);
    assert(player_layouts_get(0)->kind == PLAYER_LAYOUT_BUILTIN_C);

    assert(!player_layouts_register_c("default", "X", exported_stub));
    assert(!player_layouts_register_c("", "X", exported_stub));
    assert(!player_layouts_register_c("a/b", "X", exported_stub));
    assert(!player_layouts_register_c("ok_id", "X", NULL));
    assert(player_layouts_register_c_ex("exported_test", "Exported", exported_stub, exported_timeline_lookup));
    assert(player_layouts_register_c_ex("exported_test", "Exported again", exported_stub, exported_timeline_lookup));
    int index = player_layouts_find("exported_test");
    assert(index >= 0 && player_layouts_get(index)->kind == PLAYER_LAYOUT_EXPORTED_C);
    assert(strcmp(player_layouts_get(index)->name, "Exported again") == 0);

    /* A C id cannot be taken over by a plugin's XML registration. */
    assert(!player_layouts_session_select_xml("exported_test", "X", "/tmp/x.xml", false));

    lv_obj_t * scr = lv_obj_create(NULL);
    player_layout_kind_t kind;
    lv_obj_t * root = player_layouts_create("exported_test", scr, &kind);
    assert(root && kind == PLAYER_LAYOUT_EXPORTED_C);
    exported_timeline = (lv_anim_timeline_t *) 0x1;
    assert(player_layouts_find_timeline(root, "track_change") == exported_timeline);
    assert(!player_layouts_find_timeline(root, "lyrics_open"));
    root = player_layouts_create("default", scr, &kind);
    assert(root == scr && kind == PLAYER_LAYOUT_BUILTIN_C);
    lv_obj_delete(scr);

    /* Effective id: unknown settings value falls back to the default. */
    snprintf(current_settings.player_layout, sizeof(current_settings.player_layout), "deleted_file");
    assert(strcmp(player_layouts_effective_id(), PLAYER_LAYOUT_ID_DEFAULT) == 0);
    snprintf(current_settings.player_layout, sizeof(current_settings.player_layout), "example_minimal");
    assert(strcmp(player_layouts_effective_id(), "example_minimal") == 0);
    current_settings.player_layout[0] = '\0';
}

int main(void) {
    lv_init();
#if LV_USE_LOG
    lv_log_register_print_cb(log_cb);
#endif
    lv_display_t * display = lv_display_create(BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT);
    static uint8_t buffer[BOARD_SCREEN_WIDTH * 40 * 4];
    lv_display_set_buffers(display, buffer, NULL, sizeof(buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush_test);
    app_font_16 = app_font_20 = app_font_22 = app_font_28 = lv_font_montserrat_16;
    /* The Player's fixed title/meta sizes per board (fallback_font.c). */
#if BOARD_SCREEN_HEIGHT >= 800
    app_font_player_title = lv_font_montserrat_32; app_font_player_meta = lv_font_montserrat_22;
#elif BOARD_SCREEN_HEIGHT >= 720
    app_font_player_title = lv_font_montserrat_24; app_font_player_meta = lv_font_montserrat_20;
#else
    app_font_player_title = lv_font_montserrat_16; app_font_player_meta = lv_font_montserrat_16;
#endif

    player_layouts_init(builtin_stub);
    player_layouts_init(builtin_stub); /* idempotent: no duplicate entries */
    assert(player_layouts_find("default") == 0);

    check_registry();
    check_example_layout();
    check_switching_and_failures();
    check_no_heap_growth();
    printf("player layouts: passed\n");
    return 0;
}
