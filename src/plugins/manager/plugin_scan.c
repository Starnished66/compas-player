/* Plugin directory scan, over-limit bookkeeping and plugin_manager_scan_available(). */
#include "../lifecycle/plugin_disabled_list.h"
#include "../../network/store/plugins/plugin_store.h"
#include "../../ui/appearance/gui_themes.h"
#include <dirent.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <errno.h>
#include "plugin_manager_internal.h"
uint64_t plugin_over_limit[PLUGIN_OVER_LIMIT_MAX];
int plugin_over_limit_count = 0;

uint64_t plugin_filename_hash(const char * name) {
    uint64_t h = UINT64_C(14695981039346656037);
    for (const unsigned char * p = (const unsigned char *) name; *p; p++) h = (h ^ *p) * UINT64_C(1099511628211);
    return h;
}

static int plugin_filename_cmp(const void * a, const void * b) {
    return strcmp((const char *) a, (const char *) b);
}

/* Shared by plugin_manager_init() and plugin_manager_scan_available():
 * returns a malloc'd, filename-sorted, UNBOUNDED array of every *.lua
 * filename directly under <SD card>/.plugins/ (caller must free() it), and
 * writes that directory's path into dir_path_out. Returns NULL (and sets
 * *out_count to 0) if there's no .plugins folder or no .lua files in it.
 *
 * Deliberately not capped at PLUGIN_MAX_FILES here -- both callers need to
 * see the FULL on-disk list before applying their own separate cap
 * (plugin_manager_init() only after filtering out disabled names, so a
 * disabled file's load slot is actually reclaimed; plugin_manager_scan_available()
 * Scans and sorts plugin filenames alphabetically to ensure deterministic load order. */
char (*scan_plugin_dir_sorted_names(char dir_path_out[600], int * out_count))[256] {
    *out_count = 0;
    snprintf(dir_path_out, 600, "%s/.plugins", MUSIC_ROOT_DIR);

    DIR * d = opendir(dir_path_out);
    if (!d) return NULL; /* no .plugins folder -- nothing to do, not an error */

    char (*names)[256] = NULL;
    size_t name_count = 0;
    size_t capacity = 0;
    struct dirent * ent;
    for (;;) {
        errno = 0;
        ent = readdir(d);
        if (!ent) {
            if (errno != 0) goto fail;
            break;
        }
        size_t len = strlen(ent->d_name);
        if (len < 5 || strcasecmp(ent->d_name + len - 4, ".lua") != 0) continue;
        if (gui_themes_is_legacy_theme_loader(NULL, ent->d_name)) continue;
        if (name_count >= (size_t) INT_MAX) {
            errno = EOVERFLOW;
            goto fail;
        }
        if (name_count == capacity) {
            size_t next_capacity = capacity ? capacity * 2 : 16;
            if (next_capacity < capacity || next_capacity > (size_t) INT_MAX)
                next_capacity = (size_t) INT_MAX;
            if (next_capacity <= capacity || next_capacity > SIZE_MAX / sizeof(*names)) {
                errno = EOVERFLOW;
                goto fail;
            }
            void * grown = realloc(names, next_capacity * sizeof(*names));
            if (!grown) goto fail;
            names = grown;
            capacity = next_capacity;
        }
        snprintf(names[name_count], sizeof(names[0]), "%s", ent->d_name);
        name_count++;
    }
    if (closedir(d) != 0) {
        free(names);
        return NULL;
    }

    if (name_count == 0) {
        free(names);
        return NULL;
    }
    qsort(names, name_count, sizeof(names[0]), plugin_filename_cmp);
    *out_count = (int) name_count;
    return names;

fail:
    closedir(d);
    free(names);
    return NULL;
}

/* Fills `e` for `filename`, cross-referencing plugin_instances[] for its
 * loaded state and (when loaded) its real display name. */
