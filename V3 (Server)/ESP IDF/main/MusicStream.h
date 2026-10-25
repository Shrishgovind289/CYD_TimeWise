#ifndef TIMEWISE_MUSIC_STREAM_H
#define TIMEWISE_MUSIC_STREAM_H
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Server contract: POST /music/select {"action":"random|next|previous"}
   returns {"id":"...","title":"...","artist":"...","album":"...","duration":123}.
   GET /music/stream?id=<URL-encoded-id> streams unsigned 8-bit mono PCM
   at 24000 samples/sec, without a WAV header. Both use X-Device-Token. */
#define MUSIC_STREAM_SAMPLE_RATE 24000U

typedef struct {
    char id[96];
    char title[128];
    char artist[128];
    char album[128];
    uint32_t duration_seconds;
} music_stream_song_t;

typedef enum {
    MUSIC_STREAM_STOPPED = 0,
    MUSIC_STREAM_CONNECTING,
    MUSIC_STREAM_BUFFERING,
    MUSIC_STREAM_PLAYING,
    MUSIC_STREAM_PAUSED,
    MUSIC_STREAM_ERROR
} music_stream_state_t;

extern volatile music_stream_state_t g_music_stream_state;
extern volatile uint32_t g_music_stream_position_seconds;

esp_err_t music_stream_select(const char *action, music_stream_song_t *song);
esp_err_t music_stream_start(void);
void music_stream_stop(void);
esp_err_t music_stream_pause(void);
esp_err_t music_stream_resume(void);
bool music_stream_is_busy(void);
bool music_stream_is_paused(void);
music_stream_state_t music_stream_get_state(void);
const char *music_stream_state_to_string(music_stream_state_t state);
uint32_t music_stream_get_position_seconds(void);
void music_stream_get_current_song(music_stream_song_t *out);
#endif
