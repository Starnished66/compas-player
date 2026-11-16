#include "gui_player.h"
#include "../../application/gui_internal.h"
#include "../../../library/playlists/rating_writer.h"
#include "gui_track_info.h"
#include "../../text/i18n/i18n.h"

#include "../../application/gui.h"
#include "../../appearance/gui_theme.h"
#include "../../widgets/builders/screen_builders.h"

#include "../../../library/parsers/cue/cue_parser.h"
#include "../../../library/metadata/parsing/metadata.h"
#include "../../../core/process/worker_thread.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

extern void nav_push(lv_obj_t * screen);

static lv_obj_t * info_screen;
static lv_obj_t * info_list;
static lv_obj_t * info_title;
static gui_track_info_context_t current_context;
static gui_track_info_context_t saved_context;
static bool temporary_context_active;
static uint64_t context_generation;
static uint64_t rendered_context_generation;
static audio_current_format_info_t rendered_runtime;
static bool rendered_runtime_valid;
static int rendered_stars;
static int local_stars;
static bool local_stars_noted;

static uint64_t local_facts_generation;
static bool local_stat_ok;
static uint64_t local_file_size;
static double local_file_duration;
static int info_line_count;

static void gui_track_info_context_restore_cb(lv_event_t * e);

static const char * codec_name(audio_codec_t codec) {
    if (codec == AUDIO_CODEC_APE) return "Monkey's Audio";
    const char * name = audio_codec_name(codec);
    return name[0] ? name : NULL;
}

static audio_codec_t codec_from_container(const char * container) {
    if (!container || !container[0]) return AUDIO_CODEC_UNKNOWN;
    if (strcasecmp(container, "FLAC") == 0) return AUDIO_CODEC_FLAC;
    if (strcasecmp(container, "MP3") == 0) return AUDIO_CODEC_MP3;
    if (strcasecmp(container, "WAV") == 0 || strcasecmp(container, "AIFF") == 0)
        return AUDIO_CODEC_PCM;
    if (strcasecmp(container, "DSF") == 0 || strcasecmp(container, "DFF") == 0)
        return AUDIO_CODEC_DSD;
    if (strcasecmp(container, "AAC") == 0 || strcasecmp(container, "ADTS") == 0)
        return AUDIO_CODEC_AAC;
    /* M4A may carry AAC or ALAC and Ogg may carry Vorbis or Opus. Wait for
     * the actual decoder snapshot instead of presenting a plausible but
     * potentially wrong codec while the track is still opening. */
    if (strcasecmp(container, "WV") == 0) return AUDIO_CODEC_WAVPACK;
    if (strcasecmp(container, "APE") == 0) return AUDIO_CODEC_APE;
    if (strcasecmp(container, "ASF") == 0 || strcasecmp(container, "WMA") == 0)
        return AUDIO_CODEC_WMA;
    return AUDIO_CODEC_UNKNOWN;
}

static void format_rate(unsigned int hz, char * out, size_t out_size) {
    if (hz == 0) {
        out[0] = '\0';
    } else if (hz % 1000 == 0) {
        snprintf(out, out_size, "%u kHz", hz / 1000);
    } else {
        snprintf(out, out_size, "%.1f kHz", (double) hz / 1000.0);
    }
}

static void format_file_size(uint64_t bytes, char * out, size_t out_size) {
    if (bytes >= 1024ULL * 1024ULL * 1024ULL)
        snprintf(out, out_size, "%.2f GB", (double) bytes / (1024.0 * 1024.0 * 1024.0));
    else if (bytes >= 1024ULL * 1024ULL)
        snprintf(out, out_size, "%.1f MB", (double) bytes / (1024.0 * 1024.0));
    else if (bytes >= 1024ULL)
        snprintf(out, out_size, "%.1f KB", (double) bytes / 1024.0);
    else
        snprintf(out, out_size, TR("%llu bytes"), (unsigned long long) bytes);
}

/* Extract only a radio host. Userinfo, port, path, query and fragment are
 * intentionally discarded so even an unusual credential-bearing station
 * URL cannot leak secrets into the UI. */
