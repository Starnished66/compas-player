#include "gui_file_manager.h"
#include "file_browser.h"
#include "file_ops.h"
#include "gui.h"
#include "gui_navigation.h"
#include "gui_notifications.h"
#include "gui_text_input.h"
#include "gui_text_view.h"
#include "i18n.h"
#include "assets.h"
#include "screen_builders.h"
#include "usb_mode_control.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#ifdef HOST_BUILD
  #define FILE_MANAGER_ROOT "./music"
#else
  #define FILE_MANAGER_ROOT "/data/mnt/sd_0"
#endif

/* Playback entry points shared with Music > Files. */
extern void on_file_browser_selected(char ** new_playlist, int count, int selected_index);
extern void on_file_browser_index_selected(file_browser_index_t * index, unsigned playable_count,
                                           unsigned selected_playable);
extern void on_cue_file_selected(const char * cue_path);

static lv_obj_t * screen;
static lv_obj_t * title_label;
static file_browser_t * browser;

static gui_popup_t menu_popup;
/* Confirms Delete, and Move of the player's own folders. */
static gui_popup_t confirm_popup;
static lv_obj_t * confirm_title;
static lv_obj_t * confirm_body;
static lv_obj_t * confirm_label;
static bool confirm_is_move;
static gui_popup_t progress_popup;
static lv_obj_t * progress_body;
static lv_timer_t * progress_timer;

/* Copied or cut entries waiting for "Paste here". */
static char ** clipboard;
static int clipboard_count;
static file_op_kind_t clipboard_kind;

/* The entry being renamed. */
static char rename_path[PATH_MAX];
/* Stop was tapped; the worker finishes its current file first. */
static bool stopping;
/* file_op_status_t.id of the operation started here. Plugins share the
 * engine, and their operations are not shown or stopped from this screen. */
static unsigned own_op_id;

static void free_paths(char ** paths, int count) {
    for (int i = 0; i < count; i++) free(paths[i]);
    free(paths);
}

static void clear_clipboard(void) {
    free_paths(clipboard, clipboard_count);
    clipboard = NULL;
    clipboard_count = 0;
}

static const char * name_of(const char * path) {
    const char * slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/* A name for a new or renamed entry: not empty, no path separator. */
static bool valid_name(const char * name) {
    return name[0] && !strchr(name, '/') && strcmp(name, ".") != 0 && strcmp(name, "..") != 0;
}

/* A PC with the card mounted over USB storage must be the only writer. */
static bool storage_writable(void) {
    if (!usb_mode_control_storage_write_blocked()) return true;
    show_error_toast(TR("Disconnect USB storage from the computer before changing files."));
    return false;
}

static void update_title(void) {
    if (!title_label) return;
    if (!file_browser_is_selecting(browser)) {
        lv_label_set_text(title_label, TR("File Manager"));
        return;
    }
    int count = (int) file_browser_selected_count(browser);
    lv_label_set_text_fmt(title_label, TR_N("%d selected", "%d selected", count), count);
}

static void selection_changed_cb(file_browser_t * fb) {
    (void) fb;
    update_title();
}

/* ---- Opening files that are not music ---- */

static bool is_text_name(const char * name) {
    static const char * const extensions[] = {
        ".txt", ".log", ".md", ".lrc", ".json", ".ini", ".cfg", ".conf", ".csv", ".xml", ".nfo", ".srt",
    };
    const char * ext = strrchr(name, '.');
    if (!ext) return false;
    for (size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); i++)
        if (strcasecmp(ext, extensions[i]) == 0) return true;
    return false;
}

/* Text opens in the shared text viewer, which shows at most its own limit. */
static void open_text_file(const char * path) {
    FILE * f = fopen(path, "rb");
    char * text = f ? malloc(GUI_TEXT_VIEW_MAX_BYTES) : NULL;
    size_t length = text ? fread(text, 1, GUI_TEXT_VIEW_MAX_BYTES, f) : 0;
    if (f) fclose(f);
    if (!text || !gui_text_view_show(name_of(path), text, length, NULL, NULL, NULL, NULL))
        show_error_toast(TR("Could not open this file"));
    free(text);
}

static void open_other_cb(const char * path) {
    if (is_text_name(path)) open_text_file(path);
    else show_info_toast(TR("This type of file cannot be opened"));
}

/* ---- File operations ---- */

