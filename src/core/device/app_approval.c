#include "app_approval.h"
#include "app_registry.h"
#include "../../network/store/plugins/store_common.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define APPROVAL_MAX_LINES 256

/* Splits "<id> <sha256>"; false for any other line. */
static bool parse_line(const char * line, char id[33], char sha[65]) {
    size_t n = strcspn(line, " \r\n");
    if (n == 0 || n > 32 || line[n] != ' ') return false;
    memcpy(id, line, n);
    id[n] = '\0';
    const char * hash = line + n + 1;
    if (strcspn(hash, "\r\n") != 64) return false;
    memcpy(sha, hash, 64);
    sha[64] = '\0';
    return app_id_valid(id) && store_sha_valid(sha);
}

bool app_approval_granted(const char * file, const char * id, const char * sha256) {
    if (!app_id_valid(id) || !store_sha_valid(sha256)) return false;
    FILE * f = fopen(file, "r");
    if (!f) return false;
    char line[128], line_id[33], line_sha[65];
    bool granted = false;
    while (!granted && fgets(line, sizeof(line), f)) {
        granted = parse_line(line, line_id, line_sha) && strcmp(line_id, id) == 0 && strcmp(line_sha, sha256) == 0;
    }
    fclose(f);
    return granted;
}

bool app_approval_grant(const char * file, const char * id, const char * sha256) {
    if (!app_id_valid(id) || !store_sha_valid(sha256)) return false;
    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", file) >= (int) sizeof(tmp)) return false;
    FILE * out = fopen(tmp, "w");
    if (!out) return false;
    bool ok = true;
    FILE * in = fopen(file, "r");
    if (in) {
        char line[128], line_id[33], line_sha[65];
        int kept = 0;
        while (ok && fgets(line, sizeof(line), in) && kept < APPROVAL_MAX_LINES) {
            if (!parse_line(line, line_id, line_sha) || strcmp(line_id, id) == 0) continue;
            ok = fprintf(out, "%s %s\n", line_id, line_sha) > 0;
            kept++;
        }
        fclose(in);
    }
    ok = ok && fprintf(out, "%s %s\n", id, sha256) > 0 && fflush(out) == 0 && fsync(fileno(out)) == 0;
    ok = (fclose(out) == 0) && ok;
    if (ok) ok = rename(tmp, file) == 0;
    if (!ok) (void) remove(tmp);
    return ok;
}
