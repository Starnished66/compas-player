#include "plugin_store_internal.h"
#include "store_common.h"
#include "../../../ui/text/i18n/i18n.h"
#include "../../../library/metadata/scan/library_scan_storage.h" /* sd_card_root_is_mounted() */
#include "../../../hardware/usb/usb_mode_control.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

/* Card side of the Plugin Store: write guards, card identity, the installed
 * record, and the transactional commit of staged plugin files. */

/* Case-insensitive, since the card is FAT. */
static inline bool same_dest(const char * a, const char * b) {
    return strcasecmp(a, b) == 0;
}

/* Plugin writes share the SD card with USB mass storage. Check the live
 * gadget state and filesystem mount flags at every write boundary; persisted
 * USB settings alone do not tell us whether a host is attached. */
bool ps_storage_path_writable(const char * path, char * error, size_t error_size) {
    if (error && error_size) error[0] = '\0';
    if (usb_mode_control_storage_write_blocked()) {
        store_error_text(error, error_size, TR("Disconnect USB storage from the host before changing plugins."));
        return false;
    }
    struct statvfs fs;
    if (!path || statvfs(path, &fs) != 0) {
        store_error_text(error, error_size, TR("Cannot check plugin storage write access."));
        return false;
    }
    if (fs.f_flag & ST_RDONLY) {
        store_error_text(error, error_size, TR("The SD card is read-only. Check its write protection."));
        return false;
    }
    return true;
}

bool plugin_store_storage_writable(char * error, size_t error_size) {
    return ps_storage_path_writable(STORE_SD_ROOT, error, error_size);
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
bool ps_mkdir_tree(const char * root, const char * path) {
    char copy[PATH_MAX];
    if (!ps_storage_path_writable(root, NULL, 0)) return false;
    size_t root_len = strlen(root);
    if (strlen(path) >= sizeof(copy) || strncmp(path, root, root_len) != 0 ||
        path[root_len] != '/') {
        return false;
    }
    strcpy(copy, path);
    for (char * p = copy + root_len + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (!ps_storage_path_writable(root, NULL, 0) || !mkdir_one(copy)) {
                return false;
            }
            *p = '/';
        }
    }
    return ps_storage_path_writable(root, NULL, 0) && mkdir_one(copy);
}

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

