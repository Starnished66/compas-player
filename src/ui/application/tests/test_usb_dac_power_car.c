/* Car mode hands the USB DAC screen its policy back when its power-off no
 * longer applies: power returned, or Car Mode turned off. */
#include "src/ui/application/gui_control_poll.c"
#include <assert.h>

static usb_dac_power_action_t cancelled = USB_DAC_POWER_NONE;
static int cancels;
void gui_network_usb_dac_power_cancelled(usb_dac_power_action_t action) { cancelled = action; cancels++; }

int main(void) {
    car_mode_state_t s = {0};
    car_mode_sample_power(&s, BATTERY_EXTERNAL_POWER_CONNECTED);
    for (int i = 0; i < CAR_POWER_REMOVAL_DEBOUNCE_POLLS; ++i)
        car_mode_sample_power(&s, BATTERY_EXTERNAL_POWER_DISCONNECTED);
    assert(s.car_shutdown_pending);
    cancels = 0;
    car_mode_sample_power(&s, BATTERY_EXTERNAL_POWER_CONNECTED);
    assert(!s.car_shutdown_pending && cancels == 1 && cancelled == USB_DAC_POWER_CAR);
    for (int i = 0; i < CAR_POWER_REMOVAL_DEBOUNCE_POLLS; ++i)
        car_mode_sample_power(&s, BATTERY_EXTERNAL_POWER_DISCONNECTED);
    assert(s.car_shutdown_pending);
    cancels = 0;
    car_mode_reset_baseline(&s); /* Car Mode turned off */
    assert(!s.car_shutdown_pending && cancels == 1 && cancelled == USB_DAC_POWER_CAR);
    puts("Car mode USB DAC power cancellation passed");
}
