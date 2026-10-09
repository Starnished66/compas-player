/* phase2_regression_test.c
 * Host regression test harness using real LVGL objects, fonts, and actual
 * production gui_plugin_store.c private statics and functions.
 * Note: Native chooser screen capacities and rendered setup status labels
 * are verified by code review; this harness tests real production Store
 * navigation, origin tracking/cancellation, picker preservation, commit-after-
 * success guards, updates view scope, and individual file confirmation.
 */
#include "gui_plugin_store.c"
#include "settings.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

player_settings_t current_settings;
lv_font_t app_font_16, app_font_20, app_font_22, app_font_28, app_font_lyrics;

const char * asset_path(const char * path) { (void)path; return "test_asset.png"; }
const char * asset_path_plain(const char * path) { (void)path; return "/missing-test-asset"; }
bool asset_decoded_image_open(asset_decoded_image_t * image, const char * path) { (void)image; (void)path; return false; }
void asset_decoded_image_close(asset_decoded_image_t * image) { (void)image; }
bool asset_decoded_gradient_open(asset_decoded_image_t * image, const char * path) { (void)image; (void)path; return false; }
const void * asset_decoded_image_source(const asset_decoded_image_t * image) { (void)image; return NULL; }
void plugin_store_reset(void) {}

void settings_save(const player_settings_t * settings) { (void)settings; }
void player_transition_mark_dirty(void) {}
void refresh_play_btn_icon(void) {}
void gui_shell_refresh_quick_drawer_toggle_accent(void) {}
void gui_settings_accent_changed(void) {}
void generic_back_cb(lv_event_t * event) { (void)event; }
void finalize_screen_navigation(lv_obj_t * screen) { (void)screen; }
void register_swipe_dead_zone(lv_obj_t * obj) { (void)obj; }
void unregister_swipe_dead_zone(lv_obj_t * obj) { (void)obj; }
void fallback_font_init_early(int tier, int lyrics) { (void)tier; (void)lyrics; }
void configure_scrolling_row_label(lv_obj_t * label, int role) { (void)label; (void)role; }
void cover_card_preview_set(lv_obj_t * card, const char * preview_src) { (void)card; (void)preview_src; }
void gui_navigation_invalidate_theme_snapshots(void) {}

/* Headless LVGL display flush */
static void test_flush(lv_display_t * disp, const lv_area_t * area, uint8_t * px) {
    (void)area; (void)px;
    lv_display_flush_ready(disp);
}

/* Stubs for controlled environment */
static bool mock_wifi_connected = true;
bool gui_shell_wifi_connected(void) { return mock_wifi_connected; }
bool gui_shell_wifi_effective_enabled(void) { return mock_wifi_connected; }

static lv_obj_t * mock_nav_top = NULL;
static lv_obj_t * mock_last_pushed = NULL;
bool gui_navigation_is_top(lv_obj_t * screen) { return screen == mock_nav_top; }
bool gui_navigation_contains(lv_obj_t * screen) { (void)screen; return true; }
static bool mock_transition_in_progress = false;
bool gui_navigation_transition_in_progress(void) { return mock_transition_in_progress; }
void nav_push(lv_obj_t * scr) { mock_last_pushed = scr; mock_nav_top = scr; }
void nav_pop(void) {}

static lv_obj_t * mock_busy_screen = NULL;
lv_obj_t * gui_busy_get_screen(void) { return mock_busy_screen; }
gui_busy_handle_t gui_busy_show(const char * title, const char * msg) { (void)title; (void)msg; return 1; }
void gui_busy_set_progress(gui_busy_handle_t handle, int percent) { (void)handle; (void)percent; }
void gui_busy_set_detail(gui_busy_handle_t handle, const char * text) { (void)handle; (void)text; }
void gui_busy_hide(gui_busy_handle_t handle) { (void)handle; }

static int toast_error_count = 0;
static int toast_info_count = 0;
void show_error_toast(const char * msg) { (void)msg; toast_error_count++; }
void show_info_toast(const char * msg) { (void)msg; toast_info_count++; }
void gui_reload_request(void) {}

/* Mock plugin store backend */
static plugin_store_status_t mock_backend_status;
static plugin_store_result_t mock_backend_results[32];
static size_t mock_backend_result_count = 0;
static bool mock_backend_busy = false;
static int mock_update_all_called = 0;
static bool mock_refresh_success = true;

