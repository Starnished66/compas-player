/* Production-backed LVGL RTL/Arabic smoke and screenshot test. Run with
 * scripts/run_rtl_text_spec.sh after enabling LVGL's Bidi/Arabic options.
 * The included screen builder is production code; no shaping or font lookup
 * behavior is stubbed in this test. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl/lvgl.h"
#include "lvgl/src/misc/lv_bidi.h"
#include "lvgl/src/misc/lv_text_private.h"
#include "lvgl/src/widgets/textarea/lv_textarea.h"
#include "src/core/settings.h"
#include "src/ui/assets.h"
#include "src/ui/fallback_font.h"
#include "src/ui/gui_theme.h"
#include "src/ui/ui_label_text.h"
#include "src/ui/ui_fonts.h"

#if !LV_USE_BIDI || !LV_USE_ARABIC_PERSIAN_CHARS
#error "Run this spec with LV_USE_BIDI and LV_USE_ARABIC_PERSIAN_CHARS enabled"
#endif

#include "../src/ui/screen_builders.c"

extern player_settings_t current_settings;
extern void lv_bidi_process(const char * input, char * output, lv_base_dir_t base_dir);
extern int rtl_text_test_set_album_source(const char * raw_title);
extern bool rtl_text_test_timeline_proxy(const char * text);

enum { DISPLAY_WIDTH = 480, DISPLAY_HEIGHT = 800 };

static void flush_ready(lv_display_t * display, const lv_area_t * area, uint8_t * pixels) {
    LV_UNUSED(area);
    LV_UNUSED(pixels);
    lv_display_flush_ready(display);
}

static void snapshot_screen(lv_obj_t * screen, const char * path) {
    lv_obj_update_layout(screen);
    lv_draw_buf_t * image = lv_snapshot_take(screen, LV_COLOR_FORMAT_RGB888);
    assert(image && image->data);
    FILE * output = fopen(path, "wb");
    assert(output);
    assert(fwrite(image->data, 1, image->data_size, output) == image->data_size);
    assert(fclose(output) == 0);
    printf("SNAPSHOT %s %u %u %u\n", path, image->header.w,
           image->header.h, image->header.stride);
    lv_draw_buf_destroy(image);
}

static void expect_codepoints(const char * actual, const uint32_t * expected, size_t count) {
    uint32_t byte_offset = 0;
    for(size_t i = 0; i < count; i++) {
        uint32_t codepoint = lv_text_encoded_next(actual, &byte_offset);
        if(codepoint != expected[i]) {
            fprintf(stderr, "codepoint %zu was U+%04X, expected U+%04X\n",
                    i, (unsigned)codepoint, (unsigned)expected[i]);
            assert(0);
        }
    }
    assert(lv_text_encoded_next(actual, &byte_offset) == '\0');
}

static void expect_fallback_glyphs(const lv_font_t * font,
                                   const uint32_t * codepoints, size_t count) {
    for(size_t i = 0; i < count; i++) {
        lv_font_glyph_dsc_t glyph;
        assert(lv_font_get_glyph_dsc(font, &glyph, codepoints[i], 0));
        assert(!glyph.is_placeholder);
        assert(glyph.resolved_font);
        assert(glyph.box_w > 0 && glyph.box_h > 0);
    }
}

static lv_obj_t * make_text_label(lv_obj_t * parent, const char * text,
                                  const lv_font_t * font, int32_t y) {
    lv_obj_t * label = lv_label_create(parent);
    assert(label);
    lv_obj_set_width(label, DISPLAY_WIDTH - 48);
    lv_obj_set_height(label, 54);
    lv_obj_set_pos(label, 24, y);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_label_set_text(label, text);
    return label;
}

static lv_base_dir_t resolved_label_dir(lv_obj_t * label) {
    lv_base_dir_t base_dir = lv_obj_get_style_base_dir(label, LV_PART_MAIN);
    lv_text_align_t align = LV_TEXT_ALIGN_AUTO;
    lv_bidi_calculate_align(&align, &base_dir, lv_label_get_text(label));
    return base_dir;
}

static void test_base_dir_inheritance(lv_obj_t * screen) {
    assert(lv_obj_get_style_base_dir(screen, LV_PART_MAIN) == LV_BASE_DIR_AUTO);
    lv_obj_t * ltr_parent = lv_obj_create(screen);
    lv_obj_set_style_base_dir(ltr_parent, LV_BASE_DIR_LTR, LV_PART_MAIN);
    lv_obj_t * ltr_child = lv_label_create(ltr_parent);
    lv_label_set_text(ltr_child, "Parent LTR");
    assert(lv_obj_get_style_base_dir(ltr_child, LV_PART_MAIN) == LV_BASE_DIR_LTR);

    lv_obj_t * rtl_parent = lv_obj_create(screen);
    lv_obj_set_style_base_dir(rtl_parent, LV_BASE_DIR_RTL, LV_PART_MAIN);
    lv_obj_t * rtl_child = lv_label_create(rtl_parent);
    lv_label_set_text(rtl_child, "Parent RTL");
    assert(lv_obj_get_style_base_dir(rtl_child, LV_PART_MAIN) == LV_BASE_DIR_RTL);
}

static void test_label_pipeline(lv_obj_t * screen) {
    static const uint32_t word_forms[] = { 0xFEDB, 0xFEE0, 0xFEE4, 0xFE94 };
    lv_obj_t * word = make_text_label(screen, "كلمة", &app_font_22, 64);
    expect_codepoints(lv_label_get_text(word), word_forms,
                      sizeof(word_forms) / sizeof(word_forms[0]));
    assert(lv_obj_get_style_base_dir(word, LV_PART_MAIN) == LV_BASE_DIR_AUTO);
    assert(ui_label_text_matches(word, "كلمة"));
    assert(!ui_label_text_matches(word, "كلمه"));
    assert(resolved_label_dir(word) == LV_BASE_DIR_RTL);
    expect_fallback_glyphs(&app_font_22, word_forms,
                           sizeof(word_forms) / sizeof(word_forms[0]));

    lv_obj_t * mixed_rtl = make_text_label(screen, "مرحبا Compas 123", &app_font_22, 132);
    const char * mixed_text = lv_label_get_text(mixed_rtl);
    assert(strstr(mixed_text, "Compas 123") != NULL);
    assert(lv_obj_get_style_base_dir(mixed_rtl, LV_PART_MAIN) == LV_BASE_DIR_AUTO);
    assert(resolved_label_dir(mixed_rtl) == LV_BASE_DIR_RTL);
    assert(ui_label_text_matches(mixed_rtl, "مرحبا Compas 123"));
    assert(!ui_label_text_matches(mixed_rtl, "مرحبا Compas 124"));
    char mixed_visual[128];
    lv_bidi_process(mixed_text, mixed_visual, resolved_label_dir(mixed_rtl));
    assert(strstr(mixed_visual, "Compas 123") != NULL);

    static const char * const latin_time = "Track 07  12:34";
    lv_obj_t * latin = make_text_label(screen, latin_time, &app_font_22, 200);
    assert(strcmp(lv_label_get_text(latin), latin_time) == 0);
    assert(ui_label_text_matches(latin, latin_time));
    assert(!ui_label_text_matches(latin, "Track 08  12:34"));
    assert(lv_obj_get_style_base_dir(latin, LV_PART_MAIN) == LV_BASE_DIR_AUTO);
    assert(resolved_label_dir(latin) == LV_BASE_DIR_LTR);

    static const char all_arabic_digits[] = "٠١٢٣٤٥٦٧٨٩";
    static const char all_persian_digits[] = "۰۱۲۳۴۵۶۷۸۹";
    static const char extended_arabic_letter[] = "\u06EF";
    static const char numeral_runs[] = "مرحبا 123 ٠١٢ ۰۱۲ ٠١٢٣٤٥٦٧٨٩ ۰۱۲۳۴۵۶۷۸۹ \u06EF";
    lv_obj_t * numerals = make_text_label(screen, numeral_runs, &app_font_22, 268);
    const char * numeral_text = lv_label_get_text(numerals);
    char numeral_visual[128];
    assert(lv_obj_get_style_base_dir(numerals, LV_PART_MAIN) == LV_BASE_DIR_AUTO);
    assert(resolved_label_dir(numerals) == LV_BASE_DIR_RTL);
    assert(strstr(numeral_text, all_arabic_digits));
    assert(strstr(numeral_text, all_persian_digits));
    assert(strstr(numeral_text, extended_arabic_letter));
    lv_bidi_process(numeral_text, numeral_visual, resolved_label_dir(numerals));
    assert(strstr(numeral_visual, "123"));
    assert(strstr(numeral_visual, "٠١٢"));
    assert(strstr(numeral_visual, "۰۱۲"));
    assert(strstr(numeral_visual, all_arabic_digits));
    assert(strstr(numeral_visual, all_persian_digits));
    assert(strstr(numeral_visual, extended_arabic_letter));
    assert(ui_label_text_matches(numerals, numeral_runs));
    for(uint32_t digit = 0x0660; digit <= 0x0669; digit++)
        expect_fallback_glyphs(&app_font_22, &digit, 1);
    for(uint32_t digit = 0x06F0; digit <= 0x06F9; digit++)
        expect_fallback_glyphs(&app_font_22, &digit, 1);
    static const uint32_t extended_arabic_letter_cp[] = { 0x06EF };
    expect_fallback_glyphs(&app_font_22, extended_arabic_letter_cp, 1);

    static const char ltr_leading_mixed[] = "Track كلمة ١٢٣ ۱۲۳ ٠١٢٣٤٥٦٧٨٩ ۰۱۲۳۴۵۶۷۸۹";
    lv_obj_t * ltr_mixed = make_text_label(screen, ltr_leading_mixed, &app_font_22, 336);
    const char * ltr_mixed_text = lv_label_get_text(ltr_mixed);
    assert(lv_obj_get_style_base_dir(ltr_mixed, LV_PART_MAIN) == LV_BASE_DIR_AUTO);
    assert(resolved_label_dir(ltr_mixed) == LV_BASE_DIR_LTR);
    char ltr_mixed_visual[128];
    lv_bidi_process(ltr_mixed_text, ltr_mixed_visual, resolved_label_dir(ltr_mixed));
    assert(strstr(ltr_mixed_visual, "Track"));
    assert(strstr(ltr_mixed_visual, "١٢٣"));
    assert(strstr(ltr_mixed_visual, "۱۲۳"));
    assert(strstr(ltr_mixed_visual, all_arabic_digits));
    assert(strstr(ltr_mixed_visual, all_persian_digits));
    assert(ui_label_text_matches(ltr_mixed, ltr_leading_mixed));

    printf("TEXT word=forms-joined RTL; mixed=RTL/LTR runs retained; time=unchanged LTR; ASCII/Arabic/Persian numeral runs retained\n");
}

static void check_insert_event(lv_event_t * event);

static void test_textarea_roundtrip(lv_obj_t * parent) {
    lv_obj_t * textarea = lv_textarea_create(parent);
    assert(textarea);
    static const char source[] = "كلمة";
    lv_textarea_set_text(textarea, source);
    assert(strcmp(lv_textarea_get_text(textarea), source) == 0);
    const char * same_text = lv_textarea_get_text(textarea);
    lv_textarea_set_text(textarea, same_text);
    assert(strcmp(lv_textarea_get_text(textarea), source) == 0);
    lv_textarea_set_max_length(textarea, 4);
    same_text = lv_textarea_get_text(textarea);
    lv_textarea_set_text(textarea, same_text);
    assert(strcmp(lv_textarea_get_text(textarea), source) == 0);
    lv_textarea_set_max_length(textarea, 0);
    same_text = lv_textarea_get_text(textarea) + 2; /* Interior UTF-8 boundary after the first Arabic character. */
    lv_textarea_set_text(textarea, same_text);
    assert(strcmp(lv_textarea_get_text(textarea), "لمة") == 0);
    lv_textarea_set_text(textarea, source);
    lv_textarea_set_cursor_pos(textarea, LV_TEXTAREA_CURSOR_LAST);
    same_text = lv_textarea_get_text(textarea);
    lv_textarea_add_text(textarea, same_text);
    assert(strcmp(lv_textarea_get_text(textarea), "كلمةكلمة") == 0);
    assert(lv_textarea_get_cursor_pos(textarea) == 8);
    lv_textarea_set_text(textarea, source);
    lv_textarea_set_cursor_pos(textarea, 1);
    same_text = lv_textarea_get_text(textarea) + 2;
    lv_textarea_add_text(textarea, same_text);
    assert(strcmp(lv_textarea_get_text(textarea), "كلمةلمة") == 0);

    lv_textarea_set_accepted_chars(textarea, "كلمة");
    lv_textarea_set_max_length(textarea, 4);
    lv_textarea_set_text(textarea, "كلمةA");
    assert(strcmp(lv_textarea_get_text(textarea), source) == 0);
    assert(lv_textarea_get_cursor_pos(textarea) == 4);
    lv_textarea_set_accepted_chars(textarea, NULL);
    lv_textarea_set_max_length(textarea, 0);
    lv_textarea_set_text(textarea, source);

    static const char pasted[] = " 123 العربية";
    static const char inserted_text[] = "كلمة 123 العربية";
    lv_obj_add_event_cb(textarea, check_insert_event, LV_EVENT_INSERT, (void *)pasted);
    lv_textarea_set_cursor_pos(textarea, LV_TEXTAREA_CURSOR_LAST);
    lv_textarea_add_text(textarea, pasted);
    assert(lv_obj_remove_event_cb(textarea, check_insert_event) == 1);
    assert(strcmp(lv_textarea_get_text(textarea), inserted_text) == 0);
    assert(lv_textarea_get_cursor_pos(textarea) == 16);
    lv_textarea_delete_char(textarea);
    assert(strcmp(lv_textarea_get_text(textarea), "كلمة 123 العربي") == 0);

    /* Lam-alef's joined presentation is kept as two glyphs in text areas so
     * the cursor can address the source characters independently. */
    lv_textarea_set_text(textarea, "لا");
    assert(strcmp(lv_textarea_get_text(textarea), "لا") == 0);
    assert(lv_textarea_get_cursor_pos(textarea) == 2);
    assert(lv_text_get_encoded_length(lv_label_get_text(lv_textarea_get_label(textarea))) == 2);
    lv_textarea_set_cursor_pos(textarea, 1);
    assert(lv_textarea_get_cursor_pos(textarea) == 1);
    lv_textarea_add_text(textarea, "ب");
    assert(strcmp(lv_textarea_get_text(textarea), "لبا") == 0);
    assert(lv_textarea_get_cursor_pos(textarea) == 2);
    lv_textarea_delete_char(textarea);
    assert(strcmp(lv_textarea_get_text(textarea), "لا") == 0);
    assert(lv_textarea_get_cursor_pos(textarea) == 1);

    lv_textarea_set_password_mode(textarea, true);
    assert(strcmp(lv_textarea_get_text(textarea), "لا") == 0);
    lv_textarea_set_cursor_pos(textarea, LV_TEXTAREA_CURSOR_LAST);
    assert(lv_textarea_get_cursor_pos(textarea) == 2);
    lv_textarea_set_accepted_chars(textarea, "كلمة");
    lv_textarea_set_max_length(textarea, 4);
    lv_textarea_set_text(textarea, "كلمةA");
    assert(strcmp(lv_textarea_get_text(textarea), source) == 0);
    assert(lv_textarea_get_cursor_pos(textarea) == 4);
    lv_textarea_set_accepted_chars(textarea, NULL);
    lv_textarea_set_max_length(textarea, 0);
    lv_textarea_set_text(textarea, "لا");
    lv_textarea_set_cursor_pos(textarea, LV_TEXTAREA_CURSOR_LAST);
    lv_textarea_add_text(textarea, "A");
    assert(strcmp(lv_textarea_get_text(textarea), "لاA") == 0);
    lv_textarea_set_password_mode(textarea, false);
    assert(strcmp(lv_textarea_get_text(textarea), "لاA") == 0);
}

