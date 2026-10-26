/* Emulator manifest, game listing, catalog and install tests. No network:
 * the catalog parser, state rule and folder operations are exercised
 * directly on temporary folders. */
#include "emulator_store.h"
#include "../../../core/device/emulator_registry.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SHA "0000000000000000000000000000000000000000000000000000000000000000"

static char tmp[256];

static void write_file(const char * path, const char * text, mode_t mode) {
    FILE * f = fopen(path, "w");
    assert(f);
    fputs(text, f);
    fclose(f);
    assert(chmod(path, mode) == 0);
}

static void path_in(char * out, size_t size, const char * rel) {
    snprintf(out, size, "%s/%s", tmp, rel);
}

static void make_dir(const char * rel) {
    char path[512];
    path_in(path, sizeof(path), rel);
    assert(mkdir(path, 0755) == 0 || access(path, F_OK) == 0);
}

#define MANIFEST_HEAD "{\"id\":\"%s\",\"name\":\"QuickNES\",\"version\":\"1.0.0\",\"protocol\":%d," \
                      "\"exec\":\"%s\",\"systems\":[%s],\"author\":\"A\",\"license\":\"L\",\"description\":\"D\"}"
#define SYSTEM_OK "{\"id\":\"nes\",\"name\":\"NES\",\"games_dir\":\"NES\",\"extensions\":[\"nes\"]}"

static bool parse_manifest(const char * id, int protocol, const char * exec, const char * systems) {
    char json[2048], error[64];
    emulator_manifest_t m;
    snprintf(json, sizeof(json), MANIFEST_HEAD, id, protocol, exec, systems);
    return emulator_manifest_parse(json, strlen(json), &m, error, sizeof(error));
}

static void test_manifest(void) {
    char json[2048], error[64];
    emulator_manifest_t m;
    snprintf(json, sizeof(json), MANIFEST_HEAD, "quicknes", 1, "quicknes", SYSTEM_OK);
    assert(emulator_manifest_parse(json, strlen(json), &m, error, sizeof(error)));
    assert(strcmp(m.id, "quicknes") == 0 && strcmp(m.exec, "quicknes") == 0);
    assert(m.system_count == 1 && strcmp(m.systems[0].games_dir, "NES") == 0);
    assert(m.systems[0].extension_count == 1 && strcmp(m.systems[0].extensions[0], "nes") == 0);

    assert(!parse_manifest("Quick", 1, "quicknes", SYSTEM_OK));      /* bad id */
    assert(!parse_manifest("-quick", 1, "quicknes", SYSTEM_OK));
    assert(!parse_manifest("quicknes", 2, "quicknes", SYSTEM_OK));   /* protocol 2 */
    assert(!parse_manifest("quicknes", 1, "bin/quicknes", SYSTEM_OK)); /* exec with a slash */
    assert(!parse_manifest("quicknes", 1, "", SYSTEM_OK));
    assert(!parse_manifest("quicknes", 1, "quicknes", ""));          /* no systems */
    assert(!parse_manifest("quicknes", 1, "quicknes",
        "{\"id\":\"nes\",\"name\":\"NES\",\"games_dir\":\"../NES\",\"extensions\":[\"nes\"]}"));
    assert(!parse_manifest("quicknes", 1, "quicknes",
        "{\"id\":\"nes\",\"name\":\"NES\",\"games_dir\":\"A/B\",\"extensions\":[\"nes\"]}"));
    assert(!parse_manifest("quicknes", 1, "quicknes",
        "{\"id\":\"nes\",\"name\":\"NES\",\"games_dir\":\"NES\",\"extensions\":[\"NES\"]}")); /* not lowercase */
    assert(!parse_manifest("quicknes", 1, "quicknes",
        "{\"id\":\"nes\",\"name\":\"NES\",\"games_dir\":\"NES\",\"extensions\":[\"toolongxx\"]}"));
    assert(!parse_manifest("quicknes", 1, "quicknes",
        "{\"id\":\"nes\",\"name\":\"NES\",\"games_dir\":\"NES\","
        "\"extensions\":[\"a\",\"b\",\"c\",\"d\",\"e\",\"f\",\"g\",\"h\",\"i\"]}")); /* nine extensions */
    /* Unknown keys are ignored. */
    snprintf(json, sizeof(json), "{\"future\":1,\"id\":\"x\",\"name\":\"X\",\"version\":\"1\",\"protocol\":1,"
             "\"exec\":\"x\",\"systems\":[" SYSTEM_OK "]}");
    assert(emulator_manifest_parse(json, strlen(json), &m, error, sizeof(error)));
}

