/*
 * main.c
 *
 * RGB565 background + transparent sun and moon test
 *
 * - Mount MicroSD using SPI3
 * - Display /sdcard/BG/SUNNY.bin
 * - Alpha-blend sun.c and moon.c with the actual background
 * - Draw both objects directly to the LCD
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_attr.h"
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
 * Raw RGB565A8 arrays defined in sun.c and moon.c.
 *
 * Each array contains:
 *
 * First 800 bytes:
 *     20 x 20 RGB565 pixels
 *
 * Next 400 bytes:
 *     20 x 20 alpha values
 */
extern const uint8_t sun_map[];
extern const uint8_t moon_map[];

/* -------------------------------------------------------------------------- */
/*                           Application configuration                        */
/* -------------------------------------------------------------------------- */

static const char *TAG = "RGB565_ASTRO_TEST";

#define SD_MOUNT_POINT             "/sdcard"
#define TEST_BG_FILE               "/sdcard/BG/SUNNY.bin"

#define LCD_WIDTH                  480
#define LCD_HEIGHT                 320

#define RGB565_BG_SIZE             (LCD_WIDTH * LCD_HEIGHT * 2)
#define DRAW_CHUNK_LINES           20

/*
 * The background currently needs a byte swap before being
 * transmitted to the LCD.
 */
#define RGB565_SWAP_BYTES          1

/* -------------------------------------------------------------------------- */
/*                             MicroSD configuration                          */
/* -------------------------------------------------------------------------- */

#define SD_PIN_NUM_MISO            GPIO_NUM_19
#define SD_PIN_NUM_MOSI            GPIO_NUM_23
#define SD_PIN_NUM_CLK             GPIO_NUM_18
#define SD_PIN_NUM_CS              GPIO_NUM_5

#define SD_SPI_HOST                SPI3_HOST

#define SD_INITIAL_FREQ_KHZ        400
#define SD_MOUNT_RETRY_COUNT       3
#define SD_MOUNT_RETRY_DELAY_MS    500

/* -------------------------------------------------------------------------- */
/*                              Astro configuration                           */
/* -------------------------------------------------------------------------- */

#define ASTRO_ARC_X_START          10
#define ASTRO_ARC_X_END            470
#define ASTRO_ARC_Y_BASE           158
#define ASTRO_ARC_HEIGHT           135

#define ASTRO_WIDTH                20
#define ASTRO_HEIGHT               20
#define ASTRO_PIXEL_COUNT          (ASTRO_WIDTH * ASTRO_HEIGHT)

/*
 * Use separate DMA buffers because LCD transfers can be asynchronous.
 */
static DMA_ATTR uint16_t sun_draw_buffer[ASTRO_PIXEL_COUNT];
static DMA_ATTR uint16_t moon_draw_buffer[ASTRO_PIXEL_COUNT];

static sdmmc_card_t *g_card = NULL;

/* -------------------------------------------------------------------------- */
/*                              Stop application                              */
/* -------------------------------------------------------------------------- */