static lv_obj_t * make_library_screen(lv_obj_t ** out_list) {
    static const compact_list_item_t items[] = {
        { .label = "كلمة", .subtitle = "مرحبا Compas 123" },
        { .label = "أغنية عربية", .subtitle = "Fairuz — 03:45" },
    };
    return build_compact_list_screen("المكتبة", NULL, items, 2,
                                     NULL, NULL, out_list, NULL,
                                     LIST_ROW_WIDTH_WIDE, false, NULL);
}

static lv_obj_t * make_playlist_screen(lv_obj_t ** out_list) {
    static const compact_list_item_t items[] = {
        { .label = "فيروز — Kifak Inta", .subtitle = "كلمة · 03:45" },
        { .label = "Track 07", .subtitle = "عنوان الأغنية — 12:34" },
    };
    return build_compact_list_screen("قائمة التشغيل", NULL, items, 2,
                                     NULL, NULL, out_list, NULL,
                                     LIST_ROW_WIDTH_WIDE, false, NULL);
}

static void check_insert_event(lv_event_t * event) {
    const char * expected = lv_event_get_user_data(event);
    const char * inserted = lv_event_get_param(event);
    assert(expected && inserted && strcmp(inserted, expected) == 0);
}

static const compact_list_virtual_data_t * list_data(lv_obj_t * list) {
    const compact_list_virtual_data_t * data = lv_obj_get_user_data(list);
    assert(data && data->item_count == 2);
    return data;
}

