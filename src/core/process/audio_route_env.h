#pragma once

#include "../../audio/output/audio_output.h"

#include <stdbool.h>
#include <stddef.h>

/* Environment strings that tell an emulator or an app which audio output the
 * player uses: COMPAS_AUDIO_OUTPUT and COMPAS_AUDIO_DEVICE, plus
 * COMPAS_AUDIO_RATE for Bluetooth or COMPAS_AUDIO_GAIN for USB. */
#define AUDIO_ROUTE_ENV_MAX 3
#define AUDIO_ROUTE_ENV_LEN 80

typedef struct {
    char var[AUDIO_ROUTE_ENV_MAX][AUDIO_ROUTE_ENV_LEN];
    size_t count; /* how many of var are set; same as the build return value */
} audio_route_env_t;

typedef struct {
    audio_output_route_t route;
    const char * device; /* ALSA device for aplay; NULL or empty when unknown */
    unsigned int rate;   /* Bluetooth: the rate the accessory negotiated, 0 when unknown */
    float gain;          /* USB: the player's digital volume, linear */
} audio_route_t;

/* Fills out->var with "NAME=value" strings and returns how many. Gives none
 * for wired, or when the device (or, for Bluetooth, the rate) is unknown:
 * the emulator then uses the built-in output, as it always has. */
size_t audio_route_env_build(const audio_route_t * route, audio_route_env_t * out);

/* True for an "NAME=value" entry of one of the four COMPAS_AUDIO_ variables,
 * so a launcher can drop copies it inherited before adding its own. */
bool audio_route_env_is_var(const char * entry);
