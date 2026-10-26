/* Headless tests for the Player layout registry and its XML loader, using the
 * real LVGL and the vendored XML engine. */
#include "../../src/ui/player/layout/player_layouts.h"
#include "../../src/core/system/runtime/board_config.h"
#include "../../src/ui/text/rendering/fallback_font.h"
#include "../../src/ui/text/fonts/ui_fonts.h"
#include "../../src/ui/assets/assets.h"
#include "../../src/core/persistence/settings.h"
#include "third_party/lv_xml/src/others/xml/lv_xml.h"
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <malloc.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
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

/* Every role of docs/reference/PLAYER_LAYOUTS.md and the widget type it must have. */
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
    { "lyrics_area", NULL }, { "lyrics_active", &lv_label_class }, { "lyrics_toggle", NULL },
};

#if LV_USE_LOG
static void log_cb(lv_log_level_t level, const char * buf) {
    (void) level;
    /* The stock image files are not in the repository; their absence is not news. */
    if (strstr(buf, ".png") || strstr(buf, "No image was found")) return;
    fputs(buf, stderr);
}
#endif

static unsigned matrix_named_widgets, matrix_clickable_widgets, matrix_flushes;
static unsigned matrix_clickable_bounds_issues;
static void flush_test(lv_display_t * display, const lv_area_t * area, uint8_t * pixels) {
    (void) area; (void) pixels;
    matrix_flushes++;
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

static lv_display_t * test_display;

static bool area_inside(lv_area_t area, int32_t width, int32_t height) {
    return area.x1 >= 0 && area.y1 >= 0 && area.x2 < width && area.y2 < height;
}

static bool areas_overlap(lv_area_t a, lv_area_t b) {
    return a.x1 <= b.x2 && a.x2 >= b.x1 && a.y1 <= b.y2 && a.y2 >= b.y1;
}

static void assert_bg_color(lv_obj_t * obj, uint32_t expected) {
    assert(obj);
    assert(lv_color_eq(lv_obj_get_style_bg_color(obj, LV_PART_MAIN), lv_color_hex(expected)));
}

static void assert_border_color(lv_obj_t * obj, uint32_t expected) {
    assert(obj);
    assert(lv_color_eq(lv_obj_get_style_border_color(obj, LV_PART_MAIN), lv_color_hex(expected)));
}

static void assert_text_color(lv_obj_t * obj, uint32_t expected) {
    assert(obj);
    assert(lv_color_eq(lv_obj_get_style_text_color(obj, LV_PART_MAIN), lv_color_hex(expected)));
}

/* These assert the XML color attributes survive the real LVGL XML loader.
 * A leading `#` is reserved for constant substitution, so layouts use 0x colors. */
static void check_editorial_color_roles(const char * id, lv_obj_t * scr) {
    lv_obj_t * progress = lv_obj_find_by_name(scr, "progress_slider");
    if (strcmp(id, "panorama") == 0) {
        lv_obj_t * fade = lv_obj_find_by_name(scr, "cover_fade");
        lv_obj_t * cover = lv_obj_find_by_name(scr, "cover_card");
        assert(fade && lv_obj_check_type(fade, &lv_image_class));
        assert(!lv_obj_has_flag(fade, LV_OBJ_FLAG_CLICKABLE));
        lv_area_t fade_area, cover_area;
        lv_obj_get_coords(fade, &fade_area);
        lv_obj_get_coords(cover, &cover_area);
        assert(fade_area.x1 == cover_area.x1 && fade_area.y1 == cover_area.y1 &&
               fade_area.x2 == cover_area.x2 && fade_area.y2 == cover_area.y2);
        /* Panorama inherits these colors from the active UI theme; its XML
         * only declares lyrics_area transparency. Explicit XML colors remain
         * covered by check_example_layout's owned fixture. */
        assert(lv_obj_get_style_bg_opa(lv_obj_find_by_name(scr, "lyrics_area"), LV_PART_MAIN) == LV_OPA_TRANSP);
        assert(progress && lv_obj_check_type(progress, &lv_slider_class));
    }
    else if (strcmp(id, "vinyl") == 0) {
        lv_obj_t * record = lv_obj_find_by_name(scr, "record_outer");
        assert(lv_obj_get_style_bg_opa(record, 0) == LV_OPA_TRANSP);
        assert_border_color(record, 0x39434a);
        assert_border_color(lv_obj_find_by_name(scr, "record_ring_outer"), 0x252e34);
        assert_border_color(lv_obj_find_by_name(scr, "record_ring_inner"), 0x303a40);
        assert(!lv_obj_find_by_name(scr, "lyrics_area"));
        assert_bg_color(progress, 0x596269);
        assert_text_color(lv_obj_find_by_name(scr, "title"), 0xf2f2f2);
    }
}

static void write_file(const char * path, const char * text) {
    FILE * f = fopen(path, "wb");
    assert(f);
    fputs(text, f);
    fclose(f);
}

static char fixture_layout_dir[PATH_MAX];
static char fixture_layout_path[PATH_MAX];
static void create_layout_fixture(void) {
    const char *base = getenv("TMPDIR");
    if (!base || !base[0]) base = "/tmp";
    char template[PATH_MAX];
    assert(snprintf(template, sizeof(template), "%s/player-layout-fixture-XXXXXX", base) > 0);
    char *dir = mkdtemp(template);
    assert(dir);
    snprintf(fixture_layout_dir, sizeof(fixture_layout_dir), "%s", dir);
    snprintf(fixture_layout_path, sizeof(fixture_layout_path), "%s/example_minimal.xml", dir);
    write_file(fixture_layout_path,
        "<component><animations>"
        "<timeline name=\"lyrics_open\"><animation prop=\"width\" target=\"cover_card\" start=\"120\" end=\"60\" duration=\"100\"/></timeline>"
        "<timeline name=\"lyrics_close\"><animation prop=\"width\" target=\"cover_card\" start=\"60\" end=\"120\" duration=\"100\"/></timeline>"
        "<timeline name=\"track_change\"><animation prop=\"opa\" target=\"title\" start=\"255\" end=\"0\" duration=\"100\"/></timeline>"
        "<timeline name=\"screen_enter\"><animation prop=\"opa\" target=\"title\" start=\"0\" end=\"255\" duration=\"100\"/></timeline>"
        "</animations><view extends=\"lv_obj\" width=\"#screen_w\" height=\"#screen_h\" scrollable=\"false\">"
        "<lv_obj name=\"cover_card\" x=\"10\" y=\"20\" width=\"120\" height=\"120\"><lv_image name=\"cover_img\"/></lv_obj>"
        "<lv_obj name=\"transport\" x=\"10\" y=\"200\" width=\"200\" height=\"40\">"
        "<lv_image name=\"order_btn\"/><lv_image name=\"prev_btn\"/><lv_image name=\"play_btn\"/><lv_image name=\"next_btn\"/><lv_image name=\"more_btn\"/></lv_obj>"
        "<lv_image name=\"background_img\"/><lv_obj name=\"overlay_panel\" style_bg_color=\"0x10242d\"/>"
        "<lv_label name=\"title\" text=\"\" style_text_color=\"0xf2f7f8\"/><lv_label name=\"artist\" text=\"\"/><lv_label name=\"album\" text=\"\"/>"
        "<lv_obj name=\"favorite_circle\"><lv_image name=\"favorite_icon\"/></lv_obj>"
        "<lv_obj name=\"quality_pill\" x=\"20\" y=\"130\"><lv_label name=\"format_badge\" text=\"\"/></lv_obj>"
        "<lv_label name=\"pos_label\" text=\"\"/><lv_label name=\"dur_label\" text=\"\"/><lv_label name=\"song_count\" text=\"\"/>"
        "<lv_obj name=\"dismiss_btn\"/>"
        "<lv_slider name=\"progress_slider\" x=\"0\" y=\"150\" width=\"300\" height=\"10\" style_bg_color=\"0x789096\"/>"
        "<lv_slider name=\"volume_slider\"/></view></component>");
}
static void register_layout_fixture(void) {
    assert(player_layouts_session_register_xml("example_minimal", "Example minimal",
                                               fixture_layout_path, false));
}

static void cleanup_layout_fixture(void) {
    player_layouts_session_reset();
    if (fixture_layout_path[0]) unlink(fixture_layout_path);
    if (fixture_layout_dir[0]) rmdir(fixture_layout_dir);
    fixture_layout_path[0] = '\0';
    fixture_layout_dir[0] = '\0';
}

static unsigned action_calls;
static char last_action_owner[PLAYER_LAYOUT_ACTION_MAX];
static char last_action_name[PLAYER_LAYOUT_ACTION_MAX];

static void capture_layout_action(const char * owner, const char * action) {
    ++action_calls;
    snprintf(last_action_owner, sizeof(last_action_owner), "%s", owner ? owner : "");
    snprintf(last_action_name, sizeof(last_action_name), "%s", action ? action : "");
}

static void check_layout_interactions_and_actions(void) {
    char dir[] = "/tmp/player_layouts_interactions_XXXXXX";
    assert(mkdtemp(dir));
    char custom[256], plain[256], invalid[256];
    snprintf(custom, sizeof(custom), "%s/custom.xml", dir);
    snprintf(plain, sizeof(plain), "%s/plain.xml", dir);
    snprintf(invalid, sizeof(invalid), "%s/invalid.xml", dir);
    write_file(custom,
        "<component><consts><bool name=\"compas_lyrics_enabled\" value=\"false\"/>"
        "<str name=\"compas_artist_action\" value=\"none\"/>"
        "<str name=\"compas_album_action\" value=\"menu\"/></consts>"
        "<view extends=\"lv_obj\"><lv_obj name=\"artist_tap\">"
        "<event_cb trigger=\"clicked\" callback=\"compas_action\" user_data=\"custom_artist\"/>"
        "</lv_obj></view></component>");
    write_file(plain, "<component><view extends=\"lv_obj\"/></component>");
    write_file(invalid,
        "<component><consts><str name=\"compas_artist_action\" value=\"bad action\"/>"
        "</consts><view extends=\"lv_obj\"/></component>");

    assert(player_layouts_session_register_xml("interaction_custom", "Custom", custom, false));
    assert(player_layouts_session_register_xml("interaction_plain", "Plain", plain, false));
    assert(player_layouts_session_register_xml("interaction_invalid", "Invalid", invalid, false));
    assert(player_layouts_set_plugin_owner("interaction_custom", "plugin.owner"));
    assert(!player_layouts_set_plugin_owner("interaction_plain", "bad owner"));
    player_layouts_set_action_handler(capture_layout_action);

    lv_obj_t * screen = lv_obj_create(NULL);
    player_layout_kind_t kind;
    lv_obj_t * custom_root = player_layouts_create("interaction_custom", screen, &kind);
    assert(custom_root && kind == PLAYER_LAYOUT_XML_FILE);
    const player_layout_interactions_t * options = player_layouts_get_interactions();
    assert(options && !options->lyrics_enabled);
    assert(strcmp(options->artist_action, "none") == 0);
    assert(strcmp(options->album_action, "menu") == 0);

    lv_obj_t * artist_tap = lv_obj_find_by_name(custom_root, "artist_tap");
    assert(artist_tap);
    action_calls = 0;
    lv_obj_send_event(artist_tap, LV_EVENT_CLICKED, NULL);
    assert(action_calls == 1);
    assert(strcmp(last_action_owner, "plugin.owner") == 0);
    assert(strcmp(last_action_name, "custom_artist") == 0);
    assert(!player_layouts_dispatch_action("bad action"));
    assert(action_calls == 1);

    /* A previous tree's named event cannot route after the active root changes. */
    assert(player_layouts_set_plugin_owner("interaction_plain", "plugin.owner"));
    lv_obj_t * plain_root = player_layouts_create("interaction_plain", screen, &kind);
    assert(plain_root);
    options = player_layouts_get_interactions();
    assert(options->lyrics_enabled && strcmp(options->artist_action, "library") == 0 &&
           strcmp(options->album_action, "library") == 0);
    lv_obj_send_event(artist_tap, LV_EVENT_CLICKED, NULL);
    assert(action_calls == 1);
    /* Explicit plugin dispatch accepts custom action names independent of a
     * layout's metadata; the stale XML target above remains rejected. */
    assert(player_layouts_dispatch_action("custom_artist"));
    assert(action_calls == 2 && strcmp(last_action_name, "custom_artist") == 0);

    /* The active tree has no event declaration, but explicit dispatch is scoped
     * to the live owner and valid action. */
    assert(player_layouts_dispatch_action("menu"));
    assert(action_calls == 3 && strcmp(last_action_owner, "plugin.owner") == 0 &&
           strcmp(last_action_name, "menu") == 0);
    player_layouts_clear_plugin_owner("plugin.owner");
    assert(!player_layouts_dispatch_action("menu"));
    assert(action_calls == 3);

    /* Invalid interaction constants reject the layout without poisoning the
     * defaults used by the next successful layout. */
    assert(!player_layouts_create("interaction_invalid", screen, &kind));
    assert(lv_obj_get_child_count(screen) == 2);
    options = player_layouts_get_interactions();
    assert(options->lyrics_enabled && strcmp(options->artist_action, "library") == 0 &&
           strcmp(options->album_action, "library") == 0);

    lv_obj_delete(screen);
    assert(!player_layouts_dispatch_action("menu"));
    player_layouts_set_action_handler(NULL);
    player_layouts_session_reset();
    unlink(custom); unlink(plain); unlink(invalid);
    rmdir(dir);
}

static bool mkdir_if_missing(const char * path) {
    if (mkdir(path, 0755) == 0) return true;
    assert(errno == EEXIST);
    return false;
}

static void check_discovered_alias_owner(void) {
    bool made_music = mkdir_if_missing("music");
    bool made_plugins = mkdir_if_missing("music/.plugins");
    char bundle[] = "music/.plugins/agent_layout_owner_XXXXXX";
    assert(mkdtemp(bundle));
    char layout_dir[256], same_file[256], other_file[256], bundle_name[96];
    const char * leaf = strrchr(bundle, '/');
    assert(leaf && strlen(leaf + 1) < sizeof(bundle_name));
    snprintf(bundle_name, sizeof(bundle_name), "%s", leaf + 1);
    snprintf(layout_dir, sizeof(layout_dir), "%s/player_layouts", bundle);
    bool made_layout_dir = mkdir_if_missing(layout_dir);
    snprintf(same_file, sizeof(same_file), "%s/discovered.xml", layout_dir);
    snprintf(other_file, sizeof(other_file), "%s/other.xml", layout_dir);
    const char * xml =
        "<component><view extends=\"lv_obj\"><lv_obj name=\"action_tap\">"
        "<event_cb trigger=\"clicked\" callback=\"compas_action\" user_data=\"alias_action\"/>"
        "</lv_obj></view></component>";
    write_file(same_file, xml);
    write_file(other_file, xml);

    char discovered_id[PLAYER_LAYOUT_ID_MAX], other_id[PLAYER_LAYOUT_ID_MAX];
    snprintf(discovered_id, sizeof(discovered_id), "plugin.%s.discovered", bundle_name);
    snprintf(other_id, sizeof(other_id), "plugin.%s.other", bundle_name);
    assert(player_layouts_session_register_xml("session_alias", "Session alias", same_file, false));
    assert(player_layouts_set_plugin_owner("session_alias", "plugin.alias_owner"));
    snprintf(current_settings.player_layout, sizeof(current_settings.player_layout), "%s", discovered_id);
    player_layouts_rescan();
    assert(player_layouts_find(discovered_id) >= 0); /* selected discovered alias survives dedup */
    assert(player_layouts_find(other_id) >= 0);

    player_layouts_set_action_handler(capture_layout_action);
    lv_obj_t * screen = lv_obj_create(NULL);
    player_layout_kind_t kind;
    action_calls = 0;
    lv_obj_t * root = player_layouts_create(discovered_id, screen, &kind);
    assert(root && kind == PLAYER_LAYOUT_XML_FILE);
    lv_obj_send_event(lv_obj_find_by_name(root, "action_tap"), LV_EVENT_CLICKED, NULL);
    assert(action_calls == 1 && strcmp(last_action_owner, "plugin.alias_owner") == 0 &&
           strcmp(last_action_name, "alias_action") == 0);

    root = player_layouts_create(other_id, screen, &kind);
    assert(root && kind == PLAYER_LAYOUT_XML_FILE);
    lv_obj_send_event(lv_obj_find_by_name(root, "action_tap"), LV_EVENT_CLICKED, NULL);
    assert(action_calls == 1); /* same plugin bundle, different XML path has no owner */
    lv_obj_delete(screen);
    player_layouts_set_action_handler(NULL);
    current_settings.player_layout[0] = '\0';
    player_layouts_session_reset();
    unlink(same_file); unlink(other_file);
    player_layouts_rescan();
    if (made_layout_dir) rmdir(layout_dir);
    rmdir(bundle);
    if (made_plugins) rmdir("music/.plugins");
    if (made_music) rmdir("music");
}

/* Roles the lyrics support added; layouts without them behave as before. */
static bool role_is_lyrics_extra(const char * name) {
    return strncmp(name, "lyrics_", 7) == 0;
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
        if (role_is_lyrics_extra(roles[i].name)) { assert(!obj); continue; } /* the example has none */
        if (!obj) { fprintf(stderr, "missing role %s\n", roles[i].name); abort(); }
        if (roles[i].type && !lv_obj_check_type(obj, roles[i].type)) {
            fprintf(stderr, "role %s has the wrong type\n", roles[i].name);
            abort();
        }
    }
    assert_bg_color(lv_obj_find_by_name(scr, "overlay_panel"), 0x10242d);
    assert_text_color(lv_obj_find_by_name(scr, "title"), 0xf2f7f8);
    assert_bg_color(lv_obj_find_by_name(scr, "progress_slider"), 0x789096);

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

/* The path an XML image name resolves to in the layout created last. */
static const char * registered_image(const char * name) {
    lv_xml_component_scope_t * scope = lv_xml_component_get_scope("compas_player_layout");
    assert(scope);
    return (const char *) lv_xml_get_image(scope, name);
}

/* The stock Now Playing artwork a layout may name. */
static void check_stock_image_names(void) {
    static const struct { const char * name; const char * file; } names[] = {
        { "topbar_bg", "topbar_bg.png" }, { "bottom_panel", "buttom.png" }, { "progress", "progress.png" },
        { "progress_bg", "progress_bg.png" }, { "speed", "speed.png" }, { "dlna", "dlna.png" },
        { "btn_play_s", "btn_play_s.png" }, { "ic_more_s", "ic_more_s.png" }, { "order_s", "order_s.png" },
    };
    char dir[] = "/tmp/player_layouts_img_XXXXXX";
    assert(mkdtemp(dir));
    char path[256];
    snprintf(path, sizeof(path), "%s/stock_names.xml", dir);
    write_file(path, "<component><view extends=\"lv_obj\"/></component>");
    assert(player_layouts_session_register_xml("stock_names", "Stock names", path, false));

    lv_obj_t * scr = lv_obj_create(NULL);
    player_layout_kind_t kind;
    assert(player_layouts_create("stock_names", scr, &kind));
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        char expected[128];
        snprintf(expected, sizeof(expected), "S:assets/theme2/playing_plane/%s", names[i].file);
        const char * actual = registered_image(names[i].name);
        if (!actual || strcmp(actual, expected) != 0) {
            fprintf(stderr, "image name %s resolves to %s, expected %s\n", names[i].name, actual ? actual : "(none)", expected);
            abort();
        }
    }
    lv_obj_delete(scr);
    player_layouts_release();
    player_layouts_session_reset();
    unlink(path);
    rmdir(dir);
}

