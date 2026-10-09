#include "gui_themes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <ctype.h>

#include "lvgl.h"
#include "board_config.h"
#include "src/core/theme_file.h"
#include "src/core/storage_paths.h"
#include "src/ui/gui_theme.h"
#include "src/ui/gui_navigation.h"
#include "src/ui/gui_reload.h"
#include "src/ui/screen_builders.h"
#include "src/ui/home_layout.h"
#include "src/ui/launcher_layout.h"
#include "src/ui/i18n.h"
#include "src/ui/gui_notifications.h"
#ifndef HOST_BUILD
#include "gui_library.h"
#endif

/* Mandatory mbedtls SHA256 */
#include "mbedtls/sha256.h"

/* Setters implemented in gui_plugins.c */
extern void gui_plugin_set_background_color(const char * slot, uint32_t rgb);
extern void gui_plugin_set_text_color(const char * slot, uint32_t rgb);
extern void gui_plugin_set_home_layout(const home_layout_config_t * config);
extern void gui_plugin_reset_home_layout(void);
extern void gui_plugin_set_launcher_layout(const launcher_layout_config_t * config);
extern void gui_plugin_reset_launcher_layout(void);

/* Forward declaration for forward-compatible plugin store themes opener */
extern bool gui_plugin_store_open_themes(void);
extern bool gui_plugin_store_operation_active(void);

#ifdef HOST_BUILD
#define DEFAULT_NATIVE_SD_ROOT "./music"
#define DEFAULT_NATIVE_OVERRIDE_ROOT "./.compas/theme_overrides/"
#define DEFAULT_NATIVE_THEME1_ROOT "assets/theme1/"
#define DEFAULT_NATIVE_THEME2_ROOT "assets/theme2/"
#define DEFAULT_INTERNAL_COMPAS_DIR "./.compas"
#else
#define DEFAULT_NATIVE_SD_ROOT "/data/mnt/sd_0"
#define DEFAULT_NATIVE_OVERRIDE_ROOT "/usr/data/theme_overrides/"
#define DEFAULT_NATIVE_THEME1_ROOT "/usr/resource/litegui/theme1/"
#define DEFAULT_NATIVE_THEME2_ROOT "/usr/resource/litegui/theme2/"
#define DEFAULT_INTERNAL_COMPAS_DIR "/usr/data/.compas"
#endif

#define NATIVE_THEMES_SUBDIR "Themes"
#define NATIVE_STATE_FILENAME "theme_native_state.txt"
#define LEGACY_STATE_SUBPATH ".plugins/.theme_state"

/* Configurable roots for testing or runtime redirection */
static char s_sd_root[GUI_THEMES_MAX_PATH] = DEFAULT_NATIVE_SD_ROOT;
static char s_internal_compas_dir[GUI_THEMES_MAX_PATH] = DEFAULT_INTERNAL_COMPAS_DIR;
static char s_override_root[GUI_THEMES_MAX_PATH] = DEFAULT_NATIVE_OVERRIDE_ROOT;
static char s_theme1_root[GUI_THEMES_MAX_PATH] = DEFAULT_NATIVE_THEME1_ROOT;
static char s_theme2_root[GUI_THEMES_MAX_PATH] = DEFAULT_NATIVE_THEME2_ROOT;

static char s_canonical_roots[3][PATH_MAX];
static bool s_roots_cached = false;

#ifdef GUI_THEMES_TEST
void gui_themes_set_test_roots(const char *sd_root, const char *internal_compas_dir,
                               const char *override_root, const char *theme1_root,
                               const char *theme2_root) {
    if (sd_root && sd_root[0]) snprintf(s_sd_root, sizeof(s_sd_root), "%s", sd_root);
    else snprintf(s_sd_root, sizeof(s_sd_root), "%s", DEFAULT_NATIVE_SD_ROOT);

    if (internal_compas_dir && internal_compas_dir[0]) snprintf(s_internal_compas_dir, sizeof(s_internal_compas_dir), "%s", internal_compas_dir);
    else snprintf(s_internal_compas_dir, sizeof(s_internal_compas_dir), "%s", DEFAULT_INTERNAL_COMPAS_DIR);

    if (override_root && override_root[0]) snprintf(s_override_root, sizeof(s_override_root), "%s", override_root);
    else snprintf(s_override_root, sizeof(s_override_root), "%s", DEFAULT_NATIVE_OVERRIDE_ROOT);

    if (theme1_root && theme1_root[0]) snprintf(s_theme1_root, sizeof(s_theme1_root), "%s", theme1_root);
    else snprintf(s_theme1_root, sizeof(s_theme1_root), "%s", DEFAULT_NATIVE_THEME1_ROOT);

    if (theme2_root && theme2_root[0]) snprintf(s_theme2_root, sizeof(s_theme2_root), "%s", theme2_root);
    else snprintf(s_theme2_root, sizeof(s_theme2_root), "%s", DEFAULT_NATIVE_THEME2_ROOT);
}
#endif

