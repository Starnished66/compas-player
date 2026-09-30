#include "sd_fsck.h"

#include <stdio.h>
#include <string.h>

static bool unescape_field(const char * in, size_t length, char * out, size_t out_size) {
    size_t written = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char value;
        if (in[i] == '\\' && i + 3 < length &&
            in[i + 1] >= '0' && in[i + 1] <= '7' &&
            in[i + 2] >= '0' && in[i + 2] <= '7' &&
            in[i + 3] >= '0' && in[i + 3] <= '7') {
            value = (unsigned char) ((in[i + 1] - '0') * 64 + (in[i + 2] - '0') * 8 + (in[i + 3] - '0'));
            i += 3;
        } else {
            value = (unsigned char) in[i];
        }
        if (written + 1 >= out_size) return false;
        out[written++] = (char) value;
    }
    out[written] = '\0';
    return true;
}

static bool option_token(const char * options, const char * token) {
    size_t token_len = strlen(token);
    const char * cursor = options;
    while (*cursor) {
        const char * comma = strchr(cursor, ',');
        size_t length = comma ? (size_t) (comma - cursor) : strlen(cursor);
        if (length == token_len && memcmp(cursor, token, token_len) == 0) return true;
        if (!comma) break;
        cursor = comma + 1;
    }
    return false;
}

static bool sd_mount_point(const char * path) {
    return strcmp(path, "/data/mnt/sd_0") == 0 || strcmp(path, "/usr/data/mnt/sd_0") == 0;
}

sd_fs_kind_t sd_fsck_kind_from_type(const char * fstype) {
    if (!fstype || !fstype[0]) return SD_FS_KIND_UNKNOWN;
    if (strcmp(fstype, "vfat") == 0 || strcmp(fstype, "msdos") == 0 || strcmp(fstype, "fat") == 0)
        return SD_FS_KIND_VFAT;
    if (strcmp(fstype, "exfat") == 0 || strcmp(fstype, "fuse.exfat") == 0) return SD_FS_KIND_EXFAT;
    if (strcmp(fstype, "ntfs") == 0 || strcmp(fstype, "ntfs3") == 0 || strcmp(fstype, "fuseblk") == 0 ||
        strcmp(fstype, "fuse.ntfs-3g") == 0 || strcmp(fstype, "ntfs-3g") == 0)
        return SD_FS_KIND_NTFS;
    return SD_FS_KIND_UNKNOWN;
}

bool sd_fsck_device_allowed(const char * device) {
    return device && (strcmp(device, "/dev/mmcblk0") == 0 || strcmp(device, "/dev/mmcblk0p1") == 0);
}

bool sd_fsck_parse_mounts(const char * mounts_text, sd_mount_info_t * out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!mounts_text) return false;

    const char * line = mounts_text;
    while (*line) {
        const char * next = strchr(line, '\n');
        size_t line_len = next ? (size_t) (next - line) : strlen(line);
        const char * fields[4];
        size_t field_len[4];
        size_t count = 0;
        size_t start = 0;
        for (size_t i = 0; i <= line_len && count < 4; i++) {
            bool boundary = i == line_len || line[i] == ' ' || line[i] == '\t';
            if (!boundary) continue;
            if (i > start) {
                fields[count] = line + start;
                field_len[count] = i - start;
                count++;
            }
            start = i + 1;
        }
        char device[SD_FSCK_DEVICE_BYTES];
        char mount_point[SD_FSCK_MOUNT_BYTES];
        char fstype[SD_FSCK_FSTYPE_BYTES];
        char options[512];
        if (count == 4 &&
            unescape_field(fields[0], field_len[0], device, sizeof(device)) &&
            unescape_field(fields[1], field_len[1], mount_point, sizeof(mount_point)) &&
            unescape_field(fields[2], field_len[2], fstype, sizeof(fstype)) &&
            unescape_field(fields[3], field_len[3], options, sizeof(options)) &&
            sd_mount_point(mount_point)) {
            out->found = true;
            out->readonly = option_token(options, "ro") && !option_token(options, "rw");
            out->kind = sd_fsck_kind_from_type(fstype);
            snprintf(out->device, sizeof(out->device), "%s", device);
            snprintf(out->mount_point, sizeof(out->mount_point), "%s", mount_point);
            snprintf(out->fstype, sizeof(out->fstype), "%s", fstype);
        }
        if (!next) break;
        line = next + 1;
    }
    return out->found;
}