static void test_load_and_scan(void) {
    char path[512], dir[512];
    make_dir("emu");
    make_dir("emu/quicknes");
    path_in(path, sizeof(path), "emu/quicknes/emulator.json");
    char json[2048];
    snprintf(json, sizeof(json), MANIFEST_HEAD, "quicknes", 1, "quicknes", SYSTEM_OK);
    write_file(path, json, 0644);
    path_in(dir, sizeof(dir), "emu");

    emulator_manifest_t m;
    char error[64];
    /* Missing exec. */
    assert(!emulator_manifest_load(dir, "quicknes", &m, error, sizeof(error)));
    /* Present but not executable. */
    path_in(path, sizeof(path), "emu/quicknes/quicknes");
    write_file(path, "x", 0644);
    assert(!emulator_manifest_load(dir, "quicknes", &m, error, sizeof(error)));
    assert(chmod(path, 0755) == 0);
    assert(emulator_manifest_load(dir, "quicknes", &m, error, sizeof(error)));

    /* Folder name differs from the manifest id. */
    make_dir("emu/other");
    path_in(path, sizeof(path), "emu/other/emulator.json");
    write_file(path, json, 0644);
    path_in(path, sizeof(path), "emu/other/quicknes");
    write_file(path, "x", 0755);
    assert(!emulator_manifest_load(dir, "other", &m, error, sizeof(error)));
    /* Hidden staging folders are never scanned. */
    make_dir("emu/.stage-quicknes.1.1");

    int count;
    emulator_manifest_t * list = emulator_registry_scan(dir, &count);
    assert(list && count == 1 && strcmp(list[0].id, "quicknes") == 0);
    free(list);
    assert(emulator_registry_scan("/nonexistent-emulator-root", &count) == NULL && count == 0);
}

static void test_games(void) {
    char sd[512], path[1024];
    path_in(sd, sizeof(sd), "sd");
    make_dir("sd");
    make_dir("sd/Games");
    make_dir("sd/Games/NES");
    const char * names[] = { "zelda.nes", "Mario.NES", ".hidden.nes", "mario.srm", "readme.txt", "noext" };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        snprintf(path, sizeof(path), "%s/Games/NES/%s", sd, names[i]);
        write_file(path, "rom", 0644);
    }
    emulator_system_t system = { .id = "nes", .name = "NES", .games_dir = "NES", .extension_count = 1 };
    snprintf(system.extensions[0], sizeof(system.extensions[0]), "nes");
    int count;
    emulator_game_t * games = emulator_registry_list_games(sd, NULL, &system, &count);
    assert(count == 2 && strcmp(games[0].label, "Mario") == 0 && strcmp(games[1].label, "zelda") == 0);
    free(games);
    /* Games the emulator bundles are listed with the SD card's. */
    make_dir("bundling");
    make_dir("bundling/games");
    make_dir("bundling/games/NES");
    path_in(path, sizeof(path), "bundling/games/NES/bundled.nes");
    write_file(path, "rom", 0644);
    path_in(path, sizeof(path), "bundling");
    games = emulator_registry_list_games(sd, path, &system, &count);
    assert(count == 3 && strcmp(games[0].label, "bundled") == 0);
    free(games);
    snprintf(system.games_dir, sizeof(system.games_dir), "Missing");
    assert(emulator_registry_list_games(sd, path, &system, &count) == NULL && count == 0);
}

