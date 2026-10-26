#define _FILE_OFFSET_BITS 64

#include "cue_parser.h"
#include "../../sacd/sacd_iso.h"
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <wavpack.h>

#define CUE_TEXT_MAX (1024u * 1024u)
#define CUE_TRACK_MAX 1024
#define CUE_FILE_MAX 1024
#define CUE_FRAGMENT "#compas-cue="

typedef struct {
    char name[600];
} cue_file_t;

static bool copy_text_arg(const char *p, char *out, size_t cap) {
    while (*p == ' ' || *p == '\t')
        p++;
    const char *start = p, *end;
    if (*p == '"') {
        start = ++p;
        end = strchr(start, '"');
        if (!end)
            return false;
    } else {
        end = p;
        while (*end && !isspace((unsigned char)*end))
            end++;
    }
    size_t n = (size_t)(end - start);
    if (!n || n >= cap)
        return false;
    memcpy(out, start, n);
    out[n] = 0;
    return true;
}

static const char *keyword(const char *s, const char *word) {
    size_t n = strlen(word);
    if (strlen(s) < n || strncasecmp(s, word, n) || (s[n] && !isspace((unsigned char)s[n])))
        return NULL;
    s += n;
    while (*s == ' ' || *s == '\t')
        s++;
    return s;
}

static bool parse_time(const char *s, double *seconds) {
    unsigned long v[3] = {0};
    for (int i = 0; i < 3; ++i) {
        if (!isdigit((unsigned char)*s))
            return false;
        unsigned digits = 0;
        while (isdigit((unsigned char)*s)) {
            if (++digits > 7 || v[i] > 10000000)
                return false;
            v[i] = v[i] * 10 + (unsigned)(*s++ - '0');
        }
        if (i < 2) {
            if (*s != ':')
                return false;
            s++;
        }
    }
    while (isspace((unsigned char)*s))
        s++;
    if (*s || v[1] > 59 || v[2] > 74)
        return false;
    *seconds = v[0] * 60.0 + v[1] + v[2] / 75.0;
    return true;
}

static bool is_absolute_name(const char *p) {
    size_t len = strlen(p);
    return p[0] == '/' || p[0] == '\\' ||
           (len >= 3 && isalpha((unsigned char)p[0]) && p[1] == ':' &&
            (p[2] == '/' || p[2] == '\\'));
}

static bool resolve_name(const char *base, const char *name, char out[600]) {
    if (is_absolute_name(name)) {
        if (strlen(name) >= 600)
            return false;
        strcpy(out, name);
        return true;
    }
    const char *slash1 = strrchr(base, '/'), *slash2 = strrchr(base, '\\');
    const char *slash = slash1;
    if (slash2 && (!slash || slash2 > slash))
        slash = slash2;
    size_t dir = slash ? (size_t)(slash - base + 1) : 0, n = strlen(name);
    if (dir >= 600 || n >= 600 - dir)
        return false;
    memcpy(out, base, dir);
    memcpy(out + dir, name, n + 1);
    return true;
}

static bool append_track(cue_track_t **a, int *n, int *cap, int number, int file_id) {
    if (*n >= CUE_TRACK_MAX)
        return false;
    if (*n == *cap) {
        int nc = *cap ? *cap * 2 : 8;
        cue_track_t *p = realloc(*a, (size_t)nc * sizeof(**a));
        if (!p)
            return false;
        *a = p;
        *cap = nc;
    }
    cue_track_t *t = &(*a)[(*n)++];
    memset(t, 0, sizeof(*t));
    t->number = number;
    t->end_seconds = -1;
    (void)file_id;
    return true;
}

