#pragma once

/* UI-thread API. Build the persistent overlay before showing it; teardown
 * cancels transient motion and releases owned image buffers, and is safe to
 * call more than once during UI reload. */
void build_power_off_countdown_popup(void);
void start_power_off_countdown(void);
void gui_power_hub_teardown(void);
