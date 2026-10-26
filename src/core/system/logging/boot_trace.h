/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef COMPAS_BOOT_TRACE_H
#define COMPAS_BOOT_TRACE_H

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

#endif /* COMPAS_BOOT_TRACE_H */
