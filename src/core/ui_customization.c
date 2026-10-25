#include "ui_customization.h"
#include "settings.h"
#include "i18n.h"

#include <stdio.h>
#include <string.h>

static ui_customization_override_t active_override;

static const char * const music_keys[UI_MUSIC_COUNT] = {
    "files", "artists", "albums", "album_artist", "genres", "all_songs", "playlists"
};
static const char * const music_labels[UI_MUSIC_COUNT] = {
    N_("Files"), N_("Artists"), N_("Albums"), N_("Album Artist"), N_("Genres"), N_("All Songs"), N_("Playlists")
};
static const char * const toolbar_keys[UI_MUSIC_TOOLBAR_COUNT] = {
    "sound", "playback", "library"
};
static const char * const toolbar_labels[UI_MUSIC_TOOLBAR_COUNT] = {
    N_("Sound"), N_("Playback"), N_("Library")
};
static const char * const drawer_keys[UI_DRAWER_COUNT] = {
    "wifi", "bluetooth", "sleep", "crossfade", "airplay", "dlna", "gapless",
    "remote_control", "car_mode", "plugins"
};
static const char * const drawer_labels[UI_DRAWER_COUNT] = {
    N_("Wi-Fi"), N_("Bluetooth"), N_("Sleep Timer"), N_("Crossfade"), N_("AirPlay"), N_("DLNA"),
    N_("Gapless"), N_("Remote Control"), N_("Car Mode"), N_("Plugins")
};

typedef struct {
    const char * const * keys;
    const char * const * labels;
    int count;
} ui_order_desc_t;

static const ui_order_desc_t order_descs[] = {
    [UI_ORDER_MUSIC]         = { music_keys, music_labels, UI_MUSIC_COUNT },
    [UI_ORDER_MUSIC_TOOLBAR] = { toolbar_keys, toolbar_labels, UI_MUSIC_TOOLBAR_COUNT },
    [UI_ORDER_DRAWER]        = { drawer_keys, drawer_labels, UI_DRAWER_COUNT },
};

static const ui_order_desc_t * order_desc_for(ui_order_kind_t kind) {
    if ((unsigned)kind >= sizeof(order_descs) / sizeof(order_descs[0])) {
        return NULL;
    }
    return &order_descs[kind];
}

int ui_order_size(ui_order_kind_t kind) {
    const ui_order_desc_t * desc = order_desc_for(kind);
    return desc ? desc->count : 0;
}

const char * ui_order_key(ui_order_kind_t kind, int id) {
    const ui_order_desc_t * desc = order_desc_for(kind);
    return desc && id >= 0 && id < desc->count ? desc->keys[id] : NULL;
}

const char * ui_order_label(ui_order_kind_t kind, int id) {
    const ui_order_desc_t * desc = order_desc_for(kind);
    return desc && id >= 0 && id < desc->count ? desc->labels[id] : NULL;
}
int ui_order_find_key(ui_order_kind_t kind, const char *key) {
    if (!key) return -1;
    for (int i = 0; i < ui_order_size(kind); ++i)
        if (strcmp(key, ui_order_key(kind, i)) == 0) return i;
    return -1;
}
void ui_customization_defaults(ui_customization_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->music.count = UI_MUSIC_COUNT;
    for (int i = 0; i < UI_MUSIC_COUNT; ++i) out->music.ids[i] = (uint8_t)i;
    out->music_toolbar.count = UI_MUSIC_TOOLBAR_COUNT;
    for (int i = 0; i < UI_MUSIC_TOOLBAR_COUNT; ++i) out->music_toolbar.ids[i] = (uint8_t)i;
    out->drawer.count = UI_DRAWER_COUNT;
    for (int i = 0; i < UI_DRAWER_COUNT; ++i) out->drawer.ids[i] = (uint8_t)i;
    out->drawer_enabled = true;
    out->drawer_eq_visible = true;
}
bool ui_order_parse(ui_order_kind_t kind, const char *text, ui_item_order_t *out) {
    if (!text || !out || ui_order_size(kind) <= 0) return false;
    ui_item_order_t parsed = {0};
    bool seen[UI_ORDER_MAX] = {0};
    const char *p = text;
    if (!*p) { *out = parsed; return true; }
    while (*p) {
        const char *end = strchr(p, ',');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (!len || len >= 64 || parsed.count >= UI_ORDER_MAX) return false;
        char key[64];
        memcpy(key, p, len); key[len] = '\0';
        int id = ui_order_find_key(kind, key);
        if (id < 0 || seen[id]) return false;
        seen[id] = true;
        parsed.ids[parsed.count++] = (uint8_t)id;
        if (!end) break;
        p = end + 1;
        if (!*p) return false;
    }
    *out = parsed;
    return true;
}
void ui_order_format(ui_order_kind_t kind, const ui_item_order_t *order, char *out, unsigned capacity) {
    if (!out || !capacity) return;
    out[0] = '\0';
    if (!order || order->count > UI_ORDER_MAX) return;
    unsigned used = 0;
    for (unsigned i = 0; i < order->count; ++i) {
        const char *key = ui_order_key(kind, order->ids[i]);
        if (!key) { out[0] = '\0'; return; }
        int n = snprintf(out + used, capacity - used, "%s%s", i ? "," : "", key);
        if (n < 0 || (unsigned)n >= capacity - used) { out[0] = '\0'; return; }
        used += (unsigned)n;
    }
}
void ui_customization_set_override(const ui_customization_override_t *override) {
    if (override) active_override = *override;
    else memset(&active_override, 0, sizeof(active_override));
}
ui_customization_t ui_customization_effective(void) {
    ui_customization_t result = current_settings.ui_customization;
    if (active_override.has_music) result.music = active_override.value.music;
    if (active_override.has_music_toolbar) result.music_toolbar = active_override.value.music_toolbar;
    if (active_override.has_drawer) result.drawer = active_override.value.drawer;
    if (active_override.has_drawer_enabled) result.drawer_enabled = active_override.value.drawer_enabled;
    if (active_override.has_drawer_eq_visible) result.drawer_eq_visible = active_override.value.drawer_eq_visible;
    return result;
}
bool ui_customization_drawer_volume_visible(void) {
    return active_override.has_drawer_volume_visible ? active_override.drawer_volume_visible
                                                     : current_settings.quick_drawer_volume_visible;
}
