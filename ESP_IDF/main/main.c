/*
 * TimeWise - main.c
 *
 * main.c owns the application schedule:
 *
 *   - Read local NTP time every second.
 *   - Update time/date once per minute.
 *   - Start the alarm at HH:MM:05.
 *   - Stop the second alarm play at second 50.
 *   - Request weather once every 30 minutes.
 *   - Change weather, wind, icon and background only after a successful
 *     weather request.
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
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"

#include "Audio.h"
#include "NTPClock.h"
#include "SDCard.h"
#include "TFT_Display.h"
#include "WeatherStation.h"

static const char *TAG = "TimeWise";

/* Keep private credentials only in your local project. */
#define WIFI_SSID                       "WiFi_SSID"
#define WIFI_PASS                       "WiFi_Password"

#define WEATHER_LATITUDE                Latitude
#define WEATHER_LONGITUDE               Longitude
#define WEATHER_LOCATION_NAME          "Location"

#define LOCAL_TIMEZONE                  "EST5EDT,M3.2.0/2,M11.1.0/2"

#define WIFI_MAX_RETRIES                10
#define NTP_RETRY_COUNT                 30

#define WIFI_CONNECTED_BIT              BIT0
#define WIFI_FAILED_BIT                 BIT1

#define MAIN_LOOP_PERIOD_MS             1000U
#define VALID_LOCAL_YEAR                2024

#define ALARM_START_SECOND              5
#define ALARM_STOP_SECOND               50

/*
 * Weather is requested at minute 00 and minute 30 after second 50.
 * This keeps HTTP, icon loading and background loading outside alarm audio.
 */
#define WEATHER_REQUEST_SECOND          50

#define WEATHER_TASK_STACK_SIZE         8192U
#define WEATHER_TASK_PRIORITY           3U
#define WEATHER_TASK_CORE               0

static EventGroupHandle_t s_wifi_event_group = NULL;
static int s_wifi_retry_count = 0;

static volatile bool s_weather_request_running = false;
static volatile bool s_weather_request_due = false;

static portMUX_TYPE s_weather_data_lock = portMUX_INITIALIZER_UNLOCKED;
static weatherstation_update_t s_pending_weather = {0};
static bool s_pending_weather_valid = false;

/* -------------------------------------------------------------------------- */
/*                              Text helper                                   */
/* -------------------------------------------------------------------------- */

static void copy_text(char *destination, size_t destination_size, const char *source)
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
/*                            Wi-Fi management                                */
/* -------------------------------------------------------------------------- */

static void wifi_event_handler(void *handler_argument, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)handler_argument;
    (void)event_data;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        if (s_wifi_event_group != NULL)
        {
            xEventGroupClearBits(
                s_wifi_event_group,
                WIFI_CONNECTED_BIT
            );
        }

        if (s_wifi_retry_count < WIFI_MAX_RETRIES)
        {
            s_wifi_retry_count++;

            ESP_LOGW(
                TAG,
                "Wi-Fi reconnect attempt %d/%d",
                s_wifi_retry_count,
                WIFI_MAX_RETRIES
            );

            esp_wifi_connect();
        }
        else
        {
            ESP_LOGE(TAG, "Wi-Fi connection failed");

            if (s_wifi_event_group != NULL)
            {
                xEventGroupSetBits(
                    s_wifi_event_group,
                    WIFI_FAILED_BIT
                );
            }
        }

        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        s_wifi_retry_count = 0;

        if (s_wifi_event_group != NULL)
        {
            xEventGroupClearBits(
                s_wifi_event_group,
                WIFI_FAILED_BIT
            );

            xEventGroupSetBits(
                s_wifi_event_group,
                WIFI_CONNECTED_BIT
            );
        }

        ESP_LOGI(TAG, "Wi-Fi connected and IP received");
    }
}

