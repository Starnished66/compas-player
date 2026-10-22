#pragma once

#include <stdbool.h>
#include <stdint.h>

enum { UI_ORDER_MAX = 10, UI_MUSIC_COUNT = 7, UI_MUSIC_TOOLBAR_COUNT = 3,
       UI_DRAWER_COUNT = 10 };
typedef enum { UI_ORDER_MUSIC, UI_ORDER_MUSIC_TOOLBAR, UI_ORDER_DRAWER } ui_order_kind_t;
typedef struct { uint8_t count; uint8_t ids[UI_ORDER_MAX]; } ui_item_order_t;

/* Stable IDs in each order's own namespace. Omitted IDs are hidden. */
typedef struct {
    ui_item_order_t music;
    ui_item_order_t music_toolbar;
    ui_item_order_t drawer;
    bool drawer_enabled;
    bool drawer_eq_visible;
} ui_customization_t;

/* Plugin declarations override native Music defaults and saved Drawer
 * preferences in RAM only. The persisted native orders remain the base. */
typedef struct {
    bool has_music, has_music_toolbar, has_drawer;
    bool has_drawer_enabled, has_drawer_eq_visible, has_drawer_volume_visible;
    ui_customization_t value;
    bool drawer_volume_visible;
} ui_customization_override_t;

void ui_customization_defaults(ui_customization_t *out);
const char *ui_order_key(ui_order_kind_t kind, int id);
const char *ui_order_label(ui_order_kind_t kind, int id);
int ui_order_size(ui_order_kind_t kind);
int ui_order_find_key(ui_order_kind_t kind, const char *key);
bool ui_order_parse(ui_order_kind_t kind, const char *text, ui_item_order_t *out);
void ui_order_format(ui_order_kind_t kind, const ui_item_order_t *order, char *out, unsigned capacity);
/* UI-thread-only; no persistence, allocation, or screen rebuild. */
void ui_customization_set_override(const ui_customization_override_t *override);
ui_customization_t ui_customization_effective(void);
bool ui_customization_drawer_volume_visible(void);
