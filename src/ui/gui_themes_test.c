#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/stat.h>
#include <utime.h>

#include <lvgl.h>
#include "src/ui/gui_themes.h"
#include "src/ui/gui_theme.h"
#include "src/core/theme_file.h"
#include "src/core/storage_paths.h"
#include "src/ui/i18n.h"

static uint32_t mock_bg_screen = 0;
static uint32_t mock_bg_card = 0;
static uint32_t mock_bg_row = 0;
static uint32_t mock_text_primary = 0;
static uint32_t mock_text_muted = 0;
static int mock_reload_requests = 0;

void gui_plugin_set_background_color(const char * slot, uint32_t rgb) {
    if (strcmp(slot, "screen") == 0) mock_bg_screen = rgb;
    else if (strcmp(slot, "card") == 0) mock_bg_card = rgb;
    else if (strcmp(slot, "list_row") == 0) mock_bg_row = rgb;
}

void gui_plugin_set_text_color(const char * slot, uint32_t rgb) {
    if (strcmp(slot, "primary") == 0) mock_text_primary = rgb;
    else if (strcmp(slot, "muted") == 0) mock_text_muted = rgb;
}

home_layout_config_t home_layout_config = { 0 };

void gui_plugin_set_home_layout(const home_layout_config_t * config) {
    home_layout_config = *config;
}

void gui_plugin_reset_home_layout(void) {
    home_layout_config = (home_layout_config_t){ 0 };
}

launcher_layout_config_t launcher_layout_config = { 0 };

void gui_plugin_set_launcher_layout(const launcher_layout_config_t * config) {
    launcher_layout_config = *config;
}

void gui_plugin_reset_launcher_layout(void) {
    launcher_layout_config = (launcher_layout_config_t){ 0 };
}

void show_error_toast(const char *message) { (void)message; }

void gui_reload_request(void) {
    mock_reload_requests++;
}

bool gui_plugin_store_open_themes(void) {
    return true;
}

bool gui_plugin_store_operation_active(void) {
    return false;
}

/* Mock screen builders & navigation */
lv_color_t accent_lv_color(void) { return lv_color_hex(0x00FF00); }
void configure_cover_card_grid(lv_obj_t * list, int columns) { (void)list; (void)columns; }
lv_obj_t * add_cover_card(lv_obj_t * list, const char * label, const char * resolved_image_src, int columns, lv_event_cb_t on_click, void * user_data) {
    (void)list; (void)label; (void)resolved_image_src; (void)columns; (void)on_click; (void)user_data;
    return (lv_obj_t *)(intptr_t)1;
}
lv_obj_t * build_subsonic_list_screen(const char * default_title, lv_obj_t ** out_title_label, lv_obj_t ** out_list) {
    (void)default_title; (void)out_title_label; (void)out_list;
    return (lv_obj_t *)(intptr_t)2;
}
uint32_t gui_anim_ms(uint32_t base_ms) { return base_ms; }
bool gui_navigation_contains(lv_obj_t * screen) { (void)screen; return false; }
void gui_navigation_remove_screen_instances(lv_obj_t ** screens, int count) { (void)screens; (void)count; }
void nav_push(lv_obj_t * scr) { (void)scr; }

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "wb");
    assert(f != NULL);
    if (content) fputs(content, f);
    fclose(f);
}

