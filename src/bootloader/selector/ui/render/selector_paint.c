#include "../controller/selector_internal.h"

#include "../../assets/icons/selector_icons.h"
#include "../../assets/logos/selector_logos.h"
#include "selector_style.h"
#include "selector_ui.h"

#include <stdio.h>
#include <string.h>

static const char * const selector_detail[] = {
    "Compás player", "Rockbox player", "ADB, scripts and system options"
};
static const char * const tool_titles[] = {
    "ADB", "Run script", "Remount SD", "Repair SD card",
    "Restart", "Shutdown", "Recovery"
};
static const selector_icon_id_t tool_icons[] = {
    SELECTOR_ICON_ADB, SELECTOR_ICON_SCRIPT, SELECTOR_ICON_MOUNT,
    SELECTOR_ICON_REPAIR, SELECTOR_ICON_REBOOT, SELECTOR_ICON_SHUTDOWN,
    SELECTOR_ICON_RECOVERY
};

static uint32_t mix_rgb(uint32_t a, uint32_t b, unsigned amount_a) {
    unsigned ar = (a >> 16) & 255, ag = (a >> 8) & 255, ab = a & 255;
    unsigned br = (b >> 16) & 255, bg = (b >> 8) & 255, bb = b & 255;
    unsigned inv = 100 - amount_a;
    return ((ar * amount_a + br * inv) / 100) << 16 |
           ((ag * amount_a + bg * inv) / 100) << 8 |
           ((ab * amount_a + bb * inv) / 100);
}

static void draw_card(int x, int y, int w, int h, bool selected) {
    int radius = BREEZE_CORNER_RADIUS;
    if (selected) {
        uint32_t fill = mix_rgb(BREEZE_SELECTION_NORMAL, BREEZE_WINDOW_NORMAL, 32);
        sel_rounded_fill(x, y, w, h, radius, BREEZE_DECORATION_FOCUS);
        sel_rounded_fill(x + BREEZE_FOCUS_WIDTH, y + BREEZE_FOCUS_WIDTH,
                     w - 2 * BREEZE_FOCUS_WIDTH, h - 2 * BREEZE_FOCUS_WIDTH,
                     radius - BREEZE_FOCUS_WIDTH, fill);
    } else {
        sel_rounded_fill(x, y, w, h, radius, BREEZE_CARD_BORDER);
        sel_rounded_fill(x + 1, y + 1, w - 2, h - 2, radius - 1, BREEZE_WINDOW_ALTERNATE);
    }
}

static void blit_icon_id(selector_icon_id_t id, int x, int y, uint32_t color) {
    const selector_logo_a8_t * icon = selector_icon(id);
    if (icon && icon->pixels)
        sel_blit_a8(x, y, icon->pixels, icon->width, icon->height, color);
}

static int icon_h(selector_icon_id_t id) {
    const selector_logo_a8_t * icon = selector_icon(id);
    return icon && icon->pixels ? icon->height : 0;
}

static int icon_w(selector_icon_id_t id) {
    const selector_logo_a8_t * icon = selector_icon(id);
    return icon && icon->pixels ? icon->width : 0;
}

static void blit_icon_row(selector_icon_id_t id, int x, int y, int row_h, uint32_t color) {
    int h = icon_h(id);
    if (h <= 0) return;
    blit_icon_id(id, x, y + (row_h - h) / 2, color);
}

static bool bounded_text(char out[128], const char * input) {
    if (!input) input = "";
    size_t n = 0;
    while (n < 124 && input[n]) { out[n] = input[n]; n++; }
    bool truncated = input[n] != '\0';
    if (truncated) { out[n++] = '.'; out[n++] = '.'; out[n++] = '.'; }
    out[n] = '\0';
    return truncated;
}

static void draw_switch(int x, int y, int w, int h, bool on) {
    sel_rounded_fill(x, y, w, h, h / 2, on ? BREEZE_SELECTION_NORMAL : BREEZE_BUTTON_INACTIVE);
    int knob = h - 8;
    int kx = on ? x + w - knob - 4 : x + 4;
    sel_rounded_fill(kx, y + 4, knob, knob, knob / 2, BREEZE_WINDOW_FOREGROUND);
}

