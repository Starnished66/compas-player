#include "ui_customization.h"
#include "../persistence/settings.h"
#include "../../ui/text/i18n/i18n.h"

#include <stdio.h>
#include <string.h>

static ui_customization_override_t active_override;

static const char * const music_keys[UI_MUSIC_COUNT] = {
    "files", "artists", "albums", "album_artist", "genres", "all_songs", "playlists", "cover_flow"
};
static const char * const music_labels[UI_MUSIC_COUNT] = {
    N_("Files"), N_("Artists"), N_("Albums"), N_("Album Artist"), N_("Genres"), N_("All Songs"), N_("Playlists"),
    N_("Cover Flow")
};
static const char * const toolbar_keys[UI_MUSIC_TOOLBAR_COUNT] = {
    "sound", "playback", "library"
};
static const char * const toolbar_labels[UI_MUSIC_TOOLBAR_COUNT] = {
    N_("Sound"), N_("Playback"), N_("Library")
};
static const char * const drawer_keys[UI_DRAWER_COUNT] = {
    "wifi", "bluetooth", "sleep", "crossfade", "airplay", "bt_dac", "dlna", "gapless",
    "remote_control", "car_mode", "plugins"
};
static const char * const drawer_labels[UI_DRAWER_COUNT] = {
    N_("Wi-Fi"), N_("Bluetooth"), N_("Sleep Timer"), N_("Crossfade"), N_("AirPlay"), N_("Bluetooth DAC"), N_("DLNA"),
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

/* Plugin entries join an order after the native ids. */
typedef struct {
    int (*count)(void);
    bool (*key)(int, char *, size_t);
    const char *(*label)(int);
} ui_order_extras_t;
static ui_order_extras_t order_extras[sizeof(order_descs) / sizeof(order_descs[0])];

void ui_order_set_extras(ui_order_kind_t kind, int (*count)(void), bool (*key)(int, char *, size_t),
                         const char *(*label)(int)) {
    if (!order_desc_for(kind)) return;
    order_extras[kind] = (ui_order_extras_t){ count, key, label };
}

static int extras_count(ui_order_kind_t kind) {
    const ui_order_desc_t * desc = order_desc_for(kind);
    if (!desc || !order_extras[kind].count) return 0;
    int n = order_extras[kind].count();
    return n < 0 ? 0 : n > UI_ORDER_MAX - desc->count ? UI_ORDER_MAX - desc->count : n;
}

int ui_order_size(ui_order_kind_t kind) {
    const ui_order_desc_t * desc = order_desc_for(kind);
    return desc ? desc->count + extras_count(kind) : 0;
}

const char * ui_order_key(ui_order_kind_t kind, int id) {
    const ui_order_desc_t * desc = order_desc_for(kind);
    return desc && id >= 0 && id < desc->count ? desc->keys[id] : NULL;
}

const char * ui_order_label(ui_order_kind_t kind, int id) {
    const ui_order_desc_t * desc = order_desc_for(kind);
    if (!desc || id < 0) return NULL;
    if (id < desc->count) return desc->labels[id];
    if (id - desc->count < extras_count(kind) && order_extras[kind].label)
        return order_extras[kind].label(id - desc->count);
    return NULL;
}

/* Cover Flow is offered only while its module is built. */
bool ui_order_available(ui_order_kind_t kind, int id) {
    return kind != UI_ORDER_MUSIC || id != UI_MUSIC_COVER_FLOW || cover_flow_open_albums;
}

/* Plugin entries are hidden by a "!key" and shown when new; natives are hidden by being omitted. */
static bool order_id_hides_by_key(ui_order_kind_t kind, int id) {
    return id >= order_desc_for(kind)->count;
}

int ui_order_find_key(ui_order_kind_t kind, const char *key) {
    const ui_order_desc_t * desc = order_desc_for(kind);
    if (!key || !desc) return -1;
    for (int i = 0; i < desc->count; ++i)
        if (strcmp(key, desc->keys[i]) == 0) return i;
    return -1;
}
static void default_text(ui_order_kind_t kind, char *out, size_t cap);
void ui_customization_defaults(ui_customization_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    default_text(UI_ORDER_MUSIC, out->music_order_text, sizeof(out->music_order_text));
    default_text(UI_ORDER_DRAWER, out->drawer_order_text, sizeof(out->drawer_order_text));
    out->music_toolbar.count = UI_MUSIC_TOOLBAR_COUNT;
    for (int i = 0; i < UI_MUSIC_TOOLBAR_COUNT; ++i) out->music_toolbar.ids[i] = (uint8_t)i;
    out->drawer_enabled = true;
    out->drawer_eq_visible = true;
}

/* ---- Saved keys <-> ids (natives, then loaded plugin entries) ---- */

static bool order_id_key(ui_order_kind_t kind, int id, char *out, size_t cap) {
    const char *native = ui_order_key(kind, id);
    if (native) return snprintf(out, cap, "%s", native) < (int)cap;
    int extra = id - order_desc_for(kind)->count;
    return extra >= 0 && extra < extras_count(kind) && order_extras[kind].key &&
           order_extras[kind].key(extra, out, cap);
}

static int order_find_id(ui_order_kind_t kind, const char *key) {
    int id = ui_order_find_key(kind, key);
    if (id >= 0 && !ui_order_available(kind, id)) return -1;
    char other[UI_ORDER_KEY_MAX];
    for (int i = order_desc_for(kind)->count; id < 0 && i < ui_order_size(kind); ++i)
        if (order_id_key(kind, i, other, sizeof(other)) && strcmp(other, key) == 0) id = i;
    return id;
}

/* Next saved key; an over-long key comes back empty so callers skip it. */
static bool order_next_key(const char **p, char *key, size_t cap, bool *hidden) {
    while (**p == ',') ++*p;
    if (!**p) return false;
    const char *end = strchr(*p, ',');
    size_t len = end ? (size_t)(end - *p) : strlen(*p);
    const char *start = *p;
    *p += len;
    *hidden = *start == '!';
    start += *hidden;
    len -= *hidden;
    if (len >= cap) len = 0;
    memcpy(key, start, len);
    key[len] = '\0';
    return true;
}

static void default_text(ui_order_kind_t kind, char *out, size_t cap) {
    size_t used = 0;
    for (int i = 0; i < order_desc_for(kind)->count; ++i) {
        if (kind == UI_ORDER_MUSIC && i == UI_MUSIC_COVER_FLOW) continue; /* hidden until switched on */
        if (kind == UI_ORDER_DRAWER && !strcmp(ui_order_key(kind, i), "dlna")) continue;
        used += (size_t)snprintf(out + used, cap - used, "%s%s", used ? "," : "", ui_order_key(kind, i));
    }
    /* DLNA goes last so it is the tile dropped when plugins fill the drawer. */
    if (kind == UI_ORDER_DRAWER) snprintf(out + used, cap - used, ",dlna");
}

/* Plugin entries go at the end, or right after the "plugins" entry in the
 * Quick Drawer (dropped there when that entry is hidden). */
static void order_insert_extras(ui_order_kind_t kind, ui_item_order_t *order, const ui_item_order_t *extra) {
    int pos = (int)order->count;
    if (kind == UI_ORDER_DRAWER) {
        int anchor = ui_order_find_key(kind, "plugins");
        pos = -1;
        for (unsigned i = 0; i < order->count; ++i)
            if (order->ids[i] == anchor) pos = (int)i + 1;
        if (pos < 0) return;
    }
    for (unsigned i = 0; i < extra->count && order->count < UI_ORDER_MAX; ++i) {
        memmove(&order->ids[pos + 1], &order->ids[pos], order->count - (unsigned)pos);
        order->ids[pos++] = extra->ids[i];
        order->count++;
    }
}

void ui_order_resolve(ui_order_kind_t kind, const char *text, ui_item_order_t *out) {
    bool seen[UI_ORDER_MAX] = { false };
    const char *p = text ? text : "";
    char key[UI_ORDER_KEY_MAX];
    bool hidden;
    out->count = 0;
    while (order_next_key(&p, key, sizeof(key), &hidden)) {
        int id = order_find_id(kind, key);
        if (id < 0 || seen[id]) continue;
        seen[id] = true;
        if (!hidden) out->ids[out->count++] = (uint8_t)id;
    }
    /* Plugin entries the text never mentions (new installs) still show. */
    ui_item_order_t extra = { 0 };
    for (int id = 0; id < ui_order_size(kind); ++id)
        if (!seen[id] && order_id_hides_by_key(kind, id)) extra.ids[extra.count++] = (uint8_t)id;
    order_insert_extras(kind, out, &extra);
}

static bool order_text_add(char *out, size_t cap, size_t *used, bool hidden, const char *key) {
    int n = snprintf(out + *used, cap - *used, "%s%s%s", *used ? "," : "", hidden ? "!" : "", key);
    if (n < 0 || (size_t)n >= cap - *used) { out[*used] = '\0'; return false; }
    *used += (size_t)n;
    return true;
}

static bool order_key_unloaded(ui_order_kind_t kind, const char *key) {
    return key[0] && order_find_id(kind, key) < 0;
}

/* Appends the first UI_ORDER_UNLOADED_MAX old-text keys that match no current
 * entry, in their saved order. A plugin unloaded since the last save still sits
 * at its old place, ahead of the keys kept from earlier saves, so writing them
 * in one pass keeps the most recently seen first and drops the oldest. */
static void order_keep_unloaded(ui_order_kind_t kind, const char *old_text,
                                char *out, size_t cap, size_t *used) {
    const char *p = old_text ? old_text : "";
    char key[UI_ORDER_KEY_MAX];
    bool hidden;
    int index = 0;
    while (order_next_key(&p, key, sizeof(key), &hidden))
        if (order_key_unloaded(kind, key) && index++ < UI_ORDER_UNLOADED_MAX)
            (void)order_text_add(out, cap, used, hidden, key);
}

bool ui_order_to_text(ui_order_kind_t kind, const ui_item_order_t *visible, const char *old_text,
                      char *out, size_t cap) {
    bool shown[UI_ORDER_MAX] = { false };
    char key[UI_ORDER_KEY_MAX];
    size_t used = 0;
    if (!visible || !out || !cap || !order_desc_for(kind)) return false;
    out[0] = '\0';
    for (unsigned i = 0; i < visible->count && visible->ids[i] < UI_ORDER_MAX; ++i) {
        shown[visible->ids[i]] = true;
        if (order_id_key(kind, visible->ids[i], key, sizeof(key)) &&
            !order_text_add(out, cap, &used, false, key)) return false;
    }
    /* Hidden plugin entries get a "!" key; hidden natives are just omitted. */
    for (int id = 0; id < ui_order_size(kind); ++id)
        if (!shown[id] && order_id_hides_by_key(kind, id) && order_id_key(kind, id, key, sizeof(key)) &&
            !order_text_add(out, cap, &used, true, key)) return false;
    /* Keys of plugins that are not loaded now keep their place for later.
     * Only the newest UI_ORDER_UNLOADED_MAX are kept, so UI_ORDER_TEXT_MAX
     * always holds them and no key is ever cut. */
    order_keep_unloaded(kind, old_text, out, cap, &used);
    return true;
}

void ui_customization_set_override(const ui_customization_override_t *override) {
    if (override) active_override = *override;
    else memset(&active_override, 0, sizeof(active_override));
}

/* A plugin layout replaces the native part; plugin entries follow it. */
static void order_apply_override(ui_order_kind_t kind, ui_item_order_t *order, const ui_item_order_t *layout) {
    ui_item_order_t merged = { 0 }, extra = { 0 };
    for (unsigned i = 0; i < layout->count; ++i)
        if (ui_order_available(kind, layout->ids[i])) merged.ids[merged.count++] = layout->ids[i];
    for (unsigned i = 0; i < order->count; ++i)
        if (order->ids[i] >= order_desc_for(kind)->count) extra.ids[extra.count++] = order->ids[i];
    order_insert_extras(kind, &merged, &extra);
    *order = merged;
}

ui_customization_t ui_customization_effective(void) {
    ui_customization_t result = current_settings.ui_customization;
    ui_order_resolve(UI_ORDER_MUSIC, result.music_order_text, &result.music);
    ui_order_resolve(UI_ORDER_DRAWER, result.drawer_order_text, &result.drawer);
    if (active_override.has_music) order_apply_override(UI_ORDER_MUSIC, &result.music, &active_override.value.music);
    if (active_override.has_music_toolbar) result.music_toolbar = active_override.value.music_toolbar;
    if (active_override.has_drawer) order_apply_override(UI_ORDER_DRAWER, &result.drawer, &active_override.value.drawer);
    if (active_override.has_drawer_enabled) result.drawer_enabled = active_override.value.drawer_enabled;
    if (active_override.has_drawer_eq_visible) result.drawer_eq_visible = active_override.value.drawer_eq_visible;
    return result;
}
bool ui_customization_drawer_volume_visible(void) {
    return active_override.has_drawer_volume_visible ? active_override.drawer_volume_visible
                                                     : current_settings.quick_drawer_volume_visible;
}
