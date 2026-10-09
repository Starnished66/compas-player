#include "src/core/theme_file.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <dirent.h>

const char * const home_layout_tile_keys[HOME_LAYOUT_TILE_COUNT] = {
    "music", "stream_media", "wireless", "books", "settings", "dac", "subsonic"
};

static const char *get_scratch_dir(void) {
    const char *env = getenv("THEME_TEST_BUILD_DIR");
    if (env && env[0]) {
        return env;
    }
    return "build_ui_test/theme-parser";
}

static void test_repo_themes(void) {
    const char *repo_themes[] = {
        "plugins_examples/Themes/Earthy.theme",
        "plugins_examples/Themes/GameBoy.theme",
        "plugins_examples/Themes/Monastic.theme",
        "plugins_examples/Themes/MountainSunset.theme",
        "plugins_examples/Themes/ObsidianAudio.theme",
        "plugins_examples/Themes/Retro.theme",
        "plugins_examples/Themes/Swamp.theme",
        "plugins_examples/Themes/Terminal.theme",
        "plugins_examples/Themes/Trees.theme",
        "plugins_examples/Themes/Vaporwave.theme",
        "plugins_examples/Themes/Wavy.theme",
        "plugins_examples/Themes/White.theme",
        "plugins_examples/Themes/ZenTerracotta.theme",
    };

    printf("Testing %zu repository themes...\n", sizeof(repo_themes)/sizeof(repo_themes[0]));
    assert(sizeof(repo_themes)/sizeof(repo_themes[0]) == 13);

    for (size_t i = 0; i < sizeof(repo_themes)/sizeof(repo_themes[0]); i++) {
        theme_file_t theme;
        char err[256] = {0};
        bool ok = theme_file_parse(repo_themes[i], &theme, err, sizeof(err));
        if (!ok) {
            fprintf(stderr, "Failed to parse %s: %s\n", repo_themes[i], err);
        }
        assert(ok);
        assert(strlen(theme.name) > 0);
        assert(theme.screen <= 0xFFFFFF);
        assert(theme.card <= 0xFFFFFF);
        assert(theme.list_row <= 0xFFFFFF);
        assert(theme.text_primary <= 0xFFFFFF);
        assert(theme.text_muted <= 0xFFFFFF);
    }
}

static void test_compas_plugins_themes(void) {
    const char *compas_plugins_dir = "../compas-plugins/plugins/Themes/Themes";
    DIR *d = opendir(compas_plugins_dir);
    if (!d) {
        printf("Note: compas-plugins dir %s not available, skipping directory scan.\n", compas_plugins_dir);
        return;
    }

    struct dirent *de;
    size_t count = 0;
    while ((de = readdir(d)) != NULL) {
        size_t name_len = strlen(de->d_name);
        if (name_len > 6 && strcmp(de->d_name + name_len - 6, ".theme") == 0) {
            char filepath[512];
            snprintf(filepath, sizeof(filepath), "%s/%s", compas_plugins_dir, de->d_name);
            theme_file_t theme;
            char err[256] = {0};
            bool ok = theme_file_parse(filepath, &theme, err, sizeof(err));
            if (!ok) {
                fprintf(stderr, "Failed to parse %s: %s\n", filepath, err);
            }
            assert(ok);
            assert(strlen(theme.name) > 0);
            count++;
        }
    }
    closedir(d);
    printf("Tested %zu compas-plugins themes.\n", count);
    assert(count >= 1);
}

