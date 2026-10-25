/* Host characterization of /api/status catalog_revision serialization. */
#include "remote_control.c"

#include <assert.h>

static bool revision_available;
static const char *revision_value;
static bool getter_checked_status_lock;

bool metadata_db_catalog_get_revision(char *out_revision, size_t out_size) {
    assert(out_revision && out_size > 0);
    /* The status snapshot must not hold its mutex while querying metadata. */
    assert(pthread_mutex_trylock(&status_mutex) == 0);
    pthread_mutex_unlock(&status_mutex);
    getter_checked_status_lock = true;
    out_revision[0] = '\0';
    if (!revision_available) return false;
    size_t length = strlen(revision_value);
    if (length >= out_size) return false;
    memcpy(out_revision, revision_value, length + 1);
    return true;
}

const char *app_version_label(void) { return "status-spec"; }

int main(int argc, char **argv) {
    assert(argc == 2);
    if (strcmp(argv[1], "--special") == 0) {
        revision_available = true;
        revision_value = "catalog\"\\\nrevision";
    } else if (strcmp(argv[1], "--max") == 0) {
        static char maximum_revision[METADATA_DB_CATALOG_REVISION_SIZE];
        memset(maximum_revision, '\1', sizeof(maximum_revision) - 1);
        maximum_revision[sizeof(maximum_revision) - 1] = '\0';
        revision_available = true;
        revision_value = maximum_revision;
    } else if (strcmp(argv[1], "--empty") == 0) {
        revision_available = true;
        revision_value = "";
    } else {
        assert(strcmp(argv[1], "--unavailable") == 0);
        revision_available = false;
        revision_value = "unused";
    }

    char guarded[8] = {'x', 'C', 'A', 'N', 'A', 'R', 'Y', '\0'};
    build_status_json(guarded, 1);
    assert(guarded[0] == '\0' && memcmp(guarded + 1, "CANARY", 6) == 0);

    char json[STATUS_JSON_CAPACITY];
    build_status_json(json, sizeof(json));
    assert(getter_checked_status_lock);
    puts(json);
    return 0;
}
