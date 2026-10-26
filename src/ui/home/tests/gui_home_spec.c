#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <lvgl.h>

#include "../../assets/assets.h"
#include "../../../library/artwork/coordinator/artwork_coordinator.h"
#include "../../../library/artwork/decode/cover_decode.h"
#include "../../application/gui.h"
#include "../../shell/gui_navigation.h"
#include "../../library/collections/gui_books.h"
#include "../../network/gui_network.h"
#include "../../network/subsonic/gui_subsonic.h"
#include "../home_layout_sizing.h"
#include "../../text/i18n/i18n.h"
#include "../../widgets/builders/screen_builders.h"

home_layout_config_t home_layout_config;
lv_style_t style_theme_tile;
lv_style_t style_theme_tile_pressed;

static void * tracked_calloc_ptr;
static void * tracked_malloc_ptr;
static int tracked_calloc_freed;
static int tracked_malloc_freed;
static lv_obj_t * settings_screen;
lv_obj_t * stream_media_screen;

static void * spec_calloc(size_t count, size_t size) {
    void * ptr = calloc(count, size);
    tracked_calloc_ptr = ptr;
    return ptr;
}

static void * spec_malloc(size_t size) {
    void * ptr = malloc(size);
    tracked_malloc_ptr = ptr;
    return ptr;
}

static void spec_free(void * ptr) {
    if (ptr == tracked_calloc_ptr) tracked_calloc_freed++;
    if (ptr == tracked_malloc_ptr) tracked_malloc_freed++;
    free(ptr);
}

#define calloc spec_calloc
#define malloc spec_malloc
#define free spec_free
#ifdef HOME_SPEC_FINAL
#include "../gui_home.h"
#include "home_final_fixture.c"
#else
#include "home_baseline_fixture.c"
#endif
#undef calloc
#undef malloc
#undef free

#ifdef HOME_SPEC_FINAL
static lv_obj_t * harness_build_home_screen(void) {
    return build_home_screen();
}
#else
static lv_obj_t * harness_build_home_screen(void) { return build_home_screen(); }
#endif

static char trace[8192];
static size_t trace_len;
static icon_grid_item_t captured[HOME_LAYOUT_MAX_TILES];
static int captured_count;
static bool captured_list;
static int finalize_count;
static int plugin_click_index = -1;
static int plugin_lookup_count;
static lv_obj_t * music_dest;
static lv_obj_t * wireless_dest;
static lv_obj_t * dac_dest;
static lv_obj_t * last_nav_dest;
static const char * resolved_asset;

static void record(const char * text) {
    int n = snprintf(trace + trace_len, sizeof(trace) - trace_len, "%s\n", text);
    assert(n > 0 && (size_t)n < sizeof(trace) - trace_len);
    trace_len += (size_t)n;
}

const char * i18n_tr(const char * text) { return text; }
lv_obj_t * gui_library_get_music_screen(void) { return music_dest; }
lv_obj_t * gui_network_get_wireless_screen(void) { return wireless_dest; }
lv_obj_t * gui_shell_get_dac_home_screen(void) { return dac_dest; }
uint32_t gui_anim_ms(uint32_t base_ms) { return base_ms; }
void nav_push(lv_obj_t * screen) { last_nav_dest = screen; }
void gui_books_home_tile_cb(lv_event_t * event) { (void)event; }
void subsonic_tile_cb(lv_event_t * event) { (void)event; }
void plugin_manager_home_tile_clicked(int index) { plugin_click_index = index; }
int plugin_manager_find_home_tile_by_id(const char * id) {
    plugin_lookup_count++;
    return strcmp(id, "plugin.sample") == 0 ? 3 : -1;
}
const char * plugin_manager_get_home_tile_icon(int index) { assert(index == 3); return "plugin/icon.png"; }
const char * plugin_manager_get_home_tile_icon_selected(int index) { assert(index == 3); return "plugin/icon_s.png"; }
const char * plugin_manager_get_home_tile_label(int index) { assert(index == 3); return "Plugin tile"; }
const char * asset_path_plain(const char * relative_path) { return relative_path; }
const char * asset_path(const char * relative_path) {
    static char path[128];
    snprintf(path, sizeof(path), "resolved:%s", relative_path);
    return path;
}
lv_style_t * gui_theme_accent_style(void) { return NULL; }

/* Deliberately keep this fixture focused on BIN parsing and screen lifetime;
 * JPEG probing/decoding and coordinator behavior are not characterized here. */
bool jpeg_probe(const uint8_t * data, uint32_t size, jpeg_probe_t * probe) {
    (void)data; (void)size; memset(probe, 0, sizeof(*probe)); return false;
}
artwork_acquire_result_t artwork_coordinator_acquire(artwork_priority_t priority,
                                                     size_t bytes, uint32_t timeout,
                                                     artwork_cancel_fn cancel, void * user_data) {
    (void)priority; (void)bytes; (void)timeout; (void)cancel; (void)user_data;
    return ARTWORK_ACQUIRE_BUSY;
}
void artwork_coordinator_release(artwork_priority_t priority) { (void)priority; }
cover_decode_result_t cover_decode_to_rgb565_ex(const uint8_t * data, uint32_t size,
                                                int width, int height, artwork_priority_t priority,
                                                artwork_cancel_fn cancel, void * user_data,
                                                uint16_t ** pixels) {
    (void)data; (void)size; (void)width; (void)height; (void)priority;
    (void)cancel; (void)user_data; (void)pixels;
    return COVER_DECODE_FAIL_UNSUPPORTED;
}

