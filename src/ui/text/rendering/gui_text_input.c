#include "gui_text_input.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lvgl/lvgl.h"
#include "../../assets/assets.h"
#include "../../application/gui.h"
#include "../../widgets/builders/screen_builders.h"
#include "../../appearance/gui_theme.h"
#include "fallback_font.h"
#include <ctype.h>

extern lv_style_t style_theme_screen_bg;
extern lv_style_t style_theme_text_primary;
extern lv_style_t style_theme_text_secondary;
extern lv_style_t style_theme_row;
extern lv_style_t style_theme_text_muted;
extern lv_style_t style_theme_list_padding;
extern lv_style_t style_button_pressed;

extern void nav_remove_stack_slot(int depth);
extern void enable_gesture_bubble_recursive(lv_obj_t * parent);
extern void search_textarea_value_changed_cb(lv_event_t * e);
extern void search_textarea_clicked_cb(lv_event_t * e);
extern void search_keypad_dismissed(void);
extern void nav_push(lv_obj_t * screen);
extern void nav_pop(void);
extern void nav_reset_to_home(void);
extern void finalize_screen_navigation(lv_obj_t * screen);
extern void generic_back_cb(lv_event_t * e);
extern void plugin_manager_text_input_cancelled(void);

/* gui_font_role_t defined in gui_theme.h */

extern void plugin_text_entry_done_cb(const char * text, void * user_data);

static lv_obj_t * text_entry_screen;
static lv_obj_t * text_entry_title_label;
static lv_obj_t * text_entry_textarea;
static lv_obj_t * text_entry_keypad_group; /* wraps both keypad layouts (T9 pad + QWERTY keyboard) -- reparented onto whichever screen has inline search open, see t9_keypad_attach()/t9_keypad_release() */
static lv_obj_t * text_entry_t9_pad;      /* every T9 key lives under this; hidden while the QWERTY layout is in use */
static lv_obj_t * text_entry_qwerty_kb;   /* lv_keyboard, hidden unless Settings -> Keyboard is QWERTY and the field isn't numeric */
static lv_obj_t * text_entry_variant_overlay;
static lv_obj_t * text_entry_variant_panel;
static lv_obj_t * text_entry_variant_buttons[8];
static lv_obj_t * text_entry_variant_owner_screen;
static const char * text_entry_variant_choices[8];
static size_t text_entry_variant_choice_count;
static bool text_entry_variant_swallow_release;
/* Enter's meaning while the keypad is on loan for inline search: dismiss
 * just the keypad (search stays active) instead of text_entry_commit()'s
 * normal nav_pop()-and-fire-callback modal flow -- see
 * text_entry_enter_click_cb(). */
static bool text_entry_inline_mode_active = false;
static lv_obj_t * text_entry_reveal_btn;
static text_entry_done_cb_t text_entry_on_done;
static void * text_entry_user_data;

/* ---- T9 keypad geometry: 5 columns x 4 rows of TEXT_ENTRY_KEY_SIZE keys,
 * centered horizontally and anchored to the bottom of the screen.
 * Column 0 holds mode buttons (123/ABC/sym); columns 1-3 hold the 3x3
 * letter/digit/symbol pad; column 4 holds Del, Key 0/Shift, and Enter
 * (spanning rows 2-3). Row 3 holds Left, Right, and Space (cols 2-3). ---- */
#define TEXT_ENTRY_KEY_SIZE BOARD_SCALE_PX(90)
#define TEXT_ENTRY_KEY_GAP BOARD_SCALE_PX(4)
#define TEXT_ENTRY_GRID_COLS 5
#define TEXT_ENTRY_GRID_ROWS 4
#define TEXT_ENTRY_GRID_WIDTH (TEXT_ENTRY_GRID_COLS * TEXT_ENTRY_KEY_SIZE + (TEXT_ENTRY_GRID_COLS - 1) * TEXT_ENTRY_KEY_GAP)
#define TEXT_ENTRY_GRID_HEIGHT (TEXT_ENTRY_GRID_ROWS * TEXT_ENTRY_KEY_SIZE + (TEXT_ENTRY_GRID_ROWS - 1) * TEXT_ENTRY_KEY_GAP)
#define TEXT_ENTRY_BOTTOM_MARGIN BOARD_SCALE_PX(16)
#define TEXT_ENTRY_GRID_X ((BOARD_SCREEN_WIDTH - TEXT_ENTRY_GRID_WIDTH) / 2)
#define TEXT_ENTRY_GRID_Y (BOARD_SCREEN_HEIGHT - TEXT_ENTRY_GRID_HEIGHT - TEXT_ENTRY_BOTTOM_MARGIN)
#define TEXT_ENTRY_MULTITAP_MS 900

typedef enum {
    TEXT_ENTRY_KP_ABC = 0,
    TEXT_ENTRY_KP_NUM,
    TEXT_ENTRY_KP_SYM,
} text_entry_kp_mode_t;

/* Key index 10 is the numeric-lock-only "./-" key (T9_LETTER_GROUPS/
 * T9_SYMBOL_GROUPS below only cover 0-9, the real digit keys). NULL entries
 * mean "no letter group on this asset set" (keys 0 and 1) -- those always
 * insert their own literal digit in ABC mode, same as NUM mode does for
 * every key. */
static const char * const TEXT_ENTRY_LETTER_GROUPS[10] = {
    NULL, NULL, "abc", "def", "ghi", "jkl", "mno", "pqrs", "tuv", "wxyz"
};
/* Read off symbolN.png's own rendered glyphs directly (contact-sheet
 * inspection, see this feature's own design notes) -- no declarative source
 * for these exists anywhere in the squashfs. */
static const char * const TEXT_ENTRY_SYMBOL_GROUPS[10] = {
    ".,", "!?", "()[]", "/\\|", "$%", "@#&", "+-*^", "_{}", ":;", "<=>"
};

static text_entry_kp_mode_t text_entry_kp_mode = TEXT_ENTRY_KP_ABC;
/* Shift state: one-shot Shift capitalizes only the next letter, while tapping
 * the ABC mode button again toggles persistent Caps Lock. text_entry_is_uppercase()
 * evaluates the union of both states. */
static bool text_entry_shift = false;     /* one-shot: consumed by the next letter (see text_entry_handle_digit_key) */
static bool text_entry_caps_lock = false; /* persistent: toggled by re-tapping the ABC mode button */
static bool text_entry_numeric_only = false;
static bool text_entry_qwerty_latin_mode = false;
static int text_entry_keyboard_layout = KEYBOARD_LAYOUT_QWERTY;

static bool text_entry_is_uppercase(void) {
    return text_entry_caps_lock || text_entry_shift;
}

/* Case for a pending multi-tap cycle is pinned to whatever it was on that
 * cycle's FIRST tap (text_entry_handle_digit_key), not re-read live on every
 * repeat tap -- otherwise a one-shot Shift (consumed immediately after that
 * first tap, see below) would make the letter flip back to lowercase the
 * moment the user cycles to a second candidate for the SAME letter, even
 * though they're still resolving that one same keypress. */
static bool text_entry_pending_shift = false;

/* Multi-tap state: which key (0-10, or -1 = none) is mid-cycle and which
 * character of its cycle is currently showing in the textarea. A repeat tap
 * on the SAME key within TEXT_ENTRY_MULTITAP_MS deletes and re-inserts the
 * next character in its cycle; any other input (a different key, or the
 * timer simply elapsing) finalizes it, so the next tap on that key starts a
 * fresh cycle instead of continuing the old one. */
static int text_entry_pending_key = -1;
static int text_entry_pending_tap_index = 0;
static lv_timer_t * text_entry_multitap_timer;

/* The 10 T9 pad digit-slot keys, indexed by digit 0-9 -- kept so
 * text_entry_refresh_keys() can update their images/visibility in place
 * when the mode/shift/numeric-lock state changes, without rebuilding the
 * screen. Index 0 is special: it shares its screen position with Shift
 * (see text_entry_key0_click_cb's own comment). The numeric-lock decimal
 * and minus keys and the three mode-jump buttons (123/ABC/sym) each need
 * their own visibility toggled independently of the digit-slot keys, so
 * they get their own named pointers rather than living in this array. */
static lv_obj_t * text_entry_key_img[10];
static lv_obj_t * text_entry_dotneg_key;
static lv_obj_t * text_entry_neg_key;
static lv_obj_t * text_entry_num_mode_key;
static lv_obj_t * text_entry_abc_mode_key;
static lv_obj_t * text_entry_sym_mode_key;

/* Generation counter incremented on every show_text_entry() call to distinguish
 * between callbacks that chain into another show_text_entry() call and those
 * that complete navigation back. */
static uint32_t text_entry_generation = 0;
static void text_entry_variant_close(void);
static void text_entry_variant_owner_event_cb(lv_event_t * e);

static void text_entry_reveal_btn_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    bool now_masked = !lv_textarea_get_password_mode(text_entry_textarea);
    lv_textarea_set_password_mode(text_entry_textarea, now_masked);
    lv_image_set_src(text_entry_reveal_btn, asset_path(now_masked ? "keyboard/psk_show.png" : "keyboard/psk_hide.png"));
}

/* Commits text input and invokes the completion callback. */
static void text_entry_commit(void) {
    text_entry_variant_close();
    /* lv_textarea_get_text()'s buffer belongs to the textarea and would be
     * invalidated/reused the moment another screen starts editing it, so
     * copy it out before invoking the caller's callback (which may itself
     * push a screen that reuses this same textarea, e.g. entering the next
     * field). */
    char text_copy[256];
    snprintf(text_copy, sizeof(text_copy), "%s", lv_textarea_get_text(text_entry_textarea));
    text_entry_done_cb_t cb = text_entry_on_done;
    void * user_data = text_entry_user_data;
    uint32_t generation_before = text_entry_generation;
    int depth_before = gui_navigation_get_depth();
    /* Invoke the callback before navigation checks so any chained navigation
     * can be detected. If the callback navigated forward to another screen,
     * remove this text entry slot from the stack so navigating back returns to
     * the preceding screen. If not chained, pop back normally. */
    if (cb) cb(text_copy, user_data);
    if (gui_navigation_get_depth() > depth_before) {
        nav_remove_stack_slot(depth_before - 1);
    } else if (text_entry_generation == generation_before) {
        nav_pop();
    }
    /* else (nav_depth == depth_before && generation changed): the callback
     * chained into another show_text_entry() call reusing this exact screen
     * object -- nav_push()'s own dedup guard already reloaded it in place,
     * nothing further to do here. */
}

/* Clears any in-progress multi-tap cycle without touching the textarea --
 * called at the top of every OTHER key's handler (anything that isn't a
 * repeat tap of the same cycling key) so the next tap on that key starts a
 * fresh cycle instead of continuing whatever was pending before. Also what
 * the timeout itself does when no further tap arrives in time. */
static void text_entry_finalize_pending(void) {
    text_entry_pending_key = -1;
    lv_timer_pause(text_entry_multitap_timer);
}

static void text_entry_multitap_timeout_cb(lv_timer_t * timer) {
    (void) timer;
    text_entry_finalize_pending();
}