/* Fixed stock asset inventory copied by legacy Themes.lua */
static const char * const s_legacy_asset_inventory[] = {
    "boot_animation/en/0.jpg", "bt/bt.png",
    "keyboard/char.png", "keyboard/del.png", "keyboard/dot.png", "keyboard/enter.png",
    "keyboard/left.png", "keyboard/num.png", "keyboard/psk_show.png", "keyboard/psk_hide.png",
    "keyboard/right.png", "keyboard/space2.png", "keyboard/symbol.png", "keyboard/upper.png",
    "keyboard/0.png", "keyboard/1.png", "keyboard/2.png", "keyboard/3.png", "keyboard/4.png",
    "keyboard/5.png", "keyboard/6.png", "keyboard/7.png", "keyboard/8.png", "keyboard/9.png",
    "keyboard/symbol0.png", "keyboard/symbol1.png", "keyboard/symbol2.png", "keyboard/symbol3.png",
    "keyboard/symbol4.png", "keyboard/symbol5.png", "keyboard/symbol6.png", "keyboard/symbol7.png",
    "keyboard/symbol8.png", "keyboard/symbol9.png",
    "keyboard/char_l.png", "keyboard/char_u.png",
    "keyboard/abc_l.png", "keyboard/abc_u.png", "keyboard/def_l.png", "keyboard/def_u.png",
    "keyboard/ghi_l.png", "keyboard/ghi_u.png", "keyboard/jkl_l.png", "keyboard/jkl_u.png",
    "keyboard/mno_l.png", "keyboard/mno_u.png", "keyboard/pqrs_l.png", "keyboard/pqrs_u.png",
    "keyboard/tuv_l.png", "keyboard/tuv_u.png", "keyboard/wxyz_l.png", "keyboard/wxyz_u.png",
    "launcher/music.png", "launcher/music_s.png",
    "launcher/stream_media.png", "launcher/stream_media_s.png",
    "launcher/wireless.png", "launcher/wireless_s.png",
    "launcher/book.png", "launcher/book_s.png",
    "launcher/sys_set.png", "launcher/sys_set_s.png",
    "launcher/dac.png", "launcher/dac_s.png",
    "playing_plane/btn_next.png", "playing_plane/btn_next_s.png",
    "playing_plane/btn_play.png", "playing_plane/btn_pause.png",
    "playing_plane/btn_prev.png", "playing_plane/btn_prev_s.png",
    "playing_plane/collect_out.png", "playing_plane/collect_in.png",
    "playing_plane/default_cover_565.png", "playing_plane/ic_more.png",
    "playing_plane/loop.png", "playing_plane/single.png", "playing_plane/random.png", "playing_plane/order.png",
    "playing_plane/quality_waveform.png",
    "playing_plane/airplay_logo_white.png",
    "power_action/power.png", "power_action/reboot.png",
    "pull_down/blk.png", "pull_down/bt.png", "pull_down/bt_s.png",
    "pull_down/sleep_switch.png", "pull_down/sleep_switch_s.png",
    "pull_down/wifi.png", "pull_down/wifi_s.png", "pull_down/fade.png", "pull_down/fade_s.png",
    "pull_down/airplay.png", "pull_down/airplay_s.png",
    "pull_down/dlna.png", "pull_down/dlna_s.png",
    "pull_down/gapless_play.png", "pull_down/gapless_play_s.png",
    "pull_down/hibylink.png", "pull_down/hibylink_s.png",
    "sub_back/bg_search.png", "sub_back/btn_back.png", "sub_back/btn_search.png", "sub_back/close.png",
    "sub_back/btn_playlist.png", "sub_back/set.png",
    "topbar/lucide_audio_lines.png", "topbar/lucide_headphones.png",
    "topbar/lucide_volume_2.png", "topbar/lucide_usb.png",
    "topbar/lucide_play.png", "topbar/lucide_pause.png",
    "topbar/lucide_battery.png", "topbar/lucide_battery_charging.png",
    "topbar/lucide_battery_full.png", "topbar/lucide_battery_medium.png",
    "topbar/lucide_battery_low.png",
    "topbar/lucide_bluetooth.png", "topbar/lucide_bluetooth_off.png",
    "topbar/lucide_wifi.png", "topbar/lucide_wifi_high.png",
    "topbar/lucide_wifi_low.png", "topbar/lucide_wifi_zero.png",
    "topbar/lucide_wifi_off.png",
    "topbar/wifi_connect_0.png", "topbar/wifi_connect_1.png", "topbar/wifi_connect_2.png", "topbar/wifi_connect_3.png",
    "topbar/sbc.png", "topbar/aac.png", "topbar/aptx.png", "topbar/aptx_hd.png", "topbar/ldac.png", "topbar/uat.png",
    "touch_list/a_z_result_bg.png", "touch_list/del.png", "touch_list/item_bg.png",
    "touch_list/list_default_album.png", "touch_list/list_folder.png",
    "touch_list/quality_hr.png", "touch_list/quality_high.png", "touch_list/quality_nomal.png",
    "usb/usb.png",
    "volume/bg.png", "volume/vol.png",
    "category/artist.png", "category/album_artist.png",
    "category/all.png", "category/genre.png",
    "stream_media/subsonic.png", "stream_media/subsonic_s.png",
    "stream_media/radio.png", "stream_media/radio_s.png",
    "stream_media/subsonic_row.png", "stream_media/subsonic_row_s.png",
    "stream_media/radio_row.png", "stream_media/radio_row_s.png",
    "stream_media/podcasts_row.png", "stream_media/podcasts_row_s.png",
    "stream_media/qobuz_row.png", "stream_media/qobuz_row_s.png",
    "stream_media/tidal_row.png", "stream_media/tidal_row_s.png",
    "stream_media/download.png",
    "settings/music.png", "settings/display.png", "settings/power.png",
    "settings/system.png", "settings/about.png",
    "settings/bg_music.png", "settings/bg_display.png", "settings/bg_power.png",
    "settings/bg_system.png", "settings/bg_about.png",
    "submenu/files.png", "submenu/artists.png", "submenu/albums.png",
    "submenu/album_artist.png", "submenu/all_songs.png", "submenu/playlists.png",
    "submenu/favorites.png", "submenu/books.png", "submenu/subsonic.png",
    "submenu/bluetooth.png", "submenu/usb.png",
    "submenu/bg_blue.png", "submenu/bg_coral.png", "submenu/bg_gold.png",
    "submenu/bg_green.png", "submenu/bg_purple.png", "submenu/bg_silver.png",
    "wireless/list_wifi.png", "wireless/list_bt.png", "wireless/list_airplay.png",
    "wireless/list_dlna.png", "wireless/list_remote.png", "wireless/list_import.png"
};

/* Module state */
static bool s_state_loaded = false;
static char s_selected_filename[GUI_THEMES_MAX_NAME] = "";
static bool s_has_imported_legacy = false;

/* Active parsed theme definitions */
static bool s_active_parsed = false;
static theme_file_t s_active_theme;

/* Bounded legacy exclusions */
static gui_theme_legacy_exclusion_t s_exclusions[GUI_THEMES_MAX_EXCLUSIONS];
static size_t s_exclusion_count = 0;

/* UI Picker state */
static lv_obj_t * s_picker_screen = NULL;
static lv_obj_t * s_picker_title = NULL;
static lv_obj_t * s_picker_list = NULL;
static gui_theme_entry_t * s_picker_entries = NULL;
static size_t s_picker_entry_count = 0;

static void populate_theme_picker_grid(lv_obj_t * grid);

#ifdef GUI_THEMES_TEST
void gui_themes_reset_state(void) {
    s_roots_cached = false;
    s_state_loaded = false;
    s_selected_filename[0] = '\0';
    s_has_imported_legacy = false;
    s_active_parsed = false;
    s_exclusion_count = 0;
    gui_themes_teardown();
}
#endif

/* --------------------------------------------------------------------------
 * Path utilities & Strict Validation
 * -------------------------------------------------------------------------- */

static void get_themes_dir(char *out, size_t size) {
    snprintf(out, size, "%s/%s", s_sd_root, NATIVE_THEMES_SUBDIR);
}

static void get_state_file_path(char *out, size_t size) {
    snprintf(out, size, "%s/%s", s_internal_compas_dir, NATIVE_STATE_FILENAME);
}

static void get_legacy_state_path(char *out, size_t size) {
    snprintf(out, size, "%s/%s", s_sd_root, LEGACY_STATE_SUBPATH);
}

static bool is_valid_hex_sha256(const char *sha) {
    if (!sha) return false;
    if (strlen(sha) != GUI_THEMES_SHA256_HEX_LEN) return false;
    for (size_t i = 0; i < GUI_THEMES_SHA256_HEX_LEN; i++) {
        if (!isxdigit((unsigned char)sha[i])) return false;
    }
    return true;
}

static bool is_safe_relative_theme_path(const char *path) {
    if (!path || !path[0]) return false;
    size_t len = strlen(path);
    if (len >= GUI_THEMES_MAX_NAME) return false;
    if (len < 6 || strcmp(path + len - 6, ".theme") != 0) return false;
    if (path[0] == '/') return false;
    if (strstr(path, "..") != NULL) return false;
    if (strstr(path, "\\") != NULL) return false;

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)path[i];
        if (c < 32 || c == 127) return false;
    }
    return true;
}

