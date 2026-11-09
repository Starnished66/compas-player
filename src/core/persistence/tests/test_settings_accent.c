/* Exercise the real settings writer and splash parser in an isolated directory. */
#include <assert.h>
#include "src/core/persistence/settings.c"
#include "src/core/ui/display/startup_splash.h"

void settings_registry_note_save(const void * settings) { (void) settings; }
void subsonic_saved_servers_load(subsonic_saved_server_t ** rows, int * count) {
    *rows = NULL;
    *count = 0;
}
void subsonic_saved_servers_upsert(const char * url, const char * username,
                                 const char * password, bool verify_tls) {
    (void) url; (void) username; (void) password; (void) verify_tls;
}

static uint32_t splash_accent(void) {
    bool enabled = false;
    uint32_t accent = 0;
    startup_splash_get_tint(&enabled, &accent);
    assert(enabled);
    return accent;
}

static void write_splash_settings(bool dynamic, bool cache_valid, uint32_t manual, uint32_t cache) {
    player_settings_t settings;
    set_defaults(&settings);
    settings.setup_complete = true;
    settings.startup_splash_tint = true;
    settings.accent_dynamic = dynamic;
    settings.accent_color = manual;
    settings.accent_dynamic_color_valid = cache_valid;
    settings.accent_dynamic_color = cache;
    settings_write_file(&settings);
}

int main(void) {
    assert(mkdir(INTERNAL_COMPAS_DIR, 0700) == 0);

    write_splash_settings(true, true, 0x123456, 0xABCDEF);
    assert(splash_accent() == 0xABCDEF);
    write_splash_settings(false, true, 0x123456, 0xABCDEF);
    assert(splash_accent() == 0x123456);
    write_splash_settings(true, true, 0x123456, 0);
    assert(splash_accent() == 0); /* Black is valid when its presence bit is set. */
    write_splash_settings(true, false, 0x123456, 0);
    assert(splash_accent() == 0x123456);

    FILE * file = fopen(SETTINGS_FILE_PATH, "w");
    assert(file);
    fputs("startup_splash_tint=1\nsetup_complete=1\naccent_dynamic=1\n"
          "accent_dynamic_color=invalid\naccent_color=654321\n", file);
    assert(fclose(file) == 0);
    assert(splash_accent() == 0x654321);

    set_defaults(&current_settings);
    current_settings.setup_complete = true;
    current_settings.startup_splash_tint = true;
    current_settings.accent_dynamic = true;
    current_settings.accent_color = 0x102030;
    current_settings.accent_dynamic_color = 0x314159; /* Latest cover, not yet saved. */
    current_settings.accent_dynamic_color_valid = true;
    settings_shutdown_flush();

    player_settings_t loaded;
    assert(settings_load(&loaded));
    assert(loaded.accent_dynamic && loaded.accent_color == 0x102030);
    assert(loaded.accent_dynamic_color_valid && loaded.accent_dynamic_color == 0x314159);
    assert(splash_accent() == 0x314159);

    assert(remove(SETTINGS_FILE_PATH) == 0);
    settings_shutdown_flush();
    settings_save_drain();
    assert(access(SETTINGS_FILE_PATH, F_OK) != 0); /* A repeated flush cannot revive reset settings. */
    puts("dynamic accent persistence and splash: ok");
    return 0;
}
