/*
 * main.c
 *
 * RGB565 background + LVGL sun astro body test
 *
 * - Mount SD card
 * - Check /sdcard/BG/SUNNY.bin
 * - Draw raw RGB565 background directly to LCD
 * - Display sun.c image as astro body using LVGL
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "esp_heap_caps.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/sdspi_host.h"

#include "sdmmc_cmd.h"

#include "lvgl.h"
#include "TFT_Display.h"

#include "esp_lcd_panel_ops.h"

/*
 * sun.c defines:
 *
 * const lv_image_dsc_t sun
 */
LV_IMAGE_DECLARE(sun);

/* -------------------------------------------------------------------------- */
/*                                App config                                  */
/* -------------------------------------------------------------------------- */

static const char *TAG = "RGB565_BG_ASTRO";

#define SD_MOUNT_POINT      "/sdcard"

/*
 * SD card file:
 *
 * SD card structure:
 *
 * /BG/SUNNY.bin
 */
#define TEST_BG_FILE        "/sdcard/BG/SUNNY.bin"

/*
 * Display resolution.
 */
#define LCD_WIDTH           480
#define LCD_HEIGHT          320

/*
 * 480 x 320 x 2 = 307200 bytes
 */
#define RGB565_BG_SIZE      (LCD_WIDTH * LCD_HEIGHT * 2)

/*
 * Draw 20 lines at once:
 *
 * 480 x 20 x 2 = 19200 bytes
 */
#define DRAW_CHUNK_LINES    20

/*
 * Set to 1 if RGB565 colors need byte swap.
 */
#define RGB565_SWAP_BYTES   1

/*
 * SD card pins on LCDWIKI ESP32-32E board.
 */
#define SD_PIN_NUM_MISO     GPIO_NUM_19
#define SD_PIN_NUM_MOSI     GPIO_NUM_23
#define SD_PIN_NUM_CLK      GPIO_NUM_18
#define SD_PIN_NUM_CS       GPIO_NUM_5

/*
 * Use SPI3_HOST for SD.
 * TFT_Display.c should use SPI2_HOST for LCD.
 */
#define SD_SPI_HOST         SPI3_HOST

/*
 * Astro arc settings.
 *
 * The sun will sit on this arc.
 */
#define ASTRO_ARC_X_START   10
#define ASTRO_ARC_X_END     470
#define ASTRO_ARC_Y_BASE    158
#define ASTRO_ARC_HEIGHT    135

/*
 * sun.c image size:
 *
 * width  = 20
 * height = 20
 */
#define ASTRO_BODY_SIZE     20
#define ASTRO_BODY_HALF     10

static sdmmc_card_t *g_card = NULL;
static lv_obj_t *astro_body = NULL;

/* -------------------------------------------------------------------------- */
/*                          Check background file                             */
/* -------------------------------------------------------------------------- */