static void draw_wrapped_centered(int center_x, int top, const char * text, int px,
                                  uint32_t color, int max_width) {
    char safe[128];
    bounded_text(safe, text ? text : "");
    if (sel_text_width(safe, px) <= max_width) {
        sel_draw_text_centered(center_x, top, safe, px, color);
        return;
    }
    size_t n = strlen(safe);
    size_t split = 0;
    for (size_t i = 0; i < n; i++) {
        if (safe[i] != ' ') continue;
        safe[i] = '\0';
        if (sel_text_width(safe, px) <= max_width) split = i;
        safe[i] = ' ';
    }
    if (!split) {
        if (px > 14) {
            draw_wrapped_centered(center_x, top, text, 14, color, max_width);
            return;
        }
        sel_fit_text(safe, text ? text : "", max_width, px, false);
        sel_draw_text_centered(center_x, top, safe, px, color);
        return;
    }
    safe[split] = '\0';
    const char * line2 = safe + split + 1;
    char fitted2[128];
    sel_fit_text(fitted2, line2, max_width, px, false);
    sel_draw_text_centered(center_x, top, safe, px, color);
    sel_draw_text_centered(center_x, top + px + 6, fitted2, px, color);
}

static void draw_back_header(const char * title, bool selected) {
    int m = selector_margin(sel_canvas_width);
    int header_y = selector_header_title_y(sel_canvas_width);
    int back_size = selector_back_icon(sel_canvas_width);
    int title_px = selector_header_title_px(sel_canvas_width);
    char safe[128];
    bounded_text(safe, title ? title : "Tools");
    int ih = icon_h(SELECTOR_ICON_BACK);
    if (ih <= 0) ih = back_size;
    int row_h = ih > title_px ? ih : title_px;
    if (selected) {
        int card_h = selector_is_compact(sel_canvas_width) ? 48 : 56;
        int card_w = back_size + BREEZE_LARGE_SPACING + sel_text_width_medium(safe, title_px) + 20;
        draw_card(m, header_y - (card_h - back_size) / 2, card_w, card_h, true);
    }
    uint32_t color = selected ? BREEZE_SELECTION_NORMAL : BREEZE_WINDOW_FOREGROUND;
    if (icon_h(SELECTOR_ICON_BACK) > 0)
        blit_icon_row(SELECTOR_ICON_BACK, m, header_y, row_h, color);
    else {
        sel_line(m + back_size - 10, header_y - 2, m + back_size - 20,
             header_y + back_size / 2 - 2, color, 3);
        sel_line(m + back_size - 20, header_y + back_size / 2 - 2,
             m + back_size - 10, header_y + back_size - 2, color, 3);
    }
    sel_draw_text_medium(m + back_size + BREEZE_LARGE_SPACING,
                     header_y + (row_h - title_px) / 2, safe, title_px, color);
}