/* Every character (or character group) a given digit-slot key currently
 * produces, as a NUL-terminated string -- length 1 means "just insert this
 * one character, no cycling" (NUM mode, or ABC mode's key 0/1, or a
 * genuinely single-symbol group); length > 1 is what the multi-tap cycle in
 * text_entry_key_click_cb() steps through. key_index 10 is the
 * numeric-lock-only decimal key, meaningful only when text_entry_numeric_only
 * is set (its caller already knows not to ask otherwise). out must be at
 * least 8 bytes -- the longest real group (TEXT_ENTRY_SYMBOL_GROUPS' 4-char
 * entries) plus the NUL. uppercase is passed in explicitly (rather than read
 * live off text_entry_is_uppercase()) so text_entry_handle_digit_key() can
 * pin a multi-tap cycle's case to whatever it was on that cycle's first tap
 * -- see text_entry_pending_shift's own comment. */
static void text_entry_key_chars_ex(int key_index, char * out, size_t out_size, bool uppercase) {
    if (key_index == 10) {
        snprintf(out, out_size, ".");
        return;
    }
    if (text_entry_numeric_only || text_entry_kp_mode == TEXT_ENTRY_KP_NUM) {
        out[0] = (char) ('0' + key_index);
        out[1] = '\0';
        return;
    }
    if (text_entry_kp_mode == TEXT_ENTRY_KP_SYM) {
        snprintf(out, out_size, "%s", TEXT_ENTRY_SYMBOL_GROUPS[key_index]);
        return;
    }
    /* ABC mode: Key 1 acts as a punctuation shortcut. */
    if (key_index == 1) {
        snprintf(out, out_size, "._@/#");
        return;
    }
    const char * letters = TEXT_ENTRY_LETTER_GROUPS[key_index];
    if (!letters) {
        out[0] = (char) ('0' + key_index);
        out[1] = '\0';
        return;
    }
    size_t n = strlen(letters);
    size_t i;
    for (i = 0; i < n && i + 1 < out_size; i++) {
        out[i] = uppercase ? (char) toupper((unsigned char) letters[i]) : letters[i];
    }
    out[i] = '\0';
}

/* asset_path() only needs relative_path for the duration of its own call
 * (it strdup()s its own "S:..." result -- see its doc comment), so handing
 * back a reused static buffer here is safe: every call site below both
 * builds and consumes the path synchronously, on the GUI thread, one at a
 * time. */
static const char * text_entry_key_image(int key_index) {
    static char buf[64];
    if (text_entry_numeric_only || text_entry_kp_mode == TEXT_ENTRY_KP_NUM) {
        snprintf(buf, sizeof(buf), "keyboard/%d.png", key_index);
    } else if (text_entry_kp_mode == TEXT_ENTRY_KP_SYM) {
        snprintf(buf, sizeof(buf), "keyboard/symbol%d.png", key_index);
    } else if (key_index == 1) {
        /* char_l.png/char_u.png are byte-identical (confirmed via md5sum),
         * so this always resolves to the same image regardless of case --
         * picking by case anyway just for symmetry with every other
         * letter-group key below, which does need it. */
        snprintf(buf, sizeof(buf), "keyboard/char_%c.png", text_entry_is_uppercase() ? 'u' : 'l');
    } else {
        const char * letters = TEXT_ENTRY_LETTER_GROUPS[key_index];
        if (!letters) {
            snprintf(buf, sizeof(buf), "keyboard/%d.png", key_index);
        } else {
            snprintf(buf, sizeof(buf), "keyboard/%s_%c.png", letters, text_entry_is_uppercase() ? 'u' : 'l');
        }
    }
    return buf;
}

static void text_entry_set_key_available(lv_obj_t * key, bool available) {
    lv_obj_set_flag(key, LV_OBJ_FLAG_HIDDEN, !available);
    lv_obj_set_flag(key, LV_OBJ_FLAG_CLICKABLE, available);
}

/* Re-syncs every key's image and (for the mode buttons/numeric decimal and
 * minus keys) visibility with the current mode/shift/numeric-lock state -- called
 * once from show_text_entry() and again on every mode/Shift/digit-0 tap. */
static void text_entry_refresh_keys(void) {
    for (int i = 1; i < 10; i++) {
        lv_image_set_src(text_entry_key_img[i], asset_path(text_entry_key_image(i)));
    }
    /* Index 0's slot shows Shift while cycling letters (case has no other
     * meaning), and the literal digit-0/symbol-0 key otherwise -- see
     * text_entry_key0_click_cb's own comment for the input-side half of
     * this same split. */
    if (!text_entry_numeric_only && text_entry_kp_mode == TEXT_ENTRY_KP_ABC) {
        lv_image_set_src(text_entry_key_img[0], asset_path("keyboard/upper.png"));
    } else {
        lv_image_set_src(text_entry_key_img[0], asset_path(text_entry_key_image(0)));
    }

    if (text_entry_numeric_only) {
        text_entry_set_key_available(text_entry_num_mode_key, false);
        text_entry_set_key_available(text_entry_abc_mode_key, false);
        text_entry_set_key_available(text_entry_sym_mode_key, false);
        text_entry_set_key_available(text_entry_dotneg_key, true);
        lv_obj_move_foreground(text_entry_dotneg_key);
        text_entry_set_key_available(text_entry_neg_key, true);
        lv_obj_move_foreground(text_entry_neg_key);
        return;
    }
    text_entry_set_key_available(text_entry_dotneg_key, false);
    text_entry_set_key_available(text_entry_neg_key, false);
    text_entry_set_key_available(text_entry_num_mode_key, true);
    text_entry_set_key_available(text_entry_abc_mode_key, true);
    text_entry_set_key_available(text_entry_sym_mode_key, true);
}

/* Shared by every cycling key (the 9 digit-slot keys, the key-0/digit-0
 * slot when it isn't acting as Shift, and the numeric-lock "./-" key) --
 * pulled out of the click handler so text_entry_key0_click_cb below can
 * reuse it without duplicating the multi-tap bookkeeping. A tap on a key
 * whose current character group has more than one character, repeated on
 * the SAME key before text_entry_multitap_timer elapses, cycles to the next
 * character in that group instead of inserting a new one (classic phone-
 * keypad multi-tap: key 2 pressed twice quickly types "b", not "aa"). Any
 * other case (a new key, a single-character group, or the timer having
 * already elapsed) just inserts the group's first character fresh.
 *
 * Case is decided once, on the FIRST tap of a fresh cycle (same_key_pending
 * false), and then pinned in text_entry_pending_shift for any further taps
 * that just cycle through candidates for that SAME letter -- see that
 * variable's own comment. That first tap is also where a one-shot Shift
 * (text_entry_shift, as opposed to the persistent text_entry_caps_lock) gets
 * consumed: it already did its job baking uppercase into chars[] above, so
 * it's cleared right after, letting the very next DIFFERENT key fall back to
 * whatever text_entry_caps_lock alone says. */
static void text_entry_handle_digit_key(int key_index) {
    bool same_key_pending = (text_entry_pending_key == key_index);
    bool uppercase = same_key_pending ? text_entry_pending_shift : text_entry_is_uppercase();

    char chars[8];
    text_entry_key_chars_ex(key_index, chars, sizeof(chars), uppercase);
    size_t n = strlen(chars);

    if (n > 1 && same_key_pending) {
        lv_textarea_delete_char(text_entry_textarea);
        text_entry_pending_tap_index = (text_entry_pending_tap_index + 1) % (int) n;
    } else {
        text_entry_pending_key = n > 1 ? key_index : -1;
        text_entry_pending_tap_index = 0;
        text_entry_pending_shift = uppercase;
        if (!text_entry_numeric_only && text_entry_kp_mode == TEXT_ENTRY_KP_ABC && text_entry_shift) {
            text_entry_shift = false;
            text_entry_refresh_keys();
        }
    }
    lv_textarea_add_char(text_entry_textarea, (uint32_t) chars[text_entry_pending_tap_index]);

    if (n > 1) {
        lv_timer_reset(text_entry_multitap_timer);
        lv_timer_resume(text_entry_multitap_timer);
    }
}

static void text_entry_key_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    text_entry_handle_digit_key((int) (intptr_t) lv_event_get_user_data(e));
}

/* Key index 0's screen slot (column 4, row 1) does double duty, matching
 * the real stock keyboard's own layout: while cycling letters, tapping it
 * arms a one-shot Shift for the next letter typed (text_entry_is_uppercase()'s
 * own comment) -- there's no digit "0" needed there since ABC mode has no
 * use for one. In NUM/SYM mode (or numeric_only, which behaves like NUM mode
 * regardless of text_entry_kp_mode's stale value -- see text_entry_key_chars_ex()'s
 * own numeric_only check) it's a completely ordinary cycling digit-slot key
 * for index 0, same as any other. */
static void text_entry_key0_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!text_entry_numeric_only && text_entry_kp_mode == TEXT_ENTRY_KP_ABC) {
        text_entry_finalize_pending();
        text_entry_shift = !text_entry_shift;
        text_entry_refresh_keys();
        return;
    }
    text_entry_handle_digit_key(0);
}

/* Numeric-only keypad: toggle a leading minus so atof() sees a negative
 * value. Multi-tap ".-" at the cursor (usually the end of a pre-filled
 * "3.00") produced "3.00-", which atof() treats as +3. */
static void text_entry_neg_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    text_entry_finalize_pending();
    const char * text = lv_textarea_get_text(text_entry_textarea);
    if (text && text[0] == '-') {
        lv_textarea_set_cursor_pos(text_entry_textarea, 1);
        lv_textarea_delete_char(text_entry_textarea);
    } else {
        lv_textarea_set_cursor_pos(text_entry_textarea, 0);
        lv_textarea_add_char(text_entry_textarea, (uint32_t) '-');
    }
}

/* Column 0 mode buttons (123, ABC, SYM) switch directly to their respective modes. */
static void text_entry_mode_num_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    text_entry_finalize_pending();
    text_entry_kp_mode = TEXT_ENTRY_KP_NUM;
    text_entry_shift = false; /* one-shot Shift is meaningless outside ABC -- don't let it carry back in */
    text_entry_refresh_keys();
}

/* Tapping ABC while already in ABC mode toggles persistent Caps Lock.
 * Switching into ABC from other modes activates ABC mode without resetting Caps Lock. */
static void text_entry_mode_abc_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    text_entry_finalize_pending();
    if (!text_entry_numeric_only && text_entry_kp_mode == TEXT_ENTRY_KP_ABC) {
        text_entry_caps_lock = !text_entry_caps_lock;
        text_entry_shift = false; /* toggling caps lock supersedes any pending one-shot arm */
    } else {
        text_entry_kp_mode = TEXT_ENTRY_KP_ABC;
    }
    text_entry_refresh_keys();
}

static void text_entry_mode_sym_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    text_entry_finalize_pending();
    text_entry_kp_mode = TEXT_ENTRY_KP_SYM;
    text_entry_shift = false; /* one-shot Shift is meaningless outside ABC -- don't let it carry back in */
    text_entry_refresh_keys();
}

static void text_entry_del_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    text_entry_finalize_pending();
    lv_textarea_delete_char(text_entry_textarea);
}

static void text_entry_left_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    text_entry_finalize_pending();
    lv_textarea_cursor_left(text_entry_textarea);
}

static void text_entry_right_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    text_entry_finalize_pending();
    lv_textarea_cursor_right(text_entry_textarea);
}

static void text_entry_space_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    text_entry_finalize_pending();
    lv_textarea_add_char(text_entry_textarea, ' ');
}

/* Defined in the search-binding section below (dismisses whichever
 * library screen currently has the keypad on loan for inline search --
 * see text_entry_inline_mode_active's own comment). Forward-declared here
 * since it's this file's only reference to search state from within the
 * text-entry section, which comes first. */

