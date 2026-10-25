/* Raw pointer polling is intentional: LV_EVENT_PRESSING does not fire in
 * this LVGL build, so touch ownership cannot use that event. A dedicated
 * timer keeps A-Z dragging separate from the quick-drawer gestures. It is
 * resumed by a fresh press-down and paused when idle, just like the drawer. */
#include "library_az_index.h"

#include "assets.h"
#include "compact_list.h"
#include "fallback_font.h"
#include "screen_builders.h"

#define AZ_INDEX_BINDING_COUNT 4
#define AZ_INDEX_HIDE_DELAY_MS 900

typedef struct {
    lv_obj_t * screen;
    lv_obj_t * list;
    lv_obj_t * strip;
    lv_obj_t * popup;
    lv_obj_t * popup_label;
    metadata_db_az_kind_t db_kind;
} az_index_binding_t;

static az_index_binding_t az_index_bindings[AZ_INDEX_BINDING_COUNT];
static int az_index_registered_count;
static lv_timer_t * az_index_visibility_timer;
static az_index_binding_t * az_index_visibility_binding;
static bool az_index_dragging;
/* Whether this press's origin has already been judged against the strip --
 * see the poll callback's own comment on deciding ownership once. */
static bool az_index_press_judged;
static lv_timer_t * az_index_drag_timer;
static az_index_binding_t * az_index_active_binding;
static int az_index_jump_table[27];
static library_az_index_pointer_indev_getter_t pointer_indev_getter;
static library_az_index_strip_allowed_t strip_allowed;

static az_index_binding_t * find_az_binding_for_screen(lv_obj_t * screen) {
    for (int i = 0; i < az_index_registered_count; i++) {
        if (az_index_bindings[i].screen == screen) return &az_index_bindings[i];
    }
    return NULL;
}

static void az_index_visibility_timeout_cb(lv_timer_t * timer) {
    (void) timer;
    if (az_index_dragging) {
        lv_timer_reset(az_index_visibility_timer);
        return;
    }
    if (az_index_visibility_binding && az_index_visibility_binding->strip)
        lv_obj_add_flag(az_index_visibility_binding->strip, LV_OBJ_FLAG_HIDDEN);
    az_index_visibility_binding = NULL;
    lv_timer_pause(az_index_visibility_timer);
}