/* The png files in `<stem>/` next to `<stem>.xml` are registered by file stem. */
static void check_layout_image_folder(void) {
    char dir[] = "/tmp/player_layouts_folder_XXXXXX";
    assert(mkdtemp(dir));
    char path[256], folder[256], file[320];
    snprintf(path, sizeof(path), "%s/art_demo.xml", dir);
    snprintf(folder, sizeof(folder), "%s/art_demo", dir);
    assert(mkdir(folder, 0755) == 0);
    /* Not an image, a bad name, a directory and a duplicate of a stock name: only the
     * valid *.png files are registered, and the stock name is replaced. */
    static const char * files[] = { "my_art.png", "btn_play.png", "declared.png", "bad name.png", "dot.dot.png",
                                    "notes.txt", "UPPER-1.png" };
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
        snprintf(file, sizeof(file), "%s/%s", folder, files[i]);
        write_file(file, "x");
    }
    snprintf(file, sizeof(file), "%s/dir.png", folder);
    assert(mkdir(file, 0755) == 0);
    /* More than 32 files: the first 32 by name are kept. */
    for (int i = 0; i < 40; ++i) {
        snprintf(file, sizeof(file), "%s/n%02d.png", folder, i);
        write_file(file, "x");
    }
    write_file(path,
        "<component><images><file name=\"declared\" src_path=\"S:/declared_by_xml.png\"/></images>"
        "<view extends=\"lv_obj\"/></component>");
    assert(player_layouts_session_register_xml("art_demo", "Art demo", path, false));

    lv_obj_t * scr = lv_obj_create(NULL);
    player_layout_kind_t kind;
    for (int round = 0; round < 2; ++round) { /* the images are released and registered again */
        assert(player_layouts_create("art_demo", scr, &kind));
        char expected[320];
        snprintf(expected, sizeof(expected), "S:%s/my_art.png", folder);
        assert(strcmp(registered_image("my_art"), expected) == 0);
        snprintf(expected, sizeof(expected), "S:%s/btn_play.png", folder); /* beats the built-in name */
        assert(strcmp(registered_image("btn_play"), expected) == 0);
        assert(strcmp(registered_image("declared"), "S:/declared_by_xml.png") == 0); /* the XML wins */
        snprintf(expected, sizeof(expected), "S:%s/UPPER-1.png", folder);
        assert(strcmp(registered_image("UPPER-1"), expected) == 0);
        snprintf(expected, sizeof(expected), "S:%s/n00.png", folder);
        assert(strcmp(registered_image("n00"), expected) == 0);
        assert(!registered_image("n39")); /* past the 32nd name */
        assert(!registered_image("bad name") && !registered_image("dot.dot") && !registered_image("notes"));
        assert(strcmp(registered_image("btn_next"), "S:assets/theme2/playing_plane/btn_next.png") == 0);
        assert(!registered_image("dir")); /* a directory is not an image */
        lv_obj_clean(scr);
        player_layouts_release();
    }
    lv_obj_delete(scr);
    player_layouts_session_reset();

    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
        snprintf(file, sizeof(file), "%s/%s", folder, files[i]);
        unlink(file);
    }
    for (int i = 0; i < 40; ++i) {
        snprintf(file, sizeof(file), "%s/n%02d.png", folder, i);
        unlink(file);
    }
    snprintf(file, sizeof(file), "%s/dir.png", folder);
    rmdir(file);
    rmdir(folder);
    unlink(path);
    rmdir(dir);
}