/* Shared by the T9 and QWERTY Enter keys. */
static void text_entry_enter(void) {
    text_entry_finalize_pending();
    if (text_entry_inline_mode_active) {
        t9_keypad_dismiss_only();
        return;
    }
    text_entry_commit();
}

static void text_entry_enter_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    text_entry_enter();
}

/* Every key is a plain clickable image at its own top-left grid cell (col,
 * row) -- see the icon_tile_press_event_cb doc comment in screen_builders.c
 * for the precedent this follows (clickable lv_image, no wrapper container).
 * Taller/wider keys (Enter, Space) just have a native asset that extends
 * past one cell; their top-left anchor is computed the same way as every
 * other key. */
static lv_obj_t * text_entry_make_key(lv_obj_t * scr, int col, int row, lv_event_cb_t cb, void * user_data) {
    lv_obj_t * img = lv_image_create(scr);
    lv_obj_add_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(img, LV_ALIGN_TOP_LEFT,
                 TEXT_ENTRY_GRID_X + col * (TEXT_ENTRY_KEY_SIZE + TEXT_ENTRY_KEY_GAP),
                 TEXT_ENTRY_GRID_Y + row * (TEXT_ENTRY_KEY_SIZE + TEXT_ENTRY_KEY_GAP));
    if (cb) lv_obj_add_event_cb(img, cb, LV_EVENT_CLICKED, user_data);
    return img;
}

/* ---- QWERTY layout: an lv_keyboard inside the same keypad container as the
 * T9 pad, covering the same screen footprint (so t9_keypad_get_grid_y() and
 * every screen's layout are layout-independent). Pages map onto lv_keyboard
 * modes: TEXT_LOWER/TEXT_UPPER = letters, SPECIAL = numbers + symbols,
 * USER_1 = more symbols, USER_2/USER_3 = accented letters (lower/upper).
 * Key labels are fixed ASCII/symbol constants, never translated, so function
 * keys are told apart by label (the same way lv_keyboard_def_event_cb does);
 * they carry LV_BUTTONMATRIX_CTRL_CUSTOM_1, every other key just types its
 * label. Shift reuses text_entry_shift (one-shot) and text_entry_caps_lock. ---- */
#define QWERTY_KEY_SHIFT LV_SYMBOL_UP
#define QWERTY_KEY_BACKSPACE LV_SYMBOL_BACKSPACE
#define QWERTY_KEY_LEFT LV_SYMBOL_LEFT
#define QWERTY_KEY_RIGHT LV_SYMBOL_RIGHT
#define QWERTY_KEY_ENTER LV_SYMBOL_NEW_LINE
#define QWERTY_KEY_SPACE " "
#define QWERTY_KEY_NUMBERS "123"
#define QWERTY_KEY_SYMBOLS "#+="
#define QWERTY_KEY_LETTERS "abc"
#define QWERTY_KEY_ACCENTS "àé"
#define QWERTY_KEY_CYRILLIC "абв"

/* Character keys fire on release (like the T9 image keys' LV_EVENT_CLICKED) so
 * a back-swipe that starts on a key does not type; Backspace keeps
 * press-trigger with auto-repeat so holding it deletes continuously. */
#define QWERTY_CHAR(w) (LV_BUTTONMATRIX_CTRL_CLICK_TRIG | LV_BUTTONMATRIX_CTRL_NO_REPEAT | (w))
#define QWERTY_FUNC(w) (QWERTY_CHAR(w) | LV_BUTTONMATRIX_CTRL_CUSTOM_1)
#define QWERTY_BACKSPACE(w) (LV_BUTTONMATRIX_CTRL_CUSTOM_1 | (w))
#define QWERTY_ENTER(w) (QWERTY_FUNC(w) | LV_BUTTONMATRIX_CTRL_CHECKED)
#define QWERTY_ROW4_CTRL QWERTY_FUNC(3), QWERTY_FUNC(2), QWERTY_FUNC(2), QWERTY_FUNC(7), QWERTY_FUNC(2), QWERTY_ENTER(4)
#define QWERTY_ROW4(mode_key, other_key) mode_key, other_key, QWERTY_KEY_LEFT, QWERTY_KEY_SPACE, QWERTY_KEY_RIGHT, QWERTY_KEY_ENTER

/* Letters: 9-key middle row, 34 buttons. */
static const lv_buttonmatrix_ctrl_t qwerty_ctrl_letters[] = {
    QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2),
    QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2),
    QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2),
    QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2),
    QWERTY_FUNC(3), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2),
    QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_BACKSPACE(3),
    QWERTY_ROW4_CTRL,
};

/* Symbols and accents: 10-key middle row, 35 buttons. */
static const lv_buttonmatrix_ctrl_t qwerty_ctrl_wide[] = {
    QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2),
    QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2),
    QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2),
    QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2),
    QWERTY_FUNC(3), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2),
    QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_BACKSPACE(3),
    QWERTY_ROW4_CTRL,
};

/* A map has one entry per button plus a "\n" after each of the three row ends
 * and the "" terminator, so a ctrl map that drifts out of step with its maps
 * fails the build instead of mis-styling keys at runtime. */
#define QWERTY_MAP_LEN(map) (sizeof(map) / sizeof((map)[0]))
#define QWERTY_CTRL_LEN(ctrl) (sizeof(ctrl) / sizeof((ctrl)[0]))

static const char * const qwerty_map_lower[] = {
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    "a", "s", "d", "f", "g", "h", "j", "k", "l", "\n",
    QWERTY_KEY_SHIFT, "z", "x", "c", "v", "b", "n", "m", QWERTY_KEY_BACKSPACE, "\n",
    QWERTY_ROW4(QWERTY_KEY_NUMBERS, QWERTY_KEY_ACCENTS), ""
};

static const char * const qwerty_map_upper[] = {
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    "A", "S", "D", "F", "G", "H", "J", "K", "L", "\n",
    QWERTY_KEY_SHIFT, "Z", "X", "C", "V", "B", "N", "M", QWERTY_KEY_BACKSPACE, "\n",
    QWERTY_ROW4(QWERTY_KEY_NUMBERS, QWERTY_KEY_ACCENTS), ""
};

/* Layout variants reuse the same key geometry and controls. They are applied
 * to the shared lv_keyboard when a modal or inline-search session starts, so
 * changing the saved setting never leaves one surface on a stale map. */
static const char * const qwerty_map_azerty_lower[] = {
    "a", "z", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    "q", "s", "d", "f", "g", "h", "j", "k", "l", "m", "\n",
    QWERTY_KEY_SHIFT, "w", "x", "c", "v", "b", "n", QWERTY_KEY_BACKSPACE, "\n",
    QWERTY_ROW4(QWERTY_KEY_NUMBERS, QWERTY_KEY_ACCENTS), ""
};
static const char * const qwerty_map_azerty_upper[] = {
    "A", "Z", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    "Q", "S", "D", "F", "G", "H", "J", "K", "L", "M", "\n",
    QWERTY_KEY_SHIFT, "W", "X", "C", "V", "B", "N", QWERTY_KEY_BACKSPACE, "\n",
    QWERTY_ROW4(QWERTY_KEY_NUMBERS, QWERTY_KEY_ACCENTS), ""
};

static const char * const qwerty_map_qwertz_lower[] = {
    "q", "w", "e", "r", "t", "z", "u", "i", "o", "p", "ü", "\n",
    "a", "s", "d", "f", "g", "h", "j", "k", "l", "ö", "ä", "\n",
    QWERTY_KEY_SHIFT, "y", "x", "c", "v", "b", "n", "m", QWERTY_KEY_BACKSPACE, "\n",
    QWERTY_ROW4(QWERTY_KEY_NUMBERS, QWERTY_KEY_ACCENTS), ""
};
static const char * const qwerty_map_qwertz_upper[] = {
    "Q", "W", "E", "R", "T", "Z", "U", "I", "O", "P", "Ü", "\n",
    "A", "S", "D", "F", "G", "H", "J", "K", "L", "Ö", "Ä", "\n",
    QWERTY_KEY_SHIFT, "Y", "X", "C", "V", "B", "N", "M", QWERTY_KEY_BACKSPACE, "\n",
    QWERTY_ROW4(QWERTY_KEY_NUMBERS, QWERTY_KEY_ACCENTS), ""
};

/* Full Cyrillic alphabets fit on their language-specific primary pages.
 * Latin maps for these settings replace the accents shortcut with an абв
 * return key, so URLs and passwords can be entered and the user can switch
 * back without restarting the field. */
#define QWERTY_LAYOUT_CHAR QWERTY_CHAR(2)
static const lv_buttonmatrix_ctrl_t qwerty_ctrl_azerty[] = {
    QWERTY_LAYOUT_CHAR, QWERTY_LAYOUT_CHAR, QWERTY_LAYOUT_CHAR, QWERTY_LAYOUT_CHAR, QWERTY_LAYOUT_CHAR, QWERTY_LAYOUT_CHAR,
    QWERTY_LAYOUT_CHAR, QWERTY_LAYOUT_CHAR, QWERTY_LAYOUT_CHAR, QWERTY_LAYOUT_CHAR, QWERTY_LAYOUT_CHAR, QWERTY_LAYOUT_CHAR,
    QWERTY_LAYOUT_CHAR, QWERTY_LAYOUT_CHAR, QWERTY_LAYOUT_CHAR, QWERTY_LAYOUT_CHAR, QWERTY_LAYOUT_CHAR, QWERTY_LAYOUT_CHAR,
    QWERTY_LAYOUT_CHAR, QWERTY_LAYOUT_CHAR, QWERTY_FUNC(3), QWERTY_LAYOUT_CHAR, QWERTY_LAYOUT_CHAR, QWERTY_LAYOUT_CHAR,
    QWERTY_LAYOUT_CHAR, QWERTY_LAYOUT_CHAR, QWERTY_LAYOUT_CHAR, QWERTY_BACKSPACE(3), QWERTY_FUNC(3), QWERTY_FUNC(2),
    QWERTY_FUNC(2), QWERTY_FUNC(7), QWERTY_FUNC(2), QWERTY_ENTER(4),
};
static const lv_buttonmatrix_ctrl_t qwerty_ctrl_extended_letters[] = {
    QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2),
    QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2),
    QWERTY_FUNC(3), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_BACKSPACE(3),
    QWERTY_ROW4_CTRL,
};
static const lv_buttonmatrix_ctrl_t qwerty_ctrl_cyrillic[] = {
    QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2),
    QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2),
    QWERTY_FUNC(3), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_BACKSPACE(3),
    QWERTY_FUNC(3), QWERTY_FUNC(3), QWERTY_FUNC(2), QWERTY_FUNC(7), QWERTY_FUNC(2), QWERTY_ENTER(4),
};
static const char * const qwerty_map_russian_lower[] = {
    "ё", "й", "ц", "у", "к", "е", "н", "г", "ш", "щ", "з", "х", "ъ", "\n",
    "ф", "ы", "в", "а", "п", "р", "о", "л", "д", "ж", "э", "\n",
    QWERTY_KEY_SHIFT, "я", "ч", "с", "м", "и", "т", "ь", "б", "ю", QWERTY_KEY_BACKSPACE, "\n",
    QWERTY_ROW4(QWERTY_KEY_NUMBERS, QWERTY_KEY_LETTERS), ""
};
static const char * const qwerty_map_russian_upper[] = {
    "Ё", "Й", "Ц", "У", "К", "Е", "Н", "Г", "Ш", "Щ", "З", "Х", "Ъ", "\n",
    "Ф", "Ы", "В", "А", "П", "Р", "О", "Л", "Д", "Ж", "Э", "\n",
    QWERTY_KEY_SHIFT, "Я", "Ч", "С", "М", "И", "Т", "Ь", "Б", "Ю", QWERTY_KEY_BACKSPACE, "\n",
    QWERTY_ROW4(QWERTY_KEY_NUMBERS, QWERTY_KEY_LETTERS), ""
};
static const char * const qwerty_map_ukrainian_lower[] = {
    "й", "ц", "у", "к", "е", "н", "г", "ш", "щ", "з", "х", "ї", "ґ", "\n",
    "ф", "і", "в", "а", "п", "р", "о", "л", "д", "ж", "є", "\n",
    QWERTY_KEY_SHIFT, "я", "ч", "с", "м", "и", "т", "ь", "б", "ю", QWERTY_KEY_BACKSPACE, "\n",
    QWERTY_ROW4(QWERTY_KEY_NUMBERS, QWERTY_KEY_LETTERS), ""
};
static const char * const qwerty_map_ukrainian_upper[] = {
    "Й", "Ц", "У", "К", "Е", "Н", "Г", "Ш", "Щ", "З", "Х", "Ї", "Ґ", "\n",
    "Ф", "І", "В", "А", "П", "Р", "О", "Л", "Д", "Ж", "Є", "\n",
    QWERTY_KEY_SHIFT, "Я", "Ч", "С", "М", "И", "Т", "Ь", "Б", "Ю", QWERTY_KEY_BACKSPACE, "\n",
    QWERTY_ROW4(QWERTY_KEY_NUMBERS, QWERTY_KEY_LETTERS), ""
};
static const char * const qwerty_map_latin_cyr_lower[] = {
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    "a", "s", "d", "f", "g", "h", "j", "k", "l", "\n",
    QWERTY_KEY_SHIFT, "z", "x", "c", "v", "b", "n", "m", QWERTY_KEY_BACKSPACE, "\n",
    QWERTY_ROW4(QWERTY_KEY_NUMBERS, QWERTY_KEY_CYRILLIC), ""
};
static const char * const qwerty_map_latin_cyr_upper[] = {
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    "A", "S", "D", "F", "G", "H", "J", "K", "L", "\n",
    QWERTY_KEY_SHIFT, "Z", "X", "C", "V", "B", "N", "M", QWERTY_KEY_BACKSPACE, "\n",
    QWERTY_ROW4(QWERTY_KEY_NUMBERS, QWERTY_KEY_CYRILLIC), ""
};

