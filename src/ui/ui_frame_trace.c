#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "ui_frame_trace.h"

#ifdef UI_PERF_TRACE
#include "gui.h"

#include <stdio.h>
#include <string.h>
#include <sys/statfs.h>
#include <sys/resource.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

/* Fixed, statically allocated rings: frame records are written once per
 * presentation; sparse records carry stage boundaries and counters. */
#define FRAME_CAPACITY 4096u
#define EVENT_CAPACITY 1024u
#define TRACE_BUDGET_BYTES (256u * 1024u)
#define EXPORT_BATCH_RECORDS 64u
#define TRACE_SD_DIR "/usr/data/mnt/sd_0"
#define TRACE_LOG_PATH TRACE_SD_DIR "/compas_frames.csv"
#define TRACE_COMMAND_PATH TRACE_SD_DIR "/compas_trace.command"
#define TRACE_STATUS_PATH TRACE_SD_DIR "/compas_trace.status"
#define TRACE_EXPORT_FAILURE_LIMIT 20u

enum {
    FRAME_FLAG_COMPOSITED = 1u,
    FRAME_FLAG_PAN_FAILED = 4u,
    FRAME_FLAG_PAN_EXTERNAL = 8u
};

typedef struct {
    uint32_t sequence;
    uint32_t flags;
    uint64_t timestamp_us;
    uint32_t work_us, sync_us, drawflush_us, drawflush_area, pan_us, handler_us, sleep_us;
    uint32_t invalidations;
    uint64_t invalidated_area;
} frame_record_t;

typedef struct {
    uint32_t sequence;
    uint32_t kind;
    uint64_t timestamp_us;
    uint32_t a;
    uint32_t b;
} event_record_t;

_Static_assert(sizeof(frame_record_t) * FRAME_CAPACITY +
               sizeof(event_record_t) * EVENT_CAPACITY <= TRACE_BUDGET_BYTES,
               "UI trace rings exceed their 256 KiB budget");

static frame_record_t frames[FRAME_CAPACITY];
static event_record_t events[EVENT_CAPACITY];
static struct {
    uint32_t frame_next, frame_count, event_next, event_count;
    uint32_t frame_sequence, event_sequence;
    uint32_t frame_overwrites, event_overwrites;
    uint32_t invalidation_count;
    uint32_t pan_failures, present_failures;
    uint64_t invalidation_area;
    uint32_t pending_invalidations;
    uint32_t pending_flags;
    uint64_t pending_invalidated_area;
    uint32_t pending_sync_us, pending_drawflush_us, pending_drawflush_area, pending_pan_us;
    uint32_t pending_sleep_us;
    long handler_majflt, handler_minflt;
    bool handler_faults_valid;
    uint64_t sync_start_us, pan_start_us, handler_start_us, sleep_start_us;
    uint64_t capture_start_us, capture_stop_us;
    uint32_t handler_frame_pos, handler_frame_sequence;
    bool handler_frame_valid, pan_external;
    uint64_t last_activity_us;
    bool armed, export_pending, export_header_pending, export_footer_pending;
    char tag[64];
    uint32_t export_frame_pos, export_frame_left;
    uint32_t export_event_pos, export_event_left;
    uint32_t export_errors;
    bool mount_verified;
} trace;
static uint64_t render_start_us;

/* Some libc filesystems report statfs only for the containing root. Requiring
 * a real mount entry at the configured directory and rejecting RAM filesystems
 * prevents accidental fallback to /usr when the SD is absent. */
static bool verify_sd_mount(void) {
    struct statfs fs;
    if (statfs(TRACE_SD_DIR, &fs) != 0) return false;
    if ((unsigned long) fs.f_type == 0x01021994UL || /* TMPFS_MAGIC */
        (unsigned long) fs.f_type == 0x858458f6UL)   /* RAMFS_MAGIC */
        return false;
    FILE * mounts = fopen("/proc/self/mounts", "r");
    if (!mounts) return false;
    char canonical_path[PATH_MAX];
    if (!realpath(TRACE_SD_DIR, canonical_path)) {
        fclose(mounts);
        return false;
    }
    char line[512], device[128], mountpoint[256], type[64];
    bool found = false;
    while (fgets(line, sizeof(line), mounts)) {
        if (sscanf(line, "%127s %255s %63s", device, mountpoint, type) != 3) continue;
        if (strcmp(mountpoint, canonical_path) != 0) continue;
        bool sd_device = strcmp(device, "/dev/mmcblk0p1") == 0 ||
                         strcmp(device, "/dev/mmcblk0") == 0;
        bool sd_filesystem = strcmp(type, "vfat") == 0 || strcmp(type, "exfat") == 0 ||
                             strcmp(type, "ntfs-3g") == 0 || strcmp(type, "fuseblk") == 0;
        found = sd_device && sd_filesystem;
        break;
    }
    fclose(mounts);
    return found;
}