static const char * const vfat_tools[] = {
    "/usr/sbin/fsck.vfat", "/usr/sbin/fsck.fat", "/sbin/fsck.vfat", NULL
};
static const char * const exfat_tools[] = { "/usr/sbin/fsck.exfat", "/sbin/fsck.exfat", NULL };
static const char * const ntfs_tools[] = { "/usr/sbin/ntfsfix", "/usr/bin/ntfsfix", NULL };

const char * const * sd_fsck_tool_candidates(sd_fs_kind_t kind) {
    if (kind == SD_FS_KIND_VFAT) return vfat_tools;
    if (kind == SD_FS_KIND_EXFAT) return exfat_tools;
    if (kind == SD_FS_KIND_NTFS) return ntfs_tools;
    return NULL;
}

const char * sd_fsck_select_tool(sd_fs_kind_t kind, bool (* exists)(const char * path, void * ctx), void * ctx) {
    const char * const * candidates = sd_fsck_tool_candidates(kind);
    if (!candidates || !exists) return NULL;
    for (size_t i = 0; candidates[i]; i++) {
        if (exists(candidates[i], ctx)) return candidates[i];
    }
    return NULL;
}

bool sd_fsck_plan(sd_fs_kind_t kind, const char * tool, const char * device, sd_fsck_plan_t * out) {
    if (!out || !tool || !tool[0] || !sd_fsck_device_allowed(device)) return false;
    if (kind != SD_FS_KIND_VFAT && kind != SD_FS_KIND_EXFAT && kind != SD_FS_KIND_NTFS) return false;
    memset(out, 0, sizeof(*out));
    snprintf(out->tool, sizeof(out->tool), "%s", tool);
    snprintf(out->device, sizeof(out->device), "%s", device);
    out->argv[0] = out->tool;
    if (kind == SD_FS_KIND_VFAT) {
        snprintf(out->arg1, sizeof(out->arg1), "-a");
        snprintf(out->arg2, sizeof(out->arg2), "-w");
        out->argv[1] = out->arg1;
        out->argv[2] = out->arg2;
        out->argv[3] = out->device;
        out->argv[4] = NULL;
    } else if (kind == SD_FS_KIND_EXFAT) {
        snprintf(out->arg1, sizeof(out->arg1), "-y");
        out->argv[1] = out->arg1;
        out->argv[2] = out->device;
        out->argv[3] = NULL;
    } else {
        out->argv[1] = out->device;
        out->argv[2] = NULL;
    }
    return true;
}

bool sd_fsck_exit_usable(int exit_code) {
    return exit_code >= 0 && exit_code <= 255 && (exit_code & ~0x3) == 0;
}

void sd_repair_attempt_key(const char * device, const char * cid, char * out, size_t out_size) {
    if (!out || out_size == 0) return;
    if (cid && cid[0]) snprintf(out, out_size, "cid:%s", cid);
    else snprintf(out, out_size, "dev:%s", device ? device : "");
}

void sd_repair_fat_attempt_key(const char * stamp, bool readonly_trigger, char * out, size_t out_size) {
    if (!out || out_size == 0) return;
    snprintf(out, out_size, "fat@%.24s:%s", stamp ? stamp : "", readonly_trigger ? "ro" : "dirty");
}