static bool path_is_under_root(const char *path, const char *root) {
    if (!path || !root) return false;
    size_t root_len = strlen(root);
    if (root_len == 0) return false;
    /* Strip trailing slashes from root_len */
    while (root_len > 1 && root[root_len - 1] == '/') root_len--;

    if (strncmp(path, root, root_len) != 0) return false;
    return (path[root_len] == '\0' || path[root_len] == '/');
}

/* FAT/exFAT directory names are case-insensitive. Check each component. */
static bool contains_private_directory(const char *path) {
    const char *private_dirs[] = {".compas", ".plugins", ".secrets", ".storage", ".open_hiby_player"};
    for (const char *part = path; part && *part; ) {
        while (*part == '/') ++part;
        for (size_t i = 0; i < sizeof(private_dirs) / sizeof(private_dirs[0]); ++i) {
            if (strncasecmp(part, private_dirs[i], strlen(private_dirs[i])) == 0) return true;
        }
        part = strchr(part, '/');
    }
    return false;
}

static void refresh_canonical_roots(void) {
    const char *roots[] = {s_sd_root, s_theme1_root, s_theme2_root};
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); ++i) {
        if (!realpath(roots[i], s_canonical_roots[i])) s_canonical_roots[i][0] = '\0';
    }
    s_roots_cached = true;
}

static bool is_safe_icon_source_path(const char *path) {
    if (!path || !path[0]) return false;
    size_t len = strlen(path);
    if (len >= GUI_THEMES_MAX_PATH) return false;
    if (strstr(path, "..") != NULL) return false;
    if (strstr(path, "\\") != NULL) return false;

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)path[i];
        if (c < 32 || c == 127) return false;
    }

    if (contains_private_directory(path)) return false;

    if (strcmp(path, "theme1") == 0 || strcmp(path, "theme2") == 0) return true;

    /* Absolute paths must strictly be within allowed roots (boundary checked) */
    if (path[0] == '/') {
        char sd_themes[GUI_THEMES_MAX_PATH];
        get_themes_dir(sd_themes, sizeof(sd_themes));

        bool matched_root = false;
        if (path_is_under_root(path, s_theme1_root)) matched_root = true;
        else if (path_is_under_root(path, s_theme2_root)) matched_root = true;
        else if (path_is_under_root(path, sd_themes)) matched_root = true;
        else if (path_is_under_root(path, s_sd_root)) matched_root = true;

        if (!matched_root) return false;

        /* Final-file containment is checked after the asset suffix is appended. */
        return true;
    }

    /* Relative path is allowed (resolved under SD/Themes) */
    return true;
}

/* --------------------------------------------------------------------------
 * Mandatory mbedtls SHA-256 computation
 * -------------------------------------------------------------------------- */

static bool compute_file_sha256(const char *path, char out_hex[GUI_THEMES_SHA256_HEX_LEN + 1], uint64_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    if (mbedtls_sha256_starts(&ctx, 0) != 0) {
        fclose(f);
        mbedtls_sha256_free(&ctx);
        return false;
    }

    uint8_t buf[8192];
    size_t n;
    uint64_t total = 0;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        total += n;
        if (mbedtls_sha256_update(&ctx, buf, n) != 0) {
            ok = false;
            break;
        }
    }
    if (ferror(f)) {
        ok = false;
    }
    fclose(f);

    uint8_t digest[32];
    if (ok && mbedtls_sha256_finish(&ctx, digest) != 0) {
        ok = false;
    }
    mbedtls_sha256_free(&ctx);

    if (!ok) return false;

    for (int i = 0; i < 32; i++) {
        snprintf(out_hex + (i * 2), 3, "%02x", digest[i]);
    }
    out_hex[GUI_THEMES_SHA256_HEX_LEN] = '\0';
    if (out_size) *out_size = total;
    return true;
}

/* Validate the final file, after joining roots and following symlinks. */
static bool canonical_source_is_safe(const char *path) {
    char resolved[PATH_MAX];
    struct stat st;
    if (!realpath(path, resolved) || stat(resolved, &st) != 0 || !S_ISREG(st.st_mode)) return false;
    if (contains_private_directory(resolved)) return false;
    if (!s_roots_cached) refresh_canonical_roots();
    for (size_t i = 0; i < sizeof(s_canonical_roots) / sizeof(s_canonical_roots[0]); ++i) {
        if (path_is_under_root(resolved, s_canonical_roots[i])) return true;
    }
    return false;
}

static bool files_are_identical(const char *path1, const char *path2) {
    if (!canonical_source_is_safe(path2)) return false;
    struct stat st1, st2;
    if (stat(path1, &st1) != 0 || stat(path2, &st2) != 0) return false;
    if (!S_ISREG(st1.st_mode) || !S_ISREG(st2.st_mode)) return false;
    if (st1.st_size != st2.st_size) return false;

    FILE *f1 = fopen(path1, "rb");
    FILE *f2 = fopen(path2, "rb");
    if (!f1 || !f2) {
        if (f1) fclose(f1);
        if (f2) fclose(f2);
        return false;
    }

    char b1[4096], b2[4096];
    bool same = true;
    for (;;) {
        size_t n1 = fread(b1, 1, sizeof(b1), f1);
        size_t n2 = fread(b2, 1, sizeof(b2), f2);
        if (n1 != n2 || memcmp(b1, b2, n1) != 0) {
            same = false;
            break;
        }
        if (n1 == 0) break;
    }
    if (ferror(f1) || ferror(f2)) {
        same = false;
    }
    fclose(f1);
    fclose(f2);
    return same;
}

/* --------------------------------------------------------------------------
 * Atomic State Persistence
 * -------------------------------------------------------------------------- */

static void sync_parent_dir_best_effort(const char *path) {
    char dir_buf[GUI_THEMES_MAX_PATH];
    snprintf(dir_buf, sizeof(dir_buf), "%s", path);
    char *slash = strrchr(dir_buf, '/');
    if (!slash) return;
    *slash = '\0';

    int dfd = open(dir_buf, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
        fsync(dfd);
        close(dfd);
    }
}

