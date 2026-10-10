#include "settings.h"
#include "subsonic_saved_servers.h"

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

    write_settings("keyboard_layout=99\n");
    assert(settings_load(&settings));
    assert(settings.keyboard_layout == KEYBOARD_LAYOUT_QWERTY);

    puts("settings defaults regression passed");
    return 0;
}
