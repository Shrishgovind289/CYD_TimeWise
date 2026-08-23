/*
 * TimeWise - main.c
 *
 * Normal mode:
 *   - Update time/date once per minute.
 *   - Run alarm scheduling.
 *   - Request weather every 30 minutes.
 *   - Serve the WebSocket control page.
 *
 * Music Enable = 0:
 *   - Normal TimeWise clock/weather/alarm mode.
 *   - MusicPlayer is fully disabled.
 *
 * Music Enable = 1:
 *   - Dedicated music-only operating mode.
 *   - No clock/date/weather/alarm scheduling or dashboard updates.
 *   - Render the Music screen, then stop LVGL and release its task stack.
 *   - Replace the full server with the lightweight music WebSocket.
 *   - Navidrome/MusicPlayer are the only application-level workload.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "nvs_flash.h"

#include "esp_err.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"

#include "Audio.h"
#include "MusicPlayer.h"
#include "NavidromeClient.h"
#include "NTPClock.h"
#include "SDCard.h"
#include "TFT_Display.h"
#include "WeatherStation.h"
#include "WebSocketControl.h"

static const char *TAG = "TimeWise";

/* -------------------------------------------------------------------------- */
/*                           Application configuration                        */
/* -------------------------------------------------------------------------- */

/* Restore your existing private Wi-Fi values locally. */
#define WIFI_SSID                       "WIFI-SSID"
#define WIFI_PASS                       "WIFI-PASSWORD"

#define WEATHER_LATITUDE                40.7282
#define WEATHER_LONGITUDE              -74.0776
#define WEATHER_LOCATION_NAME          "Jersey City, New Jersey"

#define LOCAL_TIMEZONE                  "EST5EDT,M3.2.0/2,M11.1.0/2"

#define WIFI_MAX_RETRIES                10
#define NTP_RETRY_COUNT                 30

#define WIFI_CONNECTED_BIT              BIT0
#define WIFI_FAILED_BIT                 BIT1

#define MAIN_LOOP_PERIOD_MS             1000U
#define VALID_LOCAL_YEAR                2024

#define ALARM_START_SECOND              5
#define ALARM_START_WINDOW_END_SECOND   50
#define ALARM_RING_DURATION_SECONDS     45

#define WEATHER_REQUEST_SECOND          50

#define WEATHER_TASK_STACK_SIZE         8192U
#define WEATHER_TASK_PRIORITY           3U
#define WEATHER_TASK_CORE               0

#define NAVIDROME_TASK_STACK_SIZE       8192U
#define NAVIDROME_TASK_PRIORITY         3U
#define NAVIDROME_TASK_CORE             0

/* Allow deleted Navidrome task memory to be reclaimed before music startup. */
#define MUSIC_START_GRACE_MS            1000U

/* Allow the Now Playing screen to physically flush before LVGL is paused. */
#define MUSIC_DISPLAY_SETTLE_MS         200U

/* Allow WebSocket server resources to be released before MusicPlayer starts. */
#define MUSIC_SERVICE_RELEASE_MS        500U

/* Allow MusicPlayer cleanup/task deletion to finish before restoring services. */
#define MUSIC_CLEANUP_GRACE_MS          250U

/* -------------------------------------------------------------------------- */
/*                                 Wi-Fi state                                */
/* -------------------------------------------------------------------------- */

static EventGroupHandle_t s_wifi_event_group = NULL;
static int s_wifi_retry_count = 0;

/* -------------------------------------------------------------------------- */
/*                               Weather state                                */
/* -------------------------------------------------------------------------- */

static volatile bool s_weather_request_running = false;
static volatile bool s_weather_request_due = false;

static portMUX_TYPE s_weather_data_lock = portMUX_INITIALIZER_UNLOCKED;
static weatherstation_update_t s_pending_weather = {0};
static bool s_pending_weather_valid = false;

/* -------------------------------------------------------------------------- */
/*                              Navidrome state                               */
/* -------------------------------------------------------------------------- */

static volatile bool s_navidrome_task_running = false;

/* -------------------------------------------------------------------------- */
/*                                Music state                                 */
/* -------------------------------------------------------------------------- */

static portMUX_TYPE s_music_request_lock = portMUX_INITIALIZER_UNLOCKED;
static navidrome_song_t s_pending_random_song = {0};
static volatile bool s_random_song_pending = false;
static volatile TickType_t s_random_song_ready_tick = 0;

/* One-song history for the WebSocket Previous command. */
static navidrome_song_t s_last_started_song = {0};
static navidrome_song_t s_previous_song = {0};
static bool s_last_started_song_valid = false;
static bool s_previous_song_valid = false;

typedef enum
{
    MUSIC_FOLLOWUP_NONE = 0,
    MUSIC_FOLLOWUP_RANDOM,
    MUSIC_FOLLOWUP_PREVIOUS
} music_followup_action_t;

static volatile music_followup_action_t s_music_followup_action =
    MUSIC_FOLLOWUP_NONE;

/*
 * Armed only after MusicPlayer successfully starts a track.
 *
 * If that track reaches MUSIC_PLAYER_STATE_STOPPED naturally, main.c requests
 * another random Navidrome song. Explicit Stop/Next/Previous/Disable commands
 * disarm this first so they cannot accidentally trigger the natural-end path.
 */
