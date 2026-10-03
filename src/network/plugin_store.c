#include "plugin_store.h"
#include "i18n.h"
#include "http_client.h"
#include "storage_paths.h"
#include "gui_library.h" /* sd_card_root_is_mounted() */
#include "plugin_manager.h"
#include "image_thumb.h"
#include "board_config.h"
#include "cJSON.h"
#include "mbedtls/sha256.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

/* The Plugin Store downloads its catalog and release assets from the
 * compas-plugins GitHub repository. Assets are verified by size and SHA-256
 * before they are installed. The installed record lives at
 * SD/.plugins/.store_installed and stores plugin versions plus each file's
 * destination, hash, and keep flag. Replacing a local file requires caller
 * confirmation. Network and card operations run on detached workers. */

#ifndef PLUGIN_STORE_INDEX_URL
#define PLUGIN_STORE_INDEX_URL "https://github.com/Starnished66/compas-plugins/releases/latest/download/index.json"
#endif
#ifdef HOST_BUILD
#define STORE_SD_ROOT "./music"
#define STORE_COMPAS_ROOT STORE_SD_ROOT "/.compas"
#else
#define STORE_SD_ROOT "/data/mnt/sd_0"
#define STORE_COMPAS_ROOT SD_COMPAS_ROOT
#endif
#define STORE_INDEX_LIMIT (256u << 10)
#define STORE_FILE_LIMIT (1u << 20)
#define STORE_MARGIN (2u << 20)
#define STORE_WORK STORE_COMPAS_ROOT "/store"
#define STORE_TIMEOUT_CONNECT 10000
#define STORE_TIMEOUT_READ 20000
#define STORE_DEST_LIMIT 255
/* Installed-record entries. Larger than the catalog cap so retired plugins
 * still installed do not make the record unreadable after catalog turnover;
 * new installs stop at this limit instead. */
#define STORE_MAX_RECORD 400
#define STORE_PREVIEW_LIMIT 65536U
#define STORE_PREVIEW_COUNT PLUGIN_STORE_PREVIEW_CAPACITY
#ifndef STORE_PREVIEW_ROOT
#define STORE_PREVIEW_ROOT "/tmp/compas-store-previews"
#endif

static pthread_mutex_t store_mutex = PTHREAD_MUTEX_INITIALIZER;
static plugin_store_status_t store_status;
static bool store_worker_running;
static plugin_store_result_t * store_results;
static plugin_store_plugin_t * last_index;
static size_t last_index_count;
static char last_tag[64];
static uint64_t preview_epoch = 1, preview_generation, preview_use_counter;
static bool preview_worker_running;
static uint64_t preview_spawn_epoch, preview_spawn_retry_ms;
static unsigned preview_spawn_attempts;
typedef struct {
    char sha[65];
    uint64_t attempted_epoch, retry_after_ms, last_used;
    unsigned attempts;
    bool ready;
} preview_cache_entry_t;
static preview_cache_entry_t preview_cache[STORE_PREVIEW_COUNT];

static bool read_record_alloc(plugin_store_plugin_t ** out, char (**versions)[32], size_t * count);

static void error_text(char * out, size_t size, const char * text) {
    if (out && size) {
        snprintf(out, size, "%s", text);
    }
}

static bool ascii_digit(char c) {
    return c >= '0' && c <= '9';
}

static bool ascii_lower(char c) {
    return c >= 'a' && c <= 'z';
}

static bool ascii_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool ascii_alnum(char c) {
    return ascii_alpha(c) || ascii_digit(c);
}

static bool regex_id(const char * s) {
    if (!s || !*s || strlen(s) > 63 || (!ascii_lower(*s) && !ascii_digit(*s))) {
        return false;
    }
    for (; *s; s++) {
        if (!ascii_lower(*s) && !ascii_digit(*s) && *s != '_' && *s != '.' && *s != '-') {
            return false;
        }
    }
    return true;
}

static bool regex_token(const char * s, size_t max, bool dot) {
    if (!s || !*s || strlen(s) > max) {
        return false;
    }
    for (; *s; s++) {
        if (!ascii_alnum(*s) && *s != '_' && *s != '-' && (!dot || *s != '.')) {
            return false;
        }
    }
    return true;
}

static bool version_valid(const char * s) {
    if (!s || !*s || strlen(s) >= 32) {
        return false;
    }
    int dots = 0;
    bool digit = false;
    for (; *s; s++) {
        if (ascii_digit(*s)) {
            digit = true;
        } else if (*s == '.' && digit && dots < 3) {
            dots++;
            digit = false;
        } else {
            return false;
        }
    }
    return digit;
}

static bool sha_valid(const char * s) {
    if (!s || strlen(s) != 64) {
        return false;
    }
    for (int i = 0; i < 64; i++) {
        if (!ascii_digit(s[i]) && !(s[i] >= 'a' && s[i] <= 'f')) {
            return false;
        }
    }
    return true;
}

static bool safe_text(const char * s, size_t max, bool allow_empty) {
    if (!s || strlen(s) > max || (!allow_empty && !*s)) {
        return false;
    }
    for (; *s; s++) {
        if ((unsigned char) *s < 32 || (unsigned char) *s == 127) {
            return false;
        }
    }
    return true;
}

bool plugin_store_dest_valid(const char * dest) {
    if (!dest || !*dest || dest[0] == '/' || strlen(dest) > STORE_DEST_LIMIT) {
        return false;
    }
    for (const char * p = dest; *p; p++) {
        if (!(ascii_alnum(*p) || *p == '.' || *p == '_' || *p == ' ' ||
              *p == '/' || *p == '-')) {
            return false;
        }
    }
    const char * seg = dest;
    for (const char * p = dest; ; p++) {
        if (*p == '/' || *p == '\0') {
            size_t n = (size_t)(p - seg);
            if (!n || (n == 1 && seg[0] == '.') ||
                (n == 2 && seg[0] == '.' && seg[1] == '.')) {
                return false;
            }
            if (seg == dest && n == 7 && strncasecmp(seg, ".compas", n) == 0) {
                return false;
            }
            if (!*p) {
                break;
            }
            seg = p + 1;
        }
    }
    /* Dot files in .plugins are the player's own (the store record and its
     * temporary file, the disabled list). */
    if (strncasecmp(dest, ".plugins/.", 10) == 0) {
        return false;
    }
    size_t n = strlen(dest);
    return n < 4 || strcasecmp(dest + n - 4, ".upt") != 0;
}

int plugin_store_version_compare(const char * a, const char * b) {
    while (*a || *b) {
        const char * a_end = a;
        const char * b_end = b;
        while (ascii_digit(*a_end)) {
            a_end++;
        }
        while (ascii_digit(*b_end)) {
            b_end++;
        }
        const char * a_num = a;
        const char * b_num = b;
        while (a_num < a_end && *a_num == '0') {
            a_num++;
        }
        while (b_num < b_end && *b_num == '0') {
            b_num++;
        }
        size_t a_len = (size_t) (a_end - a_num);
        size_t b_len = (size_t) (b_end - b_num);
        if (a_len != b_len) {
            return a_len < b_len ? -1 : 1;
        }
        int cmp = a_len ? memcmp(a_num, b_num, a_len) : 0;
        if (cmp) {
            return cmp < 0 ? -1 : 1;
        }
        a = *a_end == '.' ? a_end + 1 : a_end;
        b = *b_end == '.' ? b_end + 1 : b_end;
    }
    return 0;
}

static const cJSON * field(const cJSON * o, const char * name) {
    return cJSON_GetObjectItemCaseSensitive(o, name);
}

