#ifndef HIBY_TAGCACHE_INTERNAL_H
#define HIBY_TAGCACHE_INTERNAL_H

/* Private declarations shared by the tagcache implementation files:
 *   core/tagcache.c          loaded state, open/close/load, read API
 *   core/tagcache_storage.c  directory pinning, file names, generation refs and pointer, headers
 *   core/tagcache_numeric.c  deferred rating/play-count writes
 *   core/tagcache_scan.c     scan staging, lookup and upsert
 *   core/tagcache_commit.c   generation commit, end/abort of a scan
 *   core/tagcache_snapshot.c immutable path snapshots
 *   reader/tagcache_reader.c reader, index and query code (includes the reader and query headers)
 *
 * Locking: the caller's query lock serializes the whole API except
 * tagcache_numeric_wait_durable() and snapshot path reads (own dup'd fds,
 * atomic reference count). During a scan the scan thread releases the query
 * lock through the unlock/lock callbacks while it builds staging files and
 * indexes; other threads then read committed_reader and may call the stat
 * setters. selected_reader is per thread, update_failed is atomic.
 * numeric_mutex guards the numeric queue; numeric_epoch lets the reader retry
 * around a flush. */
#include "tagcache.h"
#include "../../../core/system/lifecycle/dir_fd_path.h"
#include "../../../core/persistence/storage_paths.h"
#include "tagcache_generation_refs.h"
#include "../../artwork/support/library_endian.h"
#include "../../../core/system/logging/db_log.h"
#include "../../../core/text/utf8_util.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

enum tag_type {
    tag_artist = 0,
    tag_album,
    tag_genre,
    tag_title,
    tag_filename,
    tag_composer,
    tag_comment,
    tag_albumartist,
    tag_grouping,
    tag_year,
    tag_discnumber,
    tag_tracknumber,
    tag_virt_canonicalartist,
    tag_bitrate,
    tag_length,
    tag_playcount,
    tag_rating,
    tag_playtime,
    tag_lastplayed,
    tag_commitid,
    tag_mtime,
    tag_lastelapsed,
    tag_lastoffset,
    TAG_COUNT
};

#define TAGCACHE_MAGIC 0x54434810
#define TAGCACHE_INDEXED_MAGIC 0x54434811
#define FLAG_DELETED 0x0001
#define FLAG_WAV_ID3_TAGS 0x0002 /* persisted: WAV tags read with ID3 precedence */
#define FLAG_ORIGINAL_YEAR_TAGS 0x0004 /* persisted: tags read with original-year precedence (one re-read of every file) */
#define FLAG_SEEN 0x01000000 /* RAM-only, stripped before persist */
#define FLAG_TAGS_INCOMPLETE 0x02000000 /* RAM-only: unique-tag seek missed on load */
#define FLAG_RAM_ONLY (FLAG_SEEN | FLAG_TAGS_INCOMPLETE)
#ifndef TAGCACHE_MAX_ENTRIES
#define TAGCACHE_MAX_ENTRIES 524288
#endif
#define TAGCACHE_MAX_STAGING_ENTRIES (TAGCACHE_MAX_ENTRIES * 2)

struct tagfile_entry {
    int32_t tag_length;
    int32_t idx_id;
};

struct index_entry {
    int32_t tag_seek[TAG_COUNT];
    int32_t flag;
};

struct tagcache_header {
    int32_t magic;
    int32_t datasize;
    int32_t entry_count;
};

struct master_header {
    struct tagcache_header tch;
    int32_t serial;
    int32_t commitid;
    int32_t dirty;
};



#define TAGCACHE_SORT_TEXT_PREFIX 40
#define TAGCACHE_SORT_PATH_PREFIX 64

