#ifndef APP_SESSION_H
#define APP_SESSION_H

#include "../device/app_registry.h"

/* Runs an external app (app protocol 1) the way the emulators run: as a child
 * in its own process group, with its folder as the working directory and its
 * output in APP_ROOT/.last-session.log, while the caller (the UI thread)
 * waits and the app owns the framebuffer and input devices. Callers do the
 * guards and the audio stop first (gui_apps.c). */

/* The player's environment plus COMPAS_APP_PROTOCOL, COMPAS_APP_ID,
 * COMPAS_APP_DATA, COMPAS_BOARD, COMPAS_SCREEN_WIDTH and COMPAS_SCREEN_HEIGHT;
 * any copy of those variables in base is dropped. NULL-terminated; free with
 * app_session_environment_free(). */
char ** app_session_environment(char * const * base, const app_manifest_t * manifest, const char * data_dir,
                                const char * board, int screen_width, int screen_height);
void app_session_environment_free(char ** env);

#ifndef HOST_BUILD
/* Creates APP_DATA_ROOT/<id>, spawns the app (argv[1] is file unless NULL)
 * and waits for it. 0 when it quit normally, a negative value when it could
 * not be started, a positive one for a non-zero exit or a signal. However it
 * ends, the backlight and framebuffer state are put back before returning,
 * so a crashed app cannot leave the screen dark. */
int app_session_run(const app_manifest_t * manifest, const char * file);
#endif

#endif
