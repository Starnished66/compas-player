#include "theme_file.h"
#include "theme_file_values.h"

#include <stdio.h>
#include <string.h>

#define THEME_MAX_LINE_LEN 512
#define THEME_MAX_ORDER_LEN 512

static void set_err(char *error, size_t error_size, const char *msg) {
    if (error && error_size > 0) {
        snprintf(error, error_size, "%s", msg);
    }
}

static bool close_with_error(FILE *file, char *error, size_t error_size, const char *message) {
    fclose(file);
    set_err(error, error_size, message);
    return false;
}

static int find_or_add_tile(home_layout_config_t *home, const char *key) {
    for (int i = 0; i < home->tile_count; i++) {
        if (strcmp(home->tiles[i].key, key) == 0) {
            return i;
        }
    }
    if (home->tile_count >= HOME_LAYOUT_MAX_TILES) {
        return -1;
    }
    int idx = home->tile_count++;
    memcpy(home->tiles[idx].key, key, strlen(key) + 1);
    memset(&home->tiles[idx].override, 0, sizeof(home->tiles[idx].override));
    return idx;
}

static const struct {
    const char *key;
    size_t off;
    const char *bad;
    const char *missing;
} required_colors[] = {
    {"screen", offsetof(theme_file_t, screen), "invalid screen color", "missing required field: screen"},
    {"card", offsetof(theme_file_t, card), "invalid card color", "missing required field: card"},
    {"list_row", offsetof(theme_file_t, list_row), "invalid list_row color", "missing required field: list_row"},
    {"text_primary", offsetof(theme_file_t, text_primary), "invalid text_primary color", "missing required field: text_primary"},
    {"text_muted", offsetof(theme_file_t, text_muted), "invalid text_muted color", "missing required field: text_muted"},
};

#define REQUIRED_COLOR_COUNT (sizeof(required_colors) / sizeof(required_colors[0]))

static int required_color_index(const char *key) {
    for (size_t i = 0; i < REQUIRED_COLOR_COUNT; ++i) {
        if (strcmp(key, required_colors[i].key) == 0) return (int) i;
    }
    return -1;
}

typedef struct {
    bool has_name;
    uint8_t seen;
    bool has_home_mode;
    bool has_home_tile_gap;
    bool has_home_row_gap;
    bool has_home_order;
    bool has_submenu_mode;
} theme_parse_flags_t;

static bool is_known_tile_prop(const char *prop) {
    static const char *const properties[] = {
        "bg_color", "text_color", "radius", "height", "width",
        "align", "text_size", "accessory", "icon"
    };
    for (size_t i = 0; i < sizeof(properties) / sizeof(properties[0]); ++i) {
        if (strcmp(prop, properties[i]) == 0) return true;
    }
    return false;
}

static const char tile_line_skip[] = "";