#define READER_SORT_CACHE_ROWS 64
#define TC_QUERY_BLOCK_VALUES 128
typedef struct {
    int32_t entries, live, serial, commitid, generation;
    uint32_t loaded_index_version, title_order_version;
    int master_fd, tag_fd[TAG_COUNT];
    size_t tag_size[TAG_COUNT];
    bool fds_ready, requires_indexes, legacy_active, compact_sort, compact_order, sort_failed;
    int query_fd, artist_names_fd, title_fd, recency_fd, path_fd, rank_fd[2], group_fd[3], members_fd[3], group_n[3], sort_kind;
    int album_order_fd[3], album_inverse_fd[3];
    int album_year_known_count; /* -1 until the current year index is queried */
    unsigned char *join_bits; /* 1 bit per slot: album artist projected, see tagcache_reader_join.h */
    bool query_cache_valid;
    uint64_t query_cache_block;
    uint32_t query_cache_values[TC_QUERY_BLOCK_VALUES];
    struct { bool valid; int32_t slot; unsigned loaded; tagcache_song_t song; } sort_cache[READER_SORT_CACHE_ROWS];
} reader_context_t;
#define READER (selected_reader ? selected_reader : &committed_reader)
#define ent_count (READER->entries)
#define live_count (READER->live)
#define master_serial (READER->serial)
#define master_commitid (READER->commitid)
#define disk_gen (READER->generation)
#define reader_loaded_index_version (READER->loaded_index_version)
#define reader_title_order_version (READER->title_order_version)
#define reader_master_fd (READER->master_fd)
#define reader_tag_fd (READER->tag_fd)
#define reader_tag_size (READER->tag_size)
#define reader_fds_ready (READER->fds_ready)
#define reader_requires_indexes (READER->requires_indexes)
#define reader_legacy_active (READER->legacy_active)
#define reader_compact_sort (READER->compact_sort)
#define reader_compact_order (READER->compact_order)
#define reader_sort_failed (READER->sort_failed)
#define reader_sort_kind (READER->sort_kind)
#define reader_sort_cache (READER->sort_cache)
#define reader_query_fd (READER->query_fd)
#define reader_artist_names_fd (READER->artist_names_fd)
#define reader_title_fd (READER->title_fd)
#define reader_recency_fd (READER->recency_fd)
#define reader_path_fd (READER->path_fd)
#define reader_rank_fd (READER->rank_fd)
#define reader_group_fd (READER->group_fd)
#define reader_members_fd (READER->members_fd)
#define reader_group_n (READER->group_n)
#define reader_album_order_fd (READER->album_order_fd)
#define reader_album_inverse_fd (READER->album_inverse_fd)
#define reader_join_bits (READER->join_bits)
#define reader_query_cache_valid (READER->query_cache_valid)
#define reader_query_cache_block (READER->query_cache_block)
#define reader_query_cache_values (READER->query_cache_values)

#define TC_INDEX_VERSION TAGCACHE_DATABASE_FORMAT
/* Older indexes are rewritten only below this version; v8 only adds the join marks. */
#define TC_INDEX_MIGRATE_BELOW 7u

struct tc_index_meta {
    uint32_t magic;
    uint32_t version;
    int32_t generation, serial, commitid;
    int32_t entry_count;
    int32_t live_entries;
    int32_t group_count[3];
    uint64_t size[18];
};


static inline bool checked_add_size(size_t a, size_t b, size_t * out) {
    if (SIZE_MAX - a < b) return false;
    *out = a + b;
    return true;
}

static inline bool checked_mul_size(size_t a, size_t b, size_t * out) {
    if (a != 0 && b > SIZE_MAX / a) return false;
    *out = a * b;
    return true;
}


extern char db_dir[512];
extern char db_logical_dir[512];
extern int db_dir_fd;
extern struct stat db_dir_identity;
extern bool db_dir_identity_valid;
extern const int persist_tag_ids[TAGCACHE_REFS_COUNT];
extern bool db_open;
extern bool disk_ready;
extern bool rebuild_preserve_generations;
extern tagcache_load_outcome_t last_load_outcome;
extern reader_context_t committed_reader;
extern reader_context_t scan_reader;
extern reader_context_t build_reader;
extern _Thread_local reader_context_t *selected_reader;
extern bool scan_view_ready;
extern bool join_albums_request;
extern bool join_republish_request;
extern bool updating;
extern _Atomic bool update_failed;
extern bool scan_lazy_active;
extern bool scan_force_publication;
extern bool scan_preserve_unseen;
extern bool scan_initializing;
extern int scan_numeric_fd;
extern bool scan_tag_private[TAG_COUNT];
extern int32_t *commit_slot_map;
extern int32_t commit_slot_map_count;
extern atomic_uint numeric_epoch;
extern const int reader_persist_tags[6];

/* Scan and commit work use the existing worker's selected reader. */
#define scan_selected (selected_reader == &scan_reader)
static inline void scan_select(bool selected) {
    if (scan_view_ready) selected_reader = selected ? &scan_reader : &committed_reader;
}
typedef struct {
    reader_context_t *previous;
} scan_scope_t;
static inline void scan_scope_leave(scan_scope_t *scope) {
    if (scope) selected_reader = scope->previous;
}
#define SCAN_SCOPE scan_scope_t scan_scope __attribute__((cleanup(scan_scope_leave))) = { selected_reader }; scan_select(true)


