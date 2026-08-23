#include "NavidromeClient.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"

#include "mbedtls/md5.h"

static const char *TAG = "Navidrome";

/* -------------------------------------------------------------------------- */
/*                           Navidrome configuration                          */
/* -------------------------------------------------------------------------- */

#define NAVIDROME_API_VERSION                "1.16.1"
#define NAVIDROME_CLIENT_NAME                "TimeWise"

#define NAVIDROME_URL_BUFFER_SIZE            768U

#define NAVIDROME_PING_RESPONSE_SIZE         1024U
#define NAVIDROME_SONG_RESPONSE_SIZE         4096U

#define NAVIDROME_SALT_LENGTH                16U
#define NAVIDROME_TOKEN_LENGTH               32U

#define NAVIDROME_ENCODED_USERNAME_SIZE      192U

/* -------------------------------------------------------------------------- */
/*                        Runtime configuration                               */
/* -------------------------------------------------------------------------- */

char g_navidrome_base_url[NAVIDROME_BASE_URL_MAX_LENGTH] =
    "http://192.168.0.249:4533";

char g_navidrome_username[NAVIDROME_USERNAME_MAX_LENGTH] =
    "TimeWise";

char g_navidrome_password[NAVIDROME_PASSWORD_MAX_LENGTH] =
    "TimeWise32";

volatile bool g_navidrome_connected = false;

volatile uint32_t g_navidrome_timeout_ms = 8000U;

/* -------------------------------------------------------------------------- */
/*                             HTTP response                                  */
/* -------------------------------------------------------------------------- */

typedef struct
{
    char *buffer;
    size_t buffer_size;
    size_t length;

    bool overflow;
} navidrome_http_response_t;

/* -------------------------------------------------------------------------- */
/*                              Text helper                                   */
/* -------------------------------------------------------------------------- */

static void navidrome_copy_text(char *destination,
                                size_t destination_size,
                                const char *source)
{
    if (destination == NULL || destination_size == 0U)
    {
        return;
    }

    snprintf(
        destination,
        destination_size,
        "%s",
        source != NULL ? source : ""
    );
}

/* -------------------------------------------------------------------------- */
/*                              URL encoding                                  */
/* -------------------------------------------------------------------------- */

static bool navidrome_url_character_is_safe(char character)
{
    if (isalnum((unsigned char)character))
    {
        return true;
    }

    switch (character)
    {
        case '-':
        case '_':
        case '.':
        case '~':
            return true;

        default:
            return false;
    }
}

