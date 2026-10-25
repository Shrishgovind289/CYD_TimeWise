
#include "LED.h"

#include "driver/ledc.h"
#include "esp_log.h"

static const char *TAG = "LED";

#define LED_MODE           LEDC_LOW_SPEED_MODE
#define LED_TIMER          LEDC_TIMER_1

#define LED_RED_CHANNEL    LEDC_CHANNEL_3
#define LED_GREEN_CHANNEL  LEDC_CHANNEL_4
#define LED_BLUE_CHANNEL   LEDC_CHANNEL_5

static bool g_led_initialized = false;

/* -----------------------------------------------------------
 * Configure individual PWM channel
 * ----------------------------------------------------------- */
static esp_err_t led_channel_init(
    int gpio,
    ledc_channel_t channel)
{
    ledc_channel_config_t config = {
        .gpio_num = gpio,
        .speed_mode = LED_MODE,
        .channel = channel,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LED_TIMER,
        .duty = LED_PWM_MAX,
        .hpoint = 0,
        .flags.output_invert = 1
    };

    return ledc_channel_config(&config);
}

/* -----------------------------------------------------------
 * Set individual LED brightness
 *
 * PWM input: 0 - 1024
 *
 * Common-anode RGB LED:
 * 0    = OFF
 * 1024 = Maximum brightness
 * ----------------------------------------------------------- */
static esp_err_t led_set_pwm(
    ledc_channel_t channel,
    uint16_t pwm)
{
    if (!g_led_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (pwm > LED_PWM_MAX)
    {
        return ESP_ERR_INVALID_ARG;
    }

    // Active LOW LED: invert requested brightness
    uint32_t duty = LED_PWM_MAX - pwm;

    // Avoid full-scale LEDC edge case
    if (duty == LED_PWM_MAX)
    {
        duty = LED_PWM_MAX - 1;
    }

    esp_err_t result = ledc_set_duty(
        LED_MODE,
        channel,
        duty
    );

    if (result != ESP_OK)
    {
        return result;
    }

    return ledc_update_duty(
        LED_MODE,
        channel
    );
}

/* -----------------------------------------------------------
 * Initialize LEDC timer and RGB channels
 * ----------------------------------------------------------- */
esp_err_t led_init(void)
{
    if (g_led_initialized)
    {
        return ESP_OK;
    }

    ledc_timer_config_t timer = {
        .speed_mode = LED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LED_TIMER,
        .freq_hz = LED_PWM_FREQ,
        .clk_cfg = LEDC_AUTO_CLK
    };

    esp_err_t result = ledc_timer_config(&timer);

    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "LEDC timer initialization failed");
        return result;
    }

    result = led_channel_init(
        LED_RED_PIN,
        LED_RED_CHANNEL
    );

    if (result != ESP_OK)
    {
        return result;
    }

    result = led_channel_init(
        LED_GREEN_PIN,
        LED_GREEN_CHANNEL
    );

    if (result != ESP_OK)
    {
        return result;
    }

    result = led_channel_init(
        LED_BLUE_PIN,
        LED_BLUE_CHANNEL
    );

    if (result != ESP_OK)
    {
        return result;
    }

    g_led_initialized = true;

    // Default all LEDs to OFF
    led_set_red(0);
    led_set_green(0);
    led_set_blue(0);

    ESP_LOGI(TAG, "RGB LED PWM initialized");

    return ESP_OK;
}

/* -----------------------------------------------------------
 * Red LED - GPIO 22
 * ----------------------------------------------------------- */
esp_err_t led_set_red(uint16_t pwm)
{
    return led_set_pwm(LED_RED_CHANNEL, pwm);
}

/* -----------------------------------------------------------
 * Green LED - GPIO 16
 * ----------------------------------------------------------- */
esp_err_t led_set_green(uint16_t pwm)
{
    return led_set_pwm(LED_GREEN_CHANNEL, pwm);
}

/* -----------------------------------------------------------
 * Blue LED - GPIO 17
 * ----------------------------------------------------------- */
esp_err_t led_set_blue(uint16_t pwm)
{
    return led_set_pwm(LED_BLUE_CHANNEL, pwm);
}
