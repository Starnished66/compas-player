/* Exercise the production registry with native UI/audio callbacks isolated. */
#include "../plugin_registries.c"
#include <assert.h>
#include "src/core/themes/ui_customization.h"

static bool values[3];
static int opened = -1;
const char * i18n_tr(const char * text) { return text; }
/* The bundled icon is missing and no old plugin copy exists: the bundled path is kept. */
const char * asset_path_plain(const char * path) { (void)path; return "/nonexistent/icon.png"; }
bool gui_native_audio_quick_toggle_value(int kind) { return values[kind]; }
void gui_native_audio_quick_toggle_set(int kind, bool value) { values[kind] = value; }
void gui_native_audio_quick_toggle_open(int kind) { opened = kind; }
plugin_instance_t * plugin_instance_for_state(lua_State * L) { (void)L; return NULL; }
bool plugin_push_callback(lua_State * L, int ref) { (void)L; (void)ref; assert(false); return false; }
void lua_pushboolean(lua_State * L, int value) { (void)L; (void)value; assert(false); }
bool plugin_call_report(lua_State * L, int nargs, bool action, const char * fmt, ...) {
    (void)L; (void)nargs; (void)action; (void)fmt; assert(false); return false;
}

static int order_count(void) { return PLUGIN_QUICK_TOGGLE_CAPACITY; }
static bool order_key(int index, char * out, size_t cap) {
    return snprintf(out, cap, "plugin:test:%d", index) < (int)cap;
}
static const char * order_label(int index) { (void)index; return "Test"; }

static void check_drawer_order(void) {
    ui_order_set_extras(UI_ORDER_DRAWER, order_count, order_key, order_label);
    assert(ui_order_size(UI_ORDER_DRAWER) == UI_DRAWER_COUNT + PLUGIN_QUICK_TOGGLE_CAPACITY);
    ui_item_order_t order;
    ui_order_resolve(UI_ORDER_DRAWER, "plugins", &order);
    /* Power off is held back from the drawer for this release. */
    assert(order.count == 10 && order.ids[7] == 19 && order.ids[8] == 5 && order.ids[9] == 6);
    for (int i = 0; i < order.count; i++) assert(order.ids[i] != ui_order_find_key(UI_ORDER_DRAWER, "power_off"));
    /* The legacy layout omitted most native ids. Preserve those hidden
     * choices while appending available additions (BT DAC and USB DAC). */
    ui_order_resolve(UI_ORDER_DRAWER, "plugin:test:6,wifi,!plugin:test:0,plugins", &order);
    assert(order.count == 10 && order.ids[0] == 19 && order.ids[1] == 0 &&
           order.ids[8] == 5 && order.ids[9] == 6);
    char saved[UI_ORDER_TEXT_MAX];
    assert(ui_order_to_text(UI_ORDER_DRAWER, &order, "plugin:test:6,wifi,!plugin:test:0,plugins", saved, sizeof(saved)));
    assert(strstr(saved, "!bluetooth") != NULL);
    ui_item_order_t restored;
    ui_order_resolve(UI_ORDER_DRAWER, saved, &restored);
    assert(restored.count == order.count);
    assert(memcmp(restored.ids, order.ids, order.count) == 0);
    /* New drawer saves mark hidden native tiles explicitly; a later native
     * addition can then be appended without restoring any hidden tile. */
    ui_order_resolve(UI_ORDER_DRAWER, "wifi,!bluetooth,plugins", &order);
    bool has_bluetooth = false, has_gapless = false;
    for (unsigned i = 0; i < order.count; ++i) {
        has_bluetooth |= order.ids[i] == 1;
        has_gapless |= order.ids[i] == 7;
    }
    assert(!has_bluetooth && has_gapless);
    /* The version also distinguishes new orders with no hidden native tile. */
    ui_order_resolve(UI_ORDER_DRAWER, "@drawer2,wifi,plugins", &order);
    assert(order.count == UI_ORDER_MAX - 1); /* all but the held-back power tile */
    assert(ui_order_to_text(UI_ORDER_DRAWER, &order, "@drawer2,wifi,plugins", saved, sizeof(saved)));
    assert(strstr(saved, "@drawer2") == saved);
    assert(strstr(saved + 1, "@drawer2") == NULL);
    ui_order_resolve(UI_ORDER_DRAWER, "@drawer2,wifi,!bt_dac,plugins", &order);
    for (unsigned i = 0; i < order.count; ++i) assert(order.ids[i] != 5);
}

int main(void) {
    const char * keys[] = { "plugin:example.gain_mode:0", "plugin:example.mseb:0", "plugin:compas.soundstage:0" };
    plugin_register_native_audio_quick_toggles();
    plugin_register_native_audio_quick_toggles();
    assert(plugin_manager_get_quick_toggle_count() == 3);
    assert(!strcmp(plugin_quick_toggles[2].icon, "pull_down/soundstage.png"));
    for (int i = 0; i < 3; ++i) {
        char key[80];
        assert(plugin_manager_get_quick_toggle_key(i, key, sizeof(key)));
        assert(strcmp(key, keys[i]) == 0);
        assert(!plugin_manager_get_quick_toggle_key(i, key, 4));
        assert(plugin_quick_toggles[i].L == NULL);
        assert(plugin_manager_quick_toggle_has_on_hold(i));
        assert(!plugin_manager_get_quick_toggle_value(i));
        plugin_manager_quick_toggle_set(i, true);
        assert(values[i]);
        values[i] = false; /* External settings changes are reflected immediately. */
        assert(!plugin_manager_get_quick_toggle_value(i));
        assert(plugin_manager_quick_toggle_open_settings(i) && opened == i);
    }
    assert(strcmp(plugin_manager_get_quick_toggle_state_text(0, true), "High") == 0);
    assert(strcmp(plugin_manager_get_quick_toggle_state_text(0, false), "Low") == 0);
    assert(!plugin_manager_quick_toggle_open_settings(-1));
    memset(plugin_quick_toggles, 0, sizeof(plugin_quick_toggles));
    plugin_quick_toggle_count = 0;
    plugin_register_native_audio_quick_toggles();
    assert(plugin_manager_get_quick_toggle_count() == 3);
    for (int i = 0; i < PLUGIN_MAX_QUICK_TOGGLES; ++i) {
        assert(plugin_quick_toggle_has_room());
        plugin_quick_toggle_t * external = &plugin_quick_toggles[plugin_quick_toggle_count++];
        memset(external, 0, sizeof(*external));
        external->L = (lua_State *)(uintptr_t)(i + 1);
    }
    assert(plugin_quick_toggle_count == PLUGIN_QUICK_TOGGLE_CAPACITY);
    assert(!plugin_quick_toggle_has_room());
    plugin_register_native_audio_quick_toggles();
    assert(plugin_quick_toggle_count == PLUGIN_QUICK_TOGGLE_CAPACITY);
    check_drawer_order();
    return 0;
}
