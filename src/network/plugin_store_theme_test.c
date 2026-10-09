/* Exercise the real catalog classifiers without network or storage writes. */
#include "plugin_store.c"
#include <assert.h>

#define TEST_SHA "0000000000000000000000000000000000000000000000000000000000000000"

static bool parse_preview(const char * preview_json, plugin_store_preview_t * preview) {
    char json[4096];
    const char * prefix = "{\"schema\":1,\"tag\":\"v1.0\",\"plugins\":[{"
        "\"id\":\"example.plugin\",\"name\":\"Example\",\"version\":\"1.0\","
        "\"description\":\"\",\"category\":\"\",\"author\":\"\",\"api_min\":1,\"size\":1,";
    int n = snprintf(json, sizeof(json), "%s%s%s%s\"files\":[{\"asset\":\"plugin.lua\","
        "\"dest\":\".plugins/plugin.lua\",\"sha256\":\"%s\",\"size\":1}]}]}",
        prefix, preview_json ? "\"preview\":" : "", preview_json ? preview_json : "",
        preview_json ? "," : "", TEST_SHA);
    assert(n > 0 && (size_t)n < sizeof(json));
    plugin_store_plugin_t * parsed = NULL;
    size_t count = 0;
    char tag[64], error[160];
    bool ok = plugin_store_parse_index(json, strlen(json), &parsed, &count, tag, error, sizeof(error));
    if (ok && preview) *preview = parsed[0].preview;
    free(parsed);
    return ok;
}

#define PREVIEW(asset, sha, size, width, height) \
    "{\"asset\":\"" asset "\",\"sha256\":\"" sha "\",\"size\":" #size \
    ",\"width\":" #width ",\"height\":" #height "}"
#define ASSERT_PREVIEW(preview_json, expected_asset) do { \
    plugin_store_preview_t parsed_preview; \
    assert(parse_preview((preview_json), &parsed_preview)); \
    assert(strcmp(parsed_preview.asset, (expected_asset)) == 0); \
} while (0)
#define ASSERT_INVALID_PREVIEW(preview_json) \
    assert(!parse_preview((preview_json), NULL))

static void test_preview_metadata(void) {
    plugin_store_preview_t parsed;
    assert(parse_preview(NULL, &parsed));
    assert(parsed.asset[0] == '\0'); /* Legacy catalog with no preview. */

    ASSERT_PREVIEW(PREVIEW("default.jpg", TEST_SHA, 1000, 217, 325), "default.jpg");

    const char * sized_pair = "{\"asset\":\"default.jpg\",\"sha256\":\"" TEST_SHA
        "\",\"size\":1000,\"width\":217,\"height\":325,\"variants\":["
        "{\"asset\":\"small.jpg\",\"sha256\":\"" TEST_SHA
        "\",\"size\":500,\"width\":144,\"height\":216}]}";
#if defined(BOARD_R3II_2025)
    ASSERT_PREVIEW(sized_pair, "small.jpg");
#else
    ASSERT_PREVIEW(sized_pair, "default.jpg");
#endif

    /* Reversing variant order must not affect largest-area selection. */
    const char * best_fit = "{\"asset\":\"base.jpg\",\"sha256\":\"" TEST_SHA
        "\",\"size\":1000,\"width\":100,\"height\":150,\"variants\":["
        "{\"asset\":\"larger.jpg\",\"sha256\":\"" TEST_SHA
        "\",\"size\":800,\"width\":180,\"height\":270},"
        "{\"asset\":\"smaller.jpg\",\"sha256\":\"" TEST_SHA
        "\",\"size\":600,\"width\":140,\"height\":210}]}";
#if defined(BOARD_R3II_2025)
    ASSERT_PREVIEW(best_fit, "smaller.jpg");
#else
    ASSERT_PREVIEW(best_fit, "larger.jpg");
#endif
    const char * reversed = "{\"asset\":\"base.jpg\",\"sha256\":\"" TEST_SHA
        "\",\"size\":1000,\"width\":100,\"height\":150,\"variants\":["
        "{\"asset\":\"smaller.jpg\",\"sha256\":\"" TEST_SHA
        "\",\"size\":600,\"width\":140,\"height\":210},"
        "{\"asset\":\"larger.jpg\",\"sha256\":\"" TEST_SHA
        "\",\"size\":800,\"width\":180,\"height\":270}]}";
#if defined(BOARD_R3II_2025)
    ASSERT_PREVIEW(reversed, "smaller.jpg");
#else
    ASSERT_PREVIEW(reversed, "larger.jpg");
#endif

    const char * no_fit = "{\"asset\":\"default.jpg\",\"sha256\":\"" TEST_SHA
        "\",\"size\":1000,\"width\":217,\"height\":325,\"variants\":["
        "{\"asset\":\"too-large.jpg\",\"sha256\":\"" TEST_SHA
        "\",\"size\":800,\"width\":230,\"height\":345}]}";
    ASSERT_PREVIEW(no_fit, "default.jpg");

    ASSERT_INVALID_PREVIEW("{\"asset\":\"x.jpg\",\"sha256\":\"bad\",\"size\":1,\"width\":1,\"height\":1}");
    ASSERT_INVALID_PREVIEW("{\"asset\":\"x.jpg\",\"sha256\":\"" TEST_SHA "\",\"size\":65537,\"width\":1,\"height\":1}");
    ASSERT_INVALID_PREVIEW("{\"asset\":\"x.jpg\",\"sha256\":\"" TEST_SHA "\",\"size\":1,\"width\":241,\"height\":1}");
    ASSERT_INVALID_PREVIEW("{\"asset\":\"x.jpg\",\"sha256\":\"" TEST_SHA "\",\"size\":1,\"width\":1,\"height\":1,\"variants\":{}}");
    ASSERT_INVALID_PREVIEW("{\"asset\":\"x.jpg\",\"sha256\":\"" TEST_SHA
        "\",\"size\":1,\"width\":1,\"height\":1,\"variants\":["
        PREVIEW("v1.jpg", TEST_SHA, 1, 1, 1) "," PREVIEW("v2.jpg", TEST_SHA, 1, 1, 1) ","
        PREVIEW("v3.jpg", TEST_SHA, 1, 1, 1) "," PREVIEW("v4.jpg", TEST_SHA, 1, 1, 1) "]}");
    ASSERT_INVALID_PREVIEW("{\"asset\":\"x.jpg\",\"sha256\":\"" TEST_SHA
        "\",\"size\":1,\"width\":1,\"height\":1,\"variants\":["
        "{\"asset\":\"v.jpg\",\"sha256\":\"bad\",\"size\":1,\"width\":1,\"height\":1}]}");
    ASSERT_INVALID_PREVIEW("{\"asset\":\"x.jpg\",\"sha256\":\"" TEST_SHA
        "\",\"size\":1,\"width\":1,\"height\":1,\"variants\":["
        "{\"asset\":\"v.jpg\",\"sha256\":\"" TEST_SHA "\",\"size\":65537,\"width\":1,\"height\":1}]}");
    ASSERT_INVALID_PREVIEW("{\"asset\":\"x.jpg\",\"sha256\":\"" TEST_SHA
        "\",\"size\":1,\"width\":1,\"height\":1,\"variants\":["
        "{\"asset\":\"v.jpg\",\"sha256\":\"" TEST_SHA "\",\"size\":1,\"width\":241,\"height\":1}]}");
}

