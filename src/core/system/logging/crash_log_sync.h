#ifndef CRASH_LOG_SYNC_H
#define CRASH_LOG_SYNC_H

/* Copies the loader's exit log and the player's crash log (and their .1
 * rotations) from internal storage into the SD card's logs folder, so they
 * can be read from the card. A copy is skipped when the card already holds a
 * file of the same size and modification time. Errors are ignored. */
void crash_log_sync_to_sd(void);

#endif /* CRASH_LOG_SYNC_H */
