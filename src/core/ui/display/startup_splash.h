#ifndef STARTUP_SPLASH_H
#define STARTUP_SPLASH_H

#include <stdbool.h>
#include <stdint.h>

#ifdef HOST_BUILD
#define STARTUP_SPLASH_PATH "./.compas/startup.bin"
#else
#define STARTUP_SPLASH_PATH "/usr/data/.compas/startup.bin"
#endif

/* Stored payloads are uncompressed LVGL v9 RGB565 binary images. This module
 * deliberately knows the 12-byte wire header without depending on LVGL. */
bool startup_splash_read(const char *path, int width, int height, uint16_t *pixels);
bool startup_splash_install(const char *source, int width, int height, const char **reason);
bool startup_splash_configured(void);
void startup_splash_get_tint(bool *enabled, uint32_t *accent);

#endif