static bool s_music_auto_advance_armed = false;

/* True whenever music_enabled is the active top-level TimeWise operating mode. */
static bool s_music_mode_active = false;

/*
 * If the full control server cannot be recreated immediately after music,
 * normal TimeWise operation still resumes and main.c retries the server later.
 */
static bool s_full_websocket_retry_due = false;

/* -------------------------------------------------------------------------- */
/*                              Text helper                                   */
/* -------------------------------------------------------------------------- */

static void copy_text(char *destination, size_t destination_size, const char *source)
{
    if (destination == NULL || destination_size == 0U)
    {
        return;
    }

    snprintf(destination, destination_size, "%s", source != NULL ? source : "");
}

/* -------------------------------------------------------------------------- */
/*                            Wi-Fi management                                */
/* -------------------------------------------------------------------------- */

static void wifi_event_handler(void *handler_argument, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)handler_argument;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        if (s_wifi_event_group != NULL)
        {
            xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }

        if (s_wifi_retry_count < WIFI_MAX_RETRIES)
        {
            s_wifi_retry_count++;
            ESP_LOGW(TAG, "Wi-Fi reconnect attempt %d/%d", s_wifi_retry_count, WIFI_MAX_RETRIES);
            esp_wifi_connect();
        }
        else if (s_wifi_event_group != NULL)
        {
            ESP_LOGE(TAG, "Wi-Fi connection failed");
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAILED_BIT);
        }

        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        const ip_event_got_ip_t *got_ip_event = (const ip_event_got_ip_t *)event_data;

        s_wifi_retry_count = 0;

        if (s_wifi_event_group != NULL)
        {
            xEventGroupClearBits(s_wifi_event_group, WIFI_FAILED_BIT);
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }

        if (got_ip_event != NULL)
        {
            ESP_LOGI(TAG, "Wi-Fi connected: http://" IPSTR "/", IP2STR(&got_ip_event->ip_info.ip));
        }
    }
}

static esp_err_t initialize_nvs(void)
{
    esp_err_t result = nvs_flash_init();

    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        result = nvs_flash_init();
    }

    return result;
}

static esp_err_t connect_wifi(void)
{
    esp_err_t result = initialize_nvs();

    if (result != ESP_OK)
    {
        return result;
    }

    result = esp_netif_init();

    if (result != ESP_OK)
    {
        return result;
    }

    result = esp_event_loop_create_default();

    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE)
    {
        return result;
    }

    s_wifi_event_group = xEventGroupCreate();

    if (s_wifi_event_group == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    if (esp_netif_create_default_wifi_sta() == NULL)
    {
        return ESP_FAIL;
    }

    wifi_init_config_t initialization = WIFI_INIT_CONFIG_DEFAULT();

    result = esp_wifi_init(&initialization);

    if (result != ESP_OK)
    {
        return result;
    }

    result = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);

    if (result != ESP_OK)
    {
        return result;
    }

    result = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);

    if (result != ESP_OK)
    {
        return result;
    }

    wifi_config_t configuration = {0};

    copy_text((char *)configuration.sta.ssid, sizeof(configuration.sta.ssid), WIFI_SSID);
    copy_text((char *)configuration.sta.password, sizeof(configuration.sta.password), WIFI_PASS);

    configuration.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    configuration.sta.pmf_cfg.capable = true;
    configuration.sta.pmf_cfg.required = false;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "Could not set Wi-Fi mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &configuration), TAG, "Could not set Wi-Fi config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Could not start Wi-Fi");

    ESP_LOGI(TAG, "Connecting to Wi-Fi");

    EventBits_t result_bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAILED_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(30000)
    );

    if ((result_bits & WIFI_CONNECTED_BIT) != 0U)
    {
        return ESP_OK;
    }

    if ((result_bits & WIFI_FAILED_BIT) != 0U)
    {
        return ESP_FAIL;
    }

    return ESP_ERR_TIMEOUT;
}

/* -------------------------------------------------------------------------- */
/*                              Time helpers                                  */
/* -------------------------------------------------------------------------- */

static bool get_local_time(struct tm *time_info)
{
    if (time_info == NULL)
    {
        return false;
    }

    time_t now = 0;

    time(&now);
    localtime_r(&now, time_info);

    return time_info->tm_year >= (VALID_LOCAL_YEAR - 1900);
}

static int64_t make_minute_key(const struct tm *time_info)
{
    int64_t key = time_info->tm_year;

    key = key * 367 + time_info->tm_yday;
    key = key * 24 + time_info->tm_hour;
    key = key * 60 + time_info->tm_min;

    return key;
}

static int64_t make_day_key(const struct tm *time_info)
{
    int64_t key = time_info->tm_year;

    key = key * 367 + time_info->tm_yday;

    return key;
}

static int64_t make_weather_slot_key(const struct tm *time_info)
{
    int64_t key = make_day_key(time_info);

    key = key * 48;
    key += time_info->tm_hour * 2;
    key += time_info->tm_min / 30;

    return key;
}

static void update_time_display(const struct tm *time_info)
{
    char time_text[16];
    char date_text[32];

    strftime(time_text, sizeof(time_text), "%I:%M %p", time_info);
    strftime(date_text, sizeof(date_text), "%a, %b %d", time_info);

    tft_dashboard_set_time(time_text);
    tft_dashboard_set_date(date_text);
}

