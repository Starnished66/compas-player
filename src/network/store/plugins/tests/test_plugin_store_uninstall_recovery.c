#define HOST_BUILD 1
#include "../plugin_store_workers.c"

#include <assert.h>
#include <ctype.h>
#include <limits.h>
#include <errno.h>
#include <sys/types.h>

static bool test_same_card = true;
static bool test_writable = true;
bool usb_mode_control_storage_write_blocked(void) { return !test_writable; }
const char *i18n_tr(const char *s) { return s; }
bool __wrap_ps_storage_path_writable(const char *path, char *error, size_t size) {
    (void)path;
    if (error && size) error[0] = '\0';
    return test_writable;
}
bool __wrap_ps_card_still(const store_card_t *card) { (void)card; return test_same_card; }

static void mkdir_path(const char * p) {
    char cmd[PATH_MAX + 16];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", p);
    assert(system(cmd) == 0);
}
static void put_file(const char * p, const char * value) {
    FILE * f = fopen(p, "wb");
    assert(f);
    assert(fwrite(value, 1, strlen(value), f) == strlen(value));
    assert(fclose(f) == 0);
}
static bool file_is(const char * p, const char * value) {
    char got[128] = {0};
    FILE * f = fopen(p, "rb");
    if (!f) return false;
    size_t n = fread(got, 1, sizeof(got) - 1, f);
    fclose(f);
    return n == strlen(value) && strcmp(got, value) == 0;
}
static const char * test_base;
static int test_root_number;
static void make_root(char root[PATH_MAX]) {
    assert(snprintf(root, PATH_MAX, "%s/card-%d", test_base, ++test_root_number) < PATH_MAX);
    assert(mkdir(root, 0700) == 0);
    assert(chdir(root) == 0);
    mkdir_path("music/.compas/store");
    mkdir_path("music/.plugins");
    mkdir_path("music/Themes/plug");
}
static void entry_init(plugin_store_plugin_t * p) {
    memset(p, 0, sizeof(*p));
    strcpy(p->id, "plug");
    strcpy(p->version, "1.2");
    p->file_count = 2;
    strcpy(p->files[0].dest, "Themes/plug/one.lua");
    strcpy(p->files[0].sha256, "0000000000000000000000000000000000000000000000000000000000000000");
    p->files[0].size = 3;
    strcpy(p->files[1].dest, "Themes/plug/two.lua");
    strcpy(p->files[1].sha256, "1111111111111111111111111111111111111111111111111111111111111111");
    p->files[1].size = 3;
}
static void stage_create(const char *stage, const plugin_store_plugin_t *p) {
    (void)p;
    assert(mkdir(stage, 0700) == 0);
}
static void backup_file(const char * source, const char * stage, size_t i) {
    char backup[PATH_MAX];
    ps_stage_file(backup, sizeof(backup), stage, i, "backup");
    assert(rename(source, backup) == 0);
}

static void recovery_case(bool listed, bool collision, bool same, bool writable, bool short_journal) {
    char root[PATH_MAX];
    make_root(root);
    plugin_store_plugin_t entry;
    entry_init(&entry);
    store_card_t card = {0};
    const char *stage = "music/.compas/store/uninstall-plug.123.1";
    put_file("music/Themes/plug/one.lua", "old-one");
    put_file("music/Themes/plug/two.lua", "old-two");
    stage_create(stage, &entry);
    backup_file("music/Themes/plug/one.lua", stage, 0);
    if (short_journal) put_file("music/.compas/store/uninstall-plug.123.1/journal.v1", "bad");
    if (collision) put_file("music/Themes/plug/one.lua", "new-one");
    test_same_card = same;
    test_writable = writable;
    recover_uninstalls(&card, &entry, listed ? 1 : 0);
    assert(file_is("music/Themes/plug/two.lua", "old-two"));
    if (!same || !writable) {
        assert(file_is("music/.compas/store/uninstall-plug.123.1/0.backup", "old-one"));
    } else if (collision) {
        assert(file_is("music/Themes/plug/one.lua", "new-one"));
        assert(file_is("music/.compas/store/uninstall-plug.123.1/0.backup", "old-one"));
    } else if (listed) {
        assert(file_is("music/Themes/plug/one.lua", "old-one"));
    } else {
        assert(access("music/Themes/plug/one.lua", F_OK) != 0);
    }
    test_same_card = test_writable = true;
}

static void recovery_missing_parent(void) {
    char root[PATH_MAX];
    make_root(root);
    plugin_store_plugin_t entry;
    entry_init(&entry);
    strcpy(entry.id, "compas.soundstage");
    const char *stage = "music/.compas/store/uninstall-compas.soundstage.123.1";
    stage_create(stage, &entry);
    put_file("music/Themes/plug/one.lua", "recover-me");
    backup_file("music/Themes/plug/one.lua", stage, 0);
    assert(rmdir("music/Themes/plug") == 0);
    store_card_t card = {0};
    recover_uninstalls(&card, &entry, 1);
    assert(file_is("music/.compas/store/uninstall-compas.soundstage.123.1/0.backup", "recover-me"));
    mkdir_path("music/Themes/plug");
    recover_uninstalls(&card, &entry, 1);
    assert(file_is("music/Themes/plug/one.lua", "recover-me"));
    assert(access(stage, F_OK) != 0);
    char id[64];
    assert(!uninstall_stage_id("uninstall-compas.soundstage.bad.1", id));
    assert(!uninstall_stage_id("uninstall-compas.soundstage.1.bad", id));
}

int main(int argc, char **argv) {
    assert(argc == 2);
    test_base = argv[1];
    recovery_missing_parent();
    recovery_case(true, false, true, true, false);
    recovery_case(true, true, true, true, false);
    recovery_case(false, false, true, true, false);
    recovery_case(true, false, false, true, false);
    recovery_case(true, false, true, false, false);
    recovery_case(true, false, true, true, true);
    puts("plugin uninstall recovery: PASS");
    return 0;
}
