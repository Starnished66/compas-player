#ifndef SDTOOL_H
#define SDTOOL_H

#include <stdbool.h>
#include <stddef.h>

#define SDTOOL_VERSION "1.0.0"

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

/* Commands. Each prints JSON lines on stdout and returns an exit code. */
int sdtool_cmd_info(const sdtool_options_t *options);
int sdtool_cmd_scan(const sdtool_options_t *options);
int sdtool_cmd_art(const sdtool_options_t *options);

/* Opens the card's database the way the player does, rebuilding one that
 * fails to load. Emits the error line and returns false on failure. */
bool sdtool_open_database(void);

#endif /* SDTOOL_H */
