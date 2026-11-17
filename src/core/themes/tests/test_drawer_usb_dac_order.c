#include "../ui_customization.h"
#include "../../persistence/settings.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

player_settings_t current_settings;

static bool contains(const ui_item_order_t *order, unsigned id) {
    for (unsigned i = 0; i < order->count; ++i)
        if (order->ids[i] == id) return true;
    return false;
}

static unsigned position(const ui_item_order_t *order, unsigned id) {
    for (unsigned i = 0; i < order->count; ++i)
        if (order->ids[i] == id) return i;
    return UI_ORDER_MAX;
}

static int plugin_count(void) { return 7; }
static bool plugin_key(int id, char *out, size_t cap) {
    return id >= 0 && id < 7 && snprintf(out, cap, "plugin_%d", id) > 0;
}
static const char *plugin_label(int id) { (void)id; return "Plugin"; }

int main(void) {
    ui_customization_t defaults;
    ui_customization_defaults(&defaults);
    /* Power off is held back from the drawer for this release. */
    assert(strstr(defaults.drawer_order_text, "power_off") == NULL);
    const int bt_dac = ui_order_find_key(UI_ORDER_DRAWER, "bt_dac");
    const int usb_dac = ui_order_find_key(UI_ORDER_DRAWER, "usb_dac");
    const int power = ui_order_find_key(UI_ORDER_DRAWER, "power_off");
    assert(power >= 0);
    assert(!ui_order_available(UI_ORDER_DRAWER, power));
    const int remote = ui_order_find_key(UI_ORDER_DRAWER, "remote_control");
    assert(bt_dac >= 0 && usb_dac == bt_dac + 1);

    ui_item_order_t order;
    ui_order_resolve(UI_ORDER_DRAWER, "wifi,bluetooth,sleep,crossfade,airplay,dlna", &order);
    assert(contains(&order, (unsigned)bt_dac));
    assert(contains(&order, (unsigned)usb_dac));
    assert(!contains(&order, (unsigned)remote));
    assert(!contains(&order, (unsigned)power));

    ui_order_set_extras(UI_ORDER_DRAWER, plugin_count, plugin_key, plugin_label);
    ui_order_resolve(UI_ORDER_DRAWER, "@drawer2", &order);
    assert(order.count == UI_ORDER_MAX - 1); /* all but the held-back power tile */
    assert(position(&order, UI_DRAWER_COUNT) == UI_DRAWER_COUNT - 1);
    assert(order.ids[order.count - 1] == UI_ORDER_MAX - 1);

    ui_order_resolve(UI_ORDER_DRAWER, "@drawer2,wifi,bluetooth,sleep,crossfade,airplay,bt_dac", &order);
    assert(position(&order, (unsigned)usb_dac) > position(&order, (unsigned)bt_dac));
    assert(contains(&order, (unsigned)remote));

    ui_order_resolve(UI_ORDER_DRAWER, "@drawer2,wifi,bluetooth,bt_dac,usb_dac,!remote_control", &order);
    assert(position(&order, (unsigned)usb_dac) == position(&order, (unsigned)bt_dac) + 1);
    assert(!contains(&order, (unsigned)remote));
    ui_order_resolve(UI_ORDER_DRAWER, "@drawer2,!power_off", &order);
    assert(!contains(&order, (unsigned)power));

    /* Not even an explicitly saved layout shows it. */
    ui_order_resolve(UI_ORDER_DRAWER, "@drawer2,wifi,power_off,bluetooth,plugins", &order);
    assert(!contains(&order, (unsigned)power));
    puts("USB DAC and power drawer order migration passed");
    return 0;
}
