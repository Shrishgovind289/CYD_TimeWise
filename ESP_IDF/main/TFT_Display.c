#include "TFT_Display.h"

#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include <sys/param.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"

#define LCD_HOST SPI2_HOST

#define TFT_LCD_PIXEL_CLOCK_HZ     (40 * 1000 * 1000)

#define TFT_LCD_BK_LIGHT_ON_LEVEL  1
#define TFT_LCD_BK_LIGHT_OFF_LEVEL 0

#define TFT_PIN_NUM_SCLK           14
#define TFT_PIN_NUM_MOSI           13
#define TFT_PIN_NUM_MISO           -1
#define TFT_PIN_NUM_LCD_DC         2
#define TFT_PIN_NUM_LCD_RST        -1
#define TFT_PIN_NUM_LCD_CS         15
#define TFT_PIN_NUM_BK_LIGHT       27

#define TFT_LCD_H_RES              480
#define TFT_LCD_V_RES              320

#define TFT_LCD_CMD_BITS           8
#define TFT_LCD_PARAM_BITS         8

#define TFT_LVGL_DRAW_BUF_LINES    20
#define TFT_LVGL_TICK_PERIOD_MS    2
#define TFT_LVGL_TASK_MAX_DELAY_MS 500
#define TFT_LVGL_TASK_MIN_DELAY_MS (1000 / CONFIG_FREERTOS_HZ)
#define TFT_LVGL_TASK_STACK_SIZE   (16 * 1024)
#define TFT_LVGL_TASK_PRIORITY     2

static const char *TAG = "TFT_Display";

static _lock_t s_lvgl_api_lock;

static lv_display_t *s_display = NULL;
static esp_lcd_panel_handle_t s_panel_handle = NULL;
static esp_lcd_panel_io_handle_t s_io_handle = NULL;
static esp_timer_handle_t s_lvgl_tick_timer = NULL;

static bool tft_display_notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    lv_display_t *disp = (lv_display_t *)user_ctx;
    lv_display_flush_ready(disp);

    return false;
}

static void tft_display_rotation_update(lv_display_t *disp)
{
    esp_lcd_panel_handle_t panel_handle = lv_display_get_user_data(disp);
    esp_lcd_panel_swap_xy(panel_handle, true);
    esp_lcd_panel_mirror(panel_handle, true, true);
}

static void tft_display_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    tft_display_rotation_update(disp);

    esp_lcd_panel_handle_t panel_handle = lv_display_get_user_data(disp);

    int offsetx1 = area->x1;
    int offsetx2 = area->x2;
    int offsety1 = area->y1;
    int offsety2 = area->y2;

    /*
     * SPI LCD expects RGB565 byte order differently,
     * so LVGL buffer bytes are swapped before sending.
     */
    lv_draw_sw_rgb565_swap(
        px_map,
        (offsetx2 + 1 - offsetx1) * (offsety2 + 1 - offsety1)
    );

    esp_lcd_panel_draw_bitmap(
        panel_handle,
        offsetx1,
        offsety1,
        offsetx2 + 1,
        offsety2 + 1,
        px_map
    );
}

static void tft_display_lvgl_task(void *arg)
{
    ESP_LOGI(TAG, "Starting LVGL task");

    uint32_t time_till_next_ms = 0;

    while (1) {
        _lock_acquire(&s_lvgl_api_lock);

        time_till_next_ms = lv_timer_handler();

        _lock_release(&s_lvgl_api_lock);

        time_till_next_ms = MAX(
            time_till_next_ms,
            TFT_LVGL_TASK_MIN_DELAY_MS
        );

        time_till_next_ms = MIN(
            time_till_next_ms,
            TFT_LVGL_TASK_MAX_DELAY_MS
        );

        usleep(1000 * time_till_next_ms);
    }
}

static void tft_display_increase_lvgl_tick(void *arg)
{
    lv_tick_inc(TFT_LVGL_TICK_PERIOD_MS);
}

_lock_t *tft_display_get_lvgl_lock(void)
{
    return &s_lvgl_api_lock;
}

void tft_display_lock(void)
{
    _lock_acquire(&s_lvgl_api_lock);
}

void tft_display_unlock(void)
{
    _lock_release(&s_lvgl_api_lock);
}