static const char * string_field(const cJSON * o, const char * name) {
    const cJSON * v = field(o, name);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static bool integer_field(const cJSON * o, const char * name, uint32_t min, uint32_t max, uint32_t * out) {
    const cJSON * v = field(o, name);
    if (!cJSON_IsNumber(v) || v->valuedouble < min || v->valuedouble > max ||
        v->valuedouble != (double) (uint32_t) v->valuedouble) {
        return false;
    }
    *out = (uint32_t) v->valuedouble;
    return true;
}

static bool same_dest(const char * a, const char * b) {
    return strcasecmp(a, b) == 0;
}

/* Two destinations that cannot both exist: the same path, or one is a
 * folder the other lives in ("Data/a" and "Data/a/b"). */
static bool dests_collide(const char * a, const char * b) {
    size_t la = strlen(a), lb = strlen(b);
    if (la == lb) {
        return strcasecmp(a, b) == 0;
    }
    const char * shorter = la < lb ? a : b;
    const char * longer = la < lb ? b : a;
    size_t n = la < lb ? la : lb;
    return strncasecmp(shorter, longer, n) == 0 && longer[n] == '/';
}

/* The plugin script: a .lua file directly in .plugins. Case-insensitive,
 * since the card is FAT: ".PLUGINS/x.lua" lands in the same folder. */
static bool script_dest(const char * dest) {
    if (strncasecmp(dest, ".plugins/", 9) != 0 || strchr(dest + 9, '/') != NULL) {
        return false;
    }
    size_t n = strlen(dest + 9);
    return n > 4 && strcasecmp(dest + 9 + n - 4, ".lua") == 0;
}
bool plugin_store_parse_index(const char * json, size_t length, plugin_store_plugin_t ** out,
                              size_t * out_count, char tag_out[64], char * error,
                              size_t error_size) {
    if (out) {
        *out = NULL;
    }
    if (out_count) {
        *out_count = 0;
    }
    if (tag_out) {
        tag_out[0] = '\0';
    }
    error_text(error, error_size, TR("Unexpected reply from GitHub."));
    if (!json || !out || !tag_out || length > STORE_INDEX_LIMIT) {
        return false;
    }

    cJSON * root = cJSON_ParseWithLength(json, length);
    plugin_store_plugin_t * parsed = NULL;
    uint32_t schema;
    const char * tag = string_field(root, "tag");
    const cJSON * plugins = field(root, "plugins");
    if (!cJSON_IsObject(root) || !integer_field(root, "schema", 1, 1, &schema) ||
        !tag || !regex_token(tag, 63, true) || !cJSON_IsArray(plugins) ||
        cJSON_GetArraySize(plugins) > PLUGIN_STORE_MAX_PLUGINS) {
        goto fail;
    }

    size_t plugin_count = (size_t) cJSON_GetArraySize(plugins);
    parsed = calloc(plugin_count ? plugin_count : 1, sizeof(*parsed));
    if (!parsed) {
        goto fail;
    }
    for (int i = 0; i < (int) plugin_count; i++) {
        const cJSON * p = cJSON_GetArrayItem(plugins, i);
        plugin_store_plugin_t * d = &parsed[i];
        memset(d, 0, sizeof(*d));

        const char * id = string_field(p, "id");
        const char * name = string_field(p, "name");
        const char * ver = string_field(p, "version");
        const char * desc = string_field(p, "description");
        const char * cat = string_field(p, "category");
        const char * author = string_field(p, "author");
        if (!cJSON_IsObject(p) || !regex_id(id) || !safe_text(name,64,false) || !version_valid(ver) ||
            !safe_text(desc,400,true) || !safe_text(cat,32,true) || !safe_text(author,64,true) ||
            !integer_field(p, "api_min", 1, UINT32_MAX, &d->api_min) ||
            !integer_field(p, "size", 0, UINT32_MAX, &d->size)) {
            goto fail;
        }
        snprintf(d->id, sizeof(d->id), "%s", id);
        snprintf(d->name, sizeof(d->name), "%s", name);
        snprintf(d->version, sizeof(d->version), "%s", ver);
        snprintf(d->description, sizeof(d->description), "%s", desc);
        snprintf(d->category, sizeof(d->category), "%s", cat);
        snprintf(d->author, sizeof(d->author), "%s", author);

        const cJSON * preview = field(p, "preview");
        if (preview) {
            const char * asset = string_field(preview, "asset");
            const char * sha = string_field(preview, "sha256");
            if (!cJSON_IsObject(preview) || !regex_token(asset, 127, true) || !sha_valid(sha) ||
                !integer_field(preview, "size", 1, STORE_PREVIEW_LIMIT, &d->preview.size) ||
                !integer_field(preview, "width", 1, 240, &d->preview.width) ||
                !integer_field(preview, "height", 1, 400, &d->preview.height)) goto fail;
            snprintf(d->preview.asset, sizeof(d->preview.asset), "%s", asset);
            snprintf(d->preview.sha256, sizeof(d->preview.sha256), "%s", sha);
        }

        const cJSON * files = field(p, "files");
        int file_count = cJSON_GetArraySize(files);
        if (!cJSON_IsArray(files) || file_count < 1 || file_count > PLUGIN_STORE_MAX_FILES) {
            goto fail;
        }
        d->file_count = file_count;
        bool has_script = false;
        uint64_t total = 0;
        for (int j = 0; j < file_count; j++) {
            const cJSON * f = cJSON_GetArrayItem(files, j);
            plugin_store_file_t * df = &d->files[j];
            const char * asset = string_field(f, "asset");
            const char * dest = string_field(f, "dest");
            const char * sha = string_field(f, "sha256");
            uint32_t size;
            const cJSON * keep = field(f, "keep");
            if (!cJSON_IsObject(f) || !regex_token(asset, 127, true) ||
                !plugin_store_dest_valid(dest) || !sha_valid(sha) ||
                !integer_field(f, "size", 1, STORE_FILE_LIMIT, &size) ||
                (keep && !cJSON_IsBool(keep))) {
                goto fail;
            }
            snprintf(df->asset, sizeof(df->asset), "%s", asset);
            snprintf(df->dest, sizeof(df->dest), "%s", dest);
            snprintf(df->sha256, sizeof(df->sha256), "%s", sha);
            df->size = size;
            df->keep = cJSON_IsTrue(keep);
            total += size;

            if (script_dest(dest)) {
                if (has_script) {
                    goto fail;
                }
                has_script = true;
            }
            for (int k = 0; k < j; k++) {
                if (dests_collide(df->dest, d->files[k].dest)) {
                    goto fail;
                }
            }
            for (int a = 0; a < i; a++) {
                for (int b = 0; b < parsed[a].file_count; b++) {
                    if (dests_collide(df->dest, parsed[a].files[b].dest)) {
                        goto fail;
                    }
                }
            }
        }
        if (!has_script || total != d->size) {
            goto fail;
        }
        d->file_count = file_count;
        for (int x = 0; x < i; x++) {
            if (strcmp(parsed[x].id, d->id) == 0) {
                goto fail;
            }
        }
    }
    if (out_count) {
        *out_count = plugin_count;
    }
    snprintf(tag_out, 64, "%s", tag);
    *out = parsed;
    cJSON_Delete(root);
    error_text(error, error_size, "");
    return true;
fail:
    free(parsed);
    cJSON_Delete(root);
    return false;
}

static bool mkdir_one(const char * path) {
    if (mkdir(path, 0755) == 0 || errno == EEXIST) {
        struct stat st;
        return lstat(path, &st) == 0 && S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode);
    }
    return false;
}

/* Creates path's directories below root, refusing any component that is
 * not a real directory (a symlink could lead off the card). root itself is
 * trusted: on the device it is reached through the /data symlink. */
static bool mkdir_tree(const char * root, const char * path) {
    char copy[PATH_MAX];
    size_t root_len = strlen(root);
    if (strlen(path) >= sizeof(copy) || strncmp(path, root, root_len) != 0 ||
        path[root_len] != '/') {
        return false;
    }
    strcpy(copy, path);
    for (char * p = copy + root_len + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (!mkdir_one(copy)) {
                return false;
            }
            *p = '/';
        }
    }
    return mkdir_one(copy);
}

/* Which card is mounted: its device number, plus the card's own CID where
 * the kernel exposes it. A different card can mount on the same block
 * device, so the device number alone cannot detect a swap. */
typedef struct {
    dev_t dev;
    char cid[48];
} store_card_t;

static void read_card_cid(char * out, size_t size) {
    out[0] = '\0';
#ifndef HOST_BUILD
    FILE * f = fopen("/sys/class/block/mmcblk0/device/cid", "r");
    if (!f) {
        return;
    }
    if (fgets(out, (int) size, f)) {
        out[strcspn(out, "\r\n")] = '\0';
    } else {
        out[0] = '\0';
    }
    fclose(f);
#else
    (void) size;
#endif
}

static bool card_mounted(store_card_t * card) {
    struct stat st;
#ifdef HOST_BUILD
    if (stat(STORE_SD_ROOT, &st) != 0 || !S_ISDIR(st.st_mode)) {
        return false;
    }
#else
    if (!sd_card_root_is_mounted() || stat(STORE_SD_ROOT, &st) != 0 ||
        !S_ISDIR(st.st_mode)) {
        return false;
    }
#endif
    if (card) {
        card->dev = st.st_dev;
        read_card_cid(card->cid, sizeof(card->cid));
    }
    return true;
}

/* An unreadable CID on either side is uncertain, not a swap; the device
 * number still has to match. */
static bool same_card(const store_card_t * a, const store_card_t * b) {
    if (a->dev != b->dev) {
        return false;
    }
    return !a->cid[0] || !b->cid[0] || strcmp(a->cid, b->cid) == 0;
}

static bool card_still(const store_card_t * card) {
    store_card_t now;
    return card_mounted(&now) && same_card(&now, card);
}

/* The card a running worker started on; plugin_store_commit_local checks it
 * before every change it makes. NULL (the host selftest) skips the check.
 * Only one worker runs at a time. */
static const store_card_t * commit_card;

static bool path_for(char * out, size_t cap, const char * root, const char * dest) {
    if (!plugin_store_dest_valid(dest)) {
        return false;
    }
    int n = snprintf(out, cap, "%s/%s", root, dest);
    return n > 0 && (size_t) n < cap;
}

static bool parent_dirs_safe(const char * root, const char * dest, bool create) {
    char path[PATH_MAX];
    if (!path_for(path, sizeof(path), root, dest)) {
        return false;
    }
    char * p = strrchr(path, '/');
    if (!p) {
        return false;
    }
    *p = '\0';
    size_t rootlen = strlen(root);
    char * at = path + rootlen;
    if (*at == '/') {
        at++;
    }
    for (char * q = at; ; q++) {
        if (*q != '/' && *q != '\0') {
            continue;
        }
        char save = *q;
        *q = '\0';
        struct stat st;
        if (lstat(path, &st) == 0) {
            if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
                return false;
            }
        } else if (errno == ENOENT && create) {
            if (mkdir(path, 0755) != 0 && errno != EEXIST) {
                return false;
            }
            if (lstat(path, &st) != 0 || !S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
                return false;
            }
        } else if (errno != ENOENT || !create) {
            return false;
        }
        *q = save;
        if (!save) {
            break;
        }
    }
    return true;
}

static bool hash_file(const char * path, uint64_t * size, char hex[65]) {
    FILE * f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    bool ok = mbedtls_sha256_starts(&c, 0) == 0;
    uint8_t buf[8192], dig[32];
    uint64_t total = 0;
    size_t n;
    while (ok && (n = fread(buf, 1, sizeof(buf), f)) > 0) {
        ok = mbedtls_sha256_update(&c, buf, n) == 0;
        total += n;
    }
    if (ferror(f)) {
        ok = false;
    }
    fclose(f);
    if (ok) {
        ok = mbedtls_sha256_finish(&c, dig) == 0;
    }
    mbedtls_sha256_free(&c);
    if (!ok) {
        return false;
    }
    for (int i = 0; i < 32; i++) {
        snprintf(hex + i * 2, 3, "%02x", dig[i]);
    }
    *size = total;
    return true;
}

/* A file belongs to plugin owner only when the record lists it under that
 * plugin with the same hash. Another plugin's file is a conflict: replacing
 * it needs confirmation, and the record then moves it to owner. */
static bool tracked_same(const char * dest, const char * sha, const char * owner,
                         const plugin_store_plugin_t * rec,
                         const char (*vers)[32], size_t n) {
    (void) vers;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(rec[i].id, owner) != 0) {
            continue;
        }
        for (int j = 0; j < rec[i].file_count; j++) {
            if (same_dest(dest, rec[i].files[j].dest) &&
                strcmp(sha, rec[i].files[j].sha256) == 0) {
                return true;
            }
        }
    }
    return false;
}

static bool record_write_path(const char * path, const plugin_store_plugin_t * plugins,
                              const int * indices, size_t count) {
    /* A temporary name of this write's own: renaming a fixed name could put
     * another card's leftover temporary record in place. */
    static unsigned tmp_counter;
    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp.%lu.%u", path, (unsigned long) time(NULL), ++tmp_counter) >=
        (int) sizeof(tmp)) {
        return false;
    }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
    if (fd < 0) {
        return false;
    }
    FILE * f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        unlink(tmp);
        return false;
    }
    bool ok = true;
    for (size_t i = 0; i < count && ok; i++) {
        const plugin_store_plugin_t * p = &plugins[indices[i]];
        ok = fprintf(f, "P %s %s\n", p->id, p->version) > 0;
        for (int j = 0; j < p->file_count && ok; j++) {
            ok = fprintf(f, "F\t%s\t%s\t%d\n", p->files[j].dest, p->files[j].sha256,
                         p->files[j].keep ? 1 : 0) > 0;
        }
    }
    ok = fflush(f) == 0 && ok;
    ok = fsync(fileno(f)) == 0 && ok;
    ok = fclose(f) == 0 && ok;
    if (commit_card && !card_still(commit_card)) {
        return false; /* the temporary file stays on the card it was written to */
    }
    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        return false;
    }
    return true;
}

