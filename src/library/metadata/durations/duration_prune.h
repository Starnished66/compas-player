#ifndef DURATION_PRUNE_H
#define DURATION_PRUNE_H

/* After the library database changed (rescan, targeted refresh): drop the
 * duration-cache entries whose track is no longer in the database with the
 * same path, mtime and size. One detached worker walks the database once and
 * prunes; nothing runs on the caller's thread beyond spawning it, a second
 * request while one runs is coalesced, and an empty or unreadable database
 * never prunes. */
void duration_prune_request(void);

#endif