static void az_index_scroll_visibility_cb(lv_event_t * e) {
    lv_obj_t * list = lv_event_get_target(e);
    az_index_binding_t * binding = NULL;
    for (int i = 0; i < az_index_registered_count; ++i) {
        if (az_index_bindings[i].list == list) { binding = &az_index_bindings[i]; break; }
    }
    if (!binding) return;
    if (strip_allowed && !strip_allowed(binding->list)) {
        lv_obj_add_flag(binding->strip, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (az_index_visibility_binding && az_index_visibility_binding != binding)
        lv_obj_add_flag(az_index_visibility_binding->strip, LV_OBJ_FLAG_HIDDEN);
    az_index_visibility_binding = binding;
    lv_obj_remove_flag(binding->strip, LV_OBJ_FLAG_HIDDEN);
    if (!az_index_visibility_timer) {
        az_index_visibility_timer = lv_timer_create(az_index_visibility_timeout_cb,
                                                     AZ_INDEX_HIDE_DELAY_MS, NULL);
    }
    lv_timer_set_period(az_index_visibility_timer, AZ_INDEX_HIDE_DELAY_MS);
    lv_timer_reset(az_index_visibility_timer);
    lv_timer_resume(az_index_visibility_timer);
}

void library_az_index_reset_bindings(void) {
    if (az_index_visibility_timer) {
        lv_timer_delete(az_index_visibility_timer);
        az_index_visibility_timer = NULL;
    }
    az_index_visibility_binding = NULL;
    az_index_registered_count = 0;
}

void library_az_index_register(lv_obj_t * screen, lv_obj_t * list,
                               metadata_db_az_kind_t db_kind) {
    /* Not touch_list/a_z.png -- confirmed via pixel sampling that stock
     * asset is a fully opaque solid-black rectangle with the letters
     * painted in (alpha=255 everywhere, RGB (0,0,0) in the "empty" areas),
     * not a real transparent PNG -- invisible only against HiBy's own
     * pure-black screens, never designed to overlap anything lighter. It
     * visibly blacks out the right edge of these lists' (28,28,30) row
     * backgrounds, which a widget-level bg_opa/border style can't fix
     * since that opaque black is baked into the image's own pixels, not
     * the object's separate background layer. A plain label (genuinely
     * transparent -- only its glyph pixels paint anything) sidesteps this
     * entirely and needs no chroma-key support this LVGL build doesn't have. */
    lv_obj_t * strip = lv_label_create(screen);
    lv_label_set_text(strip, "A\nB\nC\nD\nE\nF\nG\nH\nI\nJ\nK\nL\nM\nN\nO\nP\nQ\nR\nS\nT\nU\nV\nW\nX\nY\nZ\n#");
    int32_t top = STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT;
    int32_t display_h = lv_display_get_vertical_resolution(lv_display_get_default());
    int32_t available_h = display_h - top - HOME_INDICATOR_CONTENT_INSET;
#if defined(BOARD_R3II_2025)
    const lv_font_t * strip_font = &lv_font_montserrat_14;
#else
    const lv_font_t * strip_font = &lv_font_montserrat_20;
#endif
    int32_t line_h = lv_font_get_line_height(strip_font);
    lv_obj_set_style_text_font(strip, strip_font, 0);
    lv_obj_set_width(strip, BOARD_SCALE_PX(30));
    lv_obj_set_style_pad_right(strip, BOARD_SCALE_PX(4), 0);
    lv_obj_add_style(strip, &style_theme_text_primary, 0);
    lv_obj_set_style_text_align(strip, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_bg_opa(strip, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(strip, 0, 0);
    lv_obj_remove_flag(strip, LV_OBJ_FLAG_SCROLLABLE);
    /* Stretched with extra line spacing to span close to the full list
     * height (27 lines * app_font_16's own line height) rather than
     * sitting bunched up near the top. */
    int32_t line_space = (available_h - line_h * 27) / 26;
    /* C truncates negative division toward zero; round down so compressed
     * columns don't leave their last letter below the available area. */
    if (line_h * 27 + line_space * 26 > available_h) line_space--;
    /* Compress spacing on shorter panels while keeping board-sized glyphs.
     * Touches map to the column's final measured bounds. */
    if (line_space < -6) line_space = -6;
    if (line_space > 3) line_space = 3;
    lv_obj_set_style_text_line_space(strip, line_space, 0);
    /* Top edge (the "A") lines up with the list's own top edge; the
     * stretched height above lands the bottom ("#") close to the screen's
     * bottom corner, matching the list's own bottom edge -- the list
     * itself starts at exactly STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT and
     * keeps HOME_INDICATOR_CONTENT_INSET clear at the bottom, matching its
     * compact-list siblings. */
    lv_obj_align(strip, LV_ALIGN_TOP_RIGHT, 0, top);
    lv_obj_add_flag(strip, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t * popup = lv_image_create(screen);
    lv_image_set_src(popup, asset_path("touch_list/a_z_result_bg.png"));
    lv_obj_center(popup);
    lv_obj_add_flag(popup, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t * popup_label = lv_label_create(popup);
    lv_obj_set_style_text_font(popup_label, &app_font_28, 0);
    lv_obj_add_style(popup_label, &style_theme_text_primary, 0);
    lv_obj_center(popup_label);

    az_index_bindings[az_index_registered_count++] =
        (az_index_binding_t){ screen, list, strip, popup, popup_label, db_kind };
    lv_obj_add_event_cb(list, az_index_scroll_visibility_cb, LV_EVENT_SCROLL_BEGIN, NULL);
    lv_obj_add_event_cb(list, az_index_scroll_visibility_cb, LV_EVENT_SCROLL, NULL);
    lv_obj_add_event_cb(list, az_index_scroll_visibility_cb, LV_EVENT_SCROLL_END, NULL);
}

lv_obj_t * library_az_index_strip_for_screen(lv_obj_t * screen) {
    az_index_binding_t * binding = find_az_binding_for_screen(screen);
    return binding ? binding->strip : NULL;
}

bool library_az_index_overlay_visible(void) {
    return az_index_dragging ||
           (az_index_active_binding && az_index_active_binding->popup &&
            !lv_obj_has_flag(az_index_active_binding->popup, LV_OBJ_FLAG_HIDDEN));
}

void library_az_index_poll(lv_timer_t * timer) {
    lv_indev_t * indev = pointer_indev_getter ? pointer_indev_getter() : NULL;
    if (!indev) return;

    bool pressed = lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED;
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    if (!az_index_dragging) {
        /* Only paused here (truly idle -- resumed on the next press-down by
         * library_az_index_resume_timer()) and in the just-released branch
         * below. */
        if (!pressed) {
            az_index_press_judged = false;
            lv_timer_pause(timer);
            return;
        }
        /* Ownership is decided ONCE, at press-down: the strip only takes a
         * drag that actually started on it.
         *
         * This used to re-test the live touch point on every tick, on
         * purpose, so that a press begun before the strip appeared could
         * still slide into it. The cost of that was GitHub #84: an ordinary
         * upward list swipe that drifts rightward lands in the strip near
         * the end of its travel, silently changes owner, and jumps the list
         * to a letter. Deciding at press-down is the same latching
         * drag_adjust_press_owned (gui_shell.c) already uses for sliders,
         * and it is what the report itself asked for. The press-begun-
         * before-the-strip-appeared case is given up deliberately: the
         * strip is not on screen to aim at then anyway, so nothing is lost
         * that the user could have been aiming for. */
        if (az_index_press_judged) return;
        az_index_press_judged = true;

        az_index_binding_t * b = find_az_binding_for_screen(lv_screen_active());
        if (!b) return;
        if (lv_obj_has_flag(b->strip, LV_OBJ_FLAG_HIDDEN)) return;

        lv_area_t area;
        lv_obj_get_coords(b->strip, &area);
        if (p.x < area.x1 || p.x > area.x2 || p.y < area.y1 || p.y > area.y2) return;

        az_index_dragging = true;
        az_index_active_binding = b;
        /* One entry per letter A-Z plus '#' (index 26) for anything not
         * starting with a letter -- the display offset of the first
         * matching entry, or the nearest one after it if that exact letter
         * has no entries ("jump forward to nearest match"). Computed fresh
         * on every touch-down from the already-sorted in-memory index
         * (metadata_db_get_az_table()). */
        metadata_db_get_az_table(b->db_kind, az_index_jump_table);
        lv_obj_remove_flag(b->popup, LV_OBJ_FLAG_HIDDEN);
    } else if (!pressed) {
        lv_obj_add_flag(az_index_active_binding->popup, LV_OBJ_FLAG_HIDDEN);
        az_index_dragging = false;
        az_index_active_binding = NULL;
        az_index_press_judged = false;
        lv_timer_pause(timer);
        return;
    }

    az_index_binding_t * b = az_index_active_binding;
    lv_area_t area;
    lv_obj_get_coords(b->strip, &area);
    int32_t rel_y = p.y - area.y1;
    int32_t h = area.y2 - area.y1;
    if (rel_y < 0) rel_y = 0;
    if (rel_y >= h) rel_y = h - 1;
    int letter_idx = (int) ((int64_t) rel_y * 27 / (h > 0 ? h : 1));
    if (letter_idx > 26) letter_idx = 26;

    int target = az_index_jump_table[letter_idx];
    if (target >= 0 && b->db_kind == METADATA_DB_AZ_ALL_SONGS) target++; /* All Songs' Play All row */
    if (target >= 0) compact_list_scroll_to_index(b->list, target);

    char letter = (letter_idx < 26) ? (char) ('A' + letter_idx) : '#';
    lv_label_set_text_fmt(b->popup_label, "%c", letter);
}

void library_az_index_init(library_az_index_pointer_indev_getter_t get_pointer_indev,
                           library_az_index_strip_allowed_t allow_strip) {
    pointer_indev_getter = get_pointer_indev;
    strip_allowed = allow_strip;
    if (!az_index_drag_timer)
        az_index_drag_timer = lv_timer_create(library_az_index_poll,
                                               LV_DEF_REFR_PERIOD, NULL);
}

void library_az_index_resume_timer(void) {
    if (az_index_drag_timer && lv_timer_get_paused(az_index_drag_timer)) {
        lv_timer_resume(az_index_drag_timer);
        lv_timer_ready(az_index_drag_timer);
    }
}

void library_az_index_reset_drag(void) {
    if (az_index_dragging && az_index_active_binding) {
        lv_obj_add_flag(az_index_active_binding->popup, LV_OBJ_FLAG_HIDDEN);
        az_index_active_binding = NULL;
    }
    az_index_dragging = false;
    /* Safe to clear even mid-press: this pauses the timer below, and it is
     * only ever resumed on a raw press-DOWN edge (gui_shell.c's indev
     * hooks), so the remainder of an in-flight press cannot be re-judged at
     * a drifted position. Leaving it set would instead disable the strip
     * for the whole of the NEXT press. */
    az_index_press_judged = false;
    if (az_index_drag_timer) {
        lv_timer_pause(az_index_drag_timer);
    }
}
