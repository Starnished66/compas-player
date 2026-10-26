#include "boot_tuning.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Measured OOM, about 30s after boot: the player (~20 MB RSS) was killed in
 * fat_write_begin. free was the 954 KB min watermark, writeback was 9.4 MB,
 * dirty 0.85 MB, and the file LRU was unreclaimable. Defaults (dirty_ratio
 * 20 and dirty_background_ratio 10 of ~58 MB, expire 30s, min_free_kbytes
 * 954) let writeback pin more RAM than the player can spare. Cap dirty pages
 * at a few MB so writers stall first, expire a little sooner so that 30s
 * burst cannot rebuild, and start reclaim slightly earlier. Best-effort:
 * a failure is logged and boot continues. */
void boot_tune_vm_writeback(void) {
    static const struct { const char * path; const char * value; } settings[] = {
        /* Flusher starts at 1 MiB instead of ~10% (~6 MiB). */
        { "/proc/sys/vm/dirty_background_bytes", "1048576\n" },
        /* Writers block at 4 MiB instead of ~20% (~12 MiB). Must stay above
         * the background threshold; 4 MiB is under the 9.4 MiB of writeback
         * that was stuck when the player was killed. */
        { "/proc/sys/vm/dirty_bytes", "4194304\n" },
        /* 15s instead of 30s: still coarse on a slow card, but dirty pages
         * cannot sit for the whole window that matched the OOM. */
        { "/proc/sys/vm/dirty_expire_centisecs", "1500\n" },
        /* 2560 KB. The kill happened with free == the 954 KB watermark;
         * this starts reclaim ~1.6 MB sooner and is ~4% of 58 MB. */
        { "/proc/sys/vm/min_free_kbytes", "2560\n" },
    };
    for (size_t i = 0; i < sizeof settings / sizeof settings[0]; i++) {
        int fd = open(settings[i].path, O_WRONLY | O_CLOEXEC);
        if (fd < 0) {
            fprintf(stderr, "compas_loader: %s: %s\n", settings[i].path, strerror(errno));
            continue;
        }
        size_t len = strlen(settings[i].value);
        ssize_t wrote = write(fd, settings[i].value, len);
        if (wrote < 0)
            fprintf(stderr, "compas_loader: write %s failed: %s\n", settings[i].path, strerror(errno));
        else if ((size_t) wrote != len)
            fprintf(stderr, "compas_loader: write %s short (%zd)\n", settings[i].path, wrote);
        close(fd);
    }
}
