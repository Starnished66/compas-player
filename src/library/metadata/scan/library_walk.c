#include "library_walk.h"
#include "../../parsers/cue/cue_parser.h"
#include "../../sacd/sacd_iso.h"
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* Kept in sync with audio.c's decoder dispatch. */
static const char * const PLAYABLE_EXTENSIONS[] = {
    ".flac", ".mp3", ".wav", ".rf64", ".w64", ".aiff", ".aif", ".aifc", ".dsf", ".dff", ".iso", ".aac", ".m4a", ".m4b", ".mp4", ".alac", ".wv", ".caf", ".ape", ".wma", ".opus", ".ogg", ".oga",
};

bool file_browser_is_playable_name(const char * name) {
    if (cue_reference_is(name)) {
        char source[PATH_MAX];
        unsigned ordinal;
        return cue_reference_parse(name, source, sizeof(source), &ordinal);
    }
    const char * ext = strrchr(name, '.');
    if (!ext) return false;
    for (size_t i = 0; i < sizeof(PLAYABLE_EXTENSIONS) / sizeof(PLAYABLE_EXTENSIONS[0]); i++) {
        if (strcasecmp(ext, PLAYABLE_EXTENSIONS[i]) == 0) return true;
    }
    return false;
}

/* ISO is a generic container extension; the library walk admits only SACD
 * images. Keep this check out of the pure filename classifier and UI index. */
static bool is_sacd_iso_path(const char * path) {
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    sacd_area_t area;
    bool ok = sacd_read_area(file, &area);
    fclose(file);
    return ok;
}

/* Maximum directory recursion depth when walking song directories. */
#define SCAN_ALL_SONGS_MAX_DEPTH 64


/* Bounded-memory variant used by the database scanner. Does not sort:
 * ordering belongs in the on-disk DB, not in the discovery pass.
 * False means the scan is void. Unreadable entries and subtrees are stepped
 * over and counted in *skipped, which marks the walk incomplete. add_files is
 * inherited from the parent and flipped by database.ignore/database.unignore;
 * subdirectories are descended either way, so a nested database.unignore can
 * re-include a subtree. */
static bool walk_all_songs_recursive(const char * dir_path, file_browser_song_visit_cb_t cb, void * user,
                                     int * count, int depth, atomic_int * progress,
                                     const char * excluded_top_level_dir, int * skipped, bool add_files) {
    if (depth > SCAN_ALL_SONGS_MAX_DEPTH) { (*skipped)++; return true; }

    DIR * dir = opendir(dir_path);
    if (!dir) { (*skipped)++; return true; }

    /* One path buffer for the whole frame: this function recurses to
     * SCAN_ALL_SONGS_MAX_DEPTH on a fixed-size thread stack. */
    char path_buf[PATH_MAX];
    int probe_len = snprintf(path_buf, sizeof(path_buf), "%s/database.ignore", dir_path);
    bool has_ignore = (probe_len > 0 && (size_t) probe_len < sizeof(path_buf) && access(path_buf, F_OK) == 0);

    probe_len = snprintf(path_buf, sizeof(path_buf), "%s/database.unignore", dir_path);
    bool has_unignore = (probe_len > 0 && (size_t) probe_len < sizeof(path_buf) && access(path_buf, F_OK) == 0);

    if (has_ignore != has_unignore) add_files = has_unignore;

    bool fatal = false;
    struct dirent * de;
    for (;;) {
        errno = 0;
        de = readdir(dir);
        if (!de) {
            /* NULL is end-of-directory or a read error; errno separates them.
             * An error hides the rest of this directory, so it counts as an
             * omission rather than a clean end. */
            if (errno) (*skipped)++;
            break;
        }
        if (de->d_name[0] == '.') continue;

        int length = snprintf(path_buf, sizeof(path_buf), "%s/%s", dir_path, de->d_name);
        if (length < 0 || (size_t) length >= sizeof(path_buf)) { (*skipped)++; continue; }

        struct stat st;
        bool stat_ok = lstat(path_buf, &st) == 0;
        if (progress) atomic_fetch_add_explicit(progress, 1, memory_order_relaxed);
        if (!stat_ok) { (*skipped)++; continue; }
        /* Reject symlinks to prevent path traversal outside the music root. */
        if (S_ISLNK(st.st_mode)) continue;

        if (S_ISDIR(st.st_mode)) {
            /* Skip the excluded top-level directory before recursion.
             * depth==0 ensures subdirectories with the same name deeper
             * in the tree are still discovered. */
            if (depth == 0 && excluded_top_level_dir &&
                strcasecmp(de->d_name, excluded_top_level_dir) == 0)
                continue;
            if (!walk_all_songs_recursive(path_buf, cb, user, count, depth + 1, progress,
                                          excluded_top_level_dir, skipped, add_files)) {
                fatal = true;
                break;
            }
            continue;
        }
        if (!file_browser_is_playable_name(de->d_name)) continue;
        const char *ext = strrchr(de->d_name, '.');
        if (ext && strcasecmp(ext, ".iso") == 0 && !is_sacd_iso_path(path_buf)) continue;

        if (add_files) {
            (*count)++;
            if (cb && !cb(path_buf, user)) { fatal = true; break; }
        }
    }

    closedir(dir);
    return !fatal;
}

bool file_browser_walk_all_songs_excluding_top_level(const char * root, const char * excluded_dir,
                                                     file_browser_song_visit_cb_t cb, void * user,
                                                     int * out_count, atomic_int * progress,
                                                     int * out_skipped) {
    int count = 0;
    int skipped = 0;
    /* A root that cannot be opened is a void scan, not an empty library. */
    DIR * root_dir = opendir(root);
    if (!root_dir) {
        if (out_count) *out_count = 0;
        if (out_skipped) *out_skipped = 0;
        return false;
    }
    closedir(root_dir);
    bool completed = walk_all_songs_recursive(root, cb, user, &count, 0, progress, excluded_dir, &skipped, true);
    if (out_count) *out_count = count;
    if (out_skipped) *out_skipped = skipped;
    return completed;
}
