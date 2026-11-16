#include "src/library/metadata/db/metadata_db_internal.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "src/core/text/utf8_util.h"
#include <string.h>
#include <stdatomic.h>
#include <sched.h>
#include <unistd.h>

bool metadata_db_db_ready = true;
uint32_t metadata_db_db_incarnation;
static int32_t generation = 1;
static bool indexed = true;
int32_t tagcache_generation(void) { return generation; }
bool tagcache_title_ranks_indexed(void) { return indexed; }
static atomic_bool yield_probe, released, acquired;
static bool change_generation, probe_test;
int __real_pthread_mutex_unlock(pthread_mutex_t *mutex);
int __wrap_pthread_mutex_unlock(pthread_mutex_t *mutex) {
    int result = __real_pthread_mutex_unlock(mutex);
    if (mutex == &metadata_db_mutex && atomic_exchange(&yield_probe, false)) {
        atomic_store(&released, true);
        while (!atomic_load(&acquired)) sched_yield();
    }
    return result;
}
static void *between_probes(void *unused) {
    (void)unused;
    while (!atomic_load(&released)) sched_yield();
    pthread_mutex_lock(&metadata_db_mutex);
    if (change_generation) generation++;
    pthread_mutex_unlock(&metadata_db_mutex);
    atomic_store(&acquired, true);
    return NULL;
}
pthread_once_t metadata_db_mutex_once = PTHREAD_ONCE_INIT;
pthread_mutex_t metadata_db_mutex;
_Thread_local unsigned metadata_query_depth;
void metadata_db_mutex_init(void) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&metadata_db_mutex, &attr);
    pthread_mutexattr_destroy(&attr);
}
static const char * const *names;
static int count, reads;
int tagcache_group_count(int kind) { (void)kind; return count; }
int32_t tagcache_live_count(void) { return count; }
bool tagcache_song_fields_at_title_rank(int32_t rank, unsigned fields, tagcache_song_t *out) {
    (void)fields;
    if (rank < 0 || rank >= count) return false;
    reads++;
    if (probe_test && reads == 1) atomic_store(&yield_probe, true);
    /* A name starting with '/' is an untagged song: empty title, that path. */
    out->path = names[rank];
    out->title = names[rank][0] == '/' ? "" : names[rank];
    return true;
}
/* Test double of the reader's display title (title, else file name). */
const char *tagcache_song_display_title(const tagcache_song_t *song) {
    if (song->title[0]) return song->title;
    const char *slash = strrchr(song->path, '/');
    return slash ? slash + 1 : song->path;
}
bool tagcache_group_at(int kind, int row, tagcache_group_t *out) {
    (void)kind;
    if (row < 0 || row >= count) return false;
    reads++;
    snprintf(out->name_storage, sizeof(out->name_storage), "%s", names[row]);
    out->name = out->name_storage;
    return true;
}
static int order_version;
int tagcache_title_order_version(void) { return order_version; }
struct fixture { const char *name; char bucket; };
static bool check_songs_only;
static const char *display_name(const char *name) {
    return name[0] == '/' ? strrchr(name, '/') + 1 : name;
}
static int compare_rows(const void *a, const void *b) {
    const struct fixture *x = a, *y = b;
    const char *xn = display_name(x->name), *yn = display_name(y->name);
    return order_version == 3 ? utf8_collate_identity_compare_v6(xn, yn)
                              : utf8_collate_identity_compare(xn, yn);
}
static void check(struct fixture *rows, int n) {
    static const char *sorted[64];
    qsort(rows, n, sizeof(*rows), compare_rows);
    for (int i = 0; i < n; i++) sorted[i] = rows[i].name;
    names = sorted; count = n; reads = 0;
    for (int kind = check_songs_only ? METADATA_DB_AZ_ALL_SONGS : METADATA_DB_AZ_ARTIST;
         kind <= METADATA_DB_AZ_ALL_SONGS; kind++) {
        for (int reverse = 0; reverse <= 1; reverse++) {
            int expected[27], jump[27], wrapper[27];
            for (int i = 0; i < 27; i++) expected[i] = -1;
            for (int display = 0; display < n; display++) {
                int row = reverse ? n - 1 - display : display;
                int bucket = rows[row].bucket == '#' ? 26 : rows[row].bucket - 'a';
                if (expected[bucket] < 0) expected[bucket] = display;
            }
            if (reverse) {
                for (int i = 1; i < 26; i++)
                    if (expected[i] < 0) expected[i] = expected[i - 1];
            } else {
                for (int i = 24; i >= 0; i--)
                    if (expected[i] < 0) expected[i] = expected[i + 1];
            }
            metadata_db_get_az_table_sorted(kind, reverse, jump);
            assert(!memcmp(jump, expected, sizeof(jump)));
            if (!reverse) {
                metadata_db_get_az_table(kind, wrapper);
                assert(!memcmp(jump, wrapper, sizeof(jump)));
            }
            if (kind == METADATA_DB_AZ_ALBUM) {
                metadata_db_get_album_az_table_sorted(reverse ? METADATA_DB_ALBUM_SORT_REVERSE : 0, wrapper);
                assert(!memcmp(jump, wrapper, sizeof(jump)));
            }
        }
    }
}
int main(void) {
    for (order_version = 3; order_version <= 4; order_version++) {
        struct fixture mixed[] = {
            {"Abba", 'a'}, {"Éclair", 'e'}, {"Zappa", 'z'},
            {"Örebro", 'o'}, {"Ñandu", 'n'}, {"\"Zombie\"", 'z'},
            {"(Untitled)", 'u'}, {"'Til", 't'}, {"10cc", '#'},
            {"20cc", '#'}, {"Жук", '#'}, {"Another", 'a'},
            {"Echo", 'e'}, {"November", 'n'}, {"Oscar", 'o'},
            {"¡Bravo", order_version == 3 ? '#' : 'b'},
            {"¿Que", order_version == 3 ? '#' : 'q'}, {"", '#'}, {"...", '#'}
        };
        check(mixed, sizeof(mixed) / sizeof(*mixed));
        struct fixture letters[] = {{"Abba", 'a'}, {"Another", 'a'}, {"Zappa", 'z'}};
        check(letters, 3);
        struct fixture trailing[] = {{"Abba", 'a'}, {"Zappa", 'z'}, {"Жук", '#'}};
        check(trailing, 3);
        struct fixture leading[] = {{"10cc", '#'}, {"20cc", '#'}, {"Abba", 'a'}, {"Zappa", 'z'}};
        check(leading, 4);
        struct fixture accented[] = {{"Abba", 'a'}, {"Zappa", 'z'}, {"Éclair", 'e'}};
        check(accented, 3);
        struct fixture others[] = {{"10cc", '#'}, {"Жук", '#'}};
        check(others, 2);
        check(letters, 0);
        /* Untagged songs sort by file name; checked for songs only, since
         * group names are never empty-title paths. */
        struct fixture untagged[] = {{"Alpha", 'a'}, {"/m/Middle.flac", 'm'}, {"Zulu", 'z'},
                                     {"/m/10 Intro.flac", '#'}, {"Beta", 'b'}};
        check_songs_only = true;
        check(untagged, sizeof(untagged) / sizeof(*untagged));
        check_songs_only = false;
    }
    static const char *probe_names[] = {"Alpha", "Zulu"};
    names = probe_names;
    count = 2;
    probe_test = true;
    alarm(10);
    for (int changed = 0; changed < 2; changed++) {
        reads = 0;
        change_generation = changed;
        atomic_store(&released, false);
        atomic_store(&acquired, false);
        pthread_t contender;
        assert(!pthread_create(&contender, NULL, between_probes, NULL));
        int jump[27];
        int32_t built_generation;
        bool available;
        bool valid = metadata_db_build_az_table(METADATA_DB_AZ_ALL_SONGS, false,
                                                jump, &built_generation, &available);
        pthread_join(contender, NULL);
        assert(atomic_load(&acquired));
        assert(valid == !changed);
        if (changed) for (int i = 0; i < 27; i++) assert(jump[i] == -1);
        else assert(jump[0] == 0 && jump[25] == 1);
    }
    probe_test = false;
    indexed = false;
    reads = 0;
    int jump[27];
    int32_t built_generation;
    bool available = true;
    assert(metadata_db_build_az_table(METADATA_DB_AZ_ALL_SONGS, false,
                                      jump, &built_generation, &available));
    assert(!available && !reads);
    alarm(0);
    puts("metadata reverse A-Z: ok");
    return 0;
}
