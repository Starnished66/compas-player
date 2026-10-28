#include "app_session.h"
#include "subprocess.h"
#include "../system/runtime/board_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef HOST_BUILD
#include "../../hardware/indicators/backlight.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <signal.h>
#include <spawn.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

extern char ** environ;

static bool own_variable(const char * entry) {
    static const char * const prefixes[] = { "COMPAS_APP_", "COMPAS_BOARD=", "COMPAS_SCREEN_" };
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        if (strncmp(entry, prefixes[i], strlen(prefixes[i])) == 0) return true;
    }
    return false;
}

char ** app_session_environment(char * const * base, const app_manifest_t * manifest, const char * data_dir,
                                const char * board, int screen_width, int screen_height) {
    enum { OWN = 6 };
    size_t n = 0;
    while (base && base[n]) n++;
    char ** env = calloc(n + OWN + 1, sizeof(*env));
    if (!env) return NULL;
    size_t used = 0;
    for (size_t i = 0; i < n; i++) {
        if (own_variable(base[i])) continue;
        env[used] = strdup(base[i]);
        if (!env[used++]) goto fail;
    }
    char text[OWN][600];
    snprintf(text[0], sizeof(text[0]), "COMPAS_APP_PROTOCOL=%d", APP_PROTOCOL);
    snprintf(text[1], sizeof(text[1]), "COMPAS_APP_ID=%s", manifest->id);
    snprintf(text[2], sizeof(text[2]), "COMPAS_APP_DATA=%s", data_dir);
    snprintf(text[3], sizeof(text[3]), "COMPAS_BOARD=%s", board);
    snprintf(text[4], sizeof(text[4]), "COMPAS_SCREEN_WIDTH=%d", screen_width);
    snprintf(text[5], sizeof(text[5]), "COMPAS_SCREEN_HEIGHT=%d", screen_height);
    for (int i = 0; i < OWN; i++) {
        env[used] = strdup(text[i]);
        if (!env[used++]) goto fail;
    }
    return env;
fail:
    app_session_environment_free(env);
    return NULL;
}

void app_session_environment_free(char ** env) {
    if (!env) return;
    for (char ** p = env; *p; p++) free(*p);
    free(env);
}

#ifndef HOST_BUILD
#define APP_LOG APP_ROOT "/.last-session.log"
#define FB_DEVICE "/dev/fb0"

/* The framebuffer mode the player's display driver believes is set, pan
 * included (it caches yoffset and flips pages from the cached value). An app
 * that changed the mode or panned must not leave a different one behind. */
static bool fb_mode_read(struct fb_var_screeninfo * var) {
    int fd = open(FB_DEVICE, O_RDWR | O_CLOEXEC);
    if (fd < 0) return false;
    bool ok = ioctl(fd, FBIOGET_VSCREENINFO, var) == 0;
    close(fd);
    return ok;
}

static void fb_mode_restore(const struct fb_var_screeninfo * saved) {
    int fd = open(FB_DEVICE, O_RDWR | O_CLOEXEC);
    if (fd < 0) return;
    struct fb_var_screeninfo now;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &now) == 0) {
        if (now.xres != saved->xres || now.yres != saved->yres || now.xres_virtual != saved->xres_virtual ||
            now.yres_virtual != saved->yres_virtual || now.bits_per_pixel != saved->bits_per_pixel ||
            now.xoffset != saved->xoffset || memcmp(&now.red, &saved->red, sizeof(now.red)) != 0 ||
            memcmp(&now.green, &saved->green, sizeof(now.green)) != 0 ||
            memcmp(&now.blue, &saved->blue, sizeof(now.blue)) != 0 || now.rotate != saved->rotate) {
            struct fb_var_screeninfo put = *saved;
            put.activate = FB_ACTIVATE_NOW;
            if (ioctl(fd, FBIOPUT_VSCREENINFO, &put) != 0) fprintf(stderr, "[apps] could not restore the framebuffer mode\n");
            if (ioctl(fd, FBIOGET_VSCREENINFO, &now) != 0) {
                close(fd);
                return;
            }
        }
        if (now.yoffset != saved->yoffset) {
            now.yoffset = saved->yoffset;
            if (ioctl(fd, FBIOPAN_DISPLAY, &now) != 0) fprintf(stderr, "[apps] could not restore the framebuffer pan\n");
        }
    }
    close(fd);
}