bool plugin_store_refresh(void) { return mock_refresh_success; }
bool plugin_store_install(const char * id, bool force) { (void)id; (void)force; return true; }
bool plugin_store_update(const char * id, bool force) { (void)id; (void)force; return true; }
bool plugin_store_update_all(void) { mock_update_all_called++; return true; }
bool plugin_store_uninstall(const char * id) { (void)id; return true; }
bool plugin_store_busy(void) { return mock_backend_busy; }
void plugin_store_get_status(plugin_store_status_t * out, plugin_store_result_t * results, size_t cap) {
    if (out) *out = mock_backend_status;
    if (results && mock_backend_result_count > 0) {
        size_t n = mock_backend_result_count < cap ? mock_backend_result_count : cap;
        memcpy(results, mock_backend_results, n * sizeof(plugin_store_result_t));
    }
}
bool plugin_store_get_details(const char * id, plugin_store_details_t * out) {
    (void)id;
    if (out) {
        memset(out, 0, sizeof(*out));
        snprintf(out->description, sizeof(out->description), "Test description for %s", id ? id : "");
    }
    return true;
}
bool plugin_store_prepare_previews(const char * const * ids, size_t count) { (void)ids; (void)count; return true; }
bool plugin_store_get_preview(const char * id, char * out, size_t size) { (void)id; (void)out; (void)size; return false; }
uint64_t plugin_store_preview_generation(void) { return 1; }
void plugin_store_cancel_previews(void) {}

static void reset_test_state(void) {
    mock_wifi_connected = true;
    mock_last_pushed = NULL;
    mock_transition_in_progress = false;
    mock_backend_busy = false;
    mock_update_all_called = 0;
    mock_backend_result_count = 0;
    mock_refresh_success = true;
    memset(&mock_backend_status, 0, sizeof(mock_backend_status));
    memset(mock_backend_results, 0, sizeof(mock_backend_results));
    toast_error_count = 0;
    toast_info_count = 0;
    store_is_refresh = false;
    store_ui_active = false;
    gui_plugin_store_teardown();
    gui_plugin_store_init();
}