static bool output_line_is_summary(const char * line, size_t length, const char * device) {
    size_t device_len = strlen(device);
    static const char files[] = " files, ";
    static const char clusters[] = " clusters";
    size_t clusters_len = sizeof(clusters) - 1;
    if (length < device_len + 2 + clusters_len) return false;
    if (memcmp(line, device, device_len) != 0 || line[device_len] != ':' || line[device_len + 1] != ' ')
        return false;
    if (memcmp(line + length - clusters_len, clusters, clusters_len) != 0) return false;
    for (size_t i = device_len; i + sizeof(files) - 1 <= length; i++) {
        if (memcmp(line + i, files, sizeof(files) - 1) == 0) return true;
    }
    return false;
}

static bool output_has_summary(const char * output, const char * device) {
    if (!output || !device || !device[0]) return false;
    const char * line = output;
    while (*line) {
        const char * next = strchr(line, '\n');
        size_t length = next ? (size_t) (next - line) : strlen(line);
        if (length > 0 && line[length - 1] == '\r') length--;
        if (output_line_is_summary(line, length, device)) return true;
        if (!next) break;
        line = next + 1;
    }
    return false;
}

bool sd_fsck_tool_succeeded(sd_fs_kind_t kind, int exit_code, const char * output, const char * device) {
    if (kind == SD_FS_KIND_VFAT) return (exit_code == 0 || exit_code == 1) && output_has_summary(output, device);
    if (kind == SD_FS_KIND_EXFAT) return sd_fsck_exit_usable(exit_code);
    if (kind == SD_FS_KIND_NTFS) return exit_code == 0;
    return false;
}

sd_repair_note_t sd_fsck_outcome(bool tool_ok, bool dirty_trigger, bool mounted, bool readonly) {
    if (!mounted) return SD_REPAIR_NOTE_FAILED;
    if (readonly) return tool_ok ? SD_REPAIR_NOTE_STILL_READONLY : SD_REPAIR_NOTE_FAILED;
    if (!tool_ok) return SD_REPAIR_NOTE_NEEDS_COMPUTER;
    return dirty_trigger ? SD_REPAIR_NOTE_CHECKED : SD_REPAIR_NOTE_REPAIRED;
}

sd_repair_note_t sd_fsck_skipped_note(bool readonly, bool dirty_trigger) {
    if (readonly) return SD_REPAIR_NOTE_READONLY_NEEDS_COMPUTER;
    if (dirty_trigger) return SD_REPAIR_NOTE_NONE;
    return SD_REPAIR_NOTE_NEEDS_COMPUTER;
}

static uint32_t read_le16(const unsigned char * p) { return (uint32_t) p[0] | ((uint32_t) p[1] << 8); }

static uint32_t read_le32(const unsigned char * p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

bool sd_fsck_parse_boot_sector(const unsigned char * sector, size_t length, sd_fs_geometry_t * out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!sector || length < 512 || sector[510] != 0x55 || sector[511] != 0xAA) return false;

    if (memcmp(sector + 3, "EXFAT   ", 8) == 0) {
        uint32_t clusters = read_le32(sector + 92);
        if (clusters == 0) return false;
        out->kind = SD_FS_KIND_EXFAT;
        out->clusters = clusters;
        return true;
    }

    uint32_t bytes_per_sector = read_le16(sector + 11);
    uint32_t sectors_per_cluster = sector[13];
    uint32_t reserved = read_le16(sector + 14);
    uint32_t fats = sector[16];
    uint32_t root_entries = read_le16(sector + 17);
    uint32_t total = read_le16(sector + 19);
    uint32_t fat_size = read_le16(sector + 22);
    if (total == 0) total = read_le32(sector + 32);
    if (fat_size == 0) fat_size = read_le32(sector + 36);
    if (bytes_per_sector != 512 && bytes_per_sector != 1024 && bytes_per_sector != 2048 && bytes_per_sector != 4096)
        return false;
    if (sectors_per_cluster == 0 || (sectors_per_cluster & (sectors_per_cluster - 1)) != 0) return false;
    if (reserved == 0 || fats == 0 || fat_size == 0 || total == 0) return false;

    uint64_t root_sectors = ((uint64_t) root_entries * 32 + bytes_per_sector - 1) / bytes_per_sector;
    uint64_t overhead = (uint64_t) reserved + (uint64_t) fats * fat_size + root_sectors;
    if (overhead >= total) return false;
    uint64_t clusters = (total - overhead) / sectors_per_cluster;
    if (clusters == 0) return false;
    out->kind = SD_FS_KIND_VFAT;
    out->fat_bits = clusters < 4085 ? 12 : clusters < 65525 ? 16 : 32;
    out->clusters = clusters;
    return true;
}