static esp_err_t check_background_file(void)
{
    struct stat st;

    ESP_LOGI(TAG, "Checking file: %s", TEST_BG_FILE);

    if (stat(TEST_BG_FILE, &st) != 0) {
        ESP_LOGE(TAG, "File not found: %s", TEST_BG_FILE);
        ESP_LOGE(TAG, "Expected SD path: /BG/SUNNY.bin");
        return ESP_FAIL;
    }

    if (S_ISDIR(st.st_mode)) {
        ESP_LOGE(TAG, "Path is a directory, not a file: %s", TEST_BG_FILE);
        return ESP_FAIL;
    }

    ESP_LOGI(
        TAG,
        "Found file: %s, size=%" PRIu64 " bytes",
        TEST_BG_FILE,
        (uint64_t)st.st_size
    );

    if (st.st_size != RGB565_BG_SIZE) {
        ESP_LOGE(
            TAG,
            "Wrong file size. Expected %d bytes, got %" PRIu64 " bytes",
            RGB565_BG_SIZE,
            (uint64_t)st.st_size
        );

        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Background file size is correct");

    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/*                              SD card mount                                 */
/* -------------------------------------------------------------------------- */

static esp_err_t mount_sd_card(void)
{
    ESP_LOGI(TAG, "Mounting SD card...");

    vTaskDelay(pdMS_TO_TICKS(300));

    /*
     * Keep SD CS high before SPI init.
     */
    gpio_config_t cs_cfg = {
        .pin_bit_mask = 1ULL << SD_PIN_NUM_CS,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&cs_cfg));
    gpio_set_level(SD_PIN_NUM_CS, 1);

    vTaskDelay(pdMS_TO_TICKS(100));

    gpio_set_pull_mode(SD_PIN_NUM_MISO, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(SD_PIN_NUM_MOSI, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(SD_PIN_NUM_CLK, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(SD_PIN_NUM_CS, GPIO_PULLUP_ONLY);

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SD_PIN_NUM_MOSI,
        .miso_io_num = SD_PIN_NUM_MISO,
        .sclk_io_num = SD_PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };

    esp_err_t ret = spi_bus_initialize(
        SD_SPI_HOST,
        &bus_cfg,
        SDSPI_DEFAULT_DMA
    );

    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "SPI bus initialize failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "SD SPI bus already initialized, continuing");
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;
    host.max_freq_khz = 1000;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = SD_PIN_NUM_CS;
    slot_config.host_id = SD_SPI_HOST;

    ret = esp_vfs_fat_sdspi_mount(
        SD_MOUNT_POINT,
        &host,
        &slot_config,
        &mount_config,
        &g_card
    );

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "SD card mounted successfully");
    sdmmc_card_print_info(stdout, g_card);

    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/*                          Optional byte swap                                */
/* -------------------------------------------------------------------------- */

static void rgb565_swap_bytes(uint16_t *buf, size_t pixel_count)
{
#if RGB565_SWAP_BYTES
    for (size_t i = 0; i < pixel_count; i++) {
        uint16_t p = buf[i];
        buf[i] = (uint16_t)((p << 8) | (p >> 8));
    }
#else
    (void)buf;
    (void)pixel_count;
#endif
}

/* -------------------------------------------------------------------------- */
/*                    Draw raw RGB565 file directly to LCD                    */
/* -------------------------------------------------------------------------- */

static esp_err_t draw_rgb565_background_from_sd(
    esp_lcd_panel_handle_t panel_handle,
    const char *path
)
{
    ESP_LOGI(TAG, "Opening RGB565 background: %s", path);

    FILE *f = fopen(path, "rb");

    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open background file: %s", path);
        return ESP_FAIL;
    }

    const size_t pixels_per_chunk = LCD_WIDTH * DRAW_CHUNK_LINES;
    const size_t bytes_per_chunk = pixels_per_chunk * 2;

    uint16_t *line_buf = heap_caps_malloc(
        bytes_per_chunk,
        MALLOC_CAP_DMA
    );

    if (line_buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate RGB565 draw buffer");
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(
        TAG,
        "Drawing background in chunks: %d lines per chunk, %d bytes",
        DRAW_CHUNK_LINES,
        (int)bytes_per_chunk
    );

    for (int y = 0; y < LCD_HEIGHT; y += DRAW_CHUNK_LINES) {
        int lines_now = DRAW_CHUNK_LINES;

        if ((y + lines_now) > LCD_HEIGHT) {
            lines_now = LCD_HEIGHT - y;
        }

        size_t bytes_to_read = LCD_WIDTH * lines_now * 2;

        size_t bytes_read = fread(
            line_buf,
            1,
            bytes_to_read,
            f
        );

        if (bytes_read != bytes_to_read) {
            ESP_LOGE(
                TAG,
                "File read error at y=%d. Expected %d bytes, got %d bytes",
                y,
                (int)bytes_to_read,
                (int)bytes_read
            );

            free(line_buf);
            fclose(f);
            return ESP_FAIL;
        }

        rgb565_swap_bytes(
            line_buf,
            LCD_WIDTH * lines_now
        );

        esp_err_t ret = esp_lcd_panel_draw_bitmap(
            panel_handle,
            0,
            y,
            LCD_WIDTH,
            y + lines_now,
            line_buf
        );

        if (ret != ESP_OK) {
            ESP_LOGE(
                TAG,
                "esp_lcd_panel_draw_bitmap failed at y=%d: %s",
                y,
                esp_err_to_name(ret)
            );

            free(line_buf);
            fclose(f);
            return ret;
        }
    }

    free(line_buf);
    fclose(f);

    ESP_LOGI(TAG, "Background draw complete");

    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/*                          Astro body display                                */
/* -------------------------------------------------------------------------- */

static void ui_set_astro_position(float progress)
{
    if (astro_body == NULL) {
        return;
    }

    if (progress < 0.0f) {
        progress = 0.0f;
    }

    if (progress > 1.0f) {
        progress = 1.0f;
    }

    int x = ASTRO_ARC_X_START +
            (int)((ASTRO_ARC_X_END - ASTRO_ARC_X_START) * progress);

    float curve = 4.0f * progress * (1.0f - progress);

    int y = ASTRO_ARC_Y_BASE -
            (int)(ASTRO_ARC_HEIGHT * curve);

    /*
     * sun.c is 20 x 20.
     * Subtract 10 so the image center sits on the arc point.
     */
    lv_obj_set_pos(
        astro_body,
        x - ASTRO_BODY_HALF,
        y - ASTRO_BODY_HALF
    );
}

static void create_astro_body_ui(lv_display_t *display)
{
    lv_obj_t *scr = lv_display_get_screen_active(display);

    /*
     * Create sun image from sun.c.
     */
    astro_body = lv_image_create(scr);
    lv_image_set_src(astro_body, &sun);

    /*
     * No clicking needed.
     */
    lv_obj_clear_flag(astro_body, LV_OBJ_FLAG_CLICKABLE);

    /*
     * Optional scale.
     *
     * 256 = 1.0x in LVGL image scaling.
     * Keep 256 for original 20 x 20 size.
     */
    lv_image_set_scale(astro_body, 256);

    /*
     * Initial astro position.
     *
     * 0.0 = left side
     * 0.5 = top center
     * 1.0 = right side
     */
    ui_set_astro_position(0.65f);

    ESP_LOGI(TAG, "sun.c displayed as astro body");
}

/* -------------------------------------------------------------------------- */
/*                                app_main                                    */
/* -------------------------------------------------------------------------- */

void app_main(void)
{
    ESP_LOGI(TAG, "Starting RGB565 background + astro body test");

    /*
     * 1. Mount SD card.
     */
    esp_err_t ret = mount_sd_card();

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Stopping test because SD mount failed");

        while (1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    /*
     * 2. Check background file.
     */
    ret = check_background_file();

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Stopping test because background file check failed");

        while (1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    /*
     * 3. Initialize TFT + LVGL.
     */
    lv_display_t *display = NULL;
    ESP_ERROR_CHECK(tft_display_init(&display));

    /*
     * 4. Get LCD panel handle from LVGL display user data.
     */
    esp_lcd_panel_handle_t panel_handle =
        (esp_lcd_panel_handle_t)lv_display_get_user_data(display);

    if (panel_handle == NULL) {
        ESP_LOGE(TAG, "LCD panel handle is NULL");
        ESP_LOGE(TAG, "Check TFT_Display.c: lv_display_set_user_data(display, panel_handle)");

        while (1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    vTaskDelay(pdMS_TO_TICKS(500));

    /*
     * 5. Draw raw RGB565 background from SD.
     */
    ret = draw_rgb565_background_from_sd(
        panel_handle,
        TEST_BG_FILE
    );

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Background draw failed");

        while (1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    ESP_LOGI(TAG, "Background displayed");

    /*
     * 6. Display sun.c as astro body.
     *
     * LVGL calls must be protected using TFT_Display lock.
     */
    tft_display_lock();
    create_astro_body_ui(display);
    tft_display_unlock();

    ESP_LOGI(TAG, "Astro body displayed");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