static bool refresh_sd_mount(void) {
    trace.mount_verified = verify_sd_mount();
    return trace.mount_verified;
}

static void record_event_at(ui_frame_trace_event_kind_t kind, uint64_t now,
                            uint32_t a, uint32_t b) {
    uint32_t pos = trace.event_next;
    events[pos] = (event_record_t) {
        .sequence = ++trace.event_sequence, .kind = (uint32_t) kind,
        .timestamp_us = now, .a = a, .b = b
    };
    trace.event_next = (pos + 1u) % EVENT_CAPACITY;
    if (trace.event_count < EVENT_CAPACITY) trace.event_count++;
    else trace.event_overwrites++;
}

static uint32_t elapsed_u32(uint64_t start, uint64_t end) {
    if (!start || end < start) return 0;
    uint64_t elapsed = end - start;
    return elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t) elapsed;
}

void ui_frame_trace_event(ui_frame_trace_event_kind_t kind, uint32_t a, uint32_t b) {
    if (!trace.armed && !(kind == UI_FRAME_TRACE_EVENT_HANDLER_END && trace.handler_start_us)) return;
    uint64_t now = ui_perf_now_us();
    trace.last_activity_us = now;
    switch (kind) {
        case UI_FRAME_TRACE_EVENT_SYNC_BEGIN: trace.sync_start_us = now; break;
        case UI_FRAME_TRACE_EVENT_SYNC_END:
            trace.pending_sync_us += elapsed_u32(trace.sync_start_us, now);
            trace.sync_start_us = 0;
            break;
        case UI_FRAME_TRACE_EVENT_DRAWFLUSH:
            trace.pending_drawflush_us += a;
            trace.pending_drawflush_area += b;
            break;
        case UI_FRAME_TRACE_EVENT_PAN_BEGIN:
            trace.pan_start_us = now;
            trace.pan_external = a != 0;
            break;
        case UI_FRAME_TRACE_EVENT_PAN_END:
            trace.pending_pan_us += elapsed_u32(trace.pan_start_us, now);
            trace.pan_start_us = 0;
            if (trace.pan_external) trace.pending_flags |= FRAME_FLAG_PAN_EXTERNAL;
            if (!a) {
                trace.pending_flags |= FRAME_FLAG_PAN_FAILED;
                trace.pan_failures++;
                if (trace.armed) record_event_at(kind, now, a, b);
            }
            trace.pan_external = false;
            break;
        case UI_FRAME_TRACE_EVENT_HANDLER_BEGIN: {
            trace.handler_start_us = now;
            trace.handler_frame_valid = false;
            struct rusage usage;
            trace.handler_faults_valid = false;
            if (getrusage(RUSAGE_THREAD, &usage) == 0) {
                trace.handler_faults_valid = true;
                trace.handler_majflt = usage.ru_majflt;
                trace.handler_minflt = usage.ru_minflt;
            }
            break;
        }
        case UI_FRAME_TRACE_EVENT_HANDLER_END: {
            uint32_t duration = elapsed_u32(trace.handler_start_us, now);
            trace.handler_start_us = 0;
            if (trace.handler_frame_valid && trace.handler_frame_pos < FRAME_CAPACITY &&
                frames[trace.handler_frame_pos].sequence == trace.handler_frame_sequence)
                frames[trace.handler_frame_pos].handler_us = duration;
            trace.handler_frame_valid = false;
            if (trace.armed && duration >= 16000u) {
                record_event_at(kind, now, duration, a);
                struct rusage usage;
                if (trace.handler_faults_valid && getrusage(RUSAGE_THREAD, &usage) == 0)
                    record_event_at(UI_FRAME_TRACE_EVENT_HANDLER_FAULTS, now,
                                    (uint32_t) (usage.ru_majflt - trace.handler_majflt),
                                    (uint32_t) (usage.ru_minflt - trace.handler_minflt));
            }
            break;
        }
        case UI_FRAME_TRACE_EVENT_SLEEP_BEGIN: trace.sleep_start_us = now; break;
        case UI_FRAME_TRACE_EVENT_SLEEP_END: {
            uint32_t duration = elapsed_u32(trace.sleep_start_us, now);
            trace.pending_sleep_us += duration;
            trace.sleep_start_us = 0;
            if (trace.armed && duration >= 16000u) record_event_at(kind, now, duration, a);
            break;
        }
        case UI_FRAME_TRACE_EVENT_COMPOSITOR_BEGIN:
            if (trace.armed) record_event_at(kind, now, a, b);
            break;
        case UI_FRAME_TRACE_EVENT_COMPOSITOR_PRESENT:
            if (!a) {
                trace.present_failures++;
                trace.pending_flags |= FRAME_FLAG_PAN_FAILED;
                if (trace.armed) record_event_at(kind, now, a, b);
            }
            break;
        default: record_event_at(kind, now, a, b); break;
    }
}

