#define _GNU_SOURCE
#include "../plugin_storage.c"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void *test_long_symlink_chain(void *unused) {
    (void)unused;
    char path[PATH_MAX] = "", cwd[PATH_MAX];
    assert(getcwd(cwd, sizeof(cwd)));
    for (int i = 0; i < 15; i++) strcat(path, "/proc/self/root");
    strcat(path, cwd);
    strcat(path, "/outside/new-file");
    assert(!plugin_storage_path_is_reserved(path));
    strcpy(path, "");
    for (int i = 0; i < 41; i++) strcat(path, "/proc/self/root");
    assert(plugin_storage_path_is_reserved(path));
    return NULL;
}

static void test_small_stack(void) {
    pthread_attr_t attr;
    pthread_t thread;
    assert(pthread_attr_init(&attr) == 0);
    assert(pthread_attr_setstacksize(&attr, 128 * 1024) == 0);
    assert(pthread_create(&thread, &attr, test_long_symlink_chain, NULL) == 0);
    assert(pthread_join(thread, NULL) == 0);
    pthread_attr_destroy(&attr);
}

int main(void) {
    char tmp[] = "/tmp/plugin-path-resolution.XXXXXX";
    assert(mkdtemp(tmp));
    assert(chdir(tmp) == 0);
    assert(mkdir(".compas", 0700) == 0);
    assert(mkdir(".compas/plugins", 0700) == 0);
    assert(mkdir(".compas/plugins/child", 0700) == 0);
    assert(mkdir("outside", 0700) == 0);
    assert(mkdir("outside/dir", 0700) == 0);
    assert(symlink("../.compas/plugins/child", "outside/link") == 0);
    assert(symlink("../.compas/plugins/newdir", "outside/dangling") == 0);

    assert(plugin_storage_path_is_reserved(".compas/plugins/new-file"));
    assert(plugin_storage_path_is_reserved("outside/link/../secret"));
    assert(plugin_storage_path_is_reserved("outside/dangling/new-file"));
    assert(!plugin_storage_path_is_reserved("outside/dir/../ordinary-file"));
    assert(!plugin_storage_path_is_reserved("outside/ordinary-file"));
    test_small_stack();
    puts("PASS symlink/parent traversal and harmless parent-path resolution");
    return 0;
}
