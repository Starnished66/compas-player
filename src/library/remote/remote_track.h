#ifndef REMOTE_TRACK_H
#define REMOTE_TRACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Native-side descriptor for one track from a provider-neutral remote music
 * plugin (Qobuz/Tidal/etc, see PLUGINS.md's plugin.play_remote() docs).
 * Populated by plugin_manager.c from a Lua table, consumed by gui.c (queue
 * display, ReplayGain, format badge) and audio.c (decoder_open()'s remote
 * branch). Never holds the plugin's own Lua state or any pointer that
 * outlives a single populate call -- every field is a bounded, owned copy. */
typedef struct {
    char provider[64];
    char track_id[128];

    char title[128];
    char artist[128];
    char album[128];
    uint32_t duration_ms;

    char artwork_url[1536];

    /* Matches decoder_open()'s DECODER_* dispatch by name -- "mp3"/"flac"/
     * "aac" (case-insensitive) -- so a remote track never needs the
     * extension/Content-Type sniffing local files and radio streams rely
     * on (see audio.c's decoder_open() remote branch). Empty defaults to
     * "mp3", matching stream_format_hint()'s own no-hint default. */
    char codec[16];
    unsigned int sample_rate;
    unsigned int bit_depth;
    unsigned int channels;
    unsigned int bitrate_kbps;

    bool has_replaygain;
    double replaygain_db;

    /* The REAL, possibly time-limited fetch URL -- deliberately never the
     * playlist[]/last_track key (see remote_track_make_key()'s comment). */
    char stream_url[2048];
    bool verify_tls;
    /* Resolver bookkeeping. owner is an opaque Lua main-state token and is
     * never dereferenced by this module. */
    uintptr_t resolver_owner;
    /* plugin.define id of the submitting plugin; a reloaded instance of the
     * same plugin adopts its queue (remote_track_stream_adopt_owner()). */
    char resolver_plugin[64];
    bool resolve_stream;
    uint32_t url_ttl_s;
    uint64_t stream_fetched_ms;
} remote_track_meta_t;

typedef struct {
    uint64_t request_id;
    uintptr_t owner;
    char provider[64], track_id[128];
} remote_stream_request_t;

/* "remote://<provider>/<track_id>" -- the stable synthetic playlist[] key
 * for a remote track. Never the actual stream_url (which may be a single-use
 * or expiring signed URL), so Favorites, Most-Played, and History state
 * keyed by path can accumulate reliably.
 *
 * Returns false (and leaves out untouched) if provider/track_id are empty,
 * don't fit, or contain '/' or a control character -- '/' specifically
 * because the two fields are joined with '/' with no escaping: without
 * this check, provider="a/b" + track_id="c" would produce the exact same
 * key as provider="a" + track_id="b/c", silently colliding two unrelated
 * tracks' Favorites/History/play-count state. Plugin-supplied provider/
 * track_id are expected to be simple identifiers (e.g. "qobuz", a numeric
 * or UUID track id) -- this is not a general escaping scheme, just enough
 * to keep the key unambiguous for those. */
bool remote_track_make_key(const char * provider, const char * track_id, char * out, size_t out_size);

/* True for any "remote://..." path, without needing a live table entry --
 * used by resume-on-launch/last_track saving to recognize the scheme even
 * after the table itself has been replaced by a later queue. */
bool remote_track_path_is_remote(const char * path);

/* Replaces the whole remote-track table for the newly built queue -- same
 * lifetime as gui.c's own subsonic_stream_meta (see that array's comment):
 * only ever replaced wholesale on the next remote queue, never proactively
 * freed just because some other source starts playing, safe because the
 * synthetic key changes whenever the provider/track_id does. Copies every
 * entry; the caller's array is not retained.
 *
 * All-or-nothing: every entry's provider/track_id must produce a valid key
 * (see remote_track_make_key()) or this returns false and the table is left
 * exactly as it was -- never partially replaced. Also returns false (table
 * likewise left untouched) if the copy itself can't be allocated. count is
 * silently capped at an internal maximum (matches plugin_manager.c's own
 * PLUGIN_MAX_LIST_ITEMS cap on any queue-list plugin API) rather than
 * failing outright, since the Lua-facing caller has already capped its own
 * input to that same bound. Safe to call from the UI thread only -- see
 * remote_track_meta_copy_for_path()'s own comment for why concurrent
 * access from other threads is fine regardless. */
bool remote_track_meta_set_all(const remote_track_meta_t * entries, int count);

/* Copies the entry matching `path` into *out (caller-owned storage) while
 * holding this module's internal lock, and returns true -- or returns
 * false (leaving *out untouched) if path isn't "remote://" or doesn't
 * match any current table entry. Deliberately never returns a pointer
 * into the shared table: audio.c's decoder_open() calls this from the
 * audio thread and a seek-worker thread, while remote_track_meta_set_all()
 * above (a brand new plugin.play_remote()/queue_remote_list() call) can
 * run on the UI thread at any time and frees the previous table once it's
 * replaced -- a borrowed pointer handed out here could otherwise be read
 * out from under a concurrent free(). */
bool remote_track_meta_copy_for_path(const char * path, remote_track_meta_t * out);
bool remote_track_meta_copy_fresh_for_path(const char *path, remote_track_meta_t *out);

/* Selects a new active path/generation and wakes canceled waiters. A queued
 * or dispatched prefetch for the newly selected different path is promoted
 * into the generation; same-path restarts clear old IDs and failures so they
 * can retry. */
void remote_track_stream_begin_playback(const char *path, uint64_t generation);
bool remote_track_stream_is_fresh(const char *path);
bool remote_track_stream_prefetch(const char *path, uint64_t generation);
bool remote_track_stream_wait(const char *path, uint64_t generation, remote_track_meta_t *out);
bool remote_track_stream_take_request(remote_stream_request_t *out);
bool remote_track_stream_provide(uintptr_t owner, uint64_t request_id,
                                 const remote_track_meta_t *stream, const char *error);
void remote_track_stream_cancel_owner(uintptr_t owner);
/* Hands entries submitted by plugin_id to its new owner token. */
void remote_track_stream_adopt_owner(const char *plugin_id, uintptr_t owner);
/* Called whenever a request becomes ready to dispatch, from any thread. */
void remote_track_stream_set_request_notify(void (*notify)(void));
void remote_track_stream_cancel_all(void);
bool remote_track_stream_error_for_path(const char *path, uint64_t generation,
                                        char *out, size_t out_size);
#ifdef REMOTE_TRACK_TESTING
void remote_track_test_set_clock(uint64_t now_ms);
void remote_track_test_use_real_clock(void);
void remote_track_test_set_wait_timeout_ms(unsigned int timeout_ms);
unsigned int remote_track_test_waiter_count(void);
unsigned int remote_track_test_pending_count(void);
bool remote_track_test_wait_for_waiters(unsigned int count);
#endif

#endif
