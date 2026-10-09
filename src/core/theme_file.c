#include "theme_file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#define THEME_MAX_LINE_LEN 512
#define THEME_MAX_ORDER_LEN 512

static void set_err(char *error, size_t error_size, const char *msg) {
    if (error && error_size > 0) {
        snprintf(error, error_size, "%s", msg);
    }
}

static void trim_trailing(char *s) {
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r' || s[len - 1] == '\n')) {
        s[--len] = '\0';
    }
}

static const char *skip_leading(const char *s) {
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    return s;
}

static bool parse_uint32_color(const char *str, uint32_t *out_val) {
    str = skip_leading(str);
    if (!*str) return false;

    char *endptr = NULL;
    errno = 0;
    unsigned long val = 0;
    if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
        val = strtoul(str, &endptr, 16);
    } else {
        val = strtoul(str, &endptr, 10);
    }

    if (errno != 0 || endptr == str) return false;
    while (*endptr == ' ' || *endptr == '\t' || *endptr == '\r' || *endptr == '\n') {
        endptr++;
    }
    if (*endptr != '\0') return false;
    if (val > 0xFFFFFF) return false;

    *out_val = (uint32_t)val;
    return true;
}

static bool parse_int32(const char *str, int32_t *out_val, bool allow_negative) {
    str = skip_leading(str);
    if (!*str) return false;

    char *endptr = NULL;
    errno = 0;
    long val = 0;
    if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
        /* Hexadecimal integer (unsigned or positive) */
        unsigned long uval = strtoul(str, &endptr, 16);
        if (errno != 0 || endptr == str) return false;
        if (uval > (unsigned long)INT32_MAX) return false;
        val = (long)uval;
    } else {
        val = strtol(str, &endptr, 10);
        if (errno != 0 || endptr == str) return false;
    }

    while (*endptr == ' ' || *endptr == '\t' || *endptr == '\r' || *endptr == '\n') {
        endptr++;
    }
    if (*endptr != '\0') return false;
    if (!allow_negative && val < 0) return false;
    if (val < INT32_MIN || val > INT32_MAX) return false;

    *out_val = (int32_t)val;
    return true;
}

static bool parse_bool(const char *str, bool *out_val) {
    str = skip_leading(str);
    if (strcmp(str, "true") == 0) {
        *out_val = true;
        return true;
    }
    if (strcmp(str, "false") == 0) {
        *out_val = false;
        return true;
    }
    return false;
}

static bool is_safe_relpath(const char *path) {
    if (!path || !path[0]) return false;
    if (path[0] == '/') return false;
    if (strstr(path, "..") != NULL) return false;
    if (strstr(path, "\\") != NULL) return false;

    for (const char *p = path; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 32 || c == 127) return false;
    }
    return true;
}

static bool is_safe_source_path(const char *path) {
    if (!path || !path[0]) return false;
    if (strstr(path, "..") != NULL) return false;
    if (strstr(path, "\\") != NULL) return false;

    for (const char *p = path; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 32 || c == 127) return false;
    }
    return true;
}

static bool is_valid_ident(const char *str) {
    if (!str || !*str) return false;
    if (!isalpha((unsigned char)*str)) return false;
    for (const char *p = str + 1; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '_') return false;
    }
    return true;
}

/* Plugin registration contract ID charset: [A-Za-z0-9._-]
 * Supports digits-leading, dots, hyphens, and underscores.
 * Must be non-empty and strictly < max_len (to fit destination without truncation). */
static bool is_valid_tile_id(const char *str, size_t max_len) {
    if (!str || !str[0]) return false;
    size_t len = 0;
    for (const char *p = str; *p; p++, len++) {
        unsigned char c = (unsigned char)*p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) {
            return false;
        }
    }
    if (len >= max_len) {
        return false;
    }
    return true;
}

static bool is_valid_align(const char *val) {
    return (strcmp(val, "left") == 0 ||
            strcmp(val, "center") == 0 ||
            strcmp(val, "right") == 0);
}