#define FILE_ENTRY(asset, dest, sha, size, extra) \
    "{\"asset\":\"" asset "\",\"dest\":\"" dest "\"," sha "\"size\":" #size extra "}"
#define SHA_FIELD "\"sha256\":\"" SHA "\","

static bool parse_catalog(const char * emulator_fields, const char * files, int total_size, int protocol,
                          emulator_store_entry_t ** out) {
    char json[4096], tag[64], error[160];
    size_t count;
    snprintf(json, sizeof(json),
             "{\"schema\":1,\"tag\":\"v1\",\"emulators\":[{\"id\":\"quicknes\",\"name\":\"QuickNES\","
             "\"version\":\"1.0.0\",\"protocol\":%d,\"systems\":[\"NES\"],\"description\":\"d\","
             "\"author\":\"a\",\"license\":\"l\",\"size\":%d,%s\"files\":[%s]}]}",
             protocol, total_size, emulator_fields, files);
    emulator_store_entry_t * entries = NULL;
    bool ok = emulator_store_parse_index(json, strlen(json), &entries, &count, tag, error, sizeof(error));
    if (ok) assert(count == 1);
    if (out) *out = entries; else free(entries);
    return ok;
}

#define GOOD_FILES \
    FILE_ENTRY("quicknes--quicknes", "quicknes/quicknes", SHA_FIELD, 100, ",\"executable\":true") "," \
    FILE_ENTRY("quicknes--emulator.json", "quicknes/emulator.json", SHA_FIELD, 20, "")

static void test_catalog(void) {
    emulator_store_entry_t * entries;
    assert(parse_catalog("", GOOD_FILES, 120, 1, &entries));
    assert(entries[0].file_count == 2 && entries[0].files[0].executable && !entries[0].files[1].executable);
    assert(strcmp(entries[0].systems, "NES") == 0);
    assert(emulator_store_compute_state(&entries[0], NULL) == EMULATOR_STORE_AVAILABLE);
    assert(emulator_store_compute_state(&entries[0], "1.0.0") == EMULATOR_STORE_INSTALLED);
    assert(emulator_store_compute_state(&entries[0], "0.9") == EMULATOR_STORE_UPDATE);
    free(entries);

    /* Protocol 2 is listed but incompatible. */
    assert(parse_catalog("", GOOD_FILES, 120, 2, &entries));
    assert(emulator_store_compute_state(&entries[0], NULL) == EMULATOR_STORE_INCOMPATIBLE);
    assert(emulator_store_compute_state(&entries[0], "0.9") == EMULATOR_STORE_INSTALLED);
    free(entries);

    /* Destination outside <id>/, with .., absolute, or the folder itself. */
    assert(!parse_catalog("", FILE_ENTRY("a", "other/quicknes", SHA_FIELD, 100, ",\"executable\":true") ","
                          FILE_ENTRY("b", "quicknes/emulator.json", SHA_FIELD, 20, ""), 120, 1, NULL));
    assert(!parse_catalog("", FILE_ENTRY("a", "quicknes/../x", SHA_FIELD, 100, ",\"executable\":true") ","
                          FILE_ENTRY("b", "quicknes/emulator.json", SHA_FIELD, 20, ""), 120, 1, NULL));
    assert(!parse_catalog("", FILE_ENTRY("a", "/quicknes/x", SHA_FIELD, 100, ",\"executable\":true") ","
                          FILE_ENTRY("b", "quicknes/emulator.json", SHA_FIELD, 20, ""), 120, 1, NULL));
    assert(!parse_catalog("", FILE_ENTRY("a", "quicknes/", SHA_FIELD, 100, ",\"executable\":true") ","
                          FILE_ENTRY("b", "quicknes/emulator.json", SHA_FIELD, 20, ""), 120, 1, NULL));
    assert(!parse_catalog("", FILE_ENTRY("a", "quicknesx/q", SHA_FIELD, 100, ",\"executable\":true") ","
                          FILE_ENTRY("b", "quicknes/emulator.json", SHA_FIELD, 20, ""), 120, 1, NULL));
    /* Missing sha256, missing manifest, no executable file, wrong total. */
    assert(!parse_catalog("", FILE_ENTRY("a", "quicknes/quicknes", "", 100, ",\"executable\":true") ","
                          FILE_ENTRY("b", "quicknes/emulator.json", SHA_FIELD, 20, ""), 120, 1, NULL));
    assert(!parse_catalog("", FILE_ENTRY("a", "quicknes/quicknes", SHA_FIELD, 100, ",\"executable\":true"), 100, 1, NULL));
    assert(!parse_catalog("", FILE_ENTRY("a", "quicknes/quicknes", SHA_FIELD, 100, "") ","
                          FILE_ENTRY("b", "quicknes/emulator.json", SHA_FIELD, 20, ""), 120, 1, NULL));
    assert(!parse_catalog("", GOOD_FILES, 121, 1, NULL));
}

