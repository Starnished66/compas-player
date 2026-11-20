#include "src/library/tagcache/core/tagcache_internal.h"
#include "src/library/metadata/scan/library_walk.h"
#include "src/library/metadata/parsing/metadata.h"
#include "src/library/browser/paths/library_paths.h"
#include <assert.h>
#include <fcntl.h>
#include <sys/stat.h>

/* The cue parser is linked for the walk but never asked about tags here. */
void metadata_ape_text_items(const char *path, metadata_text_item_fn item, void *ctx) {
    (void) path; (void) item; (void) ctx;
}

static const char *skipped_path, *changed_path;

static bool visit(const char *path, void *user) {
    (void) user;
    if (skipped_path && !strcmp(path, skipped_path)) return true;
    int32_t mtime = changed_path && !strcmp(path, changed_path) ? 2 : 1;
    if (tagcache_lookup(path, mtime, 1, NULL)) return true;
    tagcache_upsert_year_changed(path, mtime, 1, "t", "Artist", "Album", "Artist", "g", 1, 1, -1, NULL);
    return true;
}

/* An ordinary (non-forced) scan of ./music, like the library update. */
static void scan(void) {
    tagcache_begin_update_with_lock(NULL, NULL);
    int count = 0, skipped = 0;
    assert(file_browser_walk_all_songs_excluding_top_level(MUSIC_ROOT_DIR, AUDIOBOOKS_LIBRARY_DIR_NAME,
                                                           visit, NULL, &count, NULL, &skipped));
    assert(tagcache_end_update_with_lock(NULL, NULL));
}

static void touch(const char *path) {
    int fd = open(path, O_CREAT | O_WRONLY, 0644);
    assert(fd >= 0);
    close(fd);
}

static bool indexed(const char *path) { return tagcache_song_by_path(path, NULL); }

#define KEEP "./music/keep/a.mp3"
#define B "./music/ign/b.mp3"
#define LOCKED "./music/locked/e.mp3"
#define LOCKED_DEEP "./music/locked/sub/f.mp3"
#define MOVED "./music/moved/f.mp3"
#define MOVED_NEW "./music/moved_new/f.mp3"
#define CLONE "./music/clone/a.mp3"
#define FILE_LINK "./music/keep/link.mp3"
#define LOOP "./music/loop/x.mp3"
#define TOFILE "./music/tofile/y.mp3"

/* The walk skips symlinks, so rows reached only through one are excluded like
 * deleted files, while a plain unseen file keeps its row. */
static void check_symlinks(void) {
    scan();
    assert(indexed(MOVED) && indexed(CLONE) && indexed(FILE_LINK));
    /* A folder replaced by a link to where it moved. */
    assert(rename("./music/moved", "./music/moved_new") == 0);
    assert(symlink("moved_new", "./music/moved") == 0);
    /* A folder replaced by a link to another indexed folder. */
    assert(unlink(CLONE) == 0 && rmdir("./music/clone") == 0);
    assert(symlink("keep", "./music/clone") == 0);
    /* A file replaced by a link; a plain file the walk merely misses. */
    assert(unlink(FILE_LINK) == 0 && symlink("a.mp3", FILE_LINK) == 0);
    skipped_path = B;
    scan();
    skipped_path = NULL;
    assert(!indexed(MOVED) && indexed(MOVED_NEW) && !indexed(CLONE) && !indexed(FILE_LINK));
    assert(indexed(KEEP) && indexed(B));
    assert(unlink("./music/moved") == 0 && unlink("./music/clone") == 0 && unlink(FILE_LINK) == 0);
}

/* A link as the only change must publish (the lazy gate sees it) and drop the row. */
static void check_symlink_only_publication(void) {
    touch(FILE_LINK);
    scan();
    assert(indexed(FILE_LINK));
    int32_t generation = tagcache_generation();
    assert(unlink(FILE_LINK) == 0 && symlink("a.mp3", FILE_LINK) == 0);
    scan();
    assert(tagcache_generation() != generation && !indexed(FILE_LINK) && indexed(KEEP));
    assert(unlink(FILE_LINK) == 0);
}

