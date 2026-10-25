/* Characterization for gui_library's no-LVGL album/player decoder and artist resolver.
 * The runner includes exact frozen/current production definitions before this TU's main. */
#include "albumart.h"
#include "artwork_coordinator.h"
#include "artwork_failure_cache.h"
#include "artwork_memory.h"
#include "cover_decode.h"
#include "cue_parser.h"
#include "metadata.h"
#include "metadata_db.h"
#include "albumart_info.h"
#include "gui.h"
#include "library_artwork_internal.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef HOST_BUILD
#error "this characterization is host-only"
#endif

static song_row_t db_song;
static group_row_t db_album;
static int db_artist_album_count;
static metadata_artwork_result_t metadata_result;
static const uint8_t * metadata_picture;
static uint32_t metadata_picture_size;
static unsigned metadata_read_count;
static bool cue_fixture_enabled;

bool db_log_enabled(void) { return false; }
void db_log(const char * area, const char * fmt, ...) { (void) area; (void) fmt; }
uint64_t db_log_now_ms(void) { return 1; }
long db_log_rss_kb(void) { return 0; }
bool audio_is_playing(void) { return false; }

bool cue_reference_is(const char * reference) {
    return cue_fixture_enabled && reference &&
           strcmp(reference, "music/CueArtist/disc.cue") == 0;
}
bool cue_reference_audio_path(const char * reference, char * out, size_t size) {
    if (!cue_reference_is(reference) || !out || size == 0) return false;
    snprintf(out, size, "%s", "music/CueArtist/track.mp3");
    return true;
}

metadata_artwork_result_t metadata_read_artwork_isolated(const char * path,
        track_metadata_t * out, int timeout_ms, artwork_priority_t priority) {
    (void) path; (void) timeout_ms; (void) priority;
    metadata_read_count++;
    memset(out, 0, sizeof(*out));
    if (metadata_result == METADATA_ARTWORK_FOUND && metadata_picture && metadata_picture_size) {
        out->picture_data = malloc(metadata_picture_size);
        assert(out->picture_data);
        memcpy(out->picture_data, metadata_picture, metadata_picture_size);
        out->picture_size = metadata_picture_size;
        snprintf(out->artist, sizeof(out->artist), "%s", db_song.tags.artist);
        snprintf(out->album, sizeof(out->album), "%s", db_song.tags.album);
        snprintf(out->album_artist, sizeof(out->album_artist), "%s", db_song.tags.album_artist);
    }
    return metadata_result;
}

bool metadata_db_get_song_by_id(int64_t id, song_row_t * out) {
    if (id != db_song.id) return false;
    *out = db_song;
    return true;
}

int metadata_db_get_album_songs(const char * album, const char * album_artist,
        int offset, song_row_t * out, int max_rows) {
    (void) album; (void) album_artist; (void) offset; (void) out; (void) max_rows;
    return 0;
}

int64_t metadata_db_count_albums_for_group(metadata_db_group_kind_t kind, const char * name) {
    (void) kind; (void) name;
    return db_artist_album_count;
}

int metadata_db_get_albums_for_group(metadata_db_group_kind_t kind, const char * name,
        int offset, int max_rows, group_row_t * out) {
    (void) kind; (void) name;
    if (offset != 0 || max_rows <= 0 || db_artist_album_count == 0) return 0;
    out[0] = db_album;
    return 1;
}

int64_t metadata_db_get_album_for_group_offset(metadata_db_group_kind_t kind, const char * name,
        const char * album, const char * album_artist) {
    (void) kind;
    return db_artist_album_count > 0 && strcmp(name, db_song.tags.artist) == 0 &&
           strcmp(album, db_song.tags.album) == 0 && strcmp(album_artist, db_song.tags.album_artist) == 0
        ? 0 : -1;
}

#ifdef LIBRARY_ART_BASELINE
#define DB_LOG(area, ...) db_log(area, __VA_ARGS__)
#include "library_artwork_decode_fixture.inc"
#else
#include "library_album_art_decode.c"
#include "library_artist_art.c"
#endif