static bool save_state_atomic_candidate(const char *candidate_selected,
                                       bool candidate_imported_legacy,
                                       const gui_theme_legacy_exclusion_t *candidate_exclusions,
                                       size_t candidate_exclusion_count) {
    char state_path[GUI_THEMES_MAX_PATH];
    char tmp_path[GUI_THEMES_MAX_PATH + 32];
    get_state_file_path(state_path, sizeof(state_path));
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%ld", state_path, (long)getpid());

    mkdir(s_internal_compas_dir, 0755);

    FILE *f = fopen(tmp_path, "w");
    if (!f) return false;

    bool write_ok = true;
    if (fprintf(f, "# compas native theme state\n") < 0 ||
        fprintf(f, "version=1\n") < 0 ||
        fprintf(f, "imported_legacy=%d\n", candidate_imported_legacy ? 1 : 0) < 0 ||
        fprintf(f, "selected=%s\n", candidate_selected ? candidate_selected : "") < 0 ||
        fprintf(f, "exclusions_count=%zu\n", candidate_exclusion_count) < 0) {
        write_ok = false;
    }

    for (size_t i = 0; i < candidate_exclusion_count && write_ok; i++) {
        if (fprintf(f, "exclusion\t%s\t%llu\t%lld\t%s\n",
                    candidate_exclusions[i].relpath,
                    (unsigned long long)candidate_exclusions[i].size,
                    (long long)candidate_exclusions[i].mtime,
                    candidate_exclusions[i].sha256) < 0) {
            write_ok = false;
        }
    }

    if (!write_ok || ferror(f)) {
        fclose(f);
        unlink(tmp_path);
        return false;
    }

    if (fflush(f) != 0 || fsync(fileno(f)) != 0) {
        fclose(f);
        unlink(tmp_path);
        return false;
    }

    if (fclose(f) != 0) {
        unlink(tmp_path);
        return false;
    }

    /* Rename is the commit point */
    if (rename(tmp_path, state_path) != 0) {
        unlink(tmp_path);
        return false;
    }

    sync_parent_dir_best_effort(state_path);
    s_state_loaded = true;
    return true;
}

static bool load_state(void) {
    char state_path[GUI_THEMES_MAX_PATH];
    get_state_file_path(state_path, sizeof(state_path));

    FILE *f = fopen(state_path, "r");
    if (!f) return false;

    char line[512];
    char staged_selected[GUI_THEMES_MAX_NAME] = "";
    bool staged_imported = false;
    gui_theme_legacy_exclusion_t staged_exclusions[GUI_THEMES_MAX_EXCLUSIONS];
    size_t staged_count = 0;

    int version = 0;
    size_t declared_exclusions_count = 0;
    bool has_version = false, has_imported = false, has_selected = false, has_count = false;

    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        if (len == 0) continue;
        /* Line without newline that doesn't reach EOF is truncated */
        if (line[len - 1] != '\n' && !feof(f)) {
            fclose(f);
            return false;
        }
        while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) {
            line[--len] = '\0';
        }
        if (line[0] == '#' || line[0] == '\0') continue;

        if (strncmp(line, "version=", 8) == 0) {
            version = atoi(line + 8);
            if (version != 1) {
                fclose(f);
                return false;
            }
            has_version = true;
        } else if (strncmp(line, "selected=", 9) == 0) {
            const char *sel = line + 9;
            if (sel[0] != '\0' && !is_safe_relative_theme_path(sel)) {
                fclose(f);
                return false;
            }
            snprintf(staged_selected, sizeof(staged_selected), "%s", sel);
            has_selected = true;
        } else if (strncmp(line, "imported_legacy=", 16) == 0) {
            const char *val = line + 16;
            if (strcmp(val, "0") != 0 && strcmp(val, "1") != 0) {
                fclose(f);
                return false;
            }
            staged_imported = (atoi(val) != 0);
            has_imported = true;
        } else if (strncmp(line, "exclusions_count=", 17) == 0) {
            char *endptr = NULL;
            errno = 0;
            unsigned long cnt = strtoul(line + 17, &endptr, 10);
            if (errno != 0 || endptr == line + 17 || *endptr != '\0' || cnt > GUI_THEMES_MAX_EXCLUSIONS) {
                fclose(f);
                return false;
            }
            declared_exclusions_count = (size_t)cnt;
            has_count = true;
        } else if (strncmp(line, "exclusion\t", 10) == 0) {
            if (staged_count >= GUI_THEMES_MAX_EXCLUSIONS) {
                fclose(f);
                return false;
            }
            char relpath[GUI_THEMES_MAX_PATH];
            unsigned long long sz = 0;
            long long mt = 0;
            char sha[GUI_THEMES_SHA256_HEX_LEN + 1];

            if (sscanf(line + 10, "%255[^\t]\t%llu\t%lld\t%64s", relpath, &sz, &mt, sha) != 4) {
                fclose(f);
                return false;
            }
            if (relpath[0] == '/' || !is_safe_icon_source_path(relpath) || !is_valid_hex_sha256(sha)) {
                fclose(f);
                return false;
            }
            /* Duplicate check */
            for (size_t i = 0; i < staged_count; i++) {
                if (strcmp(staged_exclusions[i].relpath, relpath) == 0) {
                    fclose(f);
                    return false;
                }
            }

            snprintf(staged_exclusions[staged_count].relpath, sizeof(staged_exclusions[staged_count].relpath), "%s", relpath);
            staged_exclusions[staged_count].size = (uint64_t)sz;
            staged_exclusions[staged_count].mtime = (int64_t)mt;
            snprintf(staged_exclusions[staged_count].sha256, sizeof(staged_exclusions[staged_count].sha256), "%s", sha);
            staged_count++;
        } else {
            /* Unexpected entry */
            fclose(f);
            return false;
        }
    }

    if (ferror(f)) {
        fclose(f);
        return false;
    }
    fclose(f);

    if (!has_version || !has_imported || !has_selected || !has_count || staged_count != declared_exclusions_count) {
        return false;
    }

    /* Valid state confirmed: now prune stale exclusions without rejecting the state */
    gui_theme_legacy_exclusion_t valid_exclusions[GUI_THEMES_MAX_EXCLUSIONS];
    size_t valid_count = 0;
    for (size_t i = 0; i < staged_count; i++) {
        char override_path[GUI_THEMES_MAX_PATH];
        snprintf(override_path, sizeof(override_path), "%s%s", s_override_root, staged_exclusions[i].relpath);

        char actual_sha[GUI_THEMES_SHA256_HEX_LEN + 1];
        uint64_t actual_size = 0;
        if (compute_file_sha256(override_path, actual_sha, &actual_size) &&
            actual_size == staged_exclusions[i].size &&
            strcmp(actual_sha, staged_exclusions[i].sha256) == 0) {
            valid_exclusions[valid_count++] = staged_exclusions[i];
        }
    }

    /* Commit staged candidate to globals */
    snprintf(s_selected_filename, sizeof(s_selected_filename), "%s", staged_selected);
    s_has_imported_legacy = staged_imported;
    memcpy(s_exclusions, valid_exclusions, valid_count * sizeof(gui_theme_legacy_exclusion_t));
    s_exclusion_count = valid_count;
    s_state_loaded = true;
    return true;
}

/* --------------------------------------------------------------------------
 * One-time Legacy Themes Migration
 * -------------------------------------------------------------------------- */

