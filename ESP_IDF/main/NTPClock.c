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

#define NTPCLOCK_SNOOZE_MIN_MINUTES      1U
#define NTPCLOCK_SNOOZE_MAX_MINUTES      60U
#define NTPCLOCK_DEFAULT_SNOOZE_MINUTES  5U

static ntpclock_update_callback_t s_update_callback = NULL;
static void *s_update_user_data = NULL;
static TaskHandle_t s_clock_task_handle = NULL;

volatile int g_alarm_hour = 7;
volatile int g_alarm_minute = 45;
volatile bool g_alarm_enabled = true;
volatile uint32_t g_alarm_snooze_minutes = NTPCLOCK_DEFAULT_SNOOZE_MINUTES;

static volatile bool s_alarm_ringing = false;
static volatile bool s_alarm_stop_requested = false;
static TaskHandle_t s_alarm_task_handle = NULL;

static time_t s_alarm_snooze_until = 0;
static portMUX_TYPE s_alarm_lock = portMUX_INITIALIZER_UNLOCKED;

static bool ntpclock_time_is_valid_tm(const struct tm *time_info)
{
    if (time_info == NULL)
    {
        return false;
    }

    return time_info->tm_year >= (NTPCLOCK_VALID_YEAR - 1900);
}

static bool ntpclock_time_is_valid(void)
{
    time_t current_time = 0;
    struct tm time_info = {0};

    time(&current_time);
    localtime_r(&current_time, &time_info);

    return ntpclock_time_is_valid_tm(&time_info);
}

static void ntpclock_request_alarm_stop(void)
{
    s_alarm_stop_requested = true;
    audio_stop();
}

static void ntpclock_alarm_playback_task(void *argument)
{
    (void)argument;

    ESP_LOGI(TAG,
             "Alarm started: %s, play count=%u",
             ALARM_FILE_PATH,
             (unsigned int)NTPCLOCK_ALARM_PLAY_COUNT);

    for (unsigned int play_index = 0U;
         play_index < NTPCLOCK_ALARM_PLAY_COUNT;
         play_index++)
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

    portENTER_CRITICAL(&s_alarm_lock);

    s_alarm_ringing = false;
    s_alarm_stop_requested = false;
    s_alarm_task_handle = NULL;

    portEXIT_CRITICAL(&s_alarm_lock);

    ESP_LOGI(TAG, "Alarm playback finished");

    vTaskDelete(NULL);
}

static esp_err_t ntpclock_start_alarm_playback(void)
{
    bool alarm_busy;

    portENTER_CRITICAL(&s_alarm_lock);

    alarm_busy = s_alarm_ringing || s_alarm_task_handle != NULL;

    portEXIT_CRITICAL(&s_alarm_lock);

    if (alarm_busy)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (audio_is_playing())
    {
        ESP_LOGW(TAG, "Alarm cannot start because audio is already playing");
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&s_alarm_lock);

    s_alarm_stop_requested = false;
    s_alarm_ringing = true;

    portEXIT_CRITICAL(&s_alarm_lock);

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
        portENTER_CRITICAL(&s_alarm_lock);

        s_alarm_ringing = false;
        s_alarm_task_handle = NULL;

        portEXIT_CRITICAL(&s_alarm_lock);

        ESP_LOGE(TAG, "Failed to create alarm playback task");
        return ESP_ERR_NO_MEM;
    }

    /* Any successful alarm start consumes a pending snooze. */
    ntpclock_clear_snooze();

    return ESP_OK;
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

    time_t current_time = 0;
    struct tm time_info = {0};

    time(&current_time);
    localtime_r(&current_time, &time_info);

    ESP_LOGI(TAG,
             "NTP synchronized: %04d-%02d-%02d %02d:%02d:%02d",
             time_info.tm_year + 1900,
             time_info.tm_mon + 1,
             time_info.tm_mday,
             time_info.tm_hour,
             time_info.tm_min,
             time_info.tm_sec);

    return ESP_OK;
}