static void stop_application(const char *reason)
{
    ESP_LOGE(TAG, "%s", reason);

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* -------------------------------------------------------------------------- */
/*                         Verify background file                             */
/* -------------------------------------------------------------------------- */

static esp_err_t check_background_file(void)
{
    struct stat file_info;

    ESP_LOGI(TAG, "Checking background file: %s", TEST_BG_FILE);

    if (stat(TEST_BG_FILE, &file_info) != 0) {
        ESP_LOGE(TAG, "Background not found: %s", TEST_BG_FILE);
        ESP_LOGE(TAG, "Expected SD path: /BG/SUNNY.bin");
        return ESP_FAIL;
    }

    if (S_ISDIR(file_info.st_mode)) {
        ESP_LOGE(TAG, "Background path is a directory");
        return ESP_FAIL;
    }

    ESP_LOGI(
        TAG,
        "Background size: %" PRIu64 " bytes",
        (uint64_t)file_info.st_size
    );

    if ((uint64_t)file_info.st_size != (uint64_t)RGB565_BG_SIZE) {
        ESP_LOGE(
            TAG,
            "Wrong background size. Expected %d bytes, got %" PRIu64,
            RGB565_BG_SIZE,
            (uint64_t)file_info.st_size
        );

        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Background file is valid");

    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/*                           Configure MicroSD pins                           */
/* -------------------------------------------------------------------------- */

static esp_err_t configure_sd_pins(void)
{
    gpio_config_t cs_config = {
        .pin_bit_mask = 1ULL << SD_PIN_NUM_CS,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t ret = gpio_config(&cs_config);

    if (ret != ESP_OK) {
        return ret;
    }

    ret = gpio_set_level(SD_PIN_NUM_CS, 1);

    if (ret != ESP_OK) {
        return ret;
    }

    ret = gpio_set_pull_mode(
        SD_PIN_NUM_MISO,
        GPIO_PULLUP_ONLY
    );

    if (ret != ESP_OK) {
        return ret;
    }

    ret = gpio_set_pull_mode(
        SD_PIN_NUM_MOSI,
        GPIO_PULLUP_ONLY
    );

    if (ret != ESP_OK) {
        return ret;
    }

    ret = gpio_set_pull_mode(
        SD_PIN_NUM_CS,
        GPIO_PULLUP_ONLY
    );

    if (ret != ESP_OK) {
        return ret;
    }

    /*
     * SPI clock should not have a pull-up.
     */
    return gpio_set_pull_mode(
        SD_PIN_NUM_CLK,
        GPIO_FLOATING
    );
}

/* -------------------------------------------------------------------------- */
/*                              Mount MicroSD                                 */
/* -------------------------------------------------------------------------- */

static esp_err_t mount_sd_card(void)
{
    ESP_LOGI(TAG, "Preparing MicroSD card");

    vTaskDelay(pdMS_TO_TICKS(500));

    esp_err_t ret = configure_sd_pins();

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "SD pin configuration failed: %s",
            esp_err_to_name(ret)
        );

        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(200));

    spi_bus_config_t bus_config = {
        .mosi_io_num = SD_PIN_NUM_MOSI,
        .miso_io_num = SD_PIN_NUM_MISO,
        .sclk_io_num = SD_PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };

    ret = spi_bus_initialize(
        SD_SPI_HOST,
        &bus_config,
        SDSPI_DEFAULT_DMA
    );

    if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "SPI3 is already initialized");
    }
    else if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "SPI3 initialization failed: %s",
            esp_err_to_name(ret)
        );

        return ret;
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();

    host.slot = SD_SPI_HOST;
    host.max_freq_khz = SD_INITIAL_FREQ_KHZ;

    sdspi_device_config_t slot_config =
        SDSPI_DEVICE_CONFIG_DEFAULT();

    slot_config.gpio_cs = SD_PIN_NUM_CS;
    slot_config.host_id = SD_SPI_HOST;

    for (
        int attempt = 1;
        attempt <= SD_MOUNT_RETRY_COUNT;
        attempt++
    ) {
        ESP_LOGI(
            TAG,
            "Mount attempt %d/%d at %d kHz",
            attempt,
            SD_MOUNT_RETRY_COUNT,
            SD_INITIAL_FREQ_KHZ
        );

        gpio_set_level(SD_PIN_NUM_CS, 1);
        vTaskDelay(pdMS_TO_TICKS(100));

        g_card = NULL;

        ret = esp_vfs_fat_sdspi_mount(
            SD_MOUNT_POINT,
            &host,
            &slot_config,
            &mount_config,
            &g_card
        );

        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "SD card mounted successfully");
            sdmmc_card_print_info(stdout, g_card);

            return ESP_OK;
        }

        ESP_LOGW(
            TAG,
            "Mount attempt %d failed: %s (0x%x)",
            attempt,
            esp_err_to_name(ret),
            (unsigned int)ret
        );

        gpio_set_level(SD_PIN_NUM_CS, 1);

        vTaskDelay(
            pdMS_TO_TICKS(SD_MOUNT_RETRY_DELAY_MS)
        );
    }

    return ret;
}

/* -------------------------------------------------------------------------- */
/*                           RGB565 byte swapping                             */
/* -------------------------------------------------------------------------- */

static void rgb565_swap_bytes(
    uint16_t *buffer,
    size_t pixel_count
)
{
#if RGB565_SWAP_BYTES

    for (size_t i = 0; i < pixel_count; i++) {
        uint16_t pixel = buffer[i];

        buffer[i] = (uint16_t)(
            (pixel << 8) |
            (pixel >> 8)
        );
    }

#else

    (void)buffer;
    (void)pixel_count;

#endif
}

/* -------------------------------------------------------------------------- */
/*                       Draw full-screen background                          */
/* -------------------------------------------------------------------------- */

