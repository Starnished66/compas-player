#include "gui_emulators.h"

#include "../../../audio/engine/audio.h"
#include "../../../core/device/emulator_registry.h"
#include "../../../network/store/catalog/emulator_store.h"
#include "gui_emulator_store.h"
#include "../../shell/gui_navigation.h"
#include "../../network/gui_network.h"
#include "../../shell/gui_notifications.h"
#include "../../player/screen/gui_player.h"
#include "../../application/gui.h"
#include "../../text/i18n/i18n.h"
#include "../../widgets/builders/screen_builders.h"
#include "../../../core/persistence/settings.h"
#include "../../../core/process/subprocess.h"
#include "../../../hardware/usb/usb_mode_control.h"

#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef HOST_BUILD
#define EMULATOR_LOG EMULATOR_ROOT "/.last-session.log"
#if defined(BOARD_R3PROII)
#define EMULATOR_BOARD "r3proii"
#elif defined(BOARD_R3II_2025)
#define EMULATOR_BOARD "r3ii_2025"
#else
#define EMULATOR_BOARD "r1"
#endif
#endif

extern player_settings_t current_settings;
extern char ** environ;

#define EMULATOR_SUBTITLE_MAX 288 /* up to eight system names */

/* Row 0 of the emulator list is the Download emulators row. */
static lv_obj_t * emulators_screen;
static lv_obj_t * emulators_list;
static lv_obj_t * emulators_empty_message;
/* The lists copy their item arrays but borrow the label strings, so the
 * strings and items live exactly as long as the list shows them. */
static emulator_manifest_t * emulators;
static compact_list_item_t * emulator_items;
static char (*emulator_subtitles)[EMULATOR_SUBTITLE_MAX];
static int emulator_count;

static lv_obj_t * systems_screen;
static lv_obj_t * systems_list;
static lv_obj_t * systems_title;
static compact_list_item_t system_items[EMULATOR_MAX_SYSTEMS];

static lv_obj_t * games_screen;
static lv_obj_t * games_list;
static lv_obj_t * games_title;
static lv_obj_t * games_empty_message;
static emulator_game_t * games;
static compact_list_item_t * game_items;
static int game_count;

/* The emulator and system the system and game lists are showing. */
static emulator_manifest_t selected_emulator;
static int selected_system;

static void join_system_names(char * out, size_t size, const emulator_manifest_t * m) {
    out[0] = '\0';
    for (int i = 0; i < m->system_count; i++) {
        size_t len = strlen(out);
        snprintf(out + len, size - len, "%s%s", i ? ", " : "", m->systems[i].name);
    }
}

/* Shows the screen's own message instead of the list when there is nothing
 * to list: the built-in empty text of the list says "No items". */
