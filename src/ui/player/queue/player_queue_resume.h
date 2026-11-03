#ifndef PLAYER_QUEUE_RESUME_H
#define PLAYER_QUEUE_RESUME_H

#include <stdbool.h>
#include "../../../library/browser/files/file_browser.h"
#include "../../../library/metadata/db/metadata_db.h"

/* Queue directory helpers and SD pinning used by the queue install code in
 * gui_player.c. The checkpoint, restore and resume entry points stay declared
 * in gui_player.h. */
int open_sd_queue_dir(void);
int open_queue_dir_from_snapshot(const metadata_db_snapshot_t *snapshot);
int open_queue_dir_from_file_index(const file_browser_index_t *index);
void queue_pin_sd_dir_if_needed(void);

#endif /* PLAYER_QUEUE_RESUME_H */