/* Every ASCII symbol the T9 symbol keys offer is reachable across the two
 * symbol pages; the second page adds Latin-1 currency/section signs (Montserrat
 * covers Latin-1 and Latin Extended-A only, so no euro sign or bullet). */
static const char * const qwerty_map_special[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "@", "#", "$", "%", "&", "-", "+", "(", ")", "/", "\n",
    QWERTY_KEY_SYMBOLS, ".", ",", "?", "!", "'", "\"", ":", QWERTY_KEY_BACKSPACE, "\n",
    QWERTY_ROW4(QWERTY_KEY_LETTERS, QWERTY_KEY_ACCENTS), ""
};

static const char * const qwerty_map_symbols[] = {
    "[", "]", "{", "}", "#", "%", "^", "*", "+", "=", "\n",
    "_", "\\", "|", "~", "<", ">", "`", "£", "¥", "§", "\n",
    QWERTY_KEY_NUMBERS, ".", ",", ";", ":", "!", "?", "'", QWERTY_KEY_BACKSPACE, "\n",
    QWERTY_ROW4(QWERTY_KEY_LETTERS, QWERTY_KEY_ACCENTS), ""
};

/* Accents: acute/grave vowels, circumflex/diaeresis vowels, then the rest of
 * the Latin-1 letters (n-tilde, c-cedilla, a/o-tilde, sharp s) and the
 * French ligatures, y-diaeresis, and inverted ? and !. */
static const lv_buttonmatrix_ctrl_t qwerty_ctrl_accents[] = {
    QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2),
    QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2),
    QWERTY_FUNC(3), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_CHAR(2), QWERTY_BACKSPACE(3),
    QWERTY_FUNC(3), QWERTY_FUNC(3), QWERTY_FUNC(2), QWERTY_FUNC(7), QWERTY_FUNC(2), QWERTY_ENTER(4),
};

static const char * const qwerty_map_accents_lower[] = {
    "á", "é", "í", "ó", "ú", "à", "è", "ì", "ò", "ù", "å", "\n",
    "â", "ê", "î", "ô", "û", "ä", "ë", "ï", "ö", "ü", "ÿ", "\n",
    QWERTY_KEY_SHIFT, "ñ", "ç", "ã", "õ", "ß", "œ", "æ", "¿", "¡", QWERTY_KEY_BACKSPACE, "\n",
    QWERTY_ROW4(QWERTY_KEY_NUMBERS, QWERTY_KEY_LETTERS), ""
};

static const char * const qwerty_map_accents_upper[] = {
    "Á", "É", "Í", "Ó", "Ú", "À", "È", "Ì", "Ò", "Ù", "Å", "\n",
    "Â", "Ê", "Î", "Ô", "Û", "Ä", "Ë", "Ï", "Ö", "Ü", "Ÿ", "\n",
    QWERTY_KEY_SHIFT, "Ñ", "Ç", "Ã", "Õ", "ẞ", "Œ", "Æ", "¿", "¡", QWERTY_KEY_BACKSPACE, "\n",
    QWERTY_ROW4(QWERTY_KEY_NUMBERS, QWERTY_KEY_LETTERS), ""
};

#define QWERTY_ASSERT_MAP_CTRL(map, ctrl, name) \
    _Static_assert(QWERTY_MAP_LEN(map) == QWERTY_CTRL_LEN(ctrl) + 4, name " map/ctrl mismatch")
QWERTY_ASSERT_MAP_CTRL(qwerty_map_azerty_lower, qwerty_ctrl_azerty, "AZERTY lower");
QWERTY_ASSERT_MAP_CTRL(qwerty_map_azerty_upper, qwerty_ctrl_azerty, "AZERTY upper");
QWERTY_ASSERT_MAP_CTRL(qwerty_map_qwertz_lower, qwerty_ctrl_extended_letters, "German lower");
QWERTY_ASSERT_MAP_CTRL(qwerty_map_qwertz_upper, qwerty_ctrl_extended_letters, "German upper");
QWERTY_ASSERT_MAP_CTRL(qwerty_map_russian_lower, qwerty_ctrl_cyrillic, "Russian lower");
QWERTY_ASSERT_MAP_CTRL(qwerty_map_russian_upper, qwerty_ctrl_cyrillic, "Russian upper");
QWERTY_ASSERT_MAP_CTRL(qwerty_map_ukrainian_lower, qwerty_ctrl_cyrillic, "Ukrainian lower");
QWERTY_ASSERT_MAP_CTRL(qwerty_map_ukrainian_upper, qwerty_ctrl_cyrillic, "Ukrainian upper");
QWERTY_ASSERT_MAP_CTRL(qwerty_map_latin_cyr_lower, qwerty_ctrl_letters, "Latin Cyrillic-mode lower");
QWERTY_ASSERT_MAP_CTRL(qwerty_map_latin_cyr_upper, qwerty_ctrl_letters, "Latin Cyrillic-mode upper");
#undef QWERTY_ASSERT_MAP_CTRL

_Static_assert(QWERTY_MAP_LEN(qwerty_map_lower) == QWERTY_CTRL_LEN(qwerty_ctrl_letters) + 4, "qwerty lower map/ctrl mismatch");
_Static_assert(QWERTY_MAP_LEN(qwerty_map_upper) == QWERTY_CTRL_LEN(qwerty_ctrl_letters) + 4, "qwerty upper map/ctrl mismatch");
_Static_assert(QWERTY_MAP_LEN(qwerty_map_special) == QWERTY_CTRL_LEN(qwerty_ctrl_wide) + 4, "qwerty special map/ctrl mismatch");
_Static_assert(QWERTY_MAP_LEN(qwerty_map_symbols) == QWERTY_CTRL_LEN(qwerty_ctrl_wide) + 4, "qwerty symbols map/ctrl mismatch");
_Static_assert(QWERTY_MAP_LEN(qwerty_map_accents_lower) == QWERTY_CTRL_LEN(qwerty_ctrl_accents) + 4, "qwerty accents map/ctrl mismatch");
_Static_assert(QWERTY_MAP_LEN(qwerty_map_accents_upper) == QWERTY_CTRL_LEN(qwerty_ctrl_accents) + 4, "qwerty accents map/ctrl mismatch");

/* The page's lower-case / base mode: the upper variants fold onto it. */
static lv_keyboard_mode_t text_entry_qwerty_page(void) {
    lv_keyboard_mode_t mode = lv_keyboard_get_mode(text_entry_qwerty_kb);
    if (mode == LV_KEYBOARD_MODE_TEXT_UPPER) return LV_KEYBOARD_MODE_TEXT_LOWER;
    if (mode == LV_KEYBOARD_MODE_USER_3) return LV_KEYBOARD_MODE_USER_2;
    return mode;
}

/* Install the selected language's maps on the singleton keyboard. The
 * latin_mode argument is used only by Russian and Ukrainian layouts, where
 * the native-language key switches to Latin QWERTY and the абв key returns. */
