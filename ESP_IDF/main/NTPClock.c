#include "NTPClock.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_sntp.h"

static const char *TAG = "NTPClock";

static lv_obj_t *s_time_label = NULL;
static lv_obj_t *s_date_label = NULL;
static _lock_t *s_lvgl_lock = NULL;

static bool ntpclock_time_is_valid(void)
{
    time_t now = 0;
    struct tm timeinfo = {0};

    time(&now);
    localtime_r(&now, &timeinfo);

    return timeinfo.tm_year >= (2024 - 1900);
}

esp_err_t ntpclock_init(const char *timezone, int retry_count)
{
    ESP_LOGI(TAG, "Initializing SNTP");

    if (timezone == NULL) {
        ESP_LOGE(TAG, "Timezone is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    setenv("TZ", timezone, 1);
    tzset();

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.google.com");
    esp_sntp_init();

    time_t now = 0;
    struct tm timeinfo = {0};

    int retry = 0;

    while (timeinfo.tm_year < (2024 - 1900) && ++retry < retry_count) {
        ESP_LOGI(TAG, "Waiting for NTP time sync... (%d/%d)", retry, retry_count);

        vTaskDelay(pdMS_TO_TICKS(1000));

        time(&now);
        localtime_r(&now, &timeinfo);
    }

    if (ntpclock_time_is_valid()) {
        time(&now);
        localtime_r(&now, &timeinfo);

        ESP_LOGI(
            TAG,
            "NTP synced: %04d-%02d-%02d %02d:%02d:%02d",
            timeinfo.tm_year + 1900,
            timeinfo.tm_mon + 1,
            timeinfo.tm_mday,
            timeinfo.tm_hour,
            timeinfo.tm_min
        );

        return ESP_OK;
    }

    ESP_LOGE(TAG, "NTP sync failed");
    return ESP_FAIL;
}

static void ntpclock_update_task(void *arg)
{
    char time_str[16];
    char date_str[32];

    while (1) {
        time_t now = 0;
        struct tm timeinfo = {0};

        time(&now);
        localtime_r(&now, &timeinfo);

        if (s_lvgl_lock != NULL) {
            _lock_acquire(s_lvgl_lock);
        }

        if (timeinfo.tm_year >= (2024 - 1900)) {
            strftime(time_str, sizeof(time_str), "%I:%M %p", &timeinfo);
            strftime(date_str, sizeof(date_str), "%a, %b %d", &timeinfo);

            if (s_time_label != NULL) {
                lv_label_set_text(s_time_label, time_str);
            }

            if (s_date_label != NULL) {
                lv_label_set_text(s_date_label, date_str);
            }
        } else {
            if (s_time_label != NULL) {
                lv_label_set_text(s_time_label, "--:--");
            }

            if (s_date_label != NULL) {
                lv_label_set_text(s_date_label, "Syncing time...");
            }
        }

        if (s_lvgl_lock != NULL) {
            _lock_release(s_lvgl_lock);
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void ntpclock_start_task(
    lv_obj_t *time_label,
    lv_obj_t *date_label,
    _lock_t *lvgl_lock
)
{
    s_time_label = time_label;
    s_date_label = date_label;
    s_lvgl_lock = lvgl_lock;

    xTaskCreate(
        ntpclock_update_task,
        "ntpclock_update",
        4096,
        NULL,
        3,
        NULL
    );
}