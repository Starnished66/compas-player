/* plugin.* bindings for toast, progress, dialogs, icons, colours and the Home layout. */
#include "../../../ui/application/gui.h"
#include "../../lua/plugin_lua_runtime.h"
#include "../../../ui/appearance/gui_themes.h"
#include "../../../core/system/lifecycle/screenshot.h"
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <errno.h>
#include <math.h>
#include <unistd.h>
#include "../../manager/plugin_manager_internal.h"

static int l_plugin_show_toast(lua_State * L) {
    const char * msg = luaL_checkstring(L, 1);
    lua_Integer duration_ms = luaL_optinteger(L, 2, 5000);
    if (duration_ms < 100 || duration_ms > 30000)
        return luaL_error(L, "plugin.show_toast: duration_ms must be between 100 and 30000");
    gui_plugin_show_toast(msg, (uint32_t) duration_ms);
    return 0;
}

/* API 16: one modeless progress card. Handles are never recycled during
 * this process, so a dismissed/replaced card cannot be reopened by a stale
 * asynchronous completion. All access is on the Lua/UI thread. */
lua_State * plugin_progress_owner = NULL;
static lua_Integer plugin_progress_handle = 0;
static lua_Integer plugin_progress_next_handle = 1;

static int plugin_progress_percent(lua_State * L, int index) {
    if (lua_isnoneornil(L, index)) return -1;
    double fraction = luaL_checknumber(L, index);
    if (!isfinite(fraction) || fraction < 0.0 || fraction > 1.0)
        luaL_error(L, "plugin progress: fraction must be between 0 and 1, or nil");
    return (int) (fraction * 100.0 + 0.5);
}

static const char * plugin_progress_text(lua_State * L, int index, size_t max) {
    size_t length;
    const char * text = luaL_checklstring(L, index, &length);
    if (length > max || memchr(text, '\0', length))
        luaL_error(L, "plugin progress: text is too long or contains NUL");
    return text;
}

void plugin_progress_reset(void) {
    gui_plugin_close_progress();
    plugin_progress_owner = NULL;
    plugin_progress_handle = 0;
}

static int l_plugin_show_progress(lua_State * L) {
    const char * title = plugin_progress_text(L, 1, 128);
    const char * message = plugin_progress_text(L, 2, 512);
    int percent = plugin_progress_percent(L, 3);
    if (plugin_progress_next_handle == LUA_MAXINTEGER)
        return plugin_lua_push_error(L, "progress handle limit reached");
    if (!gui_plugin_show_progress(title, message, percent))
        return plugin_lua_push_error(L, "progress UI unavailable");
    plugin_progress_owner = plugin_lua_main_state(L);
    plugin_progress_handle = plugin_progress_next_handle++;
    lua_pushinteger(L, plugin_progress_handle);
    return 1;
}

static int l_plugin_update_progress(lua_State * L) {
    lua_Integer handle = luaL_checkinteger(L, 1);
    const char * message = plugin_progress_text(L, 2, 512);
    int percent = plugin_progress_percent(L, 3);
    bool ok = handle > 0 && handle == plugin_progress_handle
        && plugin_progress_owner == plugin_lua_main_state(L)
        && gui_plugin_update_progress(message, percent);
    lua_pushboolean(L, ok);
    return 1;
}

static int l_plugin_close_progress(lua_State * L) {
    lua_Integer handle = luaL_checkinteger(L, 1);
    bool owned = handle > 0 && handle == plugin_progress_handle
        && plugin_progress_owner == plugin_lua_main_state(L);
    bool visible = owned && gui_plugin_progress_is_visible();
    if (owned) plugin_progress_reset();
    lua_pushboolean(L, visible);
    return 1;
}

static int l_plugin_screenshot(lua_State * L) {
    screenshot_result_t result = screenshot_start();
    if (result == SCREENSHOT_RESULT_STARTED) {
        lua_pushboolean(L, true);
        return 1;
    }
    lua_pushboolean(L, false);
    lua_pushstring(L, screenshot_result_reason(result));
    return 2;
}

#ifndef HOST_BUILD
/* True when both paths exist as regular files of equal size and equal
 * bytes. Used by copy_file() to skip a NAND rewrite of an icon that is
 * already the requested image -- Themes.lua copies ~140 assets on every
 * load, and after the first successful apply those destinations already
 * match. */
static bool files_identical(const char * a_path, const char * b_path) {
    struct stat sa, sb;
    if (stat(a_path, &sa) != 0 || stat(b_path, &sb) != 0) return false;
    if (!S_ISREG(sa.st_mode) || !S_ISREG(sb.st_mode)) return false;
    if (sa.st_size != sb.st_size) return false;

    FILE * a = fopen(a_path, "rb");
    FILE * b = fopen(b_path, "rb");
    if (!a || !b) {
        if (a) fclose(a);
        if (b) fclose(b);
        return false;
    }

    char ba[4096], bb[4096];
    bool same = true;
    for (;;) {
        size_t na = fread(ba, 1, sizeof(ba), a);
        size_t nb = fread(bb, 1, sizeof(bb), b);
        if (na != nb || memcmp(ba, bb, na) != 0) {
            same = false;
            break;
        }
        if (na == 0) break;
    }
    if (ferror(a) || ferror(b)) same = false;
    fclose(a);
    fclose(b);
    return same;
}