static void progress_timer_cb(lv_timer_t * timer) {
    (void) timer;
    file_op_status_t status;
    file_op_get_status(&status);
    bool own = status.id == own_op_id;
    if (own && status.running) {
        if (stopping) return;
        unsigned item = status.done < status.total ? status.done + 1 : status.total;
        lv_label_set_text_fmt(progress_body, TR("%u of %u\n%s"), item, status.total, status.current);
        return;
    }
    lv_timer_delete(progress_timer);
    progress_timer = NULL;
    gui_popup_hide(&progress_popup);
    file_browser_refresh_all();
    /* A plugin's operation replaced the result before this tick saw it. */
    if (!own) return;
    if (status.storage_busy) {
        show_error_toast(TR("Disconnect USB storage from the computer before changing files."));
    } else if (status.stopped) {
        show_info_toast(TR("Stopped"));
    } else if (status.failed) {
        char message[160];
        snprintf(message, sizeof(message),
                 TR_N("%u item could not be completed", "%u items could not be completed", status.failed),
                 status.failed);
        show_error_toast(message);
    } else if (status.touched_library) {
        show_info_toast(TR("Done. Update the music database to see the changes in your library."));
    } else {
        show_info_toast(TR("Done"));
    }
}

static void progress_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    file_op_status_t status;
    file_op_get_status(&status);
    if (!status.running || status.id != own_op_id) return;
    file_op_cancel();
    stopping = true;
    lv_label_set_text(progress_body, TR("Stopping..."));
}

/* Shows progress until the running operation ends, then reports it. */
static void show_progress(void) {
    if (!progress_popup.popup) {
        lv_obj_t * cancel_row = NULL;
        progress_popup.popup = build_confirm_popup_with_labels(
            TR("Working..."), LV_LABEL_LONG_WRAP, NULL, "", &progress_body, TR("Stop"), NULL,
            accent_lv_color(), progress_cancel_cb, NULL, TR("Cancel"), accent_lv_color(), progress_cancel_cb,
            &cancel_row, NULL, &progress_popup.backdrop);
        /* Stop is the only action; the popup closes when the operation ends. */
        if (cancel_row) lv_obj_add_flag(cancel_row, LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(progress_body, stopping ? TR("Stopping...") : "");
    gui_popup_show(&progress_popup);
    progress_timer = lv_timer_create(progress_timer_cb, 200, NULL);
}

static bool start_operation(file_op_kind_t kind, char * const * paths, int count, const char * dest_dir) {
    if (!storage_writable()) return false;
    if (!file_op_start(kind, paths, count, dest_dir)) {
        show_error_toast(TR("Another file operation is still running"));
        return false;
    }
    file_op_status_t status;
    file_op_get_status(&status);
    own_op_id = status.id;
    stopping = false;
    show_progress();
    return true;
}

static void copy_or_move_selection(file_op_kind_t kind) {
    char ** paths;
    int count;
    if (!file_browser_selected_paths(browser, &paths, &count)) return;
    clear_clipboard();
    clipboard = paths;
    clipboard_count = count;
    clipboard_kind = kind;
    file_browser_set_selecting(browser, false);
    show_info_toast(TR("Open the destination folder, then choose Paste here from the menu."));
}

static void paste_here(void) {
    if (start_operation(clipboard_kind, clipboard, clipboard_count, file_browser_current_dir(browser)))
        clear_clipboard();
}

static bool selection_has_player_folder(void) {
    char ** paths;
    int count;
    if (!file_browser_selected_paths(browser, &paths, &count)) return false;
    bool found = false;
    for (int i = 0; i < count && !found; i++) found = file_op_is_player_folder(paths[i], FILE_MANAGER_ROOT);
    free_paths(paths, count);
    return found;
}

static void confirm_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_popup_hide(&confirm_popup);
    if (confirm_is_move) {
        copy_or_move_selection(FILE_OP_MOVE);
        return;
    }
    char ** paths;
    int count;
    if (!file_browser_selected_paths(browser, &paths, &count)) return;
    start_operation(FILE_OP_DELETE, paths, count, NULL);
    free_paths(paths, count);
}

static void confirm_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_popup_hide(&confirm_popup);
}

/* player_folder: the selection includes .compas or .plugins. */
static void ask_confirm(bool move, bool player_folder) {
    if (!confirm_popup.popup) {
        confirm_popup.popup = build_confirm_popup_with_labels(
            "", LV_LABEL_LONG_WRAP, &confirm_title, "", &confirm_body, "", &confirm_label,
            accent_lv_color(), confirm_cb, NULL, TR("Cancel"), accent_lv_color(), confirm_cancel_cb,
            NULL, confirm_cancel_cb, &confirm_popup.backdrop);
    }
    confirm_is_move = move;
    int count = (int) file_browser_selected_count(browser);
    if (move) lv_label_set_text_fmt(confirm_title, TR_N("Move %d item?", "Move %d items?", count), count);
    else lv_label_set_text_fmt(confirm_title, TR_N("Delete %d item?", "Delete %d items?", count), count);
    lv_label_set_text(confirm_label, move ? TR("Move") : TR("Delete"));
    lv_label_set_text(confirm_body, player_folder
        ? TR("This includes the player's own data (.compas or .plugins). Deleting or moving it can break "
             "features, lose data such as settings and installed plugins, and may require a restart.")
        : TR("This cannot be undone."));
    gui_popup_show(&confirm_popup);
}

