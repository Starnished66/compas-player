#include "emulator_audio_env.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>

/* Locale-independent: printf("%f") could print a comma. */
static void format_gain(float gain, char * out, size_t size) {
    long micros = gain > 0.0f ? lround((double) (gain > 1.0f ? 1.0f : gain) * 1e6) : 0;
    if (micros <= 0) snprintf(out, size, "0");
    else snprintf(out, size, "%ld.%06ld", micros / 1000000, micros % 1000000);
}

static bool add_var(emulator_audio_env_t * out, size_t * n, const char * name, const char * value) {
    int len = snprintf(out->var[*n], EMULATOR_AUDIO_ENV_LEN, "%s=%s", name, value);
    if (len < 0 || len >= EMULATOR_AUDIO_ENV_LEN) return false;
    (*n)++;
    return true;
}

size_t emulator_audio_env_build(const emulator_audio_route_t * route, emulator_audio_env_t * out) {
    size_t n = 0;
    if (!route || !out || route->route == AUDIO_OUTPUT_ROUTE_WIRED || !route->device || !route->device[0]) return 0;
    bool usb = route->route == AUDIO_OUTPUT_ROUTE_USB;
    /* Opening BlueALSA at another rate than the negotiated one recreates the
     * transport and drops the accessory, so no rate means no Bluetooth. */
    if (!usb && route->rate == 0) return 0;
    char text[24];
    if (usb) format_gain(route->gain, text, sizeof(text));
    else snprintf(text, sizeof(text), "%u", route->rate);
    if (!add_var(out, &n, "COMPAS_AUDIO_OUTPUT", usb ? "usb" : "bluetooth") ||
        !add_var(out, &n, "COMPAS_AUDIO_DEVICE", route->device) ||
        /* A USB device without its volume would play at full scale. */
        !add_var(out, &n, usb ? "COMPAS_AUDIO_GAIN" : "COMPAS_AUDIO_RATE", text))
        return 0;
    return n;
}