static void set_song(int64_t id, const char * path, const char * artist,
                     const char * album, const char * album_artist);

static void test_cue_reference_row_path(void) {
    set_song(100, "music/CueArtist/disc.cue", "CueArtist", "CueAlbum", "CueArtist");
    cue_fixture_enabled = true;
    albumart_info_t info;
    albumart_info_from_song_row(&db_song, &info);
    cue_fixture_enabled = false;
    assert(strcmp(info.path, "music/CueArtist/track.mp3") == 0);
    assert(strcmp(info.artist, "CueArtist") == 0 && strcmp(info.album, "CueAlbum") == 0);
    puts("CUE-backed song rows resolve the audio path before artwork lookup");
}

static void ensure_dir(const char * path) {
    char copy[PATH_MAX];
    assert(strlen(path) < sizeof(copy));
    strcpy(copy, path);
    for (char * p = copy + 1; *p; ++p) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(copy, 0755) != 0) assert(errno == EEXIST);
        *p = '/';
    }
    if (mkdir(copy, 0755) != 0) assert(errno == EEXIST);
}

static void write_bytes(const char * path, const void * bytes, size_t size) {
    FILE * f = fopen(path, "wb");
    assert(f);
    assert(fwrite(bytes, 1, size, f) == size);
    assert(fclose(f) == 0);
}

static void write_bmp(const char * path, uint8_t red, uint8_t green, uint8_t blue) {
    /* Minimal valid 2x2, 24-bit BMP with padded eight-byte scanlines. */
    uint8_t bmp[70] = {0};
    bmp[0] = 'B'; bmp[1] = 'M';
    bmp[2] = sizeof(bmp); bmp[10] = 54; bmp[14] = 40;
    bmp[18] = 2; bmp[22] = 2; bmp[26] = 1; bmp[28] = 24;
    for (size_t row = 0; row < 2; ++row) {
        for (size_t col = 0; col < 2; ++col) {
            size_t p = 54 + row * 8 + col * 3;
            bmp[p] = blue; bmp[p + 1] = green; bmp[p + 2] = red;
        }
    }
    write_bytes(path, bmp, sizeof(bmp));
}

static size_t read_bmp(const char * path, uint8_t * out, size_t capacity) {
    FILE * f = fopen(path, "rb");
    assert(f);
    size_t size = fread(out, 1, capacity, f);
    assert(!ferror(f));
    assert(fclose(f) == 0);
    return size;
}

static void set_song(int64_t id, const char * path, const char * artist,
                     const char * album, const char * album_artist) {
    memset(&db_song, 0, sizeof(db_song));
    db_song.id = id;
    snprintf(db_song.path, sizeof(db_song.path), "%s", path);
    snprintf(db_song.tags.artist, sizeof(db_song.tags.artist), "%s", artist);
    snprintf(db_song.tags.album, sizeof(db_song.tags.album), "%s", album);
    snprintf(db_song.tags.album_artist, sizeof(db_song.tags.album_artist), "%s", album_artist);
    db_album = (group_row_t){0};
    db_album.first_song_id = id;
    snprintf(db_album.name, sizeof(db_album.name), "%s", album);
    snprintf(db_album.album_artist, sizeof(db_album.album_artist), "%s", album_artist);
}

static albumart_info_t song_info(void) {
    albumart_info_t info = {0};
    snprintf(info.path, sizeof(info.path), "%s", db_song.path);
    snprintf(info.artist, sizeof(info.artist), "%s", db_song.tags.artist);
    snprintf(info.album, sizeof(info.album), "%s", db_song.tags.album);
    snprintf(info.albumartist, sizeof(info.albumartist), "%s", db_song.tags.album_artist);
    return info;
}

static bool cancel_now(void * user_data) { (void) user_data; return true; }

static void expect_rgb565(const uint16_t * pixels, uint16_t expected) {
    assert(pixels);
    assert(pixels[36 * ALBUM_THUMBNAIL_PX + 36] == expected);
}

