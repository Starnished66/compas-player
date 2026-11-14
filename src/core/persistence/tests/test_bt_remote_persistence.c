/* Exercise the real settings writer and loader for remembered service modes. */
#include <assert.h>
#include "src/core/persistence/settings.c"

void settings_registry_note_save(const void * settings) { (void) settings; }
void subsonic_saved_servers_load(subsonic_saved_server_t ** rows, int * count) {
    *rows = NULL;
    *count = 0;
}
void subsonic_saved_servers_upsert(const char * url, const char * username,
                                 const char * password, bool verify_tls) {
    (void) url; (void) username; (void) password; (void) verify_tls;
}

int main(void) {
    assert(mkdir(INTERNAL_COMPAS_DIR, 0700) == 0);

    player_settings_t settings;
    set_defaults(&settings);
    assert(!settings.usb_dac_mode_enabled);
    settings.usb_dac_mode_enabled = true;
    settings.usb_mode = 0;
    settings.bt_dac_mode_enabled = true;
    settings.remote_control_enabled = true;
    settings_write_file(&settings);

    player_settings_t loaded;
    assert(settings_load(&loaded));
    assert(loaded.bt_dac_mode_enabled);
    assert(loaded.usb_dac_mode_enabled && loaded.usb_mode == 0);
    assert(loaded.remote_control_enabled);

    settings.usb_dac_mode_enabled = false;
    settings.usb_mode = 1;
    settings.bt_dac_mode_enabled = false;
    settings.remote_control_enabled = false;
    settings_write_file(&settings);
    assert(settings_load(&loaded));
    assert(!loaded.bt_dac_mode_enabled);
    assert(!loaded.usb_dac_mode_enabled && loaded.usb_mode == 1);
    assert(!loaded.remote_control_enabled);

    puts("Bluetooth/USB DAC and remote-control persistence: ok");
    return 0;
}
