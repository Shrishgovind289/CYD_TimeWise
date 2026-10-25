
#include "NTPClock.h"
#include "Audio.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_sntp.h"

static const char *TAG = "NTPClock";

/* -------------------------------------------------------------------------- */
/*                              Configuration                                 */
/* -------------------------------------------------------------------------- */

#define NTPCLOCK_VALID_YEAR              2024

#define NTPCLOCK_ALARM_TASK_STACK_SIZE   4096U
#define NTPCLOCK_ALARM_TASK_PRIORITY     4U
#define NTPCLOCK_ALARM_PLAY_COUNT        2U

#define NTPCLOCK_SNOOZE_MIN_MINUTES      1U
#define NTPCLOCK_SNOOZE_MAX_MINUTES      60U
#define NTPCLOCK_DEFAULT_SNOOZE_MINUTES  5U

#define NTPCLOCK_DAILY_SYNC_HOUR         1U
#define NTPCLOCK_DAILY_SYNC_MINUTE       12U
#define NTPCLOCK_SYNC_TASK_STACK_SIZE    3072U
#define NTPCLOCK_SYNC_TASK_PRIORITY      2U
#define NTPCLOCK_SYNC_CHECK_MS           30000U
#define NTPCLOCK_SYNC_RETRY_SECONDS      300U

/* Prevent automatic SNTP polling from replacing our 1 AM schedule. */
#define NTPCLOCK_SNTP_INTERVAL_MS        (7U * 24U * 60U * 60U * 1000U)

/* -------------------------------------------------------------------------- */
/*                          Global Configuration                              */
/* -------------------------------------------------------------------------- */

volatile int g_alarm_hour;
volatile int g_alarm_minute;
volatile bool g_alarm_enabled = true;

volatile uint32_t g_alarm_snooze_minutes =
    NTPCLOCK_DEFAULT_SNOOZE_MINUTES;

volatile uint8_t g_ntp_hour = 12U;

/* -------------------------------------------------------------------------- */
/*                             Private State                                  */
/* -------------------------------------------------------------------------- */

static volatile bool s_alarm_ringing = false;
static volatile bool s_alarm_stop_requested = false;

static TaskHandle_t s_alarm_task_handle = NULL;
static TaskHandle_t s_ntp_sync_task_handle = NULL;

static time_t s_alarm_snooze_until = 0;

static portMUX_TYPE s_alarm_lock =
    portMUX_INITIALIZER_UNLOCKED;

/* Incremented only after an actual SNTP response. */
static volatile uint32_t s_ntp_sync_count = 0U;

/* -------------------------------------------------------------------------- */
/*                              Local Time                                    */
/* -------------------------------------------------------------------------- */

bool ntpclock_get_local_time(struct tm *time_info)
{
    if (time_info == NULL)
    {
        return false;
    }

    time_t current_time = time(NULL);

    if (localtime_r(&current_time, time_info) == NULL)
    {
        return false;
    }

    bool valid =
        time_info->tm_year >= (NTPCLOCK_VALID_YEAR - 1900);

    if (valid)
    {
        g_ntp_hour = (uint8_t)time_info->tm_hour;
    }

    return valid;
}

static bool ntpclock_time_is_valid(void)
{
    struct tm time_info = {0};

    return ntpclock_get_local_time(&time_info);
}

/* -------------------------------------------------------------------------- */
/*                        SNTP Synchronization                                */
/* -------------------------------------------------------------------------- */

static void ntpclock_sync_callback(struct timeval *tv)
{
    (void)tv;

    s_ntp_sync_count++;

    ESP_LOGI(
        TAG,
        "NTP synchronization completed, count=%lu",
        (unsigned long)s_ntp_sync_count
    );
}

/*
 * Each local calendar day has a unique key.
 * This also handles the repeated 1 AM hour during DST changes.
 */
static int ntpclock_get_day_key(const struct tm *time_info)
{
    return (time_info->tm_year + 1900) * 366 +
           time_info->tm_yday;
}

