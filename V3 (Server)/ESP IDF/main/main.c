/*
 * TimeWise - main.c
 *
 * Music Enable = 0:
 *   - Update time/date once per minute.
 *   - Run alarm scheduling.
 *   - Request weather every 30 minutes.
 *   - Serve the HTTP control page.
 *
 * Music Enable = 1:
 *   - Dedicated music-only operating mode.
 *   - No clock/date/weather/alarm scheduling or dashboard updates.
 *   - Render the Music screen, then stop LVGL and release its task stack.
 *   - HTTP control polling remains active.
 *   - Gateway selects songs and streams decoded 24 kHz unsigned 8-bit mono PCM.
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
#include "MusicStream.h"
#include "NTPClock.h"
#include "SDCard.h"
#include "TFT_Display.h"
#include "WeatherStation.h"
#include "HTTPControl.h"
#include "LED.h"

static const char *TAG = "TimeWise";

/* -------------------------------------------------------------------------- */
/*                           Application configuration                        */
/* -------------------------------------------------------------------------- */

#define WIFI_SSID                       "WIFI_SSID"
#define WIFI_PASS                       "WIFI_PASSWORD"

#define WEATHER_LATITUDE                LATITUDE
#define WEATHER_LONGITUDE               LONGITUDE
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

#define WEATHER_REQUEST_SECOND          0
#define FORECAST_REQUEST_MINUTE         2
#define FORECAST_REQUEST_SECOND         30

#define WEATHER_TASK_STACK_SIZE         8192U
#define WEATHER_TASK_PRIORITY           3U
#define WEATHER_TASK_CORE               0


// Allow deleted Navidrome task memory to be reclaimed before music startup.
#define MUSIC_START_GRACE_MS            1000U

// Allow the Now Playing screen to physically flush before LVGL is paused. 
#define MUSIC_DISPLAY_SETTLE_MS         200U

// Allow HTTP client resources to be released before MusicPlayer starts. 
#define MUSIC_SERVICE_RELEASE_MS        500U

// Allow MusicPlayer cleanup/task deletion to finish before restoring services. 
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

/* Forecast is requested once per hour at HH:02:30 (local NTP time). */
static weatherstation_forecast_t s_pending_forecast[WEATHERSTATION_FORECAST_HOURS] = {0};
static size_t s_pending_forecast_count = 0U;
static volatile bool s_forecast_request_due = false;
static bool s_forecast_update_pending = false;

/* -------------------------------------------------------------------------- */
/*                            Music command state                             */
/* -------------------------------------------------------------------------- */

typedef enum {
    MUSIC_ACTION_NONE = 0,
    MUSIC_ACTION_RANDOM,
    MUSIC_ACTION_NEXT,
    MUSIC_ACTION_PREVIOUS
} music_action_request_t;

static volatile music_action_request_t s_music_action = MUSIC_ACTION_NONE;
static volatile uint32_t s_music_command_generation = 0;
static bool s_music_mode_active = false;
static bool s_music_auto_advance_armed = false;

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