esp_err_t tft_display_init(lv_display_t **display_out)
{
    if (display_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Configure LCD backlight GPIO");

    gpio_config_t bk_gpio_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << TFT_PIN_NUM_BK_LIGHT,
    };

    ESP_ERROR_CHECK(gpio_config(&bk_gpio_config));
    gpio_set_level(TFT_PIN_NUM_BK_LIGHT, TFT_LCD_BK_LIGHT_OFF_LEVEL);

    ESP_LOGI(TAG, "Initialize SPI bus");

    spi_bus_config_t buscfg = {
        .sclk_io_num = TFT_PIN_NUM_SCLK,
        .mosi_io_num = TFT_PIN_NUM_MOSI,
        .miso_io_num = TFT_PIN_NUM_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = TFT_LCD_H_RES * 80 * sizeof(uint16_t),
    };

    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    ESP_LOGI(TAG, "Install panel IO");

    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = TFT_PIN_NUM_LCD_DC,
        .cs_gpio_num = TFT_PIN_NUM_LCD_CS,
        .pclk_hz = TFT_LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = TFT_LCD_CMD_BITS,
        .lcd_param_bits = TFT_LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };

    ESP_ERROR_CHECK(
        esp_lcd_new_panel_io_spi(
            (esp_lcd_spi_bus_handle_t)LCD_HOST,
            &io_config,
            &s_io_handle
        )
    );

    ESP_LOGI(TAG, "Install LCD panel driver");

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = TFT_PIN_NUM_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };

    ESP_ERROR_CHECK(
        esp_lcd_new_panel_st7789(s_io_handle, &panel_config, &s_panel_handle)
    );

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel_handle));

    /*
     * Initial mirror setting.
     * Rotation is also applied again inside the LVGL flush callback.
     */
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel_handle, true, true));

    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel_handle, true));

    //ESP_LOGI(TAG, "Turn on LCD backlight");
    gpio_set_level(TFT_PIN_NUM_BK_LIGHT, TFT_LCD_BK_LIGHT_ON_LEVEL);

    //ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    lv_fs_stdio_init();

    ESP_LOGI(TAG, "LVGL stdio filesystem initialized");         

    s_display = lv_display_create(TFT_LCD_H_RES, TFT_LCD_V_RES);

    if (s_display == NULL) {
        ESP_LOGE(TAG, "Failed to create LVGL display");
        return ESP_FAIL;
    }

    size_t draw_buffer_size =
        TFT_LCD_H_RES *
        TFT_LVGL_DRAW_BUF_LINES *
        sizeof(lv_color16_t);

    void *buf1 = spi_bus_dma_memory_alloc(LCD_HOST, draw_buffer_size, 0);
    void *buf2 = spi_bus_dma_memory_alloc(LCD_HOST, draw_buffer_size, 0);

    if (buf1 == NULL || buf2 == NULL) {
        ESP_LOGE(TAG, "Failed to allocate LVGL draw buffers");
        return ESP_ERR_NO_MEM;
    }

    lv_display_set_buffers(
        s_display,
        buf1,
        buf2,
        draw_buffer_size,
        LV_DISPLAY_RENDER_MODE_PARTIAL
    );

    lv_display_set_user_data(s_display, s_panel_handle);
    lv_display_set_color_format(s_display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_display, tft_display_flush_cb);

    ESP_LOGI(TAG, "Install LVGL tick timer");

    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &tft_display_increase_lvgl_tick,
        .name = "lvgl_tick",
    };

    ESP_ERROR_CHECK(
        esp_timer_create(
            &lvgl_tick_timer_args,
            &s_lvgl_tick_timer
        )
    );

    ESP_ERROR_CHECK(
        esp_timer_start_periodic(
            s_lvgl_tick_timer,
            TFT_LVGL_TICK_PERIOD_MS * 1000
        )
    );

    ESP_LOGI(TAG, "Register LCD flush callback");

    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = tft_display_notify_lvgl_flush_ready,
    };

    ESP_ERROR_CHECK(
        esp_lcd_panel_io_register_event_callbacks(
            s_io_handle,
            &cbs,
            s_display
        )
    );

    ESP_LOGI(TAG, "Create LVGL task");

    xTaskCreate(
        tft_display_lvgl_task,
        "LVGL",
        TFT_LVGL_TASK_STACK_SIZE,
        NULL,
        TFT_LVGL_TASK_PRIORITY,
        NULL
    );

    *display_out = s_display;

    ESP_LOGI(TAG, "TFT display initialized");

    return ESP_OK;
}