static const char *parse_tile_line(home_layout_config_t *home, const char *key, const char *val_str) {
    const char *rest = key + 5;
    const char *dot = strrchr(rest, '.');
    if (!dot || dot == rest) {
        /* Malformed tile property key skipped for forward compatibility */
        return tile_line_skip;
    }
    size_t tile_klen = dot - rest;
    const char *prop = dot + 1;
    if (!is_known_tile_prop(prop)) return tile_line_skip;
    char tile_key[64];
    if (tile_klen >= sizeof(home->tiles[0].key)) {
        return "tile key too long";
    }
    memcpy(tile_key, rest, tile_klen);
    tile_key[tile_klen] = '\0';

    /* Validate tile ID per plugin registration contract:
     * must fit in sizeof(home->tiles[0].key) (40 bytes, so <= 39 chars) */
    if (!theme_file_is_valid_tile_id(tile_key, sizeof(home->tiles[0].key))) {
        return "invalid tile key";
    }

    char val[THEME_MAX_PATH_LEN];
    if (!theme_file_copy_value(val_str, val, sizeof(val))) {
        return "value too long";
    }

    int tidx = find_or_add_tile(home, tile_key);
    if (tidx < 0) {
        return "too many tiles configured";
    }
    home_tile_override_t *ov = &home->tiles[tidx].override;

    if (strcmp(prop, "bg_color") == 0) {
        uint32_t c;
        if (!theme_file_parse_uint32_color(val, &c)) {
            return "malformed tile bg_color";
        }
        ov->has_bg_color = true;
        ov->bg_color = c;
    } else if (strcmp(prop, "text_color") == 0) {
        uint32_t c;
        if (!theme_file_parse_uint32_color(val, &c)) {
            return "malformed tile text_color";
        }
        ov->has_text_color = true;
        ov->text_color = c;
    } else if (strcmp(prop, "radius") == 0) {
        int32_t r;
        if (!theme_file_parse_int32(val, &r, false)) {
            return "malformed tile radius";
        }
        ov->has_radius = true;
        ov->radius = r;
    } else if (strcmp(prop, "height") == 0) {
        int32_t h;
        if (!theme_file_parse_int32(val, &h, false)) {
            return "malformed tile height";
        }
        ov->height = h;
    } else if (strcmp(prop, "width") == 0) {
        int32_t w;
        if (!theme_file_parse_int32(val, &w, false)) {
            return "malformed tile width";
        }
        ov->width = w;
    } else if (strcmp(prop, "align") == 0) {
        if (!theme_file_is_valid_align(val)) {
            return "invalid tile align (must be left, center, or right)";
        }
        memcpy(ov->align, val, strlen(val) + 1);
    } else if (strcmp(prop, "text_size") == 0) {
        if (!theme_file_is_valid_text_size(val)) {
            return "invalid tile text_size (must be small, medium, large, or mono)";
        }
        memcpy(ov->text_size, val, strlen(val) + 1);
    } else if (strcmp(prop, "accessory") == 0) {
        bool b;
        if (!theme_file_parse_bool(val, &b)) {
            return "malformed tile accessory boolean";
        }
        ov->has_accessory = true;
        ov->accessory = b;
    } else if (strcmp(prop, "icon") == 0) {
        bool b;
        if (!theme_file_parse_bool(val, &b)) {
            return "malformed tile icon boolean";
        }
        ov->has_icon = true;
        ov->icon = b;
    }
    return NULL;
}

static const char *parse_icon_line(theme_file_t *c, const char *key, const char *val) {
    const char *relpath = key + 5;
    if (!theme_file_is_safe_relpath(relpath)) {
        return "unsafe or invalid icon relative path";
    }

    char value[THEME_MAX_PATH_LEN];
    if (!theme_file_copy_value(val, value, sizeof(value))) {
        return "value too long";
    }

    if (!theme_file_is_safe_source_path(value)) {
        return "unsafe or invalid icon source path";
    }

    int existing_idx = -1;
    for (size_t i = 0; i < c->icon_override_count; i++) {
        if (strcmp(c->icon_overrides[i].relpath, relpath) == 0) {
            existing_idx = (int)i;
            break;
        }
    }

    if (existing_idx >= 0) {
        snprintf(c->icon_overrides[existing_idx].source,
                 sizeof(c->icon_overrides[existing_idx].source), "%s", value);
    } else {
        if (c->icon_override_count >= THEME_MAX_ICON_OVERRIDES) {
            return "too many icon overrides";
        }
        theme_icon_override_t *io = &c->icon_overrides[c->icon_override_count++];
        snprintf(io->relpath, sizeof(io->relpath), "%s", relpath);
        snprintf(io->source, sizeof(io->source), "%s", value);
    }
    return NULL;
}

static const char *parse_home_order(home_layout_config_t *home, const char *val_str) {
    char order_copy[THEME_MAX_ORDER_LEN];
    size_t order_len = strlen(val_str);
    if (order_len >= sizeof(order_copy)) {
        return "home_order line too long";
    }
    memcpy(order_copy, val_str, order_len + 1);
    theme_file_trim_trailing(order_copy);

    if (order_copy[0] == '\0') {
        /* Empty definition clears previous home_order */
        home->order_count = 0;
        memset(home->order, 0, sizeof(home->order));
    } else {
        char *saveptr = NULL;
        char *tok = strtok_r(order_copy, ",", &saveptr);
        int count = 0;
        while (tok) {
            while (*tok == ' ' || *tok == '\t') tok++;
            theme_file_trim_trailing(tok);
            if (!theme_file_is_valid_tile_id(tok, sizeof(home->order[0]))) {
                return "invalid key in home_order";
            }
            if (count >= HOME_LAYOUT_MAX_TILES) {
                return "home_order exceeds max tiles";
            }
            /* Duplicate check */
            for (int i = 0; i < count; i++) {
                if (strcmp(home->order[i], tok) == 0) {
                    return "duplicate key in home_order";
                }
            }
            memcpy(home->order[count], tok, strlen(tok) + 1);
            count++;
            tok = strtok_r(NULL, ",", &saveptr);
        }
        home->order_count = count;
    }
    return NULL;
}

