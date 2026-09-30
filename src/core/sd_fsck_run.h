#ifndef SD_FSCK_RUN_H
#define SD_FSCK_RUN_H

#include "sd_fsck.h"

#include <stdbool.h>

typedef struct {
    bool started;           /* card was unmounted and a check is running */
    bool released_handles;  /* release_handles ran; the card may still be mounted */
} sd_repair_kick_result_t;

/* True while a check is running or its remount is still waiting for the
 * UI thread. Mount attempts must not open the card during that window. */
bool sd_repair_in_progress(void);

/* The check has finished and the UI thread should mount the saved node. */
bool sd_repair_needs_remount(void);

const char * sd_repair_device(void);

/* False when the SD mount point has no filesystem. *readonly is set only
 * when this returns true. */
bool sd_repair_current_readonly(bool * readonly);

/* Called on the UI thread after the remount attempt. */
void sd_repair_complete(bool mounted, bool readonly);

/* When the SD mount is read-only, unmount it and check the filesystem on
 * a background thread. release_handles may close library files if the
 * first unmount is busy; it may be NULL. A failed unmount with no release
 * callback is not remembered, so a later call can close files and retry.
 * One remembered attempt per card, whether it repairs the filesystem or not.
 * A skipped read-only check warns the user. A skipped writable check triggered
 * only by the FAT dirty flag is diagnostic: it stays mounted and unchanged,
 * and no corruption warning is posted. */
sd_repair_kick_result_t sd_readonly_repair_kick(void (* release_handles)(void));

/* Same as sd_readonly_repair_kick(), and also checks a writable FAT card
 * the kernel reported as not safely removed. The kernel log is read on
 * each call; each FAT card insertion is one remembered attempt. */
sd_repair_kick_result_t sd_card_repair_kick(void (* release_handles)(void));

sd_repair_note_t sd_repair_take_note(void);

#endif
