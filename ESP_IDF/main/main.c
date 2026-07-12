/*
 * TimeWise - main.c
 *
 * main.c only starts and connects the modules:
 *
 *   SDCard          -> mounts storage
 *   TFT_Display     -> owns LCD, LVGL, GUI, backgrounds and icons
 *   WeatherStation  -> requests weather and decides paths/theme
 *   NTPClock        -> synchronizes and formats time
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

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

#include "NTPClock.h"
#include "SDCard.h"
#include "TFT_Display.h"
#include "WeatherStation.h"

static const char *TAG = "TimeWise";

/* Keep the real private values only in your local project. */
#define WIFI_SSID           "TP-195_2"
#define WIFI_PASS           "Porsche@911_GT"

#define WEATHER_API_KEY     "04ea7544d7474d9fb99172321250307"
#define WEATHER_LOCATION    "Jersey_City"

#define LOCAL_TIMEZONE      "EST5EDT,M3.2.0/2,M11.1.0/2"

#define WIFI_MAX_RETRIES    10
#define NTP_RETRY_COUNT     30

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAILED_BIT     BIT1

static EventGroupHandle_t s_wifi_event_group = NULL;
static int s_wifi_retry_count = 0;

static void copy_text(
    char *destination,
    size_t destination_size,
    const char *source
)
{
    if (destination == NULL || destination_size == 0)
    {
        return;
    }

    if (source == NULL)
    {
        destination[0] = '\0';
        return;
    }

    snprintf(
        destination,
        destination_size,
        "%s",
        source
    );
}

static void wifi_event_handler(
    void *handler_argument,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data
)
{
    (void)handler_argument;
    (void)event_data;

    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
        return;
    }

    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        xEventGroupClearBits(
            s_wifi_event_group,
            WIFI_CONNECTED_BIT
        );

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

            xEventGroupSetBits(
                s_wifi_event_group,
                WIFI_FAILED_BIT
            );
        }

        return;
    }

    if (event_base == IP_EVENT &&
        event_id == IP_EVENT_STA_GOT_IP)
    {
        s_wifi_retry_count = 0;

        xEventGroupClearBits(
            s_wifi_event_group,
            WIFI_FAILED_BIT
        );

        xEventGroupSetBits(
            s_wifi_event_group,
            WIFI_CONNECTED_BIT
        );

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
    ESP_ERROR_CHECK(initialize_nvs());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

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

    ESP_ERROR_CHECK(esp_wifi_init(&initialization));

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            wifi_event_handler,
            NULL
        )
    );

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            wifi_event_handler,
            NULL
        )
    );

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

    configuration.sta.threshold.authmode =
        WIFI_AUTH_WPA2_PSK;

    configuration.sta.pmf_cfg.capable = true;
    configuration.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    ESP_ERROR_CHECK(
        esp_wifi_set_config(
            WIFI_IF_STA,
            &configuration
        )
    );

    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to Wi-Fi");

    EventBits_t result_bits =
        xEventGroupWaitBits(
            s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAILED_BIT,
            pdFALSE,
            pdFALSE,
            pdMS_TO_TICKS(30000)
        );

    if ((result_bits & WIFI_CONNECTED_BIT) != 0)
    {
        return ESP_OK;
    }

    if ((result_bits & WIFI_FAILED_BIT) != 0)
    {
        return ESP_FAIL;
    }

    return ESP_ERR_TIMEOUT;
}

static void clock_update_callback(const char *time_text, void *user_data)
{
    (void)user_data;

    tft_dashboard_set_time(time_text);
}

static void weather_update_callback(const weatherstation_update_t *update, void *user_data)
{
    (void)user_data;

    if (update == NULL)
    {
        return;
    }

    tft_dashboard_set_weather(
        update->temperature_c,
        update->condition,
        update->location,
        update->background_path,
        update->icon_path,
        update->use_dark_text
    );
}

void app_main(void)
{
    ESP_LOGI(TAG, "TimeWise starting");
    ESP_LOGI(TAG, "Reset reason: %d", (int)esp_reset_reason());

    /* Give the SD card a moment after power-up. */
    vTaskDelay(pdMS_TO_TICKS(500));

    esp_err_t sd_result = sdcard_init();

    if (sd_result == ESP_OK)
    {
        ESP_LOGI(TAG, "SD card mounted");
        sdcard_check_file("/ICO/SUNNY.BIN");
    }
    else
    {
        ESP_LOGW(TAG, "SD card mount failed: %s", esp_err_to_name(sd_result));
    }

    /* Build the GUI before network operations so placeholders are visible. */
    ESP_ERROR_CHECK(tft_display_init());
    ESP_ERROR_CHECK(tft_dashboard_create());

    esp_err_t wifi_result = connect_wifi();

    if (wifi_result != ESP_OK)
    {
        ESP_LOGW(
            TAG,
            "Wi-Fi unavailable: %s",
            esp_err_to_name(wifi_result)
        );

        return;
    }

    esp_err_t weather_result =
        weatherstation_start_task(
            WEATHER_API_KEY,
            WEATHER_LOCATION,
            weather_update_callback,
            NULL
        );

    if (weather_result != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "WeatherStation startup failed: %s",
            esp_err_to_name(weather_result)
        );
    }

    esp_err_t ntp_result = ntpclock_init( LOCAL_TIMEZONE, NTP_RETRY_COUNT);

    if (ntp_result != ESP_OK)
    {
        /* SNTP is still running, so the update task may obtain time later. */
        ESP_LOGW(
            TAG,
            "Initial NTP sync did not complete: %s",
            esp_err_to_name(ntp_result)
        );
    }

    esp_err_t clock_task_result = ntpclock_start_task(clock_update_callback, NULL);

    if (clock_task_result != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "NTPClock task startup failed: %s",
            esp_err_to_name(clock_task_result)
        );
    }

    ESP_LOGI(TAG, "TimeWise startup complete");
}