/* Atomic byte copy: readers see either the old complete icon or the new
 * one, never a partially-written PNG. */
static bool copy_file(const char * src_path, const char * dst_path) {
    if (files_identical(src_path, dst_path)) return true;
    FILE * in = fopen(src_path, "rb");
    if (!in) return false;
    char tmp_path[640];
    if (snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%ld", dst_path, (long) getpid()) >= (int) sizeof(tmp_path)) {
        fclose(in);
        return false;
    }
    FILE * out = fopen(tmp_path, "wb");
    if (!out) {
        fclose(in);
        return false;
    }

    bool ok = true;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            ok = false;
            break;
        }
    }
    ok = ok && !ferror(in);
    fclose(in);
    if (fclose(out) != 0) ok = false;

    if (ok) ok = rename(tmp_path, dst_path) == 0;
    if (!ok) remove(tmp_path);
    return ok;
}
#endif

#ifndef HOST_BUILD
/* Best-effort mkdir of the override root and of dst_path's parent, ignoring
 * EEXIST -- same pattern playlist_files_create() already uses. Every real
 * theme2 asset in this app is exactly one directory level deep (e.g.
 * "launcher/", "stream_media/"), so one mkdir beyond the root is enough;
 * this doesn't attempt a general recursive mkdir -p for deeper paths
 * nothing in this app's own asset layout actually needs. */
static void ensure_override_dir(const char * dst_path) {
    mkdir(PLUGIN_THEME_OVERRIDE_ROOT, 0755);
    char dir_only[600];
    snprintf(dir_only, sizeof(dir_only), "%s", dst_path);
    char * slash = strrchr(dir_only, '/');
    if (slash) {
        *slash = '\0';
        mkdir(dir_only, 0755);
    }
}

/* options.background_image (PLUGINS.md, plugin.set_home_layout()): copies a
 * plugin-supplied SD-card image into a FIXED theme-override destination
 * ("home/background.<ext>", extension preserved from source_path so the
 * right LVGL decoder -- lv_lodepng_init()/lv_tjpgd_init(), assets.c's own
 * assets_init() -- picks it up), then records that relative path in
 * *config for build_home_screen() (gui_settings.c) to read. Unlike
 * l_plugin_set_icon() below, the destination name is fixed by native code
 * rather than plugin-controlled, so this needs none of set_icon()'s own
 * ".." rejection -- only source_path (already validated by
 * plugin_lua_check_external_path() at the call site) ever reaches copy_file().
 * No-op (config left untouched) on HOST_BUILD, where there is no writable
 * override root for asset_path() to ever check -- see PLUGIN_THEME_
 * OVERRIDE_ROOT's own comment. */
static void set_home_background_image(lua_State * L, const char * source_path, home_layout_config_t * config) {
    const char * dot = strrchr(source_path, '.');
    const char * ext = NULL;
    if (dot && strcasecmp(dot, ".png") == 0) ext = ".png";
    else if (dot && strcasecmp(dot, ".jpg") == 0) ext = ".jpg";
    else if (dot && strcasecmp(dot, ".jpeg") == 0) ext = ".jpeg";
    else if (dot && strcasecmp(dot, ".bin") == 0) ext = ".bin";
    if (!ext) {
        luaL_error(L, "plugin.set_home_layout: options.background_image '%s' must be a .png, .jpg, .jpeg, or valid RGB565 .bin file",
                   source_path);
        return; /* unreachable -- luaL_error() longjmps */
    }

    char relative_path[32];
    snprintf(relative_path, sizeof(relative_path), "home/background%s", ext);
    char dst_path[600];
    snprintf(dst_path, sizeof(dst_path), "%s%s", PLUGIN_THEME_OVERRIDE_ROOT, relative_path);

    if (strcmp(ext, ".bin") == 0) {
        int fd = open(source_path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
        lv_image_header_t header;
        memset(&header, 0, sizeof(header));
        struct stat st;
        memset(&st, 0, sizeof(st));
        lv_display_t * display = lv_display_get_default();
        int max_w = display ? lv_display_get_horizontal_resolution(display) : BOARD_SCREEN_WIDTH;
        int max_h = display ? lv_display_get_vertical_resolution(display) : BOARD_SCREEN_HEIGHT;
        bool valid = fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode);
        if (valid) {
            size_t got = 0;
            while (got < sizeof(header)) {
                ssize_t n = read(fd, (uint8_t *) &header + got, sizeof(header) - got);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) break;
                got += (size_t) n;
            }
            valid = got == sizeof(header);
        }
        if (fd >= 0) close(fd);
        uint64_t expected = (uint64_t) sizeof(header) + (uint64_t) header.stride * header.h;
        valid = valid && header.magic == LV_IMAGE_HEADER_MAGIC && header.cf == LV_COLOR_FORMAT_RGB565 &&
                header.flags == 0 && header.reserved_2 == 0 &&
                header.w == (uint32_t) max_w && header.h == (uint32_t) max_h &&
                header.stride == header.w * 2u &&
                expected <= sizeof(header) + (uint64_t) max_w * (uint64_t) max_h * 2u &&
                st.st_size >= 0 && (uint64_t) st.st_size == expected;
        if (!valid) {
            luaL_error(L, "plugin.set_home_layout: options.background_image '%s' is not a valid RGB565 LVGL image for this display",
                       source_path);
            return;
        }
    }

    ensure_override_dir(dst_path);

    if (!copy_file(source_path, dst_path)) {
        luaL_error(L, "plugin.set_home_layout: could not copy options.background_image '%s' to '%s'",
                   source_path, relative_path);
        return; /* unreachable */
    }

    snprintf(config->background_image, sizeof(config->background_image), "%s", relative_path);
    config->has_background_image = true;
}
#endif

