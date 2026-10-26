#define _POSIX_C_SOURCE 200809L

#include "internal_storage.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>

#define MOUNTINFO_PATH "/proc/self/mountinfo"
#define MOUNTINFO_LINE_MAX 1024
#define MOUNTINFO_FIELDS_MAX 64
#define MOUNTINFO_MOUNT_POINT_INDEX 4
#define MOUNTINFO_OPTIONS_INDEX 5
#define MOUNTINFO_SEPARATOR_MIN 6
#define MOUNTINFO_MALFORMED "malformed mountinfo entry"

static void set_reason(char * reason, size_t reason_size, const char * text) {
    if (reason && reason_size > 0) (void) snprintf(reason, reason_size, "%s", text);
}

/* Splits line in place at spaces, which separate mountinfo fields. Sets *more
 * when the line has more fields than fields[] holds. */
static size_t split_fields(char * line, const char * fields[], bool * more) {
    size_t n = 0;
    char * p = line;
    *more = false;
    while (*p == ' ') p++;
    while (*p) {
        if (n == MOUNTINFO_FIELDS_MAX) {
            *more = true;
            break;
        }
        fields[n++] = p;
        while (*p && *p != ' ') p++;
        if (*p == ' ') {
            *p++ = '\0';
            while (*p == ' ') p++;
        }
    }
    return n;
}

/* Parses "MAJ:MIN" and rejects any other text around the two numbers. */
static bool parse_dev_field(const char * text, unsigned long * major_out, unsigned long * minor_out) {
    char * end = NULL;
    errno = 0;
    unsigned long major_num = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != ':') return false;
    const char * minor_text = end + 1;
    unsigned long minor_num = strtoul(minor_text, &end, 10);
    if (errno != 0 || end == minor_text || *end != '\0') return false;
    *major_out = major_num;
    *minor_out = minor_num;
    return true;
}

/* True when the comma-separated option list has a "rw" token and no "ro". */
static bool options_read_write(const char * opts) {
    bool rw = false;
    const char * p = opts;
    while (*p) {
        const char * end = strchr(p, ',');
        size_t len = end ? (size_t) (end - p) : strlen(p);
        if (len == 2 && memcmp(p, "ro", 2) == 0) return false;
        if (len == 2 && memcmp(p, "rw", 2) == 0) rw = true;
        if (!end) break;
        p = end + 1;
    }
    return rw;
}

/* Returns NULL for a healthy writable UBIFS volume entry, else the reason. */
static const char * check_entry(const char * f[], size_t n, unsigned dev_major, unsigned dev_minor) {
    size_t sep = MOUNTINFO_SEPARATOR_MIN;
    while (sep < n && strcmp(f[sep], "-") != 0) sep++;
    if (sep >= n || n != sep + 4) return MOUNTINFO_MALFORMED;

    unsigned long major_num = 0;
    unsigned long minor_num = 0;
    if (!parse_dev_field(f[2], &major_num, &minor_num)) return MOUNTINFO_MALFORMED;
    if (major_num != dev_major || minor_num != dev_minor) return "device mismatch";
    if (strcmp(f[sep + 1], "ubifs") != 0) return "not ubifs";
    if (strncmp(f[sep + 2], "ubi", 3) != 0 && strncmp(f[sep + 2], "/dev/ubi", 8) != 0)
        return "not a ubi volume";
    if (!options_read_write(f[MOUNTINFO_OPTIONS_INDEX]) || !options_read_write(f[sep + 3]))
        return "mounted read-only";
    return NULL;
}

bool internal_storage_mountinfo_ok(FILE * mountinfo, const char * mount_point,
                                   unsigned dev_major, unsigned dev_minor,
                                   char * reason, size_t reason_size) {
    if (!mountinfo || !mount_point) {
        set_reason(reason, reason_size, "no mountinfo entry");
        return false;
    }

    /* Each matching line replaces the verdict: a later mount hides earlier
     * ones. NULL means the newest matching entry is healthy. */
    const char * verdict = NULL;
    bool seen = false;
    char line[MOUNTINFO_LINE_MAX];
    const char * fields[MOUNTINFO_FIELDS_MAX];
    while (fgets(line, sizeof(line), mountinfo)) {
        bool truncated = strchr(line, '\n') == NULL && !feof(mountinfo);
        if (truncated) {
            /* Discard the rest of an overlong line; it is malformed. */
            int c;
            while ((c = fgetc(mountinfo)) != '\n' && c != EOF) {}
        }
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[--len] = '\0';

        bool more = false;
        size_t n = split_fields(line, fields, &more);
        if (truncated) {
            /* The last field may be cut short, so it is not used. */
            if (n > 0) n--;
            more = true;
        }
        if (n <= MOUNTINFO_MOUNT_POINT_INDEX ||
            strcmp(fields[MOUNTINFO_MOUNT_POINT_INDEX], mount_point) != 0)
            continue;

        seen = true;
        verdict = more ? MOUNTINFO_MALFORMED : check_entry(fields, n, dev_major, dev_minor);
    }

    if (!seen) {
        set_reason(reason, reason_size, "no mountinfo entry");
        return false;
    }
    if (verdict) {
        set_reason(reason, reason_size, verdict);
        return false;
    }
    return true;
}

bool internal_storage_ready(char * reason, size_t reason_size) {
    struct stat root_st;
    struct stat data_st;
    if (stat("/", &root_st) != 0) {
        set_reason(reason, reason_size, "root stat failed");
        return false;
    }
    if (stat(INTERNAL_DATA_ROOT, &data_st) != 0 || !S_ISDIR(data_st.st_mode)) {
        set_reason(reason, reason_size, "missing");
        return false;
    }
    if (data_st.st_dev == root_st.st_dev) {
        set_reason(reason, reason_size, "not a separate mount");
        return false;
    }

    FILE * mountinfo = fopen(MOUNTINFO_PATH, "re");
    if (!mountinfo) {
        set_reason(reason, reason_size, "mountinfo unavailable");
        return false;
    }
    bool ok = internal_storage_mountinfo_ok(mountinfo, INTERNAL_DATA_ROOT,
                                            major(data_st.st_dev), minor(data_st.st_dev),
                                            reason, reason_size);
    (void) fclose(mountinfo);
    if (!ok) return false;

    struct statvfs vfs;
    if (statvfs(INTERNAL_DATA_ROOT, &vfs) != 0) {
        set_reason(reason, reason_size, "statvfs failed");
        return false;
    }
    if (vfs.f_flag & ST_RDONLY) {
        set_reason(reason, reason_size, "mounted read-only");
        return false;
    }
    return true;
}
