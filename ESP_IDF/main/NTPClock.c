#include "NTPClock.h"
#include "Audio.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_sntp.h"

static const char *TAG = "NTPClock";

#define NTPCLOCK_VALID_YEAR              2024
#define NTPCLOCK_UPDATE_PERIOD_MS        1000U
#define NTPCLOCK_TASK_STACK_SIZE         4096U
#define NTPCLOCK_TASK_PRIORITY           3U

#define NTPCLOCK_ALARM_TASK_STACK_SIZE   4096U
#define NTPCLOCK_ALARM_TASK_PRIORITY     4U
#define NTPCLOCK_ALARM_PLAY_COUNT        2U

static ntpclock_update_callback_t s_update_callback = NULL;
static void *s_update_user_data = NULL;
static TaskHandle_t s_clock_task_handle = NULL;

volatile int g_alarm_hour;
volatile int g_alarm_minute;
volatile bool g_alarm_enabled = false;
static volatile bool s_alarm_ringing = false;
static volatile bool s_alarm_stop_requested = false;
static TaskHandle_t s_alarm_task_handle = NULL;

/* Prevents the same daily alarm from triggering repeatedly during its minute. */
static int s_last_alarm_year = -1;
static int s_last_alarm_yday = -1;

static bool ntpclock_time_is_valid_tm(const struct tm *timeinfo)
{
    if (timeinfo == NULL)
    {
        return false;
    }

    return timeinfo->tm_year >= (NTPCLOCK_VALID_YEAR - 1900);
}

static bool ntpclock_time_is_valid(void)
{
    time_t now = 0;
    struct tm timeinfo = {0};

    time(&now);
    localtime_r(&now, &timeinfo);

    return ntpclock_time_is_valid_tm(&timeinfo);
}

static void ntpclock_alarm_playback_task(void *argument)
{
    (void)argument;

    ESP_LOGI(TAG,
             "Alarm started: %s, play count=%u",
             ALARM_FILE_PATH,
             (unsigned int)NTPCLOCK_ALARM_PLAY_COUNT);

    for (unsigned int play_index = 0; play_index < NTPCLOCK_ALARM_PLAY_COUNT; play_index++)
    {
        if (s_alarm_stop_requested)
        {
            break;
        }

        ESP_LOGI(TAG,
                 "Playing alarm %u/%u",
                 play_index + 1U,
                 (unsigned int)NTPCLOCK_ALARM_PLAY_COUNT);

        esp_err_t result = audio_play_wav_file(ALARM_FILE_PATH);

        if (result != ESP_OK)
        {
            ESP_LOGE(TAG,
                     "Alarm playback failed: %s",
                     esp_err_to_name(result));
            break;
        }
    }

    s_alarm_ringing = false;
    s_alarm_stop_requested = false;
    s_alarm_task_handle = NULL;

    ESP_LOGI(TAG, "Alarm playback finished");

    vTaskDelete(NULL);
}

