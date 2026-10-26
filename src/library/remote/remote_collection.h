#ifndef COMPAS_REMOTE_COLLECTION_H
#define COMPAS_REMOTE_COLLECTION_H

#include "../metadata/db/metadata_db.h"

/* Immutable request copied from the Remote Control worker to the UI thread.
 * This header deliberately has no LVGL or GUI dependencies. */
typedef enum {
    REMOTE_COLLECTION_ALL,
    REMOTE_COLLECTION_ALBUM,
    REMOTE_COLLECTION_ARTIST,
    REMOTE_COLLECTION_ALBUM_ARTIST,
    REMOTE_COLLECTION_GENRE,
    REMOTE_COLLECTION_PLAYLIST,
    REMOTE_COLLECTION_FAVORITES,
    REMOTE_COLLECTION_RECENTLY_ADDED,
    REMOTE_COLLECTION_RECENTLY_PLAYED
} remote_collection_scope_t;

typedef struct {
    remote_collection_scope_t scope;
    int play_mode; /* PLAY_MODE-compatible values 0..4, kept numeric here. */
    char name[METADATA_DB_TEXT_MAX];
    char artist[128];
    char album_artist[128];
    char album[128];
    char genre[METADATA_DB_TEXT_MAX];
    char expected_revision[METADATA_DB_CATALOG_REVISION_SIZE];
} remote_collection_play_request_t;

#endif