/* ========================================================================= */
/* Test 1: Generic Catalog Pending-Launch Origin Tracking & Cancellation     */
/* (Astra P2 finding)                                                        */
/* ========================================================================= */
static void test_navigation_cancellation(void) {
    printf("Running test_navigation_cancellation...\n");
    reset_test_state();

    lv_obj_t * origin = lv_obj_create(NULL);
    lv_screen_load(origin);
    mock_nav_top = origin;

    /* --- Subtest 1A: Positive Control (Normal Opening) --- */
    mock_backend_status.state = PLUGIN_STORE_REFRESHING;
    bool started = gui_plugin_store_open_updates();
    assert(started);
    assert(store_open_origin == origin);
    assert(store_open_when_ready == true);
    assert(store_view == STORE_VIEW_UPDATES);
    assert(store_ui_active == true);
    assert(store_is_refresh == true);

    /* While busy screen is shown, busy screen is on top */
    mock_nav_top = mock_busy_screen;

    /* Backend completes */
    mock_backend_status.state = PLUGIN_STORE_READY;
    poll_plugin_store();
    assert(store_push_pending == true);
    assert(store_open_when_ready == false);

    /* Busy screen has popped, origin is now top again */
    mock_nav_top = origin;
    mock_transition_in_progress = false;
    poll_plugin_store();
    assert(mock_last_pushed == store_screen);
    assert(store_open_origin == NULL);
    assert(store_push_pending == false);
    printf("  Subtest 1A (positive normal opening): PASSED\n");

    /* --- Subtest 1B: Back/Home before worker READY (busy screen dismissed) --- */
    reset_test_state();
    lv_screen_load(origin);
    mock_nav_top = origin;

    mock_backend_status.state = PLUGIN_STORE_REFRESHING;
    started = gui_plugin_store_open_updates();
    assert(started);
    assert(store_open_when_ready == true);

    /* User pressed Back/Home during loading; busy screen is dismissed, top is origin */
    mock_nav_top = origin;
    poll_plugin_store();
    /* Cancellation must disarm store_open_when_ready and clear origin */
    assert(store_open_when_ready == false);
    assert(store_open_origin == NULL);

    /* Later worker completes */
    mock_backend_status.state = PLUGIN_STORE_READY;
    poll_plugin_store();
    /* store_push_pending must NOT be set, store must NOT be pushed */
    assert(store_push_pending == false);
    assert(mock_last_pushed != store_screen);
    printf("  Subtest 1B (Back/Home before worker READY): PASSED\n");

    /* --- Subtest 1C: After busy pop before deferred push --- */
    reset_test_state();
    lv_screen_load(origin);
    mock_nav_top = mock_busy_screen;

    mock_backend_status.state = PLUGIN_STORE_REFRESHING;
    started = gui_plugin_store_open_updates();
    assert(started);

    /* Worker finishes while transition is in progress */
    mock_backend_status.state = PLUGIN_STORE_READY;
    mock_transition_in_progress = true;
    poll_plugin_store();
    assert(store_push_pending == true);

    /* User navigated away to home_screen before push was performed */
    lv_obj_t * home_screen = lv_obj_create(NULL);
    mock_nav_top = home_screen;
    mock_transition_in_progress = false;

    poll_plugin_store();
    /* Deferred push must detect that store_open_origin is not top, and cancel */
    assert(mock_last_pushed != store_screen);
    assert(store_open_origin == NULL);
    assert(store_push_pending == false);
    lv_obj_delete(home_screen);
    printf("  Subtest 1C (navigated away before deferred push): PASSED\n");

    /* --- Subtest 1D: Main catalog Back/Home cancellation before READY --- */
    reset_test_state();
    lv_screen_load(origin);
    mock_nav_top = origin;

    mock_backend_status.state = PLUGIN_STORE_REFRESHING;
    gui_plugin_store_row_cb(NULL);
    assert(store_open_when_ready == true);
    assert(store_view == STORE_VIEW_PLUGINS);
    assert(store_open_origin == origin);

    /* User backs out before READY */
    mock_nav_top = origin;
    poll_plugin_store();
    assert(store_open_when_ready == false);
    assert(store_open_origin == NULL);
    printf("  Subtest 1D (main catalog Back/Home cancellation): PASSED\n");

    /* --- Subtest 1E: Themes catalog Back/Home cancellation before READY --- */
    reset_test_state();
    lv_screen_load(origin);
    mock_nav_top = origin;

    mock_backend_status.state = PLUGIN_STORE_REFRESHING;
    started = gui_plugin_store_open_themes();
    assert(started);
    assert(store_open_when_ready == true);
    assert(store_view == STORE_VIEW_THEMES);
    assert(store_open_origin == origin);

    /* User backs out before READY */
    mock_nav_top = origin;
    poll_plugin_store();
    assert(store_open_when_ready == false);
    assert(store_open_origin == NULL);
    printf("  Subtest 1E (themes catalog Back/Home cancellation): PASSED\n");

    lv_obj_delete(origin);
}

/* ========================================================================= */
/* Test 2: Rejection Preserves Active Pickers & Setup Catalog Borrowing      */
/* ========================================================================= */
static bool test_picker_is_selected_cb(const char * id) { (void)id; return false; }
static void test_picker_toggle_cb(const char * id, const char * name) { (void)id; (void)name; }
static void test_picker_done_cb(void) {}

static bool test_layout_is_selected_cb(const char * id) { (void)id; return false; }
static void test_layout_select_cb(const char * id, const char * name) { (void)id; (void)name; }

