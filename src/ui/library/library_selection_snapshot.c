#include "library_selection_snapshot.h"
#include "../player/screen/gui_player.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define SELECTION_SNAPSHOT_CAPACITY 256

typedef struct {
    bool valid;
    selection_snapshot_source_t source;
    int rank;
    int64_t song_id;
    int32_t generation;
    char path[sizeof(((song_row_t *) 0)->path)];
    track_metadata_t metadata;
    uint64_t age;
} selection_snapshot_t;

static pthread_mutex_t selection_snapshot_mutex = PTHREAD_MUTEX_INITIALIZER;
static selection_snapshot_t selection_snapshots[SELECTION_SNAPSHOT_CAPACITY];
static uint64_t selection_snapshot_age;

void library_selection_snapshot_store(const song_row_t * row,
                                      selection_snapshot_source_t source, int rank,
                                      int32_t generation) {
    if (!row || row->id <= 0 || !row->path[0]) return;
    track_metadata_t metadata = {0};
#define COPY_SELECTION_TAG(field) do { \
    if (row->tags.field[0]) { \
        snprintf(metadata.field, sizeof(metadata.field), "%s", row->tags.field); \
        metadata.has_##field = metadata.field[0] != '\0'; \
    } \
} while (0)
    COPY_SELECTION_TAG(title);
    COPY_SELECTION_TAG(artist);
    COPY_SELECTION_TAG(album);
    COPY_SELECTION_TAG(album_artist);
    COPY_SELECTION_TAG(genre);
#undef COPY_SELECTION_TAG
    if (row->tags.track_number > 0) {
        metadata.track_number = row->tags.track_number;
        metadata.has_track_number = true;
    }
    if (row->tags.disc_number > 0) {
        metadata.disc_number = row->tags.disc_number;
        metadata.has_disc_number = true;
    }
    if (row->tags.release_year > 0) {
        metadata.release_year = row->tags.release_year;
        metadata.has_release_year = true;
    }

    pthread_mutex_lock(&selection_snapshot_mutex);
    size_t slot = 0;
    uint64_t oldest = UINT64_MAX;
    for (size_t i = 0; i < SELECTION_SNAPSHOT_CAPACITY; ++i) {
        selection_snapshot_t * entry = &selection_snapshots[i];
        if (entry->valid && entry->source == source && entry->rank == rank &&
            entry->song_id == row->id && entry->generation == generation &&
            strcmp(entry->path, row->path) == 0) {
            slot = i;
            oldest = 0;
            break;
        }
        if (!entry->valid) {
            slot = i;
            oldest = 0;
            break;
        }
        if (entry->age < oldest) { oldest = entry->age; slot = i; }
    }
    selection_snapshot_t * entry = &selection_snapshots[slot];
    *entry = (selection_snapshot_t){
        .valid = true, .source = source, .rank = rank, .song_id = row->id,
        .generation = generation, .metadata = metadata, .age = ++selection_snapshot_age
    };
    snprintf(entry->path, sizeof(entry->path), "%s", row->path);
    pthread_mutex_unlock(&selection_snapshot_mutex);
}

void library_selection_snapshot_seed(selection_snapshot_source_t source, int rank,
                                     int64_t song_id, const char * path) {
    int32_t generation;
    if (!metadata_db_try_get_generation(&generation)) {
        gui_player_seed_selection_song(NULL, NULL);
        return;
    }
    track_metadata_t metadata = {0};
    char cached_path[sizeof(((song_row_t *) 0)->path)] = {0};
    bool found = false;
    if (pthread_mutex_trylock(&selection_snapshot_mutex) != 0) {
        gui_player_seed_selection_song(NULL, NULL);
        return;
    }
    for (size_t i = 0; i < SELECTION_SNAPSHOT_CAPACITY; ++i) {
        selection_snapshot_t * entry = &selection_snapshots[i];
        if (!entry->valid || entry->source != source || entry->generation != generation ||
            (source != SELECTION_SNAPSHOT_GROUP && entry->rank != rank) ||
            (source == SELECTION_SNAPSHOT_GROUP &&
             (entry->song_id != song_id || !path || strcmp(entry->path, path) != 0))) continue;
        if (entry->song_id <= 0 || (song_id > 0 && entry->song_id != song_id)) continue;
        snprintf(cached_path, sizeof(cached_path), "%s", entry->path);
        metadata = entry->metadata;
        entry->age = ++selection_snapshot_age;
        found = true;
        break;
    }
    pthread_mutex_unlock(&selection_snapshot_mutex);
    if (found && path && path[0] && strcmp(path, cached_path) != 0) found = false;
    gui_player_seed_selection_song(found ? cached_path : NULL, found ? &metadata : NULL);
}