/* -------------------------------------------------------------------------- */
/*                       Daily NTP Synchronization                            */
/* -------------------------------------------------------------------------- */

static void ntpclock_daily_sync_task(void *argument)
{
    (void)argument;

    int completed_day = -1;
    int pending_day = -1;

    uint32_t request_sync_count = 0U;
    TickType_t last_request_tick = 0;

    bool request_pending = false;

    ESP_LOGI(
        TAG,
        "Requesting daily NTP synchronization at %02u:%02u",
        (unsigned int)NTPCLOCK_DAILY_SYNC_HOUR,
        (unsigned int)NTPCLOCK_DAILY_SYNC_MINUTE
    );

    while (true)
    {
        struct tm local_time = {0};

        if (ntpclock_get_local_time(&local_time))
        {
            int today = ntpclock_get_day_key(&local_time);

            /*
             * A response received after our request confirms
             * that the daily synchronization succeeded.
             */
            if (request_pending &&
                s_ntp_sync_count != request_sync_count)
            {
                completed_day = pending_day;
                request_pending = false;

                ESP_LOGI(
                    TAG,
                    "Daily NTP synchronization successful"
                );
            }

            /*
             * Retry only during the scheduled 1 AM hour.
             * A failed request is not counted as successful.
             */
            if (local_time.tm_hour == NTPCLOCK_DAILY_SYNC_HOUR && local_time.tm_min >= NTPCLOCK_DAILY_SYNC_MINUTE && today != completed_day)
            {
                TickType_t now_tick = xTaskGetTickCount();

                bool retry_due = request_pending && today == pending_day && (now_tick - last_request_tick) >= pdMS_TO_TICKS(NTPCLOCK_SYNC_RETRY_SECONDS * 1000U);

                bool new_day_request = !request_pending || today != pending_day;

                if (new_day_request || retry_due)
                {
                    ESP_LOGI(
                        TAG,
                        "Requesting NTP synchronization at 1 AM"
                    );

                    request_sync_count = s_ntp_sync_count;

                    if (esp_sntp_restart())
                    {
                        pending_day = today;
                        request_pending = true;
                        last_request_tick = now_tick;
                    }
                    else
                    {
                        ESP_LOGW(
                            TAG,
                            "SNTP restart failed"
                        );

                        pending_day = today;
                        request_pending = true;
                        last_request_tick = now_tick;
                    }
                }
            }

            /*
             * Once the scheduled hour has passed,
             * stop tracking an unconfirmed request.
             * The next day provides another opportunity.
             */
            if (local_time.tm_hour != NTPCLOCK_DAILY_SYNC_HOUR &&
                request_pending)
            {
                request_pending = false;
            }
        }

        vTaskDelay(
            pdMS_TO_TICKS(NTPCLOCK_SYNC_CHECK_MS)
        );
    }
}

/* -------------------------------------------------------------------------- */
/*                         Alarm Playback                                     */
/* -------------------------------------------------------------------------- */

static void ntpclock_request_alarm_stop(void)
{
    s_alarm_stop_requested = true;
    audio_stop();
}