/* Reskins an EXISTING theme2 asset in place, e.g.
 * plugin.set_icon("launcher/book.png", plugin.sd_root() .. "/my_book.png")
 * to change what Home's Books tile looks like -- a different case from
 * plugin.register_stream_media_tile()'s own icon argument (which points a
 * BRAND NEW tile at whatever icon it likes with no special handling
 * needed). Copies source_path's bytes into
 * THEME_OVERRIDE_ROOT/relative_path, the same writable-override mechanism
 * assets.c's own asset_path() already checks first for every icon
 * resolution in the app (already how Subsonic's own non-stock icon works
 * today).
 *
 * LVGL caches decoded images by path, so callback-time batches must end
 * with plugin.refresh_theme(). Startup calls need no refresh because
 * plugin_manager_init() runs before the UI is built. */
static int l_plugin_set_icon(lua_State * L) {
    const char * relative_path = luaL_checkstring(L, 1);
    const char * source_path = plugin_lua_check_external_path(L, 2, "plugin.set_icon");

#ifndef HOST_BUILD
    /* Reject absolute paths and directory traversal to constrain writes
     * within PLUGIN_THEME_OVERRIDE_ROOT. */
    if (relative_path[0] == '/' || strstr(relative_path, "..") != NULL) {
        return luaL_error(L, "plugin.set_icon: relative_path must be a plain path under the theme root, got '%s'",
                           relative_path);
    }

    char dst_path[600];
    snprintf(dst_path, sizeof(dst_path), "%s%s", PLUGIN_THEME_OVERRIDE_ROOT, relative_path);

    ensure_override_dir(dst_path);

    if (!copy_file(source_path, dst_path)) {
        return luaL_error(L, "plugin.set_icon: could not copy '%s' to '%s'", source_path, relative_path);
    }
    gui_themes_clear_legacy_exclusion(relative_path);
#else
    (void) source_path; /* no override root on HOST_BUILD -- see assets.c's asset_path() */
    gui_themes_clear_legacy_exclusion(relative_path);
#endif
    return 0;
}

/* Sets one of three fixed background-color slots, live, app-wide, no
 * restart needed -- see gui_plugin_set_background_color()'s own comment
 * in gui.c for the shared-style mechanism (same one apply_accent_color()
 * already uses successfully for the existing accent color feature).
 * "screen": every screen's own background. "card": popups, EQ cards,
 * settings slider cards -- every neutral dark surface that isn't a plain
 * screen or a list row. "list_row": every row in every list screen (All
 * Songs, Artists, Playlists, Files, Queue, ...). */
static int l_plugin_set_background_color(lua_State * L) {
    const char * slot = luaL_checkstring(L, 1);
    lua_Integer rgb = luaL_checkinteger(L, 2);

    if (strcmp(slot, "screen") != 0 && strcmp(slot, "card") != 0 && strcmp(slot, "list_row") != 0) {
        return luaL_error(L, "plugin.set_background_color: unknown slot '%s' (expected \"screen\", \"card\", or \"list_row\")",
                           slot);
    }

    gui_plugin_set_background_color(slot, (uint32_t) rgb);
    return 0;
}

/* Sets one of two fixed text-color slots, live, app-wide, no restart needed
 * -- see gui_plugin_set_text_color()'s own comment in gui.c. "primary": the
 * app's dominant near-white text (labels, titles, list rows). "muted":
 * secondary/disabled-ish gray text (chevrons, timestamps, subtitles).
 * Destructive-red and accent-tinted text are NOT covered by either slot --
 * they're semantically fixed, not part of the light/dark background split
 * plugin.set_background_color() drives. */
