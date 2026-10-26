#include "emulator_registry.h"
#include "../../network/store/plugins/store_common.h"

#include "cJSON.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define MANIFEST_LIMIT (16u << 10)

static void set_error(char * error, size_t size, const char * text) {
    if (error && size) snprintf(error, size, "%s", text);
}

bool emulator_id_valid(const char * id) {
    if (!id || !*id || strlen(id) > 32 || (!store_ascii_lower(*id) && !store_ascii_digit(*id))) return false;
    for (const char * p = id; *p; p++) {
        if (!store_ascii_lower(*p) && !store_ascii_digit(*p) && *p != '_' && *p != '-') return false;
    }
    return true;
}

/* A plain file or folder name: no separators, not "." or "..". */
static bool plain_name(const char * s, size_t max) {
    return store_safe_text(s, max, false) && !strchr(s, '/') && strcmp(s, ".") != 0 && strcmp(s, "..") != 0;
}

static bool extension_valid(const char * s) {
    if (!s || !*s || strlen(s) > 7) return false;
    for (; *s; s++) {
        if (!store_ascii_lower(*s) && !store_ascii_digit(*s)) return false;
    }
    return true;
}

static bool parse_system(const cJSON * item, emulator_system_t * out) {
    const char * id = store_json_string(item, "id");
    const char * name = store_json_string(item, "name");
    const char * dir = store_json_string(item, "games_dir");
    const cJSON * extensions = store_json_field(item, "extensions");
    if (!cJSON_IsObject(item) || !store_token_valid(id, 15, false) || !store_safe_text(name, 32, false) ||
        !plain_name(dir, 63) || !cJSON_IsArray(extensions)) {
        return false;
    }
    int n = cJSON_GetArraySize(extensions);
    if (n < 1 || n > EMULATOR_MAX_EXTENSIONS) return false;
    memset(out, 0, sizeof(*out));
    snprintf(out->id, sizeof(out->id), "%s", id);
    snprintf(out->name, sizeof(out->name), "%s", name);
    snprintf(out->games_dir, sizeof(out->games_dir), "%s", dir);
    for (int i = 0; i < n; i++) {
        const cJSON * e = cJSON_GetArrayItem(extensions, i);
        if (!cJSON_IsString(e) || !extension_valid(e->valuestring)) return false;
        snprintf(out->extensions[i], sizeof(out->extensions[i]), "%s", e->valuestring);
    }
    out->extension_count = n;
    return true;
}

bool emulator_manifest_parse(const char * json, size_t length, emulator_manifest_t * out,
                             char * error, size_t error_size) {
    set_error(error, error_size, "invalid manifest");
    if (!json || !out || length > MANIFEST_LIMIT) return false;
    cJSON * root = cJSON_ParseWithLength(json, length);
    bool ok = false;
    emulator_manifest_t m;
    memset(&m, 0, sizeof(m));

    const char * id = store_json_string(root, "id");
    const char * name = store_json_string(root, "name");
    const char * version = store_json_string(root, "version");
    const char * exec = store_json_string(root, "exec");
    const char * author = store_json_string(root, "author");
    const char * license = store_json_string(root, "license");
    const char * description = store_json_string(root, "description");
    const cJSON * systems = store_json_field(root, "systems");
    uint32_t protocol;

    if (!cJSON_IsObject(root)) {
        set_error(error, error_size, "not a JSON object");
    } else if (!store_json_uint(root, "protocol", 0, UINT32_MAX, &protocol) || protocol != EMULATOR_PROTOCOL) {
        set_error(error, error_size, "unsupported protocol");
    } else if (!emulator_id_valid(id)) {
        set_error(error, error_size, "bad id");
    } else if (!store_safe_text(name, 64, false) || !store_safe_text(version, 31, false)) {
        set_error(error, error_size, "bad name or version");
    } else if (!plain_name(exec, 63)) {
        set_error(error, error_size, "bad exec");
    } else if ((store_json_field(root, "author") && !store_safe_text(author, 64, true)) ||
               (store_json_field(root, "license") && !store_safe_text(license, 64, true)) ||
               (store_json_field(root, "description") && !store_safe_text(description, 400, true))) {
        set_error(error, error_size, "bad author, license or description");
    } else if (!cJSON_IsArray(systems) || cJSON_GetArraySize(systems) < 1 ||
               cJSON_GetArraySize(systems) > EMULATOR_MAX_SYSTEMS) {
        set_error(error, error_size, "bad systems");
    } else {
        snprintf(m.id, sizeof(m.id), "%s", id);
        snprintf(m.name, sizeof(m.name), "%s", name);
        snprintf(m.version, sizeof(m.version), "%s", version);
        snprintf(m.exec, sizeof(m.exec), "%s", exec);
        m.free_player_memory = cJSON_IsTrue(store_json_field(root, "free_player_memory"));
        snprintf(m.author, sizeof(m.author), "%s", author ? author : "");
        snprintf(m.license, sizeof(m.license), "%s", license ? license : "");
        snprintf(m.description, sizeof(m.description), "%s", description ? description : "");
        ok = true;
        for (int i = 0; i < cJSON_GetArraySize(systems) && ok; i++) {
            ok = parse_system(cJSON_GetArrayItem(systems, i), &m.systems[i]);
            m.system_count = i + 1;
        }
        if (ok) {
            *out = m;
            set_error(error, error_size, "");
        } else {
            set_error(error, error_size, "bad system entry");
        }
    }
    cJSON_Delete(root);
    return ok;
}