/* -------------------------------------------------------------------------- */
/*                            Weather handling                                */
/* -------------------------------------------------------------------------- */

static void weather_update_callback(const weatherstation_update_t *update, void *user_data)
{
    (void)user_data;

    if (update == NULL)
    {
        return;
    }

    if (websocket_control_get_music_enabled())
    {
        ESP_LOGI(TAG, "Ignoring weather response while Music Mode is enabled");
        return;
    }

    portENTER_CRITICAL(&s_weather_data_lock);
    s_pending_weather = *update;
    s_pending_weather_valid = true;
    portEXIT_CRITICAL(&s_weather_data_lock);

    ESP_LOGI(TAG, "Weather response ready for display: %s", update->condition);
}

static void apply_pending_weather(void)
{
    if (s_music_mode_active ||
        music_player_is_exclusive_mode() ||
        music_player_is_playing() ||
        s_random_song_pending ||
        audio_is_playing() ||
        ntpclock_alarm_is_ringing())
    {
        return;
    }

    weatherstation_update_t update = {0};
    bool update_available = false;

    portENTER_CRITICAL(&s_weather_data_lock);

    if (s_pending_weather_valid)
    {
        update = s_pending_weather;
        s_pending_weather_valid = false;
        update_available = true;
    }

    portEXIT_CRITICAL(&s_weather_data_lock);

    if (!update_available)
    {
        return;
    }

    tft_dashboard_set_weather(
        update.temperature_c,
        update.condition,
        update.location,
        update.background_path,
        update.icon_path
    );

    tft_dashboard_set_wind(update.wind_speed_kmh, update.wind_direction_degrees);

    tft_dashboard_set_astro(
        update.current_time,
        update.sunrise_time,
        update.sunset_time,
        update.moonrise_time,
        update.moonset_time
    );

    ESP_LOGI(TAG, "Weather display updated: %s", update.condition);
}

static void weather_request_task(void *argument)
{
    (void)argument;

    ESP_LOGI(TAG, "Starting one-shot weather request");

    esp_err_t result = weatherstation_request_once();

    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "Weather request failed: %s", esp_err_to_name(result));
    }

    s_weather_request_running = false;

    vTaskDelete(NULL);
}

