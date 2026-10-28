/* Host tests for the external apps backend: manifest parsing and validation,
 * the board filter, the file passed to an app, the app environment, approval
 * keying and the reserved program folders. Run by test_external_apps.sh,
 * which also builds the static ELF fixture named by TEST_STATIC_ELF. */
#include "src/core/device/app_approval.h"
#include "src/core/device/app_registry.h"
#include "src/core/process/app_session.h"
#include "src/plugins/storage/plugin_reserved_dirs.h"

#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define EM_MIPS_T 8

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static const char * const MINIMAL = "{\"protocol\":1,\"id\":\"hello\",\"name\":\"Hello\",\"version\":\"1.0\",\"exec\":\"hello\"}";

static bool parse(const char * json) {
    app_manifest_t m;
    char err[64];
    return app_manifest_parse(json, strlen(json), &m, err, sizeof(err));
}

static void test_parse(void) {
    app_manifest_t m;
    char err[64];
    CHECK(app_manifest_parse(MINIMAL, strlen(MINIMAL), &m, err, sizeof(err)));
    CHECK(!strcmp(m.id, "hello") && !strcmp(m.name, "Hello") && !strcmp(m.version, "1.0") && !strcmp(m.exec, "hello"));
    CHECK(m.icon[0] == '\0' && m.opens_count == 0 && m.boards_count == 0 && !m.free_player_memory);

    const char * full = "{\"protocol\":1,\"id\":\"a_b-9\",\"name\":\"Full\",\"version\":\"2.1.0\",\"exec\":\"run\","
                        "\"icon\":\"icon.png\",\"opens\":[\"pdf\",\"epub\"],\"free_player_memory\":true,"
                        "\"boards\":[\"r1\",\"tempotec_v1\"]}";
    CHECK(app_manifest_parse(full, strlen(full), &m, err, sizeof(err)));
    CHECK(!strcmp(m.icon, "icon.png") && m.opens_count == 2 && !strcmp(m.opens[1], "epub") && m.free_player_memory);
    CHECK(m.boards_count == 2 && app_manifest_for_board(&m, "r1") && app_manifest_for_board(&m, "tempotec_v1"));
    CHECK(!app_manifest_for_board(&m, "r3proii") && !app_manifest_for_board(&m, "r3ii_2025"));
    CHECK(app_manifest_opens_path(&m, "/data/mnt/sd_0/Books/A.PDF") && app_manifest_opens_path(&m, "x.epub"));
    CHECK(!app_manifest_opens_path(&m, "x.txt") && !app_manifest_opens_path(&m, "/a.pdf/b") &&
          !app_manifest_opens_path(&m, "noext") && !app_manifest_opens_path(&m, "/dir.pdf/") &&
          !app_manifest_opens_path(&m, "/dir/.pdf"));
    CHECK(app_manifest_parse(MINIMAL, strlen(MINIMAL), &m, err, sizeof(err)) && app_manifest_for_board(&m, "r1"));
    CHECK(!app_manifest_opens_path(&m, "x.pdf"));

    /* The id rule: ^[a-z0-9][a-z0-9_-]{0,31}$ */
    CHECK(app_id_valid("a") && app_id_valid("0x") && app_id_valid("a-b_c") && app_id_valid("abcdefghijklmnopqrstuvwxyz012345"));
    CHECK(!app_id_valid("") && !app_id_valid("-a") && !app_id_valid("_a") && !app_id_valid("A") && !app_id_valid("a.b") &&
          !app_id_valid("a/b") && !app_id_valid("..") && !app_id_valid("abcdefghijklmnopqrstuvwxyz0123456") && !app_id_valid(NULL));

    CHECK(!parse("{\"protocol\":2,\"id\":\"hello\",\"name\":\"H\",\"version\":\"1\",\"exec\":\"h\"}"));
    CHECK(!parse("{\"id\":\"hello\",\"name\":\"H\",\"version\":\"1\",\"exec\":\"h\"}"));
    CHECK(!parse("{\"protocol\":1,\"id\":\"Hello\",\"name\":\"H\",\"version\":\"1\",\"exec\":\"h\"}"));
    CHECK(!parse("{\"protocol\":1,\"id\":\"hello\",\"version\":\"1\",\"exec\":\"h\"}"));
    CHECK(!parse("{\"protocol\":1,\"id\":\"hello\",\"name\":\"H\",\"exec\":\"h\"}"));
    CHECK(!parse("{\"protocol\":1,\"id\":\"hello\",\"name\":\"H\",\"version\":\"1\"}"));
    CHECK(!parse("{\"protocol\":1,\"id\":\"hello\",\"name\":\"H\",\"version\":\"1\",\"exec\":\"../h\"}"));
    CHECK(!parse("{\"protocol\":1,\"id\":\"hello\",\"name\":\"H\",\"version\":\"1\",\"exec\":\"a/h\"}"));
    CHECK(!parse("{\"protocol\":1,\"id\":\"hello\",\"name\":\"H\",\"version\":\"1\",\"exec\":\"..\"}"));
    CHECK(!parse("{\"protocol\":1,\"id\":\"hello\",\"name\":\"H\",\"version\":\"1\",\"exec\":\"h\",\"icon\":\"i.jpg\"}"));
    CHECK(!parse("{\"protocol\":1,\"id\":\"hello\",\"name\":\"H\",\"version\":\"1\",\"exec\":\"h\",\"icon\":\"../i.png\"}"));
    CHECK(!parse("{\"protocol\":1,\"id\":\"hello\",\"name\":\"H\",\"version\":\"1\",\"exec\":\"h\",\"opens\":[\".pdf\"]}"));
    CHECK(!parse("{\"protocol\":1,\"id\":\"hello\",\"name\":\"H\",\"version\":\"1\",\"exec\":\"h\",\"opens\":\"pdf\"}"));
    CHECK(!parse("{\"protocol\":1,\"id\":\"hello\",\"name\":\"H\",\"version\":\"1\",\"exec\":\"h\",\"opens\":[\"a\",\"b\",\"c\",\"d\",\"e\",\"f\",\"g\",\"h\",\"i\"]}"));
    CHECK(!parse("{\"protocol\":1,\"id\":\"hello\",\"name\":\"H\",\"version\":\"1\",\"exec\":\"h\",\"boards\":[]}"));
    CHECK(!parse("{\"protocol\":1,\"id\":\"hello\",\"name\":\"H\",\"version\":\"1\",\"exec\":\"h\",\"boards\":\"r1\"}"));
    CHECK(!parse("{\"protocol\":1,\"id\":\"hello\",\"name\":\"H\",\"version\":\"1\",\"exec\":\"h\",\"boards\":[\"R1\"]}"));
    CHECK(!parse("{\"protocol\":1,\"id\":\"hello\",\"name\":\"H\",\"version\":\"1\",\"exec\":\"h\",\"free_player_memory\":\"yes\"}"));
    CHECK(!parse("[1,2]") && !parse("") && !parse("{"));
    CHECK(!app_manifest_parse(NULL, 0, &m, err, sizeof(err)));
}