static void test_commit_and_remove(void) {
    char root[512], stage[512], path[512], json[2048];
    path_in(root, sizeof(root), "store");
    make_dir("store");
    snprintf(json, sizeof(json), MANIFEST_HEAD, "quicknes", 1, "quicknes", SYSTEM_OK);

    path_in(stage, sizeof(stage), "store/.stage-quicknes.1.1");
    make_dir("store/.stage-quicknes.1.1");
    path_in(path, sizeof(path), "store/.stage-quicknes.1.1/emulator.json");
    write_file(path, json, 0644);
    /* No executable yet: refused, nothing installed. */
    assert(!emulator_store_commit_staged(root, stage, "quicknes"));
    path_in(path, sizeof(path), "store/.stage-quicknes.1.1/quicknes");
    write_file(path, "v1", 0755);
    assert(emulator_store_commit_staged(root, stage, "quicknes"));
    assert(access(stage, F_OK) != 0);

    /* A second install replaces the first as a whole. */
    make_dir("store/.stage-quicknes.2.1");
    path_in(stage, sizeof(stage), "store/.stage-quicknes.2.1");
    path_in(path, sizeof(path), "store/.stage-quicknes.2.1/emulator.json");
    write_file(path, json, 0644);
    path_in(path, sizeof(path), "store/.stage-quicknes.2.1/quicknes");
    write_file(path, "v2", 0755);
    path_in(path, sizeof(path), "store/.stage-quicknes.2.1/LICENSE");
    write_file(path, "lic", 0644);
    assert(emulator_store_commit_staged(root, stage, "quicknes"));
    int count;
    emulator_manifest_t * list = emulator_registry_scan(root, &count);
    assert(count == 1);
    free(list);
    path_in(path, sizeof(path), "store/quicknes/LICENSE");
    assert(access(path, F_OK) == 0);
    /* A staged folder whose manifest id differs is refused and the install stays. */
    make_dir("store/.stage-other.1.1");
    path_in(stage, sizeof(stage), "store/.stage-other.1.1");
    path_in(path, sizeof(path), "store/.stage-other.1.1/emulator.json");
    write_file(path, json, 0644);
    path_in(path, sizeof(path), "store/.stage-other.1.1/quicknes");
    write_file(path, "x", 0755);
    assert(!emulator_store_commit_staged(root, stage, "other"));

    assert(!emulator_store_remove(root, "../store"));
    assert(emulator_store_remove(root, "quicknes"));
    path_in(path, sizeof(path), "store/quicknes");
    assert(access(path, F_OK) != 0);
    assert(!emulator_store_remove(root, "quicknes"));
}

int main(void) {
    snprintf(tmp, sizeof(tmp), "/tmp/emulator_store_test.XXXXXX");
    assert(mkdtemp(tmp));
    test_manifest();
    test_load_and_scan();
    test_games();
    test_catalog();
    test_commit_and_remove();
    char cmd[300];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmp);
    int rc = system(cmd);
    (void) rc;
    puts("emulator store tests passed");
    return 0;
}
