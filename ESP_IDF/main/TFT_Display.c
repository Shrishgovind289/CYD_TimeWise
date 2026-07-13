#include "TFT_Display.h"
#include "SDCard.h"

#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/lock.h>
#include <sys/param.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"

#include "lvgl.h"

/*
 * LVGL 9.2 keeps the complete image-decoder descriptor definition here.
 */
#if defined(__has_include)
    #if __has_include("src/draw/lv_image_decoder_private.h")
        #include "src/draw/lv_image_decoder_private.h"
    #elif __has_include("lvgl/src/draw/lv_image_decoder_private.h")
        #include "lvgl/src/draw/lv_image_decoder_private.h"
    #else
        #error "Could not locate lv_image_decoder_private.h"
    #endif
#else
    #include "src/draw/lv_image_decoder_private.h"
#endif

/* Available when LV_USE_FS_STDIO is enabled in LVGL. */
void lv_fs_stdio_init(void);

/* -------------------------------------------------------------------------- */
/*                              LCD configuration                             */
/* -------------------------------------------------------------------------- */

#define LCD_HOST                         SPI2_HOST

#define TFT_LCD_PIXEL_CLOCK_HZ           (40 * 1000 * 1000)

#define TFT_LCD_BK_LIGHT_ON_LEVEL        1
#define TFT_LCD_BK_LIGHT_OFF_LEVEL       0

#define TFT_PIN_NUM_SCLK                 14
#define TFT_PIN_NUM_MOSI                 13
#define TFT_PIN_NUM_MISO                 -1
#define TFT_PIN_NUM_LCD_DC               2
#define TFT_PIN_NUM_LCD_RST              -1
#define TFT_PIN_NUM_LCD_CS               15
#define TFT_PIN_NUM_BK_LIGHT             27

#define TFT_LCD_H_RES                    480U
#define TFT_LCD_V_RES                    320U

#define TFT_LCD_CMD_BITS                 8
#define TFT_LCD_PARAM_BITS               8

#define TFT_LVGL_DRAW_BUF_LINES          20U
#define TFT_LVGL_TICK_PERIOD_MS          2U
#define TFT_LVGL_TASK_MAX_DELAY_MS       500U
#define TFT_LVGL_TASK_MIN_DELAY_MS       (1000U / CONFIG_FREERTOS_HZ)
#define TFT_LVGL_TASK_STACK_SIZE         (16U * 1024U)
#define TFT_LVGL_TASK_PRIORITY           2

/* -------------------------------------------------------------------------- */
/*                          Raw image configuration                           */
/* -------------------------------------------------------------------------- */

#define TFT_BACKGROUND_PREFIX            "S:/BG/"
#define TFT_BACKGROUND_FILE_SIZE         (TFT_LCD_H_RES * TFT_LCD_V_RES * 2U)

#define TFT_ICON_PREFIX                  "S:/ICO/"
#define TFT_ICON_SOURCE_WIDTH            100U
#define TFT_ICON_MAX_HEIGHT              80U
#define TFT_ICON_BOX_SIZE                60U
#define TFT_ICON_RENDER_LIMIT            50U

#define TFT_HOURLY_CARD_COUNT            TFT_FORECAST_CARD_COUNT
#define TFT_HOURLY_CARD_WIDTH            88U
#define TFT_HOURLY_CARD_HEIGHT           100U
#define TFT_HOURLY_ICON_BOX_SIZE         36U
#define TFT_HOURLY_ICON_RENDER_LIMIT     34U

#define TFT_RGB565_BYTES_PER_PIXEL       2U
#define TFT_ARGB8888_BYTES_PER_PIXEL     4U

/*
 * The final LVGL RGB565 draw buffer is byte-swapped in the LCD flush callback.
 *
 * Keep this at 0 while backgrounds display with correct colors.
 * Change it to 1 only if the raw source files have reversed RGB565 bytes.
 */
#define TFT_RAW_FILE_SWAP_BYTES          0

/*
 * Black-key transparency for icons.
 *
 * Pure and near-black pixels become transparent. Pixels in the fade range
 * receive partial alpha to reduce the dark outline left by antialiasing
 * against the original black background.
 */
#define TFT_ICON_TRANSPARENT_LEVEL       8U
#define TFT_ICON_OPAQUE_LEVEL            48U

#define TFT_ACTIVE_PATH_LENGTH           96U

#define TFT_WIND_ARROW_BOX_SIZE          34U
#define TFT_WIND_ARROW_LINE_WIDTH        5U
#define TFT_WIND_ARROW_HALF_LENGTH       12.0f
#define TFT_WIND_ARROW_HEAD_LENGTH       7.0f
#define TFT_WIND_ARROW_HEAD_HALF_WIDTH   5.0f
#define TFT_PI_F                         3.14159265358979323846f

/* -------------------------------------------------------------------------- */
/*                               Module state                                 */
/* -------------------------------------------------------------------------- */

static const char *TAG = "TFT_Display";

static _lock_t s_lvgl_api_lock;

static lv_display_t *s_display = NULL;

static esp_lcd_panel_handle_t s_panel_handle = NULL;
static esp_lcd_panel_io_handle_t s_io_handle = NULL;
static esp_timer_handle_t s_lvgl_tick_timer = NULL;

static lv_image_decoder_t *s_background_decoder = NULL;

/* Dashboard objects stay private to this module. */
static lv_obj_t *s_background_image = NULL;
static lv_obj_t *s_time_label = NULL;
static lv_obj_t *s_location_label = NULL;
static lv_obj_t *s_weather_row = NULL;
static lv_obj_t *s_temperature_label = NULL;
static lv_obj_t *s_weather_icon_box = NULL;
static lv_obj_t *s_weather_icon = NULL;
static lv_obj_t *s_condition_label = NULL;
static lv_obj_t *s_wind_row = NULL;
static lv_obj_t *s_wind_speed_label = NULL;
static lv_obj_t *s_wind_arrow = NULL;
static lv_point_precise_t s_wind_arrow_points[5] = {0};

static lv_obj_t *s_hourly_container = NULL;
static lv_obj_t *s_hourly_cards[TFT_HOURLY_CARD_COUNT] = {0};
static lv_obj_t *s_hourly_time_labels[TFT_HOURLY_CARD_COUNT] = {0};
static lv_obj_t *s_hourly_icon_boxes[TFT_HOURLY_CARD_COUNT] = {0};
static lv_obj_t *s_hourly_icons[TFT_HOURLY_CARD_COUNT] = {0};
static lv_obj_t *s_hourly_temp_labels[TFT_HOURLY_CARD_COUNT] = {0};
static lv_obj_t *s_hourly_condition_labels[TFT_HOURLY_CARD_COUNT] = {0};

static bool s_dashboard_created = false;

static char s_active_background_path[TFT_ACTIVE_PATH_LENGTH] = "";
static char s_active_icon_path[TFT_ACTIVE_PATH_LENGTH] = "";

/*
 * Alternate between two persistent image descriptors.
 *
 * The currently displayed descriptor and its pixel buffer remain alive while
 * the inactive slot is prepared for the next icon.
 */
static lv_image_dsc_t s_icon_descriptors[2];
static uint8_t *s_icon_pixel_buffers[2] = {NULL, NULL};
static uint8_t s_active_icon_slot = 0U;

static lv_image_dsc_t
    s_hourly_icon_descriptors[TFT_HOURLY_CARD_COUNT];

static uint8_t
    *s_hourly_icon_pixel_buffers[TFT_HOURLY_CARD_COUNT] = {0};

static char
    s_active_hourly_icon_paths[TFT_HOURLY_CARD_COUNT]
                              [TFT_ACTIVE_PATH_LENGTH] = {{0}};

/* -------------------------------------------------------------------------- */
/*                    Streamed background decoder state                       */
/* -------------------------------------------------------------------------- */

typedef struct
{
    lv_fs_file_t file;
    bool file_open;

    lv_draw_buf_t *row_buffer;

} tft_background_decoder_context_t;

/* -------------------------------------------------------------------------- */
/*                            General helpers                                 */
/* -------------------------------------------------------------------------- */