static bool record_paths(const char * root, char * plugins_dir, size_t pn,
                         char * record, size_t rn) {
    return snprintf(plugins_dir, pn, "%s/.plugins", root) < (int) pn &&
           snprintf(record, rn, "%s/.plugins/.store_installed", root) < (int) rn;
}

bool plugin_store_record_write(const char * root, const plugin_store_plugin_t * plugins,
                               const int * indices, size_t count) {
    char dir[PATH_MAX], path[PATH_MAX];
    if (!record_paths(root, dir, sizeof(dir), path, sizeof(path)) || !mkdir_tree(root, dir)) {
        return false;
    }
    return record_write_path(path, plugins, indices, count);
}

size_t plugin_store_record_read(const char * root, plugin_store_plugin_t * plugins,
                                char (*versions)[32], size_t cap) {
    char dir[PATH_MAX], path[PATH_MAX];
    if (!record_paths(root, dir, sizeof(dir), path, sizeof(path))) {
        return 0;
    }
    struct stat st;
    if (lstat(dir, &st) != 0 || !S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
        return 0;
    }
    if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
        return 0;
    }
    FILE * f = fopen(path, "r");
    if (!f) {
        return 0;
    }
    size_t count = 0;
    char line[512];
    bool valid = true;
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        if (n && line[n - 1] == '\n') {
            line[--n] = 0;
        } else if (!feof(f)) {
            valid = false;
            break;
        }
        if (!strncmp(line, "P ", 2)) {
            char id[64], ver[32], extra;
            if ((count && plugins[count - 1].file_count == 0) ||
                sscanf(line, "P %63s %31s %c", id, ver, &extra) != 2 ||
                !regex_id(id) || !version_valid(ver) || count >= cap) {
                valid = false;
                break;
            }
            for (size_t i = 0; i < count; i++) {
                if (!strcmp(plugins[i].id, id)) {
                    valid = false;
                    break;
                }
            }
            if (!valid) {
                break;
            }
            memset(&plugins[count], 0, sizeof(plugins[count]));
            snprintf(plugins[count].id, 64, "%s", id);
            snprintf(plugins[count].version, 32, "%s", ver);
            snprintf(versions[count], 32, "%s", ver);
            count++;
        } else if (!strncmp(line, "F\t", 2)) {
            char * dest = line + 2;
            char * tab = strchr(dest, '\t');
            if (!count || !tab) {
                valid = false;
                break;
            }
            *tab++ = 0;
            char * tab2 = strchr(tab, '\t');
            if (!tab2) {
                valid = false;
                break;
            }
            *tab2++ = 0;
            char * end = NULL;
            errno = 0;
            long keep_value = strtol(tab2, &end, 10);
            int keep = (int) keep_value;
            if (!plugin_store_dest_valid(dest) || !sha_valid(tab) || errno || end == tab2 ||
                *end != '\0' || (keep != 0 && keep != 1) ||
                plugins[count - 1].file_count >= PLUGIN_STORE_MAX_FILES) {
                valid = false;
                break;
            }
            for (int i = 0; i < plugins[count - 1].file_count; i++) {
                if (same_dest(dest, plugins[count - 1].files[i].dest)) {
                    valid = false;
                    break;
                }
            }
            for (size_t r = 0; r + 1 < count; r++) {
                for (int j = 0; j < plugins[r].file_count; j++) {
                    if (same_dest(dest, plugins[r].files[j].dest)) {
                        valid = false;
                    }
                }
            }
            if (!valid) {
                break;
            }
            plugin_store_file_t * fentry =
                &plugins[count - 1].files[plugins[count - 1].file_count++];
            memcpy(fentry->dest, dest, strlen(dest) + 1);
            memcpy(fentry->sha256, tab, 65);
            fentry->keep = keep != 0;
        } else {
            valid = false;
            break;
        }
    }
    if (ferror(f) || (count && plugins[count - 1].file_count == 0)) {
        valid = false;
    }
    fclose(f);
    if (!valid) {
        memset(plugins, 0, cap * sizeof(*plugins));
        memset(versions, 0, cap * 32);
        return 0;
    }
    return count;
}

static bool write_record_after(const char * root, const plugin_store_plugin_t * newp,
                               const plugin_store_plugin_t * oldp,
                               const char (*oldv)[32], size_t oldn) {
    size_t cap = oldn + 1;
    plugin_store_plugin_t * all = calloc(cap, sizeof(*all));
    char (*vers)[32] = calloc(cap, 32);
    int * idx = calloc(cap, sizeof(int));
    if (!all || !vers || !idx) {
        free(all);
        free(vers);
        free(idx);
        return false;
    }
    size_t n = 0;
    for (size_t i = 0; i < oldn; i++) {
        if (strcmp(oldp[i].id, newp->id) == 0) {
            continue;
        }
        /* A destination the new plugin took over leaves the old owner's
         * entry, so no destination is recorded twice. */
        all[n] = oldp[i];
        all[n].file_count = 0;
        for (int j = 0; j < oldp[i].file_count; j++) {
            bool taken = false;
            for (int k = 0; k < newp->file_count; k++) {
                if (same_dest(oldp[i].files[j].dest, newp->files[k].dest)) {
                    taken = true;
                    break;
                }
            }
            if (!taken) {
                all[n].files[all[n].file_count++] = oldp[i].files[j];
            }
        }
        if (all[n].file_count == 0) {
            continue; /* nothing of it is left on the card */
        }
        snprintf(vers[n], 32, "%s", oldv[i]);
        idx[n] = (int) n;
        n++;
    }
    all[n] = *newp;
    snprintf(vers[n], 32, "%s", newp->version);
    idx[n] = (int) n;
    n++;
    char dir[PATH_MAX], path[PATH_MAX];
    bool ok = record_paths(root, dir, sizeof(dir), path, sizeof(path)) && mkdir_tree(root, dir) &&
              record_write_path(path, all, idx, n);
    free(all);
    free(vers);
    free(idx);
    return ok;
}

plugin_store_plugin_state_t plugin_store_compute_state(const plugin_store_plugin_t * p,
                                                       const char * installed,
                                                       const char * plugins_dir,
                                                       bool * incompatible) {
    bool inc = p->api_min > PLUGIN_API_VERSION;
    if (incompatible) {
        *incompatible = inc;
    }
    if (installed && *installed) {
        if (plugin_store_version_compare(p->version, installed) <= 0) {
            return PLUGIN_STORE_PLUGIN_INSTALLED;
        }
        return inc ? PLUGIN_STORE_PLUGIN_INSTALLED : PLUGIN_STORE_PLUGIN_UPDATE;
    }
    for (int i = 0; i < p->file_count; i++) {
        if (script_dest(p->files[i].dest)) {
            const char * name = p->files[i].dest + 9;
            size_t dirlen = strlen(plugins_dir);
            size_t namelen = strlen(name);
            char * path = malloc(dirlen + namelen + 2);
            if (path) {
                memcpy(path, plugins_dir, dirlen);
                path[dirlen] = '/';
                memcpy(path + dirlen + 1, name, namelen + 1);
                struct stat st;
                bool exists = lstat(path, &st) == 0 && S_ISREG(st.st_mode);
                free(path);
                if (exists) {
                    return PLUGIN_STORE_PLUGIN_MANUAL;
                }
            }
            break;
        }
    }
    return inc ? PLUGIN_STORE_PLUGIN_INCOMPATIBLE : PLUGIN_STORE_PLUGIN_AVAILABLE;
}