static bool parse_text(const char *text, size_t len, const char *base_path, bool embedded,
                       cue_sheet_t *out) {
    if (len > CUE_TEXT_MAX || memchr(text, '\0', len))
        return false;
    cue_file_t *files = NULL;
    int nf = 0, cf = 0, current_file = -1;
    cue_track_t *tracks = NULL;
    int nt = 0, ct = 0, current = -1;
    char global_title[128] = "", global_performer[128] = "";
    bool bad = false;
    char *copy = malloc(len + 1);
    if (!copy)
        return false;
    memcpy(copy, text, len);
    copy[len] = 0;
    char *save = NULL;
    for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (strlen(line) >= 3 && (unsigned char)line[0] == 0xef && (unsigned char)line[1] == 0xbb &&
            (unsigned char)line[2] == 0xbf)
            line += 3;
        while (*line == ' ' || *line == '\t' || *line == '\r')
            line++;
        const char *a;
        if ((a = keyword(line, "FILE"))) {
            char name[600];
            if (embedded) {
                while (*a == ' ' || *a == '\t')
                    a++;
                if (!*a) {
                    bad = true;
                    break;
                }
                current_file = 0;
                if (!nf) {
                    files = calloc(1, sizeof(*files));
                    if (!files) {
                        bad = true;
                        break;
                    }
                    nf = cf = 1;
                    if (strlen(base_path) >= sizeof(files[0].name)) {
                        bad = true;
                        break;
                    }
                    strcpy(files[0].name, base_path);
                }
            } else {
                if (nf >= CUE_FILE_MAX) {
                    bad = true;
                    break;
                }
                if (!copy_text_arg(a, name, sizeof(name))) {
                    bad = true;
                    break;
                }
                if (nf == cf) {
                    int nc = cf ? cf * 2 : 4;
                    cue_file_t *p = realloc(files, (size_t)nc * sizeof(*p));
                    if (!p) {
                        bad = true;
                        break;
                    }
                    files = p;
                    cf = nc;
                }
                if (!resolve_name(base_path, name, files[nf].name)) {
                    bad = true;
                    break;
                }
                if (cue_reference_is(files[nf].name)) {
                    bad = true;
                    break;
                }
                current_file = nf++;
            }
            current = -1;
        } else if ((a = keyword(line, "TITLE"))) {
            char val[128];
            if (copy_text_arg(a, val, sizeof(val))) {
                if (current >= 0)
                    strcpy(tracks[current].title, val);
                else
                    strcpy(global_title, val);
            }
        } else if ((a = keyword(line, "PERFORMER"))) {
            char val[128];
            if (copy_text_arg(a, val, sizeof(val))) {
                if (current >= 0)
                    strcpy(tracks[current].performer, val);
                else
                    strcpy(global_performer, val);
            }
        } else if ((a = keyword(line, "TRACK"))) {
            int no = 0;
            char type[16] = "";
            if (sscanf(a, "%d %15s", &no, type) != 2) {
                current = -1;
                continue;
            }
            current = -1;
            if (!strcasecmp(type, "AUDIO")) {
                if (current_file < 0 || !append_track(&tracks, &nt, &ct, no, current_file)) {
                    bad = true;
                    break;
                }
                current = nt - 1;
                tracks[current].audio_path[0] = 0;
            }
        } else if (current >= 0 && (a = keyword(line, "INDEX"))) {
            int ix = -1;
            char time[64] = "";
            if (sscanf(a, "%d %63s", &ix, time) != 2)
                continue;
            if (ix == 1) {
                double sec;
                if (!parse_time(time, &sec)) {
                    bad = true;
                    break;
                }
                tracks[current].start_seconds = sec;
                tracks[current].has_index_01 = true;
            }
        }
        if (current >= 0 && current_file >= 0 && !tracks[current].audio_path[0])
            strcpy(tracks[current].audio_path, files[current_file].name);
    }
    if (!embedded && nf < 1)
        bad = true;
    if (nt < 1)
        bad = true;
    for (int i = 0; i < nt && !bad; i++) {
        if (!tracks[i].has_index_01 || !tracks[i].audio_path[0]) {
            bad = true;
            break;
        }
        if (!tracks[i].title[0])
            snprintf(tracks[i].title, sizeof(tracks[i].title), "Track %02d", tracks[i].number);
        if (!tracks[i].performer[0])
            snprintf(tracks[i].performer, sizeof(tracks[i].performer), "%s", global_performer);
        for (int j = 0; j < i; j++)
            if (!strcmp(tracks[i].audio_path, tracks[j].audio_path) &&
                tracks[i].start_seconds <= tracks[j].start_seconds) {
                bad = true;
                break;
            }
    }
    for (int i = 0; i < nt; i++)
        for (int j = i + 1; j < nt; j++)
            if (!strcmp(tracks[i].audio_path, tracks[j].audio_path) &&
                tracks[j].start_seconds > tracks[i].start_seconds &&
                (tracks[i].end_seconds < 0 || tracks[j].start_seconds < tracks[i].end_seconds))
                tracks[i].end_seconds = tracks[j].start_seconds;
    if (!bad) {
        strncpy(out->audio_path, tracks[0].audio_path, sizeof(out->audio_path) - 1);
        strncpy(out->title, global_title, sizeof(out->title) - 1);
        strncpy(out->performer, global_performer, sizeof(out->performer) - 1);
        out->tracks = tracks;
        out->track_count = nt;
        tracks = NULL;
    }
    free(tracks);
    free(files);
    free(copy);
    return !bad;
}