bool ps_card_mounted(store_card_t * card) {
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

bool ps_card_still(const store_card_t * card) {
    store_card_t now;
    return ps_card_mounted(&now) && same_card(&now, card);
}

/* The card a running worker started on; ps_commit_staged and the record writes
 * check it before every change they make. NULL skips the check. Only one
 * worker runs at a time. */
const store_card_t * ps_commit_card;

bool ps_path_for(char * out, size_t cap, const char * root, const char * dest) {
    if (!plugin_store_dest_valid(dest)) {
        return false;
    }
    int n = snprintf(out, cap, "%s/%s", root, dest);
    return n > 0 && (size_t) n < cap;
}

/* <stage>/<i>.<ext>: the staged download (part) or the replaced original (backup). */
void ps_stage_file(char * out, size_t size, const char * stage, size_t i, const char * ext) {
    snprintf(out, size, "%s/%zu.%s", stage, i, ext);
}

bool ps_parent_dirs_safe(const char * root, const char * dest, bool create) {
    char path[PATH_MAX];
    if (!ps_path_for(path, sizeof(path), root, dest)) {
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

/* A worker's next card write is allowed: the card is writable and, while a
 * worker runs, still the one it started on. */
static bool commit_target_ok(const char * path) {
    return ps_storage_path_writable(path, NULL, 0) && (!ps_commit_card || ps_card_still(ps_commit_card));
}

/* A file belongs to plugin owner only when the record lists it under that
 * plugin with the same hash. Another plugin's file is a conflict: replacing
 * it needs confirmation, and the record then moves it to owner. */
static bool tracked_same(const char * dest, const char * sha, const char * owner,
                         const plugin_store_plugin_t * rec, size_t n) {
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

int ps_record_find(const plugin_store_plugin_t * rec, size_t rn, const char * id) {
    for (size_t i = 0; i < rn; i++) {
        if (strcmp(rec[i].id, id) == 0) {
            return (int) i;
        }
    }
    return -1;
}

typedef enum { DEST_ABSENT, DEST_OWNED, DEST_CONFLICT, DEST_ERROR } dest_check_t;

/* What stands at a plugin file's destination: nothing, the owner's own
 * recorded file, another file (conflict), or something unreadable (error). */
static dest_check_t dest_check(const char * path, const plugin_store_file_t * f, const char * owner,
                               const plugin_store_plugin_t * rec, size_t rn) {
    struct stat st;
    if (lstat(path, &st) != 0) {
        return errno == ENOENT ? DEST_ABSENT : DEST_ERROR;
    }
    char sha[65];
    uint64_t size;
    if (!S_ISREG(st.st_mode) || !store_hash_file(path, &size, sha)) {
        return DEST_ERROR;
    }
    return tracked_same(f->dest, sha, owner, rec, rn) ? DEST_OWNED : DEST_CONFLICT;
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
    char parent[PATH_MAX];
    snprintf(parent, sizeof(parent), "%s", path);
    char * slash = strrchr(parent, '/');
    if (slash) *slash = '\0';
    if (!ps_storage_path_writable(parent, NULL, 0)) return false;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
    if (fd < 0) {
        return false;
    }
    FILE * f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        if (ps_storage_path_writable(parent, NULL, 0)) unlink(tmp);
        return false;
    }
    bool ok = true;
    for (size_t i = 0; i < count && ok; i++) {
        if (!commit_target_ok(parent)) { ok = false; break; }
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
    if (!commit_target_ok(parent)) {
        return false; /* the temporary file stays on the card it was written to */
    }
    if (!ok || rename(tmp, path) != 0) {
        if (commit_target_ok(parent)) unlink(tmp);
        return false;
    }
    return true;
}

static bool record_paths(const char * root, char * plugins_dir, size_t pn,
                         char * record, size_t rn) {
    return snprintf(plugins_dir, pn, "%s/.plugins", root) < (int) pn &&
           snprintf(record, rn, "%s/.plugins/.store_installed", root) < (int) rn;
}

static bool record_write(const char * root, const plugin_store_plugin_t * plugins,
                         const int * indices, size_t count) {
    char dir[PATH_MAX], path[PATH_MAX];
    if (!record_paths(root, dir, sizeof(dir), path, sizeof(path)) || !ps_mkdir_tree(root, dir)) {
        return false;
    }
    return record_write_path(path, plugins, indices, count);
}

/* Classifies the record file: OK with *present set when it is a regular file
 * or absent, DAMAGED when something else sits at the path, ERROR when it
 * cannot be examined. */
static ps_record_status_t record_stat(const char * path, bool * present) {
    struct stat st;
    *present = false;
    if (lstat(path, &st) != 0) {
        return (errno == ENOENT || errno == ENOTDIR) ? PS_RECORD_OK : PS_RECORD_ERROR;
    }
    if (!S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
        return PS_RECORD_DAMAGED;
    }
    *present = true;
    return PS_RECORD_OK;
}

/* Examines the .plugins directory once. OK with *present false when it is
 * missing (no record yet), DAMAGED when it is a symlink or anything but a
 * directory, ERROR when it cannot be examined. */
static ps_record_status_t record_dir_check(const char * dir, bool * present) {
    struct stat st;
    *present = false;
    if (lstat(dir, &st) != 0) {
        return (errno == ENOENT || errno == ENOTDIR) ? PS_RECORD_OK : PS_RECORD_ERROR;
    }
    if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
        return PS_RECORD_DAMAGED;
    }
    *present = true;
    return PS_RECORD_OK;
}

static bool record_dest_taken(const plugin_store_plugin_t * plugins, size_t count, const char * dest) {
    for (size_t r = 0; r < count; r++) {
        for (int j = 0; j < plugins[r].file_count; j++) {
            if (same_dest(dest, plugins[r].files[j].dest)) return true;
        }
    }
    return false;
}

/* Parses a "P id version" line, appending a plugin. False when invalid. */
static bool record_parse_plugin(char * line, plugin_store_plugin_t * plugins, size_t cap,
                                size_t * count) {
    char id[64], ver[32], extra;
    if ((*count && plugins[*count - 1].file_count == 0) ||
        sscanf(line, "P %63s %31s %c", id, ver, &extra) != 2 ||
        !ps_regex_id(id) || !store_version_valid(ver) || *count >= cap) {
        return false;
    }
    for (size_t i = 0; i < *count; i++) {
        if (!strcmp(plugins[i].id, id)) return false;
    }
    memset(&plugins[*count], 0, sizeof(plugins[*count]));
    snprintf(plugins[*count].id, 64, "%s", id);
    snprintf(plugins[*count].version, 32, "%s", ver);
    (*count)++;
    return true;
}

/* Splits "F<TAB>dest<TAB>sha<TAB>keep". False when malformed. */
static bool record_split_file(char * line, char ** dest, char ** sha, int * keep) {
    *dest = line + 2;
    *sha = strchr(*dest, '\t');
    if (!*sha) return false;
    *(*sha)++ = 0;
    char * keep_text = strchr(*sha, '\t');
    if (!keep_text) return false;
    *keep_text++ = 0;
    char * end = NULL;
    errno = 0;
    long keep_value = strtol(keep_text, &end, 10);
    *keep = (int) keep_value;
    return !errno && end != keep_text && *end == '\0' && (*keep == 0 || *keep == 1);
}

/* Parses an "F" line into the last plugin. False when invalid. */
static bool record_parse_file(char * line, plugin_store_plugin_t * plugins, size_t count) {
    char *dest, *sha;
    int keep;
    if (!count || !record_split_file(line, &dest, &sha, &keep)) return false;
    plugin_store_plugin_t * cur = &plugins[count - 1];
    if (!plugin_store_dest_valid(dest) || !store_sha_valid(sha) ||
        cur->file_count >= PLUGIN_STORE_MAX_FILES || record_dest_taken(plugins, count, dest)) {
        return false;
    }
    plugin_store_file_t * fentry = &cur->files[cur->file_count++];
    memcpy(fentry->dest, dest, strlen(dest) + 1);
    memcpy(fentry->sha256, sha, 65);
    fentry->keep = keep != 0;
    return true;
}

/* Reads every line of the open record. False when a line is invalid. */
static bool record_parse_lines(FILE * f, plugin_store_plugin_t * plugins, size_t cap,
                               size_t * count) {
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        if (n && line[n - 1] == '\n') {
            line[--n] = 0;
        } else if (!feof(f)) {
            return false;
        }
        bool ok;
        if (!strncmp(line, "P ", 2)) {
            ok = record_parse_plugin(line, plugins, cap, count);
        } else if (!strncmp(line, "F\t", 2)) {
            ok = record_parse_file(line, plugins, *count);
        } else {
            ok = false;
        }
        if (!ok) return false;
    }
    return true;
}

/* Fills plugins and *out_count from the record file, which exists. */
static ps_record_status_t plugin_store_record_read(const char * path, plugin_store_plugin_t * plugins,
                                                   size_t cap, size_t * out_count) {
    *out_count = 0;
    FILE * f = fopen(path, "r");
    if (!f) {
        return PS_RECORD_ERROR;
    }
    size_t count = 0;
    bool valid = record_parse_lines(f, plugins, cap, &count);
    bool io_error = ferror(f) != 0;
    if (io_error || (count && plugins[count - 1].file_count == 0)) {
        valid = false;
    }
    fclose(f);
    if (!valid) {
        memset(plugins, 0, cap * sizeof(*plugins));
        return io_error ? PS_RECORD_ERROR : PS_RECORD_DAMAGED;
    }
    *out_count = count;
    return PS_RECORD_OK;
}

static bool write_record_after(const char * root, const plugin_store_plugin_t * newp,
                               const plugin_store_plugin_t * oldp, size_t oldn) {
    size_t cap = oldn + 1;
    plugin_store_plugin_t * all = calloc(cap, sizeof(*all));
    int * idx = calloc(cap, sizeof(int));
    if (!all || !idx) {
        free(all);
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
        idx[n] = (int) n;
        n++;
    }
    all[n] = *newp;
    idx[n] = (int) n;
    n++;
    bool ok = record_write(root, all, idx, n);
    free(all);
    free(idx);
    return ok;
}

/* Per-file working state of one commit. */
typedef struct {
    size_t n;
    char (*dests)[PATH_MAX];
    char (*backs)[PATH_MAX];
    bool * existed;   /* the destination held a file that gets backed up */
    bool * skip;      /* a keep file already on the card */
    bool * installed; /* the staged file is in place */
    bool mutated;     /* the card has been changed */
} commit_files_t;

typedef enum {
    STEP_OK,
    STEP_FAIL,
    STEP_CONFLICT,
    STEP_ROLLBACK,
    STEP_CARD_LOST,
    STEP_ROLLBACK_INCOMPLETE
} commit_step_t;

static bool commit_files_alloc(commit_files_t * c, size_t n) {
    c->n = n;
    c->dests = calloc(n, sizeof(*c->dests));
    c->backs = calloc(n, sizeof(*c->backs));
    c->existed = calloc(n, sizeof(bool));
    c->skip = calloc(n, sizeof(bool));
    c->installed = calloc(n, sizeof(bool));
    c->mutated = false;
    return c->dests && c->backs && c->existed && c->skip && c->installed;
}

static void commit_files_free(commit_files_t * c) {
    free(c->dests);
    free(c->backs);
    free(c->existed);
    free(c->skip);
    free(c->installed);
}

/* Checks every destination and staged file before anything is replaced. */
static commit_step_t commit_check(commit_files_t * c, const char * root, const char * stage,
                                  const plugin_store_plugin_t * p, bool force,
                                  const plugin_store_plugin_t * rec, size_t rn) {
    for (size_t i = 0; i < c->n; i++) {
        const plugin_store_file_t * f = &p->files[i];
        if (!ps_path_for(c->dests[i], PATH_MAX, root, f->dest)) {
            return STEP_FAIL;
        }
        if (f->keep) {
            struct stat st;
            if (lstat(c->dests[i], &st) == 0) {
                c->skip[i] = true;
                continue;
            }
        }
        dest_check_t check = dest_check(c->dests[i], f, p->id, rec, rn);
        if (check == DEST_ERROR) {
            return STEP_FAIL;
        }
        if (check == DEST_CONFLICT && !force) {
            return STEP_CONFLICT;
        }
        c->existed[i] = check != DEST_ABSENT;
        char part[PATH_MAX], sha[65];
        uint64_t size;
        ps_stage_file(part, sizeof(part), stage, i, "part");
        if (!store_hash_file(part, &size, sha) || size != f->size || strcmp(sha, f->sha256) != 0) {
            return STEP_FAIL;
        }
    }
    return STEP_OK;
}

/* Backs up replaced files and moves the staged ones into place. */
static commit_step_t commit_move(commit_files_t * c, const char * root, const char * stage,
                                 const plugin_store_plugin_t * p) {
    for (size_t i = 0; i < c->n; i++) {
        if (c->skip[i]) {
            continue;
        }
        const plugin_store_file_t * f = &p->files[i];
        if (!commit_target_ok(root)) return STEP_CARD_LOST;
        if (!ps_parent_dirs_safe(root, f->dest, true)) {
            return STEP_ROLLBACK;
        }
        char part[PATH_MAX];
        ps_stage_file(part, sizeof(part), stage, i, "part");
        if (c->existed[i]) {
            ps_stage_file(c->backs[i], PATH_MAX, stage, i, "backup");
            if (rename(c->dests[i], c->backs[i]) != 0) {
                c->backs[i][0] = '\0';
                return STEP_ROLLBACK;
            }
            c->mutated = true;
        }
        if (rename(part, c->dests[i]) != 0) {
            return STEP_ROLLBACK;
        }
        c->mutated = true;
        c->installed[i] = true;
    }
    return STEP_OK;
}

/* Remove dropped files only when they still match the old installed hash. */
static void commit_remove_dropped(const char * root, const plugin_store_plugin_t * p,
                                  const plugin_store_plugin_t * rec, size_t rn) {
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
            if (ps_path_for(old_path, sizeof(old_path), root, old_file->dest) &&
                ps_parent_dirs_safe(root, old_file->dest, false) &&
                commit_target_ok(root) &&
                lstat(old_path, &old_stat) == 0 && S_ISREG(old_stat.st_mode) &&
                store_hash_file(old_path, &old_size, old_sha) &&
                strcmp(old_sha, old_file->sha256) == 0) {
                unlink(old_path);
            }
        }
    }
}

