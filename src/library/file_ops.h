#ifndef FILE_OPS_H
#define FILE_OPS_H

#include <stdbool.h>

/* Copy, move and delete of files and folders for the File Manager, run on
 * one background worker that holds the USB storage write lease, so a PC
 * cannot mount the card meanwhile. Existing files are never overwritten: a
 * name that is taken gets a " (2)" style suffix. Symbolic links are not
 * copied (the item counts as not completed). */

typedef enum {
    FILE_OP_COPY,
    FILE_OP_MOVE,
    FILE_OP_DELETE,
} file_op_kind_t;

typedef struct {
    unsigned id;           /* changes with every started operation */
    bool running;
    bool stopped;          /* file_op_cancel() left items undone */
    bool storage_busy;     /* USB storage kept the card from being written */
    bool touched_library;  /* a folder, music file or playlist was involved */
    unsigned done;         /* top-level items finished, failed ones included */
    unsigned total;
    unsigned failed;       /* items not fully done (error, refused or stopped) */
    char current[256];     /* name of the item in progress */
} file_op_status_t;

/* Starts kind on count absolute paths (copied). dest_dir is the target
 * folder for copy and move, ignored for delete. False if an operation is
 * already running or the worker could not start. */
bool file_op_start(file_op_kind_t kind, char * const * paths, int count, const char * dest_dir);
void file_op_get_status(file_op_status_t * out);
bool file_op_running(void);
/* Stops after the current file; a partly copied file is removed. */
void file_op_cancel(void);

/* True for the player's own folders at the card root (.compas, .plugins):
 * deleting or moving them breaks features, loses data such as settings and
 * installed plugins, and may need a restart. */
bool file_op_is_player_folder(const char * path, const char * sd_root);

/* True when path is root itself or root followed by "/name/name..." with no
 * empty, "." or ".." name and no trailing slash. Such paths have one
 * spelling, so string prefix checks (inside a folder) hold. root may be
 * relative (the simulator's "./music"). */
bool file_op_path_in_root(const char * path, const char * root);

#endif /* FILE_OPS_H */
