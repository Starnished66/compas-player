#include "airplay_control.h"
#include "airplay_bridge.h"
#include "airplay_metadata.h"
#include "../../core/process/subprocess.h"

#include <stdio.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <unistd.h>

/* Tracks whether AirPlay daemon and bridge are active. */
static atomic_bool control_active = false;

bool airplay_control_is_active(void) {
    return control_active;
}

bool airplay_control_start(const char * device_name) {
    /* Recreate the audio FIFO and clean up any stale now-playing FIFO. */
    unlink(AIRPLAY_FIFO_PATH);
    if (mkfifo(AIRPLAY_FIFO_PATH, 0666) != 0) {
        fprintf(stderr, "airplay_control: mkfifo(%s) failed -- AirPlay audio will not work this session\n",
                AIRPLAY_FIFO_PATH);
        control_active = false;
        return false;
    }
    unlink(AIRPLAY_NOW_PLAYING_PATH);

    if (!airplay_bridge_start()) {
        control_active = false;
        return false;
    }

    /* Metadata is non-essential; startup proceeds even if metadata reader fails. */
    if (!airplay_metadata_start()) {
        fprintf(stderr, "airplay_control: metadata reader failed to start -- AirPlay audio will still work, "
                        "no track info will be shown\n");
    }

    char * argv[] = { (char *) "shairport", (char *) "-a", (char *) device_name,
                       (char *) "-M", (char *) "/tmp", (char *) "-b", (char *) "160",
                       (char *) "-l", (char *) "/tmp/shairport.log",
                       (char *) "-o", (char *) "pipe",
                       (char *) "--", (char *) AIRPLAY_FIFO_PATH, NULL };
    if (!subprocess_spawn_daemon(argv)) {
        fprintf(stderr, "airplay_control: failed to spawn shairport -- AirPlay will not be discoverable this session\n");
        /* Roll back started threads and FIFO if shairport spawn fails. */
        airplay_bridge_stop();
        airplay_metadata_stop();
        unlink(AIRPLAY_FIFO_PATH);
        control_active = false;
        return false;
    }

    control_active = true;
    return true;
}

void airplay_control_stop(void) {
    char * argv[] = { (char *) "killall", (char *) "shairport", NULL };
    subprocess_run(argv, NULL, 0);
    airplay_bridge_stop();
    airplay_metadata_stop();
    unlink(AIRPLAY_FIFO_PATH);
    control_active = false;
}
