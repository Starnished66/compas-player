#include "sdtool.h"
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef __APPLE__
#include <sys/mount.h>
#endif

/* File times on the card, in the frame the player reads them in.
 *
 * FAT stores local wall-clock time. The player mounts it with no tz= option
 * and never sets the kernel zone, so it reads the stored digits as UTC.
 * Whatever the computer reports must be moved into that frame before it is
 * written to the database or a cover cache, or the player would see every
 * file as changed:
 *   - Windows reads FAT through FileTimeToLocalFileTime: add the zone offset;
 *   - macOS msdos does the same with the current offset;
 *   - Linux vfat without tz= already reports the raw digits: nothing to add;
 *   - exFAT is not converted (its behaviour is still to be measured on a
 *     device), and neither are NTFS or other volumes.
 * Two corrections are added: --mtime-offset, for a card the above gets wrong,
 * and the offset sdtool_mtime_calibrate() derives from a database the player
 * wrote. A wrong conversion costs a rescan, never a stale row: an edited file
 * differs from its recorded time whatever offset is applied to both. */

static long manual_offset;
static long calibrated_offset;

void sdtool_mtime_set_manual(long seconds) { manual_offset = seconds; }
void sdtool_mtime_set_calibrated(long seconds) { calibrated_offset = seconds; }

#if defined(__APPLE__)

static long platform_offset(const char *path) {
    struct statfs fs;
    if (statfs(path, &fs) != 0 || strcmp(fs.f_fstypename, "msdos") != 0) return 0;
    time_t now = time(NULL);
    struct tm local;
    return localtime_r(&now, &local) ? local.tm_gmtoff : 0;
}

#elif defined(_WIN32)

#include "compat_internal.h"
#include <windows.h>

static long platform_offset(const char *path) {
    wchar_t wide[PATH_MAX], root[MAX_PATH], fs[MAX_PATH];
    if (!compat_path_to_wide(path, wide, PATH_MAX) || !GetVolumePathNameW(wide, root, MAX_PATH) ||
        !GetVolumeInformationW(root, NULL, 0, NULL, NULL, NULL, fs, MAX_PATH))
        return 0;
    /* Only FAT12/16/32: exFAT and NTFS are left alone. */
    return wcsncmp(fs, L"FAT", 3) == 0 ? (long) compat_local_offset(time(NULL)) : 0;
}

#else

static long platform_offset(const char *path) {
    (void) path;
    return 0;
}

#endif

long sdtool_mtime_offset(const char *path) {
    return platform_offset(path) + manual_offset + calibrated_offset;
}

#ifndef _WIN32
#undef stat
int sdtool_stat(const char *path, struct stat *st) {
    int rc = stat(path, st);
    if (rc == 0 && S_ISREG(st->st_mode)) st->st_mtime += sdtool_mtime_offset(path);
    return rc;
}
#endif