const lv_font_t * pill_row_resolve_text_size(const char * size) {
    (void)size;
    return &lv_font_montserrat_16;
}

static lv_obj_t * capture_items(const icon_grid_item_t * items, int count, bool list) {
    assert(count >= 0 && count <= HOME_LAYOUT_MAX_TILES);
    captured_count = count;
    captured_list = list;
    memcpy(captured, items, (size_t)count * sizeof(*items));
    lv_obj_t * screen = lv_obj_create(NULL);
    for (int i = 0; i < count; ++i) {
        lv_obj_t * tile = lv_btn_create(screen);
        lv_obj_add_flag(tile, LV_OBJ_FLAG_USER_1);
        lv_obj_add_event_cb(tile, items[i].on_click, LV_EVENT_ALL, items[i].user_data);
    }
    record(list ? "build-list" : "build-grid");
    return screen;
}

lv_obj_t * build_icon_grid_screen(const char * title, lv_event_cb_t back,
                                  const icon_grid_item_t * items, int count,
                                  int32_t scale, bool inside, int32_t gap) {
    (void)title; (void)back; (void)scale; (void)inside; (void)gap;
    return capture_items(items, count, false);
}

lv_obj_t * build_launcher_menu_screen(const char * title, lv_event_cb_t back,
                                      const icon_grid_item_t * items, int count,
                                      int32_t scale, bool inside,
                                      const launcher_menu_layout_t * layout) {
    (void)title; (void)back; (void)scale; (void)inside;
    assert(layout && layout->list_mode);
    return capture_items(items, count, true);
}

void finalize_screen_navigation(lv_obj_t * screen) {
    assert(screen);
    assert(lv_obj_get_style_bg_image_src(screen, 0) != NULL ||
           !home_layout_config.has_background_image);
    finalize_count++;
    record("finalize");
}

static void write_bin(const char * path, bool valid) {
    FILE * file = fopen(path, "wb");
    assert(file);
    lv_image_header_t header = {0};
    if (valid) {
        header.magic = LV_IMAGE_HEADER_MAGIC;
        header.cf = LV_COLOR_FORMAT_RGB565;
        header.w = 480;
        header.h = 800;
        header.stride = 960;
    }
    assert(fwrite(&header, 1, sizeof(header), file) == sizeof(header));
    if (valid) {
        uint16_t pixel = 0x1234;
        for (size_t i = 0; i < (size_t)480 * 800; ++i)
            assert(fwrite(&pixel, sizeof(pixel), 1, file) == 1);
    }
    assert(fclose(file) == 0);
}

static void reset_config(void) {
    memset(&home_layout_config, 0, sizeof(home_layout_config));
    plugin_lookup_count = 0;
}