static void sanitized_url_host(const char * url, char * out, size_t out_size) {
    out[0] = '\0';
    if (!url || out_size == 0) return;
    const char * start = strstr(url, "://");
    start = start ? start + 3 : url;
    const char * end = start + strcspn(start, "/?#");
    const char * host = start;
    for (const char * p = start; p < end; p++) {
        if (*p == '@') host = p + 1;
    }
    const char * host_end = end;
    if (host < end && *host == '[') {
        const char * close = memchr(host, ']', (size_t) (end - host));
        if (close) {
            host++;
            host_end = close;
        }
    } else {
        const char * colon = memchr(host, ':', (size_t) (end - host));
        if (colon) host_end = colon;
    }
    size_t len = (size_t) (host_end - host);
    if (len == 0) return;
    if (len >= out_size) len = out_size - 1;
    memcpy(out, host, len);
    out[len] = '\0';
}

static void add_info_line(const char * name, const char * value) {
    if (!info_list || !name || !value || !value[0]) return;

    lv_obj_t * field = lv_obj_create(info_list);
    lv_obj_set_width(field, lv_pct(100));
    lv_obj_set_height(field, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(field, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(field, 0, 0);
    lv_obj_set_style_pad_all(field, 0, 0);
    lv_obj_set_style_pad_top(field, BOARD_SCALE_PX(8), 0);
    lv_obj_set_flex_flow(field, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(field, BOARD_SCALE_PX(2), 0);
    lv_obj_remove_flag(field, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t * name_label = lv_label_create(field);
    lv_obj_set_width(name_label, lv_pct(100));
    lv_label_set_long_mode(name_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(name_label, name);
    lv_obj_add_style(name_label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(name_label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_add_flag(name_label, LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_obj_t * value_label = lv_label_create(field);
    lv_obj_set_width(value_label, lv_pct(100));
    lv_label_set_long_mode(value_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(value_label, value);
    lv_obj_add_style(value_label, &style_theme_text_primary, 0);
    lv_obj_set_style_text_font(value_label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_add_flag(value_label, LV_OBJ_FLAG_GESTURE_BUBBLE);
    info_line_count++;
}

static void add_info_section(const char * title) {
    lv_obj_t * label = lv_label_create(info_list);
    lv_obj_set_width(label, lv_pct(100));
    lv_label_set_text(label, title);
    lv_obj_add_style(label, &style_theme_text_muted, 0);
    lv_obj_set_style_text_font(label, gui_theme_font(GUI_FONT_ROLE_SUBTEXT), 0);
    lv_obj_set_style_pad_top(label, BOARD_SCALE_PX(14), 0);
    lv_obj_set_style_pad_bottom(label, BOARD_SCALE_PX(2), 0);
}

typedef struct {
    gui_track_info_context_t context;
    uint64_t generation;
    bool stat_ok;
    uint64_t file_size;
    double file_duration;
    int stars;
    atomic_bool done;
} info_job_t;

static info_job_t * info_job;
static pthread_t info_thread;

static void load_file_format(info_job_t * job, const char * path, double cue_start, double cue_end) {
    gui_track_info_context_t * ctx = &job->context;
    audio_current_format_info_t format;
    if (audio_probe_file_format(path, &format)) {
        ctx->declared_codec = format.codec;
        ctx->declared_sample_rate = format.source_sample_rate;
        ctx->declared_bit_depth = format.source_bit_depth;
        ctx->declared_channels = format.channels;
        ctx->declared_bitrate_kbps = format.bitrate_kbps;
        job->file_duration = format.duration_seconds;
        ctx->declared_duration_seconds = (cue_end >= 0 ? cue_end : format.duration_seconds) - cue_start;
    }
}

static void load_file_facts(info_job_t * job) {
    gui_track_info_context_t * ctx = &job->context;
    char path[sizeof(ctx->path)];
    snprintf(path, sizeof(path), "%s", ctx->path);
    track_metadata_t meta;
    double cue_start = 0.0, cue_end = -1.0;
    if (cue_reference_is(path)) {
        cue_sheet_t sheet;
        unsigned ordinal;
        if (!cue_reference_resolve(path, &sheet, &ordinal)) return;
        cue_track_t * track = &sheet.tracks[ordinal - 1];
        snprintf(path, sizeof(path), "%s", track->audio_path);
        cue_start = track->start_seconds;
        cue_end = track->end_seconds;
        metadata_read_cue_track_without_artwork(&sheet, ordinal, &meta);
        cue_sheet_free(&sheet);
    } else {
        metadata_read_without_artwork(ctx->path, &meta);
    }
    const char * suffix = strrchr(path, '.');
    if (suffix) snprintf(ctx->container, sizeof(ctx->container), "%s", suffix + 1);
    struct stat st;
    if (stat(path, &st) == 0 && st.st_size >= 0) {
        job->stat_ok = true;
        job->file_size = (uint64_t) st.st_size;
    }
    load_file_format(job, path, cue_start, cue_end);
    ctx->track_number = meta.track_number;
    ctx->disc_number = meta.disc_number;
    ctx->has_track_number = meta.has_track_number;
    ctx->has_disc_number = meta.has_disc_number;
    free(meta.picture_data);
    free(meta.lyrics);
}

static void * load_local_facts(void * arg) {
    worker_lower_priority(WORKER_NICE_BACKGROUND);
    info_job_t * job = arg;
    job->stars = rating_writer_stars(job->context.path);
    if (job->context.source == GUI_TRACK_SOURCE_LOCAL) load_file_facts(job);
    atomic_store_explicit(&job->done, true, memory_order_release);
    return NULL;
}

static bool info_is_current_track(void) {
    const char * path = gui_player_get_current_track_path();
    return gui_player_has_active_track() && path && strcmp(path, current_context.path) == 0;
}

void gui_track_info_note_current_rating(int stars) {
    if (!info_is_current_track()) return;
    local_stars = stars;
    local_stars_noted = true;
}

static int info_stars(void) {
    return info_is_current_track() ? now_playing_stars : local_stars;
}

static void poll_local_facts(bool load) {
    if (info_job && atomic_load_explicit(&info_job->done, memory_order_acquire)) {
        pthread_join(info_thread, NULL);
        if (info_job->generation == context_generation) {
            current_context = info_job->context;
            if (!local_stars_noted) local_stars = info_job->stars;
            local_stat_ok = info_job->stat_ok;
            local_file_size = info_job->file_size;
            local_file_duration = info_job->file_duration;
            rendered_context_generation = context_generation - 1;
        }
        free(info_job);
        info_job = NULL;
    }
    if (!load || local_facts_generation == context_generation) return;
    local_stars = 0;
    local_stat_ok = false;
    local_file_size = 0;
    local_file_duration = 0;
    if (info_job) return;
    if (!current_context.path[0]) return;
    info_job_t * job = calloc(1, sizeof(*job));
    if (!job) return;
    job->context = current_context;
    job->generation = context_generation;
    atomic_init(&job->done, false);
    if (worker_thread_create(&info_thread, 512 * 1024, load_local_facts, job) != 0) { free(job); return; }
    info_job = job;
    local_stars_noted = false;
    local_facts_generation = context_generation;
}

static bool current_runtime(audio_current_format_info_t * out) {
    if (!audio_get_current_format_info(out)) return false;
    /* The decoder snapshot is still authoritative when the UI-side context
     * has not been populated yet.  The old strict strcmp rejected it against
     * an empty path and left the Information screen completely blank even
     * though a track was actively decoded.  Once a context exists, retain
     * the path check so a late snapshot from the previous track can never be
     * presented as information for the new one. */
    if (!current_context.path[0]) return true;
    return strcmp(out->path, current_context.path) == 0;
}

static void add_rating_line(void) {
    int stars = info_stars();
    if (stars <= 0) return;
    char value[16];
    snprintf(value, sizeof(value), "%d/5", stars);
    add_info_line(TR("Rating"), value);
}

static void rebuild_info_text(const audio_current_format_info_t * runtime, bool runtime_valid) {
    if (!info_list) return;
    lv_obj_clean(info_list);
    info_line_count = 0;

    char value[256];
    char rate[48];
    audio_codec_t codec = runtime_valid && runtime->codec != AUDIO_CODEC_UNKNOWN
        ? runtime->codec : current_context.declared_codec;
    if (codec == AUDIO_CODEC_UNKNOWN) codec = codec_from_container(current_context.container);
    add_info_section(TR("Audio"));
    add_info_line(TR("Codec"), codec_name(codec));
    add_info_line(TR("Container"), current_context.container);

    unsigned int source_rate = runtime_valid && runtime->source_sample_rate
        ? runtime->source_sample_rate : current_context.declared_sample_rate;
    unsigned int source_depth = runtime_valid && runtime->source_bit_depth
        ? runtime->source_bit_depth : current_context.declared_bit_depth;
    if (!source_rate && !current_context.path[0]) source_rate = audio_get_sample_rate();
    format_rate(source_rate, rate, sizeof(rate));
    value[0] = '\0';
    if (codec == AUDIO_CODEC_DSD && source_rate) {
        snprintf(value, sizeof(value), "1-bit DSD / %.4g MHz", (double) source_rate / 1000000.0);
    } else if (source_depth && rate[0]) {
        snprintf(value, sizeof(value), "%u-bit / %s", source_depth, rate);
    } else if (rate[0]) {
        snprintf(value, sizeof(value), "%s", rate);
    } else if (source_depth) {
        snprintf(value, sizeof(value), "%u-bit", source_depth);
    }
    add_info_line(TR("Source"), value);

    if (runtime_valid && runtime->dsd_native) {
        snprintf(value, sizeof(value), TR("Native DSD (DoP) / %.4g MHz"), (double) source_rate / 1000000.0);
        add_info_line(TR("Output"), value);
    } else if (runtime_valid && runtime->output_sample_rate) {
        format_rate(runtime->output_sample_rate, rate, sizeof(rate));
        snprintf(value, sizeof(value), "%u-bit PCM / %s",
                 runtime->output_bit_depth ? runtime->output_bit_depth : 16, rate);
        add_info_line(TR("Output"), value);
    }

    unsigned int channels = runtime_valid && runtime->channels
        ? runtime->channels : current_context.declared_channels;
    if (channels == 1) snprintf(value, sizeof(value), "%s", TR("Mono (1 channel)"));
    else if (channels == 2) snprintf(value, sizeof(value), "%s", TR("Stereo (2 channels)"));
    else if (channels > 0) snprintf(value, sizeof(value), TR("%u channels"), channels);
    else value[0] = '\0';
    add_info_line(TR("Channels"), value);

    unsigned int bitrate = runtime_valid && runtime->bitrate_kbps
        ? runtime->bitrate_kbps : current_context.declared_bitrate_kbps;
    bool bitrate_estimated = false;
    double duration = runtime_valid && runtime->duration_seconds > 0.0
        ? runtime->duration_seconds : current_context.declared_duration_seconds;
    if (duration <= 0.0 && !current_context.path[0]) duration = audio_get_duration_seconds();
    /* CUE duration describes one track; its file size describes the whole file. */
    double file_duration = local_file_duration > 0.0 ? local_file_duration : duration;
    if (!bitrate && local_stat_ok && file_duration > 0.0) {
        double estimate = ((double) local_file_size * 8.0) / file_duration / 1000.0;
        if (estimate > 0.0 && estimate < 1000000.0) {
            bitrate = (unsigned int) (estimate + 0.5);
            bitrate_estimated = true;
        }
    }
    if (bitrate) snprintf(value, sizeof(value), "%s%u kbps", bitrate_estimated ? "~" : "", bitrate);
    else value[0] = '\0';
    add_info_line(TR("Bitrate"), value);

    if (duration > 0.0) {
        gui_format_time(duration, value, sizeof(value));
    } else {
        value[0] = '\0';
    }
    add_info_line(TR("Duration"), value);
    if (local_stat_ok) {
        format_file_size(local_file_size, value, sizeof(value));
        add_info_line(TR("File size"), value);
    }

    add_info_section(TR("Track"));
    add_rating_line();
    value[0] = '\0';
    if (current_context.has_disc_number && current_context.has_track_number)
        snprintf(value, sizeof(value), TR("Disc %d / Track %d"), current_context.disc_number,
                 current_context.track_number);
    else if (current_context.has_disc_number)
        snprintf(value, sizeof(value), TR("Disc %d"), current_context.disc_number);
    else if (current_context.has_track_number)
        snprintf(value, sizeof(value), TR("Track %d"), current_context.track_number);
    add_info_line(TR("Position"), value);

    value[0] = '\0';
    if (runtime_valid && runtime->replaygain_applied) {
        snprintf(value, sizeof(value), TR("Applied %+.1f dB"), runtime->replaygain_applied_db);
    } else if (current_context.has_replaygain_track || current_context.has_replaygain_album) {
        if (current_context.replaygain_mode == 0) {
            snprintf(value, sizeof(value), "%s", TR("Off"));
        } else if (current_context.replaygain_mode == 2 && current_context.has_replaygain_album) {
            snprintf(value, sizeof(value), TR("Album %+.1f dB"), current_context.replaygain_album_db);
        } else if (current_context.has_replaygain_track) {
            snprintf(value, sizeof(value), TR("Track %+.1f dB"), current_context.replaygain_track_db);
        } else {
            snprintf(value, sizeof(value), TR("Album %+.1f dB"), current_context.replaygain_album_db);
        }
    }
    add_info_line(TR("ReplayGain"), value);

    add_info_section(TR("Source"));
    value[0] = '\0';
    if (current_context.source == GUI_TRACK_SOURCE_SUBSONIC) {
        add_info_line(TR("Provider"), TR("Subsonic"));
    } else if (current_context.source == GUI_TRACK_SOURCE_PLUGIN) {
        if (current_context.provider[0] && current_context.track_id[0])
            snprintf(value, sizeof(value), "%s / %s", current_context.provider, current_context.track_id);
        else
            snprintf(value, sizeof(value), "%s", current_context.provider);
        add_info_line(TR("Provider"), value);
    } else if (current_context.source == GUI_TRACK_SOURCE_RADIO) {
        char host[192];
        sanitized_url_host(current_context.path, host, sizeof(host));
        if (host[0]) snprintf(value, sizeof(value), TR("Radio / %s"), host);
        else snprintf(value, sizeof(value), "%s", TR("Radio"));
        add_info_line(TR("Provider"), value);
    } else if (runtime_valid && runtime->is_stream) {
        /* Do not expose runtime->path here: it may be a signed/authenticated
         * URL.  This fallback only describes the source generically. */
        add_info_line(TR("Provider"), TR("Network stream"));
    } else {
        const char * local_path = current_context.path;
        if (!local_path[0] && runtime_valid && !runtime->is_stream &&
            !strstr(runtime->path, "://"))
            local_path = runtime->path;
        /* Display the card name without changing the path used for I/O. */
        const char * card_path = NULL;
        const char * card_roots[] = { "/data/mnt/sd_0", "/usr/data/mnt/sd_0"
#ifdef HOST_BUILD
            , "./music"
#endif
        };
        for (size_t i = 0; i < sizeof(card_roots) / sizeof(card_roots[0]); i++) {
            size_t len = strlen(card_roots[i]);
            if (strncmp(local_path, card_roots[i], len) == 0 &&
                (local_path[len] == '/' || local_path[len] == '\0')) {
                card_path = local_path + len;
                while (*card_path == '/') card_path++;
                break;
            }
        }
        if (card_path) {
            char location[sizeof(current_context.path) + 3];
            snprintf(location, sizeof(location), "SD/%s", card_path);
            add_info_line(TR("Location"), location);
        } else {
            add_info_line(TR("Location"), local_path);
        }
    }

    if (info_line_count == 0) {
        lv_obj_t * label = lv_label_create(info_list);
        lv_obj_set_width(label, lv_pct(100));
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_obj_add_style(label, &style_theme_text_primary, 0);
        lv_obj_set_style_text_font(label, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
        lv_obj_set_style_pad_top(label, BOARD_SCALE_PX(8), 0);
        lv_label_set_text(label, TR("Track details are not available yet. Keep playback open and check again."));
    }
}

/* For gui_reload.c's in-process UI reload -- gui_track_info_init() is
 * guarded (`if (info_screen) return;`) so it silently no-ops on a second
 * call unless info_screen is deleted AND nulled first here. */
void gui_track_info_teardown(void) {
    if (!info_screen) return;
    if (temporary_context_active) { current_context = saved_context; context_generation++; }
    temporary_context_active = false;
    lv_obj_delete(info_screen);
    info_screen = NULL;
    info_list = NULL;
    info_title = NULL;
}

void gui_track_info_init(void) {
    if (info_screen) return;
    info_screen = build_subsonic_list_screen(TR("Information"), &info_title, &info_list);
    lv_obj_add_event_cb(info_screen, gui_track_info_context_restore_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
    /* Plain left-aligned Name: value labels, same pattern as Wi-Fi Info.
     * No pill rows, no marquee.  The list stays vertically scrollable for a
     * long local path or a larger font size. */
    lv_obj_set_flex_align(info_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_left(info_list, BOARD_SCALE_PX(24), 0);
    lv_obj_set_style_pad_right(info_list, BOARD_SCALE_PX(24), 0);
    lv_obj_set_style_pad_top(info_list, BOARD_SCALE_PX(12), 0);
    lv_obj_set_style_pad_bottom(info_list, BOARD_SCALE_PX(8), 0);
}

lv_obj_t * gui_track_info_get_screen(void) {
    return info_screen;
}

void gui_track_info_set_current(const gui_track_info_context_t * context) {
    if (temporary_context_active) {
        if (context) saved_context = *context;
        else memset(&saved_context, 0, sizeof(saved_context));
        return;
    }
    if (context) current_context = *context;
    else memset(&current_context, 0, sizeof(current_context));
    context_generation++;
}

static void gui_track_info_context_restore_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_SCREEN_UNLOADED || !temporary_context_active) return;
    temporary_context_active = false;
    current_context = saved_context;
    context_generation++;
}

void gui_track_info_open(void) {
    if (!info_screen) gui_track_info_init();
    poll_local_facts(true);
    audio_current_format_info_t runtime;
    bool valid = current_runtime(&runtime);
    rebuild_info_text(valid ? &runtime : NULL, valid);
    rendered_stars = info_stars();
    rendered_context_generation = context_generation;
    rendered_runtime_valid = valid;
    if (valid) rendered_runtime = runtime;
    else memset(&rendered_runtime, 0, sizeof(rendered_runtime));
    nav_push(info_screen);
}

void gui_track_info_open_for_context(const gui_track_info_context_t * context) {
    if (!context) return;
    if (!info_screen) gui_track_info_init();
    if (!temporary_context_active) saved_context = current_context;
    temporary_context_active = true;
    current_context = *context;
    context_generation++;
    gui_track_info_open();
}

void gui_track_info_poll(void) {
    bool visible = info_screen && lv_screen_active() == info_screen;
    poll_local_facts(visible);
    if (!visible) return;
    audio_current_format_info_t runtime;
    bool valid = current_runtime(&runtime);
    if (valid) runtime.generation = 0; /* seek-only republishes do not change displayed facts */
    audio_current_format_info_t previous = rendered_runtime;
    previous.generation = 0;
    bool runtime_changed = valid != rendered_runtime_valid ||
        (valid && memcmp(&runtime, &previous, sizeof(runtime)) != 0);
    if (rendered_context_generation == context_generation && !runtime_changed &&
        rendered_stars == info_stars()) return;

    rebuild_info_text(valid ? &runtime : NULL, valid);
    rendered_stars = info_stars();
    rendered_context_generation = context_generation;
    rendered_runtime_valid = valid;
    if (valid) rendered_runtime = runtime;
    else memset(&rendered_runtime, 0, sizeof(rendered_runtime));
}