static const char *parse_standard_line(theme_file_t *c, const char *key, const char *val,
                                      theme_parse_flags_t *flags) {
    int color_idx = required_color_index(key);
    if (color_idx >= 0) {
        uint32_t *color = (uint32_t *) ((char *) c + required_colors[color_idx].off);
        if (!theme_file_parse_uint32_color(val, color)) {
            return required_colors[color_idx].bad;
        }
        flags->seen |= (uint8_t) (1u << color_idx);
    } else if (strcmp(key, "name") == 0) {
        if (!val[0] || strlen(val) >= sizeof(c->name)) {
            return "invalid or empty name";
        }
        memcpy(c->name, val, strlen(val) + 1);
        flags->has_name = true;
    } else if (strcmp(key, "icon_root") == 0) {
        if (val[0]) {
            if (!theme_file_is_safe_source_path(val)) {
                return "unsafe icon_root path";
            }
            snprintf(c->icon_root, sizeof(c->icon_root), "%s", val);
        } else {
            /* Empty definition clears previous icon_root */
            c->icon_root[0] = '\0';
        }
    } else if (strcmp(key, "drawer_root") == 0) {
        if (val[0]) {
            if (!theme_file_is_safe_source_path(val)) {
                return "unsafe drawer_root path";
            }
            snprintf(c->drawer_root, sizeof(c->drawer_root), "%s", val);
        } else {
            /* Empty definition clears previous drawer_root */
            c->drawer_root[0] = '\0';
        }
    } else if (strcmp(key, "home_mode") == 0) {
        if (strcmp(val, "tile") == 0) {
            c->home.list_mode = false;
            flags->has_home_mode = true;
        } else if (strcmp(val, "list") == 0) {
            c->home.list_mode = true;
            flags->has_home_mode = true;
        } else {
            return "invalid home_mode (must be tile or list)";
        }
    } else if (strcmp(key, "home_tile_gap") == 0) {
        int32_t gap;
        if (!theme_file_parse_int32(val, &gap, false)) {
            return "invalid home_tile_gap";
        }
        c->home.tile_gap = gap;
        flags->has_home_tile_gap = true;
    } else if (strcmp(key, "home_row_gap") == 0) {
        int32_t gap;
        if (!theme_file_parse_int32(val, &gap, false)) {
            return "invalid home_row_gap";
        }
        c->home.row_gap = gap;
        flags->has_home_row_gap = true;
    } else if (strcmp(key, "submenu_mode") == 0) {
        if (strcmp(val, "tile") != 0 && strcmp(val, "list") != 0) {
            return "invalid submenu_mode";
        }
        flags->has_submenu_mode = true;
    } else {
        /* Unknown top-level identifier - forward compatibility */
    }
    return NULL;
}

static const char *finalize_theme(theme_file_t *c, const theme_parse_flags_t *flags) {
    if (!flags->has_name) {
        return "missing required field: name";
    }
    for (size_t i = 0; i < REQUIRED_COLOR_COUNT; ++i) {
        if (!(flags->seen & (1u << i))) {
            return required_colors[i].missing;
        }
    }

    /* Reject tile-mode home_order > 6 (only if home_mode is specified as tile or omitted, meaning tile mode) */
    if (flags->has_home_order && (!flags->has_home_mode || !c->home.list_mode) && c->home.order_count > 6) {
        return "tile mode supports at most 6 tiles in home_order";
    }

    c->has_home_mode = flags->has_home_mode;
    c->has_home_tile_gap = flags->has_home_tile_gap;
    c->has_home_row_gap = flags->has_home_row_gap;
    c->has_home_order = flags->has_home_order;
    c->has_submenu_mode = flags->has_submenu_mode;

    if (flags->has_home_mode || flags->has_home_tile_gap || flags->has_home_row_gap || flags->has_home_order || c->home.tile_count > 0) {
        c->has_home = true;
        c->home.configured = true;
    }

    return NULL;
}

