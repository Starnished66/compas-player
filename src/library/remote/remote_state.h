#ifndef REMOTE_STATE_H
#define REMOTE_STATE_H

#include <stdbool.h>
#include <stdint.h>

/* Path-keyed ratings/play counts for remote tracks and unscanned files,
 * plus album:// rating rows. Scanning migrates file rows into tagcache;
 * synthetic album rows stay in this sidecar. */

typedef struct { char *path; int32_t rating; } remote_state_rating_t;
/* Caller frees each copied path and the array. */
remote_state_rating_t *remote_state_list_prefix(const char *prefix, int *count);
/* Cheap change token for refreshing visible rating lists. */
unsigned remote_state_revision(void);
/* Publish a rating change stored outside the sidecar (tagcache). */
void remote_state_note_rating_change(void);
/* Drops the in-RAM sidecar so the next access rereads the file on the
 * currently mounted SD card. */
void remote_state_drop(void);
bool remote_state_get(const char * path, int32_t * rating, int32_t * playcount, int32_t * last_played);
/* False when the row could not be created or the sidecar was not saved
 * (an existing row keeps the new value in RAM either way). */
bool remote_state_set_rating(const char * path, int32_t rating);
void remote_state_add_play(const char * path, int32_t now);
/* Removes only durably migrated tuples that have not changed since the snapshot. */
void remote_state_remove_matches(const remote_state_rating_t *rows, const int32_t (*tuples)[3],
                                  const bool *migrated, int count);

#endif /* REMOTE_STATE_H */