static bool read_small_file(const char * path, char ** data, size_t * length) {
    FILE * f = fopen(path, "rb");
    if (!f) return false;
    char * buffer = malloc(MANIFEST_LIMIT + 1);
    size_t n = buffer ? fread(buffer, 1, MANIFEST_LIMIT + 1, f) : 0;
    bool ok = buffer && !ferror(f) && n <= MANIFEST_LIMIT;
    fclose(f);
    if (!ok) {
        free(buffer);
        return false;
    }
    *data = buffer;
    *length = n;
    return true;
}

bool emulator_manifest_load_dir(const char * dir, const char * expected_id, emulator_manifest_t * out,
                                char * error, size_t error_size) {
    char path[512];
    if (snprintf(path, sizeof(path), "%s/emulator.json", dir) >= (int) sizeof(path)) {
        set_error(error, error_size, "path too long");
        return false;
    }
    char * json;
    size_t length;
    if (!read_small_file(path, &json, &length)) {
        set_error(error, error_size, "no readable emulator.json");
        return false;
    }
    emulator_manifest_t m;
    bool ok = emulator_manifest_parse(json, length, &m, error, error_size);
    free(json);
    if (!ok) return false;
    if (strcmp(m.id, expected_id) != 0) {
        set_error(error, error_size, "id does not match folder");
        return false;
    }
    struct stat st;
    if (snprintf(path, sizeof(path), "%s/%s", dir, m.exec) >= (int) sizeof(path) ||
        stat(path, &st) != 0 || !S_ISREG(st.st_mode) || access(path, X_OK) != 0) {
        set_error(error, error_size, "exec missing or not executable");
        return false;
    }
    *out = m;
    return true;
}

bool emulator_manifest_load(const char * root, const char * folder, emulator_manifest_t * out,
                            char * error, size_t error_size) {
    char dir[512];
    if (snprintf(dir, sizeof(dir), "%s/%s", root, folder) >= (int) sizeof(dir)) {
        set_error(error, error_size, "path too long");
        return false;
    }
    return emulator_manifest_load_dir(dir, folder, out, error, error_size);
}

static int manifest_compare(const void * a, const void * b) {
    return strcasecmp(((const emulator_manifest_t *) a)->name, ((const emulator_manifest_t *) b)->name);
}

emulator_manifest_t * emulator_registry_scan(const char * root, int * count) {
    *count = 0;
    DIR * dir = opendir(root);
    if (!dir) return NULL;
    emulator_manifest_t * list = calloc(EMULATOR_MAX_INSTALLED, sizeof(*list));
    struct dirent * de;
    while (list && *count < EMULATOR_MAX_INSTALLED && (de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') continue; /* includes the store's staging folders */
        char error[64];
        if (emulator_manifest_load(root, de->d_name, &list[*count], error, sizeof(error))) {
            (*count)++;
        } else {
            fprintf(stderr, "[emulators] ignoring %s: %s\n", de->d_name, error);
        }
    }
    closedir(dir);
    if (!list || *count == 0) {
        free(list);
        *count = 0;
        return NULL;
    }
    qsort(list, (size_t) *count, sizeof(*list), manifest_compare);
    return list;
}

bool emulator_games_dir(char * out, size_t size, const char * sd_root, const emulator_system_t * system) {
    int n = snprintf(out, size, "%s/Games/%s", sd_root, system->games_dir);
    return n > 0 && (size_t) n < size;
}

static int game_compare(const void * a, const void * b) {
    return strcasecmp(((const emulator_game_t *) a)->label, ((const emulator_game_t *) b)->label);
}

static bool extension_matches(const emulator_system_t * system, const char * ext) {
    for (int i = 0; i < system->extension_count; i++) {
        if (strcasecmp(system->extensions[i], ext) == 0) return true;
    }
    return false;
}

/* Appends the matching games of one folder; a missing folder adds nothing. */
static void append_games(const char * folder, const emulator_system_t * system, emulator_game_t ** list, int * count,
                         int * capacity) {
    DIR * dir = opendir(folder);
    if (!dir) return;
    emulator_game_t * games = *list;
    int n = *count;
    struct dirent * de;
    while ((de = readdir(dir)) != NULL) {
        const char * dot = strrchr(de->d_name, '.');
        if (de->d_name[0] == '.' || !dot || dot == de->d_name || !extension_matches(system, dot + 1)) continue;
        emulator_game_t game;
        int len = snprintf(game.path, sizeof(game.path), "%s/%s", folder, de->d_name);
        struct stat st;
        if (len < 0 || (size_t) len >= sizeof(game.path) || stat(game.path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        snprintf(game.label, sizeof(game.label), "%.*s", (int) (dot - de->d_name), de->d_name);
        if (n == *capacity) {
            int grown_capacity = *capacity ? *capacity * 2 : 16;
            emulator_game_t * grown = realloc(games, sizeof(*games) * (size_t) grown_capacity);
            if (!grown) break;
            games = grown;
            *capacity = grown_capacity;
        }
        games[n++] = game;
    }
    closedir(dir);
    *list = games;
    *count = n;
}

emulator_game_t * emulator_registry_list_games(const char * sd_root, const char * emulator_dir,
                                               const emulator_system_t * system, int * count) {
    *count = 0;
    emulator_game_t * games = NULL;
    int capacity = 0;
    char folder[512];
    if (emulator_games_dir(folder, sizeof(folder), sd_root, system)) append_games(folder, system, &games, count, &capacity);
    int n = emulator_dir ? snprintf(folder, sizeof(folder), "%s/games/%s", emulator_dir, system->games_dir) : -1;
    if (n > 0 && (size_t) n < sizeof(folder)) append_games(folder, system, &games, count, &capacity);
    if (*count > 1) qsort(games, (size_t) *count, sizeof(*games), game_compare);
    return games;
}