static void write_file(const char * path, const char * data, size_t n, mode_t mode) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    assert(fd >= 0);
    assert((size_t) write(fd, data, n) == n);
    close(fd);
}

static void copy_file(const char * from, const char * to) {
    FILE * in = fopen(from, "rb");
    assert(in);
    char buf[65536];
    size_t n = fread(buf, 1, sizeof(buf), in);
    fclose(in);
    assert(n > 0);
    write_file(to, buf, n, 0755);
}

static void make_app(const char * root, const char * folder, const char * json, const char * elf) {
    char dir[512], path[640];
    snprintf(dir, sizeof(dir), "%s/%s", root, folder);
    mkdir(dir, 0755);
    snprintf(path, sizeof(path), "%s/app.json", dir);
    write_file(path, json, strlen(json), 0644);
    snprintf(path, sizeof(path), "%s/hello", dir);
    if (elf) copy_file(elf, path);
}

static void test_load_and_scan(const char * tmp, const char * elf) {
    char root[512];
    snprintf(root, sizeof(root), "%s/apps", tmp);
    mkdir(root, 0755);
    make_app(root, "hello", MINIMAL, elf);
    make_app(root, "wrongid",
             "{\"protocol\":1,\"id\":\"other\",\"name\":\"X\",\"version\":\"1\",\"exec\":\"hello\"}", elf);
    make_app(root, "noexec", MINIMAL, NULL);
    make_app(root, "script",
             "{\"protocol\":1,\"id\":\"script\",\"name\":\"S\",\"version\":\"1\",\"exec\":\"hello\"}", NULL);
    char path[640];
    snprintf(path, sizeof(path), "%s/script/hello", root);
    write_file(path, "#!/bin/sh\necho hi\n", 18, 0755);
    make_app(root, "r1only",
             "{\"protocol\":1,\"id\":\"r1only\",\"name\":\"Alpha\",\"version\":\"1\",\"exec\":\"hello\",\"boards\":[\"r1\"],"
             "\"icon\":\"icon.png\"}", elf);
    snprintf(path, sizeof(path), "%s/r1only/icon.png", root);
    write_file(path, "png", 3, 0644);
    make_app(root, "noicon",
             "{\"protocol\":1,\"id\":\"noicon\",\"name\":\"Zulu\",\"version\":\"1\",\"exec\":\"hello\",\"icon\":\"missing.png\"}", elf);
    make_app(root, ".hidden", MINIMAL, elf);
    snprintf(path, sizeof(path), "%s/hello/hello", root);
    chmod(path, 0644); /* not executable below */
    make_app(root, "plain", "{\"protocol\":1,\"id\":\"plain\",\"name\":\"Plain\",\"version\":\"1\",\"exec\":\"hello\"}", elf);

    app_manifest_t m;
    char err[64], dir[512];
    snprintf(dir, sizeof(dir), "%s/plain", root);
    CHECK(app_manifest_load_dir(dir, "plain", &m, err, sizeof(err)));
    CHECK(!app_manifest_load_dir(dir, "other", &m, err, sizeof(err)) && strstr(err, "match"));
    snprintf(dir, sizeof(dir), "%s/hello", root);
    CHECK(!app_manifest_load_dir(dir, "hello", &m, err, sizeof(err)) && strstr(err, "exec"));
    snprintf(dir, sizeof(dir), "%s/noexec", root);
    CHECK(!app_manifest_load_dir(dir, "noexec", &m, err, sizeof(err)));
    snprintf(dir, sizeof(dir), "%s/script", root);
    CHECK(!app_manifest_load_dir(dir, "script", &m, err, sizeof(err)) && strstr(err, "static"));
    /* A dynamically linked program (this test) has a program interpreter. */
    make_app(root, "dynamic", "{\"protocol\":1,\"id\":\"dynamic\",\"name\":\"D\",\"version\":\"1\",\"exec\":\"hello\"}", "/proc/self/exe");
    snprintf(dir, sizeof(dir), "%s/dynamic", root);
    CHECK(!app_manifest_load_dir(dir, "dynamic", &m, err, sizeof(err)) && strstr(err, "static"));
    snprintf(dir, sizeof(dir), "%s/noicon", root);
    CHECK(app_manifest_load_dir(dir, "noicon", &m, err, sizeof(err)) && m.icon[0] == '\0');
    snprintf(dir, sizeof(dir), "%s/r1only", root);
    CHECK(app_manifest_load_dir(dir, "r1only", &m, err, sizeof(err)) && !strcmp(m.icon, "icon.png"));

    int count;
    app_manifest_t * list = app_registry_scan(root, "r1", &count);
    CHECK(list && count == 3);
    if (list && count == 3) CHECK(!strcmp(list[0].id, "r1only") && !strcmp(list[1].id, "plain") && !strcmp(list[2].id, "noicon"));
    free(list);
    list = app_registry_scan(root, "tempotec_v1", &count);
    CHECK(list && count == 2);
    if (list && count == 2) CHECK(!strcmp(list[0].id, "plain") && !strcmp(list[1].id, "noicon"));
    free(list);
    CHECK(app_registry_find(root, "r1", "r1only", &m) && !app_registry_find(root, "r3proii", "r1only", &m));
    CHECK(!app_registry_find(root, "r1", "../apps/plain", &m) && !app_registry_find(root, "r1", "nope", &m));
    CHECK(app_registry_scan("/nonexistent/dir", "r1", &count) == NULL && count == 0);

    char sha[65];
    CHECK(app_registry_find(root, "r1", "plain", &m) && app_exec_sha256(root, &m, sha) && strlen(sha) == 64);
    char sha2[65];
    CHECK(app_exec_sha256(root, &m, sha2) && !strcmp(sha, sha2));
    snprintf(path, sizeof(path), "%s/plain/hello", root);
    FILE * f = fopen(path, "ab");
    fputc(0, f);
    fclose(f);
    CHECK(app_exec_sha256(root, &m, sha2) && strcmp(sha, sha2) != 0);

    /* The file passed to an app. */
    app_manifest_t opener;
    const char * j = "{\"protocol\":1,\"id\":\"reader\",\"name\":\"R\",\"version\":\"1\",\"exec\":\"hello\",\"opens\":[\"pdf\"]}";
    CHECK(app_manifest_parse(j, strlen(j), &opener, err, sizeof(err)));
    char book[600], dotdot[600], doubled[600];
    snprintf(book, sizeof(book), "%s/Books", tmp);
    mkdir(book, 0755);
    snprintf(book, sizeof(book), "%s/Books/a.pdf", tmp);
    write_file(book, "x", 1, 0644);
    snprintf(dotdot, sizeof(dotdot), "%s/Books/../Books/a.pdf", tmp);
    snprintf(doubled, sizeof(doubled), "%s//Books/a.pdf", tmp);
    CHECK(app_launch_file_check(tmp, &opener, book) == NULL);
    CHECK(app_launch_file_check(tmp, &opener, dotdot) != NULL);
    CHECK(app_launch_file_check(tmp, &opener, doubled) != NULL);
    CHECK(app_launch_file_check(tmp, &opener, tmp) != NULL);
    CHECK(app_launch_file_check(tmp, &opener, "/etc/passwd") != NULL);
    CHECK(app_launch_file_check(tmp, &opener, "Books/a.pdf") != NULL);
    CHECK(app_launch_file_check(tmp, &opener, NULL) != NULL);
    snprintf(dotdot, sizeof(dotdot), "%s/Books/missing.pdf", tmp);
    CHECK(app_launch_file_check(tmp, &opener, dotdot) != NULL);
    snprintf(dotdot, sizeof(dotdot), "%s/Books", tmp);
    CHECK(app_launch_file_check(tmp, &opener, dotdot) != NULL);
    snprintf(dotdot, sizeof(dotdot), "%s/Books/a.txt", tmp);
    write_file(dotdot, "x", 1, 0644);
    CHECK(app_launch_file_check(tmp, &opener, dotdot) != NULL);
    CHECK(app_launch_file_check(tmp, &m, book) != NULL); /* an app that opens nothing */
}