/* plugins_examples/player_layouts/hiby_stock*.xml: every file parses with the
 * roles it should have and fits its own screen; the one for this board is
 * what the plain name resolves to. */
static void check_hiby_stock_layouts(void) {
#if defined(BOARD_R3PROII)
    static const struct { const char * file; int32_t w, h; } variants[] = {
        { "../compas-plugins/examples/HiByStockPlayer/player_layouts/hiby_stock@480x720.xml", BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT },
    };
#elif defined(BOARD_R3II_2025)
    static const struct { const char * file; int32_t w, h; } variants[] = {
        { "../compas-plugins/examples/HiByStockPlayer/player_layouts/hiby_stock@320x480.xml", BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT },
    };
#else
    static const struct { const char * file; int32_t w, h; } variants[] = {
        { "../compas-plugins/examples/HiByStockPlayer/player_layouts/hiby_stock.xml", BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT },
    };
#endif
    static const char * const on_screen[] = {
        "cover_card", "lyrics_area", "topbar_bg", "bottom_panel", "title", "artist", "album", "favorite_circle",
        "quality_pill", "song_count", "progress_slider", "pos_label", "dur_label", "order_btn", "prev_btn",
        "play_btn", "next_btn", "more_btn", "dismiss_btn",
    };
    static const char * const transport[] = { "order_btn", "prev_btn", "play_btn", "next_btn", "more_btn" };
    for (size_t v = 0; v < sizeof(variants) / sizeof(variants[0]); ++v) {
        /* The plain file name resolves to this board's own variant. */
        const int32_t screen_w = v == 0 ? BOARD_SCREEN_WIDTH : variants[v].w;
        const int32_t screen_h = v == 0 ? BOARD_SCREEN_HEIGHT : variants[v].h;
        assert(player_layouts_session_register_xml("hiby_check", "HiBy check", variants[v].file, false));
        lv_obj_t * scr = lv_obj_create(NULL);
        lv_screen_load(scr);
        player_layout_kind_t kind;
        lv_obj_t * root = player_layouts_create("hiby_check", scr, &kind);
        if (!root) { fprintf(stderr, "%s did not load\n", variants[v].file); abort(); }
        for (size_t i = 0; i < sizeof(roles) / sizeof(roles[0]); ++i) {
            lv_obj_t * obj = lv_obj_find_by_name(scr, roles[i].name);
            bool optional = !strcmp(roles[i].name, "overlay_panel") || !strcmp(roles[i].name, "background_img") ||
                            !strcmp(roles[i].name, "lyrics_toggle") ||
                            (!strcmp(roles[i].name, "album") && screen_w == 320);
            if (!obj && optional) continue;
            if (!obj || (roles[i].type && !lv_obj_check_type(obj, roles[i].type))) {
                fprintf(stderr, "%s: role %s missing or of the wrong type\n", variants[v].file, roles[i].name);
                abort();
            }
        }
        assert(player_layouts_find_timeline(root, "lyrics_open") && player_layouts_find_timeline(root, "lyrics_close"));

        lv_obj_update_layout(scr);
        for (size_t i = 0; i < sizeof(on_screen) / sizeof(on_screen[0]); ++i) {
            lv_obj_t * obj = lv_obj_find_by_name(scr, on_screen[i]);
            if (!obj) { assert(!strcmp(on_screen[i], "album")); continue; }
            lv_area_t a;
            lv_obj_get_coords(obj, &a);
            if (a.x1 < 0 || a.y1 < 0 || a.x2 >= screen_w || a.y2 >= screen_h) {
                fprintf(stderr, "%s: %s is off screen (%d,%d)-(%d,%d)\n", variants[v].file, on_screen[i],
                        (int) a.x1, (int) a.y1, (int) a.x2, (int) a.y2);
                abort();
            }
        }
        /* The transport row reads left to right without overlap, below the progress bar. */
        lv_area_t progress, previous = { 0 };
        lv_obj_get_coords(lv_obj_find_by_name(scr, "progress_slider"), &progress);
        for (size_t i = 0; i < sizeof(transport) / sizeof(transport[0]); ++i) {
            lv_area_t a;
            lv_obj_get_coords(lv_obj_find_by_name(scr, transport[i]), &a);
            assert(a.y1 > progress.y2);
            if (i) assert(a.x1 > previous.x2);
            previous = a;
        }
        /* The lyrics area lies over the cover, below the top bar. */
        lv_area_t cover, lyrics, top_bar;
        lv_obj_get_coords(lv_obj_find_by_name(scr, "cover_card"), &cover);
        lv_obj_get_coords(lv_obj_find_by_name(scr, "lyrics_area"), &lyrics);
        lv_obj_get_coords(lv_obj_find_by_name(scr, "topbar_bg"), &top_bar);
        assert(lyrics.y1 >= top_bar.y2 && lyrics.y2 <= cover.y2 + 1 && lyrics.x1 <= cover.x1 + 24);

        lv_screen_load(lv_obj_create(NULL));
        lv_obj_delete(scr);
        player_layouts_release();
        player_layouts_session_reset();
    }

    /* The plain name resolves to the file for this board. */
    assert(player_layouts_session_register_xml("hiby_stock", "HiBy Stock", variants[0].file, false));
    lv_obj_t * scr = lv_obj_create(NULL);
    player_layout_kind_t kind;
    assert(player_layouts_create("hiby_stock", scr, &kind));
    lv_obj_update_layout(scr);
    int32_t expected_cover = BOARD_SCREEN_HEIGHT >= 800 ? 480 : BOARD_SCREEN_HEIGHT >= 720 ? 432 : 288;
    assert(lv_obj_get_width(lv_obj_find_by_name(scr, "cover_card")) == expected_cover);
    lv_obj_delete(scr);
    player_layouts_release();
    player_layouts_session_reset();
}