static esp_err_t start_weather_request_task(void)
{
    if (s_weather_request_running ||
        websocket_control_get_music_enabled() ||
        s_music_mode_active ||
        music_player_is_playing() ||
        music_player_is_exclusive_mode())
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_weather_request_running = true;

    BaseType_t task_result = xTaskCreatePinnedToCore(
        weather_request_task,
        "weather_request",
        WEATHER_TASK_STACK_SIZE,
        NULL,
        WEATHER_TASK_PRIORITY,
        NULL,
        WEATHER_TASK_CORE
    );

    if (task_result != pdPASS)
    {
        s_weather_request_running = false;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/*                          Music service control                             */
/* -------------------------------------------------------------------------- */

static esp_err_t enter_music_mode(void)
{
    if (s_music_mode_active)
    {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Music Enable=1 -> entering dedicated Music Mode");

    /*
     * From this point onward main.c will not run clock, alarm scheduling,
     * weather scheduling, weather display updates, or astronomy updates.
     */
    s_weather_request_due = false;

    portENTER_CRITICAL(&s_weather_data_lock);
    s_pending_weather_valid = false;
    portEXIT_CRITICAL(&s_weather_data_lock);

    /*
     * If an alarm was already active when Music Mode was enabled, stop it.
     * The music request waits for Audio.c to become idle before Navidrome/MP3.
     */
    if (ntpclock_alarm_is_ringing() || audio_is_playing())
    {
        ntpclock_stop_alarm();
    }

    /*
     * Draw the Music Mode screen while LVGL is still alive. After this frame
     * reaches the LCD, tft_display_set_paused(true) stops the LVGL tick and
     * deletes the LVGL task so its 16 KB stack is returned to the heap.
     */
    tft_dashboard_show_music(
        "Music Mode",
        "Loading music...",
        0U
    );

    vTaskDelay(
        pdMS_TO_TICKS(
            MUSIC_DISPLAY_SETTLE_MS
        )
    );

    tft_display_set_paused(
        true
    );

    /*
     * Replace the full TimeWise server with the lightweight music server.
     * The browser reconnects automatically and only music commands remain
     * active until Music Enable becomes 0.
     */
    esp_err_t result =
        websocket_control_enter_music_mode();

    if (result != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Could not start Music Mode WebSocket: %s",
            esp_err_to_name(result)
        );

        /*
         * Restore the normal control server so the user still has a way to
         * change Music Enable back to 0. The master flag remains enabled, so
         * main.c will retry entering Music Mode on the next scheduler pass.
         */
        tft_display_set_paused(
            false
        );

        tft_dashboard_hide_music();

        websocket_control_start();

        return result;
    }

    s_music_mode_active =
        true;

    /*
     * Enabling Music Mode starts one random track automatically. After a
     * track stops naturally or the Stop button is used, Music Mode remains
     * active but no new track is started until Play Random/Next/Previous.
     */
    if (s_music_followup_action == MUSIC_FOLLOWUP_NONE)
    {
        s_music_followup_action =
            MUSIC_FOLLOWUP_RANDOM;
    }

    ESP_LOGI(
        TAG,
        "Dedicated Music Mode ready"
    );

    return ESP_OK;
}

static esp_err_t exit_music_mode(void)
{
    if (!s_music_mode_active)
    {
        return ESP_OK;
    }

    if (music_player_is_playing() ||
        music_player_is_exclusive_mode() ||
        s_navidrome_task_running)
    {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(
        TAG,
        "Music Enable=0 -> leaving dedicated Music Mode"
    );

    s_music_followup_action =
        MUSIC_FOLLOWUP_NONE;

    s_music_auto_advance_armed =
        false;

    portENTER_CRITICAL(
        &s_music_request_lock
    );

    s_random_song_pending =
        false;

    memset(
        &s_pending_random_song,
        0,
        sizeof(s_pending_random_song)
    );

    portEXIT_CRITICAL(
        &s_music_request_lock
    );

    /*
     * Normal application scheduling is restored regardless of whether the
     * larger full WebSocket server can be allocated on this exact pass.
     */
    s_music_mode_active =
        false;

    s_weather_request_due =
        true;

    tft_display_set_paused(
        false
    );

    tft_dashboard_hide_music();

    /*
     * Force the clock/date to refresh immediately rather than waiting for the
     * next minute boundary.
     */
    struct tm now = {0};

    if (get_local_time(
            &now))
    {
        update_time_display(
            &now
        );
    }

    esp_err_t websocket_result =
        websocket_control_exit_music_mode();

    if (websocket_result != ESP_OK)
    {
        s_full_websocket_retry_due =
            true;

        ESP_LOGW(
            TAG,
            "Normal mode restored, but full WebSocket server will be retried: %s",
            esp_err_to_name(websocket_result)
        );
    }
    else
    {
        s_full_websocket_retry_due =
            false;
    }

    ESP_LOGI(
        TAG,
        "Normal TimeWise mode restored"
    );

    return websocket_result;
}

/* -------------------------------------------------------------------------- */
/*                         Random-song handoff                                */
/* -------------------------------------------------------------------------- */

static void queue_random_song(const navidrome_song_t *song)
{
    if (song == NULL)
    {
        return;
    }

    TickType_t ready_tick = xTaskGetTickCount();

    portENTER_CRITICAL(&s_music_request_lock);
    s_pending_random_song = *song;
    s_random_song_ready_tick = ready_tick;
    s_random_song_pending = true;
    portEXIT_CRITICAL(&s_music_request_lock);

    ESP_LOGI(TAG, "Random song queued for MusicPlayer");
}

static void start_pending_random_song(void)
{
    if (!websocket_control_get_music_enabled() ||
        !s_music_mode_active ||
        !s_random_song_pending ||
        s_navidrome_task_running ||
        s_weather_request_running ||
        ntpclock_alarm_is_ringing() ||
        audio_is_playing() ||
        music_player_is_playing() ||
        music_player_is_exclusive_mode())
    {
        return;
    }

    TickType_t current_tick =
        xTaskGetTickCount();

    TickType_t ready_tick =
        s_random_song_ready_tick;

    if ((TickType_t)(current_tick - ready_tick) <
        pdMS_TO_TICKS(MUSIC_START_GRACE_MS))
    {
        return;
    }

    navidrome_song_t song = {0};

    portENTER_CRITICAL(
        &s_music_request_lock
    );

    if (s_random_song_pending)
    {
        song =
            s_pending_random_song;
    }

    portEXIT_CRITICAL(
        &s_music_request_lock
    );

    if (song.id[0] == '\0')
    {
        return;
    }

    ESP_LOGI(
        TAG,
        "Starting queued music: %s - %s",
        song.artist,
        song.title
    );

    /*
     * LVGL is normally deleted in Music Mode to free RAM. Recreate it only
     * long enough to draw the new song metadata, then delete it again before
     * allocating the MP3 decoder.
     */
    tft_display_set_paused(
        false
    );

    tft_dashboard_show_music(
        song.title,
        song.artist,
        song.duration_seconds
    );

    vTaskDelay(
        pdMS_TO_TICKS(
            MUSIC_DISPLAY_SETTLE_MS
        )
    );

    tft_display_set_paused(
        true
    );

    /*
     * Give the idle tasks time to reclaim the temporary LVGL task and the
     * completed Navidrome task before MusicPlayer allocates DAC/decoder RAM.
     */
    vTaskDelay(
        pdMS_TO_TICKS(
            MUSIC_SERVICE_RELEASE_MS
        )
    );

    esp_err_t result =
        music_player_start(
            &song
        );

    if (result != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Could not start MusicPlayer: %s",
            esp_err_to_name(result)
        );

        portENTER_CRITICAL(
            &s_music_request_lock
        );

        s_random_song_pending =
            false;

        portEXIT_CRITICAL(
            &s_music_request_lock
        );

        /*
         * Music Enable is still 1, therefore remain in Music Mode. Do not
         * restore clock/weather/alarm services because playback failed.
         */
        return;
    }

    portENTER_CRITICAL(
        &s_music_request_lock
    );

    s_random_song_pending =
        false;

    portEXIT_CRITICAL(
        &s_music_request_lock
    );

    if (s_last_started_song_valid &&
        strcmp(s_last_started_song.id, song.id) != 0)
    {
        s_previous_song =
            s_last_started_song;

        s_previous_song_valid =
            true;
    }

    s_last_started_song =
        song;

    s_last_started_song_valid =
        true;

    /*
     * This track is now eligible for natural-end auto advance.
     */
    s_music_auto_advance_armed =
        true;

    ESP_LOGI(
        TAG,
        "MusicPlayer started: %s - %s",
        song.artist,
        song.title
    );
}

/* -------------------------------------------------------------------------- */
/*                           Navidrome startup                                */
/* -------------------------------------------------------------------------- */

static void navidrome_startup_task(void *argument)
{
    (void)argument;

    if (!websocket_control_get_music_enabled())
    {
        s_navidrome_task_running = false;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Testing Navidrome connection");

    esp_err_t result =
        navidrome_ping();

    if (result != ESP_OK)
    {
        ESP_LOGW(
            TAG,
            "Navidrome unavailable: %s",
            esp_err_to_name(result)
        );

        s_navidrome_task_running = false;
        vTaskDelete(NULL);
        return;
    }

    if (!websocket_control_get_music_enabled())
    {
        s_navidrome_task_running = false;
        vTaskDelete(NULL);
        return;
    }

    navidrome_song_t song = {0};

    result =
        navidrome_get_random_song(
            &song
        );

    if (result != ESP_OK)
    {
        ESP_LOGW(
            TAG,
            "Could not retrieve random song: %s",
            esp_err_to_name(result)
        );

        s_navidrome_task_running = false;
        vTaskDelete(NULL);
        return;
    }

    /*
     * Music may have been disabled while the HTTP request was in progress.
     * In that case discard the result completely.
     */
    if (!websocket_control_get_music_enabled())
    {
        ESP_LOGI(
            TAG,
            "Discarding Navidrome result because Music Mode was disabled"
        );

        s_navidrome_task_running = false;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Navidrome song ready");
    ESP_LOGI(TAG, "Title: %s", song.title);
    ESP_LOGI(TAG, "Artist: %s", song.artist);
    ESP_LOGI(
        TAG,
        "Duration: %lu:%02lu",
        (unsigned long)(song.duration_seconds / 60U),
        (unsigned long)(song.duration_seconds % 60U)
    );

    queue_random_song(
        &song
    );

    /*
     * Do not start MusicPlayer from this task. Let this task delete itself
     * first so its stack is returned to the heap.
     */
    s_navidrome_task_running =
        false;

    ESP_LOGI(
        TAG,
        "Navidrome task finished; waiting %u ms before music startup",
        MUSIC_START_GRACE_MS
    );

    vTaskDelete(NULL);
}

static esp_err_t start_navidrome_startup_task(void)
{
    if (s_navidrome_task_running)
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_navidrome_task_running = true;

    BaseType_t task_result = xTaskCreatePinnedToCore(
        navidrome_startup_task,
        "navidrome_startup",
        NAVIDROME_TASK_STACK_SIZE,
        NULL,
        NAVIDROME_TASK_PRIORITY,
        NULL,
        NAVIDROME_TASK_CORE
    );

    if (task_result != pdPASS)
    {
        s_navidrome_task_running = false;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/*                       WebSocket music command bridge                       */
/* -------------------------------------------------------------------------- */

static esp_err_t timewise_music_action_handler(
    websocket_music_action_t action,
    int32_t value,
    void *user_data)
{
    (void)user_data;

    switch (action)
    {
        case WEBSOCKET_MUSIC_ACTION_ENABLE:
        {
            if (value != 0)
            {
                /*
                 * Music Enable=1 is the master mode command. The WebSocket
                 * module updates its flag immediately after this callback;
                 * main.c sees it on the next scheduler pass and shuts down
                 * normal TimeWise processing before starting this request.
                 */
                s_music_followup_action =
                    MUSIC_FOLLOWUP_RANDOM;

                return ESP_OK;
            }

            /*
             * Music Enable=0 fully disables MusicPlayer activity.
             */
            s_music_followup_action =
                MUSIC_FOLLOWUP_NONE;

            s_music_auto_advance_armed =
                false;

            portENTER_CRITICAL(
                &s_music_request_lock
            );

            s_random_song_pending =
                false;

            memset(
                &s_pending_random_song,
                0,
                sizeof(s_pending_random_song)
            );

            portEXIT_CRITICAL(
                &s_music_request_lock
            );

            if (music_player_is_playing() ||
                music_player_is_exclusive_mode())
            {
                music_player_stop();
            }

            return ESP_OK;
        }

        case WEBSOCKET_MUSIC_ACTION_PLAY_RANDOM:
        {
            if (!websocket_control_get_music_enabled())
            {
                return ESP_ERR_INVALID_STATE;
            }

            if (music_player_is_playing() ||
                music_player_is_exclusive_mode() ||
                s_navidrome_task_running ||
                s_random_song_pending)
            {
                return ESP_ERR_INVALID_STATE;
            }

            s_music_auto_advance_armed =
                false;

            s_music_followup_action =
                MUSIC_FOLLOWUP_RANDOM;

            return ESP_OK;
        }

        case WEBSOCKET_MUSIC_ACTION_PAUSE:
            return music_player_pause();

        case WEBSOCKET_MUSIC_ACTION_RESUME:
            return music_player_resume();

        case WEBSOCKET_MUSIC_ACTION_NEXT:
        {
            if (!websocket_control_get_music_enabled())
            {
                return ESP_ERR_INVALID_STATE;
            }

            s_music_auto_advance_armed =
                false;

            s_music_followup_action =
                MUSIC_FOLLOWUP_RANDOM;

            if (music_player_is_playing() ||
                music_player_is_exclusive_mode())
            {
                music_player_stop();
            }

            return ESP_OK;
        }

        case WEBSOCKET_MUSIC_ACTION_PREVIOUS:
        {
            if (!websocket_control_get_music_enabled())
            {
                return ESP_ERR_INVALID_STATE;
            }

            if (!s_previous_song_valid)
            {
                return ESP_ERR_NOT_FOUND;
            }

            s_music_auto_advance_armed =
                false;

            s_music_followup_action =
                MUSIC_FOLLOWUP_PREVIOUS;

            if (music_player_is_playing() ||
                music_player_is_exclusive_mode())
            {
                music_player_stop();
            }

            return ESP_OK;
        }

        case WEBSOCKET_MUSIC_ACTION_STOP:
        {
            s_music_auto_advance_armed =
                false;

            s_music_followup_action =
                MUSIC_FOLLOWUP_NONE;

            portENTER_CRITICAL(
                &s_music_request_lock
            );

            s_random_song_pending =
                false;

            portEXIT_CRITICAL(
                &s_music_request_lock
            );

            if (music_player_is_playing() ||
                music_player_is_exclusive_mode())
            {
                music_player_stop();
            }

            /*
             * Stop only stops the track. Music Enable remains 1, therefore
             * the system stays in dedicated Music Mode.
             */
            return ESP_OK;
        }

        default:
            return ESP_ERR_NOT_SUPPORTED;
    }
}

static void process_music_followup_request(void)
{
    if (!websocket_control_get_music_enabled() ||
        !s_music_mode_active)
    {
        s_music_followup_action =
            MUSIC_FOLLOWUP_NONE;

        return;
    }

    music_followup_action_t action =
        s_music_followup_action;

    if (action == MUSIC_FOLLOWUP_NONE)
    {
        return;
    }

    /*
     * Absolutely no music/network allocation is started until any old normal
     * TimeWise work has finished releasing its resources.
     */
    if (music_player_is_playing() ||
        music_player_is_exclusive_mode() ||
        s_navidrome_task_running ||
        s_random_song_pending ||
        s_weather_request_running ||
        ntpclock_alarm_is_ringing() ||
        audio_is_playing())
    {
        return;
    }

    s_music_followup_action =
        MUSIC_FOLLOWUP_NONE;

    if (action == MUSIC_FOLLOWUP_PREVIOUS)
    {
        if (s_previous_song_valid)
        {
            queue_random_song(
                &s_previous_song
            );
        }

        return;
    }

    esp_err_t result =
        start_navidrome_startup_task();

    if (result != ESP_OK &&
        result != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(
            TAG,
            "Could not start Navidrome music request: %s",
            esp_err_to_name(result)
        );
    }
}

/* -------------------------------------------------------------------------- */
/*                               Application                                  */
/* -------------------------------------------------------------------------- */

void app_main(void)
{
    ESP_LOGI(TAG, "TimeWise starting");
    ESP_LOGI(TAG, "Reset reason: %d", (int)esp_reset_reason());

    vTaskDelay(
        pdMS_TO_TICKS(
            500
        )
    );

    esp_err_t result =
        sdcard_init();

    if (result != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "SD card initialization failed: %s",
            esp_err_to_name(result)
        );

        return;
    }

    result =
        audio_init();

    if (result != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Audio initialization failed: %s",
            esp_err_to_name(result)
        );

        return;
    }

    ESP_ERROR_CHECK(
        tft_display_init()
    );

    ESP_ERROR_CHECK(
        tft_dashboard_create()
    );

    result =
        connect_wifi();

    if (result != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Wi-Fi unavailable: %s",
            esp_err_to_name(result)
        );

        return;
    }

    result =
        ntpclock_init(
            LOCAL_TIMEZONE,
            NTP_RETRY_COUNT
        );

    if (result != ESP_OK)
    {
        ESP_LOGW(
            TAG,
            "Initial NTP sync did not complete: %s",
            esp_err_to_name(result)
        );
    }

    result =
        weatherstation_init(
            WEATHER_LATITUDE,
            WEATHER_LONGITUDE,
            WEATHER_LOCATION_NAME,
            weather_update_callback,
            NULL
        );

    if (result != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "WeatherStation initialization failed: %s",
            esp_err_to_name(result)
        );

        return;
    }

    ESP_ERROR_CHECK(
        ntpclock_set_alarm(
            7,
            45,
            true
        )
    );

    websocket_control_set_music_action_callback(
        timewise_music_action_handler,
        NULL
    );

    result =
        websocket_control_start();

    if (result != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "WebSocket control server failed: %s",
            esp_err_to_name(result)
        );

        return;
    }

    /*
     * Music is disabled by default in WebSocketControl.c. The normal TimeWise
     * dashboard therefore starts first. MusicPlayer is not touched until the
     * user sets Music Enable to 1.
     */
    ESP_LOGI(
        TAG,
        "Music Enable=%d at startup",
        websocket_control_get_music_enabled() ? 1 : 0
    );

    int64_t last_display_minute =
        -1;

    int64_t last_alarm_minute =
        -1;

    int64_t last_weather_slot =
        -1;

    time_t alarm_stop_deadline =
        0;

    s_weather_request_due =
        true;

    struct tm startup_time = {0};

    if (get_local_time(
            &startup_time))
    {
        last_weather_slot =
            make_weather_slot_key(
                &startup_time
            );
    }

    TickType_t last_wake_time =
        xTaskGetTickCount();

    ESP_LOGI(
        TAG,
        "Central one-second scheduler started"
    );

    while (true)
    {
        bool music_enabled =
            websocket_control_get_music_enabled();

        /* ================================================================== */
        /*                         MUSIC MODE                                 */
        /* ================================================================== */

        if (music_enabled)
        {
            /*
             * Music Enable is the master mode switch.
             *
             * Nothing below this block related to time, alarm scheduling,
             * weather, astronomy, or the normal dashboard is executed while
             * the flag remains 1.
             */
            if (!s_music_mode_active)
            {
                esp_err_t mode_result =
                    enter_music_mode();

                if (mode_result != ESP_OK)
                {
                    vTaskDelayUntil(
                        &last_wake_time,
                        pdMS_TO_TICKS(
                            MAIN_LOOP_PERIOD_MS
                        )
                    );

                    continue;
                }

                last_wake_time =
                    xTaskGetTickCount();
            }

            process_music_followup_request();

            if (s_random_song_pending &&
                !s_navidrome_task_running &&
                !music_player_is_playing() &&
                !music_player_is_exclusive_mode())
            {
                start_pending_random_song();

                last_wake_time =
                    xTaskGetTickCount();
            }

            /*
             * Natural end-of-track auto advance.
             *
             * Do this AFTER process_music_followup_request() so the random
             * Navidrome request starts on the next scheduler iteration. That
             * leaves roughly one second for the completed MusicPlayer task,
             * decoder, HTTP client and DAC resources to be fully reclaimed.
             */
            if (s_music_auto_advance_armed &&
                !music_player_is_playing() &&
                !music_player_is_exclusive_mode())
            {
                music_player_state_t player_state =
                    music_player_get_state();

                if (player_state == MUSIC_PLAYER_STATE_STOPPED)
                {
                    s_music_auto_advance_armed =
                        false;

                    if (s_music_followup_action == MUSIC_FOLLOWUP_NONE &&
                        !s_navidrome_task_running &&
                        !s_random_song_pending)
                    {
                        s_music_followup_action =
                            MUSIC_FOLLOWUP_RANDOM;

                        ESP_LOGI(
                            TAG,
                            "Song finished; randomly selecting the next song"
                        );
                    }
                }
                else if (player_state == MUSIC_PLAYER_STATE_ERROR)
                {
                    /*
                     * A decode/network failure is not treated as a completed
                     * song. Stay in Music Mode and wait for user input.
                     */
                    s_music_auto_advance_armed =
                        false;

                    ESP_LOGW(
                        TAG,
                        "MusicPlayer ended with an error; auto-next not started"
                    );
                }
            }

            vTaskDelayUntil(
                &last_wake_time,
                pdMS_TO_TICKS(
                    MAIN_LOOP_PERIOD_MS
                )
            );

            continue;
        }

        /* ================================================================== */
        /*                    LEAVE MUSIC MODE                                */
        /* ================================================================== */

        if (s_music_mode_active)
        {
            /*
             * Music Enable=0 means MusicPlayer is fully disabled. Make this
             * true even if the WebSocket callback was missed or playback is
             * currently paused.
             */
            s_music_followup_action =
                MUSIC_FOLLOWUP_NONE;

            portENTER_CRITICAL(
                &s_music_request_lock
            );

            s_random_song_pending =
                false;

            portEXIT_CRITICAL(
                &s_music_request_lock
            );

            if (music_player_is_playing() ||
                music_player_is_exclusive_mode())
            {
                music_player_stop();

                vTaskDelayUntil(
                    &last_wake_time,
                    pdMS_TO_TICKS(
                        MAIN_LOOP_PERIOD_MS
                    )
                );

                continue;
            }

            /*
             * A Navidrome HTTP request already in progress is allowed to
             * finish and discard its result. Do not restore the larger normal
             * services until that worker task has released its stack.
             */
            if (s_navidrome_task_running)
            {
                vTaskDelayUntil(
                    &last_wake_time,
                    pdMS_TO_TICKS(
                        MAIN_LOOP_PERIOD_MS
                    )
                );

                continue;
            }

            vTaskDelay(
                pdMS_TO_TICKS(
                    MUSIC_CLEANUP_GRACE_MS
                )
            );

            esp_err_t mode_result =
                exit_music_mode();

            if (mode_result != ESP_OK &&
                mode_result != ESP_ERR_INVALID_STATE)
            {
                ESP_LOGE(
                    TAG,
                    "Could not leave Music Mode: %s",
                    esp_err_to_name(mode_result)
                );
            }

            last_display_minute =
                -1;

            last_weather_slot =
                -1;

            alarm_stop_deadline =
                0;

            last_wake_time =
                xTaskGetTickCount();

            continue;
        }

        /* ================================================================== */
        /*                       NORMAL TIMEWISE MODE                          */
        /* ================================================================== */

        /*
         * Music Enable is 0 here. No Navidrome task, MP3 decoder, continuous
         * DAC, or MusicPlayer task is allowed to start from this branch.
         */

        if (s_full_websocket_retry_due)
        {
            esp_err_t websocket_result =
                websocket_control_start();

            if (websocket_result == ESP_OK)
            {
                s_full_websocket_retry_due =
                    false;

                ESP_LOGI(
                    TAG,
                    "Full WebSocket server restored"
                );
            }
        }

        struct tm now = {0};

        if (!get_local_time(
                &now))
        {
            tft_dashboard_set_time(
                "--:--"
            );

            tft_dashboard_set_date(
                "Syncing time..."
            );

            vTaskDelayUntil(
                &last_wake_time,
                pdMS_TO_TICKS(
                    MAIN_LOOP_PERIOD_MS
                )
            );

            continue;
        }

        int64_t minute_key =
            make_minute_key(
                &now
            );

        time_t current_time =
            time(
                NULL
            );

        if (minute_key !=
            last_display_minute)
        {
            update_time_display(
                &now
            );

            last_display_minute =
                minute_key;

            ESP_LOGI(
                TAG,
                "Clock display updated at %02d:%02d:%02d",
                now.tm_hour,
                now.tm_min,
                now.tm_sec
            );
        }

        /* ------------------------------------------------------------------ */
        /* Alarm scheduling                                                   */
        /* ------------------------------------------------------------------ */

        int alarm_hour = 0;
        int alarm_minute = 0;
        bool alarm_enabled = false;

        ntpclock_get_alarm(
            &alarm_hour,
            &alarm_minute,
            &alarm_enabled
        );

        bool regular_alarm_due =
            alarm_enabled &&
            now.tm_hour == alarm_hour &&
            now.tm_min == alarm_minute &&
            now.tm_sec >= ALARM_START_SECOND &&
            now.tm_sec < ALARM_START_WINDOW_END_SECOND &&
            minute_key != last_alarm_minute;

        bool snooze_alarm_due =
            ntpclock_snooze_is_due(
                current_time
            );

        if ((regular_alarm_due ||
             snooze_alarm_due) &&
            !ntpclock_alarm_is_ringing() &&
            !audio_is_playing())
        {
            esp_err_t alarm_result =
                ntpclock_trigger_alarm_now();

            if (alarm_result == ESP_OK)
            {
                if (regular_alarm_due)
                {
                    last_alarm_minute =
                        minute_key;
                }

                alarm_stop_deadline =
                    current_time +
                    ALARM_RING_DURATION_SECONDS;

                ESP_LOGI(
                    TAG,
                    "%s alarm started at %02d:%02d:%02d",
                    snooze_alarm_due
                        ? "Snoozed"
                        : "Daily",
                    now.tm_hour,
                    now.tm_min,
                    now.tm_sec
                );
            }
            else if (alarm_result !=
                     ESP_ERR_INVALID_STATE)
            {
                ESP_LOGE(
                    TAG,
                    "Alarm start failed: %s",
                    esp_err_to_name(
                        alarm_result
                    )
                );
            }
        }

        if (alarm_stop_deadline > 0 &&
            current_time >= alarm_stop_deadline)
        {
            if (ntpclock_alarm_is_ringing() ||
                audio_is_playing())
            {
                ntpclock_stop_alarm();
            }

            alarm_stop_deadline =
                0;
        }
        else if (alarm_stop_deadline > 0 &&
                 !ntpclock_alarm_is_ringing() &&
                 !audio_is_playing())
        {
            alarm_stop_deadline =
                0;
        }

        /* ------------------------------------------------------------------ */
        /* Weather                                                            */
        /* ------------------------------------------------------------------ */

        apply_pending_weather();

        bool weather_boundary =
            (now.tm_min % 30) == 0;

        int64_t weather_slot =
            make_weather_slot_key(
                &now
            );

        if (weather_boundary &&
            now.tm_sec >= WEATHER_REQUEST_SECOND &&
            weather_slot != last_weather_slot)
        {
            s_weather_request_due =
                true;

            last_weather_slot =
                weather_slot;
        }

        if (s_weather_request_due &&
            !s_weather_request_running &&
            !ntpclock_alarm_is_ringing() &&
            !audio_is_playing())
        {
            esp_err_t weather_result =
                start_weather_request_task();

            if (weather_result == ESP_OK)
            {
                s_weather_request_due =
                    false;

                ESP_LOGI(
                    TAG,
                    "Weather request task started at %02d:%02d:%02d",
                    now.tm_hour,
                    now.tm_min,
                    now.tm_sec
                );
            }
            else if (weather_result !=
                     ESP_ERR_INVALID_STATE)
            {
                ESP_LOGE(
                    TAG,
                    "Weather task start failed: %s",
                    esp_err_to_name(
                        weather_result
                    )
                );
            }
        }

        vTaskDelayUntil(
            &last_wake_time,
            pdMS_TO_TICKS(
                MAIN_LOOP_PERIOD_MS
            )
        );
    }
}
