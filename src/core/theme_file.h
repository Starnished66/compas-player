#ifndef THEME_FILE_H
#define THEME_FILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "src/ui/home_layout.h"
#include "src/ui/launcher_layout.h"

#define THEME_MAX_NAME_LEN 64
#define THEME_MAX_PATH_LEN 128
#define THEME_MAX_ICON_OVERRIDES 32

typedef struct {
    char relpath[THEME_MAX_PATH_LEN];
    char source[THEME_MAX_PATH_LEN];
} theme_icon_override_t;

typedef struct {
    char name[THEME_MAX_NAME_LEN];

    /* 5 required colors (0x000000 - 0xFFFFFF) */
    uint32_t screen;
    uint32_t card;
    uint32_t list_row;
    uint32_t text_primary;
    uint32_t text_muted;

    /* Optional roots (empty string if not specified) */
    char icon_root[THEME_MAX_PATH_LEN];
    char drawer_root[THEME_MAX_PATH_LEN];

    /* Bounded icon override pairs */
    theme_icon_override_t icon_overrides[THEME_MAX_ICON_OVERRIDES];
    size_t icon_override_count;

    /* Home & Launcher layouts
     * Note: In Lua Themes loader, layouts were reset when omitted.
     * When has_home or has_launcher are false, consumer should be aware that
     * layouts are not provided by the theme and baseline layout applies.
     */
    home_layout_config_t home;
    launcher_layout_config_t launcher;
    bool has_home;
    bool has_launcher;

    /* Presence flags for home & launcher fields */
    bool has_home_mode;
    bool has_home_tile_gap;
    bool has_home_row_gap;
    bool has_home_order;
    bool has_submenu_mode;
} theme_file_t;

/**
 * Parses a pure native .theme file from the filesystem.
 *
 * Security Note:
 * - Icon paths parsed into icon_root, drawer_root, and icon_overrides (source/relpath)
 *   are UNTRUSTED external input. The consumer MUST validate that paths resolve
 *   within safe allowed roots and do not access reserved plugin storage/secrets
 *   or escape directories via traversal or symlinks before opening or exposing them.
 *
 * Requirements:
 * - Does not call LVGL, modify global state, or persist anything.
 * - Leaves `out` completely untouched on failure.
 * - If parsing succeeds, returns true.
 * - On failure, returns false and fills `error` (if non-NULL and error_size > 0).
 */
bool theme_file_parse(const char *path, theme_file_t *out, char *error, size_t error_size);

#endif /* THEME_FILE_H */