static void commit_drop_backups(const commit_files_t * c, const char * root) {
    if (commit_target_ok(root)) {
        for (size_t i = 0; i < c->n; i++) {
            if (c->backs[i][0] && commit_target_ok(root)) unlink(c->backs[i]);
        }
    }
}

/* Undoes the moves in reverse order. */
static commit_step_t commit_rollback(const commit_files_t * c, const char * root) {
    if (!commit_target_ok(root)) return STEP_CARD_LOST;
    for (size_t i = c->n; i > 0; i--) {
        size_t k = i - 1;
        if (c->backs[k][0]) {
            if (c->installed[k]) {
                if (!commit_target_ok(root)) return STEP_CARD_LOST;
                if (unlink(c->dests[k]) != 0 && errno != ENOENT) return STEP_ROLLBACK_INCOMPLETE;
            }
            if (!commit_target_ok(root)) return STEP_CARD_LOST;
            if (rename(c->backs[k], c->dests[k]) != 0) return STEP_ROLLBACK_INCOMPLETE;
        } else if (c->installed[k]) {
            if (!commit_target_ok(root)) return STEP_CARD_LOST;
            if (unlink(c->dests[k]) != 0 && errno != ENOENT) return STEP_ROLLBACK_INCOMPLETE;
        }
    }
    return STEP_FAIL;
}