static void test_sidecar_decode_and_persist(void) {
    ensure_dir("music/SidecarArtist/SidecarAlbum");
    write_bytes("music/SidecarArtist/SidecarAlbum/track.mp3", "audio", 5);
    write_bmp("music/SidecarArtist/SidecarAlbum/cover.bmp", 255, 0, 0);
    set_song(101, "music/SidecarArtist/SidecarAlbum/track.mp3", "SidecarArtist", "SidecarAlbum", "SidecarArtist");
    artwork_failure_cache_clear();
    uint16_t * pixels = NULL;
    bool loaded = album_thumbnail_load_or_decode_ex(&db_song, ARTWORK_PRIO_THUMBNAIL,
                                                     NULL, NULL, &pixels);
    assert(loaded && pixels);
    expect_rgb565(pixels, 0xf800);
    free(pixels);
    albumart_info_t info = song_info();
    char cached[PATH_MAX];
    assert(albumart_generated_cache_find(&info, ALBUM_THUMBNAIL_PX, ALBUM_THUMBNAIL_PX,
                                         cached, sizeof(cached)));
    puts("album sidecar decode -> owned RGB565 -> persistent 72px cache");
}

static void test_corrupt_sidecar_embedded_fallback(void) {
    ensure_dir("music/FallbackArtist/FallbackAlbum");
    write_bytes("music/FallbackArtist/FallbackAlbum/track.mp3", "audio", 5);
    write_bytes("music/FallbackArtist/FallbackAlbum/cover.bmp", "bad", 3);
    static uint8_t bmp[70];
    write_bmp("music/FallbackArtist/FallbackAlbum/embedded-fixture.bmp", 0, 255, 0);
    size_t size = read_bmp("music/FallbackArtist/FallbackAlbum/embedded-fixture.bmp", bmp, sizeof(bmp));
    set_song(102, "music/FallbackArtist/FallbackAlbum/track.mp3", "FallbackArtist", "FallbackAlbum", "FallbackArtist");
    metadata_result = METADATA_ARTWORK_FOUND;
    metadata_picture = bmp; metadata_picture_size = (uint32_t) size;
    metadata_read_count = 0;
    artwork_failure_cache_clear();
    uint16_t * pixels = NULL;
    bool loaded = album_thumbnail_load_or_decode_ex(&db_song, ARTWORK_PRIO_THUMBNAIL,
                                                     NULL, NULL, &pixels);
    assert(loaded && pixels && metadata_read_count == 1);
    expect_rgb565(pixels, 0x07e0);
    free(pixels);
    puts("permanent sidecar decode failure falls through to embedded picture");
}

static void test_cancel_and_memory_temporary(void) {
    ensure_dir("music/RetryArtist/RetryAlbum");
    write_bytes("music/RetryArtist/RetryAlbum/track.mp3", "audio", 5);
    set_song(103, "music/RetryArtist/RetryAlbum/track.mp3", "RetryArtist", "RetryAlbum", "RetryArtist");
    artwork_failure_cache_clear();
    uint16_t * pixels = NULL;
    bool loaded = album_thumbnail_load_or_decode_with_mtime(&db_song, ARTWORK_PRIO_THUMBNAIL,
        cancel_now, NULL, 0, false, NULL, false, &pixels);
    assert(!loaded && pixels == NULL);
    artwork_fail_reason_t reason = ARTWORK_FAIL_NONE;
    assert(!artwork_failure_cache_is_blocked(db_song.id, 0, &reason));

    system_set_mock_mem_available(1);
    metadata_result = METADATA_ARTWORK_FOUND;
    metadata_read_count = 0;
    loaded = album_thumbnail_load_or_decode_ex(&db_song, ARTWORK_PRIO_THUMBNAIL,
                                                NULL, NULL, &pixels);
    system_set_mock_mem_available(0);
    assert(!loaded && pixels == NULL && metadata_read_count == 0);
    assert(artwork_failure_cache_is_blocked(db_song.id, album_source_mtime(&db_song, &(albumart_info_t){ .path = "music/RetryArtist/RetryAlbum/track.mp3", .artist = "RetryArtist", .album = "RetryAlbum", .albumartist = "RetryArtist" }), &reason));
    assert(reason == ARTWORK_FAIL_TEMPORARY);
    puts("cancel remains unrecorded; memory admission failure is temporary/retryable");
}

