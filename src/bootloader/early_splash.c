/* One-shot Linux init splash. Keep this independent of the full loader: init
 * can run it before SD setup and let its own stock screen remain on failure. */
#include "fb_draw.h"
#include "internal_storage.h"

int early_splash_run(void) {
    char reason[96];

    /* A stale /usr/data mount must never be used for user-provided content. */
    if (!internal_storage_ready(reason, sizeof(reason))) return 1;
    if (!fb_open()) return 1;

    const bool drawn = fb_draw_startup_background();
    if (drawn) fb_flush();
    fb_close();
    return drawn ? 0 : 1;
}

#ifndef EARLY_SPLASH_NO_MAIN
int main(void) {
    return early_splash_run();
}
#endif