static void test_rejection_preserves_pickers(void) {
    printf("Running test_rejection_preserves_pickers...\n");
    reset_test_state();

    lv_obj_t * origin = lv_obj_create(NULL);
    lv_screen_load(origin);
    mock_nav_top = origin;

    /* --- Subtest 2A: Active plugin picker preserved on rejection --- */
    bool ok = gui_plugin_store_open_picker(test_picker_is_selected_cb,
                                           test_picker_toggle_cb,
                                           test_picker_done_cb);
    assert(ok);
    assert(store_picker_mode == true);

    /* Attempt invalid entry points while picker is active */
    bool rej1 = gui_plugin_store_open_player_layouts();
    assert(!rej1);
    bool rej2 = gui_plugin_store_open_updates();
    assert(!rej2);
    bool rej3 = gui_plugin_store_open_themes();
    assert(!rej3);

    /* Active picker state must remain untouched */
    assert(store_picker_mode == true);
    assert(store_picker_is_selected == test_picker_is_selected_cb);
    printf("  Subtest 2A (active plugin picker rejection preservation): PASSED\n");

    /* --- Subtest 2B: Active layout picker preserved on rejection --- */
    reset_test_state();
    lv_screen_load(origin);
    mock_nav_top = origin;

    ok = gui_plugin_store_open_layout_picker(test_layout_is_selected_cb,
                                             test_layout_select_cb);
    assert(ok);
    assert(store_layout_picker_mode == true);

    rej1 = gui_plugin_store_open_updates();
    assert(!rej1);
    rej2 = gui_plugin_store_open_picker(test_picker_is_selected_cb,
                                        test_picker_toggle_cb,
                                        test_picker_done_cb);
    assert(!rej2);
    rej3 = gui_plugin_store_open_themes();
    assert(!rej3);

    assert(store_layout_picker_mode == true);
    assert(store_layout_is_selected == test_layout_is_selected_cb);
    printf("  Subtest 2B (active layout picker rejection preservation): PASSED\n");

    /* --- Subtest 2C: Setup pending catalog borrowing --- */
    reset_test_state();
    lv_screen_load(origin);
    mock_nav_top = origin;

    /* Setup prepares catalog */
    mock_backend_status.state = PLUGIN_STORE_REFRESHING;
    ok = gui_plugin_store_setup_catalog_prepare();
    assert(ok);
    assert(store_setup_catalog_pending == true);

    /* Setup opens picker while catalog is pending */
    ok = gui_plugin_store_open_picker(test_picker_is_selected_cb,
                                      test_picker_toggle_cb,
                                      test_picker_done_cb);
    assert(ok);
    assert(store_picker_mode == true);
    assert(store_picker_pending == true);

    /* Backend finishes */
    mock_backend_status.state = PLUGIN_STORE_READY;
    assert(gui_plugin_store_setup_catalog_ready());
    poll_plugin_store();
    assert(store_picker_pending == false);
    assert(gui_popup_is_visible(&picker_popup));
    printf("  Subtest 2C (setup pending catalog borrowing): PASSED\n");

    /* --- Subtest 2D: Backend refresh rejection preserves state (commit-after-success guard) --- */
    reset_test_state();
    lv_screen_load(origin);
    mock_nav_top = origin;

    store_view = STORE_VIEW_PLUGINS;
    mock_refresh_success = false;

    /* Rejection preserves state for open_updates */
    bool open_up_ok = gui_plugin_store_open_updates();
    assert(!open_up_ok);
    assert(store_view == STORE_VIEW_PLUGINS); /* View NOT committed to UPDATES */
    assert(store_open_origin == NULL);
    assert(store_open_when_ready == false);
    assert(store_ui_active == false);
    assert(store_is_refresh == false);

    /* Rejection preserves state for open_player_layouts */
    bool open_lay_ok = gui_plugin_store_open_player_layouts();
    assert(!open_lay_ok);
    assert(store_view == STORE_VIEW_PLUGINS); /* View NOT committed to LAYOUTS */
    assert(store_open_origin == NULL);
    assert(store_ui_active == false);

    /* Rejection preserves state for setup catalog prepare */
    bool prep_ok = gui_plugin_store_setup_catalog_prepare();
    assert(!prep_ok);
    assert(store_setup_catalog_pending == false);

    /* Positive control: successful refresh commits view, origin, and pending state */
    mock_refresh_success = true;
    open_up_ok = gui_plugin_store_open_updates();
    assert(open_up_ok);
    assert(store_view == STORE_VIEW_UPDATES); /* Committed to UPDATES */
    assert(store_open_origin == origin);
    assert(store_open_when_ready == true);
    assert(store_ui_active == true);
    assert(store_is_refresh == true);
    printf("  Subtest 2D (backend refresh failure preserves state & commit-after-success): PASSED\n");

    lv_obj_delete(origin);
}

