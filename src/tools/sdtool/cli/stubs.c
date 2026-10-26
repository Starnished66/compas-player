#include <stdbool.h>

/* The artwork coordinator asks whether music is playing so it can yield to the
 * player. A command-line tool never is. */
bool audio_is_playing(void) {
    return false;
}
