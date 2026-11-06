#include "src/library/metadata/db/metadata_db_internal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

bool metadata_db_db_ready = true;
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
int32_t tagcache_live_count(void) { return 0; }
bool tagcache_song_fields_at_title_rank(int32_t rank, unsigned fields, tagcache_song_t *out) {
    (void)rank; (void)fields; (void)out; return false;
}
bool tagcache_group_at(int kind, int row, tagcache_group_t *out) {
    (void)kind;
    if (row < 0 || row >= count) return false;
    reads++;
    out->name = names[row];
    return true;
}
static void check(const char * const *rows, int n, int a, int z, int other) {
    names = rows; count = n; reads = 0;
    int jump[27];
    metadata_db_get_album_az_table_sorted(METADATA_DB_ALBUM_SORT_REVERSE, jump);
    assert(jump[0] == a && jump[25] == z && jump[26] == other);
}
int main(void) {
    const char *mixed[] = { "10cc", "20cc", "Abba", "Another", "Zappa" };
    check(mixed, 5, 1, 0, 3);
    const char *letters[] = { "Abba", "Another", "Zappa" };
    check(letters, 3, 1, 0, -1);
    const char *nonlatin[] = { "Abba", "Zappa", "\xc3\x89" "clair" };
    check(nonlatin, 3, 2, 1, 0);
    check(NULL, 0, -1, -1, -1);
    puts("metadata reverse A-Z: ok");
    return 0;
}
