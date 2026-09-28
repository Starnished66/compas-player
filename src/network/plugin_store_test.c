/* Host checks for the plugin store's index rules, state logic, record format,
 * and staged file installation. No network requests are made. */
#include "plugin_store.h"
#include "plugin_manager.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char valid_index[] =
    "{\"schema\":1,\"tag\":\"v1.2.3\",\"extra\":true,\"plugins\":["
    "{\"id\":\"sample.plugin\",\"name\":\"Sample\",\"version\":\"1.10\",\"api_min\":1,"
    "\"description\":\"Description\",\"category\":\"Tools\",\"author\":\"Author\",\"size\":17,"
    "\"files\":["
    "{\"asset\":\"sample--main.lua\",\"dest\":\".plugins/Sample.lua\","
    "\"sha256\":\"206765d71092ac33180b0833addc346caffc31055914495c811bbb1cf7a35254\",\"size\":9},"
    "{\"asset\":\"sample--prefs.txt\",\"dest\":\"Config/User File.txt\","
    "\"sha256\":\"79e8e0efd114579477296b7b9ca52814abd4b20f6fcdf5a9b8285ef97124e04e\","
    "\"size\":8,\"keep\":true}]}]}";

static bool parse(const char * json, plugin_store_plugin_t ** plugins, size_t * count) {
    char error[128];
    char tag[64];
    return plugin_store_parse_index(json, strlen(json), plugins, count, tag, error, sizeof(error));
}
static void reject_replacement(const char * from, const char * to) {
    char * json = malloc(8192);
    assert(json);
    const char * at = strstr(valid_index, from);
    assert(at);
    size_t prefix = (size_t)(at - valid_index);
    size_t oldlen = strlen(from);
    size_t newlen = strlen(to);
    size_t suffix = strlen(at + oldlen);
    assert(prefix + newlen + suffix + 1 < 8192);
    memcpy(json, valid_index, prefix);
    memcpy(json + prefix, to, newlen);
    memcpy(json+prefix+newlen, at+oldlen, suffix+1);
    plugin_store_plugin_t * parsed = NULL;
    size_t count = 0;
    assert(!parse(json, &parsed, &count));
    free(json);
}
static int build_file_index(char * out, size_t cap, int files) {
    const char * sha = "206765d71092ac33180b0833addc346caffc31055914495c811bbb1cf7a35254";
    int used = snprintf(
        out, 
        cap, 
        "{\"schema\":1,\"tag\":\"t\",\"plugins\":["
        "{\"id\":\"files\",\"name\":\"Files\",\"version\":\"1\","
        "\"api_min\":1,\"description\":\"\",\"category\":\"\","
        "\"author\":\"\",\"size\":%d,\"files\":["
        "{\"asset\":\"f0\",\"dest\":\".plugins/Files.lua\","
        "\"sha256\":\"%s\",\"size\":1}", 
        files, 
        sha);
    assert(used>0 && (size_t)used<cap);
    for (int i = 1; i < files; i++) {
        int n = snprintf(out + used, cap - (size_t) used,
                         ",{\"asset\":\"f%d\",\"dest\":\"data/f%d.txt\","
                         "\"sha256\":\"%s\",\"size\":1}", i, i, sha);
        assert(n>0 && (size_t)n<cap-(size_t)used);
        used+=n;
    }
    int n = snprintf(out+used, cap-(size_t)used, "]}]} ");
    assert(n>0 && (size_t)n<cap-(size_t)used);
    used+=n;
    /* Remove the convenient trailing space so the byte length is exact. */
    out[used-1] = '\0';
    return used-1;
}
static void reject_duplicate_plugin(bool duplicate_id) {
    const char * start = strstr(valid_index, "\"plugins\":[");
    assert(start);
    start+=11;
    const char * end = strstr(start, "]}]");
    assert(end);
    size_t object_len = (size_t)(end-start)+2;
    char * json = malloc(8192);
    char * second = malloc(2048);
    assert(json && second && object_len<2048);
    memcpy(second, start, object_len);
    second[object_len] = 0;
    if (!duplicate_id) {
        char * id = strstr(second, "sample.plugin");
        assert(id);
        memcpy(id, "second.plugin", 13);
    }
    int used = snprintf(
        json, 
        8192, 
        "{\"schema\":1,\"tag\":\"tag\",\"plugins\":[%.*s,%s]}", 
        (int)object_len, 
        start, 
        second);
    assert(used>0);
    plugin_store_plugin_t * parsed = NULL;
    size_t count = 0;
    assert(!parse(json, &parsed, &count));
    free(json);
    free(second);
}
static void write_file(const char * path, const char * data) {
    FILE * f = fopen(path, "wb");
    assert(f);
    assert(fwrite(data, 1, strlen(data), f) == strlen(data));
    assert(fclose(f) == 0);
}
static void mkdirs(const char * path) {
    assert(mkdir(path, 0755) == 0 || errno == EEXIST);
}