bool cue_parse_file(const char *path, cue_sheet_t *out) {
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));
    if (!path)
        return false;
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    if (fseek(f, 0, SEEK_END)) {
        fclose(f);
        return false;
    }
    long z = ftell(f);
    if (z < 0 || (unsigned long)z > CUE_TEXT_MAX || fseek(f, 0, SEEK_SET)) {
        fclose(f);
        return false;
    }
    char *b = malloc((size_t)z + 1);
    if (!b) {
        fclose(f);
        return false;
    }
    size_t n = fread(b, 1, (size_t)z, f);
    fclose(f);
    bool ok = n == (size_t)z && parse_text(b, n, path, false, out);
    free(b);
    if (!ok)
        cue_sheet_free(out);
    return ok;
}

/* FLAC metadata blocks are read directly so builds without libFLAC still
 * support native CUESHEET and Vorbis-comment CUESHEET fields. */
static uint32_t be24(const unsigned char *p) {
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}
static uint32_t le32(const unsigned char *p) {
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t be64(const unsigned char *p) {
    uint64_t x = 0;
    for (int i = 0; i < 8; i++)
        x = (x << 8) | p[i];
    return x;
}
static bool parse_vorbis(const unsigned char *p, size_t n, char **cue) {
    if (n < 8)
        return false;
    uint32_t vendor = le32(p);
    p += 4;
    n -= 4;
    if (vendor > n)
        return false;
    p += vendor;
    n -= vendor;
    if (n < 4)
        return false;
    uint32_t count = le32(p);
    p += 4;
    n -= 4;
    if (count > 100000)
        return false;
    char *candidate = NULL;
    for (uint32_t i = 0; i < count; i++) {
        if (n < 4) {
            free(candidate);
            return false;
        }
        uint32_t l = le32(p);
        p += 4;
        n -= 4;
        if (l > n) {
            free(candidate);
            return false;
        }
        if (l >= 9 && !strncasecmp((const char *)p, "CUESHEET=", 9)) {
            if (!*cue && !candidate) {
                candidate = malloc((size_t)l - 8 + 1);
                if (!candidate)
                    return false;
                memcpy(candidate, p + 9, l - 9);
                candidate[l - 9] = 0;
            }
        }
        p += l;
        n -= l;
    }
    if (candidate)
        *cue = candidate;
    return candidate != NULL || *cue != NULL;
}

static bool parse_native_cuesheet(const unsigned char *sheet, size_t sheet_n, uint32_t rate,
                                  const char *path, cue_sheet_t *out) {
    if (sheet_n < 396 || !rate)
        return false;
    size_t pos = 396;
    unsigned count = sheet[395];
    unsigned leadout_number = sheet[136] & 0x80 ? 170 : 255;
    cue_track_t *tracks = NULL;
    int track_count = 0, track_capacity = 0;
    bool saw_leadout = false, failed = false;

    for (unsigned i = 0; i < count; i++) {
        if (pos > sheet_n || sheet_n - pos < 36) {
            failed = true;
            break;
        }
        uint64_t track_offset = be64(sheet + pos);
        unsigned number = sheet[pos + 8];
        unsigned flags = sheet[pos + 21];
        unsigned index_count = sheet[pos + 35];
        pos += 36;
        if ((size_t)index_count > (sheet_n - pos) / 12) {
            failed = true;
            break;
        }
        if (i + 1 == count && number == leadout_number) {
            saw_leadout = true;
            if (track_count == 0) {
                failed = true;
                break;
            }
            double leadout_seconds = (double)track_offset / rate;
            if (leadout_seconds <= tracks[track_count - 1].start_seconds) {
                failed = true;
                break;
            }
            tracks[track_count - 1].end_seconds = leadout_seconds;
            break;
        }
        if (number == leadout_number) {
            failed = true;
            break;
        }

        bool has_index_01 = false;
        uint64_t sample = track_offset;
        for (unsigned j = 0; j < index_count; j++) {
            uint64_t index_offset = be64(sheet + pos);
            unsigned index_number = sheet[pos + 8];
            pos += 12;
            if (index_number == 1) {
                if (has_index_01 || index_offset > UINT64_MAX - track_offset) {
                    failed = true;
                    break;
                }
                sample = track_offset + index_offset;
                has_index_01 = true;
            }
        }
        if (failed)
            break;
        if (flags & 0x80)
            continue;
        if (!has_index_01 ||
            !append_track(&tracks, &track_count, &track_capacity, (int)number, 0)) {
            failed = true;
            break;
        }
        cue_track_t *track = &tracks[track_count - 1];
        track->start_seconds = (double)sample / rate;
        track->has_index_01 = true;
        memcpy(track->audio_path, path, strlen(path) + 1);
        snprintf(track->title, sizeof(track->title), "Track %02u", number);
        if (track_count > 1 && track->start_seconds <= tracks[track_count - 2].start_seconds) {
            failed = true;
            break;
        }
    }

    if (!saw_leadout || !track_count)
        failed = true;
    if (!failed) {
        for (int i = 0; i + 1 < track_count; i++)
            tracks[i].end_seconds = tracks[i + 1].start_seconds;
        memcpy(out->audio_path, path, strlen(path) + 1);
        out->tracks = tracks;
        out->track_count = track_count;
        return true;
    }
    free(tracks);
    return false;
}

bool cue_parse_embedded(const char *path, cue_sheet_t *out) {
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));
    if (!path)
        return false;
    if (strlen(path) >= sizeof(out->audio_path))
        return false;
    const char *ext = strrchr(path, '.');
    if (!ext)
        return false;
    if (!strcasecmp(ext, ".iso")) {
        FILE *file = fopen(path, "rb");
        sacd_toc_t *toc = calloc(1, sizeof(*toc));
        bool ok = file && toc && sacd_read_toc(file, toc);
        if (file) fclose(file);
        if (!ok) { free(toc); return false; }
        out->tracks = calloc(toc->track_count, sizeof(*out->tracks));
        if (!out->tracks) { free(toc); return false; }
        out->track_count = (int)toc->track_count;
        snprintf(out->audio_path, sizeof(out->audio_path), "%s", path);
        snprintf(out->title, sizeof(out->title), "%s", toc->title);
        snprintf(out->performer, sizeof(out->performer), "%s", toc->artist);
        for (unsigned i = 0; i < toc->track_count; ++i) {
            cue_track_t *track = &out->tracks[i];
            track->number = (int)i + 1;
            snprintf(track->title, sizeof(track->title), "%s", toc->tracks[i].title);
            snprintf(track->performer, sizeof(track->performer), "%s", toc->tracks[i].artist);
            snprintf(track->audio_path, sizeof(track->audio_path), "%s", path);
            track->start_seconds = (double)toc->tracks[i].start_frame / 75.0;
            uint32_t end_frame = i + 1 < toc->track_count
                ? toc->tracks[i + 1].start_frame
                : toc->tracks[i].start_frame + toc->tracks[i].frame_count;
            track->end_seconds = (double)end_frame / 75.0;
            track->has_index_01 = true;
        }
        free(toc);
        return true;
    }
    if (!strcasecmp(ext, ".wv")) {
        WavpackContext *w = WavpackOpenFileInput(path, NULL, OPEN_TAGS, 0);
        if (!w)
            return false;
        int tag_size = WavpackGetTagItem(w, "CUESHEET", NULL, 0);
        if (tag_size <= 0 || (unsigned)tag_size > CUE_TEXT_MAX) {
            WavpackCloseFile(w);
            return false;
        }
        char *tag = malloc((size_t)tag_size + 1);
        if (!tag) {
            WavpackCloseFile(w);
            return false;
        }
        int got = WavpackGetTagItem(w, "CUESHEET", tag, tag_size + 1);
        WavpackCloseFile(w);
        if (got != tag_size) {
            free(tag);
            return false;
        }
        tag[got] = 0;
        bool ok = parse_text(tag, (size_t)got, path, true, out);
        free(tag);
        if (ok) {
            strncpy(out->audio_path, path, sizeof(out->audio_path) - 1);
            for (int i = 0; i < out->track_count; i++)
                strncpy(out->tracks[i].audio_path, path, sizeof(out->tracks[i].audio_path) - 1);
        }
        return ok;
    }
    if (strcasecmp(ext, ".flac"))
        return false;
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    unsigned char magic[4];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "fLaC", 4)) {
        fclose(f);
        return false;
    }
    off_t file_end;
    if (fseeko(f, 0, SEEK_END) || (file_end = ftello(f)) < 0 || fseeko(f, 4, SEEK_SET)) {
        fclose(f);
        return false;
    }
    char *comment = NULL;
    unsigned char *sheet = NULL;
    size_t sheet_n = 0;
    uint32_t rate = 0;
    bool last = false;
    bool metadata_valid = true;
    size_t metadata_bytes = 0;
    unsigned block_count = 0;
    while (!last) {
        if (++block_count > 1024) {
            metadata_valid = false;
            break;
        }
        unsigned char h[4];
        if (fread(h, 1, 4, f) != 4) {
            metadata_valid = false;
            break;
        }
        last = !!(h[0] & 128);
        unsigned type = h[0] & 127;
        uint32_t n = be24(h + 1);
        off_t data_start = ftello(f);
        if (data_start < 0 || data_start > file_end ||
            (uintmax_t)n > (uintmax_t)(file_end - data_start)) {
            metadata_valid = false;
            break;
        }
        bool relevant = type == 0 || type == 4 || type == 5;
        size_t block_limit = type == 0 ? 34 : CUE_TEXT_MAX;
        if (relevant &&
            (n > block_limit || (type == 0 && n != 34) || metadata_bytes > CUE_TEXT_MAX - n)) {
            metadata_valid = false;
            break;
        }
        if (!relevant) {
            if (fseeko(f, (off_t)n, SEEK_CUR)) {
                metadata_valid = false;
                break;
            }
            continue;
        }
        metadata_bytes += n;
        unsigned char *b = malloc(n ? n : 1);
        if (!b) {
            metadata_valid = false;
            break;
        }
        if (fread(b, 1, n, f) != n) {
            free(b);
            metadata_valid = false;
            break;
        }
        if (type == 0 && n == 34) {
            uint64_t packed = be64(b + 10);
            rate = (uint32_t)((packed >> 44) & 0xfffff);
        } else if (type == 4)
            parse_vorbis(b, n, &comment);
        else if (type == 5) {
            free(sheet);
            sheet = b;
            sheet_n = n;
            b = NULL;
        }
        free(b);
    }
    fclose(f);
    if (!last || !metadata_valid) {
        free(comment);
        free(sheet);
        return false;
    }
    bool ok = false;
    if (comment)
        ok = parse_text(comment, strlen(comment), path, true, out);
    if (!ok && sheet && sheet_n >= 396 &&
        rate) { /* map native track offsets/index 01 into standard text */
        ok = parse_native_cuesheet(sheet, sheet_n, rate, path, out);
    }
    free(comment);
    free(sheet);
    if (ok) {
        strncpy(out->audio_path, path, sizeof(out->audio_path) - 1);
        for (int i = 0; i < out->track_count; i++)
            strncpy(out->tracks[i].audio_path, path, sizeof(out->tracks[i].audio_path) - 1);
    } else
        cue_sheet_free(out);
    return ok;
}