static void text_entry_qwerty_apply_layout(bool latin_mode) {
    const char * const * lower = qwerty_map_lower;
    const char * const * upper = qwerty_map_upper;
    const char * const * accents_lower = qwerty_map_accents_lower;
    const char * const * accents_upper = qwerty_map_accents_upper;
    const lv_buttonmatrix_ctrl_t * letters_ctrl = qwerty_ctrl_letters;

    switch (text_entry_keyboard_layout) {
        case KEYBOARD_LAYOUT_AZERTY:
            lower = qwerty_map_azerty_lower;
            upper = qwerty_map_azerty_upper;
            letters_ctrl = qwerty_ctrl_azerty;
            break;
        case KEYBOARD_LAYOUT_QWERTZ:
            lower = qwerty_map_qwertz_lower;
            upper = qwerty_map_qwertz_upper;
            letters_ctrl = qwerty_ctrl_extended_letters;
            break;
        case KEYBOARD_LAYOUT_RUSSIAN:
        case KEYBOARD_LAYOUT_UKRAINIAN:
            if (!latin_mode) {
                lower = text_entry_keyboard_layout == KEYBOARD_LAYOUT_RUSSIAN ? qwerty_map_russian_lower : qwerty_map_ukrainian_lower;
                upper = text_entry_keyboard_layout == KEYBOARD_LAYOUT_RUSSIAN ? qwerty_map_russian_upper : qwerty_map_ukrainian_upper;
                letters_ctrl = qwerty_ctrl_cyrillic;
            } else {
                lower = qwerty_map_latin_cyr_lower;
                upper = qwerty_map_latin_cyr_upper;
            }
            break;
        default:
            /* Latin QWERTY is the single default for every language without
             * a distinct physical arrangement. */
            break;
    }

    /* Wider native rows must stay readable on the 320 px display and at
     * large UI font sizes. Prefer the active custom/fallback face when it
     * fits the cap, then use the shared external bitmap font. */
    const lv_font_t * font = gui_theme_font(GUI_FONT_ROLE_ROW);
    {
        bool cyrillic = text_entry_keyboard_layout == KEYBOARD_LAYOUT_RUSSIAN ||
                        text_entry_keyboard_layout == KEYBOARD_LAYOUT_UKRAINIAN;
        const lv_font_t * cap = BOARD_SCREEN_WIDTH < 480
            ? (cyrillic ? &lv_font_montserrat_16 : &lv_font_montserrat_18)
            : (cyrillic ? &lv_font_montserrat_22 : &lv_font_montserrat_26);
        if (font->line_height > cap->line_height)
            font = app_font_16.line_height <= cap->line_height ? &app_font_16 : cap;
    }
    lv_obj_set_style_text_font(text_entry_qwerty_kb, font, LV_PART_ITEMS);

    lv_keyboard_set_map(text_entry_qwerty_kb, LV_KEYBOARD_MODE_TEXT_LOWER, lower, letters_ctrl);
    lv_keyboard_set_map(text_entry_qwerty_kb, LV_KEYBOARD_MODE_TEXT_UPPER, upper, letters_ctrl);
    lv_keyboard_set_map(text_entry_qwerty_kb, LV_KEYBOARD_MODE_SPECIAL, qwerty_map_special, qwerty_ctrl_wide);
    lv_keyboard_set_map(text_entry_qwerty_kb, LV_KEYBOARD_MODE_USER_1, qwerty_map_symbols, qwerty_ctrl_wide);
    lv_keyboard_set_map(text_entry_qwerty_kb, LV_KEYBOARD_MODE_USER_2, accents_lower, qwerty_ctrl_accents);
    lv_keyboard_set_map(text_entry_qwerty_kb, LV_KEYBOARD_MODE_USER_3, accents_upper, qwerty_ctrl_accents);
}

/* Shows `page` (TEXT_LOWER, SPECIAL, USER_1 or USER_2) in the case the shift
 * state asks for, and marks the Shift key while caps lock is on (the ctrl map
 * is reapplied on every mode change, so the mark is set after it). */
static void text_entry_qwerty_show(lv_keyboard_mode_t page) {
    bool cased = page == LV_KEYBOARD_MODE_TEXT_LOWER || page == LV_KEYBOARD_MODE_USER_2;
    lv_keyboard_set_mode(text_entry_qwerty_kb,
                         (cased && text_entry_is_uppercase()) ? (lv_keyboard_mode_t) (page + 1) : page);
    if (!cased) return;
    const char * txt;
    for (uint32_t i = 0; (txt = lv_buttonmatrix_get_button_text(text_entry_qwerty_kb, i)) != NULL; i++) {
        if (strcmp(txt, QWERTY_KEY_SHIFT) != 0) continue;
        if (text_entry_caps_lock) {
            lv_buttonmatrix_set_button_ctrl(text_entry_qwerty_kb, i, LV_BUTTONMATRIX_CTRL_CHECKED);
        } else {
            lv_buttonmatrix_clear_button_ctrl(text_entry_qwerty_kb, i, LV_BUTTONMATRIX_CTRL_CHECKED);
        }
        break;
    }
}

/* Page switch: a pending one-shot Shift never carries over, and caps lock only
 * survives between the letters and accents pages. */
static void text_entry_qwerty_go(lv_keyboard_mode_t page) {
    text_entry_variant_close();
    text_entry_shift = false;
    if (page == LV_KEYBOARD_MODE_SPECIAL || page == LV_KEYBOARD_MODE_USER_1) text_entry_caps_lock = false;
    text_entry_qwerty_show(page);
}

static void text_entry_variant_close(void) {
    if (text_entry_variant_overlay) {
        lv_obj_add_flag(text_entry_variant_overlay, LV_OBJ_FLAG_HIDDEN);
        if (text_entry_variant_panel) lv_obj_add_flag(text_entry_variant_panel, LV_OBJ_FLAG_HIDDEN);
    }
    text_entry_variant_choice_count = 0;
    memset(text_entry_variant_choices, 0, sizeof(text_entry_variant_choices));
}

static void text_entry_variant_set_owner(lv_obj_t * owner) {
    if (text_entry_variant_owner_screen == owner) return;
    if (text_entry_variant_owner_screen) {
        lv_obj_remove_event_cb_with_user_data(text_entry_variant_owner_screen,
                                               text_entry_variant_owner_event_cb, NULL);
    }
    text_entry_variant_owner_screen = owner;
    if (owner) {
        lv_obj_add_event_cb(owner, text_entry_variant_owner_event_cb, LV_EVENT_SCREEN_UNLOAD_START, NULL);
        lv_obj_add_event_cb(owner, text_entry_variant_owner_event_cb, LV_EVENT_DELETE, NULL);
        lv_obj_add_event_cb(owner, text_entry_variant_owner_event_cb, LV_EVENT_GESTURE, NULL);
    }
}

static void text_entry_variant_owner_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * owner = lv_event_get_current_target(e);
    text_entry_variant_close();
    if (code == LV_EVENT_DELETE && owner == text_entry_variant_owner_screen)
        text_entry_variant_owner_screen = NULL;
}

static size_t text_entry_variant_group(const char * key, bool upper, const char * const ** choices) {
#define VARIANTS(lower_name, upper_name, lower_list, upper_list, count_) \
    if (strcmp(key, lower_name) == 0 || strcmp(key, upper_name) == 0) { \
        *choices = upper ? upper_list : lower_list; \
        return count_; \
    }
    static const char * const a_lower[] = { "a", "à", "á", "â", "ã", "ä", "å", "æ" };
    static const char * const a_upper[] = { "A", "À", "Á", "Â", "Ã", "Ä", "Å", "Æ" };
    static const char * const e_lower[] = { "e", "è", "é", "ê", "ë" };
    static const char * const e_upper[] = { "E", "È", "É", "Ê", "Ë" };
    static const char * const i_lower[] = { "i", "ì", "í", "î", "ï" };
    static const char * const i_upper[] = { "I", "Ì", "Í", "Î", "Ï" };
    static const char * const o_lower[] = { "o", "ò", "ó", "ô", "õ", "ö", "œ" };
    static const char * const o_upper[] = { "O", "Ò", "Ó", "Ô", "Õ", "Ö", "Œ" };
    static const char * const u_lower[] = { "u", "ù", "ú", "û", "ü" };
    static const char * const u_upper[] = { "U", "Ù", "Ú", "Û", "Ü" };
    static const char * const c_lower[] = { "c", "ç" };
    static const char * const c_upper[] = { "C", "Ç" };
    static const char * const n_lower[] = { "n", "ñ", "ń" };
    static const char * const n_upper[] = { "N", "Ñ", "Ń" };
    static const char * const s_lower[] = { "s", "ß" };
    static const char * const s_upper[] = { "S", "ẞ" };
    static const char * const y_lower[] = { "y", "ÿ" };
    static const char * const y_upper[] = { "Y", "Ÿ" };
    VARIANTS("a", "A", a_lower, a_upper, 8)
    VARIANTS("à", "À", a_lower, a_upper, 8)
    VARIANTS("á", "Á", a_lower, a_upper, 8)
    VARIANTS("â", "Â", a_lower, a_upper, 8)
    VARIANTS("ã", "Ã", a_lower, a_upper, 8)
    VARIANTS("ä", "Ä", a_lower, a_upper, 8)
    VARIANTS("å", "Å", a_lower, a_upper, 8)
    VARIANTS("æ", "Æ", a_lower, a_upper, 8)
    VARIANTS("e", "E", e_lower, e_upper, 5)
    VARIANTS("è", "È", e_lower, e_upper, 5)
    VARIANTS("é", "É", e_lower, e_upper, 5)
    VARIANTS("ê", "Ê", e_lower, e_upper, 5)
    VARIANTS("ë", "Ë", e_lower, e_upper, 5)
    VARIANTS("i", "I", i_lower, i_upper, 5)
    VARIANTS("ì", "Ì", i_lower, i_upper, 5)
    VARIANTS("í", "Í", i_lower, i_upper, 5)
    VARIANTS("î", "Î", i_lower, i_upper, 5)
    VARIANTS("ï", "Ï", i_lower, i_upper, 5)
    VARIANTS("o", "O", o_lower, o_upper, 7)
    VARIANTS("ò", "Ò", o_lower, o_upper, 7)
    VARIANTS("ó", "Ó", o_lower, o_upper, 7)
    VARIANTS("ô", "Ô", o_lower, o_upper, 7)
    VARIANTS("õ", "Õ", o_lower, o_upper, 7)
    VARIANTS("ö", "Ö", o_lower, o_upper, 7)
    VARIANTS("œ", "Œ", o_lower, o_upper, 7)
    VARIANTS("u", "U", u_lower, u_upper, 5)
    VARIANTS("ù", "Ù", u_lower, u_upper, 5)
    VARIANTS("ú", "Ú", u_lower, u_upper, 5)
    VARIANTS("û", "Û", u_lower, u_upper, 5)
    VARIANTS("ü", "Ü", u_lower, u_upper, 5)
    VARIANTS("c", "C", c_lower, c_upper, 2)
    VARIANTS("ç", "Ç", c_lower, c_upper, 2)
    VARIANTS("n", "N", n_lower, n_upper, 3)
    VARIANTS("ñ", "Ñ", n_lower, n_upper, 3)
    VARIANTS("ń", "Ń", n_lower, n_upper, 3)
    VARIANTS("s", "S", s_lower, s_upper, 2)
    VARIANTS("ß", "ẞ", s_lower, s_upper, 2)
    VARIANTS("y", "Y", y_lower, y_upper, 2)
    VARIANTS("ÿ", "Ÿ", y_lower, y_upper, 2)
#undef VARIANTS
    *choices = NULL;
    return 0;
}

static void text_entry_variant_select_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    lv_indev_t * indev = lv_event_get_indev(e);
    if (indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
        lv_point_t point;
        lv_indev_get_point(indev, &point);
        if (!lv_obj_hit_test(lv_event_get_current_target(e), &point)) return;
    }
    size_t index = (size_t) (uintptr_t) lv_event_get_user_data(e);
    if (index >= text_entry_variant_choice_count || !text_entry_variant_choices[index]) return;
    const char * selected = text_entry_variant_choices[index];
    text_entry_finalize_pending();
    lv_textarea_add_text(text_entry_textarea, selected);
    text_entry_shift = false;
    text_entry_qwerty_show(text_entry_qwerty_page());
    text_entry_variant_close();
}

static void text_entry_variant_overlay_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_DELETE) {
        text_entry_variant_overlay = NULL;
        text_entry_variant_panel = NULL;
        text_entry_variant_choice_count = 0;
        memset(text_entry_variant_choices, 0, sizeof(text_entry_variant_choices));
        memset(text_entry_variant_buttons, 0, sizeof(text_entry_variant_buttons));
        text_entry_variant_swallow_release = false;
        return;
    }
    if (lv_event_get_code(e) == LV_EVENT_CLICKED &&
        lv_event_get_target(e) == text_entry_variant_overlay)
        text_entry_variant_close();
}