int main(void) {
    plugin_store_plugin_t * parsed = NULL;
    size_t count = 0;
    assert(parse(valid_index, &parsed, &count) && count == 1);
    assert(strcmp(parsed[0].id, "sample.plugin") == 0 && parsed[0].file_count == 2);
    /* A file used as another file's folder cannot install. */
    reject_replacement("Config/User File.txt", ".plugins/Sample.lua/x");
    reject_replacement("Config/User File.txt", ".PLUGINS/sample.lua/x");
    /* A second script in another letter case is still a second script. */
    reject_replacement("Config/User File.txt", ".PLUGINS/Extra.lua");
    assert(plugin_store_version_compare("1.10", "1.9")>0);
    assert(plugin_store_version_compare("1.2", "1.2.0") == 0);
    assert(plugin_store_version_compare("2.0.0.1", "2.0.0")>0);
    assert(plugin_store_version_compare("99999999999999999999999999", "99999999999999999999999998")>0);
    const char * good[] = {".plugins/Audio.lua", "Config/User File.txt", "a/b-c_1.txt"};
    const char * bad[] = {
        "/x", "a//b", "a/./b", "a/../b", ".compas/x",
        ".COMPAS/x", "x.upt", "x.UPT", "a\\b", "", ".plugins/.store_installed",
        ".plugins/.store_installed.tmp", ".Plugins/.disabled"
    };
    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++)assert(plugin_store_dest_valid(good[i]));
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)assert(!plugin_store_dest_valid(bad[i]));

    char altered[4096];
#define REJECT(old_text,new_text) do { \
        assert(strlen(valid_index) < sizeof(altered)); \
        strcpy(altered, valid_index); \
        char * q = strstr(altered, old_text); \
        assert(q); \
        memcpy(q,new_text, strlen(new_text)); \
        plugin_store_plugin_t * rejected = NULL; \
        assert(!parse(altered, &rejected, &count)); \
    } while (0)
    REJECT("\"schema\":1", "\"schema\":2");
    REJECT("\"tag\":\"v1.2.3\"", "\"tag\":\"bad/tag\"");
    REJECT("\"id\":\"sample.plugin\"", "\"id\":\"Bad.plugin\"");
    REJECT("\"version\":\"1.10\"", "\"version\":\"1.x\"");
    REJECT("\"version\":\"1.10\"", "\"version\":\"1.2.3.4.5\"");
    REJECT("\"api_min\":1", "\"api_min\":0");
    REJECT("\"sha256\":\"206765d71092ac33180b0833addc346caffc31055914495c811bbb1cf7a35254\"",
           "\"sha256\":\"ABC765d71092ac33180b0833addc346caffc31055914495c811bbb1cf7a35254\"");
    REJECT("\"size\":9", "\"size\":0");
    REJECT("\"dest\":\".plugins/Sample.lua\"", "\"dest\":\"../Sample.lua\"");
    REJECT("\"asset\":\"sample--main.lua\"", "\"asset\":\"bad/name.lua\"");