static esp_err_t initialize_nvs(void)
{
    esp_err_t result = nvs_flash_init();

    if (result == ESP_ERR_NVS_NO_FREE_PAGES ||
        result == ESP_ERR_NVS_NEW_VERSION_FOUND)
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

    wifi_init_config_t initialization =
        WIFI_INIT_CONFIG_DEFAULT();

    result = esp_wifi_init(&initialization);

    if (result != ESP_OK)
    {
        return result;
    }

    result = esp_event_handler_register(
        WIFI_EVENT,
        ESP_EVENT_ANY_ID,
        wifi_event_handler,
        NULL
    );

    if (result != ESP_OK)
    {
        return result;
    }

    result = esp_event_handler_register(
        IP_EVENT,
        IP_EVENT_STA_GOT_IP,
        wifi_event_handler,
        NULL
    );

    if (result != ESP_OK)
    {
        return result;
    }

    wifi_config_t configuration = {0};

    copy_text(
        (char *)configuration.sta.ssid,
        sizeof(configuration.sta.ssid),
        WIFI_SSID
    );

    copy_text(
        (char *)configuration.sta.password,
        sizeof(configuration.sta.password),
        WIFI_PASS
    );

    configuration.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    configuration.sta.pmf_cfg.capable = true;
    configuration.sta.pmf_cfg.required = false;

    result = esp_wifi_set_mode(WIFI_MODE_STA);

    if (result != ESP_OK)
    {
        return result;
    }

    result = esp_wifi_set_config(
        WIFI_IF_STA,
        &configuration
    );

    if (result != ESP_OK)
    {
        return result;
    }

    result = esp_wifi_start();

    if (result != ESP_OK)
    {
        return result;
    }

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

    strftime(
        time_text,
        sizeof(time_text),
        "%I:%M %p",
        time_info
    );

    strftime(
        date_text,
        sizeof(date_text),
        "%a, %b %d",
        time_info
    );

    /*
     * tft_dashboard_set_time() also updates the Sun/Moon position using the
     * latest sunrise, sunset, moonrise and moonset intervals.
     */
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

    /*
     * The HTTP worker only publishes data into RAM. It never loads the
     * background or icon directly, so a late HTTP response cannot interrupt
     * alarm playback.
     */
    portENTER_CRITICAL(&s_weather_data_lock);

    s_pending_weather = *update;
    s_pending_weather_valid = true;

    portEXIT_CRITICAL(&s_weather_data_lock);

    ESP_LOGI(
        TAG,
        "Weather response ready for display: %s",
        update->condition
    );
}

static void apply_pending_weather(void)
{
    if (audio_is_playing() || ntpclock_alarm_is_ringing())
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

    /*
     * This is the only point where weather, wind, background and icon are
     * applied to the dashboard.
     */
    tft_dashboard_set_weather(
        update.temperature_c,
        update.condition,
        update.location,
        update.background_path,
        update.icon_path
    );

    tft_dashboard_set_wind(
        update.wind_speed_kmh,
        update.wind_direction_degrees
    );

    tft_dashboard_set_astro(
        update.current_time,
        update.sunrise_time,
        update.sunset_time,
        update.moonrise_time,
        update.moonset_time
    );

    ESP_LOGI(
        TAG,
        "Weather display updated: %s",
        update.condition
    );
}

static void weather_request_task(void *argument)
{
    (void)argument;

    ESP_LOGI(TAG, "Starting one-shot weather request");

    esp_err_t result = weatherstation_request_once();

    if (result != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Weather request failed: %s",
            esp_err_to_name(result)
        );
    }

    s_weather_request_running = false;

    vTaskDelete(NULL);
}