bool theme_file_parse(const char *path, theme_file_t *out, char *error, size_t error_size) {
    if (!path || !out) {
        set_err(error, error_size, "invalid arguments");
        return false;
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        set_err(error, error_size, "cannot open theme file");
        return false;
    }

    theme_file_t candidate;
    memset(&candidate, 0, sizeof(candidate));

    theme_parse_flags_t flags = {0};

    char line_buf[THEME_MAX_LINE_LEN];

    while (fgets(line_buf, sizeof(line_buf), f) != NULL) {
        size_t len = strlen(line_buf);

        if (len > 0 && line_buf[len - 1] != '\n' && !feof(f)) {
            return close_with_error(f, error, error_size, "line exceeds buffer length");
        }

        theme_file_trim_trailing(line_buf);
        const char *line = theme_file_skip_leading(line_buf);

        if (*line == '#' || *line == '\0') {
            continue;
        }

        const char *eq = strchr(line, '=');
        if (!eq) {
            /* Lines without '=' (e.g. comments like '-- comment', '; note', '[meta]')
             * are skipped for backwards compatibility with Themes.lua loader */
            continue;
        }

        size_t key_len = eq - line;
        char key[128];
        if (key_len >= sizeof(key)) {
            return close_with_error(f, error, error_size, "key name too long");
        }
        memcpy(key, line, key_len);
        key[key_len] = '\0';
        theme_file_trim_trailing(key);

        const char *val_str = eq + 1;
        while (*val_str == ' ' || *val_str == '\t') val_str++;

        if (strncmp(key, "tile.", 5) == 0) {
            const char *err = parse_tile_line(&candidate.home, key, val_str);
            if (err == tile_line_skip) continue;
            if (err) return close_with_error(f, error, error_size, err);
        } else if (strncmp(key, "icon.", 5) == 0) {
            const char *err = parse_icon_line(&candidate, key, val_str);
            if (err) return close_with_error(f, error, error_size, err);
        } else {
            /* Standard identifier keys */
            if (!theme_file_is_valid_ident(key)) {
                /* Unknown or non-identifier top-level keys (e.g. x-author, [section])
                 * are skipped for forward compatibility and Themes.lua parity */
                continue;
            }

            if (strcmp(key, "home_order") == 0) {
                const char *err = parse_home_order(&candidate.home, val_str);
                if (err) return close_with_error(f, error, error_size, err);
                flags.has_home_order = true;
            } else {
                char val[THEME_MAX_PATH_LEN];
                if (!theme_file_copy_value(val_str, val, sizeof(val))) {
                    return close_with_error(f, error, error_size, "value too long");
                }

                const char *err = parse_standard_line(&candidate, key, val, &flags);
                if (err) return close_with_error(f, error, error_size, err);
            }
        }
    }

    if (ferror(f)) {
        return close_with_error(f, error, error_size, "read error while parsing theme file");
    }

    fclose(f);

    const char *err = finalize_theme(&candidate, &flags);
    if (err) {
        set_err(error, error_size, err);
        return false;
    }

    if (candidate.has_submenu_mode) {
        candidate.has_launcher = true;

        home_tile_override_t *music_src = NULL;
        for (int i = 0; i < candidate.home.tile_count; i++) {
            if (strcmp(candidate.home.tiles[i].key, "music") == 0) {
                music_src = &candidate.home.tiles[i].override;
                break;
            }
        }

        launcher_menu_layout_t style;
        memset(&style, 0, sizeof(style));
        style.row_gap = candidate.home.row_gap;

        if (music_src) {
            style.has_bg_color = music_src->has_bg_color;
            style.bg_color = music_src->bg_color;
            style.has_text_color = music_src->has_text_color;
            style.text_color = music_src->text_color;
            style.has_radius = music_src->has_radius;
            style.radius = music_src->radius;
            style.height = music_src->height;
            style.width = music_src->width;
            snprintf(style.align, sizeof(style.align), "%s", music_src->align);
            style.has_accessory = music_src->has_accessory;
            style.accessory = music_src->accessory;
            snprintf(style.text_size, sizeof(style.text_size), "%s", music_src->text_size);
            style.has_icon = music_src->has_icon;
            style.icon = music_src->icon;
        }

        candidate.launcher.music = style;
        candidate.launcher.stream_media = style;
        candidate.launcher.wireless = style;
    }

    /* Success: copy full candidate to out */
    *out = candidate;
    return true;
}