static void test_player_embedded_negative_and_cache_materialization(void) {
    ensure_dir("music/PlayerArtist/PlayerAlbum");
    write_bytes("music/PlayerArtist/PlayerAlbum/track.mp3", "audio", 5);
    write_bmp("music/PlayerArtist/PlayerAlbum/picture.bmp", 0, 0, 255);
    static uint8_t bmp[70];
    size_t size = read_bmp("music/PlayerArtist/PlayerAlbum/picture.bmp", bmp, sizeof(bmp));
    set_song(104, "music/PlayerArtist/PlayerAlbum/track.mp3", "PlayerArtist", "PlayerAlbum", "PlayerArtist");
    metadata_result = METADATA_ARTWORK_FOUND;
    metadata_picture = bmp; metadata_picture_size = (uint32_t) size;
    artwork_failure_cache_clear();
    uint16_t * pixels = NULL;
    bool no_art = true;
    bool loaded = gui_library_generate_player_cover(db_song.path, db_song.tags.artist,
        db_song.tags.album, db_song.tags.album_artist, NULL, NULL, &pixels, &no_art);
    assert(loaded && pixels && !no_art);
    assert(pixels[240 * COVER_ART_WIDTH + 240] == 0x001f);
    free(pixels);
    char cached[PATH_MAX];
    assert(gui_library_album_cover_cached(db_song.id, ALBUM_PLAYER_CACHE_PX, cached, sizeof(cached)));

    ensure_dir("music/EmptyArtist/EmptyAlbum");
    write_bytes("music/EmptyArtist/EmptyAlbum/track.mp3", "audio", 5);
    set_song(105, "music/EmptyArtist/EmptyAlbum/track.mp3", "EmptyArtist", "EmptyAlbum", "EmptyArtist");
    metadata_result = METADATA_ARTWORK_NOT_FOUND;
    metadata_picture = NULL; metadata_picture_size = 0;
    no_art = false; pixels = NULL;
    loaded = gui_library_generate_player_cover(db_song.path, db_song.tags.artist,
        db_song.tags.album, db_song.tags.album_artist, NULL, NULL, &pixels, &no_art);
    assert(!loaded && pixels == NULL && no_art);
    puts("player embedded fallback materializes 480px cache; confirmed empty art is distinguished");
}