static bool tft_path_has_prefix(
    const char *path,
    const char *prefix
)
{
    if (path == NULL ||
        prefix == NULL)
    {
        return false;
    }

    return strncasecmp(
               path,
               prefix,
               strlen(prefix)) == 0;
}

static bool tft_background_path_supported(
    const char *path
)
{
    if (!tft_path_has_prefix(
            path,
            TFT_BACKGROUND_PREFIX))
    {
        return false;
    }

    const char *extension =
        strrchr(
            path,
            '.'
        );

    return extension != NULL &&
           strcasecmp(
               extension,
               ".BIN") == 0;
}

static bool tft_icon_path_supported(
    const char *path
)
{
    if (!tft_path_has_prefix(
            path,
            TFT_ICON_PREFIX))
    {
        return false;
    }

    const char *extension =
        strrchr(
            path,
            '.'
        );

    return extension != NULL &&
           strcasecmp(
               extension,
               ".BIN") == 0;
}

static void tft_swap_rgb565_bytes(
    uint8_t *data,
    uint32_t data_size
)
{
#if TFT_RAW_FILE_SWAP_BYTES
    if (data == NULL)
    {
        return;
    }

    for (uint32_t index = 0U;
         index + 1U < data_size;
         index += 2U)
    {
        uint8_t temporary =
            data[index];

        data[index] =
            data[index + 1U];

        data[index + 1U] =
            temporary;
    }
#else
    (void)data;
    (void)data_size;
#endif
}

static uint16_t tft_read_rgb565_pixel(
    const uint8_t *pixel_bytes
)
{
#if TFT_RAW_FILE_SWAP_BYTES
    return
        ((uint16_t)pixel_bytes[0] << 8) |
        (uint16_t)pixel_bytes[1];
#else
    return
        (uint16_t)pixel_bytes[0] |
        ((uint16_t)pixel_bytes[1] << 8);
#endif
}

static uint8_t tft_max_u8(
    uint8_t value_1,
    uint8_t value_2,
    uint8_t value_3
)
{
    uint8_t maximum =
        value_1 > value_2
            ? value_1
            : value_2;

    return maximum > value_3
        ? maximum
        : value_3;
}

static uint8_t tft_unpremultiply_channel(
    uint8_t channel,
    uint8_t alpha
)
{
    if (alpha == 0U ||
        alpha == 255U)
    {
        return channel;
    }

    uint32_t restored =
        ((uint32_t)channel * 255U) /
        alpha;

    return restored > 255U
        ? 255U
        : (uint8_t)restored;
}

/* -------------------------------------------------------------------------- */
/*                      Background decoder helpers                            */
/* -------------------------------------------------------------------------- */

static bool tft_lvfs_get_file_size(
    const char *path,
    uint32_t *file_size
)
{
    if (path == NULL ||
        file_size == NULL)
    {
        return false;
    }

    lv_fs_file_t file;

    if (lv_fs_open(
            &file,
            path,
            LV_FS_MODE_RD) != LV_FS_RES_OK)
    {
        return false;
    }

    bool success = false;

    if (lv_fs_seek(
            &file,
            0,
            LV_FS_SEEK_END) == LV_FS_RES_OK &&
        lv_fs_tell(
            &file,
            file_size) == LV_FS_RES_OK)
    {
        success = true;
    }

    lv_fs_close(
        &file
    );

    return success;
}

/* -------------------------------------------------------------------------- */
/*                       Background decoder callbacks                         */
/* -------------------------------------------------------------------------- */

static lv_result_t tft_background_decoder_info(
    lv_image_decoder_t *decoder,
    lv_image_decoder_dsc_t *descriptor,
    lv_image_header_t *header
)
{
    (void)decoder;

    if (descriptor == NULL ||
        header == NULL ||
        descriptor->src_type != LV_IMAGE_SRC_FILE)
    {
        return LV_RESULT_INVALID;
    }

    const char *path =
        (const char *)descriptor->src;

    if (!tft_background_path_supported(
            path))
    {
        return LV_RESULT_INVALID;
    }

    uint32_t file_size = 0U;

    if (!tft_lvfs_get_file_size(
            path,
            &file_size))
    {
        ESP_LOGE(
            TAG,
            "Could not inspect background: %s",
            path
        );

        return LV_RESULT_INVALID;
    }

    if (file_size !=
        TFT_BACKGROUND_FILE_SIZE)
    {
        ESP_LOGE(
            TAG,
            "Invalid background size: %s, received=%u expected=%u",
            path,
            (unsigned int)file_size,
            (unsigned int)TFT_BACKGROUND_FILE_SIZE
        );

        return LV_RESULT_INVALID;
    }

    memset(
        header,
        0,
        sizeof(*header)
    );

    header->magic =
        LV_IMAGE_HEADER_MAGIC;

    header->cf =
        LV_COLOR_FORMAT_RGB565;

    header->flags =
        0;

    header->w =
        TFT_LCD_H_RES;

    header->h =
        TFT_LCD_V_RES;

    header->stride =
        TFT_LCD_H_RES *
        TFT_RGB565_BYTES_PER_PIXEL;

    return LV_RESULT_OK;
}

static lv_result_t tft_background_decoder_open(
    lv_image_decoder_t *decoder,
    lv_image_decoder_dsc_t *descriptor
)
{
    (void)decoder;

    if (descriptor == NULL ||
        descriptor->src_type != LV_IMAGE_SRC_FILE)
    {
        return LV_RESULT_INVALID;
    }

    const char *path =
        (const char *)descriptor->src;

    if (!tft_background_path_supported(
            path))
    {
        return LV_RESULT_INVALID;
    }

    tft_background_decoder_context_t *context =
        lv_malloc(
            sizeof(*context)
        );

    if (context == NULL)
    {
        ESP_LOGE(
            TAG,
            "Could not allocate background decoder context"
        );

        return LV_RESULT_INVALID;
    }

    memset(
        context,
        0,
        sizeof(*context)
    );

    if (lv_fs_open(
            &context->file,
            path,
            LV_FS_MODE_RD) != LV_FS_RES_OK)
    {
        ESP_LOGE(
            TAG,
            "Could not open background: %s",
            path
        );

        lv_free(
            context
        );

        return LV_RESULT_INVALID;
    }

    context->file_open =
        true;

    uint32_t file_size = 0U;

    if (lv_fs_seek(
            &context->file,
            0,
            LV_FS_SEEK_END) != LV_FS_RES_OK ||
        lv_fs_tell(
            &context->file,
            &file_size) != LV_FS_RES_OK ||
        file_size != TFT_BACKGROUND_FILE_SIZE ||
        lv_fs_seek(
            &context->file,
            0,
            LV_FS_SEEK_SET) != LV_FS_RES_OK)
    {
        ESP_LOGE(
            TAG,
            "Background validation failed: %s",
            path
        );

        lv_fs_close(
            &context->file
        );

        lv_free(
            context
        );

        return LV_RESULT_INVALID;
    }

    context->row_buffer =
        lv_draw_buf_create(
            TFT_LCD_H_RES,
            1,
            LV_COLOR_FORMAT_RGB565,
            LV_STRIDE_AUTO
        );

    if (context->row_buffer == NULL)
    {
        ESP_LOGE(
            TAG,
            "Could not allocate background row buffer"
        );

        lv_fs_close(
            &context->file
        );

        lv_free(
            context
        );

        return LV_RESULT_INVALID;
    }

    descriptor->user_data =
        context;

    /*
     * A NULL decoded pointer tells LVGL to call get_area() repeatedly.
     */
    descriptor->decoded =
        NULL;

    ESP_LOGI(
        TAG,
        "Opened streamed background: %s (480x320)",
        path
    );

    return LV_RESULT_OK;
}

