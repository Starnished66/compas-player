#include "plugin_store_internal.h"
#include "store_common.h"
#include "../../../ui/text/i18n/i18n.h"
#include "../../../plugins/manager/plugin_api_version.h"
#include "../../../core/system/runtime/board_config.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

/* Catalog side of the Plugin Store: validating and parsing the release index,
 * classifying what a plugin delivers, and naming its player layout. */

bool ps_regex_id(const char * s) {
    if (!s || !*s || strlen(s) > 63 || (!store_ascii_lower(*s) && !store_ascii_digit(*s))) {
        return false;
    }
    for (; *s; s++) {
        if (!store_ascii_lower(*s) && !store_ascii_digit(*s) && *s != '_' && *s != '.' && *s != '-') {
            return false;
        }
    }
    return true;
}

bool plugin_store_dest_valid(const char * dest) {
    if (!store_dest_path_valid(dest)) {
        return false;
    }
    /* The player's own folder is never a plugin destination. */
    if (strncasecmp(dest, ".compas", 7) == 0 && (dest[7] == '/' || dest[7] == '\0')) {
        return false;
    }
    /* Dot files in .plugins are the player's own (the store record and its
     * temporary file, the disabled list). */
    if (strncasecmp(dest, ".plugins/.", 10) == 0) {
        return false;
    }
    size_t n = strlen(dest);
    return n < 4 || strcasecmp(dest + n - 4, ".upt") != 0;
}

static bool parse_preview_metadata(const cJSON * value, plugin_store_preview_t * out) {
    if (!cJSON_IsObject(value)) return false;
    const char * asset = store_json_string(value, "asset");
    const char * sha = store_json_string(value, "sha256");
    plugin_store_preview_t parsed = {0};
    if (!store_token_valid(asset, 127, true) || !store_sha_valid(sha) ||
        !store_json_uint(value, "size", 1, STORE_PREVIEW_LIMIT, &parsed.size) ||
        !store_json_uint(value, "width", 1, 240, &parsed.width) ||
        !store_json_uint(value, "height", 1, 400, &parsed.height)) {
        return false;
    }
    snprintf(parsed.asset, sizeof(parsed.asset), "%s", asset);
    snprintf(parsed.sha256, sizeof(parsed.sha256), "%s", sha);
    *out = parsed;
    return true;
}

void store_preview_bounds(int * width, int * height) {
    *width = (BOARD_SCREEN_WIDTH - 2 * BOARD_SCALE_PX(16) - BOARD_SCALE_PX(14)) / 2;
    *height = *width * 3 / 2;
}