static int64_t make_forecast_hour_key(const struct tm *time_info)
{
    return make_day_key(time_info) * 24 + time_info->tm_hour;
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

static void weather_update_callback(const weatherstation_update_t *update)
{
    if(update == NULL)
    {
        return;
    }

    if (http_control_get_music_enabled())
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
        http_control_get_music_enabled() ||
        g_tft_display_paused ||
        music_stream_is_busy() ||
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

    /* Phase one: apply background, current weather and Sun/Moon arc. */
    tft_dashboard_set_weather(
        update.temperature_c,
        update.condition,
        update.location,
        update.background_path,
        update.icon_path
    );

    tft_dashboard_set_astro(
        update.current_time,
        update.sunrise_time,
        update.sunset_time,
        update.moonrise_time,
        update.moonset_time
    );

    ESP_LOGI(TAG, "Current weather and background updated: %s", update.condition);

    /* The hourly forecast is requested separately, not parsed from /weather. */
}

static void forecast_update_callback(const weatherstation_forecast_t *forecast, size_t count)
{
    if (!forecast || count == 0U || http_control_get_music_enabled()) return;
    if (count > WEATHERSTATION_FORECAST_HOURS) count = WEATHERSTATION_FORECAST_HOURS;
    portENTER_CRITICAL(&s_weather_data_lock);
    memcpy(s_pending_forecast, forecast, count * sizeof(forecast[0]));
    s_pending_forecast_count = count;
    s_forecast_update_pending = true;
    portEXIT_CRITICAL(&s_weather_data_lock);
    ESP_LOGI(TAG, "Forecast HTTP response ready (%u entries)", (unsigned)count);
}

static void apply_pending_forecast(void)
{
    if (s_music_mode_active ||
        http_control_get_music_enabled() ||
        g_tft_display_paused ||
        s_weather_request_running ||
        music_stream_is_busy() ||
        audio_is_playing() ||
        ntpclock_alarm_is_ringing())
    {
        return;
    }

    weatherstation_forecast_t forecast[WEATHERSTATION_FORECAST_HOURS] = {0};
    size_t count = 0U;
    bool ready = false;
    portENTER_CRITICAL(&s_weather_data_lock);
    if (s_forecast_update_pending)
    {
        memcpy(forecast, s_pending_forecast, sizeof(forecast));
        count = s_pending_forecast_count;
        s_forecast_update_pending = false;
        ready = true;
    }
    portEXIT_CRITICAL(&s_weather_data_lock);

    if (!ready)
    {
        return;
    }

    tft_dashboard_set_forecast(forecast, count);
    ESP_LOGI(TAG, "Five-hour forecast display updated (%u entries)",
             (unsigned int)count);
}

static void weather_request_task(void *argument)
{
    bool is_forecast = argument != NULL;
    ESP_LOGI(TAG, "Starting %s HTTP request", is_forecast ? "forecast" : "current weather");
    esp_err_t result = is_forecast ? weatherstation_request_forecast() : weatherstation_request();

    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "Weather request failed: %s", esp_err_to_name(result));
    }

    s_weather_request_running = false;

    vTaskDelete(NULL);
}

