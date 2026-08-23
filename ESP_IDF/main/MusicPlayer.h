#ifndef MUSIC_PLAYER_H
#define MUSIC_PLAYER_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "NavidromeClient.h"

typedef enum
{
    MUSIC_PLAYER_STATE_STOPPED = 0,
    MUSIC_PLAYER_STATE_CONNECTING,
    MUSIC_PLAYER_STATE_BUFFERING,
    MUSIC_PLAYER_STATE_PLAYING,
    MUSIC_PLAYER_STATE_PAUSED,
    MUSIC_PLAYER_STATE_ERROR
} music_player_state_t;

extern volatile bool g_music_player_playing;
extern volatile bool g_music_player_stop_requested;
extern volatile bool g_music_player_exclusive_mode;
extern volatile bool g_music_player_paused;
extern volatile uint16_t g_music_player_max_bitrate_kbps;
extern volatile uint32_t g_music_player_position_seconds;
extern volatile music_player_state_t g_music_player_state;

extern navidrome_song_t g_music_player_song;

esp_err_t music_player_start(const navidrome_song_t *song);

void music_player_stop(void);
esp_err_t music_player_pause(void);
esp_err_t music_player_resume(void);

bool music_player_is_playing(void);
bool music_player_is_paused(void);
bool music_player_is_exclusive_mode(void);

music_player_state_t music_player_get_state(void);
const char *music_player_state_to_string(music_player_state_t state);
uint32_t music_player_get_position_seconds(void);

void music_player_get_current_song(navidrome_song_t *song);

#endif /* MUSIC_PLAYER_H */