/* One bounded resolver is shared by native drawing and legacy comparisons. */
static bool resolve_theme_source(const char *source, const char *relpath, bool root,
                                 char *out, size_t size) {
    if (!source[0] || !is_safe_icon_source_path(source)) return false;
    int n;
    if (strcmp(source, "theme1") == 0 || strcmp(source, "theme2") == 0) {
        const char *base = strcmp(source, "theme1") == 0 ? s_theme1_root : s_theme2_root;
        n = snprintf(out, size, "%s%s", base, relpath);
    } else if (source[0] == '/') {
        n = root ? snprintf(out, size, "%s/%s", source, relpath)
                 : snprintf(out, size, "%s", source);
    } else {
        char themes_dir[GUI_THEMES_MAX_PATH];
        get_themes_dir(themes_dir, sizeof(themes_dir));
        n = root ? snprintf(out, size, "%s/%s/%s", themes_dir, source, relpath)
                 : snprintf(out, size, "%s/%s", themes_dir, source);
    }
    return n >= 0 && (size_t)n < size && canonical_source_is_safe(out) && access(out, R_OK) == 0;
}

static bool resolve_theme_icon(const theme_file_t *theme, const char *relpath,
                                              char *out, size_t size) {
    for (size_t i = 0; i < theme->icon_override_count; ++i) {
        if (strcmp(theme->icon_overrides[i].relpath, relpath) == 0 &&
            resolve_theme_source(theme->icon_overrides[i].source, relpath, false, out, size)) return true;
    }
    if (strncmp(relpath, "pull_down/", 10) == 0 &&
        resolve_theme_source(theme->drawer_root, relpath, true, out, size)) return true;
    if (strcmp(theme->icon_root, "theme2") != 0 &&
        resolve_theme_source(theme->icon_root, relpath, true, out, size)) return true;
    return false;
}