static void record_frame(uint64_t now, uint32_t work_us, bool composited) {
    trace.last_activity_us = now;
    if (!trace.armed) return;
    uint32_t pos = trace.frame_next;
    frames[pos] = (frame_record_t) {
        .sequence = ++trace.frame_sequence,
        .flags = (composited ? FRAME_FLAG_COMPOSITED : 0u) | trace.pending_flags,
        .timestamp_us = now, .work_us = work_us,
        .sync_us = trace.pending_sync_us,
        .drawflush_us = trace.pending_drawflush_us,
        .drawflush_area = trace.pending_drawflush_area,
        .pan_us = trace.pending_pan_us,
        .sleep_us = trace.pending_sleep_us,
        .invalidations = trace.pending_invalidations,
        .invalidated_area = trace.pending_invalidated_area
    };
    trace.pending_sync_us = trace.pending_drawflush_us = trace.pending_pan_us = 0;
    trace.pending_drawflush_area = 0;
    trace.pending_sleep_us = 0;
    trace.pending_invalidations = 0;
    trace.pending_flags = 0;
    trace.pending_invalidated_area = 0;
    trace.frame_next = (pos + 1u) % FRAME_CAPACITY;
    if (trace.frame_count < FRAME_CAPACITY) trace.frame_count++;
    else trace.frame_overwrites++;
    if (trace.handler_start_us) {
        trace.handler_frame_pos = pos;
        trace.handler_frame_sequence = frames[pos].sequence;
        trace.handler_frame_valid = true;
    }
}

static void display_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_RENDER_READY) {
        uint64_t now = ui_perf_now_us();
        trace.last_activity_us = now;
        if (!trace.armed) return;
        uint32_t work = render_start_us && now >= render_start_us &&
                        now - render_start_us <= UINT32_MAX ?
                        (uint32_t) (now - render_start_us) : 0;
        render_start_us = 0;
        record_frame(now, work, false);
        return;
    }
    if (!trace.armed) return;
    if (code == LV_EVENT_RENDER_START) {
        render_start_us = ui_perf_now_us();
        return;
    }
    if (code == LV_EVENT_INVALIDATE_AREA) {
        const lv_area_t * area = lv_event_get_param(e);
        trace.invalidation_count++;
        trace.pending_invalidations++;
        if (area && area->x2 >= area->x1 && area->y2 >= area->y1) {
            uint64_t pixels = (uint64_t) (area->x2 - area->x1 + 1) *
                              (uint64_t) (area->y2 - area->y1 + 1);
            trace.invalidation_area += pixels;
            trace.pending_invalidated_area += pixels;
        }
    }
}

static bool read_command(char * out, size_t size) {
    FILE * f = fopen(TRACE_COMMAND_PATH, "r");
    if (!f) return false;
    bool ok = fgets(out, (int) size, f) != NULL;
    fclose(f);
    if (ok) {
        out[strcspn(out, "\r\n")] = '\0';
        if (unlink(TRACE_COMMAND_PATH) != 0) return false;
    }
    return ok;
}