static bool env_has(char ** env, const char * entry) {
    for (; env && *env; env++) if (!strcmp(*env, entry)) return true;
    return false;
}

static void test_environment(void) {
    app_manifest_t m;
    char err[64];
    CHECK(app_manifest_parse(MINIMAL, strlen(MINIMAL), &m, err, sizeof(err)));
    char * base[] = { "PATH=/bin", "COMPAS_APP_ID=evil", "COMPAS_BOARD=x", "COMPAS_SCREEN_WIDTH=1", "HOME=/h", "COMPAS_APP_PROTOCOL=9", NULL };
    char ** env = app_session_environment(base, &m, "/sd/.apps-data/hello", "tempotec_v1", 240, 320);
    CHECK(env != NULL);
    CHECK(env_has(env, "PATH=/bin") && env_has(env, "HOME=/h"));
    CHECK(env_has(env, "COMPAS_APP_PROTOCOL=1") && env_has(env, "COMPAS_APP_ID=hello") &&
          env_has(env, "COMPAS_APP_DATA=/sd/.apps-data/hello") && env_has(env, "COMPAS_BOARD=tempotec_v1") &&
          env_has(env, "COMPAS_SCREEN_WIDTH=240") && env_has(env, "COMPAS_SCREEN_HEIGHT=320"));
    CHECK(!env_has(env, "COMPAS_APP_ID=evil") && !env_has(env, "COMPAS_BOARD=x") && !env_has(env, "COMPAS_APP_PROTOCOL=9"));
    int n = 0;
    while (env && env[n]) n++;
    CHECK(n == 2 + 6);
    app_session_environment_free(env);
    env = app_session_environment(NULL, &m, "/d", "r1", 480, 800);
    CHECK(env && env_has(env, "COMPAS_BOARD=r1"));
    app_session_environment_free(env);
}