/* Shipped editorial layouts are checked at their real resolution: percentages,
 * flex sizes and the lyric timelines otherwise resolve against the host board. */
static void check_editorial_layouts(void) {
    static const char * const ids[] = { "gallery", "panorama" };
#if defined(BOARD_R3PROII)
    static const size_t board_size_index = 1;
#elif defined(BOARD_R3II_2025)
    static const size_t board_size_index = 2;
#else
    static const size_t board_size_index = 0;
#endif
    static const struct { int32_t w, h; } sizes[] = {
        { 480, 800 }, { 480, 720 }, { 320, 480 },
    };
    static const role_t required_roles[] = {
        { "cover_card", NULL }, { "cover_img", &lv_image_class }, { "title", &lv_label_class },
        { "play_btn", &lv_image_class }, { "progress_slider", &lv_slider_class },
    };
    static const char * const bounded_roles[] = {
        "cover_card", "cover_img", "title", "artist", "album", "quality_pill", "format_badge",
        "song_count", "progress_slider", "pos_label", "dur_label", "order_btn", "prev_btn",
        "play_circle", "play_btn", "next_btn", "more_btn", "dismiss_btn", "lyrics_area",
    };
    static const char * const transport[] = { "order_btn", "prev_btn", "play_circle", "play_btn", "next_btn", "more_btn" };
    static const char * const timelines[] = { "lyrics_open", "lyrics_close", "track_change", "screen_enter" };

    for (size_t li = 0; li < sizeof(ids) / sizeof(ids[0]); ++li) {
        for (size_t si = board_size_index; si == board_size_index; ++si) {
            char path[160];
            if (si == 0) snprintf(path, sizeof(path), "assets/theme2/player_layouts/%s.xml", ids[li]);
            else snprintf(path, sizeof(path), "assets/theme2/player_layouts/%s@%dx%d.xml", ids[li], sizes[si].w, sizes[si].h);
            lv_display_set_resolution(test_display, sizes[si].w, sizes[si].h);
            char reg_id[64];
            snprintf(reg_id, sizeof(reg_id), "editorial_%s", ids[li]);
            assert(player_layouts_session_register_xml(reg_id, ids[li], path, false));

            lv_obj_t * scr = lv_obj_create(NULL);
            lv_screen_load(scr);
            player_layout_kind_t kind;
            lv_obj_t * root = player_layouts_create(reg_id, scr, &kind);
            if (!root || kind != PLAYER_LAYOUT_XML_FILE) {
                fprintf(stderr, "%s did not parse as an XML layout\n", path);
                abort();
            }
            for (size_t i = 0; i < sizeof(required_roles) / sizeof(required_roles[0]); ++i) {
                lv_obj_t * obj = lv_obj_find_by_name(scr, required_roles[i].name);
                if (!obj || (required_roles[i].type && !lv_obj_check_type(obj, required_roles[i].type))) {
                    fprintf(stderr, "%s: required role %s missing or wrong type\n", path, required_roles[i].name);
                    abort();
                }
            }
            /* Panorama keeps the blurred backdrop; its small thumbnail is optional.
             * The other three designs use the cover card as the lyric transition. */
            if (!strcmp(ids[li], "panorama")) {
                /* Its full-screen cover_card is the backdrop; lyrics add a
                 * small thumbnail rather than shrinking that background. */
                lv_obj_t * thumb = lv_obj_find_by_name(scr, "cover_thumbnail");
                assert(!thumb || lv_obj_check_type(thumb, &lv_image_class));
            }
            lv_obj_update_layout(scr);
            check_editorial_color_roles(ids[li], scr);

            for (size_t i = 0; i < sizeof(timelines) / sizeof(timelines[0]); ++i) {
                lv_anim_timeline_t * timeline = player_layouts_find_timeline(root, timelines[i]);
                if ((i < 2 && !timeline && strcmp(ids[li], "vinyl") != 0)) {
                    fprintf(stderr, "%s: required timeline %s missing\n", path, timelines[i]);
                    abort();
                }
            }

            for (size_t i = 0; i < sizeof(bounded_roles) / sizeof(bounded_roles[0]); ++i) {
                lv_obj_t * obj = lv_obj_find_by_name(scr, bounded_roles[i]);
                if (!obj) continue;
                lv_area_t area;
                lv_obj_get_coords(obj, &area);
                if (!area_inside(area, sizes[si].w, sizes[si].h)) {
                    fprintf(stderr, "%s: initial %s out of bounds (%d,%d)-(%d,%d) at %dx%d\n", path,
                            bounded_roles[i], (int) area.x1, (int) area.y1, (int) area.x2, (int) area.y2,
                            sizes[si].w, sizes[si].h);
                    abort();
                }
            }

            lv_area_t seek, pos = { 0 }, dur = { 0 };
            lv_obj_get_coords(lv_obj_find_by_name(scr, "progress_slider"), &seek);
            lv_obj_t * pos_obj = lv_obj_find_by_name(scr, "pos_label");
            lv_obj_t * dur_obj = lv_obj_find_by_name(scr, "dur_label");
            lv_obj_t * top_status[] = {
                lv_obj_find_by_name(scr, "quality_pill"), lv_obj_find_by_name(scr, "song_count"),
            };
            if (pos_obj) lv_obj_get_coords(pos_obj, &pos);
            if (dur_obj) lv_obj_get_coords(dur_obj, &dur);
            assert(!pos_obj || !areas_overlap(seek, pos));
            assert(!dur_obj || !areas_overlap(seek, dur));
            for (size_t j = 0; j < sizeof(top_status) / sizeof(top_status[0]); ++j) {
                if (!top_status[j]) continue;
                lv_area_t status_area;
                lv_obj_get_coords(top_status[j], &status_area);
                assert(!areas_overlap(status_area, seek));
                assert(!pos_obj || !areas_overlap(status_area, pos));
                assert(!dur_obj || !areas_overlap(status_area, dur));
            }
            for (size_t i = 0; i < sizeof(transport) / sizeof(transport[0]); ++i) {
                lv_obj_t * obj = lv_obj_find_by_name(scr, transport[i]);
                if (!obj) continue;
                lv_area_t a;
                lv_obj_get_coords(obj, &a);
                assert(!areas_overlap(a, seek));
                assert(!pos_obj || !areas_overlap(a, pos));
                assert(!dur_obj || !areas_overlap(a, dur));
                for (size_t j = 0; j < sizeof(top_status) / sizeof(top_status[0]); ++j) {
                    if (!top_status[j]) continue;
                    lv_area_t status_area;
                    lv_obj_get_coords(top_status[j], &status_area);
                    assert(!areas_overlap(a, status_area));
                }
            }

            lv_obj_t * cover = lv_obj_find_by_name(scr, "cover_card");
            lv_obj_t * title = lv_obj_find_by_name(scr, "title");
            lv_obj_t * artist = lv_obj_find_by_name(scr, "artist");
            int32_t cover_w = lv_obj_get_width(cover);
            int32_t cover_h = lv_obj_get_height(cover);
            lv_area_t before_title, before_artist;
            lv_obj_get_coords(title, &before_title);
            if (artist) lv_obj_get_coords(artist, &before_artist);
            lv_anim_timeline_t * open = player_layouts_find_timeline(root, "lyrics_open");
            if (strcmp(ids[li], "vinyl") == 0) {
                /* Vinyl reuses the native lyrics morph; geometry is checked
                 * above, while the binder/device checks exercise the motion. */
                assert(!open && !player_layouts_find_timeline(root, "lyrics_close"));
                assert(lv_obj_get_style_radius(cover, 0) == 32767);
                assert(lv_obj_get_style_clip_corner(cover, 0));
                lv_screen_load(lv_obj_create(NULL));
                lv_obj_delete(scr);
                player_layouts_release();
                player_layouts_session_reset();
                continue;
            }
            lv_anim_timeline_set_progress(open, 0);
            uint32_t ms = lv_anim_timeline_start(open);
            assert(ms > 0);
            run_ms(ms + 100);
            lv_obj_update_layout(scr);
            if (strcmp(ids[li], "panorama") != 0) assert(lv_obj_get_width(cover) < cover_w);
            else assert(lv_obj_get_width(cover) == cover_w && lv_obj_get_height(cover) == cover_h);
            lv_area_t after;
            lv_obj_get_coords(cover, &after);
            assert(area_inside(after, sizes[si].w, sizes[si].h));
            lv_obj_get_coords(title, &after);
            assert(area_inside(after, sizes[si].w, sizes[si].h));
            if (artist) {
                lv_obj_get_coords(artist, &after);
                assert(area_inside(after, sizes[si].w, sizes[si].h));
            }
            lv_obj_t * lyrics_obj = lv_obj_find_by_name(scr, "lyrics_area");
            if (lyrics_obj) {
                lv_area_t lyrics;
                lv_obj_get_coords(lyrics_obj, &lyrics);
                assert(area_inside(lyrics, sizes[si].w, sizes[si].h));
                /* Leave a clear 12px gap between the lyrics text viewport and seek rail. */
                assert(lyrics.y2 + 13 <= seek.y1);
                for (size_t i = 0; i < sizeof(transport) / sizeof(transport[0]); ++i) {
                    lv_obj_t * obj = lv_obj_find_by_name(scr, transport[i]);
                    if (!obj) continue;
                    lv_area_t a;
                    lv_obj_get_coords(obj, &a);
                    assert(!areas_overlap(lyrics, a));
                }
            }
            if (strcmp(ids[li], "gallery") == 0) {
                assert(lv_obj_get_style_text_align(title, 0) == LV_TEXT_ALIGN_LEFT);
                assert(lv_obj_get_style_text_align(artist, 0) == LV_TEXT_ALIGN_LEFT);
            }
            /* Lyrics layouts reposition their metadata as well as the cover. */
            assert(before_title.x1 != after.x1 || before_title.y1 != after.y1 ||
                   before_title.x2 != after.x2 || before_title.y2 != after.y2);
            if (artist) {
                lv_area_t now_artist;
                lv_obj_get_coords(artist, &now_artist);
                assert(before_artist.x1 != now_artist.x1 || before_artist.y1 != now_artist.y1 ||
                       before_artist.x2 != now_artist.x2 || before_artist.y2 != now_artist.y2);
            }
            lv_anim_timeline_t * close = player_layouts_find_timeline(root, "lyrics_close");
            lv_anim_timeline_set_progress(close, 0);
            ms = lv_anim_timeline_start(close);
            run_ms(ms + 100);
            if (strcmp(ids[li], "gallery") == 0) {
                assert(lv_obj_get_style_text_align(title, 0) == LV_TEXT_ALIGN_CENTER);
                assert(lv_obj_get_style_text_align(artist, 0) == LV_TEXT_ALIGN_CENTER);
            }
            assert(lv_obj_get_width(cover) == cover_w);
            lv_obj_get_coords(title, &after);
            assert(area_inside(after, sizes[si].w, sizes[si].h));
            if (artist) {
                lv_obj_get_coords(artist, &after);
                assert(area_inside(after, sizes[si].w, sizes[si].h));
            }

            lv_screen_load(lv_obj_create(NULL));
            lv_obj_delete(scr);
            player_layouts_release();
            player_layouts_session_reset();
        }
    }
    lv_display_set_resolution(test_display, BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT);
}

static const char *current_matrix_layout;
static unsigned matrix_bounds_issues;
static unsigned count_named_widgets(lv_obj_t *obj) {
    unsigned count = 0;
    const char *name = lv_obj_get_name(obj);
    if (name && name[0]) {
        count++;
        matrix_named_widgets++;
        bool clickable = lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE);
        if (clickable) matrix_clickable_widgets++;
        lv_area_t area;
        lv_obj_get_coords(obj, &area);
        if (!(area.x1 >= 0 && area.y1 >= 0 && area.x2 < BOARD_SCREEN_WIDTH && area.y2 < BOARD_SCREEN_HEIGHT)) {
            fprintf(stderr, "%s: named widget %s bounds (%d,%d)-(%d,%d) outside %dx%d\n",
                    current_matrix_layout, name, (int) area.x1, (int) area.y1,
                    (int) area.x2, (int) area.y2, BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT);
            matrix_bounds_issues++;
            if (clickable) {
                fprintf(stderr, "%s: clickable target %s extends outside the display\n", current_matrix_layout, name);
                matrix_clickable_bounds_issues++;
            }
        }
    }
    uint32_t children = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < children; ++i) count += count_named_widgets(lv_obj_get_child(obj, i));
    return count;
}

static bool is_board_variant_xml(const char *name) {
    if (!name || strchr(name, '@')) return true;
    size_t length = strlen(name);
    if (length < 8 || strcmp(name + length - 4, ".xml") != 0) return false;
    const char *end = name + length - 4;
    const char *p = end;
    while (p > name && isdigit((unsigned char)p[-1])) --p;
    if (p == end || p == name || p[-1] != 'x') return false;
    const char *x = p - 1;
    p = x;
    while (p > name && isdigit((unsigned char)p[-1])) --p;
    return p != x && p > name && p[-1] == '_';
}

static bool find_base_layout_xml(const char *directory, char *out, size_t out_size) {
    DIR *layouts = opendir(directory);
    if (!layouts) return false;
    char selected[PATH_MAX] = "";
    struct dirent *entry;
    while ((entry = readdir(layouts))) {
        size_t length = strlen(entry->d_name);
        if (length < 5 || strcmp(entry->d_name + length - 4, ".xml") != 0 ||
            entry->d_name[0] == '.' || is_board_variant_xml(entry->d_name)) continue;
        if (!selected[0] || strcmp(entry->d_name, selected) < 0)
            snprintf(selected, sizeof(selected), "%s", entry->d_name);
    }
    closedir(layouts);
    if (!selected[0]) return false;
    int n = snprintf(out, out_size, "%s/%s", directory, selected);
    return n > 0 && (size_t)n < out_size;
}

static void check_plugin_layout_render_matrix(void) {
    const char *root = getenv("COMPAS_PLUGINS_ROOT");
    if (!root || !root[0]) root = "../compas-plugins/plugins";
    DIR *plugins = opendir(root);
    assert(plugins);
    struct dirent *plugin;
    unsigned rendered = 0;
    while ((plugin = readdir(plugins))) {
        if (plugin->d_name[0] == '.') continue;
        char layout_dir[PATH_MAX];
        snprintf(layout_dir, sizeof(layout_dir), "%s/%s/player_layouts", root, plugin->d_name);
        char base_path[PATH_MAX];
        if (!find_base_layout_xml(layout_dir, base_path, sizeof(base_path))) continue;
        const char *path = base_path;
        char id[PLAYER_LAYOUT_ID_MAX];
        snprintf(id, sizeof(id), "matrix_%u", rendered);
        assert(player_layouts_session_register_xml(id, plugin->d_name, path, false));
        lv_obj_t *screen = lv_obj_create(NULL);
        lv_screen_load(screen);
        player_layout_kind_t kind;
        lv_obj_t *layout = player_layouts_create(id, screen, &kind);
        if (!layout || kind != PLAYER_LAYOUT_XML_FILE) {
            fprintf(stderr, "plugin layout failed: %s\n", path);
            abort();
        }
        lv_obj_update_layout(screen);
        lv_refr_now(test_display);
        current_matrix_layout = path;
        unsigned clickable_before = matrix_clickable_widgets;
        unsigned named = count_named_widgets(layout);
        assert(named >= 3);
        assert(matrix_clickable_widgets > clickable_before);
        rendered++;
        lv_screen_load(lv_obj_create(NULL));
        lv_obj_delete(screen);
        player_layouts_release();
        player_layouts_session_reset();
    }
    closedir(plugins);
    assert(rendered > 0);
    printf("plugin layout renderer: rendered %u layouts at %dx%d\n",
           rendered, BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT);
    printf("plugin layout renderer: observed %u named widgets, %u clickable named targets, %u display flushes\n",
           matrix_named_widgets, matrix_clickable_widgets, matrix_flushes);
    if (matrix_bounds_issues)
        printf("plugin layout renderer: %u named widget rectangles extend beyond display edges (reported above; often deliberate cover cropping)\n",
               matrix_bounds_issues);
    assert(matrix_clickable_bounds_issues == 0);
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
    assert(player_layouts_set_plugin_owner("plugin.clean", "plugin.owner_a"));
    assert(!player_layouts_session_clear_plugin_selection("plugin.owner_a"));
    assert(!strcmp(player_layouts_effective_id(), "plugin.clean"));
    root = player_layouts_create("plugin.clean", scr, &kind);
    assert(root && lv_obj_get_style_width(root, 0) == 111);
    lv_obj_clean(scr);
    player_layouts_release();

    /* A plugin can clear only its own session override; doing so restores the
     * persisted choice without letting a different owner clear it. */
    assert(player_layouts_session_select_xml("from_callback2", "Callback A", good, true));
    assert(player_layouts_set_plugin_owner("from_callback2", "plugin.owner_a"));
    assert(!player_layouts_session_clear_plugin_selection("plugin.owner_b"));
    assert(!strcmp(player_layouts_effective_id(), "from_callback2"));
    assert(player_layouts_session_clear_plugin_selection("plugin.owner_a"));
    assert(!strcmp(player_layouts_effective_id(), "plugin.clean"));
    assert(!player_layouts_session_clear_plugin_selection("plugin.owner_a"));

    assert(player_layouts_session_select_xml("from_callback3", "Callback B", good, true));
    assert(player_layouts_set_plugin_owner("from_callback3", "plugin.owner_b"));
    assert(!player_layouts_session_clear_plugin_selection("plugin.owner_a"));
    assert(!strcmp(player_layouts_effective_id(), "from_callback3"));
    assert(player_layouts_session_clear_plugin_selection("plugin.owner_b"));
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

static void check_plugin_owner_selection(void) {
    char saved_setting[sizeof(current_settings.player_layout)];
    snprintf(saved_setting, sizeof(saved_setting), "%s", current_settings.player_layout);
    assert(player_layouts_session_register_xml("persisted.layout", "Persisted", "/layouts/persisted.xml", false));
    snprintf(current_settings.player_layout, sizeof(current_settings.player_layout), "persisted.layout");
    assert(!player_layouts_session_clear_plugin_selection("plugin.owner_a"));

    assert(player_layouts_session_select_xml("owner_a.layout", "Owner A", "/plugins/a.xml", true));
    assert(player_layouts_set_plugin_owner("owner_a.layout", "plugin.owner_a"));
    assert(!player_layouts_session_clear_plugin_selection("plugin.owner_b"));
    assert(!strcmp(player_layouts_effective_id(), "owner_a.layout"));
    assert(player_layouts_session_clear_plugin_selection("plugin.owner_a"));
    assert(!strcmp(player_layouts_effective_id(), "persisted.layout"));

    assert(player_layouts_session_select_xml("owner_b.layout", "Owner B", "/plugins/b.xml", true));
    assert(player_layouts_set_plugin_owner("owner_b.layout", "plugin.owner_b"));
    assert(!player_layouts_session_clear_plugin_selection("plugin.owner_a"));
    assert(!strcmp(player_layouts_effective_id(), "owner_b.layout"));
    assert(player_layouts_session_clear_plugin_selection("plugin.owner_b"));
    assert(!strcmp(player_layouts_effective_id(), "persisted.layout"));

    snprintf(current_settings.player_layout, sizeof(current_settings.player_layout), "%s", saved_setting);
    player_layouts_session_reset();
}

int main(int argc, char ** argv) {
    lv_init();
    assert(ui_fonts_init());
#if LV_USE_LOG
    lv_log_register_print_cb(log_cb);
#endif
    lv_display_t * display = lv_display_create(BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT);
    test_display = display;
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
    create_layout_fixture();
    register_layout_fixture();

    if (argc == 2 && strcmp(argv[1], "--interactions-only") == 0) {
        check_layout_interactions_and_actions();
        check_discovered_alias_owner();
        cleanup_layout_fixture();
        printf("player layout interactions: passed\n");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--plugin-owner-only") == 0) {
        check_plugin_owner_selection();
        cleanup_layout_fixture();
        printf("player layout plugin ownership: passed\n");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--plugin-render-matrix") == 0) {
        check_plugin_layout_render_matrix();
        cleanup_layout_fixture();
        return 0;
    }
    check_registry();
    check_layout_interactions_and_actions();
    check_discovered_alias_owner();
    register_layout_fixture();
    check_example_layout();
    check_stock_image_names();
    check_layout_image_folder();
    check_hiby_stock_layouts();
    check_editorial_layouts();
    check_switching_and_failures();
    register_layout_fixture();
    check_no_heap_growth();
    cleanup_layout_fixture();
    printf("player layouts: passed\n");
    return 0;
}