static esp_err_t start_weather_request_task(bool is_forecast)
{
    if (s_weather_request_running ||
        http_control_get_music_enabled() ||
        s_music_mode_active ||
        music_stream_is_busy())
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_weather_request_running = true;

    BaseType_t task_result = xTaskCreatePinnedToCore(
        weather_request_task,
        "weather_request",
        WEATHER_TASK_STACK_SIZE,
        (void *)(uintptr_t)(is_forecast ? 1U : 0U),
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
/*                       Gateway-backed music mode                            */
/* -------------------------------------------------------------------------- */

static esp_err_t enter_music_mode(void)
{
    if (s_music_mode_active) return ESP_OK;
    ESP_LOGI(TAG, "Entering Gateway music mode");
    s_weather_request_due = false;
    s_forecast_request_due = false;
    portENTER_CRITICAL(&s_weather_data_lock);
    s_pending_weather_valid = false;
    s_forecast_update_pending = false;
    s_pending_forecast_count = 0;
    portEXIT_CRITICAL(&s_weather_data_lock);
    if (ntpclock_alarm_is_ringing() || audio_is_playing()) ntpclock_stop_alarm();
    tft_dashboard_show_music("Music Mode", "Connecting to Gateway...", 0);
    vTaskDelay(pdMS_TO_TICKS(MUSIC_DISPLAY_SETTLE_MS));
    tft_display_set_paused(true);
    ESP_RETURN_ON_ERROR(http_control_enter_music_mode(), TAG, "Music HTTP mode failed");
    s_music_mode_active = true;
    if (s_music_action == MUSIC_ACTION_NONE) s_music_action = MUSIC_ACTION_RANDOM;
    return ESP_OK;
}

static esp_err_t exit_music_mode(void)
{
    if (!s_music_mode_active) return ESP_OK;
    if (music_stream_is_busy()) return ESP_ERR_INVALID_STATE;
    s_music_action = MUSIC_ACTION_NONE;
    s_music_auto_advance_armed = false;
    s_music_mode_active = false;
    s_weather_request_due = true;
    s_forecast_request_due = false;
    tft_display_set_paused(false);
    tft_dashboard_hide_music();
    struct tm now = {0};
    if (get_local_time(&now)) update_time_display(&now);
    ESP_LOGI(TAG, "Normal TimeWise mode restored");
    return http_control_exit_music_mode();
}

static esp_err_t timewise_music_action_handler(http_music_action_t action,
                                                int32_t value, void *user_data)
{
    (void)user_data;
    ++s_music_command_generation;
    switch (action) {
        case HTTP_MUSIC_ACTION_ENABLE:
            if (value) {
                s_music_action = MUSIC_ACTION_RANDOM;
            } else {
                s_music_action = MUSIC_ACTION_NONE;
                s_music_auto_advance_armed = false;
                music_stream_stop();
            }
            return ESP_OK;
        case HTTP_MUSIC_ACTION_PLAY_RANDOM:
            s_music_action = MUSIC_ACTION_RANDOM;
            s_music_auto_advance_armed = false;
            music_stream_stop();
            return ESP_OK;
        case HTTP_MUSIC_ACTION_NEXT:
            s_music_action = MUSIC_ACTION_NEXT;
            s_music_auto_advance_armed = false;
            music_stream_stop();
            return ESP_OK;
        case HTTP_MUSIC_ACTION_PREVIOUS:
            s_music_action = MUSIC_ACTION_PREVIOUS;
            s_music_auto_advance_armed = false;
            music_stream_stop();
            return ESP_OK;
        case HTTP_MUSIC_ACTION_STOP:
            s_music_action = MUSIC_ACTION_NONE;
            s_music_auto_advance_armed = false;
            music_stream_stop();
            return ESP_OK;
        case HTTP_MUSIC_ACTION_PAUSE: return music_stream_pause();
        case HTTP_MUSIC_ACTION_RESUME: return music_stream_resume();
        default: return ESP_ERR_NOT_SUPPORTED;
    }
}

static void process_music_mode(void)
{
    if (!s_music_mode_active || music_stream_is_busy()) return;
    if (s_music_action == MUSIC_ACTION_NONE && s_music_auto_advance_armed) {
        music_stream_state_t state = music_stream_get_state();
        if (state == MUSIC_STREAM_STOPPED) {
            s_music_action = MUSIC_ACTION_RANDOM;
            ESP_LOGI(TAG, "Track ended; selecting another random track");
        } else if (state == MUSIC_STREAM_ERROR) {
            ESP_LOGW(TAG, "Stream failed; waiting for user command");
        }
        s_music_auto_advance_armed = false;
    }
    music_action_request_t action = s_music_action;
    if (action == MUSIC_ACTION_NONE || s_weather_request_running) return;
    s_music_action = MUSIC_ACTION_NONE;
    uint32_t generation = s_music_command_generation;
    const char *name = action == MUSIC_ACTION_PREVIOUS ? "previous" :
                       action == MUSIC_ACTION_NEXT ? "next" : "random";
    music_stream_song_t song = {0};
    esp_err_t result = music_stream_select(name, &song);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Gateway selection %s failed: %s", name, esp_err_to_name(result));
        return;
    }
    if (!http_control_get_music_enabled() || s_music_command_generation != generation) return;
    tft_display_set_paused(false);
    tft_dashboard_show_music(song.title, song.artist, song.duration_seconds);
    vTaskDelay(pdMS_TO_TICKS(MUSIC_DISPLAY_SETTLE_MS));
    tft_display_set_paused(true);
    if (!http_control_get_music_enabled() || s_music_command_generation != generation) return;
    result = music_stream_start();
    if (result == ESP_OK) s_music_auto_advance_armed = true;
    else ESP_LOGE(TAG, "Could not start PCM stream: %s", esp_err_to_name(result));
}

/* -------------------------------------------------------------------------- */
/*                               Application                                  */
/* -------------------------------------------------------------------------- */

void app_main(void)
{
    ESP_LOGI(TAG, "TimeWise starting");
    ESP_LOGI(TAG, "Reset reason: %d", (int)esp_reset_reason());

    /* Lowest-priority RGB444 LED task; stays asleep between color changes. */
    ESP_ERROR_CHECK(led_init());
    
    int color_count = 0;
    int red = 0; int green = 0; int blue = 0;
    switch(color_count)
    {
        case 0:
            if(red > 1023)
            {
                color_count++;
                red = 0;
            }
            led_set_red(red);
            led_set_green(0);
            led_set_blue(0);
            red++;
            break;

        case 1:
            if(green > 1023)
            {
                color_count++;
                green = 0;
            }
            led_set_red(0);
            led_set_green(green);
            led_set_blue(0);
            green++;
            break;

        case 2:
            if(blue > 1023)
            {
                color_count++;
                blue = 0;
            }
            led_set_red(0);
            led_set_green(0);
            led_set_blue(blue);
            blue++;
            break;
        
        case 3:
            if(red > 1023)
            {
                color_count++;
                red = 0;
                green = 0;
            }
            led_set_red(red);
            led_set_green(green);
            led_set_blue(0);
            red++;
            green++;
            break;

        case 4:
            if(green > 1023)
            {
                color_count++;
                green = 0;
                blue = 0;
            }
            led_set_red(0);
            led_set_green(green);
            led_set_blue(blue);
            blue++;
            green++;
            break;

        case 5:
            if(red > 1023)
            {
                color_count++;
                red = 0;
                blue = 0;
            }
            led_set_red(red);
            led_set_green(0);
            led_set_blue(blue);
            red++;
            blue++;
            break;

        case 6:
            if(red > 1023)
            {
                color_count++;
                red = 0;
                blue = 0;
                green = 0;
            }
            led_set_red(red);
            led_set_green(green);
            led_set_blue(blue);
            red++;
            green++;
            blue++;
            break;
        
        default:
            led_set_red(0);
            led_set_green(0);
            led_set_blue(0);
    }
    
    vTaskDelay(pdMS_TO_TICKS(100));

    esp_err_t result = sdcard_init();

    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "SD card initialization failed: %s", esp_err_to_name(result));

        return;
    }

    result = audio_init();

    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "Audio initialization failed: %s", esp_err_to_name(result));

        return;
    }

    ESP_ERROR_CHECK(tft_display_init());

    ESP_ERROR_CHECK(tft_dashboard_create());

    result = connect_wifi();

    if (result != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Wi-Fi unavailable: %s",
            esp_err_to_name(result)
        );

        return;
    }

    result = ntpclock_init(LOCAL_TIMEZONE, NTP_RETRY_COUNT);

    if (result != ESP_OK)
    {
        ESP_LOGW(TAG, "Initial NTP sync did not complete: %s", esp_err_to_name(result));
    }

    result = weatherstation_init(WEATHER_LATITUDE, WEATHER_LONGITUDE, WEATHER_LOCATION_NAME, weather_update_callback);

    if (result != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "WeatherStation initialization failed: %s",
            esp_err_to_name(result)
        );

        return;
    }

    weatherstation_set_forecast_callback(forecast_update_callback);

    ESP_ERROR_CHECK(
        ntpclock_set_alarm(
            7,
            45,
            true
        )
    );

    http_control_set_music_action_callback(
        timewise_music_action_handler,
        NULL
    );

    result = http_control_start();

    if (result != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "HTTP control server failed: %s",
            esp_err_to_name(result)
        );

        return;
    }

    /*
     * Music is disabled by default in HTTPControl.c. The normal TimeWise
     * dashboard therefore starts first. MusicPlayer is not touched until the
     * user sets Music Enable to 1.
     */
    ESP_LOGI(
        TAG,
        "Music Enable=%d at startup",
        http_control_get_music_enabled() ? 1 : 0
    );

    int64_t last_display_minute = -1;

    int64_t last_alarm_minute = -1;

    int64_t last_weather_slot = -1;

    int64_t last_forecast_hour = -1;

    time_t alarm_stop_deadline = 0;

    s_weather_request_due = true;

    struct tm startup_time = {0};

    if (get_local_time(&startup_time))
    {
        last_weather_slot =make_weather_slot_key(&startup_time);
    }

    TickType_t last_wake_time =xTaskGetTickCount();

    ESP_LOGI(
        TAG,
        "Central one-second scheduler started"
    );

    while (true)
    {
        bool music_enabled = http_control_get_music_enabled();

        /* Music Mode: keep control HTTP task active and suspend clock/weather. */
        if (music_enabled) {
            if (!s_music_mode_active) {
                esp_err_t mode_result = enter_music_mode();
                if (mode_result != ESP_OK) ESP_LOGE(TAG, "Music mode entry: %s", esp_err_to_name(mode_result));
            }
            if (s_music_mode_active) process_music_mode();
            vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(MAIN_LOOP_PERIOD_MS));
            continue;
        }
        if (s_music_mode_active) {
            s_music_action = MUSIC_ACTION_NONE;
            music_stream_stop();
            if (!music_stream_is_busy()) {
                esp_err_t r = exit_music_mode();
                if (r != ESP_OK) ESP_LOGE(TAG, "Music mode exit: %s", esp_err_to_name(r));
                last_display_minute = -1;
                last_weather_slot = -1;
                alarm_stop_deadline = 0;
                last_wake_time = xTaskGetTickCount();
            }
            vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(MAIN_LOOP_PERIOD_MS));
            continue;
        }

        /* ================================================================== */
        /*                       NORMAL TIMEWISE MODE                          */
        /* ================================================================== */

        /*
         * Music Enable is 0 here. No Navidrome task, MP3 decoder, continuous
         * DAC, or MusicPlayer task is allowed to start from this branch.
         */

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
        apply_pending_forecast();

        /* Schedule against local wall-clock time, not time since the last fetch.
           Each time window is keyed to prevent repeat requests while the
           one-second scheduler remains in the same minute. */
        const int64_t weather_slot = make_weather_slot_key(&now);
        if ((now.tm_min == 0 || now.tm_min == 30) &&
            now.tm_sec >= WEATHER_REQUEST_SECOND &&
            weather_slot != last_weather_slot)
        {
            s_weather_request_due = true;
            last_weather_slot = weather_slot;
            ESP_LOGI(TAG, "Half-hour current weather request due");
        }

        const int64_t forecast_hour = make_forecast_hour_key(&now);
        if (now.tm_min == FORECAST_REQUEST_MINUTE &&
            now.tm_sec >= FORECAST_REQUEST_SECOND &&
            forecast_hour != last_forecast_hour)
        {
            s_forecast_request_due = true;
            last_forecast_hour = forecast_hour;
            ESP_LOGI(TAG, "Hourly forecast request due at %02d:%02d:%02d",
                     now.tm_hour, now.tm_min, now.tm_sec);
        }

        if ((s_weather_request_due || s_forecast_request_due) &&
            !s_weather_request_running &&
            !ntpclock_alarm_is_ringing() &&
            !audio_is_playing())
        {
            bool starting_forecast = !s_weather_request_due && s_forecast_request_due;
            /* Clear flags BEFORE creating the task so its completion cannot race with us. */
            if (starting_forecast) s_forecast_request_due = false;
            else {
                s_weather_request_due = false;
                /* Keep any independent hourly forecast request queued. */
            }
            esp_err_t weather_result = start_weather_request_task(starting_forecast);
            if (weather_result != ESP_OK)
            {
                if (starting_forecast) s_forecast_request_due = true;
                else s_weather_request_due = true;
            }

            if (weather_result == ESP_OK)
            {

                ESP_LOGI(
                    TAG,
                    "%s request task started at %02d:%02d:%02d",
                    starting_forecast ? "Forecast" : "Weather",
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