void selector_paint_selector(int selected, bool rockbox_available,
                             unsigned countdown_seconds, bool countdown_cancelled) {
    if (!sel_canvas) return;
    int row_count = rockbox_available ? 3 : 2;
    if (selected < 0 || selected >= row_count) selected = 0;
    sel_fill_rect(0, 0, sel_canvas_width, sel_canvas_height, BREEZE_WINDOW_NORMAL);
    int m = selector_margin(sel_canvas_width);
    int compact = selector_is_compact(sel_canvas_width);
    sel_draw_text_medium(m, selector_header_title_y(sel_canvas_width), "Choose a player",
                     selector_header_title_px(sel_canvas_width), BREEZE_WINDOW_FOREGROUND);
    sel_draw_text(m, selector_header_sub_y(sel_canvas_width), "Select what to start", 16,
              BREEZE_WINDOW_INACTIVE);

    int list_top = selector_list_top(sel_canvas_width);
    int card_h = selector_card_h(sel_canvas_width);
    int gap = selector_gap(sel_canvas_width);
    int inset = selector_logo_inset(sel_canvas_width);
    int x = m, w = sel_canvas_width - 2 * m;
    for (int row = 0; row < row_count; row++) {
        int i = rockbox_available || row == 0 ? row : 2;
        int y = list_top + row * (card_h + gap);
        bool is_selected = row == selected;
        draw_card(x, y, w, card_h, is_selected);
        uint32_t icon_color = is_selected ? BREEZE_SELECTION_NORMAL : BREEZE_WINDOW_FOREGROUND;
        int inner = x + inset;
        int drew_logo = 0;
        if (i == 0) {
            const selector_logo_a8_t * mark = selector_logo_wordmark();
            if (mark && mark->pixels && mark->width > 0 && mark->height > 0) {
                sel_blit_a8(inner, y + (card_h - mark->height) / 2,
                        mark->pixels, mark->width, mark->height, BREEZE_WINDOW_FOREGROUND);
                drew_logo = 1;
            }
        } else if (i == 1) {
            const selector_logo_rgba_t * mark = selector_logo_rockbox();
            if (mark && mark->pixels && mark->width > 0 && mark->height > 0) {
                sel_blit_rgba(inner, y + (card_h - mark->height) / 2,
                          mark->pixels, mark->width, mark->height);
                drew_logo = 1;
            }
        } else {
            int tools_icon = selector_row_icon(sel_canvas_width);
            if (icon_h(SELECTOR_ICON_UTILITIES) > 0)
                blit_icon_row(SELECTOR_ICON_UTILITIES, inner, y, card_h, icon_color);
            else
                sel_draw_tools_icon(inner, y + (card_h - tools_icon) / 2, tools_icon, icon_color);
            int nxt_w = icon_w(SELECTOR_ICON_NEXT);
            int nxt_x = x + w - inset - (nxt_w > 0 ? nxt_w : selector_next_icon());
            blit_icon_row(SELECTOR_ICON_NEXT, nxt_x, y, card_h, icon_color);
            int title_px = selector_row_title_px(sel_canvas_width);
            int sub_px = selector_row_sub_px(sel_canvas_width);
            int tx = inner + tools_icon + BREEZE_LARGE_SPACING * 2;
            int group_h = title_px + 4 + sub_px;
            int ty = y + (card_h - group_h) / 2;
            int max_w = nxt_x - BREEZE_LARGE_SPACING - tx;
            char sub[128];
            sel_fit_text(sub, selector_detail[2], max_w, sub_px, false);
            if (is_selected)
                sel_draw_text_medium(tx, ty, "Tools", title_px, BREEZE_WINDOW_FOREGROUND);
            else
                sel_draw_text(tx, ty, "Tools", title_px, BREEZE_WINDOW_FOREGROUND);
            sel_draw_text(tx, ty + title_px + 4, sub, sub_px, BREEZE_WINDOW_INACTIVE);
            drew_logo = 1;
        }
        if (!drew_logo) {
            int icon_size = compact ? 32 : 40;
            int icon_x = x + (compact ? 12 : 18);
            int icon_y = y + (card_h - icon_size) / 2;
            if (i == 0) sel_draw_music_note(icon_x, icon_y, icon_size, icon_color);
            else sel_draw_mountain(icon_x, icon_y, icon_size, icon_color);
            int tx = icon_x + icon_size + (compact ? 11 : 16);
            sel_draw_text(tx, y + (card_h - 45) / 2, i == 0 ? "Compás" : "Rockbox",
                      22, BREEZE_WINDOW_FOREGROUND);
            sel_draw_text(tx, y + (card_h - 45) / 2 + 27, selector_detail[i], 16,
                      BREEZE_WINDOW_INACTIVE);
        }
    }

    if (!countdown_cancelled && selected == 0) {
        char label[48];
        snprintf(label, sizeof(label), "Auto-start in %u sec", countdown_seconds);
        sel_draw_text_centered(sel_canvas_width / 2, selector_footer_y(sel_canvas_height),
                           label, 14, BREEZE_WINDOW_INACTIVE);
    }
}