bool cue_reference_format(const char *path, unsigned ordinal, char *out, size_t cap) {
    if (!path || !*path || cue_reference_is(path) || !ordinal || !out || !cap)
        return false;
    int n = snprintf(out, cap, "%s" CUE_FRAGMENT "%u", path, ordinal);
    return n >= 0 && (size_t)n < cap;
}
bool cue_reference_is(const char *s) { return s && strstr(s, CUE_FRAGMENT) != NULL; }
bool cue_reference_parse(const char *s, char *out, size_t cap, unsigned *ordinal) {
    if (!s || !out || !cap || !ordinal)
        return false;
    const char *p = strstr(s, CUE_FRAGMENT);
    if (!p || strstr(p + 1, CUE_FRAGMENT) || p == s)
        return false;
    const char *n = p + strlen(CUE_FRAGMENT);
    if (!*n)
        return false;
    unsigned long v = 0;
    for (; *n; n++) {
        if (!isdigit((unsigned char)*n))
            return false;
        if (v > (UINT32_MAX - (unsigned)(*n - '0')) / 10)
            return false;
        v = v * 10 + (unsigned)(*n - '0');
    }
    if (!v || (size_t)(p - s) >= cap)
        return false;
    memmove(out, s, (size_t)(p - s));
    out[p - s] = 0;
    *ordinal = (unsigned)v;
    return true;
}
bool cue_reference_resolve(const char *s, cue_sheet_t *out, unsigned *ord) {
    char path[600];
    unsigned n;
    if (!cue_reference_parse(s, path, sizeof(path), &n) || !out)
        return false;
    const char *e = strrchr(path, '.');
    const char *slash = strrchr(path, '/');
    const char *backslash = strrchr(path, '\\');
    if (backslash && (!slash || backslash > slash))
        slash = backslash;
    if (e && slash && e < slash)
        e = NULL;
    bool ok =
        e && !strcasecmp(e, ".cue") ? cue_parse_file(path, out) : cue_parse_embedded(path, out);
    if (!ok)
        return false;
    if (n > (unsigned)out->track_count) {
        cue_sheet_free(out);
        return false;
    }
    if (ord)
        *ord = n;
    return true;
}
bool cue_reference_audio_path(const char *reference, char *out, size_t cap) {
    if (!out || !cap)
        return false;
    cue_sheet_t sheet;
    unsigned ordinal;
    if (!cue_reference_resolve(reference, &sheet, &ordinal))
        return false;
    const char *path = sheet.tracks[ordinal - 1].audio_path;
    size_t len = strlen(path);
    bool ok = len < cap;
    if (ok)
        memcpy(out, path, len + 1);
    cue_sheet_free(&sheet);
    return ok;
}
void cue_sheet_free(cue_sheet_t *out) {
    if (!out)
        return;
    free(out->tracks);
    memset(out, 0, sizeof(*out));
}