static void rename_done_cb(const char * text, void * user_data) {
    (void) user_data;
    if (!valid_name(text)) {
        if (text[0]) show_error_toast(TR("That name cannot be used"));
        return;
    }
    if (strcmp(text, name_of(rename_path)) == 0 || !storage_writable()) return;
    char target[PATH_MAX];
    int n = snprintf(target, sizeof(target), "%.*s/%s", (int) (name_of(rename_path) - rename_path - 1),
                     rename_path, text);
    struct stat source, existing;
    if (n <= 0 || (size_t) n >= sizeof(target) || lstat(rename_path, &source) != 0) {
        show_error_toast(TR("Could not rename"));
        return;
    }
    /* On the FAT card a change of letter case finds the item itself. */
    bool clash = lstat(target, &existing) == 0 &&
                 (existing.st_dev != source.st_dev || existing.st_ino != source.st_ino);
    if (clash) show_error_toast(TR("An item with that name already exists"));
    else if (rename(rename_path, target) != 0) show_error_toast(TR("Could not rename"));
    else {
        file_browser_refresh_all();
        if (S_ISDIR(source.st_mode) || file_browser_is_playable_name(rename_path))
            show_info_toast(TR("Done. Update the music database to see the changes in your library."));
    }
}

static void ask_rename(void) {
    char ** paths;
    int count;
    if (!file_browser_selected_paths(browser, &paths, &count)) return;
    snprintf(rename_path, sizeof(rename_path), "%s", paths[0]);
    free_paths(paths, count);
    show_text_entry(TR("Rename"), name_of(rename_path), false, false, rename_done_cb, NULL);
}

static void new_folder_done_cb(const char * text, void * user_data) {
    (void) user_data;
    if (!browser) return;
    if (!valid_name(text)) {
        if (text[0]) show_error_toast(TR("That name cannot be used"));
        return;
    }
    if (!storage_writable()) return;
    char path[PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/%s", file_browser_current_dir(browser), text);
    if (n <= 0 || (size_t) n >= sizeof(path)) show_error_toast(TR("That name cannot be used"));
    else if (mkdir(path, 0777) != 0)
        show_error_toast(errno == EEXIST ? TR("An item with that name already exists") : TR("Could not create the folder"));
    else file_browser_refresh(browser);
}

/* ---- Header menu ---- */

enum menu_action {
    MENU_PASTE, MENU_SELECT, MENU_NEW_FOLDER, MENU_TOGGLE_HIDDEN,
    MENU_COPY, MENU_MOVE, MENU_RENAME, MENU_DELETE, MENU_SELECT_ALL, MENU_CANCEL,
};

static void run_menu_action(enum menu_action action) {
    switch (action) {
        case MENU_PASTE: paste_here(); break;
        case MENU_SELECT:
            file_browser_set_selecting(browser, true);
            if (!file_browser_is_selecting(browser)) show_info_toast(TR("Nothing to select here"));
            break;
        case MENU_NEW_FOLDER: show_text_entry(TR("New folder"), "", false, false, new_folder_done_cb, NULL); break;
        case MENU_TOGGLE_HIDDEN: file_browser_set_show_hidden(browser, !file_browser_shows_hidden(browser)); break;
        case MENU_COPY: copy_or_move_selection(FILE_OP_COPY); break;
        case MENU_MOVE:
            if (selection_has_player_folder()) ask_confirm(true, true);
            else copy_or_move_selection(FILE_OP_MOVE);
            break;
        case MENU_RENAME: ask_rename(); break;
        case MENU_DELETE: ask_confirm(false, selection_has_player_folder()); break;
        case MENU_SELECT_ALL: file_browser_select_all(browser); break;
        case MENU_CANCEL: break;
    }
}

/* Action of each menu row, by row position (build_menu_popup() gives every
 * row the same callback data). */
#define MENU_MAX_ROWS 8
static enum menu_action menu_actions[MENU_MAX_ROWS];

static void menu_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    uint32_t row = lv_obj_get_index(lv_event_get_current_target_obj(e));
    gui_popup_hide(&menu_popup);
    if (row < MENU_MAX_ROWS) run_menu_action(menu_actions[row]);
}

