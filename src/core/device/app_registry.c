#include "app_registry.h"
#include "../../network/store/plugins/store_common.h"

#include "cJSON.h"

#include <dirent.h>
#include <elf.h>
#include <limits.h>
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

const char * app_board_id(void) {
#if defined(BOARD_R3PROII)
    return "r3proii";
#elif defined(BOARD_R3II_2025)
    return "r3ii_2025";
#elif defined(BOARD_TEMPOTEC_V1)
    return "tempotec_v1";
#else
    return "r1";
#endif
}

bool app_id_valid(const char * id) {
    if (!id || !*id || strlen(id) > 32 || (!store_ascii_lower(*id) && !store_ascii_digit(*id))) return false;
    for (const char * p = id; *p; p++) {
        if (!store_ascii_lower(*p) && !store_ascii_digit(*p) && *p != '_' && *p != '-') return false;
    }
    return true;
}

/* A plain file name: no separators, not "." or "..". */
static bool plain_name(const char * s, size_t max) {
    return store_safe_text(s, max, false) && !strchr(s, '/') && !strchr(s, '\\') && strcmp(s, ".") != 0 &&
           strcmp(s, "..") != 0;
}

static bool extension_valid(const char * s) {
    if (!s || !*s || strlen(s) > 7) return false;
    for (; *s; s++) {
        if (!store_ascii_lower(*s) && !store_ascii_digit(*s)) return false;
    }
    return true;
}

static bool has_png_suffix(const char * name) {
    size_t n = strlen(name);
    return n > 4 && strcasecmp(name + n - 4, ".png") == 0;
}

static bool parse_opens(const cJSON * root, app_manifest_t * m) {
    const cJSON * opens = store_json_field(root, "opens");
    if (!opens) return true;
    int n = cJSON_GetArraySize(opens);
    if (!cJSON_IsArray(opens) || n > APP_MAX_OPENS) return false;
    for (int i = 0; i < n; i++) {
        const cJSON * e = cJSON_GetArrayItem(opens, i);
        if (!cJSON_IsString(e) || !extension_valid(e->valuestring)) return false;
        snprintf(m->opens[i], sizeof(m->opens[i]), "%s", e->valuestring);
    }
    m->opens_count = n;
    return true;
}

/* A board id: lower-case letters, digits, '_' and '-', 1..15 bytes. */
static bool board_name_valid(const char * s) {
    if (!s || !*s || strlen(s) > 15) return false;
    for (; *s; s++) {
        if (!store_ascii_lower(*s) && !store_ascii_digit(*s) && *s != '_' && *s != '-') return false;
    }
    return true;
}

static bool parse_boards(const cJSON * root, app_manifest_t * m) {
    const cJSON * boards = store_json_field(root, "boards");
    if (!boards) return true;
    int n = cJSON_GetArraySize(boards);
    if (!cJSON_IsArray(boards) || n < 1 || n > APP_MAX_BOARDS) return false;
    for (int i = 0; i < n; i++) {
        const cJSON * e = cJSON_GetArrayItem(boards, i);
        if (!cJSON_IsString(e) || !board_name_valid(e->valuestring)) return false;
        snprintf(m->boards[i], sizeof(m->boards[i]), "%s", e->valuestring);
    }
    m->boards_count = n;
    return true;
}

bool app_manifest_parse(const char * json, size_t length, app_manifest_t * out, char * error, size_t error_size) {
    set_error(error, error_size, "invalid manifest");
    if (!json || !out || length > MANIFEST_LIMIT) return false;
    cJSON * root = cJSON_ParseWithLength(json, length);
    bool ok = false;
    app_manifest_t m;
    memset(&m, 0, sizeof(m));

    const char * id = store_json_string(root, "id");
    const char * name = store_json_string(root, "name");
    const char * version = store_json_string(root, "version");
    const char * exec = store_json_string(root, "exec");
    const char * icon = store_json_string(root, "icon");
    const cJSON * memory = store_json_field(root, "free_player_memory");
    uint32_t protocol;

    if (!cJSON_IsObject(root)) {
        set_error(error, error_size, "not a JSON object");
    } else if (!store_json_uint(root, "protocol", 0, UINT32_MAX, &protocol) || protocol != APP_PROTOCOL) {
        set_error(error, error_size, "unsupported protocol");
    } else if (!app_id_valid(id)) {
        set_error(error, error_size, "bad id");
    } else if (!store_safe_text(name, 64, false) || !store_safe_text(version, 31, false)) {
        set_error(error, error_size, "bad name or version");
    } else if (!plain_name(exec, 63)) {
        set_error(error, error_size, "bad exec");
    } else if (store_json_field(root, "icon") && (!plain_name(icon, 63) || !has_png_suffix(icon))) {
        set_error(error, error_size, "bad icon");
    } else if (memory && !cJSON_IsBool(memory)) {
        set_error(error, error_size, "bad free_player_memory");
    } else if (!parse_opens(root, &m)) {
        set_error(error, error_size, "bad opens");
    } else if (!parse_boards(root, &m)) {
        set_error(error, error_size, "bad boards");
    } else {
        snprintf(m.id, sizeof(m.id), "%s", id);
        snprintf(m.name, sizeof(m.name), "%s", name);
        snprintf(m.version, sizeof(m.version), "%s", version);
        snprintf(m.exec, sizeof(m.exec), "%s", exec);
        snprintf(m.icon, sizeof(m.icon), "%s", icon ? icon : "");
        m.free_player_memory = cJSON_IsTrue(memory);
        *out = m;
        set_error(error, error_size, "");
        ok = true;
    }
    cJSON_Delete(root);
    return ok;
}

