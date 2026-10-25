#ifndef HTTP_CONTROL_H
#define HTTP_CONTROL_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Configure these globals in HTTPControl.c before flashing. */
extern char g_timewise_gateway_url[128];
extern char g_timewise_device_token[65];

typedef enum {
    HTTP_MUSIC_ACTION_ENABLE = 0,
    HTTP_MUSIC_ACTION_PLAY_RANDOM,
    HTTP_MUSIC_ACTION_PAUSE,
    HTTP_MUSIC_ACTION_RESUME,
    HTTP_MUSIC_ACTION_NEXT,
    HTTP_MUSIC_ACTION_PREVIOUS,
    HTTP_MUSIC_ACTION_STOP
} http_music_action_t;

typedef esp_err_t (*http_music_action_callback_t)(http_music_action_t action,
                                                  int32_t value, void *user_data);

void http_control_set_music_action_callback(http_music_action_callback_t callback,
                                             void *user_data);
bool http_control_get_music_enabled(void);
esp_err_t http_control_start(void);
void http_control_stop(void);
/* The HTTP task remains running in both operating modes. */
esp_err_t http_control_enter_music_mode(void);
esp_err_t http_control_exit_music_mode(void);

#endif