static lv_obj_t * row_for_logical_index(const compact_list_virtual_data_t * data, int logical_index) {
    for(int slot = 0; slot < COMPACT_LIST_POOL_SIZE; slot++)
        if(data->row_logical_index[slot] == logical_index) return data->rows[slot];
    assert(!"expected row is not in the visible compact-list pool");
    return NULL;
}

int main(void) {
    player_settings_t loaded;
    settings_load(&loaded);
    current_settings = loaded;

    lv_init();
    assets_init();
    assert(ui_fonts_init());
    lv_display_t * display = lv_display_create(DISPLAY_WIDTH, DISPLAY_HEIGHT);
    assert(display);
    static uint8_t display_buffer[DISPLAY_WIDTH * 24 * 4];
    lv_display_set_buffers(display, display_buffer, NULL, sizeof(display_buffer),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush_ready);

    gui_theme_init();
    fallback_font_load_now();
    assert(rtl_text_test_timeline_proxy("Track 07  12:34"));
    assert(!rtl_text_test_timeline_proxy("مرحبا"));
    assert(!rtl_text_test_timeline_proxy("مرحبا Compas 123"));
    lv_obj_t * text_test_screen = lv_obj_create(NULL);
    test_base_dir_inheritance(text_test_screen);
    test_label_pipeline(text_test_screen);

    lv_obj_t * library_list = NULL;
    lv_obj_t * library_screen = make_library_screen(&library_list);
    const compact_list_virtual_data_t * library_data = list_data(library_list);
    lv_obj_t * library_first_row = row_for_logical_index(library_data, 0);
    assert(lv_obj_get_style_base_dir(library_first_row, LV_PART_MAIN) == LV_BASE_DIR_AUTO);
    assert(lv_obj_get_style_base_dir(library_data->subtitle_labels[0], LV_PART_MAIN) == LV_BASE_DIR_AUTO);
    assert(resolved_label_dir(library_first_row) == LV_BASE_DIR_RTL);
    assert(resolved_label_dir(library_data->subtitle_labels[0]) == LV_BASE_DIR_RTL);
    static const uint32_t word_forms[] = { 0xFEDB, 0xFEE0, 0xFEE4, 0xFE94 };
    expect_codepoints(lv_label_get_text(library_first_row), word_forms,
                      sizeof(word_forms) / sizeof(word_forms[0]));
    assert(strstr(lv_label_get_text(library_data->subtitle_labels[0]), "Compas 123") != NULL);

    lv_screen_load(library_screen);
    lv_refr_now(display);
    snapshot_screen(library_screen, "screenshots/01-arabic-library.rgb");

    lv_obj_t * playlist_list = NULL;
    lv_obj_t * playlist_screen = make_playlist_screen(&playlist_list);
    const compact_list_virtual_data_t * playlist_data = list_data(playlist_list);
    lv_obj_t * playlist_first_row = row_for_logical_index(playlist_data, 0);
    assert(lv_obj_get_style_base_dir(playlist_first_row, LV_PART_MAIN) == LV_BASE_DIR_AUTO);
    assert(lv_obj_get_style_base_dir(playlist_data->subtitle_labels[0], LV_PART_MAIN) == LV_BASE_DIR_AUTO);
    assert(resolved_label_dir(playlist_first_row) == LV_BASE_DIR_RTL);
    assert(resolved_label_dir(playlist_data->subtitle_labels[0]) == LV_BASE_DIR_RTL);
    char playlist_visual[128];
    lv_bidi_process(lv_label_get_text(playlist_first_row), playlist_visual, resolved_label_dir(playlist_first_row));
    assert(strstr(playlist_visual, "Kifak Inta"));
    lv_bidi_process(lv_label_get_text(playlist_data->subtitle_labels[0]), playlist_visual,
                    resolved_label_dir(playlist_data->subtitle_labels[0]));
    assert(strstr(playlist_visual, "03:45"));

    lv_screen_load(playlist_screen);
    lv_refr_now(display);
    snapshot_screen(playlist_screen, "screenshots/02-mixed-playlist.rgb");

    /* Run the input-widget source-preservation check after capturing the
     * display screenshots so its failure still leaves visual evidence. */
    test_textarea_roundtrip(text_test_screen);
    assert(rtl_text_test_set_album_source("ألبوم عربي"));

    puts("PASS: LVGL Arabic shaping, bidi autodetection, fallback glyphs, native library/playlist rows and Latin time text");
    return 0;
}