bool app_manifest_for_board(const app_manifest_t * manifest, const char * board) {
    if (manifest->boards_count == 0) return true;
    for (int i = 0; i < manifest->boards_count; i++) {
        if (board && strcmp(manifest->boards[i], board) == 0) return true;
    }
    return false;
}

bool app_manifest_opens_path(const app_manifest_t * manifest, const char * path) {
    const char * slash = strrchr(path, '/');
    const char * dot = strrchr(path, '.');
    if (!dot || (slash && dot < slash) || dot == path || dot[-1] == '/' || !dot[1]) return false;
    for (int i = 0; i < manifest->opens_count; i++) {
        if (strcasecmp(manifest->opens[i], dot + 1) == 0) return true;
    }
    return false;
}

/* The same rule as file_op_path_in_root(): aliases such as "sd_0/." or
 * "sd_0//" would reach the whole card. */
static bool canonical_below(const char * path, const char * root) {
    size_t root_len = strlen(root);
    if (strncmp(path, root, root_len) != 0 || path[root_len] != '/') return false;
    for (const char * part = path + root_len + 1;;) {
        const char * end = strchr(part, '/');
        size_t len = end ? (size_t) (end - part) : strlen(part);
        if (len == 0 || (len == 1 && part[0] == '.') || (len == 2 && part[0] == '.' && part[1] == '.')) return false;
        if (!end) return true;
        part = end + 1;
    }
}