bool plugin_store_commit_local(const char * root, const char * stage,
                               const plugin_store_plugin_t * p, bool force,
                               const plugin_store_plugin_t * rec,
                               const char (*vers)[32], size_t rn) {
    char plugins_path[PATH_MAX], record[PATH_MAX];
    if (!record_paths(root, plugins_path, sizeof(plugins_path), record, sizeof(record))) {
        return false;
    }
    size_t n = (size_t) p->file_count;
    char (*dests)[PATH_MAX] = calloc(n, sizeof(*dests));
    char (*backs)[PATH_MAX] = calloc(n, sizeof(*backs));
    bool * existed = calloc(n, sizeof(bool));
    bool * skip = calloc(n, sizeof(bool));
    bool * installed = calloc(n, sizeof(bool));
    if (!dests || !backs || !existed || !skip || !installed) {
        goto no_mem;
    }
    for (size_t i = 0; i < n; i++) {
        const plugin_store_file_t * f = &p->files[i];
        if (!path_for(dests[i], PATH_MAX, root, f->dest)) {
            goto fail;
        }
        if (f->keep) {
            struct stat st;
            if (lstat(dests[i], &st) == 0) {
                skip[i] = true;
                continue;
            }
        }
        struct stat st;
        if (lstat(dests[i], &st) == 0) {
            existed[i] = true;
            char sha[65];
            uint64_t sz;
            if (!S_ISREG(st.st_mode) || !hash_file(dests[i], &sz, sha)) {
                goto fail;
            }
            if (!tracked_same(f->dest, sha, p->id, rec, vers, rn) && !force) {
                goto conflict;
            }
        } else if (errno != ENOENT) {
            goto fail;
        }
        char part[PATH_MAX], sha[65];
        uint64_t size;
        snprintf(part, sizeof(part), "%s/%zu.part", stage, i);
        if (!hash_file(part, &size, sha) || size != f->size || strcmp(sha, f->sha256) != 0) {
            goto fail;
        }
    }
    for (size_t i = 0; i < n; i++) {
        if (skip[i]) {
            continue;
        }
        const plugin_store_file_t * f = &p->files[i];
        if (commit_card && !card_still(commit_card)) {
            goto card_lost;
        }
        if (!parent_dirs_safe(root, f->dest, true)) {
            goto rollback;
        }
        char part[PATH_MAX];
        snprintf(part, sizeof(part), "%s/%zu.part", stage, i);
        if (existed[i]) {
            snprintf(backs[i], PATH_MAX, "%s/%zu.backup", stage, i);
            if (rename(dests[i], backs[i]) != 0) {
                goto rollback;
            }
        }
        if (rename(part, dests[i]) != 0) {
            goto rollback;
        }
        installed[i] = true;
    }
    if (commit_card && !card_still(commit_card)) {
        goto card_lost;
    }
    if (!write_record_after(root, p, rec, vers, rn)) {
        goto rollback;
    }
    /* Remove dropped files only when they still match the old installed hash. */
    for (size_t r = 0; r < rn; r++) {
        if (strcmp(rec[r].id, p->id) != 0) {
            continue;
        }
        for (int old = 0; old < rec[r].file_count; old++) {
            const plugin_store_file_t * old_file = &rec[r].files[old];
            bool retained = false;
            if (old_file->keep) {
                continue;
            }
            for (int current = 0; current < p->file_count; current++) {
                if (same_dest(old_file->dest, p->files[current].dest)) {
                    retained = true;
                    break;
                }
            }
            if (retained) {
                continue;
            }
            char old_path[PATH_MAX];
            struct stat old_stat;
            char old_sha[65];
            uint64_t old_size;
            if (path_for(old_path, sizeof(old_path), root, old_file->dest) &&
                parent_dirs_safe(root, old_file->dest, false) &&
                (!commit_card || card_still(commit_card)) &&
                lstat(old_path, &old_stat) == 0 && S_ISREG(old_stat.st_mode) &&
                hash_file(old_path, &old_size, old_sha) &&
                strcmp(old_sha, old_file->sha256) == 0) {
                unlink(old_path);
            }
        }
    }
    if (!commit_card || card_still(commit_card)) {
        for (size_t i = 0; i < n; i++) {
            if (backs[i][0]) {
                unlink(backs[i]);
            }
        }
    }
    free(dests);
    free(backs);
    free(existed);
    free(skip);
    free(installed);
    sync();
    return true;

rollback:
    if (commit_card && !card_still(commit_card)) {
        goto card_lost;
    }
    for (size_t i = n; i > 0; i--) {
        size_t k = i - 1;
        if (backs[k][0]) {
            if (installed[k]) {
                unlink(dests[k]);
            }
            rename(backs[k], dests[k]);
        } else if (installed[k]) {
            unlink(dests[k]);
        }
    }
    goto fail;

card_lost:
    /* Another card is mounted now: rolling back would delete and rename
     * files on it that belong to it. Leave both cards as they are. */
conflict:
    free(dests);
    free(backs);
    free(existed);
    free(skip);
    free(installed);
    return false;

fail:
    for (size_t i = 0; i < n && (!commit_card || card_still(commit_card)); i++) {
        char part[PATH_MAX];
        snprintf(part, sizeof(part), "%s/%zu.part", stage, i);
        unlink(part);
    }
    free(dests);
    free(backs);
    free(existed);
    free(skip);
    free(installed);
    return false;

no_mem:
    free(dests);
    free(backs);
    free(existed);
    free(skip);
    free(installed);
    return false;
}

static void finish(plugin_store_state_t state, const char * error, bool changed) {
    pthread_mutex_lock(&store_mutex);
    store_status.state = state;
    store_status.changed = changed;
    if (error) {
        snprintf(store_status.error, sizeof(store_status.error), "%s", error);
    }
    store_worker_running = false;
    pthread_mutex_unlock(&store_mutex);
}

static const char * http_error(const char * e) {
    if (e && strcmp(e, HTTP_ERR_TLS) == 0) {
        return TR("Secure connection failed. Check Wi-Fi and the date and time.");
    }
    if (e && (strcmp(e, HTTP_ERR_DNS) == 0 || strcmp(e, HTTP_ERR_CONNECT) == 0 ||
              strcmp(e, HTTP_ERR_CONNECT_TIMEOUT) == 0)) {
        return TR("Cannot reach GitHub. Check the Wi-Fi connection.");
    }
    if (e && strcmp(e, HTTP_ERR_TIMEOUT) == 0) {
        return TR("GitHub did not respond in time. Try again.");
    }
    return TR("Could not read the plugin list from GitHub.");
}
/* Build the compact UI rows from the current index and installed record. */
static bool plugin_contains_player_layout(const plugin_store_plugin_t * plugin) {
    for (int f = 0; f < plugin->file_count; f++) {
        const char * dest = plugin->files[f].dest;
        /* Layout XML can be installed directly in player_layouts/ or
         * inside a plugin bundle below .plugins/<bundle>/player_layouts/. */
        size_t len = strlen(dest);
        const char * layout_file = NULL;
        if (strncmp(dest, ".plugins/", 9) == 0 && !strchr(dest + 9, '/')) {
            layout_file = dest + 9;
        } else if (strncmp(dest, ".plugins/player_layouts/", 24) == 0) {
            layout_file = dest + 24;
        } else if (strncmp(dest, ".plugins/", 9) == 0) {
            const char * bundle_end = strchr(dest + 9, '/');
            if (bundle_end && strncmp(bundle_end, "/player_layouts/", 16) == 0)
                layout_file = bundle_end + 16;
        }
        if (layout_file && !strchr(layout_file, '/') && strlen(layout_file) > 4 &&
            strcasecmp(dest + len - 4, ".xml") == 0) {
            return true;
        }
    }
    return false;
}

static void compute_results(void) {
    size_t index_count;
    pthread_mutex_lock(&store_mutex);
    index_count = last_index_count;
    pthread_mutex_unlock(&store_mutex);

    plugin_store_plugin_t * rec = NULL;
    char (*vers)[32] = NULL;
    size_t rn = 0;
    if (!read_record_alloc(&rec, &vers, &rn)) {
        return;
    }

    size_t capacity = index_count + rn;
    if (capacity > PLUGIN_STORE_MAX_RESULTS) {
        capacity = PLUGIN_STORE_MAX_RESULTS;
    }
    plugin_store_result_t * result = calloc(capacity ? capacity : 1, sizeof(*result));
    if (!result) {
        free(rec);
        free(vers);
        return;
    }

    size_t count = 0;
    for (size_t i = 0; i < index_count && count < PLUGIN_STORE_MAX_RESULTS; i++) {
        plugin_store_plugin_t plugin;
        pthread_mutex_lock(&store_mutex);
        bool available = last_index && i < last_index_count;
        if (available) {
            plugin = last_index[i];
        }
        pthread_mutex_unlock(&store_mutex);
        if (!available) {
            break;
        }

        int match = -1;
        for (size_t r = 0; r < rn; r++) {
            if (strcmp(plugin.id, rec[r].id) == 0) {
                match = (int) r;
                break;
            }
        }
        plugin_store_result_t * row = &result[count++];
        snprintf(row->id, sizeof(row->id), "%s", plugin.id);
        snprintf(row->name, sizeof(row->name), "%s", plugin.name);
        snprintf(row->version, sizeof(row->version), "%s", plugin.version);
        row->player_layout = plugin_contains_player_layout(&plugin);
        row->state = plugin_store_compute_state(&plugin, match >= 0 ? vers[match] : NULL,
                                                STORE_SD_ROOT "/.plugins", &row->incompatible);
    }

    for (size_t r = 0; r < rn && count < PLUGIN_STORE_MAX_RESULTS; r++) {
        bool found = false;
        pthread_mutex_lock(&store_mutex);
        for (size_t i = 0; i < last_index_count; i++) {
            if (strcmp(rec[r].id, last_index[i].id) == 0) {
                found = true;
                break;
            }
        }
        pthread_mutex_unlock(&store_mutex);
        if (!found) {
            plugin_store_result_t * row = &result[count++];
            snprintf(row->id, sizeof(row->id), "%s", rec[r].id);
            snprintf(row->version, sizeof(row->version), "%s", vers[r]);
            row->state = PLUGIN_STORE_PLUGIN_REMOVED;
            row->player_layout = plugin_contains_player_layout(&rec[r]);
        }
    }
    pthread_mutex_lock(&store_mutex);
    free(store_results);
    store_results = result;
    store_status.result_count = count;
    pthread_mutex_unlock(&store_mutex);
    free(rec);
    free(vers);
}

/* Count record headers first so large plugin structures are allocated on demand. */
static bool read_record_alloc(plugin_store_plugin_t ** out, char (**versions)[32], size_t * count) {
    char plugins_dir[PATH_MAX];
    char record_path[PATH_MAX];
    if (!record_paths(STORE_SD_ROOT, plugins_dir, sizeof(plugins_dir), record_path,
                      sizeof(record_path))) {
        return false;
    }

    FILE * file = fopen(record_path, "r");
    size_t capacity = 0;
    if (file) {
        char line[512];
        while (fgets(line, sizeof(line), file)) {
            if (strncmp(line, "P ", 2) == 0) {
                capacity++;
            }
        }
        bool read_error = ferror(file) != 0;
        fclose(file);
        if (read_error || capacity > STORE_MAX_RECORD) {
            return false;
        }
    }

    *out = calloc(capacity ? capacity : 1, sizeof(**out));
    *versions = calloc(capacity ? capacity : 1, sizeof(**versions));
    if (!*out || !*versions) {
        free(*out);
        free(*versions);
        *out = NULL;
        *versions = NULL;
        return false;
    }
    *count = plugin_store_record_read(STORE_SD_ROOT, *out, *versions, capacity);
    return true;
}

void plugin_store_classify_layout_loaders(const char * const * filenames, bool * flags, size_t count) {
    if (!filenames || !flags || !count) return;
    plugin_store_plugin_t * records = NULL;
    char (*versions)[32] = NULL;
    size_t record_count = 0;
    if (!read_record_alloc(&records, &versions, &record_count)) return;

    for (size_t r = 0; r < record_count; r++) {
        const plugin_store_plugin_t * record = &records[r];
        if (!plugin_contains_player_layout(record)) continue;
        for (int f = 0; f < record->file_count; f++) {
            const char * dest = record->files[f].dest;
            if (strncmp(dest, ".plugins/", 9) != 0 || strchr(dest + 9, '/')) continue;
            size_t len = strlen(dest + 9);
            if (len <= 4 || strcasecmp(dest + 9 + len - 4, ".lua") != 0) continue;
            for (size_t i = 0; i < count; i++) {
                if (filenames[i] && strcmp(filenames[i], dest + 9) == 0) flags[i] = true;
            }
        }
    }
    free(records);
    free(versions);
}

