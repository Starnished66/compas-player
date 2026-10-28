#ifndef APP_APPROVAL_H
#define APP_APPROVAL_H

#include <stdbool.h>

/* The user's approval to run an external app, kept on the player's internal
 * storage (not the SD card, which a plugin or a computer can write). An
 * approval is the app id plus the SHA-256 of its executable, so an app whose
 * executable changed has to be approved again. One line per app:
 * "<id> <sha256>"; granting replaces the id's earlier line. */

#ifdef HOST_BUILD
#define APP_APPROVAL_FILE "./apps_approved.txt"
#else
#define APP_APPROVAL_FILE "/usr/data/apps_approved.txt"
#endif

/* True when file holds exactly this id and executable hash. */
bool app_approval_granted(const char * file, const char * id, const char * sha256);

/* Records the approval, replacing any earlier one of the id. False when the
 * arguments are not a valid id and hash or the file cannot be written. */
bool app_approval_grant(const char * file, const char * id, const char * sha256);

#endif
