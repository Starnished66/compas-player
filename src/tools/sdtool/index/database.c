#include "../sdtool.h"
#include "../output/json_out.h"
#include "../../../library/metadata/db/metadata_db.h"
#include "../../../core/persistence/storage_paths.h"
#include "../../../library/tagcache/core/tagcache.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* Same recovery the player's scan applies: a database that cannot be loaded is
 * replaced by an empty one that the scan then fills. */
static bool card_present(void) {
    struct stat st;
    if (stat(sdtool_root(), &st) == 0 && S_ISDIR(st.st_mode)) return true;
    json_error("card_not_found", "the card root is not a readable folder");
    return false;
}

bool sdtool_open_database(sdtool_database_t *db) {
    if (!card_present()) return false;
    db->outcome = metadata_db_open();
    db->preserve_rebuild = false;
    if (db->outcome == METADATA_DB_LOAD_FAILED && metadata_db_prepare_rebuild()) {
        /* As the player does: an empty provisional database that the scan
         * fills, replacing the unreadable one only if it finds music. */
        db->outcome = metadata_db_get_load_outcome();
        db->preserve_rebuild = db->outcome == METADATA_DB_LOAD_FAILED;
        if (db->preserve_rebuild) db->outcome = METADATA_DB_LOAD_SUCCESS_NORMAL;
    }
    if (db->outcome == METADATA_DB_LOAD_FAILED || db->outcome == METADATA_DB_LOAD_UNMOUNTED) {
        json_error("database_open_failed", "the music database could not be opened or rebuilt");
        return false;
    }
    return true;
}

int sdtool_cmd_info(const sdtool_options_t *options) {
    (void) options;
    if (!card_present()) return SDTOOL_FAILED;
    json_line_t line;
    long long songs = 0;
    int generation = 0;
    struct stat st;
    /* A card never scanned has no database: report it empty without creating
     * one. */
    if (stat(storage_sd_path(COMPAS_STORAGE_DIR "/tagcache.gen"), &st) == 0) {
        if (metadata_db_open_read_only() == METADATA_DB_LOAD_FAILED) {
            json_error("database_open_failed", "the music database could not be read");
            return SDTOOL_FAILED;
        }
        songs = metadata_db_get_song_count();
        generation = metadata_db_get_generation();
        metadata_db_close();
    }
    json_begin(&line);
    json_string(&line, "sdtool_version", SDTOOL_VERSION);
    json_string(&line, "build", sdtool_build);
    json_integer(&line, "database_format", TAGCACHE_DATABASE_FORMAT);
    json_integer(&line, "generation", generation);
    json_integer(&line, "songs", songs);
    json_emit(&line);
    return SDTOOL_OK;
}
