/* Exercise the real pairing prompt against a host fake agent. */
#undef HOST_BUILD
#include "src/network/bluetooth/control/bt_pairing_agent.h"
#define HOST_BUILD 1
#include "src/ui/network/gui_network_pairing.c"
#include "../gui_books.c"
#include "src/ui/text/fonts/ui_fonts.h"
#include "src/ui/text/rendering/gui_text_input.h"
#include "src/ui/shell/gui_navigation.h"
#include "src/audio/engine/audio.h"
#include <assert.h>

static bool pending_pin;
static int pop_requests;
void __wrap_nav_pop(void) { pop_requests++; }
extern void poll_bt_pairing(void);
bool bt_pairing_agent_pending_pin(char *name, size_t size, uint32_t *id) {
    snprintf(name, size, "Test device");
    *id = 42;
    return pending_pin;
}

void bt_pairing_agent_answer_pin(uint32_t id, const char *pin) { (void)id; (void)pin; }
void bt_pairing_agent_answer_confirmation(uint32_t id, bool accept) { (void)id; (void)accept; }
bool bt_pairing_agent_pending_confirmation(char *name, size_t size, uint32_t *key, uint32_t *id) {
    (void)name; (void)size; (void)key; (void)id; return false;
}
bool bt_pairing_agent_display_code(char *name, size_t size, char *code, size_t code_size, uint32_t *id) {
    (void)name; (void)size; (void)code; (void)code_size; (void)id; return false;
}

static void check_pin_ownership(void) {
    pending_pin = true;
    poll_bt_pairing();
    uint32_t pin_generation = gui_text_input_generation();
    nav_pop_immediate();
    show_text_entry("Other editor", "", false, false, NULL, NULL);
    assert(gui_text_input_generation() != pin_generation);
    pending_pin = false;
    poll_bt_pairing();
    assert(lv_screen_active() == gui_text_input_get_screen() && pop_requests == 0);
    nav_pop_immediate();
    pending_pin = true;
    poll_bt_pairing();
    assert(lv_screen_active() == gui_text_input_get_screen());
    pending_pin = false;
    poll_bt_pairing();
    assert(pop_requests == 1);
    nav_pop_immediate();
}

static void check_books(void) {
    FILE *file = fopen("book-one.txt", "w");
    assert(file);
    for (int i = 0; i < 1000; i++) fprintf(file, "Reading line %d\n", i);
    assert(fclose(file) == 0);
    file = fopen("book-two.txt", "w");
    assert(file && fputs("A different book", file) >= 0 && fclose(file) == 0);
    open_text_reader("book-one.txt");
    lv_obj_update_layout(text_reader_screen);
    lv_obj_scroll_to_y(text_reader_scroll, 700, LV_ANIM_OFF);
    book_position_save(NULL);
    assert(current_settings.book_positions[0].scroll == 700);
    nav_pop_immediate();
    open_text_reader("book-two.txt");
    assert(lv_obj_get_scroll_y(text_reader_scroll) == 0);
    nav_pop_immediate();
    settings_save_drain();
    assert(settings_load(&current_settings));
    open_text_reader("book-one.txt");
    assert(lv_obj_get_scroll_y(text_reader_scroll) == 700);
    nav_pop_immediate();
    assert(unlink("book-one.txt") == 0 && unlink("book-two.txt") == 0);
}

int main(void) {
    lv_init();
    assert(ui_fonts_init());
    assert(lv_sdl_window_create(BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT));
    settings_load(&current_settings);
    current_settings.animation_scale = 0;
    audio_init();
    gui_init(BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT);
    nav_reset_to_home();
    check_pin_ownership();
    check_books();
    puts("Book path positions and PIN editor ownership passed");
    return 0;
}
