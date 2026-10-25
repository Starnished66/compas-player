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

    bool has_name = false;
    bool has_screen = false;
    bool has_card = false;
    bool has_list_row = false;
    bool has_text_primary = false;
    bool has_text_muted = false;

    bool has_home_mode = false;
    bool has_home_tile_gap = false;
    bool has_home_row_gap = false;
    bool has_home_order = false;
    bool has_submenu_mode = false;
    char submenu_mode_str[16] = {0};

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
            const char *rest = key + 5;
            const char *dot = strrchr(rest, '.');
            if (!dot || dot == rest) {
                /* Malformed tile property key skipped for forward compatibility */
                continue;
            }
            size_t tile_klen = dot - rest;
            const char *prop = dot + 1;
            static const char * const properties[] = {
                "bg_color", "text_color", "radius", "height", "width",
                "align", "text_size", "accessory", "icon"
            };
            bool known_property = false;
            for (size_t i = 0; i < sizeof(properties) / sizeof(properties[0]); ++i) {
                if (strcmp(prop, properties[i]) == 0) {
                    known_property = true;
                    break;
                }
            }
            if (!known_property) continue;
            char tile_key[64];
            if (tile_klen >= sizeof(candidate.home.tiles[0].key)) {
                return close_with_error(f, error, error_size, "tile key too long");
            }
            memcpy(tile_key, rest, tile_klen);
            tile_key[tile_klen] = '\0';

            /* Validate tile ID per plugin registration contract:
             * must fit in sizeof(candidate.home.tiles[0].key) (40 bytes, so <= 39 chars) */
            if (!theme_file_is_valid_tile_id(tile_key, sizeof(candidate.home.tiles[0].key))) {
                return close_with_error(f, error, error_size, "invalid tile key");
            }

            char val[THEME_MAX_PATH_LEN];
            if (!theme_file_copy_value(val_str, val, sizeof(val))) {
                return close_with_error(f, error, error_size, "value too long");
            }

            int tidx = find_or_add_tile(&candidate.home, tile_key);
            if (tidx < 0) {
                return close_with_error(f, error, error_size, "too many tiles configured");
            }
            home_tile_override_t *ov = &candidate.home.tiles[tidx].override;

            if (strcmp(prop, "bg_color") == 0) {
                uint32_t c;
                if (!theme_file_parse_uint32_color(val, &c)) {
                    return close_with_error(f, error, error_size, "malformed tile bg_color");
                }
                ov->has_bg_color = true;
                ov->bg_color = c;
            } else if (strcmp(prop, "text_color") == 0) {
                uint32_t c;
                if (!theme_file_parse_uint32_color(val, &c)) {
                    return close_with_error(f, error, error_size, "malformed tile text_color");
                }
                ov->has_text_color = true;
                ov->text_color = c;
            } else if (strcmp(prop, "radius") == 0) {
                int32_t r;
                if (!theme_file_parse_int32(val, &r, false)) {
                    return close_with_error(f, error, error_size, "malformed tile radius");
                }
                ov->has_radius = true;
                ov->radius = r;
            } else if (strcmp(prop, "height") == 0) {
                int32_t h;
                if (!theme_file_parse_int32(val, &h, false)) {
                    return close_with_error(f, error, error_size, "malformed tile height");
                }
                ov->height = h;
            } else if (strcmp(prop, "width") == 0) {
                int32_t w;
                if (!theme_file_parse_int32(val, &w, false)) {
                    return close_with_error(f, error, error_size, "malformed tile width");
                }
                ov->width = w;
            } else if (strcmp(prop, "align") == 0) {
                if (!theme_file_is_valid_align(val)) {
                    return close_with_error(f, error, error_size, "invalid tile align (must be left, center, or right)");
                }
                memcpy(ov->align, val, strlen(val) + 1);
            } else if (strcmp(prop, "text_size") == 0) {
                if (!theme_file_is_valid_text_size(val)) {
                    return close_with_error(f, error, error_size, "invalid tile text_size (must be small, medium, large, or mono)");
                }
                memcpy(ov->text_size, val, strlen(val) + 1);
            } else if (strcmp(prop, "accessory") == 0) {
                bool b;
                if (!theme_file_parse_bool(val, &b)) {
                    return close_with_error(f, error, error_size, "malformed tile accessory boolean");
                }
                ov->has_accessory = true;
                ov->accessory = b;
            } else if (strcmp(prop, "icon") == 0) {
                bool b;
                if (!theme_file_parse_bool(val, &b)) {
                    return close_with_error(f, error, error_size, "malformed tile icon boolean");
                }
                ov->has_icon = true;
                ov->icon = b;
            } else {
                /* Unknown tile property - tolerated for forward compatibility */
            }
        } else if (strncmp(key, "icon.", 5) == 0) {
            const char *relpath = key + 5;
            if (!theme_file_is_safe_relpath(relpath)) {
                return close_with_error(f, error, error_size, "unsafe or invalid icon relative path");
            }

            char val[THEME_MAX_PATH_LEN];
            if (!theme_file_copy_value(val_str, val, sizeof(val))) {
                return close_with_error(f, error, error_size, "value too long");
            }

            if (!theme_file_is_safe_source_path(val)) {
                return close_with_error(f, error, error_size, "unsafe or invalid icon source path");
            }

            int existing_idx = -1;
            for (size_t i = 0; i < candidate.icon_override_count; i++) {
                if (strcmp(candidate.icon_overrides[i].relpath, relpath) == 0) {
                    existing_idx = (int)i;
                    break;
                }
            }

            if (existing_idx >= 0) {
                snprintf(candidate.icon_overrides[existing_idx].source,
                         sizeof(candidate.icon_overrides[existing_idx].source), "%s", val);
            } else {
                if (candidate.icon_override_count >= THEME_MAX_ICON_OVERRIDES) {
                    return close_with_error(f, error, error_size, "too many icon overrides");
                }
                theme_icon_override_t *io = &candidate.icon_overrides[candidate.icon_override_count++];
                snprintf(io->relpath, sizeof(io->relpath), "%s", relpath);
                snprintf(io->source, sizeof(io->source), "%s", val);
            }
        } else {
            /* Standard identifier keys */
            if (!theme_file_is_valid_ident(key)) {
                /* Unknown or non-identifier top-level keys (e.g. x-author, [section])
                 * are skipped for forward compatibility and Themes.lua parity */
                continue;
            }

            if (strcmp(key, "home_order") == 0) {
                char order_copy[THEME_MAX_ORDER_LEN];
                size_t order_len = strlen(val_str);
                if (order_len >= sizeof(order_copy)) {
                    return close_with_error(f, error, error_size, "home_order line too long");
                }
                memcpy(order_copy, val_str, order_len + 1);
                theme_file_trim_trailing(order_copy);

                if (order_copy[0] == '\0') {
                    /* Empty definition clears previous home_order */
                    candidate.home.order_count = 0;
                    memset(candidate.home.order, 0, sizeof(candidate.home.order));
                    has_home_order = true;
                } else {
                    char *saveptr = NULL;
                    char *tok = strtok_r(order_copy, ",", &saveptr);
                    int count = 0;
                    while (tok) {
                        while (*tok == ' ' || *tok == '\t') tok++;
                        theme_file_trim_trailing(tok);
                        if (!theme_file_is_valid_tile_id(tok, sizeof(candidate.home.order[0]))) {
                            return close_with_error(f, error, error_size, "invalid key in home_order");
                        }
                        if (count >= HOME_LAYOUT_MAX_TILES) {
                            return close_with_error(f, error, error_size, "home_order exceeds max tiles");
                        }
                        /* Duplicate check */
                        for (int i = 0; i < count; i++) {
                            if (strcmp(candidate.home.order[i], tok) == 0) {
                                return close_with_error(f, error, error_size, "duplicate key in home_order");
                            }
                        }
                        memcpy(candidate.home.order[count], tok, strlen(tok) + 1);
                        count++;
                        tok = strtok_r(NULL, ",", &saveptr);
                    }
                    candidate.home.order_count = count;
                    has_home_order = true;
                }
            } else {
                char val[THEME_MAX_PATH_LEN];
                if (!theme_file_copy_value(val_str, val, sizeof(val))) {
                    return close_with_error(f, error, error_size, "value too long");
                }

                if (strcmp(key, "name") == 0) {
                    if (!val[0] || strlen(val) >= sizeof(candidate.name)) {
                        return close_with_error(f, error, error_size, "invalid or empty name");
                    }
                    memcpy(candidate.name, val, strlen(val) + 1);
                    has_name = true;
                } else if (strcmp(key, "screen") == 0) {
                    if (!theme_file_parse_uint32_color(val, &candidate.screen)) {
                        return close_with_error(f, error, error_size, "invalid screen color");
                    }
                    has_screen = true;
                } else if (strcmp(key, "card") == 0) {
                    if (!theme_file_parse_uint32_color(val, &candidate.card)) {
                        return close_with_error(f, error, error_size, "invalid card color");
                    }
                    has_card = true;
                } else if (strcmp(key, "list_row") == 0) {
                    if (!theme_file_parse_uint32_color(val, &candidate.list_row)) {
                        return close_with_error(f, error, error_size, "invalid list_row color");
                    }
                    has_list_row = true;
                } else if (strcmp(key, "text_primary") == 0) {
                    if (!theme_file_parse_uint32_color(val, &candidate.text_primary)) {
                        return close_with_error(f, error, error_size, "invalid text_primary color");
                    }
                    has_text_primary = true;
                } else if (strcmp(key, "text_muted") == 0) {
                    if (!theme_file_parse_uint32_color(val, &candidate.text_muted)) {
                        return close_with_error(f, error, error_size, "invalid text_muted color");
                    }
                    has_text_muted = true;
                } else if (strcmp(key, "icon_root") == 0) {
                    if (val[0]) {
                        if (!theme_file_is_safe_source_path(val)) {
                            return close_with_error(f, error, error_size, "unsafe icon_root path");
                        }
                        snprintf(candidate.icon_root, sizeof(candidate.icon_root), "%s", val);
                    } else {
                        /* Empty definition clears previous icon_root */
                        candidate.icon_root[0] = '\0';
                    }
                } else if (strcmp(key, "drawer_root") == 0) {
                    if (val[0]) {
                        if (!theme_file_is_safe_source_path(val)) {
                            return close_with_error(f, error, error_size, "unsafe drawer_root path");
                        }
                        snprintf(candidate.drawer_root, sizeof(candidate.drawer_root), "%s", val);
                    } else {
                        /* Empty definition clears previous drawer_root */
                        candidate.drawer_root[0] = '\0';
                    }
                } else if (strcmp(key, "home_mode") == 0) {
                    if (strcmp(val, "tile") == 0) {
                        candidate.home.list_mode = false;
                        has_home_mode = true;
                    } else if (strcmp(val, "list") == 0) {
                        candidate.home.list_mode = true;
                        has_home_mode = true;
                    } else {
                        return close_with_error(f, error, error_size, "invalid home_mode (must be tile or list)");
                    }
                } else if (strcmp(key, "home_tile_gap") == 0) {
                    int32_t gap;
                    if (!theme_file_parse_int32(val, &gap, false)) {
                        return close_with_error(f, error, error_size, "invalid home_tile_gap");
                    }
                    candidate.home.tile_gap = gap;
                    has_home_tile_gap = true;
                } else if (strcmp(key, "home_row_gap") == 0) {
                    int32_t gap;
                    if (!theme_file_parse_int32(val, &gap, false)) {
                        return close_with_error(f, error, error_size, "invalid home_row_gap");
                    }
                    candidate.home.row_gap = gap;
                    has_home_row_gap = true;
                } else if (strcmp(key, "submenu_mode") == 0) {
                    if (strcmp(val, "tile") != 0 && strcmp(val, "list") != 0) {
                        return close_with_error(f, error, error_size, "invalid submenu_mode");
                    }
                    memcpy(submenu_mode_str, val, strlen(val) + 1);
                    has_submenu_mode = true;
                } else {
                    /* Unknown top-level identifier - forward compatibility */
                }
            }
        }
    }

    if (ferror(f)) {
        return close_with_error(f, error, error_size, "read error while parsing theme file");
    }

    fclose(f);

    if (!has_name) {
        set_err(error, error_size, "missing required field: name");
        return false;
    }
    if (!has_screen) {
        set_err(error, error_size, "missing required field: screen");
        return false;
    }
    if (!has_card) {
        set_err(error, error_size, "missing required field: card");
        return false;
    }
    if (!has_list_row) {
        set_err(error, error_size, "missing required field: list_row");
        return false;
    }
    if (!has_text_primary) {
        set_err(error, error_size, "missing required field: text_primary");
        return false;
    }
    if (!has_text_muted) {
        set_err(error, error_size, "missing required field: text_muted");
        return false;
    }

    /* Reject tile-mode home_order > 6 (only if home_mode is specified as tile or omitted, meaning tile mode) */
    if (has_home_order && (!has_home_mode || !candidate.home.list_mode) && candidate.home.order_count > 6) {
        set_err(error, error_size, "tile mode supports at most 6 tiles in home_order");
        return false;
    }

    candidate.has_home_mode = has_home_mode;
    candidate.has_home_tile_gap = has_home_tile_gap;
    candidate.has_home_row_gap = has_home_row_gap;
    candidate.has_home_order = has_home_order;
    candidate.has_submenu_mode = has_submenu_mode;

    if (has_home_mode || has_home_tile_gap || has_home_row_gap || has_home_order || candidate.home.tile_count > 0) {
        candidate.has_home = true;
        candidate.home.configured = true;
    }

    if (has_submenu_mode) {
        candidate.has_launcher = true;
        bool list_mode = (strcmp(submenu_mode_str, "list") == 0);

        home_tile_override_t *music_src = NULL;
        for (int i = 0; i < candidate.home.tile_count; i++) {
            if (strcmp(candidate.home.tiles[i].key, "music") == 0) {
                music_src = &candidate.home.tiles[i].override;
                break;
            }
        }

        launcher_menu_layout_t style;
        memset(&style, 0, sizeof(style));
        style.list_mode = list_mode;
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