/* tagcache_storage.c */
uint32_t fnv1a(const char * s, size_t n);
unsigned char ascii_fold(unsigned char c);
int ascii_casecmp(const char * a, const char * b);
bool tagcache_sort_prefix(const char *value, unsigned char *prefix, size_t width);
int tagcache_sort_prefix_compare(const unsigned char *a, const unsigned char *b, size_t width, bool *full_compare);
const char * ascii_casestr(const char * hay, const char * needle);
int db_logical_directory_status(void);
bool db_identity_current(void);
bool db_pin_directory(const char * dir);
bool tc_read_full(int fd, void *buf, size_t size, off_t offset);
bool write_at(int fd, const void *data, size_t size, off_t offset);
bool write_fully(int fd, const void * buf, size_t n);
bool close_synced(int fd);
int persisted_tag_index(int tag);
bool resolve_tag_generation(int32_t gen, int tag, int32_t *out_source);
bool write_generation_refs(int32_t gen, const int32_t source[TAGCACHE_REFS_COUNT]);
bool tag_file_path(char * out, size_t n, int tag, int32_t gen);
bool master_file_path(char * out, size_t n, int32_t gen);
bool write_gen_pointer(int32_t gen);
int read_gen_pointer(int32_t * out);
void unlink_generation(int32_t gen);
void unlink_other_generations(int32_t keep, int32_t previous);
bool scan_generation_max(int32_t * out_max);
bool validate_master_header(const struct master_header * mh, size_t file_size);
bool validate_tag_header(const struct tagcache_header * hdr, size_t file_size);
bool collect_load_generations(int32_t ** out, size_t * count, int32_t pointed, bool * saved_files, bool * pointer_file);

/* tagcache_numeric.c */
bool tagcache_flush_numeric(void);
void numeric_worker_start(void);
void numeric_worker_stop(void);
void queue_numeric_update(int32_t slot, const struct index_entry *idx, int *waiter);
void numeric_overlay(int32_t slot, struct index_entry *idx);

/* tagcache_scan.c */
void scan_seen_release(void);
void scan_session_drop(void);
bool scan_seen_test(int32_t slot);
size_t scan_hash_mem_available_bytes(void);
void scan_hash_release(void);
bool scan_intern_init(void);
bool scan_write_index(int32_t slot, const struct index_entry *idx);
bool scan_intern_find(const char *value, char *out, size_t out_size);
bool replay_scan_numeric(int fd);
void scan_intern_close(void);
bool start_staging_with_lock(void (*unlock)(void), void (*lock)(void));
int32_t find_path(const char *path);
void song_set_stats(tagcache_song_t *song, const struct index_entry *idx);

/* tagcache.c */
void reader_context_init(reader_context_t *context);
bool reload_from_disk(void);

/* reader/tagcache_reader.c */
void reader_close(void);
bool reader_open(int32_t gen);
bool reader_index(int32_t slot, struct index_entry *out);
bool reader_string(int tag, int32_t seek, int32_t slot, char *out, size_t out_size);
bool reader_song_fields_raw(int32_t slot, unsigned fields, tagcache_song_t *out);
bool reader_song_fields(int32_t slot, unsigned fields, tagcache_song_t *out);
bool reader_song(int32_t slot, tagcache_song_t *out);
bool tc_sort_fd(int fd, size_t record_size, int64_t count, int (*cmp)(const void *, const void *, void *), void *ctx, int scratch_fd);
bool choose_intern_strings(int32_t n);
bool reader_write_indexes(int32_t gen);
int reader_open_indexes(int32_t gen);
void reader_unlink_indexes(int32_t gen);
int reader_create_temp(void);
void reader_close_indexes(void);
void reader_reset_cache(void);
bool reader_build_indexes(void);
bool reader_migrate_indexes(int32_t gen);
int32_t reader_find_path(const char *path);
bool reader_order_song(int fd, bool recency, int32_t rank, tagcache_song_t *out);
bool reader_group_at(int kind, int index, tagcache_group_t *out);
int reader_group_index(int kind, const char *name, const char *aa);
int reader_group_ids(int kind, int group_index, int offset, int32_t *ids, int max);
int32_t reader_rank_of(int32_t slot, bool recency);
bool reader_join_build(bool enabled);
bool reader_legacy_init(void);
bool legacy_order_song(bool recency, int32_t rank, tagcache_song_t *out);
int legacy_group_count(int kind);

#endif /* HIBY_TAGCACHE_INTERNAL_H */