static void commit_discard_parts(const commit_files_t * c, const char * root, const char * stage) {
    for (size_t i = 0; i < c->n && commit_target_ok(root); i++) {
        char part[PATH_MAX];
        ps_stage_file(part, sizeof(part), stage, i, "part");
        unlink(part);
    }
}

bool ps_commit_staged(const char * root, const char * stage,
                          const plugin_store_plugin_t * p, bool force,
                          const plugin_store_plugin_t * rec, size_t rn,
                          bool * preserve_stage) {
    if (preserve_stage) *preserve_stage = false;
    char plugins_path[PATH_MAX], record[PATH_MAX];
    if (!record_paths(root, plugins_path, sizeof(plugins_path), record, sizeof(record))) {
        return false;
    }
    commit_files_t c;
    if (!commit_files_alloc(&c, (size_t) p->file_count)) {
        commit_files_free(&c);
        return false;
    }
    commit_step_t step = commit_check(&c, root, stage, p, force, rec, rn);
    if (step == STEP_OK) {
        step = commit_move(&c, root, stage, p);
        if (step == STEP_OK && !commit_target_ok(root)) step = STEP_CARD_LOST;
        if (step == STEP_OK && !write_record_after(root, p, rec, rn)) step = STEP_ROLLBACK;
    }
    if (step == STEP_ROLLBACK) {
        step = commit_rollback(&c, root);
    }
    switch (step) {
    case STEP_OK:
        commit_remove_dropped(root, p, rec, rn);
        commit_drop_backups(&c, root);
        commit_files_free(&c);
        sync();
        return true;
    case STEP_FAIL:
        commit_discard_parts(&c, root, stage);
        break;
    case STEP_ROLLBACK_INCOMPLETE:
        if (preserve_stage) *preserve_stage = true;
        break;
    case STEP_CARD_LOST:
        /* Another card is mounted now: rolling back would delete and rename
         * files on it that belong to it. Leave both cards as they are. */
        if (preserve_stage) *preserve_stage = c.mutated;
        break;
    case STEP_CONFLICT:
    case STEP_ROLLBACK:
        break;
    }
    commit_files_free(&c);
    return false;
}