int main(void) {
    printf("Starting comprehensive isolated gui_themes unit tests...\n");

    /* 1. Setup isolated temporary directory in build_ui_test */
    char tmpl[] = "build_ui_test/theme-test-XXXXXX";
    mkdir("build_ui_test", 0755);
    char *scratch_dir = mkdtemp(tmpl);
    assert(scratch_dir != NULL);

    char sd_root[512], compas_dir[512], override_dir[512], theme1_dir[512], theme2_dir[512];
    snprintf(sd_root, sizeof(sd_root), "%s/sd", scratch_dir);
    snprintf(compas_dir, sizeof(compas_dir), "%s/compas", scratch_dir);
    snprintf(override_dir, sizeof(override_dir), "%s/override/", scratch_dir);
    snprintf(theme1_dir, sizeof(theme1_dir), "%s/theme1/", scratch_dir);
    snprintf(theme2_dir, sizeof(theme2_dir), "%s/theme2/", scratch_dir);

    mkdir(sd_root, 0755);
    mkdir(compas_dir, 0755);
    mkdir(override_dir, 0755);
    mkdir(theme1_dir, 0755);
    mkdir(theme2_dir, 0755);

    char sd_themes[512], sd_plugins[512];
    snprintf(sd_themes, sizeof(sd_themes), "%s/Themes", sd_root);
    snprintf(sd_plugins, sizeof(sd_plugins), "%s/.plugins", sd_root);
    mkdir(sd_themes, 0755);
    mkdir(sd_plugins, 0755);

    gui_themes_set_test_roots(sd_root, compas_dir, override_dir, theme1_dir, theme2_dir);
    gui_themes_reset_state();

    /* 2. Legacy loader identity */
    assert(gui_themes_is_legacy_theme_loader("example.themes", NULL));
    assert(gui_themes_is_legacy_theme_loader("legacy.Themes", NULL));
    assert(gui_themes_is_legacy_theme_loader("legacy.Themes.lua", NULL));
    assert(gui_themes_is_legacy_theme_loader(NULL, "Themes.lua"));
    assert(gui_themes_is_legacy_theme_loader(NULL, "Themes--Themes.lua"));
    assert(!gui_themes_is_legacy_theme_loader("custom.plugin", "custom.lua"));

    /* 3. Create test fixtures */
    char test_theme_path[512];
    snprintf(test_theme_path, sizeof(test_theme_path), "%s/Test.theme", sd_themes);
    const char *test_theme =
        "name=TestTheme\n"
        "screen=0x111111\n"
        "card=0x222222\n"
        "list_row=0x333333\n"
        "text_primary=0x444444\n"
        "text_muted=0x555555\n"
        "icon_root=theme1\n"
        "drawer_root=theme2\n"
        "home_mode=list\n"
        "home_row_gap=12\n"
        "tile.music.bg_color=0x999999\n";
    write_file(test_theme_path, test_theme);

    char bad_theme_path[512];
    snprintf(bad_theme_path, sizeof(bad_theme_path), "%s/Bad.theme", sd_themes);
    write_file(bad_theme_path, "name=Bad\nscreen=bad_color\n");

    char unusable_theme_path[512];
    snprintf(unusable_theme_path, sizeof(unusable_theme_path), "%s/Bad..name.theme", sd_themes);
    write_file(unusable_theme_path, test_theme);

    /* 4. Directory scan verification: valid but unselectable filenames are omitted. */
    gui_theme_entry_t entries[16];
    size_t count = gui_themes_scan(entries, 16);
    assert(count == 2);
    assert(entries[0].is_default);
    assert(strcmp(entries[0].name, TR("Default")) == 0);
    assert(strcmp(entries[1].filename, "Test.theme") == 0);
    assert(strcmp(entries[1].name, "TestTheme") == 0);
    assert(entries[1].preview_color == 0x222222);

    /* 5. Transactional candidate persistence & rollback on error */
    /* Select invalid theme -> rejected, selection unchanged */
    assert(!gui_themes_select("Bad.theme"));
    assert(strcmp(gui_themes_get_selected(), "") == 0);

    /* Select valid theme */
    int reloads = mock_reload_requests;
    assert(gui_themes_select("Test.theme"));
    assert(strcmp(gui_themes_get_selected(), "Test.theme") == 0);
    assert(mock_reload_requests == reloads + 1);

    /* State file verification */
    char state_file[512];
    snprintf(state_file, sizeof(state_file), "%s/theme_native_state.txt", compas_dir);
    FILE *sf = fopen(state_file, "r");
    assert(sf != NULL);
    char line[256];
    bool found_version = false, found_sel = false, found_imported = false;
    while (fgets(line, sizeof(line), sf)) {
        if (strncmp(line, "version=1", 9) == 0) found_version = true;
        if (strncmp(line, "selected=Test.theme", 19) == 0) found_sel = true;
        if (strncmp(line, "imported_legacy=1", 17) == 0) found_imported = true;
    }
    fclose(sf);
    assert(found_version && found_sel && found_imported);

    /* Test persistence failure rollback: make compas directory read-only to block temp file creation */
    chmod(compas_dir, 0555);
    char sel_before[GUI_THEMES_MAX_NAME];
    snprintf(sel_before, sizeof(sel_before), "%s", gui_themes_get_selected());
    assert(!gui_themes_select(""));
    assert(strcmp(gui_themes_get_selected(), sel_before) == 0);
    chmod(compas_dir, 0755);

    /* 6. Layout blending: presence flags, omitted plugin fields, and tilemode order > 6 rejection */
    /* Baseline plugin configuration */
    home_layout_config.configured = true;
    home_layout_config.list_mode = false;
    home_layout_config.tile_gap = 10;
    home_layout_config.has_background_image = true;
    snprintf(home_layout_config.background_image, sizeof(home_layout_config.background_image), "plugin_bg.png");
    home_layout_config.tile_count = 1;
    snprintf(home_layout_config.tiles[0].key, sizeof(home_layout_config.tiles[0].key), "music");
    home_layout_config.tiles[0].override.has_radius = true;
    home_layout_config.tiles[0].override.radius = 8;

    gui_themes_apply_active();
    /* Test.theme has home_mode=list, home_row_gap=12, tile.music.bg_color=0x999999.
     * Verify list_mode and row_gap applied, tile.music.bg_color merged, while baseline radius=8
     * and background_image="plugin_bg.png" are preserved! */
    assert(home_layout_config.list_mode == true);
    assert(home_layout_config.row_gap == 12);
    assert(home_layout_config.tile_gap == 10);
    assert(home_layout_config.has_background_image == true);
    assert(strcmp(home_layout_config.background_image, "plugin_bg.png") == 0);
    assert(home_layout_config.tiles[0].override.has_bg_color == true);
    assert(home_layout_config.tiles[0].override.bg_color == 0x999999);
    assert(home_layout_config.tiles[0].override.has_radius == true);
    assert(home_layout_config.tiles[0].override.radius == 8);

    /* Test blending incompatible tile mode > 6 rejection */
    char long_tile_theme[512];
    snprintf(long_tile_theme, sizeof(long_tile_theme), "%s/LongTile.theme", sd_themes);
    const char *long_tile_src =
        "name=LongTile\n"
        "screen=0x000000\n"
        "card=0x111111\n"
        "list_row=0x222222\n"
        "text_primary=0x333333\n"
        "text_muted=0x444444\n"
        "home_mode=list\n"
        "home_order=music,wireless,books,settings,dac,subsonic,custom\n";
    write_file(long_tile_theme, long_tile_src);

    /* Baseline tile mode */
    home_layout_config.configured = true;
    home_layout_config.list_mode = false;
    assert(gui_themes_select("LongTile.theme"));
    /* When theme order has 7 entries, if theme has home_mode=list, it's valid list mode */
    gui_themes_apply_active();
    assert(home_layout_config.list_mode == true);

    /* Now test theme without home_mode that specifies order > 6 on tile mode baseline */
    char invalid_tile_order[512];
    snprintf(invalid_tile_order, sizeof(invalid_tile_order), "%s/BadOrder.theme", sd_themes);
    const char *bad_order_src =
        "name=BadOrder\n"
        "screen=0x000000\n"
        "card=0x111111\n"
        "list_row=0x222222\n"
        "text_primary=0x333333\n"
        "text_muted=0x444444\n"
        "home_order=music,wireless,books,settings,dac,subsonic,custom\n";
    write_file(invalid_tile_order, bad_order_src);

    /* BadOrder has 7 tiles and no home_mode specified -> parser correctly rejects it for standalone tile mode! */
    assert(!gui_themes_select("BadOrder.theme"));

    /* 7. Default selection preserves fresh plugin baseline */
    mock_bg_screen = 0x555555;
    assert(gui_themes_select(""));
    assert(strcmp(gui_themes_get_selected(), "") == 0);
    gui_themes_apply_active();
    assert(mock_bg_screen == 0x555555);

    /* 8. Drawer precedence & Icon layer resolution with actual fixtures */
    /* Create real fixture in theme2 root */
    char theme2_pd_dir[512], theme2_wifi[512];
    snprintf(theme2_pd_dir, sizeof(theme2_pd_dir), "%spull_down", theme2_dir);
    mkdir(theme2_pd_dir, 0755);
    snprintf(theme2_wifi, sizeof(theme2_wifi), "%s/wifi.png", theme2_pd_dir);
    write_file(theme2_wifi, "theme2_wifi_png_content");

    /* Create real fixture in theme1 root */
    char theme1_pd_dir[512], theme1_wifi[512];
    snprintf(theme1_pd_dir, sizeof(theme1_pd_dir), "%spull_down", theme1_dir);
    mkdir(theme1_pd_dir, 0755);
    snprintf(theme1_wifi, sizeof(theme1_wifi), "%s/wifi.png", theme1_pd_dir);
    write_file(theme1_wifi, "theme1_wifi_png_content");

    assert(gui_themes_select("Test.theme"));
    char resolved[512];
    bool res = gui_themes_resolve_icon_layer("pull_down/wifi.png", resolved, sizeof(resolved));
    assert(res);
    /* Drawer root was theme2: MUST resolve to theme2, NOT icon_root (theme1) */
    assert(strstr(resolved, "theme2/pull_down/wifi.png") != NULL);

    /* Resolve the final file: safe-looking roots must not bypass containment via symlinks. */
    char outside_icon[512], private_dir[512], private_icon[512], linked_icon[512];
    char pack_dir[512], pack_launcher[512], pack_icon[512], safe_icon[512], unsafe_theme[512];
    snprintf(outside_icon, sizeof(outside_icon), "%s/outside.png", scratch_dir);
    write_file(outside_icon, "outside_bytes");
    snprintf(private_dir, sizeof(private_dir), "%s/.open_hiby_player", sd_root);
    mkdir(private_dir, 0755);
    snprintf(private_icon, sizeof(private_icon), "%s/private.png", private_dir);
    write_file(private_icon, "private_bytes");
    snprintf(linked_icon, sizeof(linked_icon), "%s/link.png", sd_themes);
    /* Absolute symlink targets avoid depending on the fixture working directory. */
    char canonical_outside[4096], canonical_private[4096];
    assert(realpath(outside_icon, canonical_outside));
    assert(realpath(private_icon, canonical_private));
    assert(symlink(canonical_outside, linked_icon) == 0);
    snprintf(pack_dir, sizeof(pack_dir), "%s/Pack", sd_themes);
    mkdir(pack_dir, 0755);
    snprintf(pack_launcher, sizeof(pack_launcher), "%s/launcher", pack_dir);
    mkdir(pack_launcher, 0755);
    snprintf(pack_icon, sizeof(pack_icon), "%s/music.png", pack_launcher);
    assert(symlink(canonical_outside, pack_icon) == 0);
    snprintf(safe_icon, sizeof(safe_icon), "%s/safe.png", sd_themes);
    write_file(safe_icon, "safe_bytes");
    snprintf(unsafe_theme, sizeof(unsafe_theme), "%s/Security.theme", sd_themes);
    const char *security_header = "name=Security\nscreen=0x111111\ncard=0x222222\n"
        "list_row=0x333333\ntext_primary=0x444444\ntext_muted=0x555555\n";
    char security_content[1024];
    snprintf(security_content, sizeof(security_content), "%sicon.launcher/music.png=link.png\n", security_header);
    write_file(unsafe_theme, security_content);
    assert(gui_themes_select("Security.theme"));
    assert(!gui_themes_resolve_icon_layer("launcher/music.png", resolved, sizeof(resolved)));
    unlink(linked_icon);
    assert(symlink(canonical_private, linked_icon) == 0);
    assert(!gui_themes_resolve_icon_layer("launcher/music.png", resolved, sizeof(resolved)));
    char upper_private_dir[512], upper_private_icon[512];
    snprintf(upper_private_dir, sizeof(upper_private_dir), "%s/.PLUGINS", sd_root);
    mkdir(upper_private_dir, 0755);
    snprintf(upper_private_icon, sizeof(upper_private_icon), "%s/private.png", upper_private_dir);
    write_file(upper_private_icon, "private_bytes");
    char canonical_upper[4096];
    assert(realpath(upper_private_icon, canonical_upper));
    unlink(linked_icon);
    assert(symlink(canonical_upper, linked_icon) == 0);
    assert(!gui_themes_resolve_icon_layer("launcher/music.png", resolved, sizeof(resolved)));
    unlink(linked_icon);
    assert(symlink(canonical_private, linked_icon) == 0);
    unlink(upper_private_icon);
    rmdir(upper_private_dir);
    snprintf(security_content, sizeof(security_content), "%sicon_root=Pack\n", security_header);
    write_file(unsafe_theme, security_content);
    assert(gui_themes_select("Security.theme"));
    assert(!gui_themes_resolve_icon_layer("launcher/music.png", resolved, sizeof(resolved)));
    snprintf(security_content, sizeof(security_content), "%sicon.launcher/music.png=safe.png\n", security_header);
    write_file(unsafe_theme, security_content);
    assert(gui_themes_select("Security.theme"));
    assert(gui_themes_resolve_icon_layer("launcher/music.png", resolved, sizeof(resolved)));
    assert(!gui_themes_resolve_icon_layer("launcher/music.png", resolved, 4));
    /* Migration must preserve an override matching an unsafe linked source. */
    snprintf(security_content, sizeof(security_content), "%sicon.launcher/music.png=link.png\n", security_header);
    write_file(unsafe_theme, security_content);
    char unsafe_legacy[512], ov_launcher[512], ov_security[512];
    snprintf(unsafe_legacy, sizeof(unsafe_legacy), "%s/.theme_state", sd_plugins);
    write_file(unsafe_legacy, "Security.theme\n");
    snprintf(ov_launcher, sizeof(ov_launcher), "%slauncher", override_dir);
    mkdir(ov_launcher, 0755);
    snprintf(ov_security, sizeof(ov_security), "%s/music.png", ov_launcher);
    write_file(ov_security, "private_bytes");
    gui_themes_reset_state();
    unlink(state_file);
    gui_themes_init();
    assert(!gui_themes_is_legacy_excluded("launcher/music.png"));
    sf = fopen(state_file, "r");
    assert(sf != NULL);
    found_imported = false;
    while (fgets(line, sizeof(line), sf)) {
        if (strncmp(line, "imported_legacy=1", 17) == 0) found_imported = true;
    }
    fclose(sf);
    assert(found_imported); /* Positive control: migration actually ran. */
    unlink(unsafe_legacy);
    unlink(ov_security);
    rmdir(ov_launcher);
    unlink(unsafe_theme);
    unlink(safe_icon);
    unlink(pack_icon);
    rmdir(pack_launcher);
    rmdir(pack_dir);
    unlink(linked_icon);
    unlink(private_icon);
    rmdir(private_dir);
    unlink(outside_icon);

    /* 9. Legacy migration: stock matching vs unknown override preservation */
    gui_themes_reset_state();
    /* Remove state file to test fresh boot migration */
    unlink(state_file);

    /* Write legacy state file pointing to Test.theme */
    char leg_state[512];
    snprintf(leg_state, sizeof(leg_state), "%s/.theme_state", sd_plugins);
    write_file(leg_state, "Test.theme\n");

    /* Create 2 override files: one matching theme2 stock wifi.png, one unknown plugin icon */
    char ov_pd_dir[512], ov_stock[512], ov_unknown[512];
    snprintf(ov_pd_dir, sizeof(ov_pd_dir), "%spull_down", override_dir);
    mkdir(ov_pd_dir, 0755);
    snprintf(ov_stock, sizeof(ov_stock), "%s/wifi.png", ov_pd_dir);
    write_file(ov_stock, "theme2_wifi_png_content");

    snprintf(ov_unknown, sizeof(ov_unknown), "%s/custom_plugin.png", ov_pd_dir);
    write_file(ov_unknown, "unique_plugin_override_bytes");

    /* Run init -> triggers migration */
    gui_themes_init();

    /* ov_stock should be legacy excluded */
    assert(gui_themes_is_legacy_excluded("pull_down/wifi.png"));
    /* ov_unknown must NOT be legacy excluded */
    assert(!gui_themes_is_legacy_excluded("pull_down/custom_plugin.png"));

    /* A stale record is removed from runtime even if persistent saving fails. */
    struct stat old_stock;
    assert(stat(ov_stock, &old_stock) == 0);
    chmod(compas_dir, 0555);
    write_file(ov_stock, "changed_plugin_bytes");
    assert(!gui_themes_is_legacy_excluded("pull_down/wifi.png"));
    write_file(ov_stock, "theme2_wifi_png_content");
    struct utimbuf old_times = {old_stock.st_atime, old_stock.st_mtime};
    assert(utime(ov_stock, &old_times) == 0);
    assert(!gui_themes_is_legacy_excluded("pull_down/wifi.png"));
    gui_themes_init();
    assert(!gui_themes_is_legacy_excluded("pull_down/wifi.png"));
    chmod(compas_dir, 0755);
    /* Persistent old state remains readable; explicit plugin writes clear it too. */
    gui_themes_reset_state();
    gui_themes_init();
    assert(gui_themes_is_legacy_excluded("pull_down/wifi.png"));

    /* Test clear exclusion */
    gui_themes_clear_legacy_exclusion("pull_down/wifi.png");
    assert(!gui_themes_is_legacy_excluded("pull_down/wifi.png"));

    /* Explicit Default remains authoritative despite an existing legacy selection. */
    assert(gui_themes_select(""));
    gui_themes_reset_state();
    gui_themes_init();
    assert(strcmp(gui_themes_get_selected(), "") == 0);

    /* 10. Corrupt state file rejection */
    gui_themes_reset_state();
    write_file(state_file, "version=2\nselected=Test.theme\n");
    gui_themes_init();
    /* Bad version rejected, selected remains empty */
    assert(strcmp(gui_themes_get_selected(), "") == 0);

    char corrupt_bytes[128] = {0};
    sf = fopen(state_file, "r");
    assert(sf != NULL);
    assert(fread(corrupt_bytes, 1, sizeof(corrupt_bytes) - 1, sf) > 0);
    fclose(sf);
    assert(strcmp(corrupt_bytes, "version=2\nselected=Test.theme\n") == 0);
    gui_themes_init();
    sf = fopen(state_file, "r");
    assert(sf != NULL);
    memset(corrupt_bytes, 0, sizeof(corrupt_bytes));
    assert(fread(corrupt_bytes, 1, sizeof(corrupt_bytes) - 1, sf) > 0);
    fclose(sf);
    assert(strcmp(corrupt_bytes, "version=2\nselected=Test.theme\n") == 0);

    /* Clean up scratch directory */
    unlink(test_theme_path);
    unlink(bad_theme_path);
    unlink(unusable_theme_path);
    unlink(long_tile_theme);
    unlink(invalid_tile_order);
    unlink(theme2_wifi);
    rmdir(theme2_pd_dir);
    unlink(theme1_wifi);
    rmdir(theme1_pd_dir);
    unlink(ov_stock);
    unlink(ov_unknown);
    rmdir(ov_pd_dir);
    unlink(leg_state);
    unlink(state_file);
    rmdir(sd_themes);
    rmdir(sd_plugins);
    rmdir(sd_root);
    rmdir(compas_dir);
    rmdir(override_dir);
    rmdir(theme1_dir);
    rmdir(theme2_dir);
    rmdir(scratch_dir);

    printf("All comprehensive gui_themes tests PASSED successfully!\n");
    return 0;
}