static void start_capture(void) {
    /* Fault in all bounded ring pages before the measured window starts. */
    memset(frames, 0, sizeof(frames));
    memset(events, 0, sizeof(events));
    trace.frame_next = trace.frame_count = trace.event_next = trace.event_count = 0;
    trace.frame_sequence = trace.event_sequence = 0;
    trace.frame_overwrites = trace.event_overwrites = 0;
    trace.export_errors = 0;
    trace.invalidation_count = 0;
    trace.invalidation_area = 0;
    trace.pan_failures = trace.present_failures = 0;
    trace.pending_invalidations = 0;
    trace.pending_invalidated_area = 0;
    trace.pending_sync_us = trace.pending_drawflush_us = trace.pending_drawflush_area = 0;
    trace.pending_pan_us = trace.pending_sleep_us = trace.pending_flags = 0;
    trace.sync_start_us = trace.pan_start_us = trace.handler_start_us = trace.sleep_start_us = 0;
    trace.handler_frame_valid = false;
    trace.pan_external = false;
    render_start_us = 0;
    trace.capture_start_us = ui_perf_now_us();
    trace.capture_stop_us = 0;
    trace.armed = true;
    trace.export_pending = false;
}

static bool stop_capture(void) {
    if (!trace.armed) return false;
    trace.capture_stop_us = ui_perf_now_us();
    trace.armed = false;
    trace.export_pending = true;
    trace.export_header_pending = true;
    trace.export_footer_pending = true;
    trace.export_frame_pos = (trace.frame_next + FRAME_CAPACITY - trace.frame_count) % FRAME_CAPACITY;
    trace.export_frame_left = trace.frame_count;
    trace.export_event_pos = (trace.event_next + EVENT_CAPACITY - trace.event_count) % EVENT_CAPACITY;
    trace.export_event_left = trace.event_count;
    return true;
}

static void write_status(const char * status);

/* CSV layout: F seq,time,work,flags,sync,drawflush,drawflush_area,pan,handler,sleep,
 * invalidate_count,invalidate_area; E seq,time,kind,a,b. Durations are wall
 * microseconds. work is render-start to render-ready; drawflush ends at flush
 * entry and excludes flush/pan waiting. Sync/pan are nested in handler time,
 * so these columns must not be summed as independent CPU costs. invalidated
 * area is the sum of requested rectangles before LVGL merges them. handler_us
 * belongs to the last present in that same main-loop handler. sleep_us sums
 * the waits since the preceding present. Flags: bit0=composited present,
 * bit2=pan/present failure since prior record, bit3=external pan path. */
static void write_export_batch(void) {
    if (!trace.export_pending || !refresh_sd_mount()) return;
    FILE * f = fopen(TRACE_LOG_PATH, "a");
    if (!f) {
        trace.export_errors++;
        write_status("cannot open trace output; retrying");
        return;
    }
    uint32_t frame_pos = trace.export_frame_pos, frame_left = trace.export_frame_left;
    uint32_t event_pos = trace.export_event_pos, event_left = trace.export_event_left;
    bool header_pending = trace.export_header_pending;
    bool footer_pending = trace.export_footer_pending;
    bool ok = true;
    if (header_pending) {
        ok = fprintf(f, "# trace tag=%s start_us=%llu stop_us=%llu bytes=%u frame_record=%zu event_record=%zu frame_capacity=%u event_capacity=%u frame_overwrites=%u event_overwrites=%u invalidations=%u invalidated_area=%llu pan_failures=%u present_failures=%u pan_fail_flag=4 delivery=at-least-once dedupe=(tag,type,sequence)\n",
                trace.tag[0] ? trace.tag : "-",
                (unsigned long long) trace.capture_start_us,
                (unsigned long long) trace.capture_stop_us,
                (unsigned) (sizeof(frames) + sizeof(events)), sizeof(frame_record_t), sizeof(event_record_t),
                FRAME_CAPACITY, EVENT_CAPACITY, trace.frame_overwrites, trace.event_overwrites,
                trace.invalidation_count, (unsigned long long) trace.invalidation_area,
                trace.pan_failures, trace.present_failures) >= 0;
        if (ok) header_pending = false;
    }
    unsigned budget = EXPORT_BATCH_RECORDS;
    while (ok && budget && frame_left) {
        const frame_record_t * r = &frames[frame_pos];
        ok = fprintf(f, "F,%u,%llu,%u,%u,%u,%u,%u,%u,%u,%u,%u,%llu\n", r->sequence,
                (unsigned long long) r->timestamp_us, r->work_us, r->flags,
                r->sync_us, r->drawflush_us, r->drawflush_area, r->pan_us,
                r->handler_us, r->sleep_us,
                r->invalidations, (unsigned long long) r->invalidated_area) >= 0;
        if (ok) { frame_pos = (frame_pos + 1u) % FRAME_CAPACITY; frame_left--; budget--; }
    }
    while (ok && budget && event_left) {
        const event_record_t * r = &events[event_pos];
        ok = fprintf(f, "E,%u,%llu,%u,%u,%u\n", r->sequence,
                (unsigned long long) r->timestamp_us, r->kind, r->a, r->b) >= 0;
        if (ok) { event_pos = (event_pos + 1u) % EVENT_CAPACITY; event_left--; budget--; }
    }
    if (ok && !frame_left && !event_left && footer_pending)
        ok = fprintf(f, "# export_complete errors=%u\n", trace.export_errors) >= 0;
    if (fclose(f) != 0) ok = false;
    if (!ok) {
        trace.export_errors++;
        write_status("export write failed; retrying");
        return;
    }
    trace.export_frame_pos = frame_pos; trace.export_frame_left = frame_left;
    trace.export_event_pos = event_pos; trace.export_event_left = event_left;
    trace.export_header_pending = header_pending;
    if (!frame_left && !event_left) {
        trace.export_footer_pending = false;
        trace.export_pending = false;
        write_status("export complete");
    }
}