static void ntpclock_alarm_playback_task(void *argument)
{
    (void)argument;

    ESP_LOGI(
        TAG,
        "Alarm started: %s, play count=%u",
        ALARM_FILE_PATH,
        (unsigned int)NTPCLOCK_ALARM_PLAY_COUNT
    );

    for (unsigned int play_index = 0U;
         play_index < NTPCLOCK_ALARM_PLAY_COUNT;
         play_index++)
    {
        if (s_alarm_stop_requested)
        {
            break;
        }

        ESP_LOGI(
            TAG,
            "Playing alarm %u/%u",
            play_index + 1U,
            (unsigned int)NTPCLOCK_ALARM_PLAY_COUNT
        );

        esp_err_t result =
            audio_play_wav_file(ALARM_FILE_PATH);

        if (result != ESP_OK)
        {
            ESP_LOGE(
                TAG,
                "Alarm playback failed: %s",
                esp_err_to_name(result)
            );

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

    alarm_busy =
        s_alarm_ringing ||
        s_alarm_task_handle != NULL;

    portEXIT_CRITICAL(&s_alarm_lock);

    if (alarm_busy)
    {
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

        ESP_LOGE(
            TAG,
            "Failed to create alarm playback task"
        );

        return ESP_ERR_NO_MEM;
    }

    ntpclock_clear_snooze();

    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/*                         NTP Initialization                                 */
/* -------------------------------------------------------------------------- */

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

    esp_sntp_set_time_sync_notification_cb(
        ntpclock_sync_callback
    );

    esp_sntp_set_sync_interval(
        NTPCLOCK_SNTP_INTERVAL_MS
    );

    esp_sntp_init();

    /*
     * Wait for the first valid local time.
     */
    for (int retry = 0; retry < retry_count && !ntpclock_time_is_valid(); retry++)
    {
        ESP_LOGI(
            TAG,
            "Waiting for NTP time sync (%d/%d)",
            retry + 1,
            retry_count
        );

        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    struct tm time_info = {0};

    if (!ntpclock_get_local_time(&time_info))
    {
        ESP_LOGW(
            TAG,
            "Initial NTP synchronization timed out"
        );

        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(
        TAG,
        "NTP time available: %04d-%02d-%02d %02d:%02d:%02d",
        time_info.tm_year + 1900,
        time_info.tm_mon + 1,
        time_info.tm_mday,
        time_info.tm_hour,
        time_info.tm_min,
        time_info.tm_sec
    );

    /*
     * Create the daily scheduler only once.
     */
    if (s_ntp_sync_task_handle == NULL)
    {
        BaseType_t task_result = xTaskCreate(
            ntpclock_daily_sync_task,
            "ntp_daily_sync",
            NTPCLOCK_SYNC_TASK_STACK_SIZE,
            NULL,
            NTPCLOCK_SYNC_TASK_PRIORITY,
            &s_ntp_sync_task_handle
        );

        if (task_result != pdPASS)
        {
            s_ntp_sync_task_handle = NULL;

            ESP_LOGE(
                TAG,
                "Failed to create daily NTP task"
            );

            return ESP_ERR_NO_MEM;
        }
    }

    ESP_LOGI(
        TAG,
        "Daily NTP synchronization scheduled for 1:00 AM"
    );

    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/*                         Alarm Configuration                                */
/* -------------------------------------------------------------------------- */

esp_err_t ntpclock_set_alarm(
    int hour_24,
    int minute,
    bool enabled)
{
    if (hour_24 < 0 || hour_24 > 23 ||
        minute < 0 || minute > 59)
    {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_alarm_lock);

    g_alarm_hour = hour_24;
    g_alarm_minute = minute;
    g_alarm_enabled = enabled;
    s_alarm_snooze_until = 0;

    portEXIT_CRITICAL(&s_alarm_lock);

    ESP_LOGI(
        TAG,
        "Alarm configured: %02d:%02d, enabled=%s, WAV repeats=%u",
        hour_24,
        minute,
        enabled ? "true" : "false",
        (unsigned int)NTPCLOCK_ALARM_PLAY_COUNT
    );

    return ESP_OK;
}

void ntpclock_get_alarm(
    int *hour_24,
    int *minute,
    bool *enabled)
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

/* -------------------------------------------------------------------------- */
/*                         Alarm Snooze                                       */
/* -------------------------------------------------------------------------- */

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

    time_t snooze_until =
        current_time + (time_t)(snooze_minutes * 60U);

    portENTER_CRITICAL(&s_alarm_lock);

    g_alarm_snooze_minutes = snooze_minutes;
    s_alarm_snooze_until = snooze_until;

    portEXIT_CRITICAL(&s_alarm_lock);

    ntpclock_request_alarm_stop();

    ESP_LOGI(
        TAG,
        "Alarm snoozed for %u minute(s)",
        (unsigned int)snooze_minutes
    );

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

    due =
        s_alarm_snooze_until > 0 &&
        current_time >= s_alarm_snooze_until;

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
