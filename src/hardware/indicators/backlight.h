#ifndef BACKLIGHT_H
#define BACKLIGHT_H

#include <stdbool.h>

/* Screen backlight brightness, via the standard Linux backlight sysfs class
 * (confirmed present on a real R1: /sys/class/backlight/backlight_pwm0,
 * writable as root).
 *
 * Every function below works in "logical" percent -- a clean 0-100 the UI
 * can show directly ("0%" and "100%" included). Internally, backlight.c
 * maps that to a narrower safe raw range before ever touching hardware:
 * never all the way down to a literal 0 (see BACKLIGHT_MIN_PERCENT below),
 * and never the literal max_brightness value either -- confirmed live on a
 * real R1 that writing that exact raw value makes the screen fully
 * invisible instead of "as bright as possible" (a PWM-at-max-duty-cycle
 * glitch, not something fixable from software beyond just never writing
 * that value). Callers never need to think about either edge themselves. */

/* Minimum brightness in percent. Never go lower than this in either
 * direction -- logical 0 still maps here, not to a true off;
 * dark wake preparation writes true 0 before powering the panel.
 * Values below this make the screen fully black with no visible feedback
 * to adjust back upward. */
#define BACKLIGHT_MIN_PERCENT 5

/* Current brightness as logical 0-100, or -1 if no backlight class device
 * is present (host, or a future unit with a different driver stack
 * entirely). */
int backlight_get_percent(void);

/* Sets the user's normal brightness and makes it the level restored after
 * dimming or a full screen-off cycle. */
void backlight_set_normal_percent(int percent);

/* Coalesces normal-brightness writes on a process-lifetime worker so slider
 * callbacks never wait on sysfs I/O. */
void backlight_request_normal_percent(int percent);

/* Temporarily lowers the lit screen without losing the user's configured
 * brightness. Passing false restores that normal brightness. */
void backlight_set_dimmed(bool dimmed);

/* Screen power state shared by GUI power-button and timeout handling.
 * Reports the requested state. Thread-safe. */
bool backlight_screen_is_on(void);

/* Worker acknowledgement that the requested screen brightness was applied. */
bool backlight_screen_is_visible(void);

/* Turning off remembers the real current brightness (whatever the user last
 * set via the quick-drawer slider) and restores exactly that on the next
 * turn-on, rather than resetting to a fixed level. No-op if the screen is
 * already off. Repeating an on request retries a failed hardware write. */
void backlight_set_screen_on(bool on);

/* Prepare the powered panel while keeping its PWM at true zero. The GUI
 * polls completion, presents its wake surface, then reveals it with
 * backlight_set_screen_on(true). Turning off also cancels preparation. */
void backlight_prepare_screen_on(void);
bool backlight_screen_is_prepared(void);

#endif /* BACKLIGHT_H */
