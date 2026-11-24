#pragma once

#include "../../../audio/output/audio_output.h"

#include <stddef.h>

/* Environment strings that tell an emulator which audio output the player
 * uses: COMPAS_AUDIO_OUTPUT and COMPAS_AUDIO_DEVICE, plus COMPAS_AUDIO_RATE
 * for Bluetooth or COMPAS_AUDIO_GAIN for USB. */
#define EMULATOR_AUDIO_ENV_MAX 3
#define EMULATOR_AUDIO_ENV_LEN 80

typedef struct {
    char var[EMULATOR_AUDIO_ENV_MAX][EMULATOR_AUDIO_ENV_LEN];
} emulator_audio_env_t;

typedef struct {
    audio_output_route_t route;
    const char * device; /* ALSA device for aplay; NULL or empty when unknown */
    unsigned int rate;   /* Bluetooth: the rate the accessory negotiated, 0 when unknown */
    float gain;          /* USB: the player's digital volume, linear */
} emulator_audio_route_t;

/* Fills out->var with "NAME=value" strings and returns how many. Gives none
 * for wired, or when the device (or, for Bluetooth, the rate) is unknown:
 * the emulator then uses the built-in output, as it always has. */
size_t emulator_audio_env_build(const emulator_audio_route_t * route, emulator_audio_env_t * out);
