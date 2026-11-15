#include "src/ui/application/gui.h"
#include "src/ui/text/fonts/ui_fonts.h"
#include "src/ui/library/library_search.h"
#include "src/ui/text/rendering/gui_text_input.h"
#include "src/ui/widgets/builders/screen_builders.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
static int count;
static const char *name(int i) { (void)i; return ""; }
static lv_obj_t *az(lv_obj_t *s) { (void)s; return NULL; }
static void refresh(void) {}
int main(void) {
    lv_init(); assert(ui_fonts_init());
    assert(lv_sdl_window_create(BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT));
    settings_load(&current_settings); audio_init();
    gui_init(BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT);
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_t *list = build_compact_list_widget(screen, NULL, 0, NULL, NULL, LIST_ROW_WIDTH, false, NULL);
    library_search_config_t config = { .screen = screen, .list = list,
        .name_of = name, .count_ptr = &count, .az_strip_for_screen = az,
        .refresh_now_playing = refresh };
    library_search_register(SEARCH_BINDING_FILES, &config);
    nav_push(screen);
    for (int tier = 0; tier < 3; tier++) {
    assert(fallback_font_apply_size_tier(tier));
    fallback_font_refresh_ui();
    library_search_open(SEARCH_BINDING_FILES);
    lv_obj_update_layout(screen);
    int top = lv_obj_get_y(list), keypad = t9_keypad_get_grid_y();
    printf("Layout: keypad=%d list=%d available=%d row=%d clearance=%d\n", keypad, top,
           lv_obj_get_height(list), (int)ui_list_row_height(), HOME_INDICATOR_CONTENT_INSET);
#ifdef BOARD_TEMPOTEC_V1
    assert(keypad == 154 && HOME_INDICATOR_CONTENT_INSET == 16);
    if (!getenv("QA_MEASURE_ONLY")) assert(keypad - top >= ui_list_row_height() + GUI_ROW_GAP);
#else
    assert(top == STATUS_BAR_CLEARANCE + 80);
#endif
    search_close_if_active_for_screen(screen);
    }
    puts("Inline search layout passed");
}
