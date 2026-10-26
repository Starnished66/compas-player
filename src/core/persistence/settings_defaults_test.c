#include "settings.h"
#include "../../library/remote/subsonic/subsonic_saved_servers.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Settings loading also synchronizes its saved-server sidecar. Keep this
 * focused test independent of that unrelated persistent store. */
void subsonic_saved_servers_load(subsonic_saved_server_t **rows, int *count) {
    *rows = NULL;
    *count = 0;
}

void subsonic_saved_servers_upsert(const char *url, const char *username,
                                   const char *password, bool verify_tls) {
    (void)url;
    (void)username;
    (void)password;
    (void)verify_tls;
}

static void write_settings(const char *contents) {
    assert(mkdir(".compas", 0700) == 0 || errno == EEXIST);
    FILE *f = fopen(".compas/settings.txt", "w");
    assert(f);
    assert(fputs(contents, f) >= 0);
    assert(fclose(f) == 0);
}

int main(void) {
    player_settings_t settings;

    remove(".compas/settings.txt");
    assert(!settings_load(&settings));
    assert(settings.keyboard_layout == KEYBOARD_LAYOUT_QWERTY);
    assert(settings.idle_shutdown_enabled);
    assert(settings.idle_suspend_enabled);
    assert(settings.idle_shutdown_minutes == 10);
    assert(settings.dac_filter == 0 && !settings.dac_nos && settings.dac_dre);
    assert(settings.usb_dsd_output == USB_DSD_OUTPUT_PCM);
    assert(settings.lyrics_keep_screen_on);

    write_settings("keyboard_layout=0\nidle_shutdown_enabled=1\n"
                   "idle_suspend_enabled=0\nidle_shutdown_minutes=30\n");
    assert(settings_load(&settings));
    assert(settings.keyboard_layout == KEYBOARD_LAYOUT_T9);
    assert(settings.idle_shutdown_enabled);
    assert(!settings.idle_suspend_enabled);
    assert(settings.idle_shutdown_minutes == 30);

    /* Existing settings without these keys inherit current defaults. */
    write_settings("volume=0.5\n");
    assert(settings_load(&settings));
    assert(settings.keyboard_layout == KEYBOARD_LAYOUT_QWERTY);
    assert(settings.idle_shutdown_enabled && settings.idle_suspend_enabled);
    assert(settings.idle_shutdown_minutes == 10);
    assert(settings.dac_filter == 0 && !settings.dac_nos && settings.dac_dre);
    assert(settings.usb_dsd_output == USB_DSD_OUTPUT_PCM);
    assert(settings.lyrics_keep_screen_on);

    write_settings("volume=0.5\nlyrics_keep_screen_on=0\n");
    assert(settings_load(&settings));
    assert(!settings.lyrics_keep_screen_on);
    settings_save(&settings);
    assert(settings_load(&settings));
    assert(!settings.lyrics_keep_screen_on);
    settings.lyrics_keep_screen_on = true;
    settings_save(&settings);
    assert(settings_load(&settings));
    assert(settings.lyrics_keep_screen_on);

    /* The new global Albums order persists as mode 3, while the two nested
     * collections stay within their existing three sorting choices. */
    write_settings("album_sort_mode=3\n");
    assert(settings_load(&settings));
    assert(settings.album_sort_mode == 3);
    assert(settings.artist_albums_sort_mode == 0);
    assert(settings.album_artist_albums_sort_mode == 0);
    settings_save(&settings);
    assert(settings_load(&settings));
    assert(settings.album_sort_mode == 3);
    assert(settings.artist_albums_sort_mode == 0);
    assert(settings.album_artist_albums_sort_mode == 0);
    write_settings("album_sort_mode=3\nartist_albums_sort_mode=3\n"
                   "album_artist_albums_sort_mode=3\n");
    assert(settings_load(&settings));
    assert(settings.album_sort_mode == 3);
    assert(settings.artist_albums_sort_mode == 0);
    assert(settings.album_artist_albums_sort_mode == 0);

    write_settings("dac_filter=3\ndac_nos=1\ndac_dre=0\n");
    assert(settings_load(&settings));
    assert(settings.dac_filter == 3 && settings.dac_nos && !settings.dac_dre);
    settings_save(&settings);
    assert(settings_load(&settings));
    assert(settings.dac_filter == 3 && settings.dac_nos && !settings.dac_dre);
    write_settings("dac_filter=4\ndac_nos=invalid\ndac_dre=invalid\n");
    assert(settings_load(&settings));
    assert(settings.dac_filter == 0 && !settings.dac_nos && settings.dac_dre);

    write_settings("usb_dsd_output=1\n");
    assert(settings_load(&settings));
    assert(settings.usb_dsd_output == USB_DSD_OUTPUT_NATIVE);
    settings_save(&settings);
    assert(settings_load(&settings));
    assert(settings.usb_dsd_output == USB_DSD_OUTPUT_NATIVE);
    write_settings("usb_dsd_output=2\n");
    assert(settings_load(&settings));
    assert(settings.usb_dsd_output == USB_DSD_OUTPUT_DOP);
    write_settings("usb_dsd_output=3\n");
    assert(settings_load(&settings));
    assert(settings.usb_dsd_output == USB_DSD_OUTPUT_PCM);
    write_settings("usb_dsd_output=1junk\n");
    assert(settings_load(&settings));
    assert(settings.usb_dsd_output == USB_DSD_OUTPUT_PCM);

    /* Legacy files without the migration marker keep explicit idle choices. */
    write_settings("keyboard_layout=1\nidle_shutdown_enabled=0\n"
                   "idle_suspend_enabled=0\nidle_shutdown_minutes=15\n");
    assert(settings_load(&settings));
    assert(settings.keyboard_layout == KEYBOARD_LAYOUT_QWERTY);
    assert(!settings.idle_shutdown_enabled && !settings.idle_suspend_enabled);
    assert(settings.idle_shutdown_minutes == 15);
    settings_save(&settings);
    assert(settings_load(&settings));
    assert(settings.keyboard_layout == KEYBOARD_LAYOUT_QWERTY);
    assert(!settings.idle_shutdown_enabled && !settings.idle_suspend_enabled);
    assert(settings.idle_shutdown_minutes == 15);

    write_settings("play_mode=4\n");
    assert(settings_load(&settings));
    assert(settings.play_mode == 4);
    settings_save(&settings);
    assert(settings_load(&settings));
    assert(settings.play_mode == 4);

    /* Saved Music menu preferences preserve explicit hiding, including an
     * empty list. Plugin declarations continue to mask the native order in RAM. */
    ui_customization_defaults(&settings.ui_customization);
    settings.ui_customization.music.count = 3;
    settings.ui_customization.music.ids[0] = 6;
    settings.ui_customization.music.ids[1] = 0;
    settings.ui_customization.music.ids[2] = 4;
    settings_save(&settings);
    assert(settings_load(&settings));
    assert(settings.ui_customization.music.count == 3);
    assert(settings.ui_customization.music.ids[0] == 6);
    assert(settings.ui_customization.music.ids[1] == 0);
    assert(settings.ui_customization.music.ids[2] == 4);
    settings.ui_customization.music.count = 0;
    settings_save(&settings);
    assert(settings_load(&settings));
    assert(settings.ui_customization.music.count == 0);

    /* RAM-only plugin declarations mask saved Music orders while active and
     * leave the saved native baseline and Drawer state untouched. */
    current_settings = settings;
    current_settings.ui_customization.music.count = 2;
    current_settings.ui_customization.music.ids[0] = 5;
    current_settings.ui_customization.music.ids[1] = 1;
    current_settings.ui_customization.drawer.count = 2;
    current_settings.ui_customization.drawer.ids[0] = 9;
    current_settings.ui_customization.drawer.ids[1] = 0;
    ui_customization_set_override(NULL);
    ui_customization_t effective = ui_customization_effective();
    assert(effective.music.count == 2 && effective.music.ids[0] == 5 && effective.music.ids[1] == 1);

    ui_customization_override_t plugin_override = {0};
    plugin_override.has_music = true;
    plugin_override.value.music.count = 1;
    plugin_override.value.music.ids[0] = 3;
    ui_customization_set_override(&plugin_override);
    effective = ui_customization_effective();
    assert(effective.music.count == 1 && effective.music.ids[0] == 3);
    assert(current_settings.ui_customization.music.count == 2);
    assert(current_settings.ui_customization.music.ids[0] == 5);
    assert(effective.drawer.count == 2 && effective.drawer.ids[0] == 9 && effective.drawer.ids[1] == 0);

    ui_customization_set_override(NULL);
    effective = ui_customization_effective();
    assert(effective.music.count == 2 && effective.music.ids[0] == 5);
    assert(effective.drawer.count == 2 && effective.drawer.ids[0] == 9);

    write_settings("play_mode=5\n");
    assert(settings_load(&settings));
    assert(settings.play_mode == 0);

    write_settings("keyboard_layout=99\n");
    assert(settings_load(&settings));
    assert(settings.keyboard_layout == KEYBOARD_LAYOUT_QWERTY);

    puts("settings defaults regression passed");
    return 0;
}