static lv_result_t tft_background_decoder_get_area(
    lv_image_decoder_t *decoder,
    lv_image_decoder_dsc_t *descriptor,
    const lv_area_t *full_area,
    lv_area_t *decoded_area
)
{
    (void)decoder;

    if (descriptor == NULL ||
        full_area == NULL ||
        decoded_area == NULL)
    {
        return LV_RESULT_INVALID;
    }

    tft_background_decoder_context_t *context =
        descriptor->user_data;

    if (context == NULL ||
        !context->file_open ||
        context->row_buffer == NULL)
    {
        return LV_RESULT_INVALID;
    }

    int32_t requested_width =
        lv_area_get_width(
            full_area
        );

    if (requested_width <= 0 ||
        requested_width > (int32_t)TFT_LCD_H_RES)
    {
        return LV_RESULT_INVALID;
    }

    if (decoded_area->y1 == LV_COORD_MIN)
    {
        lv_draw_buf_t *reshaped_buffer =
            lv_draw_buf_reshape(
                context->row_buffer,
                LV_COLOR_FORMAT_RGB565,
                (uint32_t)requested_width,
                1,
                LV_STRIDE_AUTO
            );

        if (reshaped_buffer == NULL)
        {
            return LV_RESULT_INVALID;
        }

        context->row_buffer =
            reshaped_buffer;

        *decoded_area =
            *full_area;

        decoded_area->y2 =
            decoded_area->y1;
    }
    else
    {
        decoded_area->y1++;
        decoded_area->y2++;
    }

    /*
     * LV_RESULT_INVALID after the final row means that decoding is complete.
     */
    if (decoded_area->y1 >
        full_area->y2)
    {
        return LV_RESULT_INVALID;
    }

    if (decoded_area->x1 < 0 ||
        decoded_area->y1 < 0 ||
        decoded_area->x2 >= (int32_t)TFT_LCD_H_RES ||
        decoded_area->y2 >= (int32_t)TFT_LCD_V_RES)
    {
        return LV_RESULT_INVALID;
    }

    uint32_t file_offset =
        ((uint32_t)decoded_area->y1 *
         TFT_LCD_H_RES *
         TFT_RGB565_BYTES_PER_PIXEL) +

        ((uint32_t)decoded_area->x1 *
         TFT_RGB565_BYTES_PER_PIXEL);

    if (lv_fs_seek(
            &context->file,
            file_offset,
            LV_FS_SEEK_SET) != LV_FS_RES_OK)
    {
        return LV_RESULT_INVALID;
    }

    uint32_t bytes_to_read =
        (uint32_t)requested_width *
        TFT_RGB565_BYTES_PER_PIXEL;

    uint32_t bytes_read = 0U;

    if (lv_fs_read(
            &context->file,
            context->row_buffer->data,
            bytes_to_read,
            &bytes_read) != LV_FS_RES_OK ||
        bytes_read != bytes_to_read)
    {
        return LV_RESULT_INVALID;
    }

    tft_swap_rgb565_bytes(
        context->row_buffer->data,
        bytes_to_read
    );

    descriptor->decoded =
        context->row_buffer;

    return LV_RESULT_OK;
}

static void tft_background_decoder_close(
    lv_image_decoder_t *decoder,
    lv_image_decoder_dsc_t *descriptor
)
{
    (void)decoder;

    if (descriptor == NULL)
    {
        return;
    }

    tft_background_decoder_context_t *context =
        descriptor->user_data;

    if (context == NULL)
    {
        return;
    }

    if (context->row_buffer != NULL)
    {
        lv_draw_buf_destroy(
            context->row_buffer
        );

        context->row_buffer =
            NULL;
    }

    if (context->file_open)
    {
        lv_fs_close(
            &context->file
        );

        context->file_open =
            false;
    }

    lv_free(
        context
    );

    descriptor->user_data =
        NULL;

    descriptor->decoded =
        NULL;
}