static void perform_legacy_migration_if_needed(void) {
    if (s_has_imported_legacy) return;
#ifndef HOST_BUILD
    if (!sd_card_root_is_mounted()) return;
#endif

    /* Check SD availability */
    struct stat st_sd;
    if (stat(s_sd_root, &st_sd) != 0 || !S_ISDIR(st_sd.st_mode)) {
        /* Defer migration until SD card is mounted/available */
        return;
    }

    char legacy_path[GUI_THEMES_MAX_PATH];
    get_legacy_state_path(legacy_path, sizeof(legacy_path));

    /* Legacy migration requires either legacy state file or known loader evidence */
    bool has_legacy_state = (access(legacy_path, R_OK) == 0);
    char loader_path[GUI_THEMES_MAX_PATH];
    snprintf(loader_path, sizeof(loader_path), "%s/.plugins/Themes.lua", s_sd_root);
    bool has_loader = (access(loader_path, R_OK) == 0);

    if (!has_legacy_state && !has_loader) {
        /* No evidence of legacy Themes loader: save imported marker */
        if (save_state_atomic_candidate(s_selected_filename, true, s_exclusions, s_exclusion_count)) {
            s_has_imported_legacy = true;
        }
        return;
    }

    char imported_theme[GUI_THEMES_MAX_NAME] = "";
    if (has_legacy_state) {
        FILE *f = fopen(legacy_path, "r");
        if (f) {
            if (fgets(imported_theme, sizeof(imported_theme), f)) {
                size_t len = strlen(imported_theme);
                while (len > 0 && (imported_theme[len - 1] == '\r' || imported_theme[len - 1] == '\n')) {
                    imported_theme[--len] = '\0';
                }
            }
            fclose(f);
        }
    }

    char candidate_selected[GUI_THEMES_MAX_NAME];
    snprintf(candidate_selected, sizeof(candidate_selected), "%s", s_selected_filename);
    if (candidate_selected[0] == '\0' && imported_theme[0] != '\0' && is_safe_relative_theme_path(imported_theme)) {
        snprintf(candidate_selected, sizeof(candidate_selected), "%s", imported_theme);
    }

    gui_theme_legacy_exclusion_t candidate_exclusions[GUI_THEMES_MAX_EXCLUSIONS];
    memcpy(candidate_exclusions, s_exclusions, s_exclusion_count * sizeof(gui_theme_legacy_exclusion_t));
    size_t candidate_count = s_exclusion_count;

    char themes_dir[GUI_THEMES_MAX_PATH];
    get_themes_dir(themes_dir, sizeof(themes_dir));

    theme_file_t legacy_theme;
    bool has_legacy_theme = false;
    /* Strict validation BEFORE constructing path or calling theme_file_parse */
    if (imported_theme[0] != '\0' && is_safe_relative_theme_path(imported_theme)) {
        char theme_path[GUI_THEMES_MAX_PATH];
        snprintf(theme_path, sizeof(theme_path), "%s/%s", themes_dir, imported_theme);
        if (theme_file_parse(theme_path, &legacy_theme, NULL, 0)) {
            has_legacy_theme = true;
        }
    }

    /* Check fixed inventory */
    size_t inventory_count = sizeof(s_legacy_asset_inventory) / sizeof(s_legacy_asset_inventory[0]);
    for (size_t i = 0; i < inventory_count && candidate_count < GUI_THEMES_MAX_EXCLUSIONS; i++) {
        const char *rel = s_legacy_asset_inventory[i];
        char override_file[GUI_THEMES_MAX_PATH];
        snprintf(override_file, sizeof(override_file), "%s%s", s_override_root, rel);

        struct stat st;
        if (stat(override_file, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }

        /* Check stock theme2 and stock theme1 */
        char stock_theme2[GUI_THEMES_MAX_PATH], stock_theme1[GUI_THEMES_MAX_PATH];
        snprintf(stock_theme2, sizeof(stock_theme2), "%s%s", s_theme2_root, rel);
        snprintf(stock_theme1, sizeof(stock_theme1), "%s%s", s_theme1_root, rel);

        bool matches_legacy = files_are_identical(override_file, stock_theme2) ||
                              files_are_identical(override_file, stock_theme1);

        /* Check expected legacy theme source via theme resolver */
        if (!matches_legacy && has_legacy_theme) {
            char expected[GUI_THEMES_MAX_PATH];
            if (resolve_theme_icon(&legacy_theme, rel, expected, sizeof(expected))) {
                if (files_are_identical(override_file, expected)) {
                    matches_legacy = true;
                }
            }
        }

        if (matches_legacy) {
            /* De-duplicate against candidate_exclusions */
            bool already_exists = false;
            for (size_t k = 0; k < candidate_count; k++) {
                if (strcmp(candidate_exclusions[k].relpath, rel) == 0) {
                    already_exists = true;
                    break;
                }
            }
            if (!already_exists) {
                char sha[GUI_THEMES_SHA256_HEX_LEN + 1];
                uint64_t fsize = 0;
                if (compute_file_sha256(override_file, sha, &fsize)) {
                    snprintf(candidate_exclusions[candidate_count].relpath, sizeof(candidate_exclusions[candidate_count].relpath), "%s", rel);
                    candidate_exclusions[candidate_count].size = fsize;
                    candidate_exclusions[candidate_count].mtime = (int64_t)st.st_mtime;
                    snprintf(candidate_exclusions[candidate_count].sha256, sizeof(candidate_exclusions[candidate_count].sha256), "%s", sha);
                    candidate_count++;
                }
            }
        }
    }

    /* Also check any extra explicit icon override paths from legacy theme outside inventory */
    if (has_legacy_theme) {
        for (size_t i = 0; i < legacy_theme.icon_override_count && candidate_count < GUI_THEMES_MAX_EXCLUSIONS; i++) {
            const char *rel = legacy_theme.icon_overrides[i].relpath;
            char override_file[GUI_THEMES_MAX_PATH];
            snprintf(override_file, sizeof(override_file), "%s%s", s_override_root, rel);

            struct stat st;
            if (stat(override_file, &st) != 0 || !S_ISREG(st.st_mode)) continue;

            char expected[GUI_THEMES_MAX_PATH];
            if (resolve_theme_icon(&legacy_theme, rel, expected, sizeof(expected))) {
                if (files_are_identical(override_file, expected)) {
                    bool already_exists = false;
                    for (size_t k = 0; k < candidate_count; k++) {
                        if (strcmp(candidate_exclusions[k].relpath, rel) == 0) {
                            already_exists = true;
                            break;
                        }
                    }
                    if (!already_exists) {
                        char sha[GUI_THEMES_SHA256_HEX_LEN + 1];
                        uint64_t fsize = 0;
                        if (compute_file_sha256(override_file, sha, &fsize)) {
                            snprintf(candidate_exclusions[candidate_count].relpath, sizeof(candidate_exclusions[candidate_count].relpath), "%s", rel);
                            candidate_exclusions[candidate_count].size = fsize;
                            candidate_exclusions[candidate_count].mtime = (int64_t)st.st_mtime;
                            snprintf(candidate_exclusions[candidate_count].sha256, sizeof(candidate_exclusions[candidate_count].sha256), "%s", sha);
                            candidate_count++;
                        }
                    }
                }
            }
        }
    }

    if (save_state_atomic_candidate(candidate_selected, true, candidate_exclusions, candidate_count)) {
        snprintf(s_selected_filename, sizeof(s_selected_filename), "%s", candidate_selected);
        s_has_imported_legacy = true;
        memcpy(s_exclusions, candidate_exclusions, candidate_count * sizeof(gui_theme_legacy_exclusion_t));
        s_exclusion_count = candidate_count;
    }
}

/* --------------------------------------------------------------------------
 * Directory Scanning
 * -------------------------------------------------------------------------- */

static int compare_theme_entries(const void *a, const void *b) {
    const gui_theme_entry_t *ea = (const gui_theme_entry_t *)a;
    const gui_theme_entry_t *eb = (const gui_theme_entry_t *)b;
    if (ea->is_default) return -1;
    if (eb->is_default) return 1;
    return strcmp(ea->filename, eb->filename);
}

static void scan_dir_recursive(const char *base_dir, const char *sub_prefix,
                               gui_theme_entry_t *out, size_t max_entries,
                               size_t *count, int depth) {
    if (depth > 2 || *count >= max_entries) return;

    DIR *d = opendir(base_dir);
    if (!d) return;

    struct dirent *de;
    while ((de = readdir(d)) != NULL && *count < max_entries) {
        if (de->d_name[0] == '.') continue;

        char fullpath[GUI_THEMES_MAX_PATH];
        if (snprintf(fullpath, sizeof(fullpath), "%s/%s", base_dir, de->d_name) >= (int)sizeof(fullpath)) {
            continue;
        }

        struct stat st;
        if (stat(fullpath, &st) != 0) continue;

        if (S_ISDIR(st.st_mode) && depth < 2) {
            char next_prefix[GUI_THEMES_MAX_NAME];
            int n = (sub_prefix && sub_prefix[0])
                ? snprintf(next_prefix, sizeof(next_prefix), "%s/%s", sub_prefix, de->d_name)
                : snprintf(next_prefix, sizeof(next_prefix), "%s", de->d_name);
            if (n < (int)sizeof(next_prefix)) {
                scan_dir_recursive(fullpath, next_prefix, out, max_entries, count, depth + 1);
            }
        } else if (S_ISREG(st.st_mode)) {
            size_t name_len = strlen(de->d_name);
            if (name_len > 6 && strcmp(de->d_name + name_len - 6, ".theme") == 0) {
                theme_file_t tf;
                char err[128];
                if (theme_file_parse(fullpath, &tf, err, sizeof(err))) {
                    gui_theme_entry_t *entry = &out[*count];
                    int fn_len = (sub_prefix && sub_prefix[0])
                        ? snprintf(entry->filename, sizeof(entry->filename), "%s/%s", sub_prefix, de->d_name)
                        : snprintf(entry->filename, sizeof(entry->filename), "%s", de->d_name);
                    if (fn_len >= (int)sizeof(entry->filename) ||
                        !is_safe_relative_theme_path(entry->filename)) continue;

                    snprintf(entry->name, sizeof(entry->name), "%s", tf.name[0] ? tf.name : de->d_name);
                    snprintf(entry->fullpath, sizeof(entry->fullpath), "%s", fullpath);
                    entry->is_default = false;
                    entry->preview_color = tf.card ? tf.card : tf.screen;
                    (*count)++;
                }
            }
        }
    }
    closedir(d);
}

size_t gui_themes_scan(gui_theme_entry_t *out_entries, size_t max_entries) {
    if (!out_entries || max_entries == 0) return 0;

    size_t count = 0;

    /* Entry 0 is ALWAYS Default */
    gui_theme_entry_t *def = &out_entries[count++];
    snprintf(def->filename, sizeof(def->filename), "");
    snprintf(def->name, sizeof(def->name), "%s", TR("Default"));
    def->fullpath[0] = '\0';
    def->is_default = true;
    def->preview_color = GUI_COLOR_PANEL;

    char themes_dir[GUI_THEMES_MAX_PATH];
    get_themes_dir(themes_dir, sizeof(themes_dir));

    scan_dir_recursive(themes_dir, "", out_entries, max_entries, &count, 0);

    /* Stable sort filenames (excluding entry 0) */
    if (count > 2) {
        qsort(&out_entries[1], count - 1, sizeof(gui_theme_entry_t), compare_theme_entries);
    }

    return count;
}

/* --------------------------------------------------------------------------
 * Public Query & Selection
 * -------------------------------------------------------------------------- */

const char * gui_themes_get_selected(void) {
    return s_selected_filename;
}

bool gui_themes_select(const char *filename) {
    if (!filename || filename[0] == '\0') {
        /* An explicit choice is authoritative even with the SD card absent.
         * Suppress future legacy selection import; overrides without ownership
         * evidence remain untouched, as they may belong to independent plugins. */
        if (!save_state_atomic_candidate("", true, s_exclusions, s_exclusion_count)) {
            return false;
        }
        s_selected_filename[0] = '\0';
        s_has_imported_legacy = true;
        s_active_parsed = false;
        gui_reload_request();
        return true;
    }

    if (!is_safe_relative_theme_path(filename)) return false;

    char themes_dir[GUI_THEMES_MAX_PATH];
    char fullpath[GUI_THEMES_MAX_PATH];
    get_themes_dir(themes_dir, sizeof(themes_dir));
    if (snprintf(fullpath, sizeof(fullpath), "%s/%s", themes_dir, filename) >= (int)sizeof(fullpath)) {
        return false;
    }

    theme_file_t candidate;
    char err[128];
    if (!theme_file_parse(fullpath, &candidate, err, sizeof(err))) {
        return false;
    }

    /* Candidate transactional persistence: sets imported_legacy true */
    if (!save_state_atomic_candidate(filename, true, s_exclusions, s_exclusion_count)) {
        return false;
    }

    snprintf(s_selected_filename, sizeof(s_selected_filename), "%s", filename);
    s_has_imported_legacy = true;
    s_active_theme = candidate;
    s_active_parsed = true;
    gui_reload_request();
    return true;
}

bool gui_themes_is_legacy_theme_loader(const char *plugin_id, const char *filename) {
    if (plugin_id && (strcmp(plugin_id, "example.themes") == 0 ||
                      strcmp(plugin_id, "legacy.Themes") == 0 ||
                      strcmp(plugin_id, "legacy.Themes.lua") == 0)) {
        return true;
    }
    if (filename && (strcmp(filename, "Themes.lua") == 0 ||
                     strstr(filename, "Themes--Themes.lua") != NULL)) {
        return true;
    }
    return false;
}

bool gui_themes_is_legacy_excluded(const char *relpath) {
    if (!relpath || !relpath[0] || s_exclusion_count == 0) return false;

    for (size_t i = 0; i < s_exclusion_count; i++) {
        if (strcmp(s_exclusions[i].relpath, relpath) == 0) {
            char override_path[GUI_THEMES_MAX_PATH];
            snprintf(override_path, sizeof(override_path), "%s%s", s_override_root, relpath);

            struct stat st;
            if (stat(override_path, &st) != 0 || !S_ISREG(st.st_mode) ||
                (uint64_t)st.st_size != s_exclusions[i].size ||
                (int64_t)st.st_mtime != s_exclusions[i].mtime) {
                /* Clear immediately even if the persistent volume is read-only. */
                gui_themes_clear_legacy_exclusion(relpath);
                return false;
            }

            return true;
        }
    }
    return false;
}

void gui_themes_clear_legacy_exclusion(const char *relpath) {
    if (!relpath || !relpath[0] || s_exclusion_count == 0) return;

    /* Find all matching exclusion records and filter them out in memory immediately */
    gui_theme_legacy_exclusion_t remaining[GUI_THEMES_MAX_EXCLUSIONS];
    size_t remaining_count = 0;
    bool found = false;

    for (size_t i = 0; i < s_exclusion_count; i++) {
        if (strcmp(s_exclusions[i].relpath, relpath) == 0) {
            found = true;
        } else {
            if (remaining_count < GUI_THEMES_MAX_EXCLUSIONS) {
                remaining[remaining_count++] = s_exclusions[i];
            }
        }
    }

    if (!found) return;

    /* Immediately invalidate from in-memory state so newly set plugin icon is never hidden */
    memcpy(s_exclusions, remaining, remaining_count * sizeof(gui_theme_legacy_exclusion_t));
    s_exclusion_count = remaining_count;

    /* Attempt to persist updated state atomically; if saving fails, state in memory stays cleared */
    save_state_atomic_candidate(s_selected_filename, s_has_imported_legacy, s_exclusions, s_exclusion_count);
}

/* --------------------------------------------------------------------------
 * Icon Layer Resolution
 * -------------------------------------------------------------------------- */

bool gui_themes_resolve_icon_layer(const char *relpath, char *out_path, size_t out_size) {
    if (!relpath || !relpath[0] || !out_path || !out_size || !s_active_parsed) return false;
    return resolve_theme_icon(&s_active_theme, relpath, out_path, out_size);
}

/* --------------------------------------------------------------------------
 * Native Theme Apply (Boot & Full Reload)
 * -------------------------------------------------------------------------- */

void gui_themes_apply_active(void) {
    refresh_canonical_roots();
    if (s_selected_filename[0] == '\0') {
        /* Default preserves fresh plugin baseline home, launcher, and colors */
        s_active_parsed = false;
        return;
    }

    char themes_dir[GUI_THEMES_MAX_PATH];
    char fullpath[GUI_THEMES_MAX_PATH];
    get_themes_dir(themes_dir, sizeof(themes_dir));
    snprintf(fullpath, sizeof(fullpath), "%s/%s", themes_dir, s_selected_filename);

    theme_file_t tf;
    char err[128];
    if (!theme_file_parse(fullpath, &tf, err, sizeof(err))) {
        s_active_parsed = false;
        return;
    }

    /* Home Layout Overlay & Blending */
    home_layout_config_t blended = home_layout_config;
    if (tf.has_home) {
        blended.configured = true;

        if (tf.has_home_mode) {
            blended.list_mode = tf.home.list_mode;
        }
        if (tf.has_home_tile_gap) {
            blended.tile_gap = tf.home.tile_gap;
        }
        if (tf.has_home_row_gap) {
            blended.row_gap = tf.home.row_gap;
        }
        if (tf.has_home_order) {
            blended.order_count = tf.home.order_count;
            for (int i = 0; i < tf.home.order_count; i++) {
                snprintf(blended.order[i], sizeof(blended.order[i]), "%s", tf.home.order[i]);
            }
        }

        /* Check tile mode order constraint */
        if (!blended.list_mode && blended.order_count > 6) {
            /* Incompatible blend produces tilemode order > 6: reject theme apply safely */
            s_active_parsed = false;
            return;
        }

        /* Merge per-tile overrides individually */
        for (int i = 0; i < tf.home.tile_count; i++) {
            const char *key = tf.home.tiles[i].key;
            const home_tile_override_t *src_ov = &tf.home.tiles[i].override;

            int existing_idx = -1;
            for (int j = 0; j < blended.tile_count; j++) {
                if (strcmp(blended.tiles[j].key, key) == 0) {
                    existing_idx = j;
                    break;
                }
            }

            home_tile_override_t *target_ov = NULL;
            if (existing_idx >= 0) {
                target_ov = &blended.tiles[existing_idx].override;
            } else if (blended.tile_count < HOME_LAYOUT_MAX_TILES) {
                snprintf(blended.tiles[blended.tile_count].key, sizeof(blended.tiles[blended.tile_count].key), "%s", key);
                target_ov = &blended.tiles[blended.tile_count].override;
                blended.tile_count++;
            }

            if (target_ov) {
                if (src_ov->has_bg_color) {
                    target_ov->has_bg_color = true;
                    target_ov->bg_color = src_ov->bg_color;
                }
                if (src_ov->has_text_color) {
                    target_ov->has_text_color = true;
                    target_ov->text_color = src_ov->text_color;
                }
                if (src_ov->has_radius) {
                    target_ov->has_radius = true;
                    target_ov->radius = src_ov->radius;
                }
                if (src_ov->height != 0) {
                    target_ov->height = src_ov->height;
                }
                if (src_ov->width != 0) {
                    target_ov->width = src_ov->width;
                }
                if (src_ov->align[0] != '\0') {
                    snprintf(target_ov->align, sizeof(target_ov->align), "%s", src_ov->align);
                }
                if (src_ov->has_accessory) {
                    target_ov->has_accessory = true;
                    target_ov->accessory = src_ov->accessory;
                }
                if (src_ov->text_size[0] != '\0') {
                    snprintf(target_ov->text_size, sizeof(target_ov->text_size), "%s", src_ov->text_size);
                }
                if (src_ov->has_icon) {
                    target_ov->has_icon = true;
                    target_ov->icon = src_ov->icon;
                }
            }
        }

        /* Preserve HomeBackground image if theme omits it */
        if (tf.home.has_background_image) {
            blended.has_background_image = true;
            snprintf(blended.background_image, sizeof(blended.background_image), "%s", tf.home.background_image);
        }
    }

    /* Launcher Layout Overlay */
    launcher_layout_config_t blended_launcher = launcher_layout_config;
    if (tf.has_launcher && tf.has_submenu_mode) {
        blended_launcher.music = tf.launcher.music;
        blended_launcher.stream_media = tf.launcher.stream_media;
        blended_launcher.wireless = tf.launcher.wireless;
    }

    s_active_theme = tf;
    s_active_parsed = true;

    /* Apply theme colors */
    gui_plugin_set_background_color("screen", tf.screen);
    gui_plugin_set_background_color("card", tf.card);
    gui_plugin_set_background_color("list_row", tf.list_row);
    gui_plugin_set_text_color("primary", tf.text_primary);
    gui_plugin_set_text_color("muted", tf.text_muted);

    if (tf.has_home) {
        gui_plugin_set_home_layout(&blended);
    }
    if (tf.has_launcher && tf.has_submenu_mode) {
        gui_plugin_set_launcher_layout(&blended_launcher);
    }
}

/* --------------------------------------------------------------------------
 * Initialization
 * -------------------------------------------------------------------------- */

void gui_themes_init(void) {
    refresh_canonical_roots();
    if (!s_state_loaded) {
        char state_path[GUI_THEMES_MAX_PATH];
        get_state_file_path(state_path, sizeof(state_path));
        if (access(state_path, F_OK) == 0) {
            if (!load_state()) {
                /* Corrupt state file exists: do NOT run migration or overwrite corrupt state */
                return;
            }
        }
    }
    /* Attempt legacy migration if not yet imported (e.g. SD mounted now) */
    perform_legacy_migration_if_needed();
}

/* --------------------------------------------------------------------------
 * UI Picker Implementation
 * -------------------------------------------------------------------------- */

static void theme_card_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (gui_plugin_store_operation_active()) return;

    size_t idx = (size_t)(intptr_t)lv_event_get_user_data(e);
    if (!s_picker_entries || idx >= s_picker_entry_count) return;

    if (!gui_themes_select(s_picker_entries[idx].filename)) {
        show_error_toast(TR("Could not apply theme"));
    }
}