/* Counts the "P " headers of the existing record so the array can be sized. */
static ps_record_status_t record_count_headers(const char * path, size_t * capacity) {
    *capacity = 0;
    FILE * file = fopen(path, "r");
    if (!file) {
        return PS_RECORD_ERROR;
    }
    char line[512];
    while (fgets(line, sizeof(line), file)) {
        if (strncmp(line, "P ", 2) == 0) {
            (*capacity)++;
        }
    }
    bool read_error = ferror(file) != 0;
    fclose(file);
    if (read_error) {
        return PS_RECORD_ERROR;
    }
    return *capacity > STORE_MAX_RECORD ? PS_RECORD_DAMAGED : PS_RECORD_OK;
}

/* A missing .plugins or record is OK with no entries; a .plugins that is a
 * symlink or not a directory is DAMAGED. Count record headers first so large plugin structures are allocated on demand. */
ps_record_status_t ps_read_record_alloc(plugin_store_plugin_t ** out, size_t * count) {
    char plugins_dir[PATH_MAX];
    char record_path[PATH_MAX];
    if (!record_paths(STORE_SD_ROOT, plugins_dir, sizeof(plugins_dir), record_path,
                      sizeof(record_path))) {
        return PS_RECORD_ERROR;
    }
    bool dir_present, present = false;
    ps_record_status_t status = record_dir_check(plugins_dir, &dir_present);
    if (status == PS_RECORD_OK && dir_present) {
        status = record_stat(record_path, &present);
    }
    if (status != PS_RECORD_OK) {
        return status;
    }
    *count = 0;
    size_t capacity = 0;
    if (present) {
        status = record_count_headers(record_path, &capacity);
        if (status != PS_RECORD_OK) {
            return status;
        }
    }
    *out = calloc(capacity ? capacity : 1, sizeof(**out));
    if (!*out) {
        return PS_RECORD_ERROR;
    }
    status = present ? plugin_store_record_read(record_path, *out, capacity, count) : PS_RECORD_OK;
    if (status != PS_RECORD_OK) {
        free(*out);
        *out = NULL;
    }
    return status;
}

bool ps_conflict_exists(const char * root, const plugin_store_plugin_t * p,
                            const plugin_store_plugin_t * rec, size_t rn) {
    for (int i = 0; i < p->file_count; i++) {
        const plugin_store_file_t * f = &p->files[i];
        if (f->keep) {
            continue;
        }
        char path[PATH_MAX];
        if (!ps_path_for(path, sizeof(path), root, f->dest)) {
            return true;
        }
        dest_check_t check = dest_check(path, f, p->id, rec, rn);
        if (check == DEST_CONFLICT || check == DEST_ERROR) {
            return true;
        }
    }
    return false;
}

bool ps_write_record_without(const char * root, const plugin_store_plugin_t * rec,
                                size_t rn, const char * remove_id) {
    int * indices = calloc(rn, sizeof(int));
    if (rn && !indices) {
        return false;
    }
    size_t n = 0;
    for (size_t i = 0; i < rn; i++) {
        if (strcmp(rec[i].id, remove_id) != 0) {
            indices[n++] = (int) i;
        }
    }
    bool ok = record_write(root, rec, indices, n);
    free(indices);
    return ok;
}
