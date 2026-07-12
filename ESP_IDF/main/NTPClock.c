#include "NTPClock.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_sntp.h"

static const char *TAG = "NTPClock";

static ntpclock_update_callback_t s_update_callback = NULL;
static void *s_callback_user_data = NULL;
static TaskHandle_t s_update_task_handle = NULL;

static bool ntpclock_time_is_valid(void)
{
    time_t now = 0;
    struct tm time_info = {0};

    time(&now);
    localtime_r(&now, &time_info);

    return time_info.tm_year >= (2024 - 1900);
}

esp_err_t ntpclock_init(
    const char *timezone,
    int retry_count
)
{
    if (timezone == NULL || retry_count <= 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    setenv("TZ", timezone, 1);
    tzset();

    ESP_LOGI(TAG, "Initializing SNTP");

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.google.com");
    esp_sntp_init();

    for (int retry = 1; retry <= retry_count; retry++)
    {
        if (ntpclock_time_is_valid())
        {
            time_t now = 0;
            struct tm time_info = {0};

            time(&now);
            localtime_r(&now, &time_info);

            ESP_LOGI(
                TAG,
                "NTP synchronized: %04d-%02d-%02d %02d:%02d:%02d",
                time_info.tm_year + 1900,
                time_info.tm_mon + 1,
                time_info.tm_mday,
                time_info.tm_hour,
                time_info.tm_min,
                time_info.tm_sec
            );

            return ESP_OK;
        }

        ESP_LOGI(
            TAG,
            "Waiting for NTP time sync (%d/%d)",
            retry,
            retry_count
        );

        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGW(
        TAG,
        "Initial NTP synchronization timed out; SNTP remains active"
    );

    return ESP_ERR_TIMEOUT;
}

static void ntpclock_update_task(void *argument)
{
    (void)argument;

    char previous_time[16] = "";
    bool previous_valid = false;

    while (true)
    {
        char time_text[16];

        time_t now = 0;
        struct tm time_info = {0};

        time(&now);
        localtime_r(&now, &time_info);

        bool valid =
            time_info.tm_year >= (2024 - 1900);

        if (valid)
        {
            strftime(
                time_text,
                sizeof(time_text),
                "%I:%M %p",
                &time_info
            );

            /* Remove the leading zero from 01:00 through 09:59. */
            if (time_text[0] == '0')
            {
                memmove(
                    time_text,
                    time_text + 1,
                    strlen(time_text)
                );
            }
        }
        else
        {
            snprintf(
                time_text,
                sizeof(time_text),
                "--:-- --"
            );
        }

        if (s_update_callback != NULL &&
            (valid != previous_valid ||
             strcmp(previous_time, time_text) != 0))
        {
            snprintf(
                previous_time,
                sizeof(previous_time),
                "%s",
                time_text
            );

            previous_valid = valid;

            s_update_callback(
                time_text,
                s_callback_user_data
            );
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t ntpclock_start_task(
    ntpclock_update_callback_t callback,
    void *user_data
)
{
    if (callback == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_update_task_handle != NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_update_callback = callback;
    s_callback_user_data = user_data;

    BaseType_t task_result =
        xTaskCreate(
            ntpclock_update_task,
            "ntpclock_update",
            4096,
            NULL,
            3,
            &s_update_task_handle
        );

    if (task_result != pdPASS)
    {
        s_update_task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "NTPClock update task started");

    return ESP_OK;
}