static void download_row_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (gui_plugin_store_operation_active()) return;

    gui_plugin_store_open_themes();
}

static void picker_screen_loaded_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_SCREEN_LOADED) {
        if (s_picker_list) {
            populate_theme_picker_grid(s_picker_list);
        }
    }
}

static void populate_theme_picker_grid(lv_obj_t * list) {
    if (!list) return;

    lv_obj_clean(list);

    if (s_picker_entries) {
        free(s_picker_entries);
        s_picker_entries = NULL;
    }

    size_t cap = 128;
    s_picker_entries = calloc(cap, sizeof(gui_theme_entry_t));
    if (!s_picker_entries) return;

    s_picker_entry_count = gui_themes_scan(s_picker_entries, cap);

    lv_obj_t * download = add_pill_chevron_row(list, TR("Download"), download_row_click_cb);
    lv_obj_set_width(download, lv_pct(100));

    lv_obj_t * grid = lv_obj_create(list);
    lv_obj_remove_style_all(grid);
    lv_obj_set_width(grid, lv_pct(100));
    lv_obj_set_height(grid, LV_SIZE_CONTENT);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    configure_cover_card_grid(grid, 2);
    /* Preserve the full selection ring around the grid's outer cards. */
    lv_obj_set_style_pad_top(grid, BOARD_SCALE_PX(6), 0);
    lv_obj_set_style_pad_bottom(grid, BOARD_SCALE_PX(6), 0);

    for (size_t i = 0; i < s_picker_entry_count; i++) {
        gui_theme_entry_t *entry = &s_picker_entries[i];
        bool is_selected = (strcmp(entry->filename, s_selected_filename) == 0);

        char preview_resolved[GUI_THEMES_MAX_PATH + 8];
        const char *img_src = NULL;

        if (!entry->is_default && entry->fullpath[0]) {
            char preview_path[GUI_THEMES_MAX_PATH];
            snprintf(preview_path, sizeof(preview_path), "%s", entry->fullpath);
            char *dot = strrchr(preview_path, '.');
            if (dot) {
                snprintf(dot, sizeof(preview_path) - (size_t)(dot - preview_path), ".png");
                if (canonical_source_is_safe(preview_path) && access(preview_path, R_OK) == 0) {
                    snprintf(preview_resolved, sizeof(preview_resolved), "S:%s", preview_path);
                    img_src = preview_resolved;
                }
            }
        }

        lv_obj_t * card = add_cover_card(grid, entry->name, img_src, 2,
                                         theme_card_click_cb, (void *)(intptr_t)i);
        if (!card) continue;

        /* Swatch fallback styling on cover child if no preview image */
        if (!img_src && lv_obj_get_child_count(card) > 0) {
            lv_obj_t * cover = lv_obj_get_child(card, 0);
            if (cover) {
                lv_obj_set_style_bg_color(cover, lv_color_hex(entry->preview_color), 0);
                lv_obj_set_style_bg_opa(cover, LV_OPA_COVER, 0);
            }
        }

        lv_obj_set_style_radius(card, BOARD_SCALE_PX(10), 0);
        lv_obj_set_style_outline_width(card, is_selected ? BOARD_SCALE_PX(2) : 0, 0);
        lv_obj_set_style_outline_color(card, accent_lv_color(), 0);
        lv_obj_set_style_outline_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_outline_pad(card, BOARD_SCALE_PX(2), 0);
    }
}

void gui_themes_open_picker(void) {
    /* Initialize service and retry deferred migration if SD card was inserted after boot */
    gui_themes_init();

    if (!s_picker_screen) {
        s_picker_screen = build_subsonic_list_screen(TR("Themes"), &s_picker_title, &s_picker_list);
        if (s_picker_screen) {
            lv_obj_add_event_cb(s_picker_screen, picker_screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
        }
    }

    if (s_picker_screen) nav_push(s_picker_screen);
}

void gui_themes_teardown(void) {
    if (s_picker_screen) {
        if (gui_navigation_contains(s_picker_screen)) {
            lv_obj_t *scr = s_picker_screen;
            gui_navigation_remove_screen_instances(&scr, 1);
        }
        lv_obj_delete(s_picker_screen);
        s_picker_screen = NULL;
        s_picker_title = NULL;
        s_picker_list = NULL;
    }

    if (s_picker_entries) {
        free(s_picker_entries);
        s_picker_entries = NULL;
        s_picker_entry_count = 0;
    }
}
