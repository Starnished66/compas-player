#ifndef STORAGE_PATHS_H
#define STORAGE_PATHS_H

#include <stddef.h>

/* Canonical persistent application data paths. The metadata database also
 * lives in .compas; the shared directory does not make it disposable. */
#define COMPAS_STORAGE_DIR ".compas"

/* Where the player mounts the SD card. Paths stored in the music database are
 * rooted here, whichever machine wrote them. */
#define SD_DEVICE_MOUNT "/data/mnt/sd_0"

#ifdef HOST_BUILD
#define SD_ROOT_DIR "."
#define INTERNAL_COMPAS_DIR "./.compas"
#define SETTINGS_FILE_PATH "./.compas/settings.txt"
#define PEQ_FILE_PATH "./.compas/peq.txt"
#define SUBSONIC_COMPAS_DIR "./.compas"
#define PLUGIN_STORAGE_ROOT "./.compas/plugins"
#define SD_COMPAS_ROOT "./.compas"
#else
#define SD_ROOT_DIR SD_DEVICE_MOUNT
#define INTERNAL_COMPAS_DIR "/usr/data/.compas"
#define SETTINGS_FILE_PATH "/usr/data/.compas/settings.txt"
#define PEQ_FILE_PATH "/usr/data/.compas/peq.txt"
#define SUBSONIC_COMPAS_DIR "/usr/data/.compas"
#define PLUGIN_STORAGE_ROOT "/usr/data/.compas/plugins"
#define SD_COMPAS_ROOT SD_DEVICE_MOUNT "/.compas"
#endif

#ifdef COMPAS_RUNTIME_SD_ROOT
/* compas-sdtool works on a card mounted anywhere (or on a drive letter), so the
 * root is chosen at run time. storage_sd_path() names a file or folder below
 * the card; storage_stored_path() maps a path stored in the database to the
 * file it names on this machine. Both are provided by the tool. */
const char *storage_sd_path(const char *relative);
const char *storage_stored_path(const char *stored, char *out, size_t size);
#else
#define storage_sd_path(relative) (SD_ROOT_DIR "/" relative)
static inline const char *storage_stored_path(const char *stored, char *out, size_t size) {
    (void) out; (void) size;
    return stored;
}
#endif

#endif