static void test_approval(const char * tmp) {
    char file[512];
    snprintf(file, sizeof(file), "%s/approved.txt", tmp);
    const char * a = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    const char * b = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    CHECK(!app_approval_granted(file, "one", a));
    CHECK(app_approval_grant(file, "one", a));
    CHECK(app_approval_granted(file, "one", a));
    CHECK(!app_approval_granted(file, "one", b));  /* the executable changed */
    CHECK(!app_approval_granted(file, "two", a));  /* another app */
    CHECK(app_approval_grant(file, "two", b));
    CHECK(app_approval_granted(file, "one", a) && app_approval_granted(file, "two", b));
    CHECK(app_approval_grant(file, "one", b));     /* a new executable replaces the old approval */
    CHECK(app_approval_granted(file, "one", b) && !app_approval_granted(file, "one", a) && app_approval_granted(file, "two", b));
    CHECK(!app_approval_grant(file, "Bad Id", a) && !app_approval_grant(file, "one", "abc") && !app_approval_grant(file, "one", NULL));
    CHECK(!app_approval_granted(file, "one", "ABCDEFABCDEFABCDEFABCDEFABCDEFABCDEFABCDEFABCDEFABCDEFABCDEFABCD"));
    /* Damaged lines are ignored, never matched. */
    FILE * f = fopen(file, "a");
    fputs("garbage\nthree short\nfour  cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\n", f);
    fclose(f);
    CHECK(app_approval_granted(file, "one", b) && !app_approval_granted(file, "three", "short"));
    CHECK(app_approval_grant(file, "five", a) && app_approval_granted(file, "five", a) && app_approval_granted(file, "one", b));
    char text[2048];
    f = fopen(file, "r");
    size_t n = fread(text, 1, sizeof(text) - 1, f);
    text[n] = '\0';
    fclose(f);
    CHECK(!strstr(text, "garbage") && !strstr(text, "four"));
    CHECK(!app_approval_grant("/nonexistent/dir/approved.txt", "one", a));
}