static esp_err_t draw_rgb565_background_from_sd(
    esp_lcd_panel_handle_t panel_handle,
    const char *path
)
{
    if (panel_handle == NULL || path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *file = fopen(path, "rb");

    if (file == NULL) {
        ESP_LOGE(TAG, "Failed to open background");
        return ESP_FAIL;
    }

    const size_t pixels_per_chunk =
        LCD_WIDTH * DRAW_CHUNK_LINES;

    const size_t bytes_per_chunk =
        pixels_per_chunk * sizeof(uint16_t);

    uint16_t *line_buffer = heap_caps_malloc(
        bytes_per_chunk,
        MALLOC_CAP_DMA
    );

    if (line_buffer == NULL) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Drawing RGB565 background");

    for (
        int y = 0;
        y < LCD_HEIGHT;
        y += DRAW_CHUNK_LINES
    ) {
        int lines_now = DRAW_CHUNK_LINES;

        if ((y + lines_now) > LCD_HEIGHT) {
            lines_now = LCD_HEIGHT - y;
        }

        const size_t pixel_count =
            LCD_WIDTH * lines_now;

        const size_t bytes_to_read =
            pixel_count * sizeof(uint16_t);

        const size_t bytes_read = fread(
            line_buffer,
            1,
            bytes_to_read,
            file
        );

        if (bytes_read != bytes_to_read) {
            ESP_LOGE(TAG, "Background read failed at y=%d", y);

            free(line_buffer);
            fclose(file);

            return ESP_FAIL;
        }

        rgb565_swap_bytes(
            line_buffer,
            pixel_count
        );

        esp_err_t ret = esp_lcd_panel_draw_bitmap(
            panel_handle,
            0,
            y,
            LCD_WIDTH,
            y + lines_now,
            line_buffer
        );

        if (ret != ESP_OK) {
            free(line_buffer);
            fclose(file);

            return ret;
        }
    }

    free(line_buffer);
    fclose(file);

    ESP_LOGI(TAG, "Background displayed");

    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/*                           RGB565 alpha blending                            */
/* -------------------------------------------------------------------------- */

static uint16_t blend_rgb565(
    uint16_t background,
    uint16_t foreground,
    uint8_t alpha
)
{
    if (alpha == 0) {
        return background;
    }

    if (alpha == 255) {
        return foreground;
    }

    const uint32_t inverse_alpha =
        255U - alpha;

    const uint32_t bg_r =
        (background >> 11) & 0x1F;

    const uint32_t bg_g =
        (background >> 5) & 0x3F;

    const uint32_t bg_b =
        background & 0x1F;

    const uint32_t fg_r =
        (foreground >> 11) & 0x1F;

    const uint32_t fg_g =
        (foreground >> 5) & 0x3F;

    const uint32_t fg_b =
        foreground & 0x1F;

    const uint32_t out_r =
        (
            (fg_r * alpha) +
            (bg_r * inverse_alpha) +
            127U
        ) / 255U;

    const uint32_t out_g =
        (
            (fg_g * alpha) +
            (bg_g * inverse_alpha) +
            127U
        ) / 255U;

    const uint32_t out_b =
        (
            (fg_b * alpha) +
            (bg_b * inverse_alpha) +
            127U
        ) / 255U;

    return (uint16_t)(
        (out_r << 11) |
        (out_g << 5) |
        out_b
    );
}

/* -------------------------------------------------------------------------- */
/*                    Draw a generic RGB565A8 image                           */
/* -------------------------------------------------------------------------- */

static esp_err_t draw_rgb565a8_image(
    esp_lcd_panel_handle_t panel_handle,
    const char *background_path,
    const uint8_t *image_map,
    uint16_t *draw_buffer,
    int left,
    int top,
    const char *image_name
)
{
    if (
        panel_handle == NULL ||
        background_path == NULL ||
        image_map == NULL ||
        draw_buffer == NULL
    ) {
        return ESP_ERR_INVALID_ARG;
    }

    if (
        left < 0 ||
        top < 0 ||
        (left + ASTRO_WIDTH) > LCD_WIDTH ||
        (top + ASTRO_HEIGHT) > LCD_HEIGHT
    ) {
        ESP_LOGE(
            TAG,
            "%s position outside display: x=%d, y=%d",
            image_name,
            left,
            top
        );

        return ESP_ERR_INVALID_ARG;
    }

    FILE *file = fopen(background_path, "rb");

    if (file == NULL) {
        ESP_LOGE(TAG, "Failed to reopen background");
        return ESP_FAIL;
    }

    /*
     * Read the original background directly beneath the image.
     */
    for (int row = 0; row < ASTRO_HEIGHT; row++) {
        const long file_offset =
            (
                ((top + row) * LCD_WIDTH) +
                left
            ) * sizeof(uint16_t);

        if (fseek(file, file_offset, SEEK_SET) != 0) {
            ESP_LOGE(
                TAG,
                "%s background seek failed at row %d",
                image_name,
                row
            );

            fclose(file);
            return ESP_FAIL;
        }

        const size_t pixels_read = fread(
            &draw_buffer[row * ASTRO_WIDTH],
            sizeof(uint16_t),
            ASTRO_WIDTH,
            file
        );

        if (pixels_read != ASTRO_WIDTH) {
            ESP_LOGE(
                TAG,
                "%s background read failed at row %d",
                image_name,
                row
            );

            fclose(file);
            return ESP_FAIL;
        }
    }

    fclose(file);

    /*
     * RGB565A8 layout:
     *
     * image_map[0 ... 799]    = RGB565 colors
     * image_map[800 ... 1199] = alpha values
     */
    const uint8_t *color_map = image_map;

    const uint8_t *alpha_map =
        image_map +
        (ASTRO_PIXEL_COUNT * sizeof(uint16_t));

    for (int i = 0; i < ASTRO_PIXEL_COUNT; i++) {
        const uint16_t foreground =
            (uint16_t)color_map[(i * 2) + 0] |
            (
                (uint16_t)color_map[(i * 2) + 1]
                << 8
            );

        draw_buffer[i] = blend_rgb565(
            draw_buffer[i],
            foreground,
            alpha_map[i]
        );
    }

    rgb565_swap_bytes(
        draw_buffer,
        ASTRO_PIXEL_COUNT
    );

    esp_err_t ret = esp_lcd_panel_draw_bitmap(
        panel_handle,
        left,
        top,
        left + ASTRO_WIDTH,
        top + ASTRO_HEIGHT,
        draw_buffer
    );

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "%s draw failed: %s",
            image_name,
            esp_err_to_name(ret)
        );

        return ret;
    }

    ESP_LOGI(
        TAG,
        "%s drawn at x=%d, y=%d",
        image_name,
        left,
        top
    );

    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/*                       Calculate position on arc                            */
/* -------------------------------------------------------------------------- */

static esp_err_t draw_astro_at_progress(
    esp_lcd_panel_handle_t panel_handle,
    const uint8_t *image_map,
    uint16_t *draw_buffer,
    float progress,
    const char *image_name
)
{
    if (progress < 0.0f) {
        progress = 0.0f;
    }
    else if (progress > 1.0f) {
        progress = 1.0f;
    }

    const int center_x =
        ASTRO_ARC_X_START +
        (int)(
            (ASTRO_ARC_X_END - ASTRO_ARC_X_START) *
            progress
        );

    const float curve =
        4.0f *
        progress *
        (1.0f - progress);

    const int center_y =
        ASTRO_ARC_Y_BASE -
        (int)(ASTRO_ARC_HEIGHT * curve);

    const int left =
        center_x - (ASTRO_WIDTH / 2);

    const int top =
        center_y - (ASTRO_HEIGHT / 2);

    return draw_rgb565a8_image(
        panel_handle,
        TEST_BG_FILE,
        image_map,
        draw_buffer,
        left,
        top,
        image_name
    );
}

/* -------------------------------------------------------------------------- */
/*                                  app_main                                  */
/* -------------------------------------------------------------------------- */

void app_main(void)
{
    ESP_LOGI(
        TAG,
        "Starting sun and moon transparency test"
    );

    esp_err_t ret = mount_sd_card();

    if (ret != ESP_OK) {
        stop_application(
            "SD card mount failed"
        );
    }

    ret = check_background_file();

    if (ret != ESP_OK) {
        stop_application(
            "Background file validation failed"
        );
    }

    lv_display_t *display = NULL;

    ret = tft_display_init(&display);

    if (ret != ESP_OK || display == NULL) {
        stop_application(
            "TFT initialization failed"
        );
    }

    esp_lcd_panel_handle_t panel_handle =
        (esp_lcd_panel_handle_t)
        lv_display_get_user_data(display);

    if (panel_handle == NULL) {
        stop_application(
            "LCD panel handle is NULL"
        );
    }

    vTaskDelay(pdMS_TO_TICKS(500));

    ret = draw_rgb565_background_from_sd(
        panel_handle,
        TEST_BG_FILE
    );

    if (ret != ESP_OK) {
        stop_application(
            "Background drawing failed"
        );
    }

    /*
     * Sun on the left side of the arc.
     */
    ret = draw_astro_at_progress(
        panel_handle,
        sun_map,
        sun_draw_buffer,
        0.30f,
        "Sun"
    );

    if (ret != ESP_OK) {
        stop_application(
            "Sun drawing failed"
        );
    }

    /*
     * Moon on the right side of the arc.
     */
    ret = draw_astro_at_progress(
        panel_handle,
        moon_map,
        moon_draw_buffer,
        0.70f,
        "Moon"
    );

    if (ret != ESP_OK) {
        stop_application(
            "Moon drawing failed"
        );
    }

    ESP_LOGI(
        TAG,
        "Sun and moon displayed successfully"
    );

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}