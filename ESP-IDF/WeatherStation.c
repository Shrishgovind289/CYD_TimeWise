#include "WeatherStation.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_http_client.h"

#include "cJSON.h"

#define WEATHER_BUF_SIZE 4096
#define WEATHER_UPDATE_INTERVAL_MS (60 * 60 * 1000)

static const char *TAG = "WeatherStation";

static char s_weather_response[WEATHER_BUF_SIZE];
static int s_weather_response_len = 0;

static const char *s_api_key = NULL;
static const char *s_location = NULL;

static lv_obj_t *s_weather_label = NULL;
static lv_obj_t *s_location_label = NULL;
static _lock_t *s_lvgl_lock = NULL;

static esp_err_t weatherstation_http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (evt->data_len > 0) {
                int copy_len = evt->data_len;

                if (s_weather_response_len + copy_len >= WEATHER_BUF_SIZE) {
                    copy_len = WEATHER_BUF_SIZE - s_weather_response_len - 1;
                }

                if (copy_len > 0) {
                    memcpy(
                        s_weather_response + s_weather_response_len,
                        evt->data,
                        copy_len
                    );

                    s_weather_response_len += copy_len;
                    s_weather_response[s_weather_response_len] = '\0';
                }
            }
            break;

        case HTTP_EVENT_ON_FINISH:
            s_weather_response[s_weather_response_len] = '\0';
            break;

        case HTTP_EVENT_ERROR:
            ESP_LOGE(TAG, "HTTP event error");
            break;

        default:
            break;
    }

    return ESP_OK;
}

static void weatherstation_set_labels(
    const char *weather_text,
    const char *location_text
)
{
    if (s_lvgl_lock != NULL) {
        _lock_acquire(s_lvgl_lock);
    }

    if (s_weather_label != NULL && weather_text != NULL) {
        lv_label_set_text(s_weather_label, weather_text);
    }

    if (s_location_label != NULL && location_text != NULL) {
        lv_label_set_text(s_location_label, location_text);
    }

    if (s_lvgl_lock != NULL) {
        _lock_release(s_lvgl_lock);
    }
}

static bool weatherstation_parse(const char *json_text)
{
    if (json_text == NULL || strlen(json_text) == 0) {
        ESP_LOGE(TAG, "Weather JSON is empty");
        return false;
    }

    cJSON *root = cJSON_Parse(json_text);

    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse weather JSON");
        return false;
    }

    bool success = false;

    cJSON *location = cJSON_GetObjectItem(root, "location");
    cJSON *current = cJSON_GetObjectItem(root, "current");

    if (cJSON_IsObject(location) && cJSON_IsObject(current)) {
        cJSON *name = cJSON_GetObjectItem(location, "name");
        cJSON *region = cJSON_GetObjectItem(location, "region");

        cJSON *temp_c = cJSON_GetObjectItem(current, "temp_c");
        cJSON *condition = cJSON_GetObjectItem(current, "condition");

        cJSON *condition_text = NULL;

        if (cJSON_IsObject(condition)) {
            condition_text = cJSON_GetObjectItem(condition, "text");
        }

        if (
            cJSON_IsString(name) &&
            cJSON_IsString(region) &&
            cJSON_IsNumber(temp_c) &&
            cJSON_IsString(condition_text)
        ) {
            char weather_text[96];
            char location_text[64];

            snprintf(
                weather_text,
                sizeof(weather_text),
                "%.1f C, %s",
                temp_c->valuedouble,
                condition_text->valuestring
            );

            snprintf(
                location_text,
                sizeof(location_text),
                "%s, %s",
                name->valuestring,
                region->valuestring
            );

            weatherstation_set_labels(weather_text, location_text);

            ESP_LOGI(TAG, "Weather: %s", weather_text);
            ESP_LOGI(TAG, "Location: %s", location_text);

            success = true;
        } else {
            ESP_LOGE(TAG, "Weather JSON missing required fields");
            weatherstation_set_labels("Weather error", "Bad JSON data");
        }
    } else {
        ESP_LOGE(TAG, "Weather JSON missing location/current object");
        weatherstation_set_labels("Weather error", "Missing JSON object");
    }

    cJSON_Delete(root);
    return success;
}

static void weatherstation_update_task(void *arg)
{
    char url[256];

    while (1) {
        if (s_api_key == NULL || s_location == NULL) {
            ESP_LOGE(TAG, "API key or location is NULL");
            weatherstation_set_labels("Weather error", "Config missing");

            vTaskDelay(pdMS_TO_TICKS(60 * 1000));
            continue;
        }

        snprintf(
            url,
            sizeof(url),
            "http://api.weatherapi.com/v1/forecast.json?key=%s&q=%s&days=1&aqi=no&alerts=no",
            s_api_key,
            s_location
        );

        s_weather_response_len = 0;
        memset(s_weather_response, 0, sizeof(s_weather_response));

        esp_http_client_config_t config = {0};
        config.url = url;
        config.event_handler = weatherstation_http_event_handler;
        config.timeout_ms = 10000;

        esp_http_client_handle_t client = esp_http_client_init(&config);

        if (client == NULL) {
            ESP_LOGE(TAG, "Failed to initialize HTTP client");
            weatherstation_set_labels("Weather error", "HTTP init failed");

            vTaskDelay(pdMS_TO_TICKS(60 * 1000));
            continue;
        }

        esp_err_t err = esp_http_client_perform(client);

        if (err == ESP_OK) {
            int status = esp_http_client_get_status_code(client);

            ESP_LOGI(TAG, "Weather HTTP status = %d", status);
            ESP_LOGI(TAG, "Weather response length = %d", s_weather_response_len);

            if (status == 200) {
                weatherstation_parse(s_weather_response);
            } else {
                ESP_LOGE(TAG, "Weather API returned status %d", status);
                weatherstation_set_labels("Weather error", "API failed");
            }
        } else {
            ESP_LOGE(TAG, "Weather HTTP request failed: %s", esp_err_to_name(err));
            weatherstation_set_labels("Weather error", "HTTP failed");
        }

        esp_http_client_cleanup(client);

        vTaskDelay(pdMS_TO_TICKS(WEATHER_UPDATE_INTERVAL_MS));
    }
}

void weatherstation_start_task(
    const char *api_key,
    const char *location,
    lv_obj_t *weather_label,
    lv_obj_t *location_label,
    _lock_t *lvgl_lock
)
{
    s_api_key = api_key;
    s_location = location;

    s_weather_label = weather_label;
    s_location_label = location_label;
    s_lvgl_lock = lvgl_lock;

    xTaskCreate(
        weatherstation_update_task,
        "weatherstation_update",
        8192,
        NULL,
        3,
        NULL
    );
}