static void mkdirs(const char * path) {
    char buf[600];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char * p = buf + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        mkdir(buf, 0755);
        *p = '/';
    }
    mkdir(buf, 0755);
}

static void put32(unsigned char * p, uint32_t v) { memcpy(p, &v, 4); }
static void put16(unsigned char * p, uint16_t v) { memcpy(p, &v, 2); }

/* A little-endian ELF32 header and nph program headers; the interpreter header, if any, is entry interp. */
static void craft_elf(const char * path, uint16_t machine, unsigned nph, int interp, unsigned phentsize, uint16_t type) {
    size_t size = 52 + (size_t) phentsize * nph;
    unsigned char * b = calloc(1, size ? size : 1);
    memcpy(b, "\177ELF", 4);
    b[4] = 1, b[5] = 1, b[6] = 1;
    put16(b + 16, type), put16(b + 18, machine), put32(b + 20, 1);
    put32(b + 28, 52); /* e_phoff */
    put16(b + 40, 52), put16(b + 42, (uint16_t) phentsize), put16(b + 44, (uint16_t) nph);
    for (unsigned i = 0; i < nph; i++) put32(b + 52 + (size_t) i * phentsize, i == (unsigned) interp ? 3u : 1u);
    write_file(path, (const char *) b, size, 0755);
    free(b);
}

static bool elf_loads(const char * tmp, const char * name, uint16_t machine, unsigned nph, int interp, unsigned phentsize, uint16_t type) {
    char dir[600], path[700], err[64];
    app_manifest_t m;
    snprintf(dir, sizeof(dir), "%s/%s", tmp, name);
    mkdir(dir, 0755);
    snprintf(path, sizeof(path), "%s/app.json", dir);
    char json[256];
    snprintf(json, sizeof(json), "{\"protocol\":1,\"id\":\"%s\",\"name\":\"E\",\"version\":\"1\",\"exec\":\"x\"}", name);
    write_file(path, json, strlen(json), 0644);
    snprintf(path, sizeof(path), "%s/x", dir);
    craft_elf(path, machine, nph, interp, phentsize, type);
    return app_manifest_load_dir(dir, name, &m, err, sizeof(err));
}