static void menu_backdrop_cb(lv_event_t * e) {
    (void) e;
    gui_popup_hide(&menu_popup);
}

static void add_menu_row(menu_popup_row_t * rows, int * count, const char * label, enum menu_action action) {
    rows[*count] = (menu_popup_row_t) { label, menu_row_cb, action == MENU_DELETE, action == MENU_CANCEL };
    menu_actions[(*count)++] = action;
}

/* The rows depend on the current state, so the menu is rebuilt each time. */
static void menu_button_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || progress_timer) return;
    static char paste_label[96];
    menu_popup_row_t rows[MENU_MAX_ROWS];
    int count = 0;
    if (file_browser_is_selecting(browser)) {
        unsigned selected = file_browser_selected_count(browser);
        if (selected) {
            add_menu_row(rows, &count, TR("Copy"), MENU_COPY);
            add_menu_row(rows, &count, TR("Move"), MENU_MOVE);
            if (selected == 1) add_menu_row(rows, &count, TR("Rename"), MENU_RENAME);
            add_menu_row(rows, &count, TR("Delete"), MENU_DELETE);
        }
        add_menu_row(rows, &count, TR("Select all"), MENU_SELECT_ALL);
    } else {
        if (clipboard_count) {
            snprintf(paste_label, sizeof(paste_label),
                     TR_N("Paste %d item here", "Paste %d items here", clipboard_count), clipboard_count);
            add_menu_row(rows, &count, paste_label, MENU_PASTE);
        }
        add_menu_row(rows, &count, TR("Select"), MENU_SELECT);
        add_menu_row(rows, &count, TR("New folder"), MENU_NEW_FOLDER);
        add_menu_row(rows, &count, file_browser_shows_hidden(browser) ? TR("Hide hidden files")
                                                                     : TR("Show hidden files"),
                     MENU_TOGGLE_HIDDEN);
    }
    add_menu_row(rows, &count, TR("Cancel"), MENU_CANCEL);
    gui_popup_teardown(&menu_popup);
    menu_popup.popup = build_menu_popup(rows, count, menu_backdrop_cb, &menu_popup.backdrop);
    gui_popup_show(&menu_popup);
}

/* ---- Screen ---- */

/* The header back ends selection mode first; the back swipe does the same
 * through file_browser_go_up_for_screen(). */
static void back_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (file_browser_is_selecting(browser)) file_browser_set_selecting(browser, false);
    else nav_pop();
}

static void screen_delete_cb(lv_event_t * e) {
    (void) e;
    screen = NULL;
    title_label = NULL;
    browser = NULL;
}

static void build_screen(void) {
    screen = lv_obj_create(NULL);
    lv_obj_add_style(screen, &style_theme_screen_bg, 0);
    lv_obj_add_event_cb(screen, screen_delete_cb, LV_EVENT_DELETE, NULL);
    title_label = build_screen_header(screen, TR("File Manager"), back_cb, asset_path("playing_plane/ic_more.png"),
                                      menu_button_cb);
    const file_browser_options_t options = {
        .on_select = on_file_browser_selected,
        .on_cue_select = on_cue_file_selected,
        .on_index_select = on_file_browser_index_selected,
        .on_open_other = open_other_cb,
        .on_selection = selection_changed_cb,
    };
    browser = file_browser_create(screen, FILE_MANAGER_ROOT, &options);
    finalize_screen_navigation(screen);
}

bool gui_file_manager_open(const char * folder) {
    if (!screen) build_screen();
    /* Under another screen, pushing it again would put it on the stack twice. */
    bool covered = lv_screen_active() != screen && gui_navigation_contains(screen);
    if (!browser || covered || (folder && !file_browser_open_dir(browser, folder))) return false;
    if (lv_screen_active() != screen) nav_push(screen);
    /* An operation outlives a UI reload; bring its progress back. */
    file_op_status_t status;
    file_op_get_status(&status);
    if (status.running && status.id == own_op_id && !progress_timer) show_progress();
    return true;
}

void gui_file_manager_tile_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    gui_file_manager_open(NULL);
}

void gui_file_manager_teardown(void) {
    if (progress_timer) {
        lv_timer_delete(progress_timer);
        progress_timer = NULL;
    }
    gui_popup_teardown(&menu_popup);
    gui_popup_teardown(&confirm_popup);
    gui_popup_teardown(&progress_popup);
    confirm_title = confirm_body = confirm_label = NULL;
    progress_body = NULL;
    if (screen) lv_obj_delete(screen);
}
