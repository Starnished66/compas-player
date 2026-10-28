#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    char ** paths;
    int * order;
    int * continuation;
    int count, current, pending, mode, shuffle_pos;
    double position;
} queue_resume_t;
void queue_resume_free(queue_resume_t * state);
bool queue_resume_write(const char * path, const queue_resume_t * state);
/* Publish `name` inside an already-open directory.  The descriptor pins the
 * mount generation for the complete temporary-file/write/rename/cleanup
 * sequence; callers may run this from a worker without re-resolving a path. */
bool queue_resume_write_at(int dirfd, const char * name, const queue_resume_t * state);
bool queue_resume_read(const char * path, queue_resume_t * state);

/* The fixed-size front of the file: everything except the path list and the
 * shuffle arrays. A header-only rewrite changes the resume position and
 * current/pending/shuffle cursor of an existing file without materializing
 * the queue. */
typedef struct {
    int count, current, pending, mode, shuffle_pos;
    bool has_order, has_continuation;
    double position;
} queue_resume_header_t;
/* Atomically replaces `name` with a copy of itself carrying `header`. Fails
 * (leaving the old file) if the existing file is not a valid queue file with
 * the same count and the same shuffle-array layout. Same temp-file, fsync and
 * rename sequence as queue_resume_write_at(). */
bool queue_resume_rewrite_header_at(int dirfd, const char * name, const queue_resume_header_t * header);