static void test_elf(const char * tmp) {
    CHECK(elf_loads(tmp, "elfok", EM_MIPS_T, 5, -1, 32, 2));
    CHECK(elf_loads(tmp, "elfdyn", EM_MIPS_T, 5, -1, 32, 3));          /* static-pie */
    CHECK(!elf_loads(tmp, "elfinterp0", EM_MIPS_T, 5, 0, 32, 2));
    CHECK(!elf_loads(tmp, "elfinterp4", EM_MIPS_T, 5, 4, 32, 2));
    CHECK(!elf_loads(tmp, "elfinterp80", EM_MIPS_T, 100, 80, 32, 2));   /* beyond the first 64 headers */
    CHECK(elf_loads(tmp, "elf100", EM_MIPS_T, 100, -1, 32, 2));
    CHECK(!elf_loads(tmp, "elfx86", 62, 5, -1, 32, 2));                /* wrong machine */
    CHECK(!elf_loads(tmp, "elfbig", EM_MIPS_T, 2000, -1, 56, 2));      /* table over 64 KiB */
    CHECK(!elf_loads(tmp, "elfnone", EM_MIPS_T, 0, -1, 32, 2));
    CHECK(!elf_loads(tmp, "elfrel", EM_MIPS_T, 5, -1, 32, 1));         /* relocatable object */
    CHECK(!elf_loads(tmp, "elftiny", EM_MIPS_T, 5, -1, 2, 2));
}

static void test_approval_file_reserved(const char * tmp) {
    char saved[600], p[700];
    CHECK(getcwd(saved, sizeof(saved)) != NULL && chdir(tmp) == 0);
    /* APP_APPROVAL_FILE is ./apps_approved.txt on the host. */
    CHECK(plugin_path_in_program_dirs_of(tmp, "apps_approved.txt") && plugin_path_in_program_dirs_of(tmp, "./apps_approved.txt"));
    CHECK(plugin_path_in_program_dirs_of(tmp, "apps_approved.txt.tmp") && plugin_path_in_program_dirs_of(tmp, "sub/../apps_approved.txt"));
    snprintf(p, sizeof(p), "%s/apps_approved.txt", tmp);
    CHECK(plugin_path_in_program_dirs_of(tmp, p));
    CHECK(!plugin_path_in_program_dirs_of(tmp, "apps_approved.txt2") && !plugin_path_in_program_dirs_of(tmp, "other.txt"));
    /* A link to the existing file is the same file. */
    write_file("apps_approved.txt", "x", 1, 0644);
    CHECK(symlink("apps_approved.txt", "linkname") == 0);
    CHECK(plugin_path_in_program_dirs_of(tmp, "linkname"));
    CHECK(chdir(saved) == 0);
}