static void test_regression_cases(void) {
    char scratch_path[512];
    snprintf(scratch_path, sizeof(scratch_path), "%s/test_temp.theme", get_scratch_dir());

    #define WRITE_TMP(content) do { \
        FILE *fp = fopen(scratch_path, "w"); \
        assert(fp != NULL); \
        fputs(content, fp); \
        fclose(fp); \
    } while (0)

    /* Baseline valid theme header */
    const char *base_theme =
        "name=ValidTheme\n"
        "screen=0x000000\n"
        "card=0x111111\n"
        "list_row=0x222222\n"
        "text_primary=0x333333\n"
        "text_muted=0x444444\n";

    /* Case 1: Align & text_size enums */
    {
        char buf[1024];
        /* Valid align and text_size */
        snprintf(buf, sizeof(buf), "%stile.music.align=center\ntile.music.text_size=large\n", base_theme);
        WRITE_TMP(buf);
        theme_file_t t;
        char err[256] = {0};
        assert(theme_file_parse(scratch_path, &t, err, sizeof(err)));
        assert(strcmp(t.home.tiles[0].override.align, "center") == 0);
        assert(strcmp(t.home.tiles[0].override.text_size, "large") == 0);

        /* Invalid align */
        snprintf(buf, sizeof(buf), "%stile.music.align=justify\n", base_theme);
        WRITE_TMP(buf);
        assert(!theme_file_parse(scratch_path, &t, err, sizeof(err)));
        assert(strstr(err, "align") != NULL);

        /* Invalid text_size */
        snprintf(buf, sizeof(buf), "%stile.music.text_size=huge\n", base_theme);
        WRITE_TMP(buf);
        assert(!theme_file_parse(scratch_path, &t, err, sizeof(err)));
        assert(strstr(err, "text_size") != NULL);
    }

    /* Case 2: Reject tile-mode home_order > 6 (list mode supports up to HOME_LAYOUT_MAX_TILES) */
    {
        char buf[1024];
        /* tile mode (implicit default) with 7 tiles -> must reject */
        snprintf(buf, sizeof(buf), "%shome_order=music,stream_media,subsonic,wireless,books,settings,dac\n", base_theme);
        WRITE_TMP(buf);
        theme_file_t t;
        char err[256] = {0};
        assert(!theme_file_parse(scratch_path, &t, err, sizeof(err)));
        assert(strstr(err, "tile mode supports at most 6 tiles") != NULL);

        /* list mode with 7 tiles -> must succeed */
        snprintf(buf, sizeof(buf), "%shome_mode=list\nhome_order=music,stream_media,subsonic,wireless,books,settings,dac\n", base_theme);
        WRITE_TMP(buf);
        assert(theme_file_parse(scratch_path, &t, err, sizeof(err)));
        assert(t.home.order_count == 7);

        /* list mode with 12 tiles -> succeeds */
        snprintf(buf, sizeof(buf), "%shome_mode=list\nhome_order=t1,t2,t3,t4,t5,t6,t7,t8,t9,t10,t11,t12\n", base_theme);
        WRITE_TMP(buf);
        assert(theme_file_parse(scratch_path, &t, err, sizeof(err)));
        assert(t.home.order_count == 12);

        /* list mode with 13 tiles -> exceeds max tiles */
        snprintf(buf, sizeof(buf), "%shome_mode=list\nhome_order=t1,t2,t3,t4,t5,t6,t7,t8,t9,t10,t11,t12,t13\n", base_theme);
        WRITE_TMP(buf);
        assert(!theme_file_parse(scratch_path, &t, err, sizeof(err)));
    }

    /* Case 3: Registration contract IDs (digits-leading, dots, hyphens, underscores; reject >= 40 chars) */
    {
        char buf[1024];
        /* Digits-leading tile ID, dots and hyphens */
        snprintf(buf, sizeof(buf), "%shome_mode=list\nhome_order=01.my-tile_v1\ntile.01.my-tile_v1.radius=4\n", base_theme);
        WRITE_TMP(buf);
        theme_file_t t;
        char err[256] = {0};
        assert(theme_file_parse(scratch_path, &t, err, sizeof(err)));
        assert(strcmp(t.home.order[0], "01.my-tile_v1") == 0);
        assert(strcmp(t.home.tiles[0].key, "01.my-tile_v1") == 0);
        assert(t.home.tiles[0].override.radius == 4);

        /* Exactly 39 characters ID (fits in char key[40]) -> OK */
        char id39[40];
        memset(id39, 'a', 39);
        id39[39] = '\0';
        snprintf(buf, sizeof(buf), "%shome_mode=list\nhome_order=%s\ntile.%s.radius=4\n", base_theme, id39, id39);
        WRITE_TMP(buf);
        assert(theme_file_parse(scratch_path, &t, err, sizeof(err)));
        assert(strcmp(t.home.order[0], id39) == 0);
        assert(strcmp(t.home.tiles[0].key, id39) == 0);

        /* 40 characters ID -> reject */
        char id40[41];
        memset(id40, 'a', 40);
        id40[40] = '\0';
        snprintf(buf, sizeof(buf), "%shome_mode=list\nhome_order=%s\n", base_theme, id40);
        WRITE_TMP(buf);
        assert(!theme_file_parse(scratch_path, &t, err, sizeof(err)));

        snprintf(buf, sizeof(buf), "%stile.%s.radius=4\n", base_theme, id40);
        WRITE_TMP(buf);
        assert(!theme_file_parse(scratch_path, &t, err, sizeof(err)));
    }

    /* Case 4: Empty definitions clear previous fields */
    {
        char buf[1024];
        snprintf(buf, sizeof(buf),
            "%sicon_root=theme1\n"
            "icon_root=\n"
            "drawer_root=theme2\n"
            "drawer_root=\n"
            "home_order=music,wireless\n"
            "home_order=\n", base_theme);
        WRITE_TMP(buf);
        theme_file_t t;
        char err[256] = {0};
        assert(theme_file_parse(scratch_path, &t, err, sizeof(err)));
        assert(t.icon_root[0] == '\0');
        assert(t.drawer_root[0] == '\0');
        assert(t.home.order_count == 0);
    }

    /* Case 5: Hexadecimal integer dimensions syntax */
    {
        char buf[1024];
        snprintf(buf, sizeof(buf),
            "%shome_tile_gap=0x06\n"
            "home_row_gap=0x0A\n"
            "tile.music.radius=0x18\n"
            "tile.music.height=0x5C\n"
            "tile.music.width=0x01B8\n", base_theme);
        WRITE_TMP(buf);
        theme_file_t t;
        char err[256] = {0};
        assert(theme_file_parse(scratch_path, &t, err, sizeof(err)));
        assert(t.home.tile_gap == 6);
        assert(t.home.row_gap == 10);
        assert(t.home.tiles[0].override.radius == 24);
        assert(t.home.tiles[0].override.height == 92);
        assert(t.home.tiles[0].override.width == 440);

        /* Malformed hex integer */
        snprintf(buf, sizeof(buf), "%shome_row_gap=0xGG\n", base_theme);
        WRITE_TMP(buf);
        assert(!theme_file_parse(scratch_path, &t, err, sizeof(err)));
    }

    /* Case 6: home_order raw buffer holding 12 legal 39-byte IDs */
    {
        char long_order_buf[700];
        strcpy(long_order_buf, "home_mode=list\nhome_order=");
        for (int i = 0; i < 12; i++) {
            char chunk[48];
            /* Exactly 39 chars: "tile_00_" (8 chars) + 31 'x' chars */
            snprintf(chunk, sizeof(chunk), "tile_%02d_xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", i);
            assert(strlen(chunk) == 39);
            strcat(long_order_buf, chunk);
            if (i < 11) strcat(long_order_buf, ",");
        }
        strcat(long_order_buf, "\n");

        char full_buf[2048];
        snprintf(full_buf, sizeof(full_buf), "%s%s", base_theme, long_order_buf);
        WRITE_TMP(full_buf);

        theme_file_t t;
        char err[256] = {0};
        assert(theme_file_parse(scratch_path, &t, err, sizeof(err)));
        assert(t.home.order_count == 12);
        for (int i = 0; i < 12; i++) {
            char expected[48];
            snprintf(expected, sizeof(expected), "tile_%02d_xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", i);
            assert(strcmp(t.home.order[i], expected) == 0);
        }
    }

    /* Case 7: Skipping comments without equals, sections, unknown keys, and malformed tile props */
    {
        const char *compat_theme =
            "-- Lua style comment\n"
            "[metadata_section]\n"
            "; INI style comment\n"
            "name=CompatTheme\n"
            "screen=0x112233\n"
            "card=0x445566\n"
            "list_row=0x778899\n"
            "text_primary=0xAABBCC\n"
            "text_muted=0xDDEEFF\n"
            "x-custom-author=UnknownAuthor\n"
            "tile.music.bg-color=0x123456\n"
            "tile.music.bg_color=0x654321\n"
            "unknown_future_key=some_value\n";
        WRITE_TMP(compat_theme);

        theme_file_t t;
        char err[256] = {0};
        assert(theme_file_parse(scratch_path, &t, err, sizeof(err)));
        assert(strcmp(t.name, "CompatTheme") == 0);
        assert(t.screen == 0x112233);
        assert(t.card == 0x445566);
        assert(t.home.tile_count == 1);
        assert(strcmp(t.home.tiles[0].key, "music") == 0);
        assert(t.home.tiles[0].override.has_bg_color);
        assert(t.home.tiles[0].override.bg_color == 0x654321);
    }

    /* Clean up */
    unlink(scratch_path);
    #undef WRITE_TMP
}

int main(void) {
    printf("Running theme_file unit tests...\n");
    test_repo_themes();
    test_compas_plugins_themes();
    test_regression_cases();
    printf("All theme_file unit tests and regressions PASSED successfully!\n");
    return 0;
}