static esp_err_t ntpclock_start_alarm_playback(void)
{
    if (s_alarm_ringing || s_alarm_task_handle != NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (audio_is_playing())
    {
        ESP_LOGW(TAG, "Alarm cannot start because audio is already playing");
        return ESP_ERR_INVALID_STATE;
    }

    s_alarm_stop_requested = false;
    s_alarm_ringing = true;

    BaseType_t task_result = xTaskCreate(
        ntpclock_alarm_playback_task,
        "ntp_alarm_audio",
        NTPCLOCK_ALARM_TASK_STACK_SIZE,
        NULL,
        NTPCLOCK_ALARM_TASK_PRIORITY,
        &s_alarm_task_handle
    );

    if (task_result != pdPASS)
    {
        s_alarm_ringing = false;
        s_alarm_task_handle = NULL;

        ESP_LOGE(TAG, "Failed to create alarm playback task");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static void ntpclock_check_alarm(const struct tm *timeinfo)
{
    if (!g_alarm_enabled || !ntpclock_time_is_valid_tm(timeinfo))
    {
        return;
    }

    if (timeinfo->tm_hour != g_alarm_hour || timeinfo->tm_min != g_alarm_minute)
    {
        return;
    }

    if (timeinfo->tm_year == s_last_alarm_year &&
        timeinfo->tm_yday == s_last_alarm_yday)
    {
        return;
    }

    esp_err_t result = ntpclock_start_alarm_playback();

    if (result == ESP_OK)
    {
        s_last_alarm_year = timeinfo->tm_year;
        s_last_alarm_yday = timeinfo->tm_yday;

        ESP_LOGI(TAG,
                 "Daily alarm triggered at %02d:%02d",
                 g_alarm_hour,
                 g_alarm_minute);
    }
}

esp_err_t ntpclock_init(const char *timezone, int retry_count)
{
    if (timezone == NULL || retry_count < 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Initializing SNTP");

    setenv("TZ", timezone, 1);
    tzset();

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.google.com");
    esp_sntp_init();

    for (int retry = 0;
         retry < retry_count && !ntpclock_time_is_valid();
         retry++)
    {
        ESP_LOGI(TAG,
                 "Waiting for NTP time sync (%d/%d)",
                 retry + 1,
                 retry_count);

        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    if (!ntpclock_time_is_valid())
    {
        ESP_LOGW(TAG, "Initial NTP synchronization timed out");
        return ESP_ERR_TIMEOUT;
    }

    time_t now = 0;
    struct tm timeinfo = {0};

    time(&now);
    localtime_r(&now, &timeinfo);

    ESP_LOGI(TAG,
             "NTP synchronized: %04d-%02d-%02d %02d:%02d:%02d",
             timeinfo.tm_year + 1900,
             timeinfo.tm_mon + 1,
             timeinfo.tm_mday,
             timeinfo.tm_hour,
             timeinfo.tm_min,
             timeinfo.tm_sec);

    return ESP_OK;
}

static void ntpclock_update_task(void *argument)
{
    (void)argument;

    char time_text[16];
    char date_text[32];

    while (true)
    {
        time_t now = 0;
        struct tm timeinfo = {0};

        time(&now);
        localtime_r(&now, &timeinfo);

        if (ntpclock_time_is_valid_tm(&timeinfo))
        {
            strftime(time_text, sizeof(time_text), "%I:%M %p", &timeinfo);
            strftime(date_text, sizeof(date_text), "%a, %b %d", &timeinfo);

            /* The alarm time comparison lives inside the NTP clock task. */
            ntpclock_check_alarm(&timeinfo);
        }
        else
        {
            snprintf(time_text, sizeof(time_text), "--:--");
            snprintf(date_text, sizeof(date_text), "Syncing time...");
        }

        if (s_update_callback != NULL)
        {
            s_update_callback(time_text, date_text, s_update_user_data);
        }

        vTaskDelay(pdMS_TO_TICKS(NTPCLOCK_UPDATE_PERIOD_MS));
    }
}

esp_err_t ntpclock_start_task(ntpclock_update_callback_t update_callback, void *user_data)
{
    if (s_clock_task_handle != NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_update_callback = update_callback;
    s_update_user_data = user_data;

    BaseType_t task_result = xTaskCreate(
        ntpclock_update_task,
        "ntpclock_update",
        NTPCLOCK_TASK_STACK_SIZE,
        NULL,
        NTPCLOCK_TASK_PRIORITY,
        &s_clock_task_handle
    );

    if (task_result != pdPASS)
    {
        s_clock_task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t ntpclock_set_alarm(int hour_24, int minute, bool enabled)
{
    if (hour_24 < 0 || hour_24 > 23 || minute < 0 || minute > 59)
    {
        return ESP_ERR_INVALID_ARG;
    }

    g_alarm_hour = hour_24;
    g_alarm_minute = minute;
    g_alarm_enabled = enabled;

    /* Allow the newly configured alarm to trigger for the current date. */
    s_last_alarm_year = -1;
    s_last_alarm_yday = -1;

    ESP_LOGI(TAG,
             "Alarm configured: %02d:%02d, enabled=%s, WAV repeats=%u",
             g_alarm_hour,
             g_alarm_minute,
             g_alarm_enabled ? "true" : "false",
             (unsigned int)NTPCLOCK_ALARM_PLAY_COUNT);

    return ESP_OK;
}

void ntpclock_enable_alarm(bool enabled)
{
    g_alarm_enabled = enabled;
}

bool ntpclock_alarm_is_ringing(void)
{
    return s_alarm_ringing;
}

void ntpclock_stop_alarm(void)
{
    if (!s_alarm_ringing)
    {
        return;
    }

    s_alarm_stop_requested = true;
    audio_stop();

    ESP_LOGI(TAG, "Alarm stop requested");
}

esp_err_t ntpclock_trigger_alarm_now(void)
{
    return ntpclock_start_alarm_playback();
}