void selector_paint_starting(bool rockbox) {
    if (!sel_canvas) return;
    sel_fill_rect(0, 0, sel_canvas_width, sel_canvas_height, BREEZE_WINDOW_NORMAL);
    int compact = selector_is_compact(sel_canvas_width);
    int icon_size = compact ? 48 : 56;
    int font_px = compact ? 20 : 24;
    const char * label = rockbox ? "Starting Rockbox…" : "Starting Compás…";
    int group_height = icon_size + 26 + font_px + 8;
    int top = (sel_canvas_height - group_height) / 2;
    int icon_x = (sel_canvas_width - icon_size) / 2;
    if (rockbox) sel_draw_mountain(icon_x, top, icon_size, BREEZE_SELECTION_NORMAL);
    else sel_draw_music_note(icon_x, top, icon_size, BREEZE_SELECTION_NORMAL);
    sel_draw_text_centered(sel_canvas_width / 2, top + icon_size + 26, label, font_px,
                       BREEZE_WINDOW_FOREGROUND);
}

void selector_paint_tools(int selected, int scroll_pixels, bool adb_enabled) {
    if (!sel_canvas) return;
    if (selected != SELECTOR_UI_HIT_BACK && (selected < 0 || selected > 6)) selected = 0;
    sel_fill_rect(0, 0, sel_canvas_width, sel_canvas_height, BREEZE_WINDOW_NORMAL);
    draw_back_header("Tools", selected == SELECTOR_UI_HIT_BACK);

    int m = selector_margin(sel_canvas_width);
    int list_top = selector_tool_list_top(sel_canvas_width);
    int row_h = selector_tool_row_h(sel_canvas_width);
    int gap = selector_tool_gap(sel_canvas_width);
    int inset = selector_logo_inset(sel_canvas_width);
    int x = m, w = sel_canvas_width - 2 * m;
    int previous_clip_top = sel_clip_top, previous_clip_bottom = sel_clip_bottom;
    sel_clip_top = list_top;
    sel_clip_bottom = sel_canvas_height;
    int title_px = selector_row_title_px(sel_canvas_width);
    for (int i = 0; i < 7; i++) {
        int y = list_top + i * (row_h + gap) - scroll_pixels;
        if (y + row_h <= list_top || y >= sel_canvas_height) continue;
        bool is_selected = i == selected;
        draw_card(x, y, w, row_h, is_selected);
        uint32_t col = is_selected ? BREEZE_SELECTION_NORMAL : BREEZE_WINDOW_FOREGROUND;
        int icon_x = x + inset;
        if (icon_h(tool_icons[i]) > 0)
            blit_icon_row(tool_icons[i], icon_x, y, row_h, col);
        int tw_y = y + (row_h - title_px) / 2;
        if (is_selected)
            sel_draw_text_medium(icon_x + 32 + BREEZE_LARGE_SPACING, tw_y, tool_titles[i],
                             title_px, BREEZE_WINDOW_FOREGROUND);
        else
            sel_draw_text(icon_x + 32 + BREEZE_LARGE_SPACING, tw_y, tool_titles[i],
                      title_px, BREEZE_WINDOW_FOREGROUND);
        if (i == 0) {
            int sw = selector_switch_w(sel_canvas_width), sh = selector_switch_h(sel_canvas_width);
            draw_switch(x + w - inset - sw, y + (row_h - sh) / 2, sw, sh, adb_enabled);
        } else if (is_selected) {
            int nxt_w = icon_w(SELECTOR_ICON_NEXT);
            blit_icon_row(SELECTOR_ICON_NEXT, x + w - inset - (nxt_w > 0 ? nxt_w : 22),
                          y, row_h, BREEZE_SELECTION_NORMAL);
        }
    }
    sel_clip_top = previous_clip_top;
    sel_clip_bottom = previous_clip_bottom;
}