/* A folder replaced by a link to itself or to a file: the leaf lstat fails
 * (ELOOP, ENOTDIR) but the link above it is positive, so the rows go. */
static void check_dead_links(void) {
    scan();
    assert(indexed(LOOP) && indexed(TOFILE));
    assert(unlink(LOOP) == 0 && rmdir("./music/loop") == 0 && symlink("loop", "./music/loop") == 0);
    assert(unlink(TOFILE) == 0 && rmdir("./music/tofile") == 0 && symlink("keep/a.mp3", "./music/tofile") == 0);
    scan();
    assert(!indexed(LOOP) && !indexed(TOFILE) && indexed(KEEP));
    assert(unlink("./music/loop") == 0 && unlink("./music/tofile") == 0);
}

/* A pass that finds every file unchanged publishes nothing, however many rows
 * it keeps unseen; a changed, added or deleted file still publishes. */
static void check_kept_rows_publish_nothing(void) {
    scan();
    int32_t generation = tagcache_generation();
    skipped_path = B;                       /* kept: the file exists, the walk missed it */
    scan();
    skipped_path = NULL;
    assert(tagcache_generation() == generation && indexed(B));
    changed_path = KEEP;
    scan();
    changed_path = NULL;
    assert(tagcache_generation() != generation);
    generation = tagcache_generation();
    touch("./music/keep/new.mp3");
    skipped_path = B;
    scan();
    skipped_path = NULL;
    assert(tagcache_generation() != generation && indexed("./music/keep/new.mp3"));
    generation = tagcache_generation();
    assert(unlink("./music/keep/new.mp3") == 0);
    scan();
    assert(tagcache_generation() != generation && !indexed("./music/keep/new.mp3"));
}

/* The music root itself may be a link: the walk starts there, so rows stay. */
static void check_root_link(void) {
    assert(rename("./music", "./music_real") == 0 && symlink("music_real", "./music") == 0);
    skipped_path = B;
    scan();
    skipped_path = NULL;
    assert(indexed(KEEP) && indexed(B));
    assert(unlink("./music") == 0 && rename("./music_real", "./music") == 0);
}

/* ENOENT rule: unreadable folders and merely unseen files keep their rows. */
static void check_enoent_rule(void) {
    /* As root chmod 0 does not make a folder unreadable. */
    if (geteuid() != 0) {
        assert(chmod("./music/locked", 0) == 0);
        scan();
        /* The deep file's own lstat fails and so does the probe of its folder. */
        assert(indexed(LOCKED) && indexed(LOCKED_DEEP));
        assert(chmod("./music/locked", 0755) == 0);
    }
    skipped_path = KEEP;
    scan();
    skipped_path = NULL;
    assert(indexed(KEEP));
    assert(unlink(KEEP) == 0);
    scan();
    assert(!indexed(KEEP) && indexed(B));
}

int main(void) {
    char root[] = "/tmp/compas-ignore-XXXXXX";
    assert(mkdtemp(root));
    assert(chdir(root) == 0);
    const char *dirs[] = {"music", "music/keep", "music/ign", "music/locked", "music/locked/sub", "music/moved", "music/clone", "music/loop", "music/tofile", "db"};
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) assert(mkdir(dirs[i], 0755) == 0);
    const char *files[] = {KEEP, B, LOCKED, LOCKED_DEEP, MOVED, CLONE, FILE_LINK, LOOP, TOFILE};
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) touch(files[i]);
    assert(tagcache_open("db"));
    check_symlinks();
    check_symlink_only_publication();
    check_dead_links();
    check_kept_rows_publish_nothing();
    check_root_link();
    check_enoent_rule();
    tagcache_close();
    assert(chdir("/") == 0);
    char command[64];
    snprintf(command, sizeof(command), "rm -rf '%s'", root);
    assert(system(command) == 0);
    return 0;
}