static bool is_valid_text_size(const char *val) {
    return (strcmp(val, "small") == 0 ||
            strcmp(val, "medium") == 0 ||
            strcmp(val, "large") == 0 ||
            strcmp(val, "mono") == 0);
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
            fclose(f);
            set_err(error, error_size, "line exceeds buffer length");
            return false;
        }

        trim_trailing(line_buf);
        const char *line = skip_leading(line_buf);

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
            fclose(f);
            set_err(error, error_size, "key name too long");
            return false;
        }
        memcpy(key, line, key_len);
        key[key_len] = '\0';
        trim_trailing(key);

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
                fclose(f);
                set_err(error, error_size, "tile key too long");
                return false;
            }
            memcpy(tile_key, rest, tile_klen);
            tile_key[tile_klen] = '\0';

            /* Validate tile ID per plugin registration contract:
             * must fit in sizeof(candidate.home.tiles[0].key) (40 bytes, so <= 39 chars) */
            if (!is_valid_tile_id(tile_key, sizeof(candidate.home.tiles[0].key))) {
                fclose(f);
                set_err(error, error_size, "invalid tile key");
                return false;
            }

            char val[THEME_MAX_PATH_LEN];
            size_t val_len = strlen(val_str);
            if (val_len >= sizeof(val)) {
                fclose(f);
                set_err(error, error_size, "value too long");
                return false;
            }
            memcpy(val, val_str, val_len + 1);
            trim_trailing(val);

            int tidx = find_or_add_tile(&candidate.home, tile_key);
            if (tidx < 0) {
                fclose(f);
                set_err(error, error_size, "too many tiles configured");
                return false;
            }
            home_tile_override_t *ov = &candidate.home.tiles[tidx].override;

            if (strcmp(prop, "bg_color") == 0) {
                uint32_t c;
                if (!parse_uint32_color(val, &c)) {
                    fclose(f);
                    set_err(error, error_size, "malformed tile bg_color");
                    return false;
                }
                ov->has_bg_color = true;
                ov->bg_color = c;
            } else if (strcmp(prop, "text_color") == 0) {
                uint32_t c;
                if (!parse_uint32_color(val, &c)) {
                    fclose(f);
                    set_err(error, error_size, "malformed tile text_color");
                    return false;
                }
                ov->has_text_color = true;
                ov->text_color = c;
            } else if (strcmp(prop, "radius") == 0) {
                int32_t r;
                if (!parse_int32(val, &r, false)) {
                    fclose(f);
                    set_err(error, error_size, "malformed tile radius");
                    return false;
                }
                ov->has_radius = true;
                ov->radius = r;
            } else if (strcmp(prop, "height") == 0) {
                int32_t h;
                if (!parse_int32(val, &h, false)) {
                    fclose(f);
                    set_err(error, error_size, "malformed tile height");
                    return false;
                }
                ov->height = h;
            } else if (strcmp(prop, "width") == 0) {
                int32_t w;
                if (!parse_int32(val, &w, false)) {
                    fclose(f);
                    set_err(error, error_size, "malformed tile width");
                    return false;
                }
                ov->width = w;
            } else if (strcmp(prop, "align") == 0) {
                if (!is_valid_align(val)) {
                    fclose(f);
                    set_err(error, error_size, "invalid tile align (must be left, center, or right)");
                    return false;
                }
                memcpy(ov->align, val, strlen(val) + 1);
            } else if (strcmp(prop, "text_size") == 0) {
                if (!is_valid_text_size(val)) {
                    fclose(f);
                    set_err(error, error_size, "invalid tile text_size (must be small, medium, large, or mono)");
                    return false;
                }
                memcpy(ov->text_size, val, strlen(val) + 1);
            } else if (strcmp(prop, "accessory") == 0) {
                bool b;
                if (!parse_bool(val, &b)) {
                    fclose(f);
                    set_err(error, error_size, "malformed tile accessory boolean");
                    return false;
                }
                ov->has_accessory = true;
                ov->accessory = b;
            } else if (strcmp(prop, "icon") == 0) {
                bool b;
                if (!parse_bool(val, &b)) {
                    fclose(f);
                    set_err(error, error_size, "malformed tile icon boolean");
                    return false;
                }
                ov->has_icon = true;
                ov->icon = b;
            } else {
                /* Unknown tile property - tolerated for forward compatibility */
            }
        } else if (strncmp(key, "icon.", 5) == 0) {
            const char *relpath = key + 5;
            if (!is_safe_relpath(relpath)) {
                fclose(f);
                set_err(error, error_size, "unsafe or invalid icon relative path");
                return false;
            }

            char val[THEME_MAX_PATH_LEN];
            size_t val_len = strlen(val_str);
            if (val_len >= sizeof(val)) {
                fclose(f);
                set_err(error, error_size, "value too long");
                return false;
            }
            memcpy(val, val_str, val_len + 1);
            trim_trailing(val);

            if (!is_safe_source_path(val)) {
                fclose(f);
                set_err(error, error_size, "unsafe or invalid icon source path");
                return false;
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
                    fclose(f);
                    set_err(error, error_size, "too many icon overrides");
                    return false;
                }
                theme_icon_override_t *io = &candidate.icon_overrides[candidate.icon_override_count++];
                snprintf(io->relpath, sizeof(io->relpath), "%s", relpath);
                snprintf(io->source, sizeof(io->source), "%s", val);
            }
        } else {
            /* Standard identifier keys */
            if (!is_valid_ident(key)) {
                /* Unknown or non-identifier top-level keys (e.g. x-author, [section])
                 * are skipped for forward compatibility and Themes.lua parity */
                continue;
            }

            if (strcmp(key, "home_order") == 0) {
                char order_copy[THEME_MAX_ORDER_LEN];
                size_t order_len = strlen(val_str);
                if (order_len >= sizeof(order_copy)) {
                    fclose(f);
                    set_err(error, error_size, "home_order line too long");
                    return false;
                }
                memcpy(order_copy, val_str, order_len + 1);
                trim_trailing(order_copy);

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
                        trim_trailing(tok);
                        if (!is_valid_tile_id(tok, sizeof(candidate.home.order[0]))) {
                            fclose(f);
                            set_err(error, error_size, "invalid key in home_order");
                            return false;
                        }
                        if (count >= HOME_LAYOUT_MAX_TILES) {
                            fclose(f);
                            set_err(error, error_size, "home_order exceeds max tiles");
                            return false;
                        }
                        /* Duplicate check */
                        for (int i = 0; i < count; i++) {
                            if (strcmp(candidate.home.order[i], tok) == 0) {
                                fclose(f);
                                set_err(error, error_size, "duplicate key in home_order");
                                return false;
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
                size_t val_len = strlen(val_str);
                if (val_len >= sizeof(val)) {
                    fclose(f);
                    set_err(error, error_size, "value too long");
                    return false;
                }
                memcpy(val, val_str, val_len + 1);
                trim_trailing(val);

                if (strcmp(key, "name") == 0) {
                    if (!val[0] || strlen(val) >= sizeof(candidate.name)) {
                        fclose(f);
                        set_err(error, error_size, "invalid or empty name");
                        return false;
                    }
                    memcpy(candidate.name, val, strlen(val) + 1);
                    has_name = true;
                } else if (strcmp(key, "screen") == 0) {
                    if (!parse_uint32_color(val, &candidate.screen)) {
                        fclose(f);
                        set_err(error, error_size, "invalid screen color");
                        return false;
                    }
                    has_screen = true;
                } else if (strcmp(key, "card") == 0) {
                    if (!parse_uint32_color(val, &candidate.card)) {
                        fclose(f);
                        set_err(error, error_size, "invalid card color");
                        return false;
                    }
                    has_card = true;
                } else if (strcmp(key, "list_row") == 0) {
                    if (!parse_uint32_color(val, &candidate.list_row)) {
                        fclose(f);
                        set_err(error, error_size, "invalid list_row color");
                        return false;
                    }
                    has_list_row = true;
                } else if (strcmp(key, "text_primary") == 0) {
                    if (!parse_uint32_color(val, &candidate.text_primary)) {
                        fclose(f);
                        set_err(error, error_size, "invalid text_primary color");
                        return false;
                    }
                    has_text_primary = true;
                } else if (strcmp(key, "text_muted") == 0) {
                    if (!parse_uint32_color(val, &candidate.text_muted)) {
                        fclose(f);
                        set_err(error, error_size, "invalid text_muted color");
                        return false;
                    }
                    has_text_muted = true;
                } else if (strcmp(key, "icon_root") == 0) {
                    if (val[0]) {
                        if (!is_safe_source_path(val)) {
                            fclose(f);
                            set_err(error, error_size, "unsafe icon_root path");
                            return false;
                        }
                        snprintf(candidate.icon_root, sizeof(candidate.icon_root), "%s", val);
                    } else {
                        /* Empty definition clears previous icon_root */
                        candidate.icon_root[0] = '\0';
                    }
                } else if (strcmp(key, "drawer_root") == 0) {
                    if (val[0]) {
                        if (!is_safe_source_path(val)) {
                            fclose(f);
                            set_err(error, error_size, "unsafe drawer_root path");
                            return false;
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
                        fclose(f);
                        set_err(error, error_size, "invalid home_mode (must be tile or list)");
                        return false;
                    }
                } else if (strcmp(key, "home_tile_gap") == 0) {
                    int32_t gap;
                    if (!parse_int32(val, &gap, false)) {
                        fclose(f);
                        set_err(error, error_size, "invalid home_tile_gap");
                        return false;
                    }
                    candidate.home.tile_gap = gap;
                    has_home_tile_gap = true;
                } else if (strcmp(key, "home_row_gap") == 0) {
                    int32_t gap;
                    if (!parse_int32(val, &gap, false)) {
                        fclose(f);
                        set_err(error, error_size, "invalid home_row_gap");
                        return false;
                    }
                    candidate.home.row_gap = gap;
                    has_home_row_gap = true;
                } else if (strcmp(key, "submenu_mode") == 0) {
                    if (strcmp(val, "tile") != 0 && strcmp(val, "list") != 0) {
                        fclose(f);
                        set_err(error, error_size, "invalid submenu_mode");
                        return false;
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
        fclose(f);
        set_err(error, error_size, "read error while parsing theme file");
        return false;
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
