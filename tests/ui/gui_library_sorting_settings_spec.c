/* Library sorting settings callbacks source-sliced from gui_settings.c. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <lvgl/lvgl.h>

#include "../../src/core/persistence/settings.h"
#include "../../src/library/metadata/db/metadata_db.h"
#include "../../src/ui/library/browse/gui_library.h"
#include "../../src/ui/appearance/gui_theme.h"
#include "../../src/ui/shell/gui_navigation.h"
#include "../../src/ui/widgets/builders/screen_builders.h"
#include "../../src/ui/text/i18n/i18n.h"
#include "../../src/ui/application/gui.h"

player_settings_t current_settings;
static lv_display_t *display;
static lv_obj_t *list_root;
static lv_obj_t *sorting_screen;
static lv_obj_t *option_rows[16];
static const char *option_labels[16];
static lv_event_cb_t option_callbacks[16];
static void *option_user_data[16];
static int option_count, save_count, apply_count, error_count, nav_count;
static gui_library_sort_scope_t last_scope;
static bool years_available = true;
static char effects[128];
static lv_style_t outline_style;

static void record(const char *s) {
    size_t n = strlen(effects);
    snprintf(effects + n, sizeof(effects) - n, "%s;", s);
}
static void reset_effects(void) {
    effects[0] = '\0'; save_count = apply_count = error_count = nav_count = 0;
}
static void assert_effects(const char *expected) { assert(strcmp(effects, expected) == 0); }

const char *i18n_tr(const char *s) { return s; }
uint32_t gui_anim_ms(uint32_t ms) { return ms; }
static bool test_font_glyph(const lv_font_t *font, lv_font_glyph_dsc_t *dsc,
                            uint32_t letter, uint32_t next_letter) {
    LV_UNUSED(letter); LV_UNUSED(next_letter); memset(dsc, 0, sizeof(*dsc));
    dsc->resolved_font = font; dsc->adv_w = 12; dsc->box_w = 10; dsc->box_h = 16; return true;
}
static const void *test_font_bitmap(lv_font_glyph_dsc_t *dsc, lv_draw_buf_t *draw_buf) {
    LV_UNUSED(dsc); LV_UNUSED(draw_buf); return NULL;
}
lv_font_t lv_font_montserrat_16 = { .get_glyph_dsc = test_font_glyph,
    .get_glyph_bitmap = test_font_bitmap, .line_height = 20 };
void settings_save_async(const player_settings_t *s) { assert(s == &current_settings); save_count++; record("save"); }
bool metadata_db_album_sort_available(metadata_db_album_sort_t sort) {
    return sort != METADATA_DB_ALBUM_SORT_YEAR || years_available;
}
void gui_library_apply_sorting(gui_library_sort_scope_t scope) { last_scope = scope; apply_count++; record("apply"); }
void show_error_toast(const char *s) { assert(strcmp(s, "Update Music Database to enable this album order") == 0); error_count++; record("error"); }
void nav_push(lv_obj_t *screen) { assert(screen); nav_count++; record("nav-push"); }
lv_style_t *gui_theme_accent_outline_style(void) { return &outline_style; }
lv_obj_t *build_subsonic_list_screen(const char *title, lv_obj_t **title_label, lv_obj_t **list) {
    lv_obj_t *screen = lv_obj_create(lv_screen_active());
    *title_label = lv_label_create(screen); lv_label_set_text(*title_label, title);
    *list = lv_obj_create(screen); list_root = *list; return screen;
}
lv_obj_t *add_section_header(lv_obj_t *parent, const char *title) {
    lv_obj_t *label = lv_label_create(parent); lv_label_set_text(label, title); return label;
}
lv_obj_t *add_pill_option_row(lv_obj_t *parent, const char *label, bool selected,
                              lv_event_cb_t callback, void *user_data) {
    assert(option_count < 16);
    int i = (int)(intptr_t)user_data;
    assert(i >= 0 && i < 16 && option_rows[i] == NULL);
    option_count++;
    option_labels[i] = label; option_callbacks[i] = callback; option_user_data[i] = user_data;
    option_rows[i] = lv_obj_create(parent);
    lv_obj_t *text = lv_label_create(option_rows[i]); lv_label_set_text(text, label);
    lv_obj_add_event_cb(option_rows[i], callback, LV_EVENT_CLICKED, user_data);
    lv_obj_set_style_border_width(option_rows[i], selected ? BOARD_SCALE_PX(3) : 0, LV_PART_MAIN);
    return option_rows[i];
}

#include "library_sorting_settings_fixture.inc"

static void click_option(int option) {
    assert(option >= 0 && option < option_count);
    lv_obj_send_event(option_rows[option], LV_EVENT_CLICKED, NULL);
}
static int find_label(const char *label) {
    for (int i = 0; i < option_count; i++) if (strcmp(option_labels[i], label) == 0) return i;
    return -1;
}
static void assert_selected(int file, int albums, int artists, int album_artists, int global) {
    int expected[] = { file, albums, artists, album_artists, global };
    for (int i = 0; i < option_count; i++) {
        bool selected = false;
        for (size_t j = 0; j < sizeof(expected) / sizeof(expected[0]); j++) selected |= expected[j] == i;
        assert((lv_obj_get_style_border_width(option_rows[i], LV_PART_MAIN) != 0) == selected);
    }
}
static void test_screen_options_and_selection(void) {
    current_settings.file_sort_mode = 0;
    current_settings.album_sort_mode = METADATA_DB_ALBUM_SORT_NAME;
    current_settings.artist_albums_sort_mode = METADATA_DB_ALBUM_SORT_NAME;
    current_settings.album_artist_albums_sort_mode = METADATA_DB_ALBUM_SORT_NAME;
    option_count = 0;
    lv_obj_t *open_row = lv_obj_create(lv_screen_active());
    lv_obj_add_event_cb(open_row, sorting_row_cb, LV_EVENT_CLICKED, NULL);
    reset_effects(); lv_obj_send_event(open_row, LV_EVENT_CLICKED, NULL);
    sorting_screen = settings_sorting_screen; assert(sorting_screen && list_root);
    assert(nav_count == 1 && strstr(effects, "nav-push;") != NULL);
    assert(option_count == 12);
    assert(strcmp(option_labels[0], "Name (A–Z)") == 0);
    assert(strcmp(option_labels[1], "Newest Modified") == 0);
    assert(strcmp(option_labels[2], "Name (A–Z)") == 0);
    assert(strcmp(option_labels[3], "Recently Added") == 0);
    assert(strcmp(option_labels[4], "Release Year (oldest first)") == 0);
    assert(strcmp(option_labels[5], "Name (A–Z)") == 0);
    assert(strcmp(option_labels[8], "Name (A–Z)") == 0);
    assert(strcmp(option_labels[11], "Album Artist (A–Z)") == 0);
    assert_selected(0, 2, 5, 8, -1);

    reset_effects(); click_option(1);
    assert(current_settings.file_sort_mode == 1 && last_scope == GUI_LIBRARY_SORT_FILES);
    assert(save_count == 1 && apply_count == 1); assert_effects("save;apply;"); assert_selected(1, 2, 5, 8, -1);
    reset_effects(); click_option(0);
    assert(current_settings.file_sort_mode == 0 && last_scope == GUI_LIBRARY_SORT_FILES);
    assert(save_count == 1 && apply_count == 1); assert_effects("save;apply;"); assert_selected(0, 2, 5, 8, -1);
    reset_effects(); click_option(0);
    assert(save_count == 0 && apply_count == 0 && error_count == 0); assert_effects("");

    reset_effects(); click_option(3);
    assert(current_settings.album_sort_mode == METADATA_DB_ALBUM_SORT_RECENT);
    assert(last_scope == GUI_LIBRARY_SORT_ALBUMS && save_count == 1 && apply_count == 1);
    assert_effects("save;apply;"); assert_selected(0, 3, 5, 8, -1);
    reset_effects(); click_option(2);
    assert(current_settings.album_sort_mode == METADATA_DB_ALBUM_SORT_NAME);
    assert(last_scope == GUI_LIBRARY_SORT_ALBUMS && save_count == 1 && apply_count == 1);
    assert_effects("save;apply;"); assert_selected(0, 2, 5, 8, -1);
    reset_effects(); lv_obj_send_event(open_row, LV_EVENT_CLICKED, NULL);
    assert(nav_count == 1 && last_scope == GUI_LIBRARY_SORT_ALBUMS);
    assert_selected(0, 2, 5, 8, -1);
    lv_obj_delete(open_row);

    reset_effects(); click_option(6);
    assert(current_settings.artist_albums_sort_mode == METADATA_DB_ALBUM_SORT_NAME + 1);
    assert(last_scope == GUI_LIBRARY_SORT_ARTIST_ALBUMS && save_count == 1 && apply_count == 1);
    assert_effects("save;apply;"); assert_selected(0, 2, 6, 8, -1);
    reset_effects(); click_option(5);
    assert(current_settings.artist_albums_sort_mode == METADATA_DB_ALBUM_SORT_NAME);
    assert(last_scope == GUI_LIBRARY_SORT_ARTIST_ALBUMS && save_count == 1 && apply_count == 1);
    assert_effects("save;apply;"); assert_selected(0, 2, 5, 8, -1);

    reset_effects(); click_option(9);
    assert(current_settings.album_artist_albums_sort_mode == METADATA_DB_ALBUM_SORT_NAME + 1);
    assert(last_scope == GUI_LIBRARY_SORT_ALBUM_ARTIST_ALBUMS && save_count == 1 && apply_count == 1);
    assert_effects("save;apply;"); assert_selected(0, 2, 5, 9, -1);
    reset_effects(); click_option(8);
    assert(current_settings.album_artist_albums_sort_mode == METADATA_DB_ALBUM_SORT_NAME);
    assert(last_scope == GUI_LIBRARY_SORT_ALBUM_ARTIST_ALBUMS && save_count == 1 && apply_count == 1);
    assert_effects("save;apply;"); assert_selected(0, 2, 5, 8, -1);

    reset_effects(); click_option(11);
    assert(current_settings.album_sort_mode == METADATA_DB_ALBUM_SORT_ALBUM_ARTIST_YEAR);
    assert(last_scope == GUI_LIBRARY_SORT_ALBUMS && save_count == 1 && apply_count == 1);
    assert_effects("save;apply;"); assert_selected(0, -1, 5, 8, 11);
}

static void assert_enabled_year_choice(int option, gui_library_sort_scope_t scope,
                                       int selected_albums, int selected_artists,
                                       int selected_album_artists) {
    current_settings.album_sort_mode = METADATA_DB_ALBUM_SORT_NAME;
    current_settings.artist_albums_sort_mode = METADATA_DB_ALBUM_SORT_NAME;
    current_settings.album_artist_albums_sort_mode = METADATA_DB_ALBUM_SORT_NAME;
    refresh_sorting_options();
    reset_effects(); click_option(option);
    assert(save_count == 1 && apply_count == 1 && error_count == 0);
    assert(last_scope == scope); assert_effects("save;apply;");
    assert(current_settings.album_sort_mode == (option == 4 ? METADATA_DB_ALBUM_SORT_YEAR : METADATA_DB_ALBUM_SORT_NAME));
    assert(current_settings.artist_albums_sort_mode == (option == 7 ? METADATA_DB_ALBUM_SORT_YEAR : METADATA_DB_ALBUM_SORT_NAME));
    assert(current_settings.album_artist_albums_sort_mode == (option == 10 ? METADATA_DB_ALBUM_SORT_YEAR : METADATA_DB_ALBUM_SORT_NAME));
    assert_selected(0, selected_albums, selected_artists, selected_album_artists, -1);
    if (option == 4) current_settings.album_sort_mode = METADATA_DB_ALBUM_SORT_NAME;
    if (option == 7) current_settings.artist_albums_sort_mode = METADATA_DB_ALBUM_SORT_NAME;
    if (option == 10) current_settings.album_artist_albums_sort_mode = METADATA_DB_ALBUM_SORT_NAME;
    refresh_sorting_options();
}

static void test_invalid_and_unavailable_choices(void) {
    years_available = false;
    current_settings.album_sort_mode = METADATA_DB_ALBUM_SORT_YEAR;
    current_settings.artist_albums_sort_mode = METADATA_DB_ALBUM_SORT_ALBUM_ARTIST_YEAR;
    current_settings.album_artist_albums_sort_mode = METADATA_DB_ALBUM_SORT_ALBUM_ARTIST_YEAR;
    refresh_sorting_options();
    assert_selected(0, 2, 5, 8, -1); /* unavailable and context-invalid saved values display as Name */
    current_settings.album_sort_mode = METADATA_DB_ALBUM_SORT_NAME;
    current_settings.artist_albums_sort_mode = METADATA_DB_ALBUM_SORT_NAME;
    current_settings.album_artist_albums_sort_mode = METADATA_DB_ALBUM_SORT_NAME;
    refresh_sorting_options();
    years_available = true;
    assert_enabled_year_choice(4, GUI_LIBRARY_SORT_ALBUMS, 4, 5, 8);
    assert_enabled_year_choice(7, GUI_LIBRARY_SORT_ARTIST_ALBUMS, 2, 7, 8);
    assert_enabled_year_choice(10, GUI_LIBRARY_SORT_ALBUM_ARTIST_ALBUMS, 2, 5, 10);
    years_available = false;
    current_settings.album_sort_mode = METADATA_DB_ALBUM_SORT_NAME;
    current_settings.artist_albums_sort_mode = METADATA_DB_ALBUM_SORT_NAME;
    current_settings.album_artist_albums_sort_mode = METADATA_DB_ALBUM_SORT_NAME;
    refresh_sorting_options();
    int saved_album_sort = current_settings.album_sort_mode;
    int saved_artist_sort = current_settings.artist_albums_sort_mode;
    int saved_album_artist_sort = current_settings.album_artist_albums_sort_mode;
    const int unavailable_year_options[] = { 4, 7, 10 };
    for (size_t i = 0; i < sizeof(unavailable_year_options) / sizeof(unavailable_year_options[0]); i++) {
        reset_effects(); click_option(unavailable_year_options[i]);
        assert(current_settings.album_sort_mode == saved_album_sort);
        assert(current_settings.artist_albums_sort_mode == saved_artist_sort);
        assert(current_settings.album_artist_albums_sort_mode == saved_album_artist_sort);
        assert(save_count == 0 && apply_count == 0 && error_count == 1);
        assert_effects("error;"); assert_selected(0, 2, 5, 8, -1);
    }

    lv_obj_t *bad = lv_obj_create(list_root);
    lv_obj_add_event_cb(bad, sorting_option_cb, LV_EVENT_CLICKED, (void *)(intptr_t)99);
    reset_effects(); lv_obj_send_event(bad, LV_EVENT_CLICKED, NULL);
    assert(save_count == 0 && apply_count == 0 && error_count == 0); assert_effects("");
    lv_obj_delete(bad);

    reset_effects(); lv_obj_send_event(option_rows[4], LV_EVENT_RELEASED, NULL);
    assert(save_count == 0 && apply_count == 0 && error_count == 0); assert_effects("");
    years_available = true;
}

int main(void) {
    lv_init(); display = lv_display_create(240, 320); assert(display);
    static uint8_t buffer[240 * 16 * 4];
    lv_display_set_buffers(display, buffer, NULL, sizeof(buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    test_screen_options_and_selection();
    test_invalid_and_unavailable_choices();
    puts("Library sorting settings: exposed modes, selection scopes, save/apply ordering, and guarded choices passed");
    if (sorting_screen) lv_obj_delete(sorting_screen);
    lv_display_delete(display); lv_deinit(); return 0;
}