static esp_err_t navidrome_url_encode(const char *source,
                                      char *destination,
                                      size_t destination_size)
{
    static const char hex[] =
        "0123456789ABCDEF";

    if (source == NULL ||
        destination == NULL ||
        destination_size == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }

    size_t destination_index = 0U;

    for (size_t source_index = 0U;
         source[source_index] != '\0';
         source_index++)
    {
        unsigned char character =
            (unsigned char)source[source_index];

        if (navidrome_url_character_is_safe((char)character))
        {
            if (destination_index + 1U >= destination_size)
            {
                return ESP_ERR_INVALID_SIZE;
            }

            destination[destination_index++] =
                (char)character;
        }
        else
        {
            if (destination_index + 3U >= destination_size)
            {
                return ESP_ERR_INVALID_SIZE;
            }

            destination[destination_index++] = '%';

            destination[destination_index++] =
                hex[(character >> 4U) & 0x0FU];

            destination[destination_index++] =
                hex[character & 0x0FU];
        }
    }

    destination[destination_index] = '\0';

    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/*                              Authentication                                */
/* -------------------------------------------------------------------------- */

static void navidrome_generate_salt(char salt[NAVIDROME_SALT_LENGTH + 1U])
{
    uint32_t random_1 = esp_random();
    uint32_t random_2 = esp_random();

    snprintf(
        salt,
        NAVIDROME_SALT_LENGTH + 1U,
        "%08lx%08lx",
        (unsigned long)random_1,
        (unsigned long)random_2
    );
}

static esp_err_t navidrome_generate_token(
    const char *password,
    const char *salt,
    char token[NAVIDROME_TOKEN_LENGTH + 1U])
{
    if (password == NULL ||
        salt == NULL ||
        token == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    char authentication_text[
        NAVIDROME_PASSWORD_MAX_LENGTH +
        NAVIDROME_SALT_LENGTH +
        1U
    ];

    int written = snprintf(
        authentication_text,
        sizeof(authentication_text),
        "%s%s",
        password,
        salt
    );

    if (written < 0 ||
        (size_t)written >= sizeof(authentication_text))
    {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t digest[16];

    int md5_result = mbedtls_md5(
        (const unsigned char *)authentication_text,
        strlen(authentication_text),
        digest
    );

    if (md5_result != 0)
    {
        ESP_LOGE(
            TAG,
            "MD5 generation failed: %d",
            md5_result
        );

        return ESP_FAIL;
    }

    for (size_t index = 0U;
         index < sizeof(digest);
         index++)
    {
        snprintf(
            &token[index * 2U],
            3U,
            "%02x",
            digest[index]
        );
    }

    token[NAVIDROME_TOKEN_LENGTH] = '\0';

    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/*                       Authentication preparation                           */
/* -------------------------------------------------------------------------- */

static esp_err_t navidrome_prepare_authentication(
    char salt[NAVIDROME_SALT_LENGTH + 1U],
    char token[NAVIDROME_TOKEN_LENGTH + 1U],
    char encoded_username[NAVIDROME_ENCODED_USERNAME_SIZE])
{
    if (g_navidrome_base_url[0] == '\0' ||
        g_navidrome_username[0] == '\0' ||
        g_navidrome_password[0] == '\0')
    {
        ESP_LOGE(
            TAG,
            "Navidrome configuration is incomplete"
        );

        return ESP_ERR_INVALID_STATE;
    }

    navidrome_generate_salt(
        salt
    );

    esp_err_t result = navidrome_generate_token(
        g_navidrome_password,
        salt,
        token
    );

    if (result != ESP_OK)
    {
        return result;
    }

    result = navidrome_url_encode(
        g_navidrome_username,
        encoded_username,
        NAVIDROME_ENCODED_USERNAME_SIZE
    );

    if (result != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Could not URL encode Navidrome username"
        );

        return result;
    }

    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/*                             HTTP handler                                   */
/* -------------------------------------------------------------------------- */

static esp_err_t navidrome_http_event_handler(
    esp_http_client_event_t *event)
{
    if (event == NULL)
    {
        return ESP_OK;
    }

    if (event->event_id != HTTP_EVENT_ON_DATA)
    {
        return ESP_OK;
    }

    if (event->data == NULL ||
        event->data_len <= 0 ||
        event->user_data == NULL)
    {
        return ESP_OK;
    }

    navidrome_http_response_t *response =
        (navidrome_http_response_t *)event->user_data;

    if (response->buffer == NULL ||
        response->buffer_size == 0U)
    {
        return ESP_OK;
    }

    size_t available =
        response->buffer_size -
        response->length -
        1U;

    size_t bytes_to_copy =
        (size_t)event->data_len;

    if (bytes_to_copy > available)
    {
        bytes_to_copy = available;
        response->overflow = true;
    }

    if (bytes_to_copy > 0U)
    {
        memcpy(
            &response->buffer[response->length],
            event->data,
            bytes_to_copy
        );

        response->length +=
            bytes_to_copy;

        response->buffer[
            response->length
        ] = '\0';
    }

    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/*                          Generic HTTP request                              */
/* -------------------------------------------------------------------------- */

static esp_err_t navidrome_http_get(const char *url,
                                    char *response_buffer,
                                    size_t response_buffer_size)
{
    if (url == NULL ||
        response_buffer == NULL ||
        response_buffer_size == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(
        response_buffer,
        0,
        response_buffer_size
    );

    navidrome_http_response_t response =
    {
        .buffer = response_buffer,
        .buffer_size = response_buffer_size,
        .length = 0U,
        .overflow = false
    };

    esp_http_client_config_t configuration =
    {
        .url = url,
        .method = HTTP_METHOD_GET,

        .timeout_ms =
            (int)g_navidrome_timeout_ms,

        .event_handler =
            navidrome_http_event_handler,

        .user_data =
            &response,

        .buffer_size = 1024,
        .buffer_size_tx = 1024
    };

    esp_http_client_handle_t client =
        esp_http_client_init(
            &configuration
        );

    if (client == NULL)
    {
        ESP_LOGE(
            TAG,
            "Could not create HTTP client"
        );

        return ESP_ERR_NO_MEM;
    }

    esp_err_t result =
        esp_http_client_perform(
            client
        );

    if (result != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "HTTP request failed: %s",
            esp_err_to_name(result)
        );

        esp_http_client_cleanup(
            client
        );

        return result;
    }

    int http_status =
        esp_http_client_get_status_code(
            client
        );

    esp_http_client_cleanup(
        client
    );

    ESP_LOGI(
        TAG,
        "HTTP status: %d",
        http_status
    );

    if (http_status != 200)
    {
        ESP_LOGE(
            TAG,
            "Unexpected HTTP status: %d",
            http_status
        );

        return ESP_FAIL;
    }

    if (response.overflow)
    {
        ESP_LOGE(
            TAG,
            "HTTP response buffer overflow"
        );

        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/*                       API response status check                            */
/* -------------------------------------------------------------------------- */

static esp_err_t navidrome_check_api_status(
    cJSON *subsonic_response)
{
    if (!cJSON_IsObject(subsonic_response))
    {
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *status =
        cJSON_GetObjectItemCaseSensitive(
            subsonic_response,
            "status"
        );

    if (cJSON_IsString(status) &&
        status->valuestring != NULL &&
        strcmp(status->valuestring, "ok") == 0)
    {
        return ESP_OK;
    }

    cJSON *error =
        cJSON_GetObjectItemCaseSensitive(
            subsonic_response,
            "error"
        );

    if (cJSON_IsObject(error))
    {
        cJSON *code =
            cJSON_GetObjectItemCaseSensitive(
                error,
                "code"
            );

        cJSON *message =
            cJSON_GetObjectItemCaseSensitive(
                error,
                "message"
            );

        if (cJSON_IsNumber(code) &&
            cJSON_IsString(message) &&
            message->valuestring != NULL)
        {
            ESP_LOGE(
                TAG,
                "Navidrome API error %d: %s",
                code->valueint,
                message->valuestring
            );
        }
    }

    return ESP_FAIL;
}

/* -------------------------------------------------------------------------- */
/*                              Ping                                          */
/* -------------------------------------------------------------------------- */

esp_err_t navidrome_ping(void)
{
    char salt[
        NAVIDROME_SALT_LENGTH + 1U
    ];

    char token[
        NAVIDROME_TOKEN_LENGTH + 1U
    ];

    char encoded_username[
        NAVIDROME_ENCODED_USERNAME_SIZE
    ];

    char url[
        NAVIDROME_URL_BUFFER_SIZE
    ];

    esp_err_t result = navidrome_prepare_authentication(
        salt,
        token,
        encoded_username
    );

    if (result != ESP_OK)
    {
        g_navidrome_connected = false;
        return result;
    }

    size_t base_length =
        strlen(g_navidrome_base_url);

    const char *separator =
        (
            base_length > 0U &&
            g_navidrome_base_url[
                base_length - 1U
            ] == '/'
        )
        ? ""
        : "/";

    int written = snprintf(
        url,
        sizeof(url),

        "%s%srest/ping.view"
        "?u=%s"
        "&t=%s"
        "&s=%s"
        "&v=%s"
        "&c=%s"
        "&f=json",

        g_navidrome_base_url,
        separator,

        encoded_username,
        token,
        salt,

        NAVIDROME_API_VERSION,
        NAVIDROME_CLIENT_NAME
    );

    if (written < 0 ||
        (size_t)written >= sizeof(url))
    {
        return ESP_ERR_INVALID_SIZE;
    }

    char *response_buffer =
        calloc(
            NAVIDROME_PING_RESPONSE_SIZE,
            sizeof(char)
        );

    if (response_buffer == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(
        TAG,
        "Connecting to Navidrome: %s",
        g_navidrome_base_url
    );

    result = navidrome_http_get(
        url,
        response_buffer,
        NAVIDROME_PING_RESPONSE_SIZE
    );

    if (result != ESP_OK)
    {
        free(response_buffer);

        g_navidrome_connected = false;

        return result;
    }

    cJSON *root =
        cJSON_Parse(
            response_buffer
        );

    free(
        response_buffer
    );

    if (root == NULL)
    {
        ESP_LOGE(
            TAG,
            "Could not parse Navidrome ping JSON"
        );

        g_navidrome_connected = false;

        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *subsonic_response =
        cJSON_GetObjectItemCaseSensitive(
            root,
            "subsonic-response"
        );

    result =
        navidrome_check_api_status(
            subsonic_response
        );

    if (result == ESP_OK)
    {
        g_navidrome_connected = true;

        ESP_LOGI(
            TAG,
            "Navidrome connected successfully"
        );

        cJSON *version =
            cJSON_GetObjectItemCaseSensitive(
                subsonic_response,
                "version"
            );

        if (cJSON_IsString(version) &&
            version->valuestring != NULL)
        {
            ESP_LOGI(
                TAG,
                "Subsonic API version: %s",
                version->valuestring
            );
        }
    }
    else
    {
        g_navidrome_connected = false;
    }

    cJSON_Delete(
        root
    );

    return result;
}

/* -------------------------------------------------------------------------- */
/*                          Get one random song                               */
/* -------------------------------------------------------------------------- */

esp_err_t navidrome_get_random_song(navidrome_song_t *song)
{
    if (song == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(
        song,
        0,
        sizeof(*song)
    );

    char salt[
        NAVIDROME_SALT_LENGTH + 1U
    ];

    char token[
        NAVIDROME_TOKEN_LENGTH + 1U
    ];

    char encoded_username[
        NAVIDROME_ENCODED_USERNAME_SIZE
    ];

    char url[
        NAVIDROME_URL_BUFFER_SIZE
    ];

    /* ---------------------------------------------------------------------- */
    /* Authentication                                                         */
    /* ---------------------------------------------------------------------- */

    esp_err_t result = navidrome_prepare_authentication(
        salt,
        token,
        encoded_username
    );

    if (result != ESP_OK)
    {
        return result;
    }

    /* ---------------------------------------------------------------------- */
    /* Build getRandomSongs request                                           */
    /* ---------------------------------------------------------------------- */

    size_t base_length =
        strlen(g_navidrome_base_url);

    const char *separator =
        (
            base_length > 0U &&
            g_navidrome_base_url[
                base_length - 1U
            ] == '/'
        )
        ? ""
        : "/";

    int written = snprintf(
        url,
        sizeof(url),

        "%s%srest/getRandomSongs.view"
        "?u=%s"
        "&t=%s"
        "&s=%s"
        "&v=%s"
        "&c=%s"
        "&f=json"
        "&size=1",

        g_navidrome_base_url,
        separator,

        encoded_username,
        token,
        salt,

        NAVIDROME_API_VERSION,
        NAVIDROME_CLIENT_NAME
    );

    if (written < 0 ||
        (size_t)written >= sizeof(url))
    {
        ESP_LOGE(
            TAG,
            "Random-song URL is too long"
        );

        return ESP_ERR_INVALID_SIZE;
    }

    /* ---------------------------------------------------------------------- */
    /* Allocate response buffer                                               */
    /* ---------------------------------------------------------------------- */

    char *response_buffer =
        calloc(
            NAVIDROME_SONG_RESPONSE_SIZE,
            sizeof(char)
        );

    if (response_buffer == NULL)
    {
        ESP_LOGE(
            TAG,
            "Could not allocate song response buffer"
        );

        return ESP_ERR_NO_MEM;
    }

    /* ---------------------------------------------------------------------- */
    /* Request song                                                           */
    /* ---------------------------------------------------------------------- */

    ESP_LOGI(
        TAG,
        "Requesting one random song"
    );

    result = navidrome_http_get(
        url,
        response_buffer,
        NAVIDROME_SONG_RESPONSE_SIZE
    );

    if (result != ESP_OK)
    {
        free(
            response_buffer
        );

        return result;
    }

    /* ---------------------------------------------------------------------- */
    /* Parse JSON                                                             */
    /* ---------------------------------------------------------------------- */

    cJSON *root =
        cJSON_Parse(
            response_buffer
        );

    free(
        response_buffer
    );

    if (root == NULL)
    {
        ESP_LOGE(
            TAG,
            "Could not parse random-song JSON"
        );

        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *subsonic_response =
        cJSON_GetObjectItemCaseSensitive(
            root,
            "subsonic-response"
        );

    result =
        navidrome_check_api_status(
            subsonic_response
        );

    if (result != ESP_OK)
    {
        cJSON_Delete(
            root
        );

        return result;
    }

    /* ---------------------------------------------------------------------- */
    /* randomSongs                                                            */
    /* ---------------------------------------------------------------------- */

    cJSON *random_songs =
        cJSON_GetObjectItemCaseSensitive(
            subsonic_response,
            "randomSongs"
        );

    if (!cJSON_IsObject(random_songs))
    {
        ESP_LOGE(
            TAG,
            "randomSongs object missing"
        );

        cJSON_Delete(
            root
        );

        return ESP_ERR_INVALID_RESPONSE;
    }

    /* ---------------------------------------------------------------------- */
    /* song array                                                             */
    /* ---------------------------------------------------------------------- */

    cJSON *song_array =
        cJSON_GetObjectItemCaseSensitive(
            random_songs,
            "song"
        );

    if (!cJSON_IsArray(song_array))
    {
        ESP_LOGE(
            TAG,
            "song array missing"
        );

        cJSON_Delete(
            root
        );

        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *song_object =
        cJSON_GetArrayItem(
            song_array,
            0
        );

    if (!cJSON_IsObject(song_object))
    {
        ESP_LOGE(
            TAG,
            "Navidrome returned no songs"
        );

        cJSON_Delete(
            root
        );

        return ESP_ERR_NOT_FOUND;
    }

    /* ---------------------------------------------------------------------- */
    /* Song ID                                                                */
    /* ---------------------------------------------------------------------- */

    cJSON *id_item =
        cJSON_GetObjectItemCaseSensitive(
            song_object,
            "id"
        );

    if (!cJSON_IsString(id_item) ||
        id_item->valuestring == NULL)
    {
        ESP_LOGE(
            TAG,
            "Song ID missing"
        );

        cJSON_Delete(
            root
        );

        return ESP_ERR_INVALID_RESPONSE;
    }

    navidrome_copy_text(
        song->id,
        sizeof(song->id),
        id_item->valuestring
    );

    /* ---------------------------------------------------------------------- */
    /* Title                                                                  */
    /* ---------------------------------------------------------------------- */

    cJSON *title_item =
        cJSON_GetObjectItemCaseSensitive(
            song_object,
            "title"
        );

    if (cJSON_IsString(title_item) &&
        title_item->valuestring != NULL)
    {
        navidrome_copy_text(
            song->title,
            sizeof(song->title),
            title_item->valuestring
        );
    }
    else
    {
        navidrome_copy_text(
            song->title,
            sizeof(song->title),
            "Unknown"
        );
    }

    /* ---------------------------------------------------------------------- */
    /* Artist                                                                 */
    /* ---------------------------------------------------------------------- */

    cJSON *artist_item =
        cJSON_GetObjectItemCaseSensitive(
            song_object,
            "artist"
        );

    if (cJSON_IsString(artist_item) &&
        artist_item->valuestring != NULL)
    {
        navidrome_copy_text(
            song->artist,
            sizeof(song->artist),
            artist_item->valuestring
        );
    }
    else
    {
        navidrome_copy_text(
            song->artist,
            sizeof(song->artist),
            "Unknown"
        );
    }

    /* ---------------------------------------------------------------------- */
    /* Album                                                                  */
    /* ---------------------------------------------------------------------- */

    cJSON *album_item =
        cJSON_GetObjectItemCaseSensitive(
            song_object,
            "album"
        );

    if (cJSON_IsString(album_item) &&
        album_item->valuestring != NULL)
    {
        navidrome_copy_text(
            song->album,
            sizeof(song->album),
            album_item->valuestring
        );
    }
    else
    {
        navidrome_copy_text(
            song->album,
            sizeof(song->album),
            "Unknown"
        );
    }

    /* ---------------------------------------------------------------------- */
    /* Duration                                                               */
    /* ---------------------------------------------------------------------- */

    cJSON *duration_item =
        cJSON_GetObjectItemCaseSensitive(
            song_object,
            "duration"
        );

    if (cJSON_IsNumber(duration_item))
    {
        song->duration_seconds =
            (uint32_t)duration_item->valuedouble;
    }
    else
    {
        song->duration_seconds =
            0U;
    }

    cJSON_Delete(
        root
    );

    /* ---------------------------------------------------------------------- */
    /* Result                                                                 */
    /* ---------------------------------------------------------------------- */

    ESP_LOGI(
        TAG,
        "Random song selected"
    );

    ESP_LOGI(
        TAG,
        "ID: %s",
        song->id
    );

    ESP_LOGI(
        TAG,
        "Title: %s",
        song->title
    );

    ESP_LOGI(
        TAG,
        "Artist: %s",
        song->artist
    );

    ESP_LOGI(
        TAG,
        "Album: %s",
        song->album
    );

    ESP_LOGI(
        TAG,
        "Duration: %lu:%02lu",
        (unsigned long)(
            song->duration_seconds / 60U
        ),
        (unsigned long)(
            song->duration_seconds % 60U
        )
    );

    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/*                              Connection state                              */
/* -------------------------------------------------------------------------- */

bool navidrome_is_connected(void)
{
    return g_navidrome_connected;
}