uint64_t sd_fsck_memory_estimate(const sd_fs_geometry_t * geometry) {
    if (!geometry || geometry->clusters == 0) return 0;
    const uint64_t fixed = 2u * 1024u * 1024u;
    uint64_t entries = geometry->clusters + 2;
    uint64_t peak;
    if (geometry->kind == SD_FS_KIND_VFAT) {
        /* read_fat() holds two FAT copies, then reclaim_free() holds one copy,
         * cluster_owner and num_refs at once. Pointers are 4 bytes here. */
        uint64_t fat_bytes = (entries * geometry->fat_bits + 7) / 8;
        uint64_t reading = 2 * fat_bytes;
        uint64_t reclaiming = fat_bytes + 8 * entries;
        peak = reading > reclaiming ? reading : reclaiming;
    } else if (geometry->kind == SD_FS_KIND_EXFAT) {
        /* Allocation bitmaps, one bit per cluster, a few copies. */
        peak = entries / 8 * 4 + 2u * 1024u * 1024u;
    } else {
        return 0;
    }
    /* Directory entries and names are not known ahead of the check. */
    return peak + peak / 4 + fixed;
}

static bool line_contains(const char * line, size_t length, const char * needle) {
    size_t needle_len = strlen(needle);
    if (needle_len == 0 || needle_len > length) return needle_len == 0;
    for (size_t i = 0; i + needle_len <= length; i++) {
        if (memcmp(line + i, needle, needle_len) == 0) return true;
    }
    return false;
}

static void line_stamp(const char * line, size_t length, char * out, size_t out_size) {
    const char * open = memchr(line, '[', length);
    const char * close = open ? memchr(open, ']', length - (size_t) (open - line)) : NULL;
    if (!open || !close) {
        snprintf(out, out_size, "none");
        return;
    }
    const char * start = open + 1;
    while (start < close && *start == ' ') start++;
    int stamp_len = (int) (close - start);
    if (stamp_len <= 0) snprintf(out, out_size, "none");
    else snprintf(out, out_size, "%.*s", stamp_len, start);
}

bool sd_fsck_klog_dirty(const char * log, const char * device_base, char * stamp, size_t stamp_size) {
    char dirty_text[96];
    char latest_stamp[32] = "none";
    if (stamp && stamp_size) snprintf(stamp, stamp_size, "none");
    if (!log || !device_base || !device_base[0]) return false;
    snprintf(dirty_text, sizeof(dirty_text), "FAT-fs (%s): Volume was not properly unmounted", device_base);

    bool dirty = false;
    const char * line = log;
    while (*line) {
        const char * next = strchr(line, '\n');
        size_t length = next ? (size_t) (next - line) : strlen(line);
        if (line_contains(line, length, "mmc") && line_contains(line, length, ": new ") &&
            line_contains(line, length, " card at address ") && !line_contains(line, length, " SDIO card")) {
            /* The log is in order: a warning before this insertion belongs
             * to an earlier card. */
            dirty = false;
            line_stamp(line, length, latest_stamp, sizeof(latest_stamp));
        } else if (line_contains(line, length, dirty_text)) {
            dirty = true;
        }
        if (!next) break;
        line = next + 1;
    }
    if (stamp && stamp_size) snprintf(stamp, stamp_size, "%s", latest_stamp);
    return dirty;
}