static int l_plugin_set_text_color(lua_State * L) {
    const char * slot = luaL_checkstring(L, 1);
    lua_Integer rgb = luaL_checkinteger(L, 2);

    if (strcmp(slot, "primary") != 0 && strcmp(slot, "muted") != 0) {
        return luaL_error(L, "plugin.set_text_color: unknown slot '%s' (expected \"primary\" or \"muted\")", slot);
    }

    gui_plugin_set_text_color(slot, (uint32_t) rgb);
    return 0;
}

/* Reads an optional 0xRRGGBB-style integer field into out_has/out_value --
 * *out_has stays whatever the caller initialized it to (false) if the field
 * is nil/absent, so a tile table that only sets some fields leaves the rest
 * genuinely unset rather than defaulting to black/0. luaL_checkinteger (not
 * lua_tointeger) so a wrong-typed value fails loudly rather than silently
 * becoming 0, same convention every other typed plugin.* field uses. */
void get_opt_color_field(lua_State * L, int idx, const char * field, bool * out_has, uint32_t * out_value) {
    lua_getfield(L, idx, field);
    if (!lua_isnil(L, -1)) {
        *out_has = true;
        *out_value = (uint32_t) luaL_checkinteger(L, -1);
    }
    lua_pop(L, 1);
}

/* Same shape, for an optional boolean field (accessory/icon) -- lua_toboolean
 * alone can't tell "false" apart from "absent", both would otherwise read as
 * false, so *out_has records whether the field was actually present.
 * lua_toboolean() itself (not a stricter LUA_TBOOLEAN type check) is
 * deliberate: Lua's own C API has no narrower notion of "boolean" than
 * truthiness, and every other boolean-ish field in this file (toggle
 * initial state, verify_tls, ...) already reads exactly this way -- adding
 * a stricter check here alone would make this one field the inconsistent
 * one, not the other way around. */
void get_opt_bool_field(lua_State * L, int idx, const char * field, bool * out_has, bool * out_value) {
    lua_getfield(L, idx, field);
    if (!lua_isnil(L, -1)) {
        *out_has = true;
        *out_value = lua_toboolean(L, -1);
    }
    lua_pop(L, 1);
}

/* Validates that a 64-bit Lua integer fits within int32_t bounds before casting. */
int32_t check_int32_field(lua_State * L, lua_Integer value, const char * fn_name, const char * field) {
    if (value < INT32_MIN || value > INT32_MAX) {
        return (int32_t) luaL_error(L, "%s: %s (%lld) is out of range", fn_name, field, (long long) value);
    }
    return (int32_t) value;
}

/* Restyles Home's tiles and controls which ones are shown, and in what
 * order (PLUGINS.md). This never writes anything to disk; persistence
 * requires a top-level re-call next boot. plugin.refresh_theme() applies a
 * callback-time change immediately.
 *
 * Every plugin restyles the SAME single global layout -- there is no
 * per-plugin slot, so if two installed plugins both call this, whichever
 * one loads later (see plugin_manager_init()'s now-deterministic,
 * alphabetical-by-filename load order) completely replaces the earlier
 * one's config, not merged.
 *
 * `tiles`: array of tables, one per tile being restyled -- { key, bg_color,
 * text_color, radius, height, width, align, accessory, text_size, icon }.
 * `key` is one of the native names ("music"/"stream_media"/"wireless"/
 * "books"/"settings"/"dac"/"subsonic") or a plugin.register_home_tile() id -- unlike
 * before API 7, an unrecognized-looking key is NOT rejected here: it may
 * name a plugin tile that hasn't registered yet (plugin_manager_init()
 * loads plugins in filename order, so a theme plugin can run before the
 * plugin providing a tile it references), so resolution against both the
 * native keys and the live plugin-tile registry happens later, in
 * build_home_screen(), which always runs after every plugin has finished
 * loading. A tile key never mentioned keeps every field at its native
 * default; a REPEATED key within this same array cleanly replaces the
 * earlier entry for that tile rather than merging the two. height/width are
 * passed through unclamped (same as register_list_item()'s own options) --
 * build_pill_list_screen() clamps them to PILL_ROW_HEIGHT_MIN/MAX and
 * PILL_ROW_WIDTH_MIN/MAX itself when list mode actually renders them.
 *
 * `options` (optional): { mode = "tile"|"list", tile_gap, row_gap, order }.
 * `mode` defaults to "tile" (today's icon grid) if `options` or `mode` is
 * omitted. `tile_gap`/`row_gap` are passed through unclamped here too --
 * clamped to 16-64 (card gap floor) in build_icon_grid_screen() and, for row_gap,
 * to 0-84 in home_list_row_gap() (gui_home.c) before build_pill_list_screen()
 * (screen_builders_menus.c), the same "clamp at the point of use"
 * pattern height/width above already follow, since tile_gap in particular
 * feeds directly into an available-space subtraction that goes wrong long
 * before any Lua-side integer range check would think to reject it.
 *
 * `order` (optional array of strings): which tiles Home shows and in what
 * position -- position IS order. Each entry is a native key or a plugin
 * tile id, same "resolved later" deferral as `tiles`' own `key` above.
 * Omitted entirely (order left empty) means "unconfigured", falling back to
 * today's exact fixed 6-native-tile order. A native key simply not
 * mentioned in `order` is not shown at all -- this is how a theme drops a
 * tile, not just reorders one. Duplicate entries are rejected immediately
 * (unlike unresolved-key names, a literal repeat is unambiguously a
 * mistake, not a load-order timing issue). */