static bool make_data_dir(const char * data_dir) {
    struct stat st;
    return (mkdir(APP_DATA_ROOT, 0755) == 0 || errno == EEXIST) && (mkdir(data_dir, 0755) == 0 || errno == EEXIST) &&
           stat(data_dir, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Spawns the app and returns its pid, or -1. */
static pid_t spawn_app(const char * dir, const char * exec, const char * file, char ** envp) {
    char * const argv[] = { (char *) exec, (char *) file, NULL };
    posix_spawn_file_actions_t fa;
    pid_t pid = -1;
    int log_fd = open(APP_LOG, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (log_fd < 0) log_fd = open("/dev/null", O_WRONLY | O_CLOEXEC);
    int spawn_rc = log_fd < 0 ? errno : posix_spawn_file_actions_init(&fa);
    if (spawn_rc == 0) {
        /* Keep logs on the SD card. If it went away after the launch guard,
         * fall back to /dev/null instead of writing to RAM-backed /tmp. */
        spawn_rc = posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
        if (spawn_rc == 0) spawn_rc = posix_spawn_file_actions_adddup2(&fa, log_fd, STDOUT_FILENO);
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
    return spawn_rc == 0 ? pid : -1;
}

int app_session_run(const app_manifest_t * manifest, const char * file) {
    char dir[256], exec[400], data_dir[256];
    if (!app_dir_path(dir, sizeof(dir), APP_ROOT, manifest) || !app_exec_path(exec, sizeof(exec), APP_ROOT, manifest) ||
        snprintf(data_dir, sizeof(data_dir), "%s/%s", APP_DATA_ROOT, manifest->id) >= (int) sizeof(data_dir) ||
        !make_data_dir(data_dir)) {
        return -1;
    }
    char ** envp = app_session_environment(environ, manifest, data_dir, app_board_id(), BOARD_SCREEN_WIDTH,
                                           BOARD_SCREEN_HEIGHT);
    if (!envp) return -1;
    struct fb_var_screeninfo saved_mode;
    bool mode_known = fb_mode_read(&saved_mode);
    pid_t pid = spawn_app(dir, exec, file, envp);
    app_session_environment_free(envp);
    if (pid < 0) return -1;

    /* Apps are disposable and can use more memory than the player. Match the
     * emulators' OOM preference after the child has been created. */
    char oom_path[64];
    snprintf(oom_path, sizeof(oom_path), "/proc/%ld/oom_score_adj", (long) pid);
    int oom_fd = open(oom_path, O_WRONLY | O_CLOEXEC);
    if (oom_fd >= 0) {
        static const char score[] = "500";
        ssize_t ignored = write(oom_fd, score, sizeof(score) - 1);
        (void) ignored;
        close(oom_fd);
    }

    /* Blocking the UI thread here is intended, as for the emulators: LVGL
     * timers stop, so nothing draws over the app, the screen timeout and idle
     * suspend or shutdown cannot fire, and the hardware-button consumers do
     * not run while the app owns the framebuffer and input devices. Audio,
     * network and library threads keep running. A crash in the app ends only
     * the child; the player must stay alive because the bootloader reboots
     * the device when it exits. */
    int status = 0;
    pid_t waited;
    while ((waited = waitpid(pid, &status, 0)) < 0 && errno == EINTR) {}
    /* Anything the app left running in its group must not keep the screen. */
    (void) kill(-pid, SIGKILL);

    /* However the app ended, it may have blanked the panel, written the
     * backlight or panned the framebuffer behind the player's caches. */
    if (mode_known) fb_mode_restore(&saved_mode);
    backlight_resync_after_external_session();

    if (waited < 0) return 1;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 1;
}
#endif
