#ifndef GUI_THEMES_H
#define GUI_THEMES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GUI_THEMES_MAX_PATH 256
#define GUI_THEMES_MAX_NAME 64
#define GUI_THEMES_MAX_EXCLUSIONS 256
#define GUI_THEMES_SHA256_HEX_LEN 64

/* Scanned theme descriptor */
typedef struct {
    char filename[GUI_THEMES_MAX_NAME];  /* e.g. "White.theme", "pack/custom.theme", or "" for Default */
    char name[GUI_THEMES_MAX_NAME];      /* Display label */
    char fullpath[GUI_THEMES_MAX_PATH];  /* Absolute path to .theme file; empty string for Default */
    bool is_default;                     /* True if built-in Default theme */
    uint32_t preview_color;              /* Swatch fallback color (card or screen color) */
} gui_theme_entry_t;

/* Legacy exclusion record (persisted in state file) */
typedef struct {
    char relpath[GUI_THEMES_MAX_PATH];
    uint64_t size;
    int64_t mtime;
    char sha256[GUI_THEMES_SHA256_HEX_LEN + 1];
} gui_theme_legacy_exclusion_t;

#ifdef GUI_THEMES_TEST
/**
 * Configure storage paths for testing or runtime customization.
 * Passing NULL or empty string resets the respective root to its compile-time default.
 */
void gui_themes_set_test_roots(const char *sd_root, const char *internal_compas_dir,
                               const char *override_root, const char *theme1_root,
                               const char *theme2_root);

/**
 * Resets in-memory service state (for unit testing isolation).
 */
void gui_themes_reset_state(void);
#endif

/**
 * Initializes the native theme service.
 * Loads atomic state from persistent storage, performs one-time legacy migration
 * if evidence exists and SD is available, and validates the saved theme selection.
 * If SD was previously unavailable, calling init again will retry migration.
 */
void gui_themes_init(void);

/**
 * Applies the currently selected native theme onto the fresh plugin baseline.
 * Must be called after plugin_manager_init() on boot or full UI reload.
 *
 * Requirements:
 * - Default or omitted theme home/launcher preserves full independent plugin configuration.
 * - When a theme defines home fields, overlays only provided fields onto baseline.
 * - Preserves HomeBackground image if theme omits it.
 * - Default preserves fresh plugin colors.
 * - Missing/invalid file falls back without mutating saved user intent.
 */
void gui_themes_apply_active(void);

/**
 * Opens the native theme picker screen.
 * Rescans SD/Themes fresh upon opening.
 */
void gui_themes_open_picker(void);

/**
 * Tears down and frees any picker-allocated screens/buffers safely.
 * Removes screen from nav stack and calls lv_obj_delete before nulling.
 */
void gui_themes_teardown(void);

/**
 * Resolves an icon relative path against the active native theme layer.
 *
 * Resolution order:
 * 1. Explicit theme icon.* override (falls through if unreadable)
 * 2. Theme drawer_root for "pull_down/..." assets ("theme2" chooses stock drawer before icon_root)
 * 3. Theme icon_root (if defined, and != "theme2")
 * 4. Stock fallback (returns false)
 *
 * Safe allowed roots are strictly checked.
 */
bool gui_themes_resolve_icon_layer(const char *relpath, char *out_path, size_t out_size);

/**
 * Checks whether a given relative path in /usr/data/theme_overrides/ is excluded
 * due to being a legacy Themes.lua bulk-copy of stock or legacy theme assets.
 *
 * Compares size and mtime against recorded exclusion.
 * Size/mtime mismatch invalidates the exclusion entry (no sha256 hashing per lookup).
 */
bool gui_themes_is_legacy_excluded(const char *relpath);

/**
 * Clears an exclusion for relpath after a successful plugin.set_icon call.
 * Persists updated state only if a matching exclusion record was actually present.
 */
void gui_themes_clear_legacy_exclusion(const char *relpath);

/**
 * Recognizes legacy Themes.lua plugin loader identity.
 */
bool gui_themes_is_legacy_theme_loader(const char *plugin_id, const char *filename);

/**
 * Returns the currently selected theme filename/identifier.
 * Returns "" if Default is selected.
 */
const char * gui_themes_get_selected(void);

/**
 * Selects a theme filename, verifies validity, atomically commits state,
 * and requests deferred UI reload. Returns true on success, false on validation
 * or atomic save failure (leaving state and globals unchanged).
 */
bool gui_themes_select(const char *filename);

/**
 * Scans SD/Themes directory for available .theme files.
 * Always prepends "Default" as the first entry.
 * Out array is populated up to max_entries.
 * Returns the total count of scanned entries.
 */
size_t gui_themes_scan(gui_theme_entry_t *out_entries, size_t max_entries);

#ifdef __cplusplus
}
#endif

#endif /* GUI_THEMES_H */