#undef REJECT
    /* Field lengths, integer shapes, file ranges, and script cardinality. */
    char * long_value = malloc(402);
    assert(long_value);
    memset(long_value, 'x', 401);
    long_value[401] = 0;
    char * replacement = malloc(512);
    assert(replacement);
    snprintf(replacement, 512, "\"description\":\"%s\"", long_value);
    reject_replacement("\"description\":\"Description\"", replacement);
    memset(long_value, 'x', 65);
    long_value[65] = 0;
    snprintf(replacement, 512, "\"author\":\"%s\"", long_value);
    reject_replacement("\"author\":\"Author\"", replacement);
    memset(long_value, 'x', 65);
    long_value[65] = 0;
    snprintf(replacement, 512, "\"name\":\"%s\"", long_value);
    reject_replacement("\"name\":\"Sample\"", replacement);
    memset(long_value, 'x', 33);
    long_value[33] = 0;
    snprintf(replacement, 512, "\"category\":\"%s\"", long_value);
    reject_replacement("\"category\":\"Tools\"", replacement);
    reject_replacement("\"api_min\":1", "\"api_min\":1.5");
    reject_replacement("\"name\":\"Sample\"", "\"name\":\"bad\\nname\"");
    reject_replacement("\"keep\":true", "\"keep\":\"yes\"");
    reject_replacement("\"size\":9", "\"size\":1048577");
    reject_replacement("\"dest\":\"Config/User File.txt\"", "\"dest\":\".plugins/sample.lua\"");
    reject_replacement("\"dest\":\".plugins/Sample.lua\"", "\"dest\":\"Data/Sample.lua\"");
    reject_replacement("\"dest\":\"Config/User File.txt\"", "\"dest\":\".plugins/Other.lua\"");
    free(long_value);
    free(replacement);

    /* One valid script is required and file destinations are unique after case folding. */
    reject_replacement("\"dest\":\"Config/User File.txt\"", "\"dest\":\".plugins/sample.lua\"");
    /* The unknown root property in valid_index is deliberately accepted. */
    free(parsed);
    parsed = NULL;
    assert(parse(valid_index, &parsed, &count));

    /* 32 files are accepted; 33 files and 201 plugins are rejected. */
    char * generated = malloc(100000);
    assert(generated);
    int used = build_file_index(generated, 100000, 32);
    plugin_store_plugin_t * many = NULL;
    char error[128];
    char tag[64];
    size_t many_count = 0;
    assert(plugin_store_parse_index(generated, (size_t) used, &many, &many_count,
                                    tag, error, sizeof(error)) && many_count == 1 &&
           many[0].file_count == 32);
    free(many);
    many = NULL;
    used = build_file_index(generated, 100000, 33);
    assert(!plugin_store_parse_index(generated, (size_t) used, &many, &many_count,
                                    tag, error, sizeof(error)));
    int pos = snprintf(generated, 100000, "{\"schema\":1,\"tag\":\"t\",\"plugins\":[");
    for (int i = 0; i < 201; i++)pos+=snprintf(generated+pos, 100000-(size_t)pos, "%s{}", i?",":"");
    pos+=snprintf(generated+pos, 100000-(size_t)pos, "]}");
    assert(!plugin_store_parse_index(generated, (size_t) pos, &many, &many_count, tag, error, sizeof(error)));
    pos = snprintf(generated, 100000, "{\"schema\":1,\"tag\":\"t\",\"plugins\":[");
    for (int i = 0; i < PLUGIN_STORE_MAX_PLUGINS; i++) {
        int n = snprintf(generated + pos, 100000 - (size_t) pos,
                         "%s{\"id\":\"p%d\",\"name\":\"P%d\",\"version\":\"1\","
                         "\"api_min\":1,\"description\":\"\",\"category\":\"\","
                         "\"author\":\"\",\"size\":1,\"files\":["
                         "{\"asset\":\"a%d\",\"dest\":\".plugins/P%d.lua\","
                         "\"sha256\":\"206765d71092ac33180b0833addc346caffc31055914495c811bbb1cf7a35254\","
                         "\"size\":1}]}", i ? "," : "", i, i, i, i);
        assert(n>0 && (size_t)n<100000-(size_t)pos);
        pos+=n;
    }
    pos+=snprintf(generated+pos, 100000-(size_t)pos, "]}");
    assert(plugin_store_parse_index(generated, (size_t) pos, &many, &many_count,
                                    tag, error, sizeof(error)) &&
           many_count == PLUGIN_STORE_MAX_PLUGINS);
    free(many);
    many = NULL;
    const char no_files[] = "{\"schema\":1,\"tag\":\"t\",\"plugins\":["
                            "{\"id\":\"x\",\"name\":\"X\",\"version\":\"1\","
                            "\"api_min\":1,\"description\":\"\",\"category\":\"\","
                            "\"author\":\"\",\"size\":0,\"files\":[]}]}";
    assert(!plugin_store_parse_index(no_files, sizeof(no_files) - 1, &many, &many_count,
                                    tag, error, sizeof(error)));
    reject_duplicate_plugin(true);
    reject_duplicate_plugin(false);
    free(generated);
    /* API/version incompatibility is carried as a note for installed entries. */
    free(parsed);
    parsed = NULL;
    assert(parse(valid_index, &parsed, &count));
    assert(plugin_store_compute_state(&parsed[0], NULL, "/missing", NULL) == PLUGIN_STORE_PLUGIN_AVAILABLE);
    assert(plugin_store_compute_state(&parsed[0], "1.9", "/missing", NULL) == PLUGIN_STORE_PLUGIN_UPDATE);
    parsed[0].api_min = PLUGIN_API_VERSION+1;
    bool incompatible = false;
    assert(plugin_store_compute_state(&parsed[0], "1.10", "/missing", &incompatible) ==
           PLUGIN_STORE_PLUGIN_INSTALLED && incompatible);
    assert(plugin_store_compute_state(&parsed[0], "1.9", "/missing", &incompatible) ==
           PLUGIN_STORE_PLUGIN_INSTALLED && incompatible);

    char template[] = "/tmp/plugin-store-test-XXXXXX";
    char * root = mkdtemp(template);
    assert(root);
    char plugins_dir[512], config_dir[512], stage[512], path[1024];
    snprintf(plugins_dir, sizeof(plugins_dir), "%s/.plugins", root);
    mkdirs(plugins_dir);
    snprintf(config_dir, sizeof(config_dir), "%s/Config", root);
    mkdirs(config_dir);
    snprintf(path, sizeof(path), "%s/Sample.lua", plugins_dir);
    write_file(path, "manual");
    assert(plugin_store_compute_state(&parsed[0], NULL, plugins_dir, &incompatible) ==
           PLUGIN_STORE_PLUGIN_MANUAL);
    snprintf(path, sizeof(path), "%s/User File.txt", config_dir);
    write_file(path, "custom");
    snprintf(stage, sizeof(stage), "%s/stage", root);
    mkdirs(stage);
    snprintf(path, sizeof(path), "%s/0.part", stage);
    write_file(path, "script-v1");
    /* A hand-copied script is never silently replaced. */
    plugin_store_plugin_t empty_record[1];
    memset(empty_record, 0, sizeof(empty_record));
    char versions[1][32] = {{0}};
    assert(!plugin_store_commit_local(root, stage, &parsed[0], false, empty_record, versions, 0));
    snprintf(path, sizeof(path), "%s/Sample.lua", plugins_dir);
    FILE * f = fopen(path, "rb");
    assert(f);
    char content[16] = {0};
    assert(fread(content, 1, sizeof(content)-1, f) == 6);
    fclose(f);
    assert(!strcmp(content, "manual"));
    snprintf(path, sizeof(path), "%s/0.part", stage);
    write_file(path, "script-v1");
    assert(plugin_store_commit_local(root, stage, &parsed[0], true, empty_record, versions, 0));
    snprintf(path, sizeof(path), "%s/Sample.lua", plugins_dir);
    f = fopen(path, "rb");
    assert(f);
    memset(content, 0, sizeof(content));
    assert(fread(content, 1, sizeof(content)-1, f) == 9);
    fclose(f);
    assert(!strcmp(content, "script-v1"));
    snprintf(path, sizeof(path), "%s/User File.txt", config_dir);
    f = fopen(path, "rb");
    assert(f);
    memset(content, 0, sizeof(content));
    assert(fread(content, 1, sizeof(content)-1, f) == 6);
    fclose(f);
    assert(!strcmp(content, "custom"));
    plugin_store_plugin_t * record = calloc(2, sizeof(*record));
    char (*record_versions)[32] = calloc(2, 32);
    assert(record && record_versions);
    assert(plugin_store_record_read(root, record, record_versions, 2) == 1);
    assert(!strcmp(record[0].id, "sample.plugin") &&
           !strcmp(record_versions[0], "1.10") && record[0].file_count == 2);
    assert(!strcmp(record[0].files[1].dest, "Config/User File.txt") && record[0].files[1].keep);
    assert(plugin_store_compute_state(&parsed[0], record_versions[0], plugins_dir, &incompatible) ==
           PLUGIN_STORE_PLUGIN_INSTALLED);
    /* Existing keep files are retained even under force. */
    plugin_store_plugin_t next = parsed[0];
    strcpy(next.version, "1.11");
    strcpy(next.files[0].sha256, "e8a403e9d1918286abece1b37618da7e4263538333b6ebe4df740d7fc67e6347");
    next.files[0].size = 9;
    snprintf(path, sizeof(path), "%s/Old Clean.txt", config_dir);
    write_file(path, "clean");
    snprintf(path, sizeof(path), "%s/Old Modified.txt", config_dir);
    write_file(path, "changed");
    record[0].file_count = 4;
    strcpy(record[0].files[2].dest, "Config/Old Clean.txt");
    strcpy(record[0].files[2].sha256, "3b066804f6d1d077173cfe4d06002e6a61e6f21c2b2e648417962115f1afcd8e");
    strcpy(record[0].files[3].dest, "Config/Old Modified.txt");
    strcpy(record[0].files[3].sha256, "0682c5f2076f099c34cfdd15a9e063849ed437a49677e6fcc5b4198c76575be5");
    int record_index = 0;
    assert(plugin_store_record_write(root, record, &record_index, 1));
    next.file_count = 1;
    snprintf(path, sizeof(path), "%s/0.part", stage);
    write_file(path, "script-v2");
    assert(plugin_store_commit_local(root, stage, &next, true, record, record_versions, 1));
    snprintf(path, sizeof(path), "%s/User File.txt", config_dir);
    f = fopen(path, "rb");
    assert(f);
    memset(content, 0, sizeof(content));
    assert(fread(content, 1, sizeof(content)-1, f) == 6);
    fclose(f);
    assert(!strcmp(content, "custom"));
    snprintf(path, sizeof(path), "%s/Old Clean.txt", config_dir);
    assert(access(path, F_OK) != 0 && errno == ENOENT);
    snprintf(path, sizeof(path), "%s/Old Modified.txt", config_dir);
    f = fopen(path, "rb");
    assert(f);
    memset(content, 0, sizeof(content));
    assert(fread(content, 1, sizeof(content)-1, f) == 7);
    fclose(f);
    assert(!strcmp(content, "changed"));
    /* Another plugin's file is a conflict; forcing it moves ownership, so
     * the record never lists one destination twice. */
    assert(plugin_store_record_read(root, record, record_versions, 2) == 1);
    plugin_store_plugin_t other = next;
    strcpy(other.id, "other.plugin");
    strcpy(other.version, "1");
    snprintf(path, sizeof(path), "%s/0.part", stage);
    write_file(path, "script-v2");
    assert(!plugin_store_commit_local(root, stage, &other, false, record, record_versions, 1));
    write_file(path, "script-v2");
    assert(plugin_store_commit_local(root, stage, &other, true, record, record_versions, 1));
    assert(plugin_store_record_read(root, record, record_versions, 2) == 1);
    assert(!strcmp(record[0].id, "other.plugin") && record[0].file_count == 1);
    snprintf(path, sizeof(path), "%s/.plugins/.store_installed", root);
    write_file(path, "corrupt record\n");
    assert(plugin_store_record_read(root, record, record_versions, 2) == 0);
    free(record);
    free(record_versions);
    free(parsed);
    printf("plugin-store-selftest: PASS\n");
    return 0;
}