static bool conflict_exists(const char * root, const plugin_store_plugin_t * p,
                            const plugin_store_plugin_t * rec, const char (*vers)[32],
                            size_t rn) {
    for (int i = 0; i < p->file_count; i++) {
        const plugin_store_file_t * f = &p->files[i];
        if (f->keep) {
            continue;
        }
        char path[PATH_MAX];
        struct stat st;
        if (!path_for(path, sizeof(path), root, f->dest)) {
            return true;
        }
        if (lstat(path, &st) == 0) {
            char sha[65];
            uint64_t size;
            if (!S_ISREG(st.st_mode) || !hash_file(path, &size, sha) ||
                !tracked_same(f->dest, sha, p->id, rec, vers, rn)) {
                return true;
            }
        } else if (errno != ENOENT) {
            return true;
        }
    }
    return false;
}

static bool make_asset_url(char * out, size_t cap, const char * tag, const char * asset) {
    int n = snprintf(out, cap,
                     "https://github.com/Starnished66/compas-plugins/releases/download/%s/%s",
                     tag, asset);
    return n > 0 && (size_t) n < cap;
}

static bool download_progress(uint64_t downloaded, uint64_t total, void * unused) {
    (void) unused;
    int percent = total ? (int) (downloaded * 100 / total) : 0;
    if (percent > 100) {
        percent = 100;
    }
    pthread_mutex_lock(&store_mutex);
    store_status.percent = percent;
    pthread_mutex_unlock(&store_mutex);
    return true;
}

static bool download_plugin_files(const plugin_store_plugin_t * p, const char * tag,
                                 const char * stage, uint64_t * bytes) {
    *bytes = 0;
    if (!mkdir_tree(STORE_SD_ROOT, stage)) {
        return false;
    }
    for (int i = 0; i < p->file_count; i++) {
        const plugin_store_file_t * f = &p->files[i];
        char dest[PATH_MAX], url[512];
        snprintf(dest, sizeof(dest), "%s/%d.part", stage, i);
        if (f->keep) {
            char final[PATH_MAX];
            snprintf(final, sizeof(final), "%s/%s", STORE_SD_ROOT, f->dest);
            struct stat st;
            if (lstat(final, &st) == 0) {
                continue;
            }
        }
        if (!make_asset_url(url, sizeof(url), tag, f->asset)) {
            return false;
        }
        int status = 0;
        if (!http_get_to_file_redirects(url, true, dest, STORE_FILE_LIMIT, download_progress,
                                        NULL, STORE_TIMEOUT_CONNECT, STORE_TIMEOUT_READ, NULL, 5,
                                        &status)) {
            return false;
        }
        char sha[65];
        uint64_t size;
        if (!hash_file(dest, &size, sha) || size != f->size || strcmp(sha, f->sha256) != 0) {
            return false;
        }
        *bytes += size;
    }
    return true;
}

static void progress_set(int percent, const char * name) {
    pthread_mutex_lock(&store_mutex);
    store_status.percent = percent;
    if (name) {
        snprintf(store_status.current_plugin, sizeof(store_status.current_plugin), "%.64s", name);
    }
    pthread_mutex_unlock(&store_mutex);
}

static bool begin(plugin_store_state_t state) {
    pthread_mutex_lock(&store_mutex);
    if (store_worker_running) {
        pthread_mutex_unlock(&store_mutex);
        return false;
    }
    store_worker_running = true;
    store_status.state = state;
    store_status.percent = 0;
    store_status.changed = false;
    store_status.error[0] = 0;
    store_status.current_plugin[0] = 0;
    pthread_mutex_unlock(&store_mutex);
    return true;
}

static bool start_thread(void * (*fn)(void *), void * arg) {
    pthread_attr_t a;
    pthread_t t;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&a, 256 * 1024);
    bool ok = pthread_create(&t, &a, fn, arg) == 0;
    pthread_attr_destroy(&a);
    return ok;
}
/* Preview I/O never runs on LVGL's thread or changes installer progress. */
typedef struct {
    int slot;
    plugin_store_preview_t preview;
    char evicted_sha[65];
    preview_cache_entry_t previous;
} preview_item_t;
typedef struct {
    uint64_t epoch;
    char tag[64];
    size_t count;
    preview_item_t items[STORE_PREVIEW_COUNT];
} preview_job_t;
static int preview_dir_fd = -1, preview_lock_fd = -1;

static uint64_t preview_now_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t) now.tv_sec * 1000 + (uint64_t) now.tv_nsec / 1000000;
}

