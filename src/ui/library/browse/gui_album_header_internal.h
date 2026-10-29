#pragma once
/* Private to the album header: its state and the probe side's entry points. */
#include <lvgl/lvgl.h>
#include "gui_album_header.h"

/* Per-album UI state (UI thread only), shared by the widget layout
 * (gui_album_header.c) and the duration/probe side (gui_album_probe.c). */
typedef struct {
    uint64_t key;
    int count;
    char ** paths;      /* own copies: a probe batch outlives the entry array */
    uint8_t * state;    /* 0 not yet known, 1 known (cache or probe), 2 positively rejected (known, unplayable),
                         3 unknown for good this visit (transient error, too long a path, gave up) */
    duration_info_t * dur;
    int32_t * ident_mtime, * ident_size; /* from the cache lookup, to store probe results */
    uint8_t * identified;
    uint8_t * unplayable; /* the decoder rejected the file, or the cache says it did: cacheable as failed */
    int * track;
    bool have_record, dirty;
    int year;
    char genre[128], hires_icon[96];
    lv_obj_t * root, * info, * chips, * cover_img;
    /* What the chips currently show; they are rebuilt only when this changes. */
    char chip_format[24], chip_total[48];
    bool chip_hires;
    int cover_px, info_w;
    int64_t cover_song_id;
    uint64_t cover_key;
    uint32_t epoch; /* duration-cache storage epoch when this album was set up */
} album_t;
extern album_t A; /* defined in gui_album_probe.c */

/* gui_album_probe.c */
/* True when every track is known (playable) or positively rejected: only then
 * do the format and time chips show. A track in state 0 or 3 keeps them hidden. */
bool album_settled(void);
void album_state_reset(uint64_t key, const group_song_entry_t * entries, int count);
void album_state_free(void);
void album_probe_poll(bool allowed);
bool album_probe_busy(void); /* a lookup or probe worker is still running */
void album_probe_shutdown(int64_t deadline_ms);