static bool parse_plugin_preview(const cJSON * preview, plugin_store_preview_t * out) {
    plugin_store_preview_t candidates[4];
    if (!parse_preview_metadata(preview, &candidates[0])) return false;

    const cJSON * variants = store_json_field(preview, "variants");
    int variant_count = 0;
    if (variants) {
        if (!cJSON_IsArray(variants)) return false;
        variant_count = cJSON_GetArraySize(variants);
        if (variant_count > 3) return false;
        for (int i = 0; i < variant_count; i++) {
            if (!parse_preview_metadata(cJSON_GetArrayItem(variants, i), &candidates[i + 1])) {
                return false;
            }
        }
    }

    /* The bounds use the Store's two-column cover layout. */
    int max_width, max_height;
    store_preview_bounds(&max_width, &max_height);
    int best = -1;
    uint64_t best_area = 0;
    for (int i = 0; i <= variant_count; i++) {
        if (candidates[i].width > (uint32_t)max_width || candidates[i].height > (uint32_t)max_height) continue;
        uint64_t area = (uint64_t)candidates[i].width * candidates[i].height;
        if (best < 0 || area > best_area) {
            best = i;
            best_area = area;
        }
    }
    /* The publisher's default remains a compatible fallback on older indexes
     * whose preview exceeds this board's card bounds. */
    *out = candidates[best >= 0 ? best : 0];
    return true;
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
    store_error_text(error, error_size, TR("Unexpected reply from GitHub."));
    if (!json || !out || !tag_out || length > STORE_INDEX_LIMIT) {
        return false;
    }

    cJSON * root = cJSON_ParseWithLength(json, length);
    plugin_store_plugin_t * parsed = NULL;
    uint32_t schema;
    const char * tag = store_json_string(root, "tag");
    const cJSON * plugins = store_json_field(root, "plugins");
    if (!cJSON_IsObject(root) || !store_json_uint(root, "schema", 1, 1, &schema) ||
        !tag || !store_token_valid(tag, 63, true) || !cJSON_IsArray(plugins) ||
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

        const char * id = store_json_string(p, "id");
        const char * name = store_json_string(p, "name");
        const char * ver = store_json_string(p, "version");
        const char * desc = store_json_string(p, "description");
        const char * cat = store_json_string(p, "category");
        const char * author = store_json_string(p, "author");
        if (!cJSON_IsObject(p) || !ps_regex_id(id) || !store_safe_text(name,64,false) || !store_version_valid(ver) ||
            !store_safe_text(desc,400,true) || !store_safe_text(cat,32,true) || !store_safe_text(author,64,true) ||
            !store_json_uint(p, "api_min", 1, UINT32_MAX, &d->api_min) ||
            !store_json_uint(p, "size", 0, UINT32_MAX, &d->size)) {
            goto fail;
        }
        snprintf(d->id, sizeof(d->id), "%s", id);
        snprintf(d->name, sizeof(d->name), "%s", name);
        snprintf(d->version, sizeof(d->version), "%s", ver);
        snprintf(d->description, sizeof(d->description), "%s", desc);
        snprintf(d->category, sizeof(d->category), "%s", cat);
        snprintf(d->author, sizeof(d->author), "%s", author);

        const cJSON * preview = store_json_field(p, "preview");
        if (preview && !parse_plugin_preview(preview, &d->preview)) {
            goto fail;
        }

        const cJSON * files = store_json_field(p, "files");
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
            const char * asset = store_json_string(f, "asset");
            const char * dest = store_json_string(f, "dest");
            const char * sha = store_json_string(f, "sha256");
            uint32_t size;
            const cJSON * keep = store_json_field(f, "keep");
            if (!cJSON_IsObject(f) || !store_token_valid(asset, 127, true) ||
                !plugin_store_dest_valid(dest) || !store_sha_valid(sha) ||
                !store_json_uint(f, "size", 1, STORE_FILE_LIMIT, &size) ||
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
                if (store_dests_collide(df->dest, d->files[k].dest)) {
                    goto fail;
                }
            }
            for (int a = 0; a < i; a++) {
                for (int b = 0; b < parsed[a].file_count; b++) {
                    if (store_dests_collide(df->dest, parsed[a].files[b].dest)) {
                        goto fail;
                    }
                }
            }
        }
        if (!has_script || total != d->size) {
            goto fail;
        }
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
    store_error_text(error, error_size, "");
    return true;
fail:
    free(parsed);
    cJSON_Delete(root);
    return false;
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
        if (store_version_compare(p->version, installed) <= 0) {
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

static bool plugin_contains_theme_pack(const plugin_store_plugin_t * plugin) {
    for (int f = 0; f < plugin->file_count; f++) {
        const char * dest = plugin->files[f].dest;
        size_t len = strlen(dest);
        /* Recognized via dest Themes/star.theme. Must align with native gui_themes_scan
         * eligibility:
         * 1. Path begins with "Themes/"
         * 2. Strict lowercase ".theme" extension
         * 3. Max depth <= 2 subdirectories beneath Themes/ (e.g. Themes/file.theme,
         *    Themes/sub1/file.theme, Themes/sub1/sub2/file.theme)
         * 4. Total theme identifier relative to Themes/ is < 64 chars (GUI_THEMES_MAX_NAME)
         *    so the installed pack will have at least one browsable theme in the picker.
         */
        if (strncmp(dest, "Themes/", 7) != 0) continue;
        if (len <= 13 || strcmp(dest + len - 6, ".theme") != 0) continue;

        const char * rel = dest + 7;
        size_t rel_len = strlen(rel);
        if (rel_len >= 64 || strstr(rel, "..") || strchr(rel, '\\')) continue; /* Native identifier policy. */

        int slashes = 0;
        bool valid_depth = true;
        for (const char * p = rel; *p; p++) {
            if ((unsigned char)*p < 32 || (unsigned char)*p == 127 ||
                ((p == rel || p[-1] == '/') && (*p == '.' || *p == '/'))) {
                valid_depth = false;
                break;
            }
            if (*p == '/') {
                slashes++;
                if (slashes > 2) {
                    valid_depth = false;
                    break;
                }
            }
        }
        if (valid_depth) {
            return true;
        }
    }
    return false;
}

static bool plugin_has_layout_xml(const plugin_store_plugin_t * plugin) {
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

/* Mixed XML / theme precedence: If a plugin package delivers both .theme
 * files and layout XML (e.g. Themes.lua or bundles), it is classified as
 * a theme_pack first and excluded from player_layout. HiByStockPlayer and
 * other pure XML layouts without .theme files remain ordinary player layouts.
 */
void ps_plugin_kind(const plugin_store_plugin_t * plugin, bool * layout, bool * theme) {
    *theme = plugin_contains_theme_pack(plugin);
    *layout = !*theme && plugin_has_layout_xml(plugin);
}

/* Only layout and theme packs with a catalog preview get one. */
bool ps_preview_eligible(const plugin_store_plugin_t * plugin) {
    bool layout, theme;
    if (!plugin->preview.asset[0]) {
        return false;
    }
    ps_plugin_kind(plugin, &layout, &theme);
    return layout || theme;
}

bool ps_make_asset_url(char * out, size_t cap, const char * tag, const char * asset) {
    int n = snprintf(out, cap,
                     "https://github.com/Starnished66/compas-plugins/releases/download/%s/%s",
                     tag, asset);
    return n > 0 && (size_t) n < cap;
}

static bool layout_stem_valid(const char * stem) {
    if (!stem || !stem[0] || strlen(stem) >= 64) return false;
    for (const unsigned char * p = (const unsigned char *) stem; *p; ++p)
        if (!store_ascii_alnum((char)*p) && *p != '_' && *p != '.' && *p != '-') return false;
    return true;
}

static bool layout_filename_is_primary(const char * name) {
    size_t len = name ? strlen(name) : 0;
    if (len < 5 || strcasecmp(name + len - 4, ".xml") != 0 || strchr(name, '@')) return false;
    size_t stem_len = len - 4;
    char stem[64];
    if (!stem_len || stem_len >= sizeof(stem)) return false;
    memcpy(stem, name, stem_len);
    stem[stem_len] = '\0';
    const char * underscore = strrchr(stem, '_');
    if (underscore && (size_t)(stem + stem_len - underscore) >= 4) {
        const char * p = underscore + 1;
        const char * end = stem + stem_len;
        const char * x = p;
        while (x < end && store_ascii_digit(*x)) ++x;
        if (x > p && x < end && *x == 'x') {
            const char * h = x + 1;
            while (h < end && store_ascii_digit(*h)) ++h;
            if (h == end && h > x + 1) return false;
        }
    }
    return layout_stem_valid(stem);
}

bool ps_layout_id_from_dest(const char * dest, char * out, size_t size) {
    if (!plugin_store_dest_valid(dest)) return false;
    const char * filename = NULL;
    char prefix[64] = "";
    if (strncmp(dest, ".plugins/player_layouts/", 24) == 0) {
        filename = dest + 24;
    } else if (strncmp(dest, ".plugins/", 9) == 0) {
        const char * rest = dest + 9;
        const char * slash = strchr(rest, '/');
        if (!slash) {
            filename = rest;
            snprintf(prefix, sizeof(prefix), "plugin.");
        } else {
            if (strncmp(slash, "/player_layouts/", 16) != 0) return false;
            size_t bundle_len = (size_t)(slash - rest);
            char bundle[64];
            if (!bundle_len || bundle_len >= sizeof(bundle)) return false;
            memcpy(bundle, rest, bundle_len);
            bundle[bundle_len] = '\0';
            if (!layout_stem_valid(bundle) || snprintf(prefix, sizeof(prefix), "plugin.%s.", bundle) >= (int) sizeof(prefix))
                return false;
            filename = slash + 16;
        }
    } else {
        return false;
    }
    if (!filename || strchr(filename, '/') || !layout_filename_is_primary(filename)) return false;
    char stem[64];
    size_t len = strlen(filename) - 4;
    if (!len || len >= sizeof(stem)) return false;
    memcpy(stem, filename, len);
    stem[len] = '\0';
    int n = snprintf(out, size, "%s%s", prefix, stem);
    return n > 0 && (size_t)n < size && layout_stem_valid(out) && strcmp(out, "default") != 0;
}