static void fill_available_entry(plugin_available_entry_t * e, const char * filename) {
    snprintf(e->filename, sizeof(e->filename), "%s", filename);
    e->disabled = plugin_disabled_list_contains(filename);
    e->loaded = false;
    e->over_limit = false;
    e->player_layout = false;
    e->theme_package = false;
    uint64_t h = plugin_filename_hash(filename);
    for (int j = 0; j < plugin_over_limit_count; j++) {
        if (plugin_over_limit[j] == h) {
            e->over_limit = true;
            break;
        }
    }
    snprintf(e->display_name, sizeof(e->display_name), "%s", filename);

    for (int j = 0; j < plugin_instance_count; j++) {
        if (strcmp(plugin_instances[j].filename, filename) == 0) {
            e->loaded = true;
            snprintf(e->display_name, sizeof(e->display_name), "%s", plugin_instances[j].name);
            break;
        }
    }

    /* Layout-only bundles are still represented by their Lua loader file.
     * Detect their companion directory from disk so disabled and over-limit
     * plugins are classified without executing their scripts. */
    size_t filename_len = strlen(filename);
    if (filename_len > 4 && strcasecmp(filename + filename_len - 4, ".lua") == 0) {
        char stem[256];
        size_t stem_len = filename_len - 4;
        if (stem_len < sizeof(stem)) {
            memcpy(stem, filename, stem_len);
            stem[stem_len] = '\0';
            char bundle[900], layout_dir[1000];
            int bn = snprintf(bundle, sizeof(bundle), "%s/%s", MUSIC_ROOT_DIR "/.plugins", stem);
            int ln = (bn > 0 && bn < (int) sizeof(bundle)) ?
                snprintf(layout_dir, sizeof(layout_dir), "%s/player_layouts", bundle) : -1;
            struct stat st;
            if (ln > 0 && ln < (int) sizeof(layout_dir) &&
                lstat(bundle, &st) == 0 && S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode) &&
                lstat(layout_dir, &st) == 0 && S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
                DIR * layouts = opendir(layout_dir);
                if (layouts) {
                    struct dirent * ent;
                    while ((ent = readdir(layouts)) != NULL) {
                        size_t len = strlen(ent->d_name);
                        if (ent->d_name[0] == '.' || len <= 4 ||
                            strcasecmp(ent->d_name + len - 4, ".xml") != 0) continue;
                        char path[1200];
                        int pn = snprintf(path, sizeof(path), "%s/%s", layout_dir, ent->d_name);
                        if (pn <= 0 || pn >= (int) sizeof(path)) continue;
                        if (lstat(path, &st) == 0 && S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode)) {
                            e->player_layout = true;
                            break;
                        }
                    }
                    closedir(layouts);
                }
            }
        }
    }
}

int plugin_manager_scan_available(plugin_available_entry_t * out, int max) {
    char dir_path[600];
    int name_count = 0;
    char (*names)[256] = scan_plugin_dir_sorted_names(dir_path, &name_count);
    if (!names) return 0;

    int count = 0;
    if (name_count <= max) {
        /* Common case -- every file fits, plain alphabetical order. */
        for (int i = 0; i < name_count; i++) fill_available_entry(&out[count++], names[i]);
    } else {
        /* More .lua files on disk than `max` can return. Guarantee every
         * currently-LOADED plugin is still included -- there can only ever
         * be PLUGIN_MAX_FILES of those, always <= max -- rather than
         * truncating in pure alphabetical order and silently hiding an
         * active plugin with no way to toggle it off from this list (real
         * finding: N alphabetically-early disabled files would otherwise
         * push a loaded, alphabetically-later one out of a naive cap).
         * Loaded entries go first, then whatever budget remains is filled
         * in alphabetical order -- sacrifices strict A-Z ordering only in
         * this already-degenerate case. */
        bool * included = calloc((size_t) name_count, sizeof(bool));
        if (!included) { free(names); return 0; }

        for (int i = 0; i < name_count && count < max; i++) {
            bool is_loaded = false;
            for (int j = 0; j < plugin_instance_count; j++) {
                if (strcmp(plugin_instances[j].filename, names[i]) == 0) { is_loaded = true; break; }
            }
            if (is_loaded) {
                fill_available_entry(&out[count++], names[i]);
                included[i] = true;
            }
        }
        for (int i = 0; i < name_count && count < max; i++) {
            if (!included[i]) fill_available_entry(&out[count++], names[i]);
        }
        free(included);
    }

    if (count > 0) {
        const char ** filenames = calloc((size_t) count, sizeof(*filenames));
        bool * layout_flags = calloc((size_t) count, sizeof(*layout_flags));
        bool * theme_flags = calloc((size_t) count, sizeof(*theme_flags));
        if (filenames && layout_flags) {
            for (int i = 0; i < count; i++) {
                filenames[i] = out[i].filename;
                layout_flags[i] = out[i].player_layout;
            }
            plugin_store_classify_loaders(filenames, layout_flags, theme_flags, (size_t) count);
            for (int i = 0; i < count; i++) out[i].player_layout = layout_flags[i];
        }
        if (theme_flags) for (int i = 0; i < count; i++) out[i].theme_package = theme_flags[i];
        free(filenames);
        free(layout_flags);
        free(theme_flags);
    }
    free(names);
    return count;
}