static esp_err_t start_weather_request_task(void)
{
    if (s_weather_request_running)
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
/*                               Application                                  */
/* -------------------------------------------------------------------------- */

void app_main(void)
{
    ESP_LOGI(TAG, "TimeWise starting");
    ESP_LOGI(TAG, "Reset reason: %d", (int)esp_reset_reason());

    vTaskDelay(pdMS_TO_TICKS(500));

    //SD card
    esp_err_t sd_result = sdcard_init();

    if (sd_result != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "SD card initialization failed: %s",
            esp_err_to_name(sd_result)
        );

        return;
    }

    ESP_LOGI(TAG, "SD card mounted");

    //Audio
    esp_err_t audio_result = audio_init();

    if (audio_result != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Audio initialization failed: %s",
            esp_err_to_name(audio_result)
        );

        return;
    }

    //Display
    ESP_ERROR_CHECK(tft_display_init());
    ESP_ERROR_CHECK(tft_dashboard_create());

    //Wi-Fi
    esp_err_t wifi_result = connect_wifi();

    if (wifi_result != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Wi-Fi unavailable: %s",
            esp_err_to_name(wifi_result)
        );

        return;
    }

    //NTP
    esp_err_t ntp_result = ntpclock_init( LOCAL_TIMEZONE, NTP_RETRY_COUNT);

    if (ntp_result != ESP_OK)
    {
        ESP_LOGW(
            TAG,
            "Initial NTP sync did not complete: %s",
            esp_err_to_name(ntp_result)
        );
    }

    //Do not call ntpclock_start_task().
    //main.c owns the one-second scheduler.

    //Weather
    esp_err_t weather_init_result = weatherstation_init(
        WEATHER_LATITUDE,
        WEATHER_LONGITUDE,
        WEATHER_LOCATION_NAME,
        weather_update_callback,
        NULL
    );

    if (weather_init_result != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "WeatherStation initialization failed: %s",
            esp_err_to_name(weather_init_result)
        );

        return;
    }

    //Alarm
    g_alarm_hour = 7;
    g_alarm_minute = 45;
    g_alarm_enabled = true;

    //Scheduler state
    int64_t last_display_minute = -1;
    int64_t last_alarm_day = -1;
    int64_t last_alarm_stop_minute = -1;
    int64_t last_weather_slot = -1;

    //Request weather once immediately after startup.
    s_weather_request_due = true;

    struct tm startup_time = {0};

    if (get_local_time(&startup_time))
    {
        last_weather_slot = make_weather_slot_key(&startup_time);
    }

    TickType_t last_wake_time = xTaskGetTickCount();

    ESP_LOGI(TAG, "Central one-second scheduler started");

    while (true)
    {
        struct tm now = {0};

        if (!get_local_time(&now))
        {
            tft_dashboard_set_time("--:--");
            tft_dashboard_set_date("Syncing time...");

            vTaskDelayUntil(
                &last_wake_time,
                pdMS_TO_TICKS(MAIN_LOOP_PERIOD_MS)
            );

            continue;
        }

        int64_t minute_key = make_minute_key(&now);

        int64_t day_key = make_day_key(&now);

        //Update time/date
        if (minute_key != last_display_minute)
        {
            update_time_display(&now);
            last_display_minute = minute_key;

            ESP_LOGI(
                TAG,
                "Clock display updated at %02d:%02d:%02d",
                now.tm_hour,
                now.tm_min,
                now.tm_sec
            );
        }

        //Start alarm
        if (g_alarm_enabled && now.tm_hour == g_alarm_hour && now.tm_min == g_alarm_minute && now.tm_sec >= ALARM_START_SECOND && now.tm_sec < ALARM_STOP_SECOND && day_key != last_alarm_day)
        {
            esp_err_t alarm_result = ntpclock_trigger_alarm_now();

            if (alarm_result == ESP_OK)
            {
                last_alarm_day = day_key;

                ESP_LOGI(
                    TAG,
                    "Alarm started at %02d:%02d:%02d",
                    now.tm_hour,
                    now.tm_min,
                    now.tm_sec
                );
            }
            else
            {
                ESP_LOGE(
                    TAG,
                    "Alarm start failed: %s",
                    esp_err_to_name(alarm_result)
                );
            }
        }

        //Stop alarm
        if (now.tm_sec >= ALARM_STOP_SECOND && minute_key != last_alarm_stop_minute)
        {
            if (ntpclock_alarm_is_ringing() || audio_is_playing())
            {
                ntpclock_stop_alarm();

                ESP_LOGI(
                    TAG,
                    "Alarm stop requested at %02d:%02d:%02d",
                    now.tm_hour,
                    now.tm_min,
                    now.tm_sec
                );
            }
            last_alarm_stop_minute = minute_key;
        }

        //Apply a completed request only while alarm audio is idle.
        apply_pending_weather();

        //Schedule weather
        bool weather_boundary = (now.tm_min % 30) == 0;

        int64_t weather_slot = make_weather_slot_key(&now);

        if (weather_boundary && now.tm_sec >= WEATHER_REQUEST_SECOND && weather_slot != last_weather_slot)
        {
            s_weather_request_due = true;
            last_weather_slot = weather_slot;
        }

        /*
         * A weather request starts only when alarm audio and its task have
         * completely stopped.
         */
        if (s_weather_request_due && !s_weather_request_running && !ntpclock_alarm_is_ringing() && !audio_is_playing())
        {
            esp_err_t weather_result = start_weather_request_task();

            if (weather_result == ESP_OK)
            {
                s_weather_request_due = false;

                ESP_LOGI(
                    TAG,
                    "Weather request task started at %02d:%02d:%02d",
                    now.tm_hour,
                    now.tm_min,
                    now.tm_sec
                );
            }
            else if (weather_result != ESP_ERR_INVALID_STATE)
            {
                ESP_LOGE(
                    TAG,
                    "Weather task start failed: %s",
                    esp_err_to_name(weather_result)
                );
            }
        }

        //Start alarm
        vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(MAIN_LOOP_PERIOD_MS));
    }
}