static void write_status(const char * status) {
    if (!refresh_sd_mount()) return;
    FILE * f = fopen(TRACE_STATUS_PATH, "w");
    if (!f) return;
    fprintf(f, "tag=%s %s\n", trace.tag[0] ? trace.tag : "-", status);
    fclose(f);
}

/* Low frequency command polling stays outside frame callbacks. stop is
 * always accepted during motion; only bulk export waits for an idle window. */
static void control_timer_cb(lv_timer_t * timer) {
    (void) timer;
    /* Poll only the command entry each 500 ms; mount parsing and file reads
     * happen only when a command exists, not on every active frame window. */
    if (access(TRACE_COMMAND_PATH, F_OK) != 0) return;
    if (!refresh_sd_mount()) return;
    char command[96];
    if (read_command(command, sizeof(command))) {
        if (strncmp(command, "arm", 3) == 0 && (command[3] == '\0' || command[3] == ' ')) {
            if (trace.armed || trace.export_pending) {
                write_status("arm refused; active capture or export pending");
                return;
            }
            const char * tag = command[3] == ' ' ? command + 4 : "";
            size_t n = strnlen(tag, sizeof(trace.tag) - 1);
            memcpy(trace.tag, tag, n);
            trace.tag[n] = '\0';
            start_capture();
            write_status("armed");
        } else if (strcmp(command, "stop") == 0 || strcmp(command, "export") == 0) {
            if (stop_capture()) write_status("stopped; export pending");
            else if (trace.export_pending) write_status("export already pending");
            else write_status("not armed");
        } else if (strcmp(command, "discard") == 0) {
            if (trace.armed) {
                write_status("discard refused; capture active");
            } else if (trace.export_pending) {
                trace.export_pending = false;
                trace.export_header_pending = trace.export_footer_pending = false;
                trace.frame_count = trace.event_count = 0;
                write_status("pending trace discarded");
            } else {
                write_status("no pending trace");
            }
        }
    }
}

static void idle_timer_cb(lv_timer_t * timer) {
    (void) timer;
    uint64_t now = ui_perf_now_us();
    if (trace.last_activity_us && now - trace.last_activity_us < 150000ULL) return;
    write_export_batch();
}

void ui_frame_trace_install(lv_display_t * disp) {
    if (!disp) return;
    trace.mount_verified = verify_sd_mount();
    lv_display_add_event_cb(disp, display_event_cb, LV_EVENT_INVALIDATE_AREA, NULL);
    lv_display_add_event_cb(disp, display_event_cb, LV_EVENT_RENDER_START, NULL);
    lv_display_add_event_cb(disp, display_event_cb, LV_EVENT_RENDER_READY, NULL);
    lv_timer_create(control_timer_cb, 500, NULL);
    lv_timer_create(idle_timer_cb, 200, NULL);
}

void ui_frame_trace_compositor_present(uint64_t work_us) {
    record_frame(ui_perf_now_us(), work_us > UINT32_MAX ? UINT32_MAX : (uint32_t) work_us, true);
}
#endif