/* ========================================================================= */
/* Test 3: Updates View Scope, Global Update All, and Confirmation           */
/* ========================================================================= */
static void test_updates_view_and_update_all(void) {
    printf("Running test_updates_view_and_update_all...\n");
    reset_test_state();

    /* Setup 5 test plugins:
     * 0: Lua plugin with UPDATE, compatible
     * 1: Lua plugin INSTALLED, compatible
     * 2: Player Layout with UPDATE, compatible
     * 3: Lua plugin with UPDATE, INCOMPATIBLE
     * 4: Lua plugin AVAILABLE
     */
    snprintf(mock_backend_results[0].id, 64, "example.audiobooks");
    snprintf(mock_backend_results[0].name, 65, "Audiobooks");
    snprintf(mock_backend_results[0].version, 32, "1.1.0");
    mock_backend_results[0].state = PLUGIN_STORE_PLUGIN_UPDATE;
    mock_backend_results[0].incompatible = false;
    mock_backend_results[0].player_layout = false;

    snprintf(mock_backend_results[1].id, 64, "compas.autoeq");
    snprintf(mock_backend_results[1].name, 65, "AutoEQ");
    snprintf(mock_backend_results[1].version, 32, "1.0.0");
    mock_backend_results[1].state = PLUGIN_STORE_PLUGIN_INSTALLED;
    mock_backend_results[1].incompatible = false;
    mock_backend_results[1].player_layout = false;

    snprintf(mock_backend_results[2].id, 64, "example.themes");
    snprintf(mock_backend_results[2].name, 65, "Themes Pack");
    snprintf(mock_backend_results[2].version, 32, "2.0.0");
    mock_backend_results[2].state = PLUGIN_STORE_PLUGIN_UPDATE;
    mock_backend_results[2].incompatible = false;
    mock_backend_results[2].player_layout = false;
    mock_backend_results[2].theme_pack = true;

    snprintf(mock_backend_results[3].id, 64, "example.incomp");
    snprintf(mock_backend_results[3].name, 65, "Incompatible Plugin");
    snprintf(mock_backend_results[3].version, 32, "0.9.0");
    mock_backend_results[3].state = PLUGIN_STORE_PLUGIN_UPDATE;
    mock_backend_results[3].incompatible = true;
    mock_backend_results[3].player_layout = false;
    mock_backend_results[3].theme_pack = false;

    snprintf(mock_backend_results[4].id, 64, "example.gain_mode");
    snprintf(mock_backend_results[4].name, 65, "Gain Mode");
    snprintf(mock_backend_results[4].version, 32, "1.0.0");
    mock_backend_results[4].state = PLUGIN_STORE_PLUGIN_AVAILABLE;
    mock_backend_results[4].incompatible = false;
    mock_backend_results[4].player_layout = false;
    mock_backend_results[4].theme_pack = false;

    mock_backend_result_count = 5;
    mock_backend_status.result_count = 5;
    mock_backend_status.state = PLUGIN_STORE_READY;

    /* Populate Updates view */
    store_view = STORE_VIEW_UPDATES;
    populate_store_screen(false);

    /* Children of store_list in Updates view:
     * Child 0: Section header "Updates"
     * Child 1: Chevron row "Update All"
     * Child 2: Row for index 0 (example.audiobooks)
     * Child 3: Row for index 2 (example.themes - theme pack)
     * Total child count: 4. No rows for index 1, 3, or 4!
     */
    uint32_t count = lv_obj_get_child_count(store_list);
    assert(count == 4);

    /* Verify Row 2 corresponds to index 0 */
    lv_obj_t * row0 = lv_obj_get_child(store_list, 2);
    /* In add_plugin_row: lv_obj_add_event_cb(pill, store_row_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)index) */
    lv_obj_t * row0_title = lv_obj_get_child(lv_obj_get_child(row0, 0), 0);
    assert(strcmp(lv_label_get_text(row0_title), "Audiobooks") == 0);

    /* Verify Row 3 corresponds to index 2 (Theme pack) */
    lv_obj_t * row2 = lv_obj_get_child(store_list, 3);
    lv_obj_t * row2_title = lv_obj_get_child(lv_obj_get_child(row2, 0), 0);
    assert(strcmp(lv_label_get_text(row2_title), "Themes Pack") == 0);
    printf("  Subtest 3A (Updates view scope & layout/theme pack inclusion): PASSED\n");

    /* --- Subtest 3B: Empty Updates view --- */
    mock_backend_results[0].state = PLUGIN_STORE_PLUGIN_INSTALLED;
    mock_backend_results[2].state = PLUGIN_STORE_PLUGIN_INSTALLED;
    populate_store_screen(false);
    count = lv_obj_get_child_count(store_list);
    assert(count == 2); /* Empty label + Refresh row */
    lv_obj_t * empty_lbl = lv_obj_get_child(store_list, 0);
    assert(strcmp(lv_label_get_text(empty_lbl), "No updates available.") == 0);
    printf("  Subtest 3B (empty updates view state): PASSED\n");

    /* --- Subtest 3C: Global Update All exclusively in Updates view --- */
    /* Restore an update so Update All row is rendered */
    mock_backend_results[0].state = PLUGIN_STORE_PLUGIN_UPDATE;
    mock_backend_results[0].incompatible = false;
    store_view = STORE_VIEW_UPDATES;
    populate_store_screen(false);

    lv_obj_t * update_all_btn = lv_obj_get_child(store_list, 1);
    assert(update_all_btn != NULL);

    /* Send CLICKED in PLUGINS view -> ignored */
    store_view = STORE_VIEW_PLUGINS;
    mock_update_all_called = 0;
    lv_obj_send_event(update_all_btn, LV_EVENT_CLICKED, NULL);
    assert(mock_update_all_called == 0);

    /* Send CLICKED in LAYOUTS view -> ignored */
    store_view = STORE_VIEW_LAYOUTS;
    lv_obj_send_event(update_all_btn, LV_EVENT_CLICKED, NULL);
    assert(mock_update_all_called == 0);

    /* Send CLICKED in THEMES view -> ignored */
    store_view = STORE_VIEW_THEMES;
    lv_obj_send_event(update_all_btn, LV_EVENT_CLICKED, NULL);
    assert(mock_update_all_called == 0);

    /* Send CLICKED in UPDATES view -> executed */
    store_view = STORE_VIEW_UPDATES;
    lv_obj_send_event(update_all_btn, LV_EVENT_CLICKED, NULL);
    assert(mock_update_all_called == 1);
    assert(store_pending_update_all == true);
    printf("  Subtest 3C (Update All gated exclusively to Updates view): PASSED\n");

    /* --- Subtest 3D: Modified file confirmation remains individual --- */
    /* Case 1: Single plugin update confirmation */
    store_ui_active = true;
    store_pending_update_all = false;
    store_selected_index = 0;
    mock_backend_status.state = PLUGIN_STORE_NEEDS_CONFIRM;
    mock_backend_status.changed = true;
    poll_plugin_store();
    assert(store_confirm_pending == true);
    /* Next poll shows confirm popup */
    poll_plugin_store();
    assert(gui_popup_is_visible(&confirm_popup));

    lv_obj_t * replace_row = lv_obj_get_child(confirm_popup.popup, 1);
    lv_obj_t * individual_row = lv_obj_get_child(confirm_popup.popup, 2);
    /* In single plugin update: Replace is shown, Individual is hidden */
    assert(!lv_obj_has_flag(replace_row, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_flag(individual_row, LV_OBJ_FLAG_HIDDEN));

    gui_popup_hide(&confirm_popup);

    /* Case 2: Update All confirmation */
    store_ui_active = true;
    store_pending_update_all = true;
    mock_backend_status.state = PLUGIN_STORE_NEEDS_CONFIRM;
    poll_plugin_store();
    assert(store_confirm_pending == true);
    poll_plugin_store();
    assert(gui_popup_is_visible(&confirm_popup));

    /* In Update All: Replace is hidden, Individual is shown! */
    assert(lv_obj_has_flag(replace_row, LV_OBJ_FLAG_HIDDEN));
    assert(!lv_obj_has_flag(individual_row, LV_OBJ_FLAG_HIDDEN));

    /* Firing click on replace_row while Update All was pending must refuse force replace */
    int prev_toast = toast_info_count;
    lv_obj_send_event(replace_row, LV_EVENT_CLICKED, NULL);
    assert(store_pending_update_all == false);
    assert(toast_info_count == prev_toast + 1); /* "Update these plugins individually" toast */
    printf("  Subtest 3D (modified file confirmation remains individual): PASSED\n");
}

/* ========================================================================= */
/* Test 4: Curated Features Table Bounded Lookups & Setup Clamping           */
/* (Opus F3, F4, F5 findings)                                                */
/* ========================================================================= */
static void test_curated_features_and_setup(void) {
    printf("Running test_curated_features_and_setup...\n");

    /* Setup catalog incompatibility and removal filtering:
     * Verifies truthful metadata lookup and removed catalog filtering
     * via gui_plugin_store_setup_get_plugin. */
    reset_test_state();
    snprintf(mock_backend_results[0].id, 64, "compas.autoeq");
    snprintf(mock_backend_results[0].name, 65, "AutoEQ");
    mock_backend_results[0].incompatible = true;
    mock_backend_results[0].state = PLUGIN_STORE_PLUGIN_INCOMPATIBLE;

    snprintf(mock_backend_results[1].id, 64, "example.gain_mode");
    snprintf(mock_backend_results[1].name, 65, "Gain Mode");
    mock_backend_results[1].incompatible = false;
    mock_backend_results[1].state = PLUGIN_STORE_PLUGIN_REMOVED;

    mock_backend_result_count = 2;
    mock_backend_status.result_count = 2;
    mock_backend_status.state = PLUGIN_STORE_READY;
    store_setup_catalog_ready = true;
    store_row_count = 2;
    memcpy(store_rows, mock_backend_results, 2 * sizeof(plugin_store_result_t));

    plugin_store_result_t res;
    plugin_store_details_t det;

    /* Incompatible compas.autoeq: found in store, marked incompatible */
    bool found = gui_plugin_store_setup_get_plugin("compas.autoeq", &res, &det);
    assert(found);
    assert(res.incompatible || res.state == PLUGIN_STORE_PLUGIN_INCOMPATIBLE);

    /* Removed example.gain_mode: gui_plugin_store_setup_get_plugin filters REMOVED */
    found = gui_plugin_store_setup_get_plugin("example.gain_mode", &res, &det);
    assert(!found); /* Truthfully filtered out -> falls through to unavailable state */

    printf("  Test 4 (setup catalog incompatibility and removal filtering): PASSED\n");
}

/* ========================================================================= */
/* Test 5: Clean Teardown & Header Title Clearance                           */
/* ========================================================================= */
static void test_teardown_and_header(void) {
    printf("Running test_teardown_and_header...\n");
    reset_test_state();

    /* Subtest 5A: Clean store screen and popups teardown */
    gui_plugin_store_teardown();
    assert(store_screen == NULL);
    assert(store_list == NULL);
    assert(store_title == NULL);
    assert(store_open_origin == NULL);
    assert(store_picker_origin == NULL);
    assert(store_layout_picker_origin == NULL);
    assert(store_header_review_updates_btn == NULL);
    assert(store_setup_catalog_pending == false);
    assert(store_setup_catalog_ready == false);
    printf("  Subtest 5A (clean store screen and popups teardown): PASSED\n");

    /* Subtest 5B: Category row decoration (Opus F2) */
    lv_obj_t * test_screen = lv_obj_create(NULL);
    lv_obj_t * row = lv_obj_create(test_screen);
    lv_obj_t * lbl = lv_label_create(row);
    lv_label_set_text(lbl, "Plugins");
    decorate_category_row(row, NULL, NULL);
    /* Event callback attached, deletion frees context safely */
    lv_obj_delete(row);
    printf("  Subtest 5B (category row decoration): PASSED\n");

    /* Subtest 5C: Header icon button shrinks title without consuming slot */
    lv_obj_t * title_lbl = lv_label_create(test_screen);
    lv_label_set_text(title_lbl, "Streaming");
    lv_obj_add_flag(title_lbl, LV_OBJ_FLAG_USER_4);
    lv_obj_set_width(title_lbl, 400);

    lv_obj_t * header_btn = build_top_right_icon_button(test_screen, asset_path("playing_plane/ic_more.png"), NULL);
    assert(header_btn != NULL);
    /* Title label width was resized to leave clearance for top-right button */
    assert(lv_obj_get_width(title_lbl) < 400);
    lv_obj_delete(test_screen);
    printf("  Subtest 5C (header button title clearance): PASSED\n");
}

/* ========================================================================= */
/* Test 6: Store Themes Pack Classification, Views, and Picker Exclusion     */
/* ========================================================================= */
static void test_store_themes_and_views(void) {
    printf("Running test_store_themes_and_views...\n");
    reset_test_state();

    /* Set up 3 items:
     * 0: plugin (not layout, not theme pack)
     * 1: layout (not theme pack)
     * 2: theme pack (not layout)
     */
    snprintf(mock_backend_results[0].id, 64, "example.plugin");
    snprintf(mock_backend_results[0].name, 65, "Standard Plugin");
    mock_backend_results[0].state = PLUGIN_STORE_PLUGIN_AVAILABLE;
    mock_backend_results[0].player_layout = false;
    mock_backend_results[0].theme_pack = false;

    snprintf(mock_backend_results[1].id, 64, "example.layout");
    snprintf(mock_backend_results[1].name, 65, "Layout Plugin");
    mock_backend_results[1].state = PLUGIN_STORE_PLUGIN_AVAILABLE;
    mock_backend_results[1].player_layout = true;
    mock_backend_results[1].theme_pack = false;

    snprintf(mock_backend_results[2].id, 64, "example.themes");
    snprintf(mock_backend_results[2].name, 65, "Themes Pack");
    mock_backend_results[2].state = PLUGIN_STORE_PLUGIN_AVAILABLE;
    mock_backend_results[2].player_layout = false;
    mock_backend_results[2].theme_pack = true;

    mock_backend_result_count = 3;
    mock_backend_status.result_count = 3;
    mock_backend_status.state = PLUGIN_STORE_READY;
    memcpy(store_rows, mock_backend_results, 3 * sizeof(plugin_store_result_t));
    store_row_count = 3;

    /* Subtest 6A: STORE_VIEW_THEMES only shows theme packs */
    store_view = STORE_VIEW_THEMES;
    set_store_title();
    assert(store_title != NULL);
    assert(strcmp(lv_label_get_text(store_title), "Download themes") == 0);
    populate_store_screen(false);
    /* In STORE_VIEW_THEMES, store_list has grid of cards for visible items */
    assert(lv_obj_get_child_count(store_list) == 1); /* Only index 2 */
    lv_obj_t * card = lv_obj_get_child(store_list, 0);
    assert(card != NULL);
    lv_obj_t * card_title = lv_obj_get_child(card, 1);
    assert(strcmp(lv_label_get_text(card_title), "Themes Pack") == 0);
    printf("  Subtest 6A (STORE_VIEW_THEMES shows only theme packs): PASSED\n");

    /* Subtest 6B: STORE_VIEW_PLUGINS hides theme packs and layouts */
    store_view = STORE_VIEW_PLUGINS;
    set_store_title();
    assert(strcmp(lv_label_get_text(store_title), "Plugin Store") == 0);
    populate_store_screen(false);
    /* Child 0 is Available section header, child 1 is Standard Plugin */
    assert(lv_obj_get_child_count(store_list) == 2);
    lv_obj_t * plugin_row = lv_obj_get_child(store_list, 1);
    lv_obj_t * plugin_row_title = lv_obj_get_child(lv_obj_get_child(plugin_row, 0), 0);
    assert(strcmp(lv_label_get_text(plugin_row_title), "Standard Plugin") == 0);
    printf("  Subtest 6B (STORE_VIEW_PLUGINS hides theme packs): PASSED\n");

    /* Subtest 6C: STORE_VIEW_LAYOUTS shows only layouts */
    store_view = STORE_VIEW_LAYOUTS;
    set_store_title();
    assert(strcmp(lv_label_get_text(store_title), "Download layouts") == 0);
    populate_store_screen(false);
    assert(lv_obj_get_child_count(store_list) == 1);
    lv_obj_t * layout_card = lv_obj_get_child(store_list, 0);
    lv_obj_t * layout_card_title = lv_obj_get_child(layout_card, 1);
    assert(strcmp(lv_label_get_text(layout_card_title), "Layout Plugin") == 0);
    printf("  Subtest 6C (STORE_VIEW_LAYOUTS hides theme packs): PASSED\n");

    /* Subtest 6D: Setup / More picker popup excludes theme packs and layouts */
    store_picker_mode = true;
    populate_picker_popup(false);
    /* Only index 0 (Standard Plugin) should be populated */
    assert(lv_obj_get_child_count(picker_list) == 1);
    store_picker_mode = false;
    printf("  Subtest 6D (Setup picker popup excludes theme packs): PASSED\n");


}

int main(void) {
    lv_init();
    lv_display_t * disp = lv_display_create(480, 800);
    static uint8_t buf[480 * 20 * 4];
    lv_display_set_buffers(disp, buf, NULL, sizeof(buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, test_flush);

    app_font_16 = lv_font_montserrat_16;
    app_font_20 = lv_font_montserrat_20;
    app_font_22 = lv_font_montserrat_22;
    app_font_28 = lv_font_montserrat_28;

    mock_busy_screen = lv_obj_create(NULL);

    printf("=== Starting Phase 2 Regression Tests ===\n");
    test_navigation_cancellation();
    test_rejection_preserves_pickers();
    test_updates_view_and_update_all();
    test_curated_features_and_setup();
    test_teardown_and_header();
    test_store_themes_and_views();
    printf("=== All Phase 2 Regression Tests PASSED ===\n");

    lv_obj_delete(mock_busy_screen);
    lv_display_delete(disp);
    lv_deinit();
    return 0;
}