void selector_paint_list(const char * title, const char * const * items,
                         size_t count, int selected, int scroll_pixels,
                         const char * notice) {
    if (!sel_canvas) return;
    if (count > SELECTOR_UI_MAX_LIST_ITEMS) count = SELECTOR_UI_MAX_LIST_ITEMS;
    if (selected != SELECTOR_UI_HIT_BACK) {
        if (count == 0) selected = -1;
        else if (selected < 0 || (size_t)selected >= count) selected = 0;
    }
    sel_fill_rect(0, 0, sel_canvas_width, sel_canvas_height, BREEZE_WINDOW_NORMAL);
    draw_back_header(title ? title : "Select a file", selected == SELECTOR_UI_HIT_BACK);
    int list_top = selector_tool_list_top(sel_canvas_width);
    int row_h = selector_tool_row_h(sel_canvas_width);
    int gap = selector_tool_gap(sel_canvas_width);
    int m = selector_margin(sel_canvas_width), w = sel_canvas_width - 2 * m;
    int bottom = sel_list_viewport_bottom();
    int previous_clip_top = sel_clip_top, previous_clip_bottom = sel_clip_bottom;
    sel_clip_top = list_top;
    sel_clip_bottom = bottom;
    int item_px = selector_row_title_px(sel_canvas_width);
    int inset = selector_logo_inset(sel_canvas_width);
    for (size_t i = 0; i < count; i++) {
        int y = list_top + (int)i * (row_h + gap) - scroll_pixels;
        if (y + row_h <= list_top || y >= bottom) continue;
        bool is_selected = (int)i == selected;
        draw_card(m, y, w, row_h, is_selected);
        char safe_item[128];
        int item_x = m + inset;
        int nxt_w = icon_w(SELECTOR_ICON_NEXT);
        int max_text_width = w - inset - (nxt_w > 0 ? nxt_w : 22) - BREEZE_LARGE_SPACING - inset;
        sel_fit_text(safe_item, items ? items[i] : "", max_text_width, item_px, is_selected);
        int tw_y = y + (row_h - item_px) / 2;
        if (is_selected)
            sel_draw_text_medium(item_x, tw_y, safe_item, item_px, BREEZE_WINDOW_FOREGROUND);
        else
            sel_draw_text(item_x, tw_y, safe_item, item_px, BREEZE_WINDOW_FOREGROUND);
        if (is_selected)
            blit_icon_row(SELECTOR_ICON_NEXT, m + w - inset - (nxt_w > 0 ? nxt_w : 22),
                          y, row_h, BREEZE_SELECTION_NORMAL);
    }
    sel_clip_top = previous_clip_top;
    sel_clip_bottom = previous_clip_bottom;
    if (notice && *notice) {
        int max_w = sel_canvas_width - 2 * m;
        char safe_notice[128];
        sel_fit_text(safe_notice, notice, max_w, 14, false);
        sel_draw_text_centered(sel_canvas_width / 2, sel_canvas_height - 30, safe_notice, 14,
                           BREEZE_WINDOW_INACTIVE);
    }
}

void selector_paint_notice(const char * title, const char * message) {
    if (!sel_canvas) return;
    sel_fill_rect(0, 0, sel_canvas_width, sel_canvas_height, BREEZE_WINDOW_NORMAL);
    draw_back_header(title ? title : "Notice", false);
    int m = selector_margin(sel_canvas_width);
    int max_w = sel_canvas_width - 2 * m;
    int px = 18;
    draw_wrapped_centered(sel_canvas_width / 2, sel_canvas_height / 2 - 14,
                          message ? message : "", px, BREEZE_WINDOW_FOREGROUND, max_w);
}

void selector_paint_progress(const char * title, const char * message, int percent) {
    if (!sel_canvas) return;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    sel_fill_rect(0, 0, sel_canvas_width, sel_canvas_height, BREEZE_WINDOW_NORMAL);
    draw_back_header(title ? title : "Working", false);
    int m = selector_margin(sel_canvas_width);
    int max_w = sel_canvas_width - 2 * m;
    draw_wrapped_centered(sel_canvas_width / 2, sel_canvas_height / 2 - 54,
                          message ? message : "", 18, BREEZE_WINDOW_FOREGROUND, max_w);
    int bar_w = sel_canvas_width - 2 * m, bar_h = 20;
    int bar_x = m, bar_y = sel_canvas_height / 2 - bar_h / 2;
    sel_rounded_fill(bar_x, bar_y, bar_w, bar_h, bar_h / 2, BREEZE_CARD_BORDER);
    if (percent > 0) {
        int fill_w = (bar_w - 4) * percent / 100;
        if (fill_w < bar_h - 4) fill_w = bar_h - 4;
        sel_rounded_fill(bar_x + 2, bar_y + 2, fill_w, bar_h - 4,
                     (bar_h - 4) / 2, BREEZE_SELECTION_NORMAL);
    }
}