static int l_plugin_set_home_layout(lua_State * L) {
    luaL_checktype(L, 1, LUA_TTABLE);

    home_layout_config_t config;
    memset(&config, 0, sizeof(config));
    config.configured = true;

    /* A `tiles` array longer than the max any theme could ever legitimately
     * need cannot arise from real use, only from a duplicate key or a
     * copy-paste mistake, so this fails loudly instead of quietly
     * truncating. */
    lua_Unsigned raw_n = lua_rawlen(L, 1);
    if (raw_n > (lua_Unsigned) HOME_LAYOUT_MAX_TILES) {
        return luaL_error(L, "plugin.set_home_layout: tiles has %lld entries, more than the %d-tile limit",
                           (long long) raw_n, HOME_LAYOUT_MAX_TILES);
    }
    int n = (int) raw_n;
    for (int i = 0; i < n; i++) {
        lua_rawgeti(L, 1, i + 1); /* tile table -> stack top */
        if (!lua_istable(L, -1)) {
            return luaL_error(L, "plugin.set_home_layout: tile %d must be a table", i + 1);
        }

        lua_getfield(L, -1, "key");
        const char * key = lua_tostring(L, -1);
        if (!key || key[0] == '\0') {
            return luaL_error(L, "plugin.set_home_layout: tile %d has a missing or empty key", i + 1);
        }
        if (strlen(key) >= sizeof(config.tiles[0].key)) {
            return luaL_error(L, "plugin.set_home_layout: tile %d's key '%s' is too long", i + 1, key);
        }
        char key_copy[sizeof(config.tiles[0].key)];
        snprintf(key_copy, sizeof(key_copy), "%s", key);
        lua_pop(L, 1); /* key string */

        /* A repeated key within one call cleanly replaces the earlier entry
         * rather than merging fields across the two -- without this, a
         * second { key = "music", text_color = ... } entry would keep
         * whatever an earlier { key = "music", bg_color = ... } entry set,
         * silently combining two calls' worth of fields into one tile. */
        int idx = -1;
        for (int t = 0; t < config.tile_count; t++) {
            if (strcmp(config.tiles[t].key, key_copy) == 0) { idx = t; break; }
        }
        if (idx < 0) {
            if (config.tile_count >= HOME_LAYOUT_MAX_TILES) {
                return luaL_error(L, "plugin.set_home_layout: too many distinct tile keys (max %d)",
                                   HOME_LAYOUT_MAX_TILES);
            }
            idx = config.tile_count++;
            snprintf(config.tiles[idx].key, sizeof(config.tiles[idx].key), "%s", key_copy);
        }
        home_tile_override_t * ov = &config.tiles[idx].override;
        memset(ov, 0, sizeof(*ov));
        int row_idx = lua_gettop(L); /* the tile table itself, still on the stack */

        get_opt_color_field(L, row_idx, "bg_color", &ov->has_bg_color, &ov->bg_color);
        get_opt_color_field(L, row_idx, "text_color", &ov->has_text_color, &ov->text_color);

        lua_getfield(L, row_idx, "radius");
        if (!lua_isnil(L, -1)) {
            lua_Integer radius = luaL_checkinteger(L, -1);
            if (radius < 0) {
                lua_pop(L, 1);
                return luaL_error(L, "plugin.set_home_layout: tile %d has a negative radius", i + 1);
            }
            ov->has_radius = true;
            ov->radius = check_int32_field(L, radius, "plugin.set_home_layout", "radius");
        }
        lua_pop(L, 1);

        ov->height = check_int32_field(L, plugin_lua_opt_integer_field(L, row_idx, "height", 0), "plugin.set_home_layout", "height");

        ov->width = check_int32_field(L, plugin_lua_opt_integer_field(L, row_idx, "width", 0), "plugin.set_home_layout", "width");

        lua_getfield(L, row_idx, "align");
        const char * align = lua_tostring(L, -1);
        if (align) {
            if (strcmp(align, "left") != 0 && strcmp(align, "center") != 0 && strcmp(align, "right") != 0) {
                lua_pop(L, 1);
                return luaL_error(L, "plugin.set_home_layout: tile %d has an unknown align '%s' "
                                   "(expected \"left\", \"center\", or \"right\")", i + 1, align);
            }
            snprintf(ov->align, sizeof(ov->align), "%s", align);
        }
        lua_pop(L, 1);

        get_opt_bool_field(L, row_idx, "accessory", &ov->has_accessory, &ov->accessory);
        get_opt_bool_field(L, row_idx, "icon", &ov->has_icon, &ov->icon);

        lua_getfield(L, row_idx, "text_size");
        const char * text_size = lua_tostring(L, -1);
        if (text_size) {
            if (!is_valid_text_size(text_size)) {
                lua_pop(L, 1);
                return luaL_error(L, "plugin.set_home_layout: tile %d has an unknown text_size '%s' "
                                   "(expected \"small\", \"medium\", \"large\", or \"mono\")", i + 1, text_size);
            }
            snprintf(ov->text_size, sizeof(ov->text_size), "%s", text_size);
        }
        lua_pop(L, 1);

        lua_pop(L, 1); /* tile table */
    }

    /* Unlike show_list()'s own options table (a plain "resize every row"
     * convenience where silently ignoring a wrong-typed argument costs
     * nothing), a plugin passing a non-table, non-nil `options` here has
     * unambiguously made a mistake -- there is no reasonable "options" value
     * that isn't a table, so this fails loudly rather than silently
     * behaving as if `options` were omitted. */
    if (lua_gettop(L) >= 2 && !lua_isnil(L, 2) && !lua_istable(L, 2)) {
        return luaL_error(L, "plugin.set_home_layout: options must be a table");
    }
    if (lua_gettop(L) >= 2 && lua_istable(L, 2)) {
        lua_getfield(L, 2, "mode");
        const char * mode = lua_tostring(L, -1);
        if (mode) {
            if (strcmp(mode, "tile") == 0) config.list_mode = false;
            else if (strcmp(mode, "list") == 0) config.list_mode = true;
            else {
                lua_pop(L, 1);
                return luaL_error(L, "plugin.set_home_layout: unknown mode '%s' (expected \"tile\" or \"list\")", mode);
            }
        }
        lua_pop(L, 1);

        config.tile_gap = check_int32_field(L, plugin_lua_opt_integer_field(L, 2, "tile_gap", 0), "plugin.set_home_layout", "tile_gap");

        config.row_gap = check_int32_field(L, plugin_lua_opt_integer_field(L, 2, "row_gap", 0), "plugin.set_home_layout", "row_gap");

#ifndef HOST_BUILD
        lua_getfield(L, 2, "background_image");
        if (!lua_isnil(L, -1)) {
            const char * source_path = plugin_lua_check_external_path(L, -1, "plugin.set_home_layout");
            set_home_background_image(L, source_path, &config);
        }
        lua_pop(L, 1);
#endif

        lua_getfield(L, 2, "order");
        if (!lua_isnil(L, -1)) {
            if (!lua_istable(L, -1)) {
                return luaL_error(L, "plugin.set_home_layout: options.order must be an array table");
            }
            lua_Unsigned order_n = lua_rawlen(L, -1);
            if (order_n > (lua_Unsigned) HOME_LAYOUT_MAX_TILES) {
                return luaL_error(L, "plugin.set_home_layout: options.order has %lld entries, more than the %d-tile limit",
                                   (long long) order_n, HOME_LAYOUT_MAX_TILES);
            }
            int order_idx = lua_gettop(L); /* the order table itself, still on the stack */
            int order_n_int = (int) order_n;
            for (int i = 0; i < order_n_int; i++) {
                lua_rawgeti(L, order_idx, i + 1);
                const char * entry = lua_tostring(L, -1);
                if (!entry || entry[0] == '\0') {
                    return luaL_error(L, "plugin.set_home_layout: options.order entry %d must be a non-empty string",
                                       i + 1);
                }
                if (strlen(entry) >= sizeof(config.order[0])) {
                    return luaL_error(L, "plugin.set_home_layout: options.order entry %d ('%s') is too long",
                                       i + 1, entry);
                }
                /* Unlike an unresolved-key name (which may just not have
                 * registered yet, see this function's own doc comment), a
                 * literal duplicate within one `order` array is unambiguous
                 * -- reject immediately rather than defer. */
                for (int j = 0; j < i; j++) {
                    if (strcmp(config.order[j], entry) == 0) {
                        return luaL_error(L, "plugin.set_home_layout: options.order has a duplicate entry '%s'", entry);
                    }
                }
                snprintf(config.order[i], sizeof(config.order[i]), "%s", entry);
                lua_pop(L, 1); /* entry string */
            }
            config.order_count = order_n_int;
        }
        lua_pop(L, 1); /* order (nil or table) */

        /* build_icon_grid_screen() (screen_builders_menus.c) cannot scroll -- its
         * vertical scroll-direction setting exists only to stop it from
         * capturing the app-wide back-swipe gesture, not to provide real
         * scrolling -- so a tile-mode order past 2 columns x 3 reference
         * rows (ICON_GRID_REFERENCE_ROWS*2, screen_builders_menus.c -- duplicated
         * here as a literal since that constant isn't exposed via a header)
         * would render with tiles silently unreachable off-screen. Fail
         * loud here instead, matching this function's own tiles-array-
         * length rejection above and register_stream_media_tile()'s own
         * cap rejection. */
        if (!config.list_mode && config.order_count > 6) {
            return luaL_error(L, "plugin.set_home_layout: tile mode supports at most 6 tiles (got %d in "
                               "options.order) -- use { mode = \"list\" } for more", config.order_count);
        }
    }

    gui_plugin_set_home_layout(&config);
    return 0;
}