static void test_artist_own_image_alias_and_negative(void) {
    ensure_dir("music/OwnArtist/Album");
    write_bytes("music/OwnArtist/Album/track.mp3", "audio", 5);
    write_bmp("music/OwnArtist/artist.jpg", 255, 255, 0);
    set_song(106, "music/OwnArtist/Album/track.mp3", "OwnArtist", "Album", "OwnArtist");
    artwork_failure_cache_clear();
    artist_thumbnail_resolution_t resolution;
    artist_thumbnail_result_t result = artist_thumbnail_load_or_decode(db_song.id,
        METADATA_DB_GROUP_ARTIST, "OwnArtist", ARTWORK_PRIO_THUMBNAIL, true,
        albumart_artist_thumbnail_key("OwnArtist"), NULL, NULL, &resolution);
    assert(result == ARTIST_THUMB_FOUND && resolution.pixels && !resolution.has_alias);
    free(resolution.pixels);
    result = artist_thumbnail_load_or_decode(db_song.id, METADATA_DB_GROUP_ARTIST,
        "OwnArtist", ARTWORK_PRIO_WARMER, false, albumart_artist_thumbnail_key("OwnArtist"),
        NULL, NULL, &resolution);
    assert(result == ARTIST_THUMB_OWN_IMAGE && resolution.pixels == NULL);

    ensure_dir("music/AliasArtist/Album");
    write_bytes("music/AliasArtist/Album/track.mp3", "audio", 5);
    write_bmp("music/AliasArtist/Album/cover.bmp", 255, 0, 255);
    set_song(107, "music/AliasArtist/Album/track.mp3", "AliasArtist", "Album", "AliasArtist");
    db_artist_album_count = 1;
    artwork_failure_cache_clear();
    result = artist_thumbnail_load_or_decode(db_song.id, METADATA_DB_GROUP_ARTIST,
        "AliasArtist", ARTWORK_PRIO_THUMBNAIL, true,
        albumart_artist_thumbnail_key("AliasArtist"), NULL, NULL, &resolution);
    assert(result == ARTIST_THUMB_FOUND && resolution.pixels && resolution.has_alias);
    assert(resolution.alias_song_id == db_song.id && resolution.pixel_key_kind == THUMBNAIL_KEY_ALBUM);
    free(resolution.pixels);
    uint64_t album_key = 0; int64_t song_id = 0;
    assert(albumart_artist_alias_load("AliasArtist", METADATA_DB_GROUP_ARTIST, &album_key, &song_id));
    assert(song_id == db_song.id && album_key == song_album_thumbnail_key(&db_song));

    ensure_dir("music/NegativeArtist/Album");
    write_bytes("music/NegativeArtist/Album/track.mp3", "audio", 5);
    set_song(108, "music/NegativeArtist/Album/track.mp3", "NegativeArtist", "Album", "NegativeArtist");
    db_artist_album_count = 0;
    artwork_failure_cache_clear();
    result = artist_thumbnail_load_or_decode(db_song.id, METADATA_DB_GROUP_ARTIST,
        "NegativeArtist", ARTWORK_PRIO_THUMBNAIL, true,
        albumart_artist_thumbnail_key("NegativeArtist"), NULL, NULL, &resolution);
    assert(result == ARTIST_THUMB_MISSING && resolution.negative_confirmed);
    result = artist_thumbnail_load_or_decode(db_song.id, METADATA_DB_GROUP_ARTIST,
        "NegativeArtist", ARTWORK_PRIO_THUMBNAIL, true,
        albumart_artist_thumbnail_key("NegativeArtist"), NULL, NULL, &resolution);
    assert(result == ARTIST_THUMB_MISSING && resolution.negative_confirmed);
    puts("artist own-image, album-alias persistence and fresh negative marker are preserved");
}


static void test_warmer_thread_materializes_player_cache(void) {
    ensure_dir("music/WarmArtist/WarmAlbum");
    write_bytes("music/WarmArtist/WarmAlbum/track.mp3", "audio", 5);
    write_bmp("music/WarmArtist/WarmAlbum/cover.bmp", 32, 64, 192);
    set_song(109, "music/WarmArtist/WarmAlbum/track.mp3", "WarmArtist", "WarmAlbum", "WarmArtist");
    artwork_failure_cache_clear();
#ifdef LIBRARY_ART_BASELINE
    album_art_materialize_player = true; /* exact former warmer-thread TLS assignment */
#else
    album_art_enable_player_cache_materialization();
#endif
    uint16_t * pixels = NULL;
    bool loaded = album_thumbnail_load_or_decode_ex(&db_song, ARTWORK_PRIO_WARMER,
                                                     NULL, NULL, &pixels);
    assert(loaded && pixels);
    free(pixels);
    assert(album_thumbnail_reload_persisted(&db_song));
    char cached[PATH_MAX];
    assert(gui_library_album_cover_cached(db_song.id, ALBUM_PLAYER_CACHE_PX, cached, sizeof(cached)));
    puts("warmer-only TLS mode materializes player cache and keeps reload persistence true");
}

int main(void) {
    system_set_mock_mem_available(0);
    ensure_dir(".compas/albumart");
    ensure_dir(".compas/artist");
    test_cue_reference_row_path();
    test_sidecar_decode_and_persist();
    test_corrupt_sidecar_embedded_fallback();
    test_cancel_and_memory_temporary();
    test_artist_own_image_alias_and_negative();
    test_player_embedded_negative_and_cache_materialization();
    test_warmer_thread_materializes_player_cache();
    artwork_failure_cache_clear();
    puts("library artwork decode characterization: PASS");
    return 0;
}