static void show_message_or_list(lv_obj_t * list, lv_obj_t * message, bool show_message) {
    if (show_message) {
        lv_obj_add_flag(list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(message, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(message, LV_OBJ_FLAG_HIDDEN);
    }
}

#ifndef HOST_BUILD
static bool dac_mode_active(void) {
    return current_settings.wifi_dac_mode_enabled || current_settings.bt_dac_mode_enabled ||
           current_settings.usb_mode == USB_MODE_DAC;
}

/* The player's environment plus the two variables of the emulator contract.
 * Free with free(); the strings are static. */
static char ** emulator_environment(void) {
    static char board[] = "COMPAS_BOARD=" EMULATOR_BOARD;
    static char protocol[] = "COMPAS_EMULATOR_PROTOCOL=1";
    size_t n = 0;
    while (environ && environ[n]) n++;
    char ** env = malloc((n + 3) * sizeof(*env));
    if (!env) return NULL;
    size_t used = 0;
    for (size_t i = 0; i < n; i++) {
        if (strncmp(environ[i], "COMPAS_BOARD=", 13) == 0 ||
            strncmp(environ[i], "COMPAS_EMULATOR_PROTOCOL=", 25) == 0) continue;
        env[used++] = environ[i];
    }
    env[used++] = board;
    env[used++] = protocol;
    env[used] = NULL;
    return env;
}

/* Runs the emulator as a child of this process and waits for it. 0 when it
 * quit normally, otherwise a negative value for a spawn failure or a
 * positive one for a non-zero exit or a signal. */
static int run_emulator_process(const emulator_manifest_t * emulator, const char * rom) {
    char dir[256], exec[400];
    snprintf(dir, sizeof(dir), "%s/%s", EMULATOR_ROOT, emulator->id);
    snprintf(exec, sizeof(exec), "%s/%s", dir, emulator->exec);
    char * const argv[] = { exec, (char *) rom, NULL };
    char ** envp = emulator_environment();
    posix_spawn_file_actions_t fa;
    pid_t pid = -1;
    int log_fd = open(EMULATOR_LOG, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (log_fd < 0) log_fd = open("/dev/null", O_WRONLY | O_CLOEXEC);
    int spawn_rc = !envp ? ENOMEM : log_fd < 0 ? errno : posix_spawn_file_actions_init(&fa);
    if (spawn_rc == 0) {
        /* Keep logs on the SD card. If it went away after the launch guard,
         * fall back to /dev/null instead of writing to RAM-backed /tmp. */
        spawn_rc = posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
        if (spawn_rc == 0)
            spawn_rc = posix_spawn_file_actions_adddup2(&fa, log_fd, STDOUT_FILENO);
        if (spawn_rc == 0) spawn_rc = posix_spawn_file_actions_adddup2(&fa, STDOUT_FILENO, STDERR_FILENO);
        /* After the dup2: it must not close a descriptor the child still needs. */
        if (spawn_rc == 0) spawn_rc = subprocess_close_inherited_fds_action(&fa);
        if (spawn_rc == 0) spawn_rc = posix_spawn_file_actions_addchdir_np(&fa, dir);
        posix_spawnattr_t attrs;
        bool attrs_ready = false;
        if (spawn_rc == 0) {
            spawn_rc = posix_spawnattr_init(&attrs);
            attrs_ready = spawn_rc == 0;
        }
        if (spawn_rc == 0) spawn_rc = posix_spawnattr_setpgroup(&attrs, 0);
        if (spawn_rc == 0) spawn_rc = posix_spawnattr_setflags(&attrs, POSIX_SPAWN_SETPGROUP);
        if (spawn_rc == 0) spawn_rc = posix_spawn(&pid, exec, &fa, &attrs, argv, envp);
        if (attrs_ready) posix_spawnattr_destroy(&attrs);
        posix_spawn_file_actions_destroy(&fa);
    }
    if (log_fd >= 0) close(log_fd);
    free(envp);
    if (spawn_rc != 0) return -1;

    /* Games are disposable and can use more memory than the player. Match
     * the metadata helper's OOM preference after the child has been created. */
    char oom_path[64];
    snprintf(oom_path, sizeof(oom_path), "/proc/%ld/oom_score_adj", (long) pid);
    int oom_fd = open(oom_path, O_WRONLY | O_CLOEXEC);
    if (oom_fd >= 0) {
        static const char score[] = "500";
        ssize_t ignored = write(oom_fd, score, sizeof(score) - 1);
        (void) ignored;
        close(oom_fd);
    }

    /* Blocking the UI thread here is intended. LVGL timers stop, so nothing
     * draws over the game, the screen timeout and idle suspend or shutdown
     * cannot fire, and the hardware-button consumers do not run while the
     * emulator owns the framebuffer and input devices. Audio, network and
     * library threads keep running. A crash in the emulator ends only the
     * child; the player must stay alive because the bootloader reboots the
     * device when it exits. */
    int status = 0;
    pid_t waited;
    while ((waited = waitpid(pid, &status, 0)) < 0 && errno == EINTR) {}
    if (waited < 0) return 1;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 1;
}
#endif

static void launch_game(const emulator_manifest_t * emulator, const char * path) {
#ifdef HOST_BUILD
    (void) emulator;
    (void) path;
    show_info_toast(TR("Games run on the player only."));
#else
    if (emulator_store_busy()) {
        show_info_toast(TR("Emulator store is busy."));
        return;
    }
    if (!emulator_store_sd_usable() || gui_network_usb_storage_session_active()) {
        show_info_toast(TR("Check the SD card and disconnect USB storage before playing."));
        return;
    }
    if (dac_mode_active()) {
        show_info_toast(TR("Turn off DAC mode to play games."));
        return;
    }
    if (!usb_mode_control_storage_write_begin()) {
        show_info_toast(TR("USB storage is busy."));
        return;
    }
    /* The emulator owns the audio device. Pausing only schedules a fade, so
     * wait until the audio worker confirms that it released the device. */
    gui_player_stop_for_external_session();
    const struct timespec audio_wait = { .tv_sec = 0, .tv_nsec = 20000000 };
    bool audio_idle = false;
    for (int waited_ms = 0; waited_ms < 2000; waited_ms += 20) {
        if (audio_is_idle()) {
            audio_idle = true;
            break;
        }
        (void) nanosleep(&audio_wait, NULL);
    }
    if (!audio_idle && audio_is_idle()) audio_idle = true;
    if (!audio_idle) {
        usb_mode_control_storage_write_end();
        show_info_toast(TR("Audio is still active; cannot start the game."));
        return;
    }
    if (emulator->free_player_memory) gui_release_memory_for_external_session();
    int rc = run_emulator_process(emulator, path);
    gui_resume_after_external_session();
    usb_mode_control_storage_write_end();
    if (rc < 0) show_error_toast(TR("Could not start the game."));
    else if (rc > 0) show_error_toast(TR("The game closed unexpectedly."));
#endif
}

static void game_row_click_cb(int index) {
    if (index < 0 || index >= game_count) return;
    /* Copy first: the toast and resume paths must not depend on list data. */
    char path[sizeof(games[0].path)];
    snprintf(path, sizeof(path), "%s", games[index].path);
    launch_game(&selected_emulator, path);
}

static void refresh_games(void) {
    int count;
    const emulator_system_t * system = &selected_emulator.systems[selected_system];
    char emulator_dir[160];
    snprintf(emulator_dir, sizeof(emulator_dir), "%s/%s", EMULATOR_ROOT, selected_emulator.id);
    emulator_game_t * entries = emulator_registry_list_games(EMULATOR_GAMES_SD_ROOT, emulator_dir, system, &count);
    compact_list_item_t * items = count ? calloc((size_t) count, sizeof(*items)) : NULL;
    if (count && !items) {
        free(entries);
        entries = NULL;
        count = 0;
    }
    for (int i = 0; i < count; i++) items[i].label = entries[i].label;

    compact_list_set_items(games_list, items, count);
    if (games_empty_message) lv_obj_delete(games_empty_message);
    char folder[96], detail[160];
    snprintf(folder, sizeof(folder), "Games/%s", system->games_dir);
    snprintf(detail, sizeof(detail), TR("Copy games to the %s folder on the SD card."), folder);
    games_empty_message = build_list_message(games_screen, TR("No games found"), detail);
    lv_obj_align(games_empty_message, LV_ALIGN_TOP_MID, 0, STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT);
    show_message_or_list(games_list, games_empty_message, count == 0);
    lv_label_set_text(games_title, system->name);
    free(games);
    free(game_items);
    games = entries;
    game_items = items;
    game_count = count;
}

static void games_screen_delete_cb(lv_event_t * e) {
    (void) e;
    games_screen = NULL;
    games_list = NULL;
    games_title = NULL;
    games_empty_message = NULL;
}

static void open_games(int system_index) {
    selected_system = system_index;
    if (!games_screen) {
        games_screen = build_compact_list_screen(TR("Emulators"), generic_back_cb, NULL, 0, game_row_click_cb, NULL,
                                                 &games_list, &games_title, LIST_ROW_WIDTH, false, NULL);
        lv_obj_add_event_cb(games_screen, games_screen_delete_cb, LV_EVENT_DELETE, NULL);
        finalize_screen_navigation(games_screen);
    }
    refresh_games();
    nav_push(games_screen);
}

static void system_row_click_cb(int index) {
    if (index >= 0 && index < selected_emulator.system_count) open_games(index);
}

static void systems_screen_delete_cb(lv_event_t * e) {
    (void) e;
    systems_screen = NULL;
    systems_list = NULL;
    systems_title = NULL;
}

static void open_systems(void) {
    for (int i = 0; i < selected_emulator.system_count; i++) {
        system_items[i] = (compact_list_item_t){ .label = selected_emulator.systems[i].name };
    }
    if (!systems_screen) {
        systems_screen = build_compact_list_screen(TR("Emulators"), generic_back_cb, NULL, 0, system_row_click_cb, NULL,
                                                   &systems_list, &systems_title, LIST_ROW_WIDTH, false, NULL);
        lv_obj_add_event_cb(systems_screen, systems_screen_delete_cb, LV_EVENT_DELETE, NULL);
        finalize_screen_navigation(systems_screen);
    }
    compact_list_set_items(systems_list, system_items, selected_emulator.system_count);
    lv_label_set_text(systems_title, selected_emulator.name);
    nav_push(systems_screen);
}

static void emulator_row_click_cb(int index) {
    if (index == 0) {
        gui_emulator_store_open();
        return;
    }
    if (index < 1 || index > emulator_count) return;
    /* Copy first: the next screens rescan and may free the list data. */
    selected_emulator = emulators[index - 1];
    if (selected_emulator.system_count == 1) open_games(0);
    else open_systems();
}

/* Rescans the installed emulators. Runs whenever the screen is shown, so it
 * also picks up what the store just installed or removed. */
static void refresh_emulators(void) {
    int count;
    emulator_manifest_t * found = NULL;
    count = 0;
    if (emulator_store_sd_usable()) found = emulator_registry_scan(EMULATOR_ROOT, &count);
    compact_list_item_t * items = calloc((size_t) count + 1, sizeof(*items));
    char (*subtitles)[EMULATOR_SUBTITLE_MAX] = calloc(count ? (size_t) count : 1, sizeof(*subtitles));
    if (!items || !subtitles) {
        free(items);
        free(subtitles);
        free(found);
        found = NULL;
        count = 0;
        items = calloc(1, sizeof(*items));
        subtitles = NULL;
        if (!items) return;
    }
    items[0].label = TR("Download emulators");
    for (int i = 0; i < count; i++) {
        join_system_names(subtitles[i], sizeof(subtitles[i]), &found[i]);
        items[i + 1].label = found[i].name;
        items[i + 1].subtitle = subtitles[i];
    }
    compact_list_set_items(emulators_list, items, count + 1);
    lv_obj_set_flag(emulators_empty_message, LV_OBJ_FLAG_HIDDEN, count > 0);
    free(emulators);
    free(emulator_items);
    free(emulator_subtitles);
    emulators = found;
    emulator_items = items;
    emulator_subtitles = subtitles;
    emulator_count = count;
}

static void emulators_screen_loaded_cb(lv_event_t * e) {
    (void) e;
    /* The rescan resets the list to the top; coming back from an emulator
     * should land where the user left it. */
    int32_t scroll_y = emulators_list ? lv_obj_get_scroll_y(emulators_list) : 0;
    refresh_emulators();
    if (!emulators_list) return;
    lv_obj_update_layout(emulators_screen);
    lv_obj_scroll_to_y(emulators_list, scroll_y > 0 ? scroll_y : 0, LV_ANIM_OFF);
}

static void emulators_screen_delete_cb(lv_event_t * e) {
    (void) e;
    emulators_screen = NULL;
    emulators_list = NULL;
    emulators_empty_message = NULL;
}

static void build_emulators_screen(void) {
    emulators_screen = build_compact_list_screen(TR("Emulators"), generic_back_cb, NULL, 0, emulator_row_click_cb, NULL,
                                                 &emulators_list, NULL, LIST_ROW_WIDTH, false, NULL);
    lv_obj_add_event_cb(emulators_screen, emulators_screen_delete_cb, LV_EVENT_DELETE, NULL);
    lv_obj_add_event_cb(emulators_screen, emulators_screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
    /* Sits under the Download emulators row, which is always listed. */
    emulators_empty_message = build_list_message(emulators_screen, TR("No emulators installed"),
                                                 TR("Download an emulator to play games."));
    lv_obj_align(emulators_empty_message, LV_ALIGN_TOP_MID, 0,
                 STATUS_BAR_CLEARANCE + TITLE_ROW_HEIGHT + GUI_ROW_GAP + ui_list_row_height() + GUI_ROW_GAP);
    finalize_screen_navigation(emulators_screen);
}

void gui_emulators_row_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!emulators_screen) build_emulators_screen();
    nav_push(emulators_screen);
}

void gui_emulators_teardown(void) {
    if (games_screen) lv_obj_delete(games_screen);
    if (systems_screen) lv_obj_delete(systems_screen);
    if (emulators_screen) lv_obj_delete(emulators_screen);
    free(games);
    free(game_items);
    free(emulators);
    free(emulator_items);
    free(emulator_subtitles);
    games = NULL;
    game_items = NULL;
    emulators = NULL;
    emulator_items = NULL;
    emulator_subtitles = NULL;
    game_count = 0;
    emulator_count = 0;
}