/* The single pending plugin.show_text_input() call's on_submit function --
 * show_text_entry() (gui.c) is itself a true singleton screen. A second
 * plugin request now returns "text input busy" rather than overwriting this
 * callback, and Back explicitly releases it. on_submit only fires on a T9
 * keypad Enter, never on the screen's own back button. Documented in
 * PLUGINS.md rather than engineered away, matching this codebase's existing
 * tolerance for show_list()'s own analogous single-ref caveat. */
lua_State * pending_text_input_L = NULL;
int pending_text_input_ref = LUA_NOREF;

/* A confirmation callback is rooted in its owner's main state, even when
 * show_confirmation() was called from a coroutine. */
static lua_State * pending_confirmation_L = NULL;
static int pending_confirmation_ref = LUA_NOREF;

/* Same ownership rule for the one pending plugin.show_menu() callback. */
static lua_State * pending_menu_L = NULL;
static int pending_menu_ref = LUA_NOREF;

static int l_plugin_show_text_input(lua_State * L) {
    const char * title = luaL_checkstring(L, 1);
    const char * initial_text = (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) ? luaL_checkstring(L, 2) : NULL;
    bool is_password = lua_toboolean(L, 3);
    luaL_checktype(L, 4, LUA_TFUNCTION);

    if (pending_text_input_L && pending_text_input_ref != LUA_NOREF) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "text input busy");
        return 2;
    }
    lua_pushvalue(L, 4);
    pending_text_input_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    pending_text_input_L = plugin_lua_main_state(L);

    gui_plugin_show_text_input(title, initial_text, is_password);
    lua_pushboolean(L, true);
    return 1;
}

