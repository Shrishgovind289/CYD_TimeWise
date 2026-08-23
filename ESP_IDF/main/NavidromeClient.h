#ifndef NAVIDROME_CLIENT_H
#define NAVIDROME_CLIENT_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* -------------------------------------------------------------------------- */
/*                           Configuration sizes                              */
/* -------------------------------------------------------------------------- */

#define NAVIDROME_BASE_URL_MAX_LENGTH       128U
#define NAVIDROME_USERNAME_MAX_LENGTH       64U
#define NAVIDROME_PASSWORD_MAX_LENGTH       64U

#define NAVIDROME_SONG_ID_MAX_LENGTH        96U
#define NAVIDROME_SONG_TITLE_MAX_LENGTH     128U
#define NAVIDROME_SONG_ARTIST_MAX_LENGTH    128U
#define NAVIDROME_SONG_ALBUM_MAX_LENGTH     128U

/* -------------------------------------------------------------------------- */
/*                              Song metadata                                 */
/* -------------------------------------------------------------------------- */

typedef struct
{
    char id[NAVIDROME_SONG_ID_MAX_LENGTH];
    char title[NAVIDROME_SONG_TITLE_MAX_LENGTH];
    char artist[NAVIDROME_SONG_ARTIST_MAX_LENGTH];
    char album[NAVIDROME_SONG_ALBUM_MAX_LENGTH];

    uint32_t duration_seconds;
} navidrome_song_t;

/* -------------------------------------------------------------------------- */
/*                        Runtime Navidrome settings                          */
/* -------------------------------------------------------------------------- */

extern char g_navidrome_base_url[NAVIDROME_BASE_URL_MAX_LENGTH];
extern char g_navidrome_username[NAVIDROME_USERNAME_MAX_LENGTH];
extern char g_navidrome_password[NAVIDROME_PASSWORD_MAX_LENGTH];

extern volatile bool g_navidrome_connected;
extern volatile uint32_t g_navidrome_timeout_ms;

/* -------------------------------------------------------------------------- */
/*                              Public API                                    */
/* -------------------------------------------------------------------------- */

esp_err_t navidrome_ping(void);

esp_err_t navidrome_get_random_song(navidrome_song_t *song);

bool navidrome_is_connected(void);

#endif /* NAVIDROME_CLIENT_H */