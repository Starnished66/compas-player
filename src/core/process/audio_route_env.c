#include "audio_route_env.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* Locale-independent: printf("%f") could print a comma. */
static void format_gain(float gain, char * out, size_t size) {
    long micros = gain > 0.0f ? lround((double) (gain > 1.0f ? 1.0f : gain) * 1e6) : 0;
    if (micros <= 0) snprintf(out, size, "0");
    else snprintf(out, size, "%ld.%06ld", micros / 1000000, micros % 1000000);
}

static bool add_var(audio_route_env_t * out, size_t * n, const char * name, const char * value) {
    int len = snprintf(out->var[*n], AUDIO_ROUTE_ENV_LEN, "%s=%s", name, value);
    if (len < 0 || len >= AUDIO_ROUTE_ENV_LEN) return false;
    (*n)++;
    return true;
}

size_t audio_route_env_build(const audio_route_t * route, audio_route_env_t * out) {
    size_t n = 0;
    if (!out) return 0;
    out->count = 0;
    if (!route || route->route == AUDIO_OUTPUT_ROUTE_WIRED || !route->device || !route->device[0]) return 0;
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
    out->count = n;
    return n;
}

bool audio_route_env_is_var(const char * entry) {
    static const char * const names[] = {
        "COMPAS_AUDIO_OUTPUT", "COMPAS_AUDIO_DEVICE", "COMPAS_AUDIO_GAIN", "COMPAS_AUDIO_RATE",
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        size_t len = strlen(names[i]);
        if (strncmp(entry, names[i], len) == 0 && entry[len] == '=') return true;
    }
    return false;
}