static void ntpclock_update_task(void *argument)
{
    (void)argument;

    char time_text[16];
    char date_text[32];

    while (true)
    {
        time_t current_time = 0;
        struct tm time_info = {0};

        time(&current_time);
        localtime_r(&current_time, &time_info);

        if (ntpclock_time_is_valid_tm(&time_info))
        {
            strftime(time_text, sizeof(time_text), "%I:%M %p", &time_info);
            strftime(date_text, sizeof(date_text), "%a, %b %d", &time_info);
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

    portENTER_CRITICAL(&s_alarm_lock);

    g_alarm_hour = hour_24;
    g_alarm_minute = minute;
    g_alarm_enabled = enabled;
    s_alarm_snooze_until = 0;

    portEXIT_CRITICAL(&s_alarm_lock);

    ESP_LOGI(TAG,
             "Alarm configured: %02d:%02d, enabled=%s, WAV repeats=%u",
             hour_24,
             minute,
             enabled ? "true" : "false",
             (unsigned int)NTPCLOCK_ALARM_PLAY_COUNT);

    return ESP_OK;
}

void ntpclock_get_alarm(int *hour_24, int *minute, bool *enabled)
{
    int saved_hour;
    int saved_minute;
    bool saved_enabled;

    portENTER_CRITICAL(&s_alarm_lock);

    saved_hour = g_alarm_hour;
    saved_minute = g_alarm_minute;
    saved_enabled = g_alarm_enabled;

    portEXIT_CRITICAL(&s_alarm_lock);

    if (hour_24 != NULL)
    {
        *hour_24 = saved_hour;
    }

    if (minute != NULL)
    {
        *minute = saved_minute;
    }

    if (enabled != NULL)
    {
        *enabled = saved_enabled;
    }
}

void ntpclock_enable_alarm(bool enabled)
{
    int hour;
    int minute;
    bool current_enabled;

    ntpclock_get_alarm(&hour, &minute, &current_enabled);
    (void)current_enabled;

    ntpclock_set_alarm(hour, minute, enabled);
}

bool ntpclock_alarm_is_ringing(void)
{
    return s_alarm_ringing;
}

esp_err_t ntpclock_trigger_alarm_now(void)
{
    return ntpclock_start_alarm_playback();
}

void ntpclock_stop_alarm(void)
{
    ntpclock_clear_snooze();

    if (!s_alarm_ringing && !audio_is_playing())
    {
        return;
    }

    ntpclock_request_alarm_stop();

    ESP_LOGI(TAG, "Alarm stop requested");
}

esp_err_t ntpclock_snooze_alarm(uint32_t snooze_minutes)
{
    if (!s_alarm_ringing && !audio_is_playing())
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (snooze_minutes < NTPCLOCK_SNOOZE_MIN_MINUTES)
    {
        snooze_minutes = NTPCLOCK_SNOOZE_MIN_MINUTES;
    }
    else if (snooze_minutes > NTPCLOCK_SNOOZE_MAX_MINUTES)
    {
        snooze_minutes = NTPCLOCK_SNOOZE_MAX_MINUTES;
    }

    time_t current_time = time(NULL);
    time_t snooze_until = current_time + (time_t)(snooze_minutes * 60U);

    portENTER_CRITICAL(&s_alarm_lock);

    g_alarm_snooze_minutes = snooze_minutes;
    s_alarm_snooze_until = snooze_until;

    portEXIT_CRITICAL(&s_alarm_lock);

    ntpclock_request_alarm_stop();

    ESP_LOGI(TAG,
             "Alarm snoozed for %u minute(s)",
             (unsigned int)snooze_minutes);

    return ESP_OK;
}

bool ntpclock_snooze_is_pending(void)
{
    bool pending;

    portENTER_CRITICAL(&s_alarm_lock);

    pending = s_alarm_snooze_until > 0;

    portEXIT_CRITICAL(&s_alarm_lock);

    return pending;
}

bool ntpclock_snooze_is_due(time_t current_time)
{
    bool due;

    portENTER_CRITICAL(&s_alarm_lock);

    due = s_alarm_snooze_until > 0 && current_time >= s_alarm_snooze_until;

    portEXIT_CRITICAL(&s_alarm_lock);

    return due;
}

time_t ntpclock_get_snooze_until(void)
{
    time_t snooze_until;

    portENTER_CRITICAL(&s_alarm_lock);

    snooze_until = s_alarm_snooze_until;

    portEXIT_CRITICAL(&s_alarm_lock);

    return snooze_until;
}

void ntpclock_clear_snooze(void)
{
    portENTER_CRITICAL(&s_alarm_lock);

    s_alarm_snooze_until = 0;

    portEXIT_CRITICAL(&s_alarm_lock);
}