static int l_plugin_show_confirmation(lua_State * L) {
    const char * title = luaL_checkstring(L, 1);
    const char * confirm_label = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    if (pending_confirmation_L && pending_confirmation_ref != LUA_NOREF) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "confirmation busy");
        return 2;
    }
    lua_pushvalue(L, 3);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_State * owner = plugin_lua_main_state(L);
    if (!gui_plugin_show_confirmation(title, confirm_label)) {
        luaL_unref(owner, LUA_REGISTRYINDEX, ref);
        lua_pushboolean(L, false);
        lua_pushstring(L, "unable to show confirmation");
        return 2;
    }
    pending_confirmation_ref = ref;
    pending_confirmation_L = owner;
    lua_pushboolean(L, true);
    return 1;
}

#define PLUGIN_MENU_MAX_ITEMS 64
#define PLUGIN_MENU_LABEL_BYTES 160

/* plugin.show_menu(title, items, on_select [, { selected = n }]) -- a native
 * popup of 1..64 string choices plus Cancel; it scrolls when they do not fit. on_select(index) runs after the
 * popup closes; every other dismissal releases the callback without a call. */
static int l_plugin_show_menu(lua_State * L) {
    const char * title = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    lua_Unsigned raw_n = lua_rawlen(L, 2);
    if (raw_n < 1 || raw_n > PLUGIN_MENU_MAX_ITEMS)
        return luaL_error(L, "plugin.show_menu: items must hold 1 to %d strings", PLUGIN_MENU_MAX_ITEMS);
    int n = (int) raw_n;
    for (int i = 0; i < n; i++) {
        lua_rawgeti(L, 2, i + 1);
        if (lua_type(L, -1) != LUA_TSTRING)
            return luaL_error(L, "plugin.show_menu: item %d must be a string", i + 1);
        lua_pop(L, 1);
    }
    int selected_index = -1;
    if (!lua_isnoneornil(L, 4)) {
        luaL_checktype(L, 4, LUA_TTABLE);
        lua_getfield(L, 4, "selected");
        if (!lua_isnil(L, -1)) {
            lua_Integer selected = luaL_checkinteger(L, -1);
            if (selected < 1 || selected > n)
                return luaL_error(L, "plugin.show_menu: selected index %lld is outside 1..%d",
                                  (long long) selected, n);
            selected_index = (int) selected - 1;
        }
        lua_pop(L, 1);
    }
    if (pending_menu_L && pending_menu_ref != LUA_NOREF) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "busy");
        return 2;
    }
    /* Labels are copied (cut to 159 bytes) after every check that can raise
     * a Lua error, so the buffer cannot leak. */
    char (*labels)[PLUGIN_MENU_LABEL_BYTES] = malloc((size_t) n * sizeof(*labels));
    const char ** label_ptrs = malloc((size_t) n * sizeof(*label_ptrs));
    if (!labels || !label_ptrs) {
        free(labels);
        free(label_ptrs);
        lua_pushboolean(L, false);
        lua_pushstring(L, "unavailable");
        return 2;
    }
    for (int i = 0; i < n; i++) {
        lua_rawgeti(L, 2, i + 1);
        plugin_copy_label(labels[i], sizeof(labels[i]), lua_tostring(L, -1));
        label_ptrs[i] = labels[i];
        lua_pop(L, 1);
    }
    bool shown = gui_plugin_show_menu(title, label_ptrs, n, selected_index);
    free(labels);
    free(label_ptrs);
    if (!shown) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "unavailable");
        return 2;
    }
    /* The popup only calls back through pending_menu_ref, so taking the ref
     * after showing it is safe even if luaL_ref raises. */
    lua_pushvalue(L, 3);
    pending_menu_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    pending_menu_L = plugin_lua_main_state(L);
    lua_pushboolean(L, true);
    return 1;
}

