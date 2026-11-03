#pragma once
#include <lvgl/lvgl.h>
#include "gui_theme.h"
#include <stdbool.h>
#include <stdint.h>

/* A reusable color picker: preview swatch with hex value, a saturation/
 * brightness square with a hue bar, a "Hex code" row that opens the keyboard,
 * and an optional preset grid. Several instances can exist at once; each owns
 * its state, freed with the returned object.
 *
 * on_change fires with every pick: final is false while a finger is still
 * dragging (preview only) and true once a pick is settled (release, preset,
 * typed hex code). It never fires from gui_color_picker_set_rgb(). */
typedef void (*gui_color_picker_cb_t)(uint32_t rgb, bool final, void * user_data);

#define GUI_COLOR_PICKER_MAX_PRESETS 16

/* width is the card width; presets (0xRRGGBB, copied) may be NULL. */
lv_obj_t * gui_color_picker_create(lv_obj_t * parent, int32_t width, uint32_t rgb, const uint32_t * presets,
                                   int preset_count, gui_color_picker_cb_t on_change, void * user_data);
void gui_color_picker_set_rgb(lv_obj_t * picker, uint32_t rgb);
uint32_t gui_color_picker_get_rgb(lv_obj_t * picker);
/* A muted line under the hex value; NULL hides it. */
void gui_color_picker_set_info(lv_obj_t * picker, const char * text);
/* false dims the picker and hides the preset selection (it still works). */
void gui_color_picker_set_active(lv_obj_t * picker, bool active);

/* "#RRGGBB" or "RRGGBB" -> 0xRRGGBB; false for anything else. */
bool gui_color_parse_hex(const char * text, uint32_t * rgb);

/* Card, label and plain (touch-transparent) object builders shared with the
 * Accent Options screen. */
lv_obj_t * gui_card_create(lv_obj_t * parent, int32_t width);
lv_obj_t * gui_card_text(lv_obj_t * parent, const char * text, gui_font_role_t role, bool muted);
lv_obj_t * gui_plain_obj_create(lv_obj_t * parent);
#define GUI_CARD_PAD BOARD_SCALE_PX(16)
