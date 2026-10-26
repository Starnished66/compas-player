#ifndef PLAYER_UPDATE_H
#define PLAYER_UPDATE_H

#include "../../network/firmware/firmware_ota.h"

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    PLAYER_UPDATE_IDLE,
    PLAYER_UPDATE_CHECKING,
    PLAYER_UPDATE_CHECKED,
    PLAYER_UPDATE_DOWNLOADING,
    PLAYER_UPDATE_INSTALLING,
    PLAYER_UPDATE_DONE,
    PLAYER_UPDATE_FAILED
} player_update_state_t;

typedef struct {
    player_update_state_t state;
    int percent;
    firmware_ota_release_t release;
    char error[160];
} player_update_status_t;

bool player_update_start_check(void);
bool player_update_start_online_install(char *error, size_t error_size);
bool player_update_start_sd_install(char *error, size_t error_size);
void player_update_get_status(player_update_status_t *out);
void player_update_reset(void);

#endif
