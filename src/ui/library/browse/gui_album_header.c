#include "gui_album_header_internal.h"
#include "gui_album_facts.h"
#include "gui_album_cover.h"
#include "../library_thumbnails.h"
#include "../../text/i18n/i18n.h"
#include "../../appearance/gui_theme.h"
#include "../../widgets/builders/screen_builders.h"
#include "../../../library/metadata/durations/duration_lookup.h"
#include "../../../core/system/runtime/board_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>


static void load_record(void) {
    free(A.track);
    A.track = NULL;
    A.have_record = gui_album_facts_lookup(A.key, A.count, &A.year, A.genre, sizeof(A.genre), &A.track);
}

static int shown_track_number(int index) {
    return A.track && index >= 0 && index < A.count && A.track[index] > 0 ? A.track[index] : index + 1;
}

static int widest_track_number(void) {
    int widest = 1;
    for (int i = 0; i < A.count; i++) if (shown_track_number(i) > widest) widest = shown_track_number(i);
    return widest;
}

/* Hides a chip whose data is not known; never shows a partial total. The chips
 * are only rebuilt when what they show changes, so a probe result that adds no
 * chip costs nothing. The column's height needs no measuring: it is content
 * sized with the cover as its minimum (see gui_album_header_build()), so LVGL's
 * normal layout pass grows it when wrapping chips need more room. */
static bool update_chips(void) {
    if (!A.view.chips) return false;
    char format[sizeof(A.chip_format)] = "", total_text[sizeof(A.chip_total)] = "";
    bool hires_chip = false;
    if (album_settled()) {
        /* A failed track is known but unplayable: the chips cover the playable ones. */
        bool mixed = false, hires = true;
        double total = 0;
        int playable = 0, first = -1;
        for (int i = 0; i < A.count; i++) {
            if (A.state[i] != 1) continue;
            if (first < 0) first = i;
            playable++;
            total += A.dur[i].duration_ms / 1000.0;
            if (!duration_info_is_hires(&A.dur[i])) hires = false;
            if (A.dur[i].codec != A.dur[first].codec) mixed = true;
        }
        snprintf(format, sizeof(format), "%s",
                 playable ? (mixed ? TR("Mixed") : audio_codec_name((audio_codec_t) A.dur[first].codec)) : "");
        hires_chip = playable && hires && A.view.hires_icon[0];
        if (playable) gui_album_view_total_text(total, total_text, sizeof(total_text));
    }
    if (hires_chip == A.chip_hires && !strcmp(format, A.chip_format) && !strcmp(total_text, A.chip_total)) return false;
    snprintf(A.chip_format, sizeof(A.chip_format), "%s", format);
    snprintf(A.chip_total, sizeof(A.chip_total), "%s", total_text);
    A.chip_hires = hires_chip;
    gui_album_view_set_chips(&A.view, format, hires_chip, total_text);
    return true;
}

void gui_album_header_build(lv_obj_t * list, const gui_album_header_params_t * p) {
    int count = p->entries ? p->count : 0;
    uint64_t key = gui_album_facts_seed(count);
    for (int i = 0; i < count; i++) key = gui_album_facts_add(key, p->entries[i].path);
    if (key != A.key || count != A.count || !A.paths) album_state_reset(key, p->entries, count);
    if (!A.have_record && count > 0 && A.paths) load_record();
    A.cover_song_id = 0; A.cover_key = 0;
    for (int i = 0; i < count; i++)
        if (p->entries[i].song_id > 0 && p->entries[i].artwork_key != 0) {
            A.cover_song_id = p->entries[i].song_id; A.cover_key = p->entries[i].artwork_key; break;
        }

    if (A.cover_song_id > 0) gui_album_cover_request(A.cover_song_id, BOARD_SCALE_PX(170));
    char meta[160];
    gui_album_view_meta_text(A.year, A.genre, meta, sizeof(meta));
    gui_album_view_params_t view = {
        .album = p->album, .artist = p->album_artist, .meta = meta,
        .hires_icon = count > 0 && p->quality_asset ? p->quality_asset(p->entries[0].path) : "",
        .show_buttons = p->show_buttons, .play_cb = p->play_cb, .shuffle_cb = p->shuffle_cb,
    };
    gui_album_view_build(&A.view, list, &view);
    A.view.num_w = gui_album_view_number_width(widest_track_number());
    A.chip_format[0] = A.chip_total[0] = 0;
    A.chip_hires = false;
    update_chips();
    A.dirty = false;
}

void gui_album_header_release_cover(void) { gui_album_cover_release(); }

void gui_album_header_forget(void) {
    A.view.root = A.view.info = A.view.chips = A.view.cover_img = NULL;
}

/* ---- Cover --------------------------------------------------------------- */
/* The sharp cover replaces the thumbnail once it is decoded. It is decoded at
 * exactly the cover's size, so it is drawn as it is (centred, no scaling). */
static bool show_sharp_cover(void) {
    const lv_image_dsc_t * dsc = A.view.cover_img && A.cover_song_id > 0 ? gui_album_cover_get(A.cover_song_id) : NULL;
    if (!dsc) return false;
    gui_album_view_show_cover(&A.view, dsc);
    return true;
}

void gui_album_header_refresh_cover(lv_obj_t * list) {
    if (!A.view.root || !A.view.cover_img || A.cover_song_id <= 0 || show_sharp_cover()) return;
    /* The shared decorator pads a row for a thumbnail; this header lays out
     * its own children, so undo that and let the image fill the cover box. */
    library_thumbnails_decorate_cover_image(list, A.view.root, A.view.cover_img, A.cover_song_id, A.cover_key);
    gui_album_view_fit_cover(&A.view, LV_IMAGE_ALIGN_STRETCH);
}

bool gui_album_header_detach_cover(const lv_image_dsc_t * dsc) {
    return gui_album_view_detach_cover(&A.view, dsc);
}

/* ---- Rows ---------------------------------------------------------------- */
const char * gui_album_header_row_title(const char * title, char * buf, size_t size) {
    const char * newline = title ? strchr(title, '\n') : NULL;
    size_t len = newline ? (size_t) (newline - title) : (title ? strlen(title) : 0);
    if (len >= size) len = size - 1;
    if (title) memcpy(buf, title, len);
    buf[len] = 0;
    return buf;
}

void gui_album_header_decorate_row(lv_obj_t * row, int index) {
    bool known = A.state && index >= 0 && index < A.count && A.state[index] == 1;
    gui_album_view_decorate_row(row, shown_track_number(index), A.view.num_w,
                                known ? (int) (A.dur[index].duration_ms / 1000.0 + 0.5) : -1);
}

void gui_album_header_row_set_duration(lv_obj_t * row, unsigned seconds) {
    gui_album_view_row_set_duration(row, seconds);
}

/* ---- Polling and shutdown ------------------------------------------------ */
bool gui_album_header_poll(bool visible, bool allowed) {
    if (gui_album_cover_poll(visible) && A.view.root) show_sharp_cover();
    album_probe_poll(allowed);
    if (!A.dirty || !A.view.root) return false;
    A.dirty = false;
    return update_chips();
}

bool gui_album_header_busy(void) { return album_probe_busy() || gui_album_cover_busy(); }

void gui_album_header_shutdown(int64_t deadline_ms) {
    gui_album_cover_shutdown();
    album_probe_shutdown(deadline_ms);
}
