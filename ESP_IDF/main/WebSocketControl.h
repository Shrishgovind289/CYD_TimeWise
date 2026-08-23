#ifndef WEBSOCKET_CONTROL_H
#define WEBSOCKET_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum
{
    WEBSOCKET_MUSIC_ACTION_ENABLE = 0,
    WEBSOCKET_MUSIC_ACTION_PLAY_RANDOM,
    WEBSOCKET_MUSIC_ACTION_PAUSE,
    WEBSOCKET_MUSIC_ACTION_RESUME,
    WEBSOCKET_MUSIC_ACTION_NEXT,
    WEBSOCKET_MUSIC_ACTION_PREVIOUS,
    WEBSOCKET_MUSIC_ACTION_STOP
} websocket_music_action_t;

typedef esp_err_t (*websocket_music_action_callback_t)(
    websocket_music_action_t action,
    int32_t value,
    void *user_data
);

void websocket_control_set_music_action_callback(
    websocket_music_action_callback_t callback,
    void *user_data
);

bool websocket_control_get_music_enabled(void);

esp_err_t websocket_control_start(void);
esp_err_t websocket_control_enter_music_mode(void);
esp_err_t websocket_control_exit_music_mode(void);
void websocket_control_stop(void);

#endif /* WEBSOCKET_CONTROL_H */