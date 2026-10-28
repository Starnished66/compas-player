#define _GNU_SOURCE
#include <assert.h>
#include <limits.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "src/core/system/runtime/swap_pressure.h"
#include "src/library/playlists/queue_resume.h"
#include "src/ui/application/gui_screen_off_memory.h"
#define due gui_screen_off_memory_due

static void read_all(int dirfd, const char * name, char ** buf, long * len) {
    int fd = openat(dirfd, name, O_RDONLY);
    assert(fd >= 0);
    struct stat st; assert(fstat(fd, &st) == 0);
    *buf = malloc((size_t) st.st_size); *len = st.st_size;
    assert(read(fd, *buf, (size_t) st.st_size) == st.st_size);
    close(fd);
}

static queue_resume_t make(int count, int current, double position, bool shuffle) {
    queue_resume_t q = { .count = count, .current = current, .pending = 0, .mode = shuffle ? 3 : 0,
                         .shuffle_pos = shuffle ? current : 0, .position = position };
    q.paths = calloc((size_t) count, sizeof(char *));
    for (int i = 0; i < count; i++) { char p[64]; snprintf(p, sizeof p, "/music/track %d.flac", i); q.paths[i] = strdup(p); }
    if (shuffle) { q.order = malloc(sizeof(int) * (size_t) count); for (int i = 0; i < count; i++) q.order[i] = i; }
    return q;
}

static queue_resume_header_t header_of(const queue_resume_t * q) {
    return (queue_resume_header_t){ q->count, q->current, q->pending, q->mode, q->shuffle_pos,
                                    q->order != NULL, q->continuation != NULL, q->position };
}

int main(void) {
    /* swap pressure math */
    assert(!swap_pressure_next(false, 0, 0));
    assert(!swap_pressure_next(false, 24000, 24000));
    assert(!swap_pressure_next(false, 24000, 6000));      /* exactly 25% free */
    assert(swap_pressure_next(false, 24000, 5999));
    assert(swap_pressure_next(true, 24000, 8000));         /* hysteresis: 33% still pressure */
    assert(!swap_pressure_next(true, 24000, 8400));        /* 35% ends it */
    assert(!swap_pressure_next(true, 0, 0));
    assert(!swap_pressure_next(false, 100, 500));          /* free clamps to total */

    /* screen-off policy */
    assert(!due(false, true, 59999));
    assert(due(false, true, 60000));
    assert(!due(true, true, 90000));
    assert(!due(false, false, 90000));

    char tmpl[] = "/tmp/compas-qr-XXXXXX";
    assert(mkdtemp(tmpl));
    int dirfd = open(tmpl, O_RDONLY | O_DIRECTORY);
    assert(dirfd >= 0);

    /* unchanged queue: header-only rewrite == a full write with the new position */
    for (int shuffle = 0; shuffle < 2; shuffle++) {
        queue_resume_t a = make(5, 2, 10.0, shuffle);
        assert(queue_resume_write_at(dirfd, "queue.bin", &a));
        queue_resume_header_t h = header_of(&a);
        h.position = 77.5;
        assert(queue_resume_rewrite_header_at(dirfd, "queue.bin", &h));
        a.position = 77.5;
        assert(queue_resume_write_at(dirfd, "full.bin", &a));
        char *x, *y; long lx, ly;
        read_all(dirfd, "queue.bin", &x, &lx); read_all(dirfd, "full.bin", &y, &ly);
        assert(lx == ly && memcmp(x, y, (size_t) lx) == 0);
        free(x); free(y);
        queue_resume_free(&a);
    }

    /* cursor change (next track) rewritten with the header, then resumed */
    {
        queue_resume_t a = make(5, 1, 3.0, true);
        assert(queue_resume_write_at(dirfd, "queue.bin", &a));
        queue_resume_header_t h = header_of(&a);
        h.current = 3; h.shuffle_pos = 3; h.position = 42.0;
        assert(queue_resume_rewrite_header_at(dirfd, "queue.bin", &h));
        char path[128]; snprintf(path, sizeof path, "%s/queue.bin", tmpl);
        queue_resume_t r; assert(queue_resume_read(path, &r));
        assert(r.count == 5 && r.current == 3 && r.shuffle_pos == 3 && r.position == 42.0);
        assert(strcmp(r.paths[4], "/music/track 4.flac") == 0 && r.order && r.order[3] == 3);
        queue_resume_free(&r); queue_resume_free(&a);
    }

    /* changed queue (count or layout differs): header rewrite refuses, old file survives */
    {
        queue_resume_t a = make(5, 2, 10.0, true);
        assert(queue_resume_write_at(dirfd, "queue.bin", &a));
        queue_resume_header_t h = header_of(&a);
        h.count = 6;
        assert(!queue_resume_rewrite_header_at(dirfd, "queue.bin", &h));
        h = header_of(&a); h.has_order = false; h.mode = 0;
        assert(!queue_resume_rewrite_header_at(dirfd, "queue.bin", &h));
        assert(!queue_resume_rewrite_header_at(dirfd, "missing.bin", &h));
        char path[128]; snprintf(path, sizeof path, "%s/queue.bin", tmpl);
        queue_resume_t r; assert(queue_resume_read(path, &r) && r.count == 5 && r.position == 10.0);
        queue_resume_free(&r);
        /* full write of the changed queue restores consistency */
        queue_resume_t b = make(6, 4, 1.0, true);
        assert(queue_resume_write_at(dirfd, "queue.bin", &b));
        h = header_of(&b); h.position = 9.0;
        assert(queue_resume_rewrite_header_at(dirfd, "queue.bin", &h));
        assert(queue_resume_read(path, &r) && r.count == 6 && r.current == 4 && r.position == 9.0);
        queue_resume_free(&r); queue_resume_free(&a); queue_resume_free(&b);
    }

    /* crash safety: a stale temp file left by an interrupted rewrite never replaces the file */
    {
        queue_resume_t a = make(3, 0, 5.0, false);
        assert(queue_resume_write_at(dirfd, "queue.bin", &a));
        char tmp[NAME_MAX]; snprintf(tmp, sizeof tmp, ".queue.bin.%ld.0.tmp", (long) getpid());
        int fd = openat(dirfd, tmp, O_WRONLY | O_CREAT, 0600); assert(fd >= 0);
        assert(write(fd, "HIBYQ001garbage", 15) == 15); close(fd);
        queue_resume_header_t h = header_of(&a); h.position = 6.0;
        assert(queue_resume_rewrite_header_at(dirfd, "queue.bin", &h));
        char path[128]; snprintf(path, sizeof path, "%s/queue.bin", tmpl);
        queue_resume_t r; assert(queue_resume_read(path, &r) && r.position == 6.0 && r.count == 3);
        queue_resume_free(&r); queue_resume_free(&a);
    }

    close(dirfd);
    char cmd[160]; snprintf(cmd, sizeof cmd, "rm -rf '%s'", tmpl);
    (void) !system(cmd);
    puts("screen-off memory tests passed");
    return 0;
}