static esp_err_t tft_register_background_decoder(void)
{
    if (s_background_decoder != NULL)
    {
        return ESP_OK;
    }

    s_background_decoder =
        lv_image_decoder_create();

    if (s_background_decoder == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    lv_image_decoder_set_info_cb(
        s_background_decoder,
        tft_background_decoder_info
    );

    lv_image_decoder_set_open_cb(
        s_background_decoder,
        tft_background_decoder_open
    );

    lv_image_decoder_set_get_area_cb(
        s_background_decoder,
        tft_background_decoder_get_area
    );

    lv_image_decoder_set_close_cb(
        s_background_decoder,
        tft_background_decoder_close
    );

    s_background_decoder->name =
        "TFT_BACKGROUND_RGB565";

    ESP_LOGI(
        TAG,
        "Raw RGB565 background decoder registered for S:/BG"
    );

    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/*                             Icon conversion                                */
/* -------------------------------------------------------------------------- */

static bool tft_icon_make_stdio_path(
    const char *lvgl_path,
    char *stdio_path,
    size_t stdio_path_size
)
{
    if (!tft_icon_path_supported(
            lvgl_path) ||
        stdio_path == NULL ||
        stdio_path_size == 0U)
    {
        return false;
    }

    return sdcard_make_path(
               lvgl_path + 2,
               stdio_path,
               stdio_path_size) == ESP_OK;
}

static bool tft_icon_load_transparent(
    const char *lvgl_path,
    uint32_t render_limit,
    uint8_t **pixel_data_out,
    uint32_t *data_size_out,
    uint32_t *width_out,
    uint32_t *height_out
)
{
    if (pixel_data_out != NULL)
    {
        *pixel_data_out = NULL;
    }

    if (data_size_out != NULL)
    {
        *data_size_out = 0U;
    }

    if (width_out != NULL)
    {
        *width_out = 0U;
    }

    if (height_out != NULL)
    {
        *height_out = 0U;
    }

    if (lvgl_path == NULL ||
        render_limit == 0U ||
        pixel_data_out == NULL ||
        data_size_out == NULL ||
        width_out == NULL ||
        height_out == NULL)
    {
        return false;
    }

    char stdio_path[160];

    if (!tft_icon_make_stdio_path(
            lvgl_path,
            stdio_path,
            sizeof(stdio_path)))
    {
        ESP_LOGE(TAG, "Invalid icon path: %s", lvgl_path);
        return false;
    }

    FILE *file = fopen(stdio_path, "rb");

    if (file == NULL)
    {
        ESP_LOGE(TAG, "Could not open icon: %s", stdio_path);
        return false;
    }

    if (fseek(file, 0, SEEK_END) != 0)
    {
        fclose(file);
        return false;
    }

    long file_size_long = ftell(file);

    if (file_size_long <= 0 ||
        file_size_long > UINT32_MAX)
    {
        fclose(file);
        return false;
    }

    if (fseek(file, 0, SEEK_SET) != 0)
    {
        fclose(file);
        return false;
    }

    uint32_t source_data_size =
        (uint32_t)file_size_long;

    uint32_t source_stride =
        TFT_ICON_SOURCE_WIDTH *
        TFT_RGB565_BYTES_PER_PIXEL;

    if ((source_data_size % source_stride) != 0U)
    {
        ESP_LOGE(
            TAG,
            "Invalid icon dimensions: %s, size=%u is not divisible by %u",
            stdio_path,
            (unsigned int)source_data_size,
            (unsigned int)source_stride
        );

        fclose(file);
        return false;
    }

    uint32_t source_width = TFT_ICON_SOURCE_WIDTH;
    uint32_t source_height = source_data_size / source_stride;

    if (source_height == 0U ||
        source_height > TFT_ICON_MAX_HEIGHT)
    {
        ESP_LOGE(
            TAG,
            "Invalid icon height: %s, calculated=%u",
            stdio_path,
            (unsigned int)source_height
        );

        fclose(file);
        return false;
    }

    uint8_t *source_pixels = malloc(source_data_size);

    if (source_pixels == NULL)
    {
        ESP_LOGE(
            TAG,
            "Could not allocate %u bytes for icon source",
            (unsigned int)source_data_size
        );

        fclose(file);
        return false;
    }

    size_t bytes_read =
        fread(
            source_pixels,
            1,
            source_data_size,
            file
        );

    fclose(file);

    if (bytes_read != source_data_size)
    {
        ESP_LOGE(
            TAG,
            "Incomplete icon read: %s, received=%u expected=%u",
            stdio_path,
            (unsigned int)bytes_read,
            (unsigned int)source_data_size
        );

        free(source_pixels);
        return false;
    }

    uint32_t largest_dimension =
        source_width > source_height
            ? source_width
            : source_height;

    uint32_t icon_width = source_width;
    uint32_t icon_height = source_height;

    if (largest_dimension > render_limit)
    {
        icon_width =
            (source_width * render_limit) /
            largest_dimension;

        icon_height =
            (source_height * render_limit) /
            largest_dimension;

        if (icon_width == 0U)
        {
            icon_width = 1U;
        }

        if (icon_height == 0U)
        {
            icon_height = 1U;
        }
    }

    uint32_t pixel_count = icon_width * icon_height;
    uint32_t converted_data_size =
        pixel_count * sizeof(lv_color32_t);

    lv_color32_t *converted_pixels =
        malloc(converted_data_size);

    if (converted_pixels == NULL)
    {
        ESP_LOGE(
            TAG,
            "Could not allocate %u bytes for transparent icon",
            (unsigned int)converted_data_size
        );

        free(source_pixels);
        return false;
    }

    for (uint32_t output_y = 0U;
         output_y < icon_height;
         output_y++)
    {
        uint32_t source_y =
            (output_y * source_height) /
            icon_height;

        for (uint32_t output_x = 0U;
             output_x < icon_width;
             output_x++)
        {
            uint32_t source_x =
                (output_x * source_width) /
                icon_width;

            uint32_t source_pixel_index =
                (source_y * source_width) +
                source_x;

            uint32_t source_byte_index =
                source_pixel_index *
                TFT_RGB565_BYTES_PER_PIXEL;

            uint16_t rgb565 =
                tft_read_rgb565_pixel(
                    &source_pixels[source_byte_index]
                );

            uint8_t red =
                (uint8_t)(
                    (((rgb565 >> 11) & 0x1FU) * 255U) /
                    31U
                );

            uint8_t green =
                (uint8_t)(
                    (((rgb565 >> 5) & 0x3FU) * 255U) /
                    63U
                );

            uint8_t blue =
                (uint8_t)(
                    ((rgb565 & 0x1FU) * 255U) /
                    31U
                );

            uint8_t intensity =
                tft_max_u8(red, green, blue);

            uint8_t alpha;

            if (intensity <= TFT_ICON_TRANSPARENT_LEVEL)
            {
                alpha = 0U;
            }
            else if (intensity >= TFT_ICON_OPAQUE_LEVEL)
            {
                alpha = 255U;
            }
            else
            {
                alpha =
                    (uint8_t)(
                        ((uint32_t)(
                            intensity -
                            TFT_ICON_TRANSPARENT_LEVEL
                        ) * 255U) /
                        (
                            TFT_ICON_OPAQUE_LEVEL -
                            TFT_ICON_TRANSPARENT_LEVEL
                        )
                    );

                red =
                    tft_unpremultiply_channel(
                        red,
                        alpha
                    );

                green =
                    tft_unpremultiply_channel(
                        green,
                        alpha
                    );

                blue =
                    tft_unpremultiply_channel(
                        blue,
                        alpha
                    );
            }

            uint32_t output_pixel_index =
                (output_y * icon_width) +
                output_x;

            converted_pixels[output_pixel_index].red = red;
            converted_pixels[output_pixel_index].green = green;
            converted_pixels[output_pixel_index].blue = blue;
            converted_pixels[output_pixel_index].alpha = alpha;
        }
    }

    free(source_pixels);

    *pixel_data_out = (uint8_t *)converted_pixels;
    *data_size_out = converted_data_size;
    *width_out = icon_width;
    *height_out = icon_height;

    ESP_LOGI(
        TAG,
        "Transparent icon loaded: %s (%ux%u, %u bytes)",
        stdio_path,
        (unsigned int)icon_width,
        (unsigned int)icon_height,
        (unsigned int)converted_data_size
    );

    return true;
}

/* -------------------------------------------------------------------------- */
/*                           Display callbacks                                */
/* -------------------------------------------------------------------------- */

static bool tft_display_notify_lvgl_flush_ready(
    esp_lcd_panel_io_handle_t panel_io,
    esp_lcd_panel_io_event_data_t *event_data,
    void *user_context
)
{
    (void)panel_io;
    (void)event_data;

    lv_display_flush_ready(
        (lv_display_t *)user_context
    );

    return false;
}

static void tft_display_flush_callback(
    lv_display_t *display,
    const lv_area_t *area,
    uint8_t *pixel_map
)
{
    esp_lcd_panel_handle_t panel_handle =
        lv_display_get_user_data(
            display
        );

    int32_t x1 =
        area->x1;

    int32_t x2 =
        area->x2;

    int32_t y1 =
        area->y1;

    int32_t y2 =
        area->y2;

    lv_draw_sw_rgb565_swap(
        pixel_map,
        (uint32_t)(
            (x2 + 1 - x1) *
            (y2 + 1 - y1)
        )
    );

    esp_lcd_panel_draw_bitmap(
        panel_handle,
        x1,
        y1,
        x2 + 1,
        y2 + 1,
        pixel_map
    );
}

static void tft_display_lvgl_task(
    void *argument
)
{
    (void)argument;

    ESP_LOGI(
        TAG,
        "LVGL task started"
    );

    while (true)
    {
        _lock_acquire(
            &s_lvgl_api_lock
        );

        uint32_t delay_ms =
            lv_timer_handler();

        _lock_release(
            &s_lvgl_api_lock
        );

        delay_ms =
            MAX(
                delay_ms,
                TFT_LVGL_TASK_MIN_DELAY_MS
            );

        delay_ms =
            MIN(
                delay_ms,
                TFT_LVGL_TASK_MAX_DELAY_MS
            );

        usleep(
            delay_ms *
            1000U
        );
    }
}

static void tft_display_tick_callback(
    void *argument
)
{
    (void)argument;

    lv_tick_inc(
        TFT_LVGL_TICK_PERIOD_MS
    );
}

/* -------------------------------------------------------------------------- */
/*                           Dashboard helpers                                */
/* -------------------------------------------------------------------------- */

static bool tft_dashboard_background_ready(
    const char *path
)
{
    if (!tft_background_path_supported(
            path))
    {
        return false;
    }

    lv_image_header_t header;

    if (lv_image_decoder_get_info(
            path,
            &header) != LV_RESULT_OK)
    {
        ESP_LOGE(
            TAG,
            "Background decoder rejected: %s",
            path
        );

        return false;
    }

    return header.w == TFT_LCD_H_RES &&
           header.h == TFT_LCD_V_RES &&
           header.cf == LV_COLOR_FORMAT_RGB565;
}

static void tft_dashboard_update_wind_arrow_locked(int wind_direction_degrees)
{
    if (s_wind_arrow == NULL)
    {
        return;
    }

    while (wind_direction_degrees < 0)
    {
        wind_direction_degrees += 360;
    }

    wind_direction_degrees %= 360;

    float angle_radians = ((float)wind_direction_degrees * TFT_PI_F) / 180.0f;
    float direction_x = sinf(angle_radians);
    float direction_y = -cosf(angle_radians);
    float perpendicular_x = -direction_y;
    float perpendicular_y = direction_x;
    float center = (float)(TFT_WIND_ARROW_BOX_SIZE - 1U) * 0.5f;

    float tail_x = center - (direction_x * TFT_WIND_ARROW_HALF_LENGTH);
    float tail_y = center - (direction_y * TFT_WIND_ARROW_HALF_LENGTH);
    float head_x = center + (direction_x * TFT_WIND_ARROW_HALF_LENGTH);
    float head_y = center + (direction_y * TFT_WIND_ARROW_HALF_LENGTH);
    float wing_base_x = head_x - (direction_x * TFT_WIND_ARROW_HEAD_LENGTH);
    float wing_base_y = head_y - (direction_y * TFT_WIND_ARROW_HEAD_LENGTH);

    s_wind_arrow_points[0].x = (lv_value_precise_t)tail_x;
    s_wind_arrow_points[0].y = (lv_value_precise_t)tail_y;
    s_wind_arrow_points[1].x = (lv_value_precise_t)head_x;
    s_wind_arrow_points[1].y = (lv_value_precise_t)head_y;
    s_wind_arrow_points[2].x = (lv_value_precise_t)(wing_base_x + (perpendicular_x * TFT_WIND_ARROW_HEAD_HALF_WIDTH));
    s_wind_arrow_points[2].y = (lv_value_precise_t)(wing_base_y + (perpendicular_y * TFT_WIND_ARROW_HEAD_HALF_WIDTH));
    s_wind_arrow_points[3].x = (lv_value_precise_t)head_x;
    s_wind_arrow_points[3].y = (lv_value_precise_t)head_y;
    s_wind_arrow_points[4].x = (lv_value_precise_t)(wing_base_x - (perpendicular_x * TFT_WIND_ARROW_HEAD_HALF_WIDTH));
    s_wind_arrow_points[4].y = (lv_value_precise_t)(wing_base_y - (perpendicular_y * TFT_WIND_ARROW_HEAD_HALF_WIDTH));

    lv_line_set_points(s_wind_arrow, s_wind_arrow_points, 5U);
    lv_obj_invalidate(s_wind_arrow);
}

static uint32_t tft_dashboard_icon_scale(
    uint32_t width,
    uint32_t height
)
{
    uint32_t largest_dimension =
        width > height
            ? width
            : height;

    if (largest_dimension == 0U ||
        largest_dimension <= TFT_ICON_RENDER_LIMIT)
    {
        return 256U;
    }

    uint32_t scale =
        (TFT_ICON_RENDER_LIMIT *
         256U) /
        largest_dimension;

    return scale == 0U
        ? 1U
        : scale;
}

/* -------------------------------------------------------------------------- */
/*                              Public API                                    */
/* -------------------------------------------------------------------------- */

esp_err_t tft_display_init(void)
{
    if (s_display != NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    gpio_config_t backlight_configuration =
    {
        .mode =
            GPIO_MODE_OUTPUT,

        .pin_bit_mask =
            1ULL <<
            TFT_PIN_NUM_BK_LIGHT
    };

    ESP_ERROR_CHECK(
        gpio_config(
            &backlight_configuration
        )
    );

    gpio_set_level(
        TFT_PIN_NUM_BK_LIGHT,
        TFT_LCD_BK_LIGHT_OFF_LEVEL
    );

    spi_bus_config_t bus_configuration =
    {
        .sclk_io_num =
            TFT_PIN_NUM_SCLK,

        .mosi_io_num =
            TFT_PIN_NUM_MOSI,

        .miso_io_num =
            TFT_PIN_NUM_MISO,

        .quadwp_io_num =
            -1,

        .quadhd_io_num =
            -1,

        .max_transfer_sz =
            TFT_LCD_H_RES *
            80U *
            sizeof(uint16_t)
    };

    ESP_ERROR_CHECK(
        spi_bus_initialize(
            LCD_HOST,
            &bus_configuration,
            SPI_DMA_CH_AUTO
        )
    );

    esp_lcd_panel_io_spi_config_t io_configuration =
    {
        .dc_gpio_num =
            TFT_PIN_NUM_LCD_DC,

        .cs_gpio_num =
            TFT_PIN_NUM_LCD_CS,

        .pclk_hz =
            TFT_LCD_PIXEL_CLOCK_HZ,

        .lcd_cmd_bits =
            TFT_LCD_CMD_BITS,

        .lcd_param_bits =
            TFT_LCD_PARAM_BITS,

        .spi_mode =
            0,

        .trans_queue_depth =
            10
    };

    ESP_ERROR_CHECK(
        esp_lcd_new_panel_io_spi(
            (esp_lcd_spi_bus_handle_t)LCD_HOST,
            &io_configuration,
            &s_io_handle
        )
    );

    esp_lcd_panel_dev_config_t panel_configuration =
    {
        .reset_gpio_num =
            TFT_PIN_NUM_LCD_RST,

        .rgb_ele_order =
            LCD_RGB_ELEMENT_ORDER_BGR,

        .bits_per_pixel =
            16
    };

    ESP_ERROR_CHECK(
        esp_lcd_new_panel_st7789(
            s_io_handle,
            &panel_configuration,
            &s_panel_handle
        )
    );

    ESP_ERROR_CHECK(
        esp_lcd_panel_reset(
            s_panel_handle
        )
    );

    ESP_ERROR_CHECK(
        esp_lcd_panel_init(
            s_panel_handle
        )
    );

    ESP_ERROR_CHECK(
        esp_lcd_panel_swap_xy(
            s_panel_handle,
            true
        )
    );

    ESP_ERROR_CHECK(
        esp_lcd_panel_mirror(
            s_panel_handle,
            true,
            true
        )
    );

    ESP_ERROR_CHECK(
        esp_lcd_panel_disp_on_off(
            s_panel_handle,
            true
        )
    );

    lv_init();

    lv_fs_stdio_init();

    ESP_ERROR_CHECK(
        tft_register_background_decoder()
    );

    s_display =
        lv_display_create(
            TFT_LCD_H_RES,
            TFT_LCD_V_RES
        );

    if (s_display == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    size_t draw_buffer_size =
        TFT_LCD_H_RES *
        TFT_LVGL_DRAW_BUF_LINES *
        sizeof(uint16_t);

    void *buffer_1 =
        spi_bus_dma_memory_alloc(
            LCD_HOST,
            draw_buffer_size,
            0
        );

    void *buffer_2 =
        spi_bus_dma_memory_alloc(
            LCD_HOST,
            draw_buffer_size,
            0
        );

    if (buffer_1 == NULL ||
        buffer_2 == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    lv_display_set_buffers(
        s_display,
        buffer_1,
        buffer_2,
        draw_buffer_size,
        LV_DISPLAY_RENDER_MODE_PARTIAL
    );

    lv_display_set_user_data(
        s_display,
        s_panel_handle
    );

    lv_display_set_color_format(
        s_display,
        LV_COLOR_FORMAT_RGB565
    );

    lv_display_set_flush_cb(
        s_display,
        tft_display_flush_callback
    );

    const esp_timer_create_args_t tick_timer_arguments =
    {
        .callback =
            tft_display_tick_callback,

        .name =
            "lvgl_tick"
    };

    ESP_ERROR_CHECK(
        esp_timer_create(
            &tick_timer_arguments,
            &s_lvgl_tick_timer
        )
    );

    ESP_ERROR_CHECK(
        esp_timer_start_periodic(
            s_lvgl_tick_timer,
            TFT_LVGL_TICK_PERIOD_MS *
            1000U
        )
    );

    const esp_lcd_panel_io_callbacks_t panel_callbacks =
    {
        .on_color_trans_done =
            tft_display_notify_lvgl_flush_ready
    };

    ESP_ERROR_CHECK(
        esp_lcd_panel_io_register_event_callbacks(
            s_io_handle,
            &panel_callbacks,
            s_display
        )
    );

    BaseType_t task_result =
        xTaskCreate(
            tft_display_lvgl_task,
            "LVGL",
            TFT_LVGL_TASK_STACK_SIZE,
            NULL,
            TFT_LVGL_TASK_PRIORITY,
            NULL
        );

    if (task_result != pdPASS)
    {
        return ESP_ERR_NO_MEM;
    }

    gpio_set_level(
        TFT_PIN_NUM_BK_LIGHT,
        TFT_LCD_BK_LIGHT_ON_LEVEL
    );

    ESP_LOGI(
        TAG,
        "TFT display initialized"
    );

    return ESP_OK;
}

esp_err_t tft_dashboard_create(void)
{
    if (s_display == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_dashboard_created)
    {
        return ESP_OK;
    }

    _lock_acquire(&s_lvgl_api_lock);

    lv_obj_t *screen = lv_display_get_screen_active(s_display);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0B1220), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    // Background
    s_background_image = lv_image_create(screen);
    lv_obj_set_pos(s_background_image, 0, 0);
    lv_obj_clear_flag(s_background_image, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_background_image, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_background(s_background_image);

    //Time
    s_time_label = lv_label_create(screen);
    lv_label_set_text(s_time_label, "--:-- --");
    lv_obj_set_style_text_color(s_time_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(s_time_label, &lv_font_montserrat_48, 0);
    lv_obj_align(s_time_label, LV_ALIGN_TOP_MID, 0, 60);

    /* Location */
    s_location_label = lv_label_create(screen);
    lv_label_set_text(s_location_label, "Loading location...");
    lv_obj_set_width(
        s_location_label,
        440
    );
    lv_label_set_long_mode(
        s_location_label,
        LV_LABEL_LONG_CLIP
    );
    lv_obj_set_style_text_align(
        s_location_label,
        LV_TEXT_ALIGN_CENTER,
        0
    );
    lv_obj_set_style_text_color(s_location_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(
        s_location_label,
        &lv_font_montserrat_18,
        0
    );
    lv_obj_align(
        s_location_label,
        LV_ALIGN_TOP_MID,
        0,
        115
    );

    /* Weather row */
    s_weather_row = lv_obj_create(screen);
    lv_obj_remove_style_all(s_weather_row);
    lv_obj_set_size(s_weather_row, 460, 64);
    lv_obj_set_flex_flow(
        s_weather_row,
        LV_FLEX_FLOW_ROW
    );
    lv_obj_set_flex_align(
        s_weather_row,
        LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_CENTER
    );
    lv_obj_set_style_pad_column(
        s_weather_row,
        8,
        0
    );
    lv_obj_clear_flag(
        s_weather_row,
        LV_OBJ_FLAG_SCROLLABLE
    );
    lv_obj_align(
        s_weather_row,
        LV_ALIGN_TOP_MID,
        55,
        135
    );

    /* Temperature */
    s_temperature_label =
        lv_label_create(
            s_weather_row
        );
    lv_label_set_text(
        s_temperature_label,
        "-- C |"
    );
    lv_obj_set_style_text_color(s_temperature_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(
        s_temperature_label,
        &lv_font_montserrat_24,
        0
    );

    /* Transparent fixed-size icon slot */
    s_weather_icon_box = lv_obj_create(s_weather_row);
    lv_obj_remove_style_all(s_weather_icon_box);
    lv_obj_set_size(
        s_weather_icon_box,
        TFT_ICON_BOX_SIZE,
        TFT_ICON_BOX_SIZE
    );
    lv_obj_set_style_bg_opa(
        s_weather_icon_box,
        LV_OPA_TRANSP,
        0
    );
    lv_obj_set_style_border_width(
        s_weather_icon_box,
        0,
        0
    );
    lv_obj_clear_flag(
        s_weather_icon_box,
        LV_OBJ_FLAG_SCROLLABLE
    );
    s_weather_icon =
        lv_image_create(
            s_weather_icon_box
        );
    lv_obj_clear_flag(
        s_weather_icon,
        LV_OBJ_FLAG_CLICKABLE
    );
    lv_obj_clear_flag(
        s_weather_icon,
        LV_OBJ_FLAG_SCROLLABLE
    );
    lv_obj_add_flag(
        s_weather_icon,
        LV_OBJ_FLAG_HIDDEN
    );
    lv_obj_center(s_weather_icon);

    /* Condition */

    s_condition_label = lv_label_create(s_weather_row);

    lv_label_set_text(
        s_condition_label,
        "Weather unavailable"
    );

    lv_obj_set_width(
        s_condition_label,
        280
    );

    lv_label_set_long_mode(
        s_condition_label,
        LV_LABEL_LONG_WRAP
    );

    lv_obj_set_style_text_align(
        s_condition_label,
        LV_TEXT_ALIGN_LEFT,
        0
    );

    lv_obj_set_style_text_color(s_condition_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(s_condition_label, &lv_font_montserrat_24, 0);

    /* Current wind: speed plus a thick directional arrow. */

    s_wind_row = lv_obj_create(screen);

    lv_obj_remove_style_all(s_wind_row);
    lv_obj_set_size(s_wind_row, 260, TFT_WIND_ARROW_BOX_SIZE);
    lv_obj_set_flex_flow(s_wind_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_wind_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_wind_row, 10, 0);
    lv_obj_clear_flag(s_wind_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_wind_row, LV_ALIGN_TOP_MID, 0, 183);

    s_wind_speed_label = lv_label_create(s_wind_row);

    lv_label_set_text(s_wind_speed_label, "Wind -- km/h");
    lv_obj_set_style_text_color(s_wind_speed_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(s_wind_speed_label, &lv_font_montserrat_18, 0);

    s_wind_arrow = lv_line_create(s_wind_row);

    lv_obj_set_size(s_wind_arrow, TFT_WIND_ARROW_BOX_SIZE, TFT_WIND_ARROW_BOX_SIZE);
    lv_obj_set_style_line_width(s_wind_arrow, TFT_WIND_ARROW_LINE_WIDTH, 0);
    lv_obj_set_style_line_color(s_wind_arrow, lv_color_black(), 0);
    lv_obj_set_style_line_rounded(s_wind_arrow, true, 0);
    lv_obj_clear_flag(s_wind_arrow, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_wind_arrow, LV_OBJ_FLAG_SCROLLABLE);

    tft_dashboard_update_wind_arrow_locked(0);

    /* Five-hour forecast cards */

    s_hourly_container = lv_obj_create(screen);

    lv_obj_remove_style_all(s_hourly_container);

    lv_obj_set_size(
        s_hourly_container,
        468,
        104
    );

    lv_obj_set_flex_flow(
        s_hourly_container,
        LV_FLEX_FLOW_ROW
    );

    lv_obj_set_flex_align(
        s_hourly_container,
        LV_FLEX_ALIGN_SPACE_BETWEEN,
        LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_CENTER
    );

    lv_obj_set_style_pad_all(
        s_hourly_container,
        0,
        0
    );

    lv_obj_set_style_pad_column(
        s_hourly_container,
        5,
        0
    );

    lv_obj_clear_flag(
        s_hourly_container,
        LV_OBJ_FLAG_SCROLLABLE
    );

    lv_obj_set_scrollbar_mode(
        s_hourly_container,
        LV_SCROLLBAR_MODE_OFF
    );

    lv_obj_align(
        s_hourly_container,
        LV_ALIGN_BOTTOM_MID,
        0,
        -2
    );

    lv_obj_add_flag(
        s_hourly_container,
        LV_OBJ_FLAG_HIDDEN
    );

    for (size_t index = 0U; index < TFT_HOURLY_CARD_COUNT; index++)
    {
        s_hourly_cards[index] =
            lv_obj_create(s_hourly_container);

        lv_obj_remove_style_all(
            s_hourly_cards[index]
        );

        lv_obj_set_size(
            s_hourly_cards[index],
            TFT_HOURLY_CARD_WIDTH,
            TFT_HOURLY_CARD_HEIGHT
        );

        lv_obj_set_style_radius(
            s_hourly_cards[index],
            12,
            0
        );

        lv_obj_set_style_bg_color(
            s_hourly_cards[index],
            lv_color_hex(0x0B1220),
            0
        );

        lv_obj_set_style_bg_opa(
            s_hourly_cards[index],
            LV_OPA_70,
            0
        );

        lv_obj_set_style_border_width(
            s_hourly_cards[index],
            1,
            0
        );

        lv_obj_set_style_border_color(
            s_hourly_cards[index],
            lv_color_white(),
            0
        );

        lv_obj_set_style_border_opa(
            s_hourly_cards[index],
            LV_OPA_30,
            0
        );

        lv_obj_clear_flag(
            s_hourly_cards[index],
            LV_OBJ_FLAG_SCROLLABLE
        );

        lv_obj_clear_flag(
            s_hourly_cards[index],
            LV_OBJ_FLAG_CLICKABLE
        );

        s_hourly_time_labels[index] =
            lv_label_create(s_hourly_cards[index]);

        lv_label_set_text(
            s_hourly_time_labels[index],
            "--"
        );

        lv_obj_set_style_text_color(
            s_hourly_time_labels[index],
            lv_color_hex(0xDCE6F7),
            0
        );

        lv_obj_set_style_text_font(
            s_hourly_time_labels[index],
            LV_FONT_DEFAULT,
            0
        );

        lv_obj_align(
            s_hourly_time_labels[index],
            LV_ALIGN_TOP_MID,
            0,
            4
        );

        s_hourly_icon_boxes[index] =
            lv_obj_create(s_hourly_cards[index]);

        lv_obj_remove_style_all(
            s_hourly_icon_boxes[index]
        );

        lv_obj_set_size(
            s_hourly_icon_boxes[index],
            TFT_HOURLY_ICON_BOX_SIZE,
            TFT_HOURLY_ICON_BOX_SIZE
        );

        lv_obj_set_style_bg_opa(
            s_hourly_icon_boxes[index],
            LV_OPA_TRANSP,
            0
        );

        lv_obj_clear_flag(
            s_hourly_icon_boxes[index],
            LV_OBJ_FLAG_SCROLLABLE
        );

        lv_obj_align(
            s_hourly_icon_boxes[index],
            LV_ALIGN_TOP_MID,
            0,
            21
        );

        s_hourly_icons[index] =
            lv_image_create(s_hourly_icon_boxes[index]);

        lv_obj_add_flag(
            s_hourly_icons[index],
            LV_OBJ_FLAG_HIDDEN
        );

        lv_obj_clear_flag(
            s_hourly_icons[index],
            LV_OBJ_FLAG_CLICKABLE
        );

        lv_obj_clear_flag(
            s_hourly_icons[index],
            LV_OBJ_FLAG_SCROLLABLE
        );

        lv_obj_center(s_hourly_icons[index]);

        s_hourly_temp_labels[index] =
            lv_label_create(s_hourly_cards[index]);

        lv_label_set_text(
            s_hourly_temp_labels[index],
            "-- C"
        );

        lv_obj_set_style_text_color(
            s_hourly_temp_labels[index],
            lv_color_white(),
            0
        );

        lv_obj_set_style_text_font(
            s_hourly_temp_labels[index],
            &lv_font_montserrat_18,
            0
        );

        lv_obj_align(
            s_hourly_temp_labels[index],
            LV_ALIGN_TOP_MID,
            0,
            75
        );

    }

    s_dashboard_created = true;

    _lock_release(&s_lvgl_api_lock);

    ESP_LOGI(
        TAG,
        "TimeWise dashboard created"
    );

    return ESP_OK;
}

void tft_dashboard_set_time(const char *time_text)
{
    if (!s_dashboard_created || s_time_label == NULL || time_text == NULL)
    {
        return;
    }

    _lock_acquire(&s_lvgl_api_lock);

    lv_label_set_text(
        s_time_label,
        time_text
    );

    _lock_release(&s_lvgl_api_lock);
}

void tft_dashboard_set_weather(float temperature_c, const char *condition, const char *location, const char *background_path, const char *icon_path)
{
    if (!s_dashboard_created)
    {
        return;
    }

    char temperature_text[24];

    snprintf(
        temperature_text,
        sizeof(temperature_text),
        "%.0f C |",
        temperature_c
    );

    /*
     * WeatherStation is the only caller, so reading these path strings before
     * taking the LVGL lock is safe in this design.
     */
    bool valid_background_path =
        background_path != NULL &&
        background_path[0] != '\0';

    bool background_changed =
        valid_background_path &&
        strcmp(
            s_active_background_path,
            background_path) != 0;

    bool background_ready =
        !background_changed ||
        tft_dashboard_background_ready(
            background_path
        );

    bool valid_icon_path =
        icon_path != NULL &&
        icon_path[0] != '\0';

    bool icon_changed =
        valid_icon_path &&
        strcmp(
            s_active_icon_path,
            icon_path) != 0;

    uint8_t *new_icon_pixels = NULL;
    uint32_t new_icon_data_size = 0U;
    uint32_t new_icon_width = 0U;
    uint32_t new_icon_height = 0U;

    bool icon_loaded = false;

    /*
     * Read and convert the icon before taking the LVGL lock.
     */
    if (icon_changed)
    {
        icon_loaded =
            tft_icon_load_transparent(
                icon_path,
                TFT_ICON_RENDER_LIMIT,
                &new_icon_pixels,
                &new_icon_data_size,
                &new_icon_width,
                &new_icon_height
            );
    }

    _lock_acquire(
        &s_lvgl_api_lock
    );

    /* Text */

    if (s_location_label != NULL &&
        location != NULL)
    {
        lv_label_set_text(
            s_location_label,
            location
        );
    }

    if (s_temperature_label != NULL)
    {
        lv_label_set_text(
            s_temperature_label,
            temperature_text
        );
    }

    if (s_condition_label != NULL)
    {
        lv_label_set_text(
            s_condition_label,
            condition != NULL
                ? condition
                : "Weather unavailable"
        );
    }

    /* Background */

    if (background_changed)
    {
        if (background_ready &&
            s_background_image != NULL)
        {
            lv_image_set_src(
                s_background_image,
                background_path
            );

            lv_obj_move_background(
                s_background_image
            );

            lv_obj_invalidate(
                s_background_image
            );

            snprintf(
                s_active_background_path,
                sizeof(s_active_background_path),
                "%s",
                background_path
            );

            ESP_LOGI(
                TAG,
                "Background changed: %s",
                background_path
            );
        }
        else
        {
            ESP_LOGE(
                TAG,
                "Background unavailable or invalid: %s",
                background_path
            );
        }
    }

    /* Weather icon */

    if (s_weather_icon != NULL)
    {
        if (!valid_icon_path)
        {
            lv_obj_add_flag(
                s_weather_icon,
                LV_OBJ_FLAG_HIDDEN
            );

            s_active_icon_path[0] =
                '\0';
        }
        else if (!icon_changed)
        {
            lv_obj_clear_flag(
                s_weather_icon,
                LV_OBJ_FLAG_HIDDEN
            );
        }
        else if (icon_loaded &&
                 new_icon_pixels != NULL &&
                 new_icon_data_size > 0U &&
                 new_icon_width > 0U &&
                 new_icon_height > 0U)
        {
            uint8_t new_slot =
                (uint8_t)(
                    1U -
                    s_active_icon_slot
                );

            if (s_icon_pixel_buffers[new_slot] != NULL)
            {
                free(
                    s_icon_pixel_buffers[new_slot]
                );

                s_icon_pixel_buffers[new_slot] =
                    NULL;
            }

            s_icon_pixel_buffers[new_slot] =
                new_icon_pixels;

            lv_image_dsc_t *descriptor =
                &s_icon_descriptors[new_slot];

            memset(
                descriptor,
                0,
                sizeof(*descriptor)
            );

            descriptor->header.magic =
                LV_IMAGE_HEADER_MAGIC;

            descriptor->header.cf =
                LV_COLOR_FORMAT_ARGB8888;

            descriptor->header.flags =
                0;

            descriptor->header.w =
                new_icon_width;

            descriptor->header.h =
                new_icon_height;

            descriptor->header.stride =
                new_icon_width *
                TFT_ARGB8888_BYTES_PER_PIXEL;

            descriptor->data_size =
                new_icon_data_size;

            descriptor->data =
                s_icon_pixel_buffers[new_slot];

            lv_image_set_src(
                s_weather_icon,
                descriptor
            );

            lv_obj_set_size(
                s_weather_icon,
                new_icon_width,
                new_icon_height
            );

            lv_image_set_scale(
                s_weather_icon,
                tft_dashboard_icon_scale(
                    new_icon_width,
                    new_icon_height
                )
            );

            lv_obj_clear_flag(
                s_weather_icon,
                LV_OBJ_FLAG_HIDDEN
            );

            lv_obj_center(
                s_weather_icon
            );

            lv_obj_invalidate(
                s_weather_icon
            );

            s_active_icon_slot =
                new_slot;

            snprintf(
                s_active_icon_path,
                sizeof(s_active_icon_path),
                "%s",
                icon_path
            );

            /*
             * Ownership moved into s_icon_pixel_buffers[new_slot].
             */
            new_icon_pixels =
                NULL;

            ESP_LOGI(
                TAG,
                "Weather icon installed: %s (%ux%u, ARGB=%u bytes)",
                icon_path,
                (unsigned int)new_icon_width,
                (unsigned int)new_icon_height,
                (unsigned int)new_icon_data_size
            );
        }
        else
        {
            lv_obj_add_flag(
                s_weather_icon,
                LV_OBJ_FLAG_HIDDEN
            );

            s_active_icon_path[0] =
                '\0';

            ESP_LOGE(
                TAG,
                "Could not load weather icon: %s",
                icon_path
            );
        }

        if (s_weather_icon_box != NULL)
        {
            lv_obj_update_layout(
                s_weather_icon_box
            );
        }

        if (s_weather_row != NULL)
        {
            lv_obj_update_layout(
                s_weather_row
            );
        }
    }

    _lock_release(
        &s_lvgl_api_lock
    );

    /*
     * Free temporary memory only when ownership was not transferred.
     */
    if (new_icon_pixels != NULL)
    {
        free(
            new_icon_pixels
        );
    }
}
void tft_dashboard_set_wind(float wind_speed_kmh, int wind_direction_degrees)
{
    if (!s_dashboard_created || s_wind_speed_label == NULL || s_wind_arrow == NULL)
    {
        return;
    }

    char wind_text[32];

    snprintf(wind_text, sizeof(wind_text), "Wind %.0f km/h", wind_speed_kmh);

    _lock_acquire(&s_lvgl_api_lock);
    lv_label_set_text(s_wind_speed_label, wind_text);
    tft_dashboard_update_wind_arrow_locked(wind_direction_degrees);
    _lock_release(&s_lvgl_api_lock);
}

void tft_dashboard_set_hourly_forecast(
    const tft_forecast_hour_t *forecast,
    size_t forecast_count
)
{
    if (!s_dashboard_created ||
        s_hourly_container == NULL)
    {
        return;
    }

    if (forecast == NULL)
    {
        forecast_count = 0U;
    }

    if (forecast_count > TFT_HOURLY_CARD_COUNT)
    {
        forecast_count = TFT_HOURLY_CARD_COUNT;
    }

    uint8_t *new_icon_pixels[TFT_HOURLY_CARD_COUNT] = {0};
    uint32_t new_icon_data_sizes[TFT_HOURLY_CARD_COUNT] = {0};
    uint32_t new_icon_widths[TFT_HOURLY_CARD_COUNT] = {0};
    uint32_t new_icon_heights[TFT_HOURLY_CARD_COUNT] = {0};
    bool icon_changed[TFT_HOURLY_CARD_COUNT] = {0};
    bool icon_loaded[TFT_HOURLY_CARD_COUNT] = {0};

    for (size_t index = 0U;
         index < forecast_count;
         index++)
    {
        const char *icon_path =
            forecast[index].icon_path;

        bool valid_icon_path =
            icon_path != NULL &&
            icon_path[0] != '\0';

        icon_changed[index] =
            valid_icon_path &&
            strcmp(
                s_active_hourly_icon_paths[index],
                icon_path) != 0;

        if (icon_changed[index])
        {
            icon_loaded[index] =
                tft_icon_load_transparent(
                    icon_path,
                    TFT_HOURLY_ICON_RENDER_LIMIT,
                    &new_icon_pixels[index],
                    &new_icon_data_sizes[index],
                    &new_icon_widths[index],
                    &new_icon_heights[index]
                );
        }
    }

    uint8_t *old_icon_pixels[TFT_HOURLY_CARD_COUNT] = {0};

    _lock_acquire(
        &s_lvgl_api_lock
    );

    if (forecast_count == 0U)
    {
        lv_obj_add_flag(
            s_hourly_container,
            LV_OBJ_FLAG_HIDDEN
        );
    }
    else
    {
        lv_obj_clear_flag(
            s_hourly_container,
            LV_OBJ_FLAG_HIDDEN
        );
    }

    for (size_t index = 0U;
         index < TFT_HOURLY_CARD_COUNT;
         index++)
    {
        if (s_hourly_cards[index] == NULL)
        {
            continue;
        }

        if (index >= forecast_count)
        {
            lv_obj_add_flag(
                s_hourly_cards[index],
                LV_OBJ_FLAG_HIDDEN
            );

            continue;
        }

        lv_obj_clear_flag(
            s_hourly_cards[index],
            LV_OBJ_FLAG_HIDDEN
        );

        const char *time_text =
            forecast[index].time_text != NULL
                ? forecast[index].time_text
                : "--";

        const char *condition_text =
            forecast[index].condition != NULL
                ? forecast[index].condition
                : "--";

        char temperature_text[20];

        snprintf(
            temperature_text,
            sizeof(temperature_text),
            "%.0f C",
            forecast[index].temperature_c
        );

        if (s_hourly_time_labels[index] != NULL)
        {
            lv_label_set_text(
                s_hourly_time_labels[index],
                time_text
            );
        }

        if (s_hourly_temp_labels[index] != NULL)
        {
            lv_label_set_text(
                s_hourly_temp_labels[index],
                temperature_text
            );
        }

        if (s_hourly_condition_labels[index] != NULL)
        {
            lv_label_set_text(
                s_hourly_condition_labels[index],
                condition_text
            );
        }

        const char *icon_path =
            forecast[index].icon_path;

        bool valid_icon_path =
            icon_path != NULL &&
            icon_path[0] != '\0';

        if (s_hourly_icons[index] == NULL)
        {
            continue;
        }

        if (!valid_icon_path)
        {
            lv_obj_add_flag(
                s_hourly_icons[index],
                LV_OBJ_FLAG_HIDDEN
            );

            s_active_hourly_icon_paths[index][0] = '\0';
        }
        else if (!icon_changed[index])
        {
            lv_obj_clear_flag(
                s_hourly_icons[index],
                LV_OBJ_FLAG_HIDDEN
            );
        }
        else if (icon_loaded[index] &&
                 new_icon_pixels[index] != NULL &&
                 new_icon_data_sizes[index] > 0U &&
                 new_icon_widths[index] > 0U &&
                 new_icon_heights[index] > 0U)
        {
            old_icon_pixels[index] =
                s_hourly_icon_pixel_buffers[index];

            s_hourly_icon_pixel_buffers[index] =
                new_icon_pixels[index];

            lv_image_dsc_t *descriptor =
                &s_hourly_icon_descriptors[index];

            memset(
                descriptor,
                0,
                sizeof(*descriptor)
            );

            descriptor->header.magic =
                LV_IMAGE_HEADER_MAGIC;

            descriptor->header.cf =
                LV_COLOR_FORMAT_ARGB8888;

            descriptor->header.flags =
                0;

            descriptor->header.w =
                new_icon_widths[index];

            descriptor->header.h =
                new_icon_heights[index];

            descriptor->header.stride =
                new_icon_widths[index] *
                TFT_ARGB8888_BYTES_PER_PIXEL;

            descriptor->data_size =
                new_icon_data_sizes[index];

            descriptor->data =
                s_hourly_icon_pixel_buffers[index];

            lv_image_set_src(
                s_hourly_icons[index],
                descriptor
            );

            lv_obj_set_size(
                s_hourly_icons[index],
                new_icon_widths[index],
                new_icon_heights[index]
            );

            lv_image_set_scale(
                s_hourly_icons[index],
                256U
            );

            lv_obj_center(
                s_hourly_icons[index]
            );

            lv_obj_clear_flag(
                s_hourly_icons[index],
                LV_OBJ_FLAG_HIDDEN
            );

            lv_obj_invalidate(
                s_hourly_icons[index]
            );

            snprintf(
                s_active_hourly_icon_paths[index],
                sizeof(s_active_hourly_icon_paths[index]),
                "%s",
                icon_path
            );

            new_icon_pixels[index] = NULL;

            ESP_LOGI(
                TAG,
                "Forecast icon %u installed: %s (%ux%u)",
                (unsigned int)(index + 1U),
                icon_path,
                (unsigned int)new_icon_widths[index],
                (unsigned int)new_icon_heights[index]
            );
        }
        else
        {
            lv_obj_add_flag(s_hourly_icons[index], LV_OBJ_FLAG_HIDDEN);

            s_active_hourly_icon_paths[index][0] = '\0';

            ESP_LOGE(
                TAG,
                "Could not load forecast icon %u: %s",
                (unsigned int)(index + 1U),
                icon_path
            );
        }

        if (s_hourly_icon_boxes[index] != NULL)
        {
            lv_obj_update_layout(s_hourly_icon_boxes[index]);
        }
    }

    lv_obj_update_layout(s_hourly_container);

    _lock_release(&s_lvgl_api_lock);

    for (size_t index = 0U; index < TFT_HOURLY_CARD_COUNT; index++)
    {
        if (old_icon_pixels[index] != NULL)
        {
            free(old_icon_pixels[index]);
        }

        if (new_icon_pixels[index] != NULL)
        {
            free(new_icon_pixels[index]);
        }
    }
}