int main(void) {
    lv_init();
    lv_display_t * display = lv_display_create(480, 800);
    assert(display);
    static uint8_t draw_buffer[480 * 16 * 4];
    lv_display_set_buffers(display, draw_buffer, NULL, sizeof(draw_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    settings_screen = lv_obj_create(NULL);
    stream_media_screen = lv_obj_create(NULL);
    music_dest = lv_obj_create(NULL);
    wireless_dest = lv_obj_create(NULL);
    dac_dest = lv_obj_create(NULL);

    /* The stock fallback remains the same six-item grid and order. */
    reset_config();
    lv_obj_t * screen = harness_build_home_screen();
    assert(!captured_list && captured_count == HOME_LAYOUT_DEFAULT_TILE_COUNT);
    const char * default_labels[] = {"Music", "Stream Media", "Wireless", "Books", "Settings", "More"};
    for (int i = 0; i < captured_count; ++i) assert(strcmp(captured[i].label, default_labels[i]) == 0);
    assert(strcmp(captured[0].icon_asset, "launcher/music.png") == 0);
    assert(strcmp(captured[4].icon_asset, "launcher/sys_set.png") == 0);
    assert(strcmp(captured[0].icon_asset_selected, "launcher/music_s.png") == 0);
    assert(strstr(trace, "build-grid\nfinalize\n") != NULL);
    lv_obj_t * replacement_stream = lv_obj_create(NULL);
    stream_media_screen = replacement_stream;
    lv_obj_send_event(lv_obj_get_child(screen, 1), LV_EVENT_VALUE_CHANGED, NULL);
    assert(last_nav_dest == NULL);
    lv_obj_send_event(lv_obj_get_child(screen, 1), LV_EVENT_CLICKED, NULL);
    assert(last_nav_dest == replacement_stream);
    lv_obj_t * replacement_settings = lv_obj_create(NULL);
    settings_screen = replacement_settings;
    lv_obj_send_event(lv_obj_get_child(screen, 4), LV_EVENT_CLICKED, NULL);
    assert(last_nav_dest == replacement_settings);
    lv_obj_send_event(lv_obj_get_child(screen, 0), LV_EVENT_CLICKED, NULL);
    assert(last_nav_dest == music_dest);
    lv_obj_send_event(lv_obj_get_child(screen, 2), LV_EVENT_CLICKED, NULL);
    assert(last_nav_dest == wireless_dest);
    lv_obj_send_event(lv_obj_get_child(screen, 5), LV_EVENT_CLICKED, NULL);
    assert(last_nav_dest == dac_dest);
    assert(captured[1].on_click && captured[4].on_click);
    lv_obj_delete(screen);

    /* Custom tile order resolves native/plugin entries and skips missing ids. */
    reset_config();
    home_layout_config.configured = true;
    home_layout_config.order_count = 4;
    snprintf(home_layout_config.order[0], sizeof(home_layout_config.order[0]), "settings");
    snprintf(home_layout_config.order[1], sizeof(home_layout_config.order[1]), "plugin.sample");
    snprintf(home_layout_config.order[2], sizeof(home_layout_config.order[2]), "missing.plugin");
    snprintf(home_layout_config.order[3], sizeof(home_layout_config.order[3]), "stream_media");
    home_layout_config.tiles[0].override.has_bg_color = true;
    home_layout_config.tiles[0].override.bg_color = 0x123456;
    snprintf(home_layout_config.tiles[0].key, sizeof(home_layout_config.tiles[0].key), "settings");
    home_layout_config.tile_count = 1;
    screen = harness_build_home_screen();
    assert(captured_count == 3 && plugin_lookup_count == 2);
    assert(strcmp(captured[0].label, "Settings") == 0);
    assert(strcmp(captured[1].label, "Plugin tile") == 0);
    assert(strcmp(captured[2].label, "Stream Media") == 0);
    assert(captured[0].has_bg_color && captured[0].bg_color == 0x123456);
    assert(strcmp(captured[1].icon_asset, "plugin/icon.png") == 0);
    lv_obj_send_event(lv_obj_get_child(screen, 1), LV_EVENT_CLICKED, NULL);
    assert(plugin_click_index == 3);
    lv_obj_delete(screen);

    /* List-mode metadata and an owned RGB565 wallpaper survive until delete. */
    reset_config();
    home_layout_config.configured = true;
    home_layout_config.list_mode = true;
    home_layout_config.order_count = 2;
    snprintf(home_layout_config.order[0], sizeof(home_layout_config.order[0]), "music");
    snprintf(home_layout_config.order[1], sizeof(home_layout_config.order[1]), "settings");
    home_layout_config.tiles[0].override.height = 88;
    snprintf(home_layout_config.tiles[0].key, sizeof(home_layout_config.tiles[0].key), "music");
    home_layout_config.tile_count = 1;
    home_layout_config.has_background_image = true;
    snprintf(home_layout_config.background_image, sizeof(home_layout_config.background_image), "wallpaper.bin");
    write_bin("wallpaper.bin", true);
    int finalizes_before = finalize_count;
    tracked_calloc_ptr = tracked_malloc_ptr = NULL;
    tracked_calloc_freed = tracked_malloc_freed = 0;
    screen = harness_build_home_screen();
    assert(captured_list && captured_count == 2);
    assert(captured[0].has_row_height && captured[0].row_height == 220);
    const lv_image_dsc_t * image = lv_obj_get_style_bg_image_src(screen, 0);
    assert(image && image->header.cf == LV_COLOR_FORMAT_RGB565 && image->header.w == 480);
    assert(tracked_calloc_ptr && tracked_malloc_ptr);
    assert(finalize_count == finalizes_before + 1);
    lv_obj_delete(screen);
    assert(tracked_calloc_freed == 1 && tracked_malloc_freed == 1);

    write_bin("bad.bin", false);
    snprintf(home_layout_config.background_image, sizeof(home_layout_config.background_image), "bad.bin");
    screen = harness_build_home_screen();
    resolved_asset = asset_path("bad.bin");
    assert(lv_obj_get_style_bg_image_src(screen, 0) == resolved_asset);
    lv_obj_delete(screen);
    snprintf(home_layout_config.background_image, sizeof(home_layout_config.background_image), "missing.png");
    screen = harness_build_home_screen();
    resolved_asset = asset_path("missing.png");
    assert(lv_obj_get_style_bg_image_src(screen, 0) == resolved_asset);
    lv_obj_delete(screen);

    lv_obj_delete(settings_screen);
    lv_obj_delete(stream_media_screen);
    lv_obj_delete(music_dest);
    lv_obj_delete(wireless_dest);
    lv_obj_delete(dac_dest);
    lv_display_delete(display);
    printf("home:items=%d;plugin-lookups=%d;finalize=%d;cache-freed=%d/%d\n",
           captured_count, plugin_lookup_count, finalize_count,
           tracked_calloc_freed, tracked_malloc_freed);
    return 0;
}