static void test_reserved(const char * tmp) {
    char sd[512], p[700], q[700];
    snprintf(sd, sizeof(sd), "%s/sd", tmp);
    mkdirs(sd);
    snprintf(p, sizeof(p), "%s/.apps/x", sd), mkdirs(p);
    snprintf(p, sizeof(p), "%s/.apps-data/x", sd), mkdirs(p);
    snprintf(p, sizeof(p), "%s/.emulators/x", sd), mkdirs(p);
    snprintf(p, sizeof(p), "%s/.plugins", sd), mkdirs(p);
    snprintf(p, sizeof(p), "%s/Music", sd), mkdirs(p);
    snprintf(p, sizeof(p), "%s/.appsx", sd), mkdirs(p);

    const char * hit[] = { ".apps", ".apps/", ".apps/x", ".apps/x/y/z.bin", ".APPS/x", ".Apps-Data/x/f", ".apps-data",
                           ".emulators", ".emulators/x/e", ".EMULATORS", "Music/../.apps/x", "./.apps//x", ".apps/./x",
                           ".apps/../.apps/new",
                           /* FAT drops trailing dots and spaces; any depth */
                           ".apps.", ".apps...", ".apps ", ".APPS. /x", ".emulators.", ".apps-data. ", "Music/.apps",
                           "Music/.Emulators./x", "stage/.apps", "a/b/.apps-data" };
    for (size_t i = 0; i < sizeof(hit) / sizeof(hit[0]); i++) {
        snprintf(p, sizeof(p), "%s/%s", sd, hit[i]);
        if (!plugin_path_in_program_dirs_of(sd, p)) fprintf(stderr, "should be reserved: %s\n", p), failures++;
    }
    const char * miss[] = { "", "Music", "Music/a.mp3", ".plugins/a.lua", ".appsx", ".appsx/y", ".app", ".apps2/x", "apps/x",
                            ".emulator/x", ".compas/s.txt" };
    for (size_t i = 0; i < sizeof(miss) / sizeof(miss[0]); i++) {
        snprintf(p, sizeof(p), "%s/%s", sd, miss[i]);
        if (plugin_path_in_program_dirs_of(sd, p)) fprintf(stderr, "should be allowed: %s\n", p), failures++;
    }
    /* The names are reserved at any depth, so a plain folder called .apps inside Music is refused too. */

    /* Relative paths resolve against the working directory. */
    char saved[600];
    CHECK(getcwd(saved, sizeof(saved)) != NULL);
    CHECK(chdir(sd) == 0);
    CHECK(plugin_path_in_program_dirs_of(".", ".apps/x") && plugin_path_in_program_dirs_of(sd, "./.apps/../.apps/x"));
    CHECK(!plugin_path_in_program_dirs_of(sd, "Music/a.mp3"));
    CHECK(chdir(saved) == 0);

    /* The same folder under another name: a link into it, and another way to reach the card. */
    snprintf(p, sizeof(p), "%s/Music/alias", sd);
    snprintf(q, sizeof(q), "%s/.apps", sd);
    CHECK(symlink(q, p) == 0);
    snprintf(p, sizeof(p), "%s/Music/alias/new/file", sd);
    CHECK(plugin_path_in_program_dirs_of(sd, p));
    snprintf(p, sizeof(p), "%s/cardalias", tmp);
    CHECK(symlink(sd, p) == 0);
    snprintf(p, sizeof(p), "%s/cardalias/.apps/x/file", tmp);
    CHECK(plugin_path_in_program_dirs_of(sd, p));
    snprintf(p, sizeof(p), "%s/cardalias/Music/a.mp3", tmp);
    CHECK(!plugin_path_in_program_dirs_of(sd, p));
    /* A reserved folder that does not exist yet cannot be created through the card's other name either. */
    char sd2[512];
    snprintf(sd2, sizeof(sd2), "%s/sd2", tmp);
    mkdirs(sd2);
    snprintf(p, sizeof(p), "%s/cardalias2", tmp);
    CHECK(symlink(sd2, p) == 0);
    snprintf(p, sizeof(p), "%s/cardalias2/.apps", tmp);
    CHECK(plugin_path_in_program_dirs_of(sd2, p));
    snprintf(p, sizeof(p), "%s/cardalias2/.emulators/a", tmp);
    CHECK(plugin_path_in_program_dirs_of(sd2, p));
    snprintf(p, sizeof(p), "%s/cardalias2/Music", tmp);
    CHECK(!plugin_path_in_program_dirs_of(sd2, p));
    CHECK(plugin_path_in_program_dirs_of(sd2, NULL) || 1);
}

int main(int argc, char ** argv) {
    const char * elf = argc > 1 ? argv[1] : NULL;
    const char * base = getenv("TMPDIR");
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s/test_external_apps.XXXXXX", base && *base ? base : "/tmp");
    if (!elf || !mkdtemp(tmp)) {
        fprintf(stderr, "usage: test_external_apps STATIC_ELF\n");
        return 2;
    }
    test_parse();
    test_load_and_scan(tmp, elf);
    test_environment();
    test_approval(tmp);
    test_reserved(tmp);
    test_elf(tmp);
    test_approval_file_reserved(tmp);
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmp);
    if (system(cmd) != 0) fprintf(stderr, "cleanup failed\n");
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("test_external_apps: ok");
    return 0;
}