static bool preview_cache_open(void) {
    if (preview_dir_fd >= 0) return true;
    if (mkdir(STORE_PREVIEW_ROOT, 0700) != 0 && errno != EEXIST) return false;
    int dir = open(STORE_PREVIEW_ROOT, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dir < 0) return false;
    struct stat st;
    if (fstat(dir, &st) != 0 || st.st_uid != geteuid() || (st.st_mode & 0077)) {
        close(dir); return false;
    }
    int lock = openat(dir, ".owner-lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
    if (lock < 0 || fstat(lock, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_uid != geteuid() || (st.st_mode & 0077) || flock(lock, LOCK_EX | LOCK_NB) != 0) {
        if (lock >= 0) close(lock);
        close(dir); return false;
    }
    /* Keep only files assigned to our bounded cache, including previews that
     * can be reused after a player restart. Ignore unrelated filenames. */
    int scan_fd = dup(dir);
    DIR * scan = scan_fd >= 0 ? fdopendir(scan_fd) : NULL;
    if (!scan && scan_fd >= 0) close(scan_fd);
    if (scan) {
        struct dirent * ent;
        while ((ent = readdir(scan)) != NULL) {
            size_t len = strlen(ent->d_name);
            bool part = strncmp(ent->d_name, ".part-", 6) == 0;
            bool thumb_tmp = len == 72 && strcmp(ent->d_name + 64, ".bin.tmp") == 0;
            bool keep = false;
            if ((len == 68 && (strcmp(ent->d_name + 64, ".jpg") == 0 ||
                               strcmp(ent->d_name + 64, ".bin") == 0))) {
                char sha[65]; memcpy(sha, ent->d_name, 64); sha[64] = 0;
                if (!sha_valid(sha)) continue;
                pthread_mutex_lock(&store_mutex);
                for (int i = 0; i < STORE_PREVIEW_COUNT; i++)
                    if (strcmp(preview_cache[i].sha, sha) == 0) { keep = true; break; }
                pthread_mutex_unlock(&store_mutex);
            } else if (!part && !thumb_tmp) continue;
            if (!keep && fstatat(dir, ent->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0 &&
                S_ISREG(st.st_mode) && st.st_uid == geteuid()) unlinkat(dir, ent->d_name, 0);
        }
        closedir(scan);
    }
    preview_lock_fd = lock; /* Held for this process; no stale PID/lock directory. */
    preview_dir_fd = dir;
    return true;
}

static bool preview_jpeg_valid(const char * path, const plugin_store_preview_t * meta) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st;
    bool valid = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
                 st.st_size == meta->size && st.st_size >= 4 && st.st_size <= STORE_PREVIEW_LIMIT;
    unsigned char * data = valid ? malloc(meta->size) : NULL;
    size_t used = 0;
    while (data && used < meta->size) {
        ssize_t n = read(fd, data + used, meta->size - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        used += (size_t) n;
    }
    close(fd);
    valid = data && used == meta->size && data[0] == 0xff && data[1] == 0xd8 &&
            data[used - 2] == 0xff && data[used - 1] == 0xd9;
    bool frame = false, scan = false;
    for (size_t pos = 2; valid && pos < used; ) {
        if (data[pos] != 0xff) { valid = false; break; }
        while (pos < used && data[pos] == 0xff) pos++;
        if (pos >= used) { valid = false; break; }
        unsigned marker = data[pos++];
        if (marker == 0xd9) break;
        if (marker == 0xd8 || (marker >= 0xd0 && marker <= 0xd7) || marker == 1) continue;
        if (used - pos < 2) { valid = false; break; }
        size_t len = ((size_t) data[pos] << 8) | data[pos + 1];
        if (len < 2 || len > used - pos) { valid = false; break; }
        if (marker == 0xc0) {
            if (frame || len < 8) { valid = false; break; }
            unsigned h = ((unsigned) data[pos + 3] << 8) | data[pos + 4];
            unsigned w = ((unsigned) data[pos + 5] << 8) | data[pos + 6];
            unsigned components = data[pos + 7];
            valid = data[pos + 2] == 8 && (components == 1 || components == 3) &&
                    len == 8 + 3 * components && w == meta->width && h == meta->height;
            frame = valid;
        } else if (marker >= 0xc1 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc) {
            valid = false;
        } else if (marker == 0xda) { scan = frame && len >= 6; break; }
        pos += len;
    }
    free(data);
    return valid && frame && scan;
}

static bool preview_file_verified(const char * path, const plugin_store_preview_t * meta) {
    char sha[65]; uint64_t bytes;
    return preview_jpeg_valid(path, meta) && hash_file(path, &bytes, sha) &&
           bytes == meta->size && strcmp(sha, meta->sha256) == 0;
}

static bool preview_progress(uint64_t downloaded, uint64_t total, void * context) {
    (void) downloaded; (void) total;
    const preview_job_t * job = context;
    pthread_mutex_lock(&store_mutex);
    bool current = preview_epoch == job->epoch;
    pthread_mutex_unlock(&store_mutex);
    return current;
}

static bool preview_decode_cancel(void * context) {
    return !preview_progress(0, 0, context);
}

static bool preview_read_jpeg(const char * path, const plugin_store_preview_t * meta,
                              uint8_t ** data_out) {
    *data_out = NULL;
    if (!preview_file_verified(path, meta)) return false;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st;
    bool valid = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == meta->size &&
                 st.st_size > 0 && st.st_size <= STORE_PREVIEW_LIMIT;
    uint8_t * data = valid ? malloc(meta->size) : NULL;
    size_t used = 0;
    while (data && used < meta->size) {
        ssize_t n = read(fd, data + used, meta->size - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        used += (size_t) n;
    }
    close(fd);
    if (!data || used != meta->size) { free(data); return false; }
    *data_out = data;
    return true;
}

static void * preview_worker(void * context) {
    preview_job_t * job = context;
    bool opened = preview_cache_open();
    /* Drain every eviction even if navigation cancelled the transfer batch.
     * Slot ownership changed before launch; skipping later evictions would
     * lose their filenames and let abandoned cache files accumulate. */
    for (size_t i = 0; opened && i < job->count; i++) {
        if (!job->items[i].evicted_sha[0]) continue;
        char name[80];
        snprintf(name, sizeof(name), "%s.jpg", job->items[i].evicted_sha);
        unlinkat(preview_dir_fd, name, 0);
        snprintf(name, sizeof(name), "%s.bin", job->items[i].evicted_sha);
        unlinkat(preview_dir_fd, name, 0);
    }
    for (size_t i = 0; opened && i < job->count && preview_progress(0, 0, job); i++) {
        preview_item_t * item = &job->items[i];
        char name[80], path[PATH_MAX], part[PATH_MAX], url[512], bin_path[PATH_MAX];
        snprintf(name, sizeof(name), "%s.jpg", item->preview.sha256);
        snprintf(path, sizeof(path), "%s/%s", STORE_PREVIEW_ROOT, name);
        bool retryable = false;
        bool ready = preview_file_verified(path, &item->preview);
        if (!ready) {
            snprintf(part, sizeof(part), "%s/.part-%s", STORE_PREVIEW_ROOT, item->preview.sha256);
            unlink(part); /* Private directory, owned by this worker's process lock. */
            int status = 0;
            ready = make_asset_url(url, sizeof(url), job->tag, item->preview.asset) &&
                http_get_to_file_redirects(url, true, part, STORE_PREVIEW_LIMIT, preview_progress,
                    job, 5000, 8000, NULL, 5, &status) &&
                preview_progress(0, 0, job) && preview_file_verified(part, &item->preview) &&
                rename(part, path) == 0;
            if (!ready) unlink(part);
        }
        if (ready && preview_progress(0, 0, job)) {
            snprintf(name, sizeof(name), "%s.bin", item->preview.sha256);
            snprintf(bin_path, sizeof(bin_path), "%s/%s", STORE_PREVIEW_ROOT, name);
            uint8_t * jpeg = NULL;
            const char * reason = NULL;
            if (preview_read_jpeg(path, &item->preview, &jpeg)) {
                int card_w = (BOARD_SCREEN_WIDTH - 2 * BOARD_SCALE_PX(16) - BOARD_SCALE_PX(14)) / 2;
                int cover_h = card_w * 3 / 2;
                ready = image_thumb_write_bin(jpeg, item->preview.size, card_w, cover_h, bin_path,
                                              ARTWORK_PRIO_THUMBNAIL, preview_decode_cancel, job,
                                              &reason) && preview_progress(0, 0, job);
                retryable = !ready && reason && strcmp(reason, "busy") == 0;
                free(jpeg);
            } else {
                ready = false;
            }
            if (!ready) unlink(bin_path);
        } else {
            ready = false;
        }
        pthread_mutex_lock(&store_mutex);
        preview_cache_entry_t * cached = &preview_cache[item->slot];
        if (preview_epoch == job->epoch && strcmp(cached->sha, item->preview.sha256) == 0) {
            if (ready) {
                cached->ready = true;
                preview_generation++;
            } else if (retryable && cached->attempts < 3) {
                cached->retry_after_ms = preview_now_ms() + 1000 * cached->attempts;
            }
        }
        pthread_mutex_unlock(&store_mutex);
    }
    pthread_mutex_lock(&store_mutex);
    preview_worker_running = false;
    pthread_mutex_unlock(&store_mutex);
    free(job);
    return NULL;
}

bool plugin_store_prepare_previews(const char * const * ids, size_t count) {
    if (!ids || !count) return false;
    if (count > STORE_PREVIEW_COUNT) count = STORE_PREVIEW_COUNT;
    pthread_mutex_lock(&store_mutex);
    if (preview_worker_running || !last_index_count) {
        pthread_mutex_unlock(&store_mutex); return false;
    }
    uint64_t now = preview_now_ms();
    if (preview_spawn_epoch != preview_epoch) {
        preview_spawn_epoch = preview_epoch;
        preview_spawn_attempts = 0;
        preview_spawn_retry_ms = 0;
    }
    if (now < preview_spawn_retry_ms || preview_spawn_attempts >= 3) {
        pthread_mutex_unlock(&store_mutex); return false;
    }
    const plugin_store_plugin_t * requested[STORE_PREVIEW_COUNT] = {0};
    bool protected[STORE_PREVIEW_COUNT] = {0};
    for (size_t i = 0; i < count; i++) {
        if (!ids[i]) continue;
        for (size_t p = 0; p < last_index_count; p++) {
            if (strcmp(last_index[p].id, ids[i]) != 0) continue;
            if (last_index[p].preview.asset[0] && plugin_contains_player_layout(&last_index[p]))
                requested[i] = &last_index[p];
            break;
        }
        if (!requested[i]) continue;
        for (int c = 0; c < STORE_PREVIEW_COUNT; c++)
            if (strcmp(preview_cache[c].sha, requested[i]->preview.sha256) == 0)
                protected[c] = true;
    }
    preview_job_t * job = NULL;
    for (size_t i = 0; i < count; i++) {
        const plugin_store_plugin_t * p = requested[i];
        if (!p) continue;
        int slot = -1;
        for (int c = 0; c < STORE_PREVIEW_COUNT; c++)
            if (strcmp(preview_cache[c].sha, p->preview.sha256) == 0) { slot = c; break; }
        if (slot >= 0) {
            preview_cache_entry_t * cached = &preview_cache[slot];
            cached->last_used = ++preview_use_counter;
            if (cached->ready) continue;
            if (cached->attempted_epoch == preview_epoch && cached->attempts &&
                (!cached->retry_after_ms || now < cached->retry_after_ms || cached->attempts >= 3)) continue;
        } else {
            /* Visible/nearby cards are pinned for this request. Replace only
             * an offscreen slot, preferring an empty then the oldest slot. */
            for (int c = 0; c < STORE_PREVIEW_COUNT; c++) {
                if (protected[c]) continue;
                if (slot < 0 || preview_cache[c].last_used < preview_cache[slot].last_used) slot = c;
                if (!preview_cache[c].sha[0]) { slot = c; break; }
            }
        }
        if (slot < 0) continue;
        if (!job) {
            job = calloc(1, sizeof(*job));
            if (!job) break;
            job->epoch = preview_epoch;
            snprintf(job->tag, sizeof(job->tag), "%s", last_tag);
        }
        preview_item_t * item = &job->items[job->count++];
        item->slot = slot;
        item->preview = p->preview;
        preview_cache_entry_t * cached = &preview_cache[slot];
        item->previous = *cached;
        if (strcmp(cached->sha, p->preview.sha256) != 0) {
            memcpy(item->evicted_sha, cached->sha, sizeof(item->evicted_sha));
            if (cached->ready) preview_generation++;
            memset(cached, 0, sizeof(*cached));
            memcpy(cached->sha, p->preview.sha256, sizeof(cached->sha));
        }
        if (cached->attempted_epoch != job->epoch) cached->attempts = 0;
        cached->attempted_epoch = job->epoch;
        cached->attempts++;
        cached->retry_after_ms = 0;
        cached->last_used = ++preview_use_counter;
        protected[slot] = true;
    }
    preview_worker_running = job != NULL;
    pthread_mutex_unlock(&store_mutex);
    if (!job) return false;
    if (!start_thread(preview_worker, job)) {
        pthread_mutex_lock(&store_mutex);
        preview_worker_running = false;
        preview_spawn_attempts++;
        preview_spawn_retry_ms = preview_now_ms() + 1000;
        for (size_t i = 0; i < job->count; i++) {
            /* No worker ran: keep old files and their ownership so a failed
             * thread start cannot orphan evicted files in the RAM cache. */
            preview_cache[job->items[i].slot] = job->items[i].previous;
        }
        pthread_mutex_unlock(&store_mutex);
        free(job); return false;
    }
    pthread_mutex_lock(&store_mutex);
    preview_spawn_attempts = 0;
    pthread_mutex_unlock(&store_mutex);
    return true;
}

bool plugin_store_get_preview(const char * id, char * out, size_t size) {
    if (!id || !out || !size) return false;
    out[0] = 0;
    bool found = false;
    pthread_mutex_lock(&store_mutex);
    for (size_t i = 0; i < last_index_count; i++) {
        if (strcmp(last_index[i].id, id) != 0 || !last_index[i].preview.asset[0]) continue;
        for (int c = 0; c < STORE_PREVIEW_COUNT; c++) {
            if (!preview_cache[c].ready || strcmp(preview_cache[c].sha, last_index[i].preview.sha256) != 0) continue;
            int n = snprintf(out, size, "%s/%s.bin", STORE_PREVIEW_ROOT, preview_cache[c].sha);
            found = n > 0 && (size_t) n < size;
            break;
        }
        break;
    }
    pthread_mutex_unlock(&store_mutex);
    if (!found) out[0] = 0;
    return found;
}

void plugin_store_cancel_previews(void) {
    pthread_mutex_lock(&store_mutex);
    preview_epoch++;
    pthread_mutex_unlock(&store_mutex);
}

uint64_t plugin_store_preview_generation(void) {
    pthread_mutex_lock(&store_mutex);
    uint64_t generation = preview_generation;
    pthread_mutex_unlock(&store_mutex);
    return generation;
}

static void * refresh_worker(void * unused) {
    (void) unused;
    http_request_t req;
    memset(&req, 0, sizeof(req));
    snprintf(req.url, sizeof(req.url), "%s", PLUGIN_STORE_INDEX_URL);
    req.method = HTTP_METHOD_GET;
    req.verify_tls = true;
    req.connect_timeout_ms = STORE_TIMEOUT_CONNECT;
    req.read_timeout_ms = STORE_TIMEOUT_READ;
    req.total_timeout_ms = 45000;
    req.max_response_bytes = STORE_INDEX_LIMIT;
    req.redirect_limit = 5;
    http_response_t response;
    bool ok = http_request_ex(&req, NULL, &response);
    if (!ok || !response.body) {
        const char * msg = http_error(ok ? NULL : response.error);
        http_response_free(&response);
        finish(PLUGIN_STORE_FAILED, msg, false);
        return NULL;
    }
    if (response.status != 200) {
        http_response_free(&response);
        finish(PLUGIN_STORE_FAILED, TR("Could not read the plugin list from GitHub."), false);
        return NULL;
    }
    plugin_store_plugin_t * parsed = NULL;
    size_t count = 0;
    char tag[64];
    char err[128];
    bool valid = plugin_store_parse_index((char *) response.body, response.body_len, &parsed,
                                          &count, tag, err, sizeof(err));
    http_response_free(&response);
    if (!valid) {
        finish(PLUGIN_STORE_FAILED, err, false);
        return NULL;
    }
    pthread_mutex_lock(&store_mutex);
    free(last_index);
    last_index = parsed;
    last_index_count = count;
    preview_epoch++;
    snprintf(last_tag, sizeof(last_tag), "%s", tag);
    pthread_mutex_unlock(&store_mutex);
    compute_results();
    finish(PLUGIN_STORE_READY, NULL, false);
    return NULL;
}
typedef struct {
    plugin_store_plugin_t plugin;
    char tag[64];
    bool force;
    plugin_store_state_t state;
} store_job_t;
static bool disk_space(uint64_t bytes) {
    struct statvfs fs;
    return statvfs(STORE_SD_ROOT, &fs) == 0 &&
           (uint64_t) fs.f_bavail * fs.f_frsize >= bytes + STORE_MARGIN;
}

/* Removes staged files, but only from a real directory on the card the
 * operation started on: a symlinked stage must not make this delete
 * elsewhere, and a swapped card's files are not ours. */
/* A staging folder of this operation's own. The name is new each time, so
 * a failed download (http_get_to_file_redirects removes its partial file)
 * or a cleanup can never hit files another card left at a fixed path. */
static void make_stage_path(char * out, size_t size, const char * kind, const char * id) {
    static unsigned stage_counter;
    snprintf(out, size, "%s/%s%.63s.%lu.%u", STORE_WORK, kind, id, (unsigned long) time(NULL),
             ++stage_counter);
}

static void cleanup_stage(const store_card_t * card, const char * path, int count) {
    if (!card_still(card) || !mkdir_tree(STORE_SD_ROOT, path)) {
        return;
    }
    for (int i = 0; i < count; i++) {
        char p[PATH_MAX + 32];
        snprintf(p, sizeof(p), "%s/%d.part", path, i);
        unlink(p);
        snprintf(p, sizeof(p), "%s/%d.backup", path, i);
        unlink(p);
    }
    rmdir(path); /* only succeeds once empty */
}
static void * install_worker(void * arg) {
    store_job_t job = *(store_job_t *) arg;
    free(arg);
    store_card_t card;
    if (!card_mounted(&card)) {
        finish(PLUGIN_STORE_FAILED, TR("Insert an SD card to install plugins."), false);
        return NULL;
    }
    plugin_store_plugin_t p = job.plugin;
    progress_set(0, p.name);
    plugin_store_plugin_t * rec;
    char (*vers)[32];
    size_t rn;
    if (!read_record_alloc(&rec, &vers, &rn)) {
        finish(PLUGIN_STORE_FAILED, TR("Could not read installed plugins."), false);
        return NULL;
    }
    int installed = -1;
    for (size_t i = 0; i < rn; i++) {
        if (strcmp(rec[i].id, p.id) == 0) {
            installed = (int) i;
            break;
        }
    }
    bool updateable = installed >= 0 &&
                      plugin_store_version_compare(p.version, vers[installed]) > 0;
    if (p.api_min > PLUGIN_API_VERSION) {
        free(rec);
        free(vers);
        finish(PLUGIN_STORE_FAILED, TR("This plugin needs a newer player version."), false);
        return NULL;
    }
    if (installed < 0 && rn >= STORE_MAX_RECORD) {
        free(rec);
        free(vers);
        finish(PLUGIN_STORE_FAILED, TR("Too many plugins are installed. Remove one and try again."), false);
        return NULL;
    }
    if (job.state == PLUGIN_STORE_INSTALLING && installed >= 0) {
        free(rec);
        free(vers);
        finish(PLUGIN_STORE_FAILED, TR("This plugin is already installed by the store."), false);
        return NULL;
    }
    if (job.state == PLUGIN_STORE_UPDATING && !updateable) {
        free(rec);
        free(vers);
        finish(PLUGIN_STORE_FAILED, TR("This plugin has no update available."), false);
        return NULL;
    }
    uint64_t total = 0;
    for (int i = 0; i < p.file_count; i++) {
        char final[PATH_MAX];
        struct stat st;
        if (p.files[i].keep &&
            snprintf(final, sizeof(final), "%s/%s", STORE_SD_ROOT, p.files[i].dest) > 0 &&
            lstat(final, &st) == 0) {
            continue;
        }
        total += p.files[i].size;
    }
    if (!disk_space(total)) {
        free(rec);
        free(vers);
        finish(PLUGIN_STORE_FAILED, TR("Not enough free space on the SD card."), false);
        return NULL;
    }
    char stage[PATH_MAX];
    make_stage_path(stage, sizeof(stage), "", p.id);
    if (!download_plugin_files(&p, job.tag, stage, &total)) {
        cleanup_stage(&card, stage, p.file_count);
        free(rec);
        free(vers);
        finish(PLUGIN_STORE_FAILED, TR("The plugin download failed verification. Try again."),
               false);
        return NULL;
    }
    if (!card_still(&card)) {
        /* No cleanup: the stage path now belongs to the other card. */
        free(rec);
        free(vers);
        finish(PLUGIN_STORE_FAILED, TR("The SD card changed during the download."), false);
        return NULL;
    }
    if (conflict_exists(STORE_SD_ROOT, &p, rec, vers, rn) && !job.force) {
        cleanup_stage(&card, stage, p.file_count);
        free(rec);
        free(vers);
        finish(PLUGIN_STORE_NEEDS_CONFIRM,
               TR("A local plugin file will be replaced. Confirm to continue."), false);
        return NULL;
    }
    commit_card = &card;
    bool ok = plugin_store_commit_local(STORE_SD_ROOT, stage, &p, job.force, rec, vers, rn);
    commit_card = NULL;
    bool same = card_still(&card);
    bool needs_confirmation = !ok && same && !job.force &&
                             conflict_exists(STORE_SD_ROOT, &p, rec, vers, rn);
    if (same) {
        cleanup_stage(&card, stage, p.file_count); /* never on another card */
    }
    free(rec);
    free(vers);
    if (!ok) {
        if (!same) {
            finish(PLUGIN_STORE_FAILED, TR("The SD card changed during the operation."), false);
        } else if (needs_confirmation) {
            finish(PLUGIN_STORE_NEEDS_CONFIRM,
                   TR("A local plugin file will be replaced. Confirm to continue."), false);
        } else {
            finish(PLUGIN_STORE_FAILED, TR("Could not install the plugin on the SD card."), false);
        }
        return NULL;
    }
    compute_results();
    finish(PLUGIN_STORE_READY, NULL, true);
    return NULL;
}

/* Snapshot the selected catalog entry and claim the worker slot atomically. */
static bool start_install(const char * id, bool force, plugin_store_state_t state) {
    if (!id || !regex_id(id)) {
        return false;
    }
    store_job_t * job = calloc(1, sizeof(*job));
    if (!job) {
        return false;
    }
    bool found = false;
    pthread_mutex_lock(&store_mutex);
    if (!store_worker_running && last_index) {
        for (size_t i = 0; i < last_index_count; i++) {
            if (strcmp(last_index[i].id, id) == 0) {
                job->plugin = last_index[i];
                snprintf(job->tag, sizeof(job->tag), "%s", last_tag);
                found = true;
                break;
            }
        }
        if (found) {
            store_worker_running = true;
            store_status.state = state;
            store_status.percent = 0;
            store_status.changed = false;
            store_status.error[0] = '\0';
            store_status.current_plugin[0] = '\0';
        }
    }
    pthread_mutex_unlock(&store_mutex);
    if (!found) {
        free(job);
        return false;
    }
    job->force = force;
    job->state = state;
    if (!start_thread(install_worker, job)) {
        free(job);
        finish(PLUGIN_STORE_FAILED, TR("Could not start the plugin operation."), false);
        return false;
    }
    return true;
}
bool plugin_store_refresh(void) {
    if (!begin(PLUGIN_STORE_REFRESHING)) {
        return false;
    }
    if (!start_thread(refresh_worker, NULL)) {
        finish(PLUGIN_STORE_FAILED, TR("Could not start the plugin refresh."), false);
        return false;
    }
    return true;
}

bool plugin_store_install(const char * id, bool force) {
    return start_install(id, force, PLUGIN_STORE_INSTALLING);
}

bool plugin_store_update(const char * id, bool force) {
    return start_install(id, force, PLUGIN_STORE_UPDATING);
}

static bool write_record_without(const char * root, const plugin_store_plugin_t * rec,
                                const char (*vers)[32], size_t rn, const char * remove_id) {
    int * indices = calloc(rn, sizeof(int));
    plugin_store_plugin_t * keep = calloc(rn, sizeof(*keep));
    if ((rn && !indices) || (rn && !keep)) {
        free(indices);
        free(keep);
        return false;
    }
    size_t n = 0;
    for (size_t i = 0; i < rn; i++) {
        if (strcmp(rec[i].id, remove_id) != 0) {
            keep[n] = rec[i];
            indices[n] = (int) n;
            n++;
        }
    }
    char dir[PATH_MAX], path[PATH_MAX];
    bool ok = record_paths(root, dir, sizeof(dir), path, sizeof(path)) && mkdir_tree(root, dir) &&
              record_write_path(path, keep, indices, n);
    free(indices);
    free(keep);
    (void) vers;
    return ok;
}
static void * uninstall_worker(void * arg) {
    char id[64];
    snprintf(id, sizeof(id), "%s", (char *) arg);
    free(arg);
    store_card_t card;
    if (!card_mounted(&card)) {
        finish(PLUGIN_STORE_FAILED, TR("Insert an SD card to remove plugins."), false);
        return NULL;
    }
    plugin_store_plugin_t * rec;
    char (*vers)[32];
    size_t rn;
    if (!read_record_alloc(&rec, &vers, &rn)) {
        finish(PLUGIN_STORE_FAILED, TR("Could not read installed plugins."), false);
        return NULL;
    }
    int found = -1;
    for (size_t i = 0; i < rn; i++) {
        if (strcmp(rec[i].id, id) == 0) {
            found = (int) i;
            break;
        }
    }
    if (found < 0) {
        free(rec);
        free(vers);
        finish(PLUGIN_STORE_FAILED, TR("This plugin is not installed by the store."), false);
        return NULL;
    }
    char stage[PATH_MAX];
    make_stage_path(stage, sizeof(stage), "uninstall-", id);
    if (!mkdir_tree(STORE_SD_ROOT, stage)) {
        free(rec);
        free(vers);
        finish(PLUGIN_STORE_FAILED, TR("Could not prepare plugin removal."), false);
        return NULL;
    }
    bool backed[PLUGIN_STORE_MAX_FILES] = {false};
    for (int i = 0; i < rec[found].file_count; i++) {
        plugin_store_file_t * f = &rec[found].files[i];
        if (f->keep) {
            continue;
        }
        if (!card_still(&card)) {
            /* Another card: restoring backups would write to it. */
            free(rec);
            free(vers);
            finish(PLUGIN_STORE_FAILED, TR("The SD card changed during the operation."), false);
            return NULL;
        }
        char path[PATH_MAX], backup[PATH_MAX + 32];
        struct stat st;
        if (!path_for(path, sizeof(path), STORE_SD_ROOT, f->dest) ||
            !parent_dirs_safe(STORE_SD_ROOT, f->dest, false)) {
            continue;
        }
        if (lstat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            snprintf(backup, sizeof(backup), "%s/%d.backup", stage, i);
            unlink(backup);
            if (rename(path, backup) != 0) {
                for (int j = 0; j < i; j++) {
                    if (backed[j]) {
                        char dst[PATH_MAX], bak[PATH_MAX + 32];
                        path_for(dst, sizeof(dst), STORE_SD_ROOT, rec[found].files[j].dest);
                        snprintf(bak, sizeof(bak), "%s/%d.backup", stage, j);
                        rename(bak, dst);
                    }
                }
                free(rec);
                free(vers);
                finish(PLUGIN_STORE_FAILED, TR("Could not remove a plugin file."), false);
                return NULL;
            }
            backed[i] = true;
        }
    }
    if (!card_still(&card)) {
        /* Another card: restoring backups would write to it. */
        free(rec);
        free(vers);
        finish(PLUGIN_STORE_FAILED, TR("The SD card changed during the operation."), false);
        return NULL;
    }
    commit_card = &card;
    bool ok = write_record_without(STORE_SD_ROOT, rec, vers, rn, id);
    commit_card = NULL;
    bool same = card_still(&card); /* a swapped card's files are left alone */
    if (!ok && same) {
        for (int i = 0; i < rec[found].file_count; i++) {
            if (backed[i]) {
                char dst[PATH_MAX], bak[PATH_MAX + 32];
                path_for(dst, sizeof(dst), STORE_SD_ROOT, rec[found].files[i].dest);
                snprintf(bak, sizeof(bak), "%s/%d.backup", stage, i);
                rename(bak, dst);
            }
        }
    } else if (ok && same) {
        for (int i = 0; i < rec[found].file_count; i++) {
            if (backed[i]) {
                char bak[PATH_MAX + 32];
                snprintf(bak, sizeof(bak), "%s/%d.backup", stage, i);
                unlink(bak);
            }
        }
        rmdir(stage);
    }
    free(rec);
    free(vers);
    if (!ok) {
        finish(PLUGIN_STORE_FAILED, TR("Could not update the installed plugin record."), false);
        return NULL;
    }
    sync();
    compute_results();
    finish(PLUGIN_STORE_READY, NULL, true);
    return NULL;
}
bool plugin_store_uninstall(const char * id) {
    if (!id || !regex_id(id) || !begin(PLUGIN_STORE_UNINSTALLING)) {
        return false;
    }
    char * copy = strdup(id);
    if (!copy) {
        finish(PLUGIN_STORE_FAILED, TR("Could not start the plugin operation."), false);
        return false;
    }
    if (!start_thread(uninstall_worker, copy)) {
        free(copy);
        finish(PLUGIN_STORE_FAILED, TR("Could not start the plugin operation."), false);
        return false;
    }
    return true;
}
static void * update_all_worker(void * unused) {
    (void) unused;
    store_card_t card;
    if (!card_mounted(&card)) {
        finish(PLUGIN_STORE_FAILED, TR("Insert an SD card to update plugins."), false);
        return NULL;
    }
    plugin_store_plugin_t * rec;
    char (*vers)[32];
    size_t rn;
    if (!read_record_alloc(&rec, &vers, &rn)) {
        finish(PLUGIN_STORE_FAILED, TR("Could not read installed plugins."), false);
        return NULL;
    }
    pthread_mutex_lock(&store_mutex);
    size_t index_count = last_index_count;
    char tag[64];
    snprintf(tag, sizeof(tag), "%s", last_tag);
    pthread_mutex_unlock(&store_mutex);
    bool changed = false;
    const char * failure = NULL; /* the last update that failed, reported at the end */
    char pending[65] = "";
    char pending_ids[PLUGIN_STORE_MAX_PLUGINS][64];
    size_t pending_count = 0;
    for (size_t i = 0; i < index_count; i++) {
        plugin_store_plugin_t current;
        pthread_mutex_lock(&store_mutex);
        bool have_plugin = last_index && i < last_index_count;
        if (have_plugin) {
            current = last_index[i];
        }
        pthread_mutex_unlock(&store_mutex);
        if (!have_plugin) {
            break;
        }
        plugin_store_plugin_t * p = &current;
        int r = -1;
        for (size_t j = 0; j < rn; j++) {
            if (!strcmp(p->id, rec[j].id)) {
                r = (int) j;
            }
        }
        if (r < 0 || plugin_store_version_compare(p->version, vers[r]) <= 0 ||
            p->api_min > PLUGIN_API_VERSION) {
            continue;
        }
        progress_set((int) (i * 100 / (index_count ? index_count : 1)), p->name);
        if (conflict_exists(STORE_SD_ROOT, p, rec, vers, rn)) {
            snprintf(pending, sizeof(pending), "%.64s", p->name);
            snprintf(pending_ids[pending_count++], 64, "%.63s", p->id);
            continue;
        }
        uint64_t required = 0;
        for (int j = 0; j < p->file_count; j++) {
            char final[PATH_MAX];
            struct stat st;
            if (p->files[j].keep &&
                snprintf(final, sizeof(final), "%s/%s", STORE_SD_ROOT,
                         p->files[j].dest) > 0 &&
                lstat(final, &st) == 0) {
                continue;
            }
            required += p->files[j].size;
        }
        if (!disk_space(required)) {
            failure = TR("Not enough free space on the SD card.");
            continue;
        }
        char stage[PATH_MAX];
        make_stage_path(stage, sizeof(stage), "", p->id);
        uint64_t bytes = 0;
        if (!download_plugin_files(p, tag, stage, &bytes)) {
            cleanup_stage(&card, stage, p->file_count);
            failure = TR("A plugin download failed verification. Try again.");
            continue;
        }
        if (!card_still(&card)) {
            free(rec);
            free(vers);
            finish(PLUGIN_STORE_FAILED, TR("The SD card changed during the operation."), changed);
            return NULL;
        }
        commit_card = &card;
        bool installed = plugin_store_commit_local(STORE_SD_ROOT, stage, p, false, rec, vers, rn);
        commit_card = NULL;
        if (!installed && !card_still(&card)) {
            free(rec);
            free(vers);
            finish(PLUGIN_STORE_FAILED, TR("The SD card changed during the operation."), changed);
            return NULL;
        }
        if (installed) {
            changed = true;
            free(rec);
            free(vers);
            if (!read_record_alloc(&rec, &vers, &rn)) {
                finish(PLUGIN_STORE_FAILED, TR("Could not read installed plugins."), changed);
                return NULL;
            }
        } else if (conflict_exists(STORE_SD_ROOT, p, rec, vers, rn) &&
                   pending_count < PLUGIN_STORE_MAX_PLUGINS) {
            snprintf(pending, sizeof(pending), "%.64s", p->name);
            snprintf(pending_ids[pending_count++], 64, "%.63s", p->id);
        } else if (!installed) {
            failure = TR("Could not install a plugin on the SD card.");
        }
        cleanup_stage(&card, stage, p->file_count);
    }
    free(rec);
    free(vers);
    compute_results();
    if (pending[0] || failure) {
        pthread_mutex_lock(&store_mutex);
        snprintf(store_status.current_plugin, sizeof(store_status.current_plugin), "%s", pending);
        for (size_t i = 0; i < store_status.result_count; i++) {
            for (size_t j = 0; j < pending_count; j++) {
                if (strcmp(store_results[i].id, pending_ids[j]) == 0) {
                    store_results[i].needs_confirmation = true;
                }
            }
        }
        pthread_mutex_unlock(&store_mutex);
        /* A failure is the more urgent news; the plugins needing
         * confirmation stay marked and can be updated one by one. */
        if (failure) {
            finish(PLUGIN_STORE_FAILED, failure, changed);
        } else {
            finish(PLUGIN_STORE_NEEDS_CONFIRM,
                   TR("Some updates need confirmation before replacing local files."), changed);
        }
    } else {
        finish(PLUGIN_STORE_READY, NULL, changed);
    }
    return NULL;
}
bool plugin_store_update_all(void) {
    pthread_mutex_lock(&store_mutex);
    bool have_index = last_index != NULL;
    pthread_mutex_unlock(&store_mutex);
    if (!have_index) {
        return false;
    }
    if (!begin(PLUGIN_STORE_UPDATING_ALL)) {
        return false;
    }
    if (!start_thread(update_all_worker, NULL)) {
        finish(PLUGIN_STORE_FAILED, TR("Could not start the plugin update."), false);
        return false;
    }
    return true;
}
void plugin_store_get_status(plugin_store_status_t * out, plugin_store_result_t * results,
                             size_t cap) {
    if (!out) {
        return;
    }
    pthread_mutex_lock(&store_mutex);
    *out=store_status;
    size_t n=store_status.result_count;
    if (n > cap) {
        n = cap;
    }
    if (results && store_results && n) {
        memcpy(results, store_results, n * sizeof(*results));
    }
    pthread_mutex_unlock(&store_mutex);
}
bool plugin_store_get_details(const char * id, plugin_store_details_t * out) {
    if (!id || !out) {
        return false;
    }
    bool found = false;
    pthread_mutex_lock(&store_mutex);
    for (size_t i = 0; i < last_index_count; i++) {
        if (strcmp(last_index[i].id, id) == 0) {
            snprintf(out->description, sizeof(out->description), "%s", last_index[i].description);
            snprintf(out->category, sizeof(out->category), "%s", last_index[i].category);
            snprintf(out->author, sizeof(out->author), "%s", last_index[i].author);
            out->size = last_index[i].size;
            found = true;
            break;
        }
    }
    pthread_mutex_unlock(&store_mutex);
    return found;
}
bool plugin_store_busy(void) {
    pthread_mutex_lock(&store_mutex);
    bool busy = store_worker_running;
    pthread_mutex_unlock(&store_mutex);
    return busy;
}

void plugin_store_reset(void) {
    pthread_mutex_lock(&store_mutex);
    if (!store_worker_running) {
        memset(&store_status, 0, sizeof(store_status));
        free(store_results);
        store_results = NULL;
        free(last_index);
        last_index = NULL;
        last_index_count = 0;
        preview_epoch++;
        last_tag[0] = 0;
    }
    pthread_mutex_unlock(&store_mutex);
}