static void text_entry_variant_show(const char * key, lv_indev_t * indev) {
    const char * const * choices;
    size_t count = text_entry_variant_group(key, text_entry_is_uppercase(), &choices);
    if (!count || !text_entry_variant_overlay || !text_entry_variant_panel) return;

    text_entry_variant_choice_count = count;
    const lv_font_t * popup_font = lv_obj_get_style_text_font(text_entry_qwerty_kb, LV_PART_ITEMS);
    for (size_t i = 0; i < 8; i++) {
        if (text_entry_variant_buttons[i]) {
            if (i < count) {
                text_entry_variant_choices[i] = choices[i];
                lv_obj_remove_flag(text_entry_variant_buttons[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_t * label = lv_obj_get_child(text_entry_variant_buttons[i], 0);
                lv_label_set_text(label, choices[i]);
                lv_obj_set_style_text_font(label, popup_font, 0);
            } else {
                text_entry_variant_choices[i] = NULL;
                lv_obj_add_flag(text_entry_variant_buttons[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    int32_t cols = (int32_t) (count > 4 ? 4 : count);
    int32_t rows = (int32_t) ((count + 3) / 4);
    int32_t gap = BOARD_SCALE_PX(4);
    int32_t pad = BOARD_SCALE_PX(6);
    int32_t cell = BOARD_SCALE_PX(48);
    if (cell < 40) cell = 40;
    if (cell < popup_font->line_height + BOARD_SCALE_PX(8))
        cell = popup_font->line_height + BOARD_SCALE_PX(8);
    int32_t screen_w = BOARD_SCREEN_WIDTH;
    int32_t screen_h = BOARD_SCREEN_HEIGHT;
    int32_t width = cols * cell + (cols - 1) * gap + pad * 2;
    if (width > screen_w - 8) width = screen_w - 8;
    int32_t height = rows * cell + (rows - 1) * gap + pad * 2;
    lv_obj_set_size(text_entry_variant_panel, width, height);
    lv_obj_remove_flag(text_entry_variant_panel, LV_OBJ_FLAG_HIDDEN);

    lv_point_t point = { screen_w / 2, screen_h / 2 };
    if (indev) lv_indev_get_point(indev, &point);
    int32_t x = point.x - width / 2;
    int32_t y = point.y - height - BOARD_SCALE_PX(8);
    if (x < 4) x = 4;
    if (x + width > screen_w - 4) x = screen_w - width - 4;
    if (y < 4) y = 4;
    if (y + height > screen_h - 4) y = screen_h - height - 4;
    lv_obj_set_pos(text_entry_variant_panel, x, y);

    for (size_t i = 0; i < count; i++) {
        int32_t col = (int32_t) (i % 4);
        int32_t row = (int32_t) (i / 4);
        int32_t actual_cell = (width - 2 * pad - (cols - 1) * gap) / cols;
        lv_obj_set_size(text_entry_variant_buttons[i], actual_cell, cell);
        lv_obj_set_pos(text_entry_variant_buttons[i], pad + col * (actual_cell + gap), pad + row * (cell + gap));
    }
    lv_obj_remove_flag(text_entry_variant_overlay, LV_OBJ_FLAG_HIDDEN);
    /* Inline release can put the textarea above the shared keypad group.
     * Bring the transparent group forward so outside taps reach the shield. */
    lv_obj_move_foreground(text_entry_keypad_group);
    lv_obj_move_foreground(text_entry_variant_overlay);
}

static void text_entry_qwerty_long_press_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_LONG_PRESSED) return;
    lv_obj_t * kb = lv_event_get_current_target(e);
    uint32_t btn = lv_buttonmatrix_get_selected_button(kb);
    if (btn == LV_BUTTONMATRIX_BUTTON_NONE) return;
    const char * txt = lv_buttonmatrix_get_button_text(kb, btn);
    if (!txt || lv_buttonmatrix_has_button_ctrl(kb, btn, LV_BUTTONMATRIX_CTRL_CUSTOM_1)) return;
    const char * const * choices;
    if (!text_entry_variant_group(txt, text_entry_is_uppercase(), &choices)) return;
    text_entry_variant_swallow_release = true;
    text_entry_variant_show(txt, lv_event_get_indev(e));
}

static void text_entry_variant_cancel_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESS_LOST) {
        text_entry_variant_swallow_release = false;
        text_entry_variant_close();
    } else if (code == LV_EVENT_GESTURE) {
        text_entry_variant_close();
    }
}

static void text_entry_variant_release_cb(lv_event_t * e) {
    /* Buttonmatrix sends VALUE_CHANGED from its class RELEASED handler first.
     * Clear the guard afterwards, including releases that slid off the key. */
    if (lv_event_get_code(e) == LV_EVENT_RELEASED)
        text_entry_variant_swallow_release = false;
}

static void text_entry_qwerty_event_cb(lv_event_t * e) {
    lv_obj_t * kb = lv_event_get_current_target(e);
    if (text_entry_variant_swallow_release) {
        text_entry_variant_swallow_release = false;
        return;
    }
    if (text_entry_variant_overlay && !lv_obj_has_flag(text_entry_variant_overlay, LV_OBJ_FLAG_HIDDEN)) return;
    uint32_t btn = lv_buttonmatrix_get_selected_button(kb);
    const char * txt = lv_buttonmatrix_get_button_text(kb, btn);
    if (!txt) return;
    text_entry_finalize_pending();

    if (!lv_buttonmatrix_has_button_ctrl(kb, btn, LV_BUTTONMATRIX_CTRL_CUSTOM_1)) {
        lv_textarea_add_text(text_entry_textarea, txt);
        if (text_entry_shift) { /* one-shot Shift covers exactly one character */
            text_entry_shift = false;
            text_entry_qwerty_show(text_entry_qwerty_page());
        }
    } else if (strcmp(txt, QWERTY_KEY_BACKSPACE) == 0) {
        lv_textarea_delete_char(text_entry_textarea);
    } else if (strcmp(txt, QWERTY_KEY_SPACE) == 0) {
        lv_textarea_add_char(text_entry_textarea, ' ');
    } else if (strcmp(txt, QWERTY_KEY_LEFT) == 0) {
        lv_textarea_cursor_left(text_entry_textarea);
    } else if (strcmp(txt, QWERTY_KEY_RIGHT) == 0) {
        lv_textarea_cursor_right(text_entry_textarea);
    } else if (strcmp(txt, QWERTY_KEY_ENTER) == 0) {
        text_entry_enter();
    } else if (strcmp(txt, QWERTY_KEY_SHIFT) == 0) {
        /* off -> one-shot -> caps lock -> off */
        if (text_entry_caps_lock) {
            text_entry_caps_lock = false;
        } else if (text_entry_shift) {
            text_entry_shift = false;
            text_entry_caps_lock = true;
        } else {
            text_entry_shift = true;
        }
        text_entry_qwerty_show(text_entry_qwerty_page());
    } else if (strcmp(txt, QWERTY_KEY_NUMBERS) == 0) {
        text_entry_qwerty_go(LV_KEYBOARD_MODE_SPECIAL);
    } else if (strcmp(txt, QWERTY_KEY_SYMBOLS) == 0) {
        text_entry_qwerty_go(LV_KEYBOARD_MODE_USER_1);
    } else if (strcmp(txt, QWERTY_KEY_LETTERS) == 0) {
        if ((text_entry_keyboard_layout == KEYBOARD_LAYOUT_RUSSIAN ||
             text_entry_keyboard_layout == KEYBOARD_LAYOUT_UKRAINIAN) &&
            !text_entry_qwerty_latin_mode) {
            text_entry_qwerty_latin_mode = true;
            text_entry_qwerty_apply_layout(true);
        }
        text_entry_qwerty_go(LV_KEYBOARD_MODE_TEXT_LOWER);
    } else if (strcmp(txt, QWERTY_KEY_CYRILLIC) == 0 &&
               (text_entry_keyboard_layout == KEYBOARD_LAYOUT_RUSSIAN ||
                text_entry_keyboard_layout == KEYBOARD_LAYOUT_UKRAINIAN)) {
        text_entry_qwerty_latin_mode = false;
        text_entry_qwerty_apply_layout(false);
        text_entry_qwerty_go(LV_KEYBOARD_MODE_TEXT_LOWER);
    } else if (strcmp(txt, QWERTY_KEY_ACCENTS) == 0) {
        text_entry_qwerty_go(LV_KEYBOARD_MODE_USER_2);
    }
}

/* Footprint and padding make the four key rows span exactly the T9 grid's
 * vertical range, so the keyboard replaces the T9 pad without moving anything
 * around it. Colors come from the shared theme-live styles, so the keyboard
 * follows accent and palette changes like the rest of the UI. */
static lv_obj_t * build_qwerty_keyboard(lv_obj_t * parent) {
    lv_obj_t * kb = lv_keyboard_create(parent);
    /* Our handler edits text_entry_textarea directly; lv_keyboard_set_textarea()
     * is deliberately not used since it would flag the shared textarea focused,
     * which the T9 keypad never did. Popovers stay at LVGL's default (off). */
    lv_obj_remove_event_cb(kb, lv_keyboard_def_event_cb);
    lv_obj_add_event_cb(kb, text_entry_qwerty_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(kb, text_entry_variant_release_cb, LV_EVENT_RELEASED, NULL);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_LOWER, qwerty_map_lower, qwerty_ctrl_letters);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_UPPER, qwerty_map_upper, qwerty_ctrl_letters);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_SPECIAL, qwerty_map_special, qwerty_ctrl_wide);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_USER_1, qwerty_map_symbols, qwerty_ctrl_wide);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_USER_2, qwerty_map_accents_lower, qwerty_ctrl_accents);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_USER_3, qwerty_map_accents_upper, qwerty_ctrl_accents);
    lv_obj_add_event_cb(kb, text_entry_qwerty_long_press_cb, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_event_cb(kb, text_entry_variant_cancel_event_cb, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_add_event_cb(kb, text_entry_variant_cancel_event_cb, LV_EVENT_GESTURE, NULL);

    lv_obj_remove_flag(kb, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(kb, lv_pct(100), TEXT_ENTRY_GRID_HEIGHT + TEXT_ENTRY_BOTTOM_MARGIN + 8);
    lv_obj_align(kb, LV_ALIGN_TOP_LEFT, 0, TEXT_ENTRY_GRID_Y - 8);

    lv_obj_add_style(kb, &style_theme_screen_bg, 0);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(kb, 0, 0);
    lv_obj_set_style_radius(kb, 0, 0);
    lv_obj_set_style_pad_top(kb, 8, 0);
    lv_obj_set_style_pad_bottom(kb, TEXT_ENTRY_BOTTOM_MARGIN, 0);
    lv_obj_set_style_pad_left(kb, BOARD_SCALE_PX(4), 0);
    lv_obj_set_style_pad_right(kb, BOARD_SCALE_PX(4), 0);
    lv_obj_set_style_pad_row(kb, TEXT_ENTRY_KEY_GAP, 0);
    lv_obj_set_style_pad_column(kb, TEXT_ENTRY_KEY_GAP, 0);

    /* LVGL's default keyboard theme styles the CHECKED state (grey bg) and the
     * pressed recolor, so the selectors used here are each given the full set
     * of properties they need rather than inheriting from the default state. */
    lv_obj_add_style(kb, &style_theme_card_bg, LV_PART_ITEMS);
    lv_obj_add_style(kb, &style_theme_text_primary, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_radius(kb, BOARD_SCALE_PX(12), LV_PART_ITEMS);
    lv_obj_set_style_border_width(kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_text_font(kb, gui_theme_font(GUI_FONT_ROLE_ROW), LV_PART_ITEMS);

    lv_obj_add_style(kb, &list_row_pressed_style, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_add_style(kb, gui_theme_accent_outline_style(), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_border_width(kb, BOARD_SCALE_PX(2), LV_PART_ITEMS | LV_STATE_PRESSED);

    /* Enter and a caps-locked Shift: panel background, accent outline. The
     * accent outline style is transparent and gui_theme_accent_style() would
     * recolor the label too, hence the explicit opaque panel bg here. */
    lv_obj_add_style(kb, &style_theme_card_bg, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_add_style(kb, gui_theme_accent_outline_style(), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_add_style(kb, &style_theme_text_primary, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_width(kb, BOARD_SCALE_PX(3), LV_PART_ITEMS | LV_STATE_CHECKED);

    lv_obj_t * overlay = lv_obj_create(parent);
    text_entry_variant_overlay = overlay;
    lv_obj_set_size(overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_pad_all(overlay, 0, 0);
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(overlay, text_entry_variant_overlay_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(overlay, text_entry_variant_overlay_cb, LV_EVENT_DELETE, NULL);
    lv_obj_add_event_cb(overlay, text_entry_variant_cancel_event_cb, LV_EVENT_GESTURE, NULL);

    lv_obj_t * panel = lv_obj_create(overlay);
    text_entry_variant_panel = panel;
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_add_style(panel, &style_theme_card_bg, 0);
    lv_obj_set_style_border_width(panel, BOARD_SCALE_PX(2), 0);
    lv_obj_add_style(panel, gui_theme_accent_outline_style(), 0);
    lv_obj_set_style_radius(panel, BOARD_SCALE_PX(10), 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    for (size_t i = 0; i < 8; i++) {
        lv_obj_t * button = lv_obj_create(panel);
        text_entry_variant_buttons[i] = button;
        lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
        lv_obj_add_style(button, &style_theme_card_bg, 0);
        lv_obj_add_style(button, &list_row_pressed_style, LV_STATE_PRESSED);
        lv_obj_set_style_radius(button, BOARD_SCALE_PX(8), 0);
        lv_obj_set_style_border_width(button, 0, 0);
        lv_obj_set_style_pad_all(button, 0, 0);
        lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_PRESS_LOCK);
        lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(button, text_entry_variant_select_cb, LV_EVENT_CLICKED, (void *) (uintptr_t) i);
        lv_obj_t * label = lv_label_create(button);
        lv_obj_add_style(label, &style_theme_text_primary, 0);
        lv_obj_set_style_text_font(label, lv_obj_get_style_text_font(kb, LV_PART_ITEMS), 0);
        lv_obj_center(label);
    }
    lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
    return kb;
}

/* T9 keypad: 5 columns x 4 rows. Row 0: Mode:123 / 1 / 2-ABC / 3-DEF / Del.
 * Row 1: Mode:ABC / 4-GHI / 5-JKL / 6-MNO / 0-or-Shift. Row 2: Mode:sym / 7-PQRS /
 * 8-TUV / 9-WXYZ / Enter (spans rows 2-3). Row 3: Left / Right / Space (spans
 * cols 2-3) / Enter continues. Numeric-only fields hide the three mode buttons
 * and show a decimal key (same cell as Mode:123) and a minus key (same cell as
 * Mode:ABC) -- see text_entry_refresh_keys().
 *
 * Every key is built as a child of one `t9_pad` container, and that and the
 * QWERTY keyboard (see build_qwerty_keyboard()) are children of one `group`
 * container (a plain full-screen-sized, invisible object at (0,0)) rather
 * than directly on `parent`, so the whole keypad can be reparented in one
 * lv_obj_set_parent(group, ...) call -- see t9_keypad_attach()/
 * t9_keypad_release() -- instead of moving each key individually. Every
 * screen this ever attaches to is the same full-screen size, so
 * TEXT_ENTRY_GRID_X/Y's absolute-pixel positioning (already fixed
 * relative to `group`'s own (0,0) origin, which `t9_pad` shares) lands
 * identically regardless of which screen currently owns it. Only one of the
 * two layouts is visible at a time, see text_entry_select_layout(). */
static lv_obj_t * text_entry_make_container(lv_obj_t * parent) {
    lv_obj_t * obj = lv_obj_create(parent);
    lv_obj_set_size(obj, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(obj, 0, 0);
    lv_obj_set_style_bg_opa(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    /* Remove padding so key coordinates align cleanly with the container. */
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

static lv_obj_t * build_t9_keypad_group(lv_obj_t * parent) {
    lv_obj_t * group = text_entry_make_container(parent);
    lv_obj_t * pad = text_entry_make_container(group);
    text_entry_t9_pad = pad;

    /* Opaque black backing sized to the keypad footprint, placed behind keys
     * so underlying screen content does not show through the key gaps. */
    lv_obj_t * backing = lv_obj_create(pad);
    lv_obj_set_size(backing, lv_pct(100), TEXT_ENTRY_GRID_HEIGHT + TEXT_ENTRY_BOTTOM_MARGIN + 8);
    lv_obj_set_pos(backing, 0, TEXT_ENTRY_GRID_Y - 8);
    lv_obj_set_style_bg_opa(backing, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(backing, lv_color_black(), 0);
    lv_obj_set_style_border_width(backing, 0, 0);
    lv_obj_set_style_radius(backing, 0, 0);
    lv_obj_remove_flag(backing, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(backing, LV_OBJ_FLAG_CLICKABLE);

    for (int i = 1; i <= 9; i++) {
        int col = 1 + (i - 1) % 3;
        int row = (i - 1) / 3;
        text_entry_key_img[i] = text_entry_make_key(pad, col, row, text_entry_key_click_cb, (void *) (intptr_t) i);
    }
    text_entry_key_img[0] = text_entry_make_key(pad, 4, 1, text_entry_key0_click_cb, NULL);

    text_entry_num_mode_key = text_entry_make_key(pad, 0, 0, text_entry_mode_num_click_cb, NULL);
    lv_image_set_src(text_entry_num_mode_key, asset_path("keyboard/num.png"));
    text_entry_abc_mode_key = text_entry_make_key(pad, 0, 1, text_entry_mode_abc_click_cb, NULL);
    lv_image_set_src(text_entry_abc_mode_key, asset_path("keyboard/char.png"));
    text_entry_sym_mode_key = text_entry_make_key(pad, 0, 2, text_entry_mode_sym_click_cb, NULL);
    lv_image_set_src(text_entry_sym_mode_key, asset_path("keyboard/symbol.png"));

    /* Built after the mode keys they replace so they sit on top when shown. */
    text_entry_dotneg_key = text_entry_make_key(pad, 0, 0, text_entry_key_click_cb, (void *) (intptr_t) 10);
    lv_image_set_src(text_entry_dotneg_key, asset_path("keyboard/dot.png"));
    lv_obj_add_flag(text_entry_dotneg_key, LV_OBJ_FLAG_HIDDEN);

    text_entry_neg_key = lv_obj_create(pad);
    lv_obj_set_size(text_entry_neg_key, TEXT_ENTRY_KEY_SIZE, TEXT_ENTRY_KEY_SIZE);
    lv_obj_align(text_entry_neg_key, LV_ALIGN_TOP_LEFT,
                 TEXT_ENTRY_GRID_X + 0 * (TEXT_ENTRY_KEY_SIZE + TEXT_ENTRY_KEY_GAP),
                 TEXT_ENTRY_GRID_Y + 1 * (TEXT_ENTRY_KEY_SIZE + TEXT_ENTRY_KEY_GAP));
    lv_obj_set_style_bg_opa(text_entry_neg_key, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(text_entry_neg_key, lv_color_make(48, 48, 48), 0);
    lv_obj_set_style_border_width(text_entry_neg_key, 0, 0);
    lv_obj_set_style_radius(text_entry_neg_key, 8, 0);
    lv_obj_set_style_pad_all(text_entry_neg_key, 0, 0);
    lv_obj_remove_flag(text_entry_neg_key, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(text_entry_neg_key, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(text_entry_neg_key, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(text_entry_neg_key, text_entry_neg_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * neg_label = lv_label_create(text_entry_neg_key);
    lv_label_set_text(neg_label, "-");
    lv_obj_set_style_text_color(neg_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(neg_label, gui_theme_font(GUI_FONT_ROLE_TITLE), 0);
    lv_obj_remove_flag(neg_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(neg_label);

    lv_obj_t * del_key = text_entry_make_key(pad, 4, 0, text_entry_del_click_cb, NULL);
    lv_image_set_src(del_key, asset_path("keyboard/del.png"));
    lv_obj_t * left_key = text_entry_make_key(pad, 0, 3, text_entry_left_click_cb, NULL);
    lv_image_set_src(left_key, asset_path("keyboard/left.png"));
    lv_obj_t * right_key = text_entry_make_key(pad, 1, 3, text_entry_right_click_cb, NULL);
    lv_image_set_src(right_key, asset_path("keyboard/right.png"));
    lv_obj_t * enter_key = text_entry_make_key(pad, 4, 2, text_entry_enter_click_cb, NULL);
    lv_image_set_src(enter_key, asset_path("keyboard/enter.png"));
    /* Match the last row's actual image bounds, including board-specific
     * row spacing. Stock enter.png is 190px tall versus 188px for the R1
     * two-row footprint; stretching preserves its rounded bottom edge. */
    lv_obj_update_layout(right_key);
    lv_obj_set_size(enter_key, lv_obj_get_width(right_key),
                    TEXT_ENTRY_KEY_SIZE + TEXT_ENTRY_KEY_GAP + lv_obj_get_height(right_key));
    lv_image_set_inner_align(enter_key, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_t * space_key = text_entry_make_key(pad, 2, 3, text_entry_space_click_cb, NULL);
    lv_image_set_src(space_key, asset_path("keyboard/space2.png"));

    text_entry_multitap_timer = lv_timer_create(text_entry_multitap_timeout_cb, TEXT_ENTRY_MULTITAP_MS, NULL);
    lv_timer_pause(text_entry_multitap_timer);

    text_entry_qwerty_kb = build_qwerty_keyboard(group);
    lv_obj_add_flag(text_entry_qwerty_kb, LV_OBJ_FLAG_HIDDEN);

    /* Lets a swipe started on the keypad (inline search's Enter key sits
     * right where a right-swipe could plausibly start) still bubble up to
     * the target screen's own back-gesture handler -- built once here, so
     * it persists across every future t9_keypad_attach() reparenting
     * (flags are object properties, not derived from the parent). */
    lv_obj_add_flag(group, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(group, text_entry_variant_cancel_event_cb, LV_EVENT_GESTURE, NULL);
    enable_gesture_bubble_recursive(group);

    return group;
}

static bool text_entry_use_qwerty(void) {
    return !text_entry_numeric_only && text_entry_keyboard_layout != KEYBOARD_LAYOUT_T9;
}

/* Shows the T9 pad or the QWERTY keyboard for a fresh session, resetting
 * QWERTY to the lowercase letters page. Setting changes therefore apply the
 * next time a field or inline search opens; a keypad currently lent to a
 * library screen keeps its layout until that search session ends. Numeric
 * fields always get the T9 pad. */
static void text_entry_select_layout(void) {
    text_entry_variant_close();
    text_entry_keyboard_layout = current_settings.keyboard_layout;
    if (text_entry_keyboard_layout == KEYBOARD_LAYOUT_LEGACY_4 ||
        text_entry_keyboard_layout == KEYBOARD_LAYOUT_LEGACY_5 ||
        text_entry_keyboard_layout == KEYBOARD_LAYOUT_LEGACY_6 ||
        text_entry_keyboard_layout < KEYBOARD_LAYOUT_T9 || text_entry_keyboard_layout > KEYBOARD_LAYOUT_UKRAINIAN)
        text_entry_keyboard_layout = KEYBOARD_LAYOUT_QWERTY;
    if (text_entry_use_qwerty()) {
        lv_obj_add_flag(text_entry_t9_pad, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(text_entry_qwerty_kb, LV_OBJ_FLAG_HIDDEN);
        text_entry_qwerty_latin_mode = false;
        text_entry_qwerty_apply_layout(false);
        text_entry_qwerty_show(LV_KEYBOARD_MODE_TEXT_LOWER);
    } else {
        lv_obj_remove_flag(text_entry_t9_pad, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(text_entry_qwerty_kb, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Reparents the shared keypad_group + text_entry_textarea onto
 * target_screen for inline search -- see text_entry_inline_mode_active's
 * own comment. Resets layout/mode/shift/caps-lock/numeric state. clear_text
 * is true for a new search and false when resuming a dismissed search so its
 * query and result filter remain intact. Search never needs
 * the password reveal button -- it stays on text_entry_screen, harmless
 * since that screen isn't the active one while this is attached elsewhere. */
void t9_keypad_attach(lv_obj_t * target_screen, lv_obj_t * textarea_parent, int32_t textarea_x, int32_t textarea_y,
                              int32_t textarea_w, bool clear_text) {
    text_entry_variant_close();
    text_entry_variant_set_owner(target_screen);
    lv_obj_set_parent(text_entry_keypad_group, target_screen);
    lv_obj_set_parent(text_entry_textarea, textarea_parent);
    /* Align with LV_ALIGN_TOP_LEFT so textarea_x and textarea_y are treated as
     * absolute coordinates relative to the parent container. */
    lv_obj_align(text_entry_textarea, LV_ALIGN_TOP_LEFT, textarea_x, textarea_y);
    lv_obj_set_width(text_entry_textarea, textarea_w);
    lv_textarea_set_password_mode(text_entry_textarea, false);
    if (clear_text) lv_textarea_set_text(text_entry_textarea, "");

    text_entry_kp_mode = TEXT_ENTRY_KP_ABC;
    text_entry_shift = false;
    text_entry_caps_lock = false;
    text_entry_numeric_only = false;
    text_entry_select_layout();
    text_entry_finalize_pending();
    text_entry_refresh_keys();
    text_entry_generation++;
    text_entry_inline_mode_active = true;
}

/* Derives textarea resting height dynamically from the font line height plus padding. */
static int32_t text_entry_field_height(void) {
    return lv_font_get_line_height(gui_theme_font(GUI_FONT_ROLE_BODY)) + 26;
}

/* Returns keypad_group + text_entry_textarea to text_entry_screen, their
 * resting parent when neither the modal flow nor inline search is using
 * them -- called both when inline search fully closes and defensively at
 * the start of show_text_entry() (see its own comment) so opening the
 * modal always reclaims them correctly regardless of which screen
 * currently holds them. */
void t9_keypad_release(void) {
    text_entry_variant_close();
    text_entry_variant_set_owner(text_entry_screen);
    lv_obj_set_parent(text_entry_keypad_group, text_entry_screen);
    lv_obj_set_parent(text_entry_textarea, text_entry_screen);
    lv_obj_set_size(text_entry_textarea, lv_pct(78), text_entry_field_height());
    lv_obj_align(text_entry_textarea, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + 8);
    text_entry_inline_mode_active = false;
}

static void text_entry_back_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    text_entry_variant_close();
    if (text_entry_on_done == plugin_text_entry_done_cb) plugin_manager_text_input_cancelled();
    generic_back_cb(e);
}

static lv_obj_t * build_text_entry_screen(void) {
    lv_obj_t * scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &style_theme_screen_bg, 0);

    /* Top-left back button navigating back via text_entry_back_cb. */
    text_entry_title_label = build_screen_header(scr, "", text_entry_back_cb, NULL, NULL);

    text_entry_textarea = lv_textarea_create(scr);
    /* Configure font based on active theme role to respect user font settings. */
    lv_obj_set_style_text_font(text_entry_textarea, gui_theme_font(GUI_FONT_ROLE_BODY), 0);
    lv_obj_set_size(text_entry_textarea, lv_pct(78), text_entry_field_height());
    lv_obj_align(text_entry_textarea, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + 8);
    lv_textarea_set_one_line(text_entry_textarea, true);
    /* Built once here, so it persists across every future t9_keypad_attach()
     * reparenting onto a library screen's own search bar -- lets a swipe
     * started on the textarea itself still bubble up to that screen's
     * back-gesture handler (see search_close_if_active_for_screen()). */
    lv_obj_add_flag(text_entry_textarea, LV_OBJ_FLAG_GESTURE_BUBBLE);
    /* Fires on every keystroke (lv_textarea's own native behavior --
     * nothing listened for this before inline search existed, since every
     * other text_entry_textarea use is one-shot-on-Enter via
     * text_entry_commit()). search_textarea_value_changed_cb() itself
     * no-ops unless text_entry_inline_mode_active is set, so this is inert
     * for all 9 existing modal callers. */
    lv_obj_add_event_cb(text_entry_textarea, search_textarea_value_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    /* After Enter dismisses the shared keypad, the query field stays in the
     * search bar. Let the owning search binding reclaim the keypad on tap. */
    lv_obj_add_event_cb(text_entry_textarea, search_textarea_clicked_cb, LV_EVENT_CLICKED, NULL);

    /* Password unmask toggle button, positioned adjacent to the textarea. */
    text_entry_reveal_btn = lv_image_create(scr);
    lv_image_set_src(text_entry_reveal_btn, asset_path("keyboard/psk_show.png"));
    lv_obj_align_to(text_entry_reveal_btn, text_entry_textarea, LV_ALIGN_OUT_RIGHT_MID, 8, 0);
    lv_obj_add_flag(text_entry_reveal_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(text_entry_reveal_btn, text_entry_reveal_btn_cb, LV_EVENT_CLICKED, NULL);

    text_entry_keypad_group = build_t9_keypad_group(scr);

    return scr;
}

/* Pushes the text entry screen pre-filled with initial_text (may be NULL
 * for empty) under `title`; on_done fires with whatever was typed when the
 * T9 keypad's Enter key is pressed (not called at all if the screen's own
 * back button is used instead -- callers shouldn't assume it always fires).
 * is_password masks the input the same way a real password field would.
 * numeric locks the keypad to plain-digit entry (see text_entry_numeric_
 * only's own comment) instead of the normal ABC/NUM/SYM T9 cycle --
 * everything peq.c's freq/gain/Q fields need (digits, a decimal point, and
 * a dedicated minus key that toggles the leading sign), nothing else. */
void show_text_entry(const char * title, const char * initial_text, bool is_password, bool numeric,
                             text_entry_done_cb_t on_done, void * user_data) {
    if (text_entry_on_done == plugin_text_entry_done_cb && on_done != plugin_text_entry_done_cb) {
        plugin_manager_text_input_cancelled();
    }
    /* Defensive reclaim -- if inline search happened to be holding the
     * shared keypad/textarea when some other flow (e.g. a Wi-Fi password
     * prompt) opens the modal, this puts them back on text_entry_screen
     * (and restores the textarea's modal position/size) before anything
     * below touches them. Harmless no-op when they're already there. */
    t9_keypad_release();

    lv_label_set_text(text_entry_title_label, title);
    lv_textarea_set_text(text_entry_textarea, initial_text ? initial_text : "");
    lv_textarea_set_password_mode(text_entry_textarea, is_password);
    /* Always reset to masked/hidden -- otherwise a field left revealed by
     * the previous show_text_entry() call (e.g. Wi-Fi's chained SSID ->
     * password, which reuses this same singleton screen without an
     * intervening rebuild) would carry that state into an unrelated field. */
    lv_image_set_src(text_entry_reveal_btn, asset_path("keyboard/psk_show.png"));
    if (is_password) {
        lv_obj_remove_flag(text_entry_reveal_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(text_entry_reveal_btn, LV_OBJ_FLAG_HIDDEN);
    }
    text_entry_kp_mode = TEXT_ENTRY_KP_ABC;
    text_entry_shift = false;
    text_entry_caps_lock = false;
    text_entry_numeric_only = numeric;
    text_entry_select_layout();
    text_entry_finalize_pending();
    text_entry_refresh_keys();
    text_entry_on_done = on_done;
    text_entry_user_data = user_data;
    text_entry_generation++;
    nav_push(text_entry_screen);
}

bool gui_text_input_init(void) {
    text_entry_screen = build_text_entry_screen();
    return true;
}

/* For gui_reload.c's in-process UI reload -- deletes the text-entry screen
 * so gui_text_input_init() can rebuild it from a clean slate. Covers
 * text_entry_keypad_group too even when it's currently reparented onto some
 * other screen's inline search (t9_keypad_attach()) -- that other screen is
 * being torn down in the same reload pass regardless. */
void gui_text_input_teardown(void) {
    text_entry_variant_close();
    text_entry_variant_set_owner(NULL);
    text_entry_variant_overlay = NULL;
    text_entry_variant_panel = NULL;
    memset(text_entry_variant_buttons, 0, sizeof(text_entry_variant_buttons));
    text_entry_variant_swallow_release = false;
    /* Unguarded lv_timer_create() at this screen's own build site -- same
     * leaked-old-timer hazard as gui_player_teardown()'s volume_popup_hide_
     * timer, see its own comment. Checked independently of text_entry_screen
     * below rather than under one shared early-return, so it's still
     * cleaned up even if the two ever get out of sync. */
    if (text_entry_multitap_timer) { lv_timer_del(text_entry_multitap_timer); text_entry_multitap_timer = NULL; }
    if (text_entry_screen) { lv_obj_delete(text_entry_screen); text_entry_screen = NULL; }
    text_entry_t9_pad = NULL;
    text_entry_qwerty_kb = NULL;
}

lv_obj_t * gui_text_input_get_screen(void) {
    return text_entry_screen;
}

void gui_text_input_cancel_if_removed(lv_obj_t * removed_screen) {
    if (removed_screen == text_entry_screen) text_entry_variant_close();
    if (removed_screen == text_entry_screen &&
        text_entry_on_done == plugin_text_entry_done_cb) {
        plugin_manager_text_input_cancelled();
    }
}

void t9_keypad_dismiss_only(void) {
    text_entry_variant_close();
    text_entry_variant_set_owner(text_entry_screen);
    /* The inline-search list was shortened to leave room for the keypad.
     * Restore it before moving the shared keypad off the active screen. */
    search_keypad_dismissed();
    lv_obj_set_parent(text_entry_keypad_group, text_entry_screen);
    text_entry_inline_mode_active = false;
}

int32_t t9_keypad_get_grid_y(void) {
    return TEXT_ENTRY_GRID_Y;
}

bool t9_keypad_is_inline_active(void) {
    return text_entry_inline_mode_active;
}

const char * t9_keypad_get_text(void) {
    return lv_textarea_get_text(text_entry_textarea);
}
