#pragma once
/* Shared by the cover_flow*.c files only. */
#include "cover_flow.h"
#include "cover_flow_azmap.h"
#include "cover_flow_loader.h"
#include "cover_flow_span.h"
#include "cover_flow_tilt.h"
#include "cover_flow_warp.h"
#include "../../../core/system/runtime/board_config.h"
#include <lvgl/lvgl.h>

/* ---- Tunables ---- */
#define CF_FACE_PX BOARD_SCALE_PX(280)       /* side of the flat centre cover */
#define CF_FAR_WIDTH_PCT 22                  /* projected width of a fully tilted cover, % of the face */
#define CF_SIDE_HEIGHT_PCT 80                /* its outer (near) edge, % of the centre cover's height */
#define CF_FAR_HEIGHT_PCT 75                 /* its inner (far) edge, % of its outer edge */
#define CF_FAR_SHADE_PCT 35                  /* darkening of its inner edge, % */
#define CF_REFLECT_PCT 35                    /* reflection height, % of the cover's height */
#define CF_REFLECT_ALPHA_PCT 40              /* reflection opacity at the cover's bottom edge, % */
#define CF_GAP_PX BOARD_SCALE_PX(4)          /* gap between the centre cover and the first tilted one */
#define CF_PITCH_FAR BOARD_SCALE_PX(11)      /* spacing of the tilted covers beyond the first */
#define CF_STEP_PX ((CF_FACE_PX * 3) / 4)    /* finger travel per item */
#define CF_EDGE_PX BOARD_SCALE_PX(48)        /* left strip kept free for the shell's back swipe */
#define CF_WARP_PX_PER_FRAME 100000          /* pixels of new warps per frame, nearest the centre first */
#define CF_TRIM_MS 2000                      /* still this long: drop the in-between warps */
#define CF_COAST_MS 300                      /* how far a fling coasts, in ms of release velocity */
#define CF_PUMP_MS 30
#define CF_TEXT_MAX 160                      /* bytes kept of a title or subtitle */

#define CF_FAR_WIDTH_PX (CF_FACE_PX * CF_FAR_WIDTH_PCT / 100)
#define CF_OFF1 (CF_FACE_PX / 2 + CF_GAP_PX + CF_FAR_WIDTH_PX / 2) /* centre to the first tilted centre */

#define CF_SLOTS 11
#define CF_CENTRE 5
#define CF_ONE COVER_FLOW_ONE
#define CF_HALF_STEP (COVER_FLOW_TILT_STEPS / 2)

/* The image widget of one slot position. */
typedef struct {
    lv_obj_t * img;
    lv_image_dsc_t dsc; /* the visible columns of a warp, pointing into its buffer */
    const void * data;  /* what dsc points at now (NULL: set it again) */
    bool shown;
    int32_t x;
    int32_t clip;       /* first column in the full warp */
    int step, side;
} cf_view_t;

typedef struct {
    int32_t cx, cover_y, label_y, area_top, area_bottom, screen_w;
    int32_t strip_y, strip_h; /* the scrub track, when enabled */
} cf_geometry_t;

typedef struct {
    int32_t x;
    uint32_t t;
} cf_sample_t;

/* The album scrub track and its letter bubble. */
typedef struct {
    lv_obj_t * touch, * bubble, * bubble_label;
    lv_obj_t * track, * fill, * grip;
    lv_draw_buf_t * cap;
    lv_timer_t * poll;
    lv_indev_t * pointer;
    int32_t left, width, offset, start_x;
    bool moved;
    bool visible;        /* the track is faded in */
    bool tap_reveals;    /* the press started on a hidden track */
    char text[2];
    int shown;           /* letter in the bubble, -1 none */
    bool scrubbing;
} cf_az_t;

typedef struct {
    lv_obj_t * screen, * touch, * title_label, * sub_label, * header_title;
    lv_timer_t * pump;
    uint32_t quiet_since; /* tick of the last activity, for the idle trim */
    cf_geometry_t g;
    bool az_on;          /* the scrub track is part of this screen */
    cf_az_t az;
    void (*on_select)(int);
    void (*on_hold)(int);
    cover_flow_face_fn face; /* NULL: the library's covers */

    void * items_block;
    cover_flow_item_t * items;
    int count;

    cf_warps_t warps;    /* the tilt tables, the screen's solid colour baked into every bitmap */
    cover_flow_loader_t * loader; /* the face being decoded, if any */
    cf_content_t ph[2];   /* the placeholder every faceless slot shares: [0] flat and left of the centre, [1] right */
    cf_content_t * slot[CF_SLOTS];
    cf_view_t view[CF_SLOTS];

    int32_t pos;         /* scroll position in items, 16.16 */
    int base;            /* item nearest the centre */
    int label_item;
    bool relayout;       /* a pass had to skip work; run it again */
    int32_t warp_budget; /* pixels of new warps left in this frame */
    uint32_t budget_tick; /* when the budget was last refilled */
    bool animating;

    /* input */
    bool pressed, dragging, long_fired, was_animating;
    lv_point_t start;
    int32_t grab_x, grab_pos;
    cf_sample_t sample[4];
    int sample_n;
} cover_flow_t;

extern cover_flow_t * cf_instance;

/* The k-th slot counting outward from the centre: 0, +1, -1, +2, -2, ... */
static inline int cf_centre_out(int k) {
    return CF_CENTRE + ((k & 1) ? (k + 1) / 2 : -(k / 2));
}

/* cover_flow.c */
void cf_update_labels(void);

/* cover_flow_layout.c */
bool cf_slots_create(cover_flow_t * cf);
/* Frees the in-between warps once the covers rest: they show only the flat or the full tilt. */
void cf_trim_idle(cover_flow_t * cf);
void cf_slots_destroy(cover_flow_t * cf);
/* Rebinds every slot to the items around `base` and drops what they held. */
void cf_slots_reset(cover_flow_t * cf, int base);
void cf_layout_update(void);
/* Apply input/animation geometry synchronously, before display preparation. */
/* The slot whose cover is at (x, y), or -1. */
int cf_slot_at(int32_t x, int32_t y);

/* cover_flow_faces.c */
bool cf_faces_create(cover_flow_t * cf);
void cf_faces_destroy(cover_flow_t * cf);
/* Wakes the pump: a face may be missing or a pass is pending. */
void cf_pump_kick(void);
void cf_pump_cb(lv_timer_t * timer);

/* cover_flow_az.c */
/* Height the strip takes at the bottom of the screen. */
int32_t cf_az_height(void);
/* The track's centre, from the strip's top. */
int32_t cf_az_track_center(void);
/* Builds or removes the strip to match cf->az_on (geometry in cf->g is current). */
void cf_az_sync(cover_flow_t * cf);
/* The items changed: reset the scrub and move the handle. */
void cf_az_content(cover_flow_t * cf);
void cf_az_destroy(cover_flow_t * cf);
void cf_az_update(cover_flow_t * cf);
void cf_az_show(cover_flow_t * cf, bool show);
void cf_az_cancel(cover_flow_t * cf);

/* cover_flow_input.c */
void cf_input_attach(lv_obj_t * touch);
void cf_input_stop(void);
bool cf_settled(void);
/* Animates to an album with the covers' normal snap. */
void cf_scroll_to(int item);
