/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef COMPAS_BOOT_TRACE_H
#define COMPAS_BOOT_TRACE_H

#include <stdbool.h>
#include <stddef.h>

#define BOOT_TRACE_PATH_BYTES 1024U
#define BOOT_TRACE_INITIALIZER \
	{ -1, -1, 0, 0, 0, 0, { 0 }, { 0 }, { 0 } }

typedef struct boot_trace {
	int fd;
	int lock_fd;
	int initialized;
	int failed;
	volatile int busy;
	size_t bytes;
	char path[BOOT_TRACE_PATH_BYTES];
	char boot_id[64];
	char launch_id[37];
} boot_trace_t;

/* Open an append-only persistent trace, rotating at most eight generations.
 * `absolute_path` must name a file on a writable persistent filesystem.
 * The caller should initialize the context with BOOT_TRACE_INITIALIZER. */
int boot_trace_open(boot_trace_t *trace, const char *absolute_path);

/* Append and sync one bounded checkpoint. Use labels such as
 * "before:gui_init" and "after:gui_init" to bracket startup steps. */
int boot_trace_checkpoint(boot_trace_t *trace, const char *step);

/* Release the per-path lock and close the trace. Safe after failed open. */
void boot_trace_close(boot_trace_t *trace);

/* Whether Settings > System > Developer Options > "Enable debug logging"
 * (db_logging_enabled=1) is saved in /usr/data/.compas/settings.txt. For
 * code that must decide before settings_load() (boot traces) or that runs in
 * the bootloader, which never loads settings. Reads the file directly with a
 * bounded line count and no allocation; false when missing or unreadable
 * (logging defaults off). */
bool logging_enabled_in_settings_file(void);

/* Same check against an explicit path (the function above passes the real
 * settings path). Exposed so tests can use a temporary file. */
bool logging_enabled_in_settings_path(const char *path);

#endif /* COMPAS_BOOT_TRACE_H */