void discard_pending_confirmation(lua_State * L, bool aborted) {
    if (!failed_load_owner_matches(pending_confirmation_L, L)) return;
    gui_plugin_close_confirmation();
    if (!aborted) luaL_unref(L, LUA_REGISTRYINDEX, pending_confirmation_ref);
    pending_confirmation_L = NULL;
    pending_confirmation_ref = LUA_NOREF;
}

void discard_pending_menu(lua_State * L, bool aborted) {
    if (!failed_load_owner_matches(pending_menu_L, L)) return;
    gui_plugin_close_menu();
    if (!aborted) luaL_unref(L, LUA_REGISTRYINDEX, pending_menu_ref);
    pending_menu_L = NULL;
    pending_menu_ref = LUA_NOREF;
}

void plugin_manager_text_input_submitted(const char * text) {
    if (!pending_text_input_L || pending_text_input_ref == LUA_NOREF) return;

    lua_State * L = pending_text_input_L;
    int ref = pending_text_input_ref;
    pending_text_input_L = NULL;
    pending_text_input_ref = LUA_NOREF;

    if (!plugin_push_callback(L, ref)) return;
    lua_pushstring(L, text ? text : "");
    plugin_call_report(L, 1, false, "show_text_input on_submit error");
    plugin_unref(L, ref);
}

void plugin_manager_text_input_cancelled(void) {
    if (!pending_text_input_L || pending_text_input_ref == LUA_NOREF) return;
    plugin_unref(pending_text_input_L, pending_text_input_ref);
    pending_text_input_L = NULL;
    pending_text_input_ref = LUA_NOREF;
}

void plugin_manager_confirmation_confirmed(void) {
    if (!pending_confirmation_L || pending_confirmation_ref == LUA_NOREF) return;
    lua_State * L = pending_confirmation_L;
    int ref = pending_confirmation_ref;
    /* Clear first so a callback can safely open another confirmation. */
    pending_confirmation_L = NULL;
    pending_confirmation_ref = LUA_NOREF;
    if (!plugin_push_callback(L, ref)) return;
    plugin_call_report(L, 0, true, "show_confirmation callback error");
    plugin_unref(L, ref);
}

void plugin_manager_confirmation_cancelled(void) {
    if (!pending_confirmation_L || pending_confirmation_ref == LUA_NOREF) return;
    plugin_unref(pending_confirmation_L, pending_confirmation_ref);
    pending_confirmation_L = NULL;
    pending_confirmation_ref = LUA_NOREF;
}

void plugin_manager_menu_selected(int index) {
    if (!pending_menu_L || pending_menu_ref == LUA_NOREF) return;
    lua_State * L = pending_menu_L;
    int ref = pending_menu_ref;
    /* Clear first so a callback can safely open another menu. */
    pending_menu_L = NULL;
    pending_menu_ref = LUA_NOREF;
    if (!plugin_push_callback(L, ref)) return;
    lua_pushinteger(L, index + 1);
    plugin_call_report(L, 1, true, "show_menu callback error");
    plugin_unref(L, ref);
}

void plugin_manager_menu_cancelled(void) {
    if (!pending_menu_L || pending_menu_ref == LUA_NOREF) return;
    plugin_unref(pending_menu_L, pending_menu_ref);
    pending_menu_L = NULL;
    pending_menu_ref = LUA_NOREF;
}

const luaL_Reg plugin_ui_funcs[] = {
    { "show_toast",                l_plugin_show_toast },
    { "show_progress",             l_plugin_show_progress },
    { "update_progress",           l_plugin_update_progress },
    { "close_progress",            l_plugin_close_progress },
    { "screenshot",                l_plugin_screenshot },
    { "set_icon",                  l_plugin_set_icon },
    { "set_background_color",      l_plugin_set_background_color },
    { "set_text_color",            l_plugin_set_text_color },
    { "set_home_layout",           l_plugin_set_home_layout },
    { "show_text_input",           l_plugin_show_text_input },
    { "show_confirmation",         l_plugin_show_confirmation },
    { "show_menu",                 l_plugin_show_menu },
    { NULL, NULL }
};
