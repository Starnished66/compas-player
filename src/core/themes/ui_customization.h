#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Native ids come first in each namespace; plugin entries follow. */
enum { UI_ORDER_MAX = 16, UI_MUSIC_COUNT = 8, UI_MUSIC_COVER_FLOW = 7, UI_MUSIC_TOOLBAR_COUNT = 3,
       UI_DRAWER_COUNT = 11, UI_DRAWER_MAX_TILES = 12, UI_ORDER_KEY_MAX = 80, UI_ORDER_UNLOADED_MAX = 16,
       /* Every loaded entry plus the remembered unloaded ones, each a full
        * key with its "!" and separator. */
       UI_ORDER_TEXT_MAX = (UI_ORDER_MAX + UI_ORDER_UNLOADED_MAX) * (UI_ORDER_KEY_MAX + 2) };
/* The optional cover flow module (src/ui/widgets/cover_flow): NULL when it is not built. */
extern void cover_flow_open_albums(void) __attribute__((weak));

typedef enum { UI_ORDER_MUSIC, UI_ORDER_MUSIC_TOOLBAR, UI_ORDER_DRAWER } ui_order_kind_t;
typedef struct { uint8_t count; uint8_t ids[UI_ORDER_MAX]; } ui_item_order_t;

/* Stable IDs in each order's own namespace. Omitted IDs are hidden. */
typedef struct {
    /* Saved Music and Quick Drawer orders as keys. The resolved id lists
     * (music, drawer) are only filled in by ui_customization_effective(). */
    char music_order_text[UI_ORDER_TEXT_MAX];
    ui_item_order_t music;
    ui_item_order_t music_toolbar;
    char drawer_order_text[UI_ORDER_TEXT_MAX];
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
/* False for an id that is not offered now (Cover Flow without its module); editors skip it. */
bool ui_order_available(ui_order_kind_t kind, int id);
/* Plugin entries join an order at ids ui_order_size(kind) - extras.. after the
 * native ones. ui_order_key() stays NULL for them; use ui_order_to_text(). */
void ui_order_set_extras(ui_order_kind_t kind, int (*count)(void), bool (*key)(int, char *, size_t),
                         const char *(*label)(int));
/* Native keys only. */
int ui_order_find_key(ui_order_kind_t kind, const char *key);
/* Saved key text to the visible order, and back (keeps unloaded plugin keys).
 * "!key" hides a plugin entry; natives are hidden by being omitted
 * (Cover Flow starts omitted). Plugin entries the text never mentions show at the end. */
void ui_order_resolve(ui_order_kind_t kind, const char *text, ui_item_order_t *out);
bool ui_order_to_text(ui_order_kind_t kind, const ui_item_order_t *visible, const char *old_text,
                      char *out, size_t cap);
/* UI-thread-only; no persistence, allocation, or screen rebuild. */
void ui_customization_set_override(const ui_customization_override_t *override);
ui_customization_t ui_customization_effective(void);
bool ui_customization_drawer_volume_visible(void);
