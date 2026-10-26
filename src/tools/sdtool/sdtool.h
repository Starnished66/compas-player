#ifndef SDTOOL_H
#define SDTOOL_H

#include <stdbool.h>
#include <stddef.h>
#include "../../library/metadata/db/metadata_db.h"

#define SDTOOL_VERSION "1.1.0"
/* Source revision this binary was built from (version.c). */
extern const char sdtool_build[];

/* Boards the player ships for, as written in device.json. */
#define SDTOOL_DEFAULT_BOARD "r1"

typedef struct {
    const char *card_root; /* where the card is mounted on this computer */
    bool full;             /* scan: re-read every file's tags */
    const char *board;     /* art: --board value, or NULL for device.json/default */
} sdtool_options_t;

/* Process exit codes. */
enum { SDTOOL_OK = 0, SDTOOL_FAILED = 1, SDTOOL_USAGE = 2 };

/* root.c: the card root and its relation to the paths the database stores. */
bool sdtool_root_set(const char *card_root);
const char *sdtool_root(void);
/* Reads "model" from <card>/.compas/device.json into out; false when absent. */
bool sdtool_root_device_model(char *out, size_t size);

/* mtime.c: card file times in the player's frame (see there). */
void sdtool_mtime_set_manual(long seconds);
void sdtool_mtime_set_calibrated(long seconds);
long sdtool_mtime_offset(const char *path);
/* The corrections beyond the operating system's: --mtime-offset plus calibration. */
long sdtool_mtime_extra(void);
/* mtime_calibrate.c: derives the offset from the open database's rows. */
void sdtool_mtime_calibrate(void);

/* Commands. Each prints JSON lines on stdout and returns an exit code. */
int sdtool_cmd_info(const sdtool_options_t *options);
int sdtool_cmd_scan(const sdtool_options_t *options);
int sdtool_cmd_art(const sdtool_options_t *options);

/* How the database opened: the player's load outcome, and whether it is an
 * empty stand-in for one that could not be read (a "preserved rebuild"). */
typedef struct {
    metadata_db_load_outcome_t outcome;
    bool preserve_rebuild;
} sdtool_database_t;

/* Opens the card's database the way the player's scan does. Emits the error
 * line and returns false on failure. */
bool sdtool_open_database(sdtool_database_t *db);

#endif /* SDTOOL_H */
