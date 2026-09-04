#include "publisher_capture.h"

#include "bsp_display.h"
#include "bsp_pins.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "src/core/lv_refr.h"
#include "src/core/lv_refr_private.h"
#include "src/display/lv_display_private.h"
#include "src/draw/lv_draw_private.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define CAPTURE_ROWS 16
#define CAPTURE_STRIDE (BSP_LCD_W * 2)
#define CAPTURE_BYTES (CAPTURE_STRIDE * CAPTURE_ROWS)

static const char *TAG = "publisher_capture";
static TaskHandle_t s_task;
static uint8_t s_pixels[CAPTURE_BYTES] __attribute__((aligned(64)));

static int quiet_vprintf(const char *format, va_list args)
{
    (void)format;
    (void)args;
    return 0;
}

static bool serial_write_all(const void *data, size_t size)
{
    const uint8_t *cursor = data;
    while (size > 0) {
        int written = usb_serial_jtag_write_bytes(
            cursor, size, pdMS_TO_TICKS(2000));
        if (written <= 0) return false;
        cursor += written;
        size -= (size_t)written;
    }
    return true;
}

static bool render_strip(lv_obj_t *screen, int32_t y, uint32_t rows)
{
    lv_draw_buf_t draw_buf;
    uint32_t data_size = CAPTURE_STRIDE * rows;
    memset(s_pixels, 0, data_size);
    if (lv_draw_buf_init(&draw_buf, BSP_LCD_W, rows, LV_COLOR_FORMAT_RGB565,
                         CAPTURE_STRIDE, s_pixels, data_size) != LV_RESULT_OK) {
        return false;
    }

    lv_area_t area = {
        .x1 = 0,
        .y1 = y,
        .x2 = BSP_LCD_W - 1,
        .y2 = y + (int32_t)rows - 1,
    };
    lv_layer_t layer;
    lv_layer_init(&layer);
    layer.draw_buf = &draw_buf;
    layer.buf_area = area;
    layer.color_format = LV_COLOR_FORMAT_RGB565;
    layer._clip_area = area;
    layer.phy_clip_area = area;

    lv_draw_unit_send_event(NULL, LV_EVENT_CHILD_CREATED, &layer);
    lv_display_t *display = lv_obj_get_display(screen);
    lv_display_t *previous_display = lv_refr_get_disp_refreshing();
    lv_layer_t *previous_layer = display->layer_head;
    display->layer_head = &layer;
    lv_refr_set_disp_refreshing(display);

    lv_obj_redraw(&layer, screen);
    layer.all_tasks_added = true;
    while (layer.draw_task_head) {
        lv_draw_dispatch_wait_for_request();
        lv_draw_dispatch();
    }

    display->layer_head = previous_layer;
    lv_refr_set_disp_refreshing(previous_display);
    lv_draw_unit_send_event(NULL, LV_EVENT_SCREEN_LOAD_START, &layer);
    lv_draw_unit_send_event(NULL, LV_EVENT_CHILD_DELETED, &layer);
    return true;
}

static void send_capture(void)
{
    if (!bsp_lvgl_lock(3000)) {
        ESP_LOGW(TAG, "LVGL lock timed out");
        return;
    }

    lv_obj_t *screen = lv_screen_active();
    if (!screen) {
        bsp_lvgl_unlock();
        return;
    }

    char header[80];
    int header_len = snprintf(
        header, sizeof(header), "FAP_SCREENSHOT_V1 %d %d RGB565LE %d\n",
        BSP_LCD_W, BSP_LCD_H, BSP_LCD_W * BSP_LCD_H * 2);

    vprintf_like_t previous_vprintf = esp_log_set_vprintf(quiet_vprintf);
    vTaskDelay(pdMS_TO_TICKS(20));
    bool ok = serial_write_all(header, (size_t)header_len);

    for (int32_t y = 0; ok && y < BSP_LCD_H; y += CAPTURE_ROWS) {
        uint32_t rows = (uint32_t)(BSP_LCD_H - y);
        if (rows > CAPTURE_ROWS) rows = CAPTURE_ROWS;
        ok = render_strip(screen, y, rows) &&
             serial_write_all(s_pixels, CAPTURE_STRIDE * rows);
    }
    usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(5000));
    esp_log_set_vprintf(previous_vprintf);
    bsp_lvgl_unlock();

    if (!ok) ESP_LOGW(TAG, "screen capture transfer failed");
}

static void capture_task(void *arg)
{
    (void)arg;
    char line[48];
    size_t used = 0;

    for (;;) {
        uint8_t ch;
        int received = usb_serial_jtag_read_bytes(
            &ch, 1, pdMS_TO_TICKS(200));
        if (received <= 0) continue;
        if (ch == '\r') continue;
        if (ch == '\n') {
            line[used] = '\0';
            if (strcmp(line, "FAP_SCREENSHOT_V1") == 0) send_capture();
            used = 0;
        } else if (used + 1 < sizeof(line)) {
            line[used++] = (char)ch;
        } else {
            used = 0;
        }
    }
}

void publisher_capture_start(void)
{
    if (s_task) return;

    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t config =
            USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        config.tx_buffer_size = 8192;
        if (usb_serial_jtag_driver_install(&config) != ESP_OK) {
            ESP_LOGW(TAG, "USB serial driver unavailable");
            return;
        }
    }
    usb_serial_jtag_vfs_use_driver();
    if (xTaskCreate(capture_task, "fap_capture", 4096, NULL, 4, &s_task) !=
        pdPASS) {
        s_task = NULL;
        ESP_LOGW(TAG, "capture task creation failed");
    }
}
