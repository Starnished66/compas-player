#include "storage_roots.h"

#include "../../library/browser/paths/library_paths.h"

#include <stdio.h>
#include <string.h>

size_t storage_roots_build(const char * sd_path, const usb_drive_t * drives, size_t drive_count,
                           storage_root_t * out, size_t max) {
    size_t count = 0;
    if (count < max) {
        memset(&out[count], 0, sizeof(out[count]));
        snprintf(out[count].id, sizeof(out[count].id), "sd");
        snprintf(out[count].path, sizeof(out[count].path), "%s", sd_path);
        count++;
    }
    for (size_t i = 0; i < drive_count && count < max; i++) {
        storage_root_t * root = &out[count++];
        memset(root, 0, sizeof(*root));
        /* The id follows the mount point (usb_0 -> usb0), so it stays the same
         * while the drive is plugged in. */
        const char * number = strrchr(drives[i].mount_point, '_');
        snprintf(root->id, sizeof(root->id), "usb%s", number ? number + 1 : "");
        snprintf(root->path, sizeof(root->path), "%s", drives[i].mount_point);
        snprintf(root->label, sizeof(root->label), "%s", drives[i].label);
        root->removable = true;
    }
    return count;
}

size_t storage_roots_list(storage_root_t * out, size_t max) {
    usb_drive_t drives[USB_DRIVE_MAX];
    size_t drive_count = usb_drive_list(drives, USB_DRIVE_MAX);
    return storage_roots_build(MUSIC_ROOT_DIR, drives, drive_count, out, max);
}

size_t storage_roots_drive_count(void) {
    usb_drive_t drives[USB_DRIVE_MAX];
    return usb_drive_list(drives, USB_DRIVE_MAX);
}

bool storage_roots_find_path(const storage_root_t * roots, size_t count, const char * path, size_t * index) {
    for (size_t i = 0; path && i < count; i++) {
        size_t length = strlen(roots[i].path);
        if (strncmp(path, roots[i].path, length) == 0 && (path[length] == '/' || path[length] == '\0')) {
            if (index) *index = i;
            return true;
        }
    }
    return false;
}

bool storage_roots_find_id(const storage_root_t * roots, size_t count, const char * id, size_t * index) {
    for (size_t i = 0; id && i < count; i++) {
        if (strcmp(roots[i].id, id) == 0) {
            if (index) *index = i;
            return true;
        }
    }
    return false;
}

const char * storage_root_name(const storage_root_t * root, const char * sd_default, const char * usb_default) {
    if (root->label[0]) return root->label;
    return root->removable ? usb_default : sd_default;
}