const char * app_launch_file_check(const char * sd_root, const app_manifest_t * manifest, const char * file) {
    struct stat st;
    if (!file || !canonical_below(file, sd_root)) return "path must be a file on the SD card";
    if (strlen(file) >= PATH_MAX) return "path is too long";
    if (!app_manifest_opens_path(manifest, file)) return "the app does not open this kind of file";
    if (stat(file, &st) != 0 || !S_ISREG(st.st_mode)) return "no such file";
    return NULL;
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

/* A regular file (not a link) at dir/name. */
static bool plain_file_in(const char * dir, const char * name, char * path, size_t size) {
    struct stat st;
    return snprintf(path, size, "%s/%s", dir, name) < (int) size && lstat(path, &st) == 0 && S_ISREG(st.st_mode);
}

#define PHDR_TABLE_MAX (64u << 10)

/* True for a little-endian MIPS ELF executable with no program interpreter
 * in any of its program headers: a script or a dynamically linked program
 * would run against the player's libraries or not at all. */
static bool static_elf(const char * path) {
    FILE * f = fopen(path, "rb");
    if (!f) return false;
    unsigned char id[EI_NIDENT];
    bool ok = fread(id, 1, sizeof(id), f) == sizeof(id) && memcmp(id, ELFMAG, SELFMAG) == 0 &&
              id[EI_DATA] == ELFDATA2LSB && (id[EI_CLASS] == ELFCLASS32 || id[EI_CLASS] == ELFCLASS64);
    uint64_t phoff = 0;
    unsigned phentsize = 0, phnum = 0;
    if (ok && id[EI_CLASS] == ELFCLASS32) {
        Elf32_Ehdr h;
        ok = fseek(f, 0, SEEK_SET) == 0 && fread(&h, sizeof(h), 1, f) == 1 && h.e_machine == EM_MIPS &&
             (h.e_type == ET_EXEC || h.e_type == ET_DYN);
        phoff = h.e_phoff, phentsize = h.e_phentsize, phnum = h.e_phnum;
    } else if (ok) {
        Elf64_Ehdr h;
        ok = fseek(f, 0, SEEK_SET) == 0 && fread(&h, sizeof(h), 1, f) == 1 && h.e_machine == EM_MIPS &&
             (h.e_type == ET_EXEC || h.e_type == ET_DYN);
        phoff = h.e_phoff, phentsize = h.e_phentsize, phnum = h.e_phnum;
    }
    /* Every program header is read: an interpreter entry placed late must not slip through. */
    if (ok && (phentsize < 4 || phnum == 0 || (uint64_t) phentsize * phnum > PHDR_TABLE_MAX)) ok = false;
    unsigned char * table = ok ? malloc((size_t) phentsize * phnum) : NULL;
    ok = ok && table && fseek(f, (long) phoff, SEEK_SET) == 0 && fread(table, phentsize, phnum, f) == phnum;
    for (unsigned i = 0; ok && i < phnum; i++) {
        uint32_t type;
        memcpy(&type, table + (size_t) i * phentsize, sizeof(type));
        if (type == PT_INTERP) ok = false;
    }
    free(table);
    fclose(f);
    return ok;
}

bool app_manifest_load_dir(const char * dir, const char * expected_id, app_manifest_t * out, char * error,
                           size_t error_size) {
    char path[512];
    if (snprintf(path, sizeof(path), "%s/app.json", dir) >= (int) sizeof(path)) {
        set_error(error, error_size, "path too long");
        return false;
    }
    char * json;
    size_t length;
    if (!read_small_file(path, &json, &length)) {
        set_error(error, error_size, "no readable app.json");
        return false;
    }
    app_manifest_t m;
    bool ok = app_manifest_parse(json, length, &m, error, error_size);
    free(json);
    if (!ok) return false;
    if (strcmp(m.id, expected_id) != 0) {
        set_error(error, error_size, "id does not match folder");
        return false;
    }
    if (!plain_file_in(dir, m.exec, path, sizeof(path)) || access(path, X_OK) != 0) {
        set_error(error, error_size, "exec missing or not executable");
        return false;
    }
    if (!static_elf(path)) {
        set_error(error, error_size, "exec is not a static ELF program");
        return false;
    }
    if (m.icon[0] && !plain_file_in(dir, m.icon, path, sizeof(path))) m.icon[0] = '\0';
    *out = m;
    return true;
}

static int manifest_compare(const void * a, const void * b) {
    return strcasecmp(((const app_manifest_t *) a)->name, ((const app_manifest_t *) b)->name);
}

app_manifest_t * app_registry_scan(const char * root, const char * board, int * count) {
    *count = 0;
    DIR * dir = opendir(root);
    if (!dir) return NULL;
    app_manifest_t * list = calloc(APP_MAX_INSTALLED, sizeof(*list));
    struct dirent * de;
    while (list && *count < APP_MAX_INSTALLED && (de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char folder[512], error[64];
        if (snprintf(folder, sizeof(folder), "%s/%s", root, de->d_name) >= (int) sizeof(folder)) continue;
        if (!app_manifest_load_dir(folder, de->d_name, &list[*count], error, sizeof(error))) {
            fprintf(stderr, "[apps] ignoring %s: %s\n", de->d_name, error);
        } else if (app_manifest_for_board(&list[*count], board)) {
            (*count)++;
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

bool app_registry_find(const char * root, const char * board, const char * id, app_manifest_t * out) {
    char folder[512], error[64];
    if (!app_id_valid(id) || snprintf(folder, sizeof(folder), "%s/%s", root, id) >= (int) sizeof(folder)) return false;
    return app_manifest_load_dir(folder, id, out, error, sizeof(error)) && app_manifest_for_board(out, board);
}

bool app_dir_path(char * out, size_t size, const char * root, const app_manifest_t * manifest) {
    int n = snprintf(out, size, "%s/%s", root, manifest->id);
    return n > 0 && (size_t) n < size;
}

bool app_exec_path(char * out, size_t size, const char * root, const app_manifest_t * manifest) {
    int n = snprintf(out, size, "%s/%s/%s", root, manifest->id, manifest->exec);
    return n > 0 && (size_t) n < size;
}

bool app_exec_sha256(const char * root, const app_manifest_t * manifest, char hex[65]) {
    char path[512];
    uint64_t size;
    return app_exec_path(path, sizeof(path), root, manifest) && store_hash_file(path, &size, hex);
}