static void set_payload(plugin_store_plugin_t *pack, const char *dest) {
    memset(pack, 0, sizeof(*pack));
    pack->file_count = 1;
    snprintf(pack->files[0].dest, sizeof(pack->files[0].dest), "%s", dest);
}

int main(void) {
    plugin_store_plugin_t pack;
    const char *valid[] = {"Themes/White.theme", "Themes/a/White.theme", "Themes/a/b/White.theme"};
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); ++i) {
        set_payload(&pack, valid[i]);
        assert(plugin_contains_theme_pack(&pack));
    }
    const char *invalid[] = {"Themes/a/b/c/White.theme", "Themes/White.THEME", "Other/White.theme", "Themes/.theme", "Themes/.x/a.theme", "Themes/a/.x.theme", "Themes//x.theme", "Themes/a..b.theme", "Themes/a\\b.theme"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        set_payload(&pack, invalid[i]);
        assert(!plugin_contains_theme_pack(&pack));
    }
    set_payload(&pack, ".plugins/Themes.lua");
    strcpy(pack.id, "example.themes");
    assert(!plugin_contains_theme_pack(&pack));
    set_payload(&pack, "Themes/White.theme");
    strcpy(pack.files[1].dest, ".plugins/player_layouts/sample.xml");
    pack.file_count = 2;
    assert(plugin_contains_theme_pack(&pack));
    assert(!plugin_contains_player_layout(&pack));
    set_payload(&pack, ".plugins/player_layouts/stock.xml");
    strcpy(pack.id, "example.hiby_stock_player");
    assert(!plugin_contains_theme_pack(&pack));
    assert(plugin_contains_player_layout(&pack));
    test_preview_metadata();
    char boundary[80] = "Themes/";
    memset(boundary + 7, 'a', 57);
    strcpy(boundary + 64, ".theme"); /* 63-byte native identifier. */
    set_payload(&pack, boundary);
    assert(plugin_contains_theme_pack(&pack));
    memset(boundary + 7, 'a', 58);
    strcpy(boundary + 65, ".theme"); /* 64-byte native identifier cannot fit. */
    set_payload(&pack, boundary);
    assert(!plugin_contains_theme_pack(&pack));
    puts("Theme Store classification tests passed.");
    return 0;
}
