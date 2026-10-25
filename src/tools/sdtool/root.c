#include "sdtool.h"
#include "storage_paths.h"
#include "cJSON.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The card is read from wherever the computer mounted it, but the database
 * stores the paths the player sees (SD_DEVICE_MOUNT/...), so a card scanned
 * here is usable on the player without rewriting. */

static char card_root[PATH_MAX];

bool sdtool_root_set(const char *root) {
    size_t length = root ? strlen(root) : 0;
    if (length == 0 || length >= sizeof(card_root)) return false;
    memcpy(card_root, root, length + 1);
    /* Drive paths such as "E:\" arrive with backslashes. */
    for (char *p = card_root; *p; p++)
        if (*p == '\\') *p = '/';
    while (length > 1 && card_root[length - 1] == '/' &&
           !(length == 3 && card_root[1] == ':'))
        card_root[--length] = '\0';
    return true;
}

const char *sdtool_root(void) {
    return card_root;
}

const char *storage_sd_path(const char *relative) {
    static _Thread_local char paths[4][PATH_MAX];
    static _Thread_local unsigned next;
    char *out = paths[next++ % 4];
    size_t root_length = strlen(card_root);
    bool bare_root = root_length > 0 && card_root[root_length - 1] == '/';
    int length = snprintf(out, PATH_MAX, "%s%s%s", card_root, bare_root ? "" : "/", relative);
    /* A path that does not fit names nothing, so its use fails cleanly. */
    if (length < 0 || length >= PATH_MAX) out[0] = '\0';
    return out;
}

const char *storage_stored_path(const char *stored, char *out, size_t size) {
    size_t mount_length = strlen(SD_DEVICE_MOUNT);
    if (strncmp(stored, SD_DEVICE_MOUNT "/", mount_length + 1) != 0) return stored;
    if (snprintf(out, size, "%s", storage_sd_path(stored + mount_length + 1)) >= (int) size)
        return stored;
    return out;
}

bool sdtool_root_device_model(char *out, size_t size) {
    FILE *file = fopen(storage_sd_path(COMPAS_STORAGE_DIR "/device.json"), "rb");
    if (!file) return false;
    char text[1024];
    size_t length = fread(text, 1, sizeof(text) - 1, file);
    fclose(file);
    text[length] = '\0';
    cJSON *json = cJSON_Parse(text);
    const cJSON *model = json ? cJSON_GetObjectItemCaseSensitive(json, "model") : NULL;
    bool found = cJSON_IsString(model) && model->valuestring &&
                 snprintf(out, size, "%s", model->valuestring) < (int) size;
    cJSON_Delete(json);
    return found;
}
