/*
 * SPDX-FileCopyrightText: 2026 V4C38
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: MIT AND Apache-2.0
 *
 * Modified for Mosaico FluidBox: the ESP-Mosaico BSP initialization sequence
 * is integrated into a standalone renderer without LVGL. Display transfers
 * use PSRAM frame buffers and TE-synchronized band scheduling.
 * See NOTICE for the source repositories and revision identifiers.
 */
#include "display.h"

#include "config.h"
#include "board.h"
#include "driver/gpio.h"
#include <string.h>
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "soc/spi_struct.h"

#define LCD_HOST SPI2_HOST
#define LCD_CS 50
#define LCD_TE 43
#define LCD_DATA0 36
#define LCD_DATA1 51
#define LCD_DATA2 35
#define LCD_DATA3 9

#define BAND_PIXELS (LCD_H_RES * BAND_ROWS)
#define TE_TIMEOUT_US 25000  // no TE edge for this long: send unsynchronized
#define SCAN_MARGIN_US 200   // extra wait behind the scan for measurement error

// Panel initialization sequence adapted from the Apache-2.0 ESP-Mosaico BSP.
static const co5300_lcd_init_cmd_t s_vendor_init[] = {
    {0x11, NULL, 0, 600},
    {0xFE, (uint8_t[]){0x20}, 1, 0},
    {0x19, (uint8_t[]){0x10}, 1, 0},
    {0x1C, (uint8_t[]){0xA0}, 1, 0},
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4, 0},
    {0x29, NULL, 0, 600},
};

static const char *TAG = "display";

typedef struct {
    uint16_t *pixels;  // packed band windows; band b starts at b * BAND_PIXELS
    struct { int left, right; } windows[BAND_COUNT];
    SemaphoreHandle_t available;  // given once the slot has been sent
} frame_t;

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;
static frame_t s_frames[2];
static unsigned s_staging;     // slot the renderer is filling
static QueueHandle_t s_ready;  // slot index waiting to be sent
// TE timing: latest rising edge and measured period, in 32-bit microseconds.
// Individual ISR updates are atomic; the pair is not a coherent snapshot.
static volatile uint32_t s_te_rise_us, s_te_period_us = 16667;
// Estimated offset from the TE rising edge to active scan, and active scan duration.
static uint32_t s_scan_offset_us, s_scan_us = 16667;
static portMUX_TYPE s_stats_lock = portMUX_INITIALIZER_UNLOCKED;
static display_stats_t s_stats;
static void display_task(void *arg);

static void IRAM_ATTR on_te(void *arg)
{
    const uint32_t now = (uint32_t)esp_timer_get_time(), period = now - s_te_rise_us;
    if (period > 8000 && period < 25000) s_te_period_us = period;
    s_te_rise_us = now;
}

// Estimates active-scan timing from one TE pulse. A high pulse shorter than half
// a period is treated as blanking; a longer pulse is treated as active scan.
static void measure_scan(void)
{
    const int64_t give_up = esp_timer_get_time() + 100000;
    while (gpio_get_level(LCD_TE) && esp_timer_get_time() < give_up) {}
    while (!gpio_get_level(LCD_TE) && esp_timer_get_time() < give_up) {}
    const int64_t rise = esp_timer_get_time();
    while (gpio_get_level(LCD_TE) && esp_timer_get_time() < give_up) {}
    const int64_t fall = esp_timer_get_time();
    if (fall >= give_up) {
        ESP_LOGW(TAG, "TE not toggling; frames go out without scan synchronization");
        return;
    }
    const uint32_t high = (uint32_t)(fall - rise), period = s_te_period_us;
    s_scan_offset_us = high < period / 2 ? high : 0;
    s_scan_us = high < period / 2 ? period - high : high;
    ESP_LOGI(TAG, "TE period %lu us, scan starts %lu us after the rising edge and takes %lu us",
             (unsigned long)period, (unsigned long)s_scan_offset_us, (unsigned long)s_scan_us);
}

// Runs as each band's colour transfer ends, before the driver starts the next transaction and clears
// the flag: set when DMA could not fetch the frame slot from PSRAM as fast as the SPI clock sent it.
static bool IRAM_ATTR on_color_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *event, void *ctx)
{
    if (GPSPI2.dma_int_raw.dma_outfifo_empty_err_int_raw) {
        taskENTER_CRITICAL_ISR(&s_stats_lock);
        s_stats.underruns++;
        taskEXIT_CRITICAL_ISR(&s_stats_lock);
    }
    return false;
}

esp_err_t display_init(void)
{
    const gpio_config_t power = {.pin_bit_mask = 1ULL << 60, .mode = GPIO_MODE_OUTPUT};
    ESP_RETURN_ON_ERROR(gpio_config(&power), TAG, "configure LCD power");
    ESP_RETURN_ON_ERROR(gpio_set_level(60, 0), TAG, "enable LCD power");
    vTaskDelay(pdMS_TO_TICKS(20));
    for (int i = 0; i < 2; ++i) {
        s_frames[i].pixels = heap_caps_aligned_alloc(64, LCD_H_RES * LCD_V_RES * sizeof(uint16_t),
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        s_frames[i].available = xSemaphoreCreateBinary();
        ESP_RETURN_ON_FALSE(s_frames[i].pixels && s_frames[i].available, ESP_ERR_NO_MEM, TAG, "allocate frame slot");
    }
    // The renderer starts on slot zero; slot one is free for the next frame.
    xSemaphoreGive(s_frames[1].available);
    s_ready = xQueueCreate(1, sizeof(unsigned));
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_NO_MEM, TAG, "allocate frame queue");
    const gpio_config_t te = {
        .pin_bit_mask = 1ULL << LCD_TE, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, .intr_type = GPIO_INTR_POSEDGE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&te), TAG, "configure TE input");
    esp_err_t isr_status = gpio_install_isr_service(0);
    if (isr_status != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(isr_status, TAG, "install GPIO ISR service");
    }
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(LCD_TE, on_te, NULL), TAG, "attach TE interrupt");

    const spi_bus_config_t bus_config = CO5300_PANEL_BUS_QSPI_CONFIG(
        board_lcd_clock_pin(), LCD_DATA0, LCD_DATA1, LCD_DATA2, LCD_DATA3,
        BAND_PIXELS * sizeof(uint16_t));
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &bus_config, SPI_DMA_CH_AUTO),
                        TAG, "spi bus init failed");

    const int pins[] = {board_lcd_clock_pin(), LCD_DATA0, LCD_DATA1, LCD_DATA2, LCD_DATA3};
    for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
        ESP_RETURN_ON_ERROR(gpio_set_drive_capability(pins[i], LCD_QSPI_DRIVE_CAP), TAG, "QSPI drive strength");
    }

    esp_lcd_panel_io_spi_config_t io_config =
        CO5300_PANEL_IO_QSPI_CONFIG(LCD_CS, on_color_done, NULL);
    io_config.pclk_hz = LCD_PIXEL_CLOCK_HZ;
    io_config.trans_queue_depth = 2;
    io_config.flags.psram_dma_direct = 1;
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &s_io),
        TAG, "panel io init failed");

    const co5300_vendor_config_t vendor_config = {
        .init_cmds = s_vendor_init,
        .init_cmds_size = sizeof(s_vendor_init) / sizeof(s_vendor_init[0]),
        .flags.use_qspi_interface = 1,
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = board_lcd_reset_pin(),
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = (void *)&vendor_config,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_co5300(s_io, &panel_config, &s_panel),
                        TAG, "panel init failed");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel setup failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "display on failed");
    vTaskDelay(pdMS_TO_TICKS(50));  // let the TE interrupt measure a period first
    measure_scan();
    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCore(display_task, "display", 4096, NULL, 6, NULL, 0) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "create display task");

    return ESP_OK;
}

void display_stage_band(int band_index, int x_start, int x_end, const uint16_t *pixels)
{
    frame_t *frame = &s_frames[s_staging];
    memcpy(frame->pixels + band_index * BAND_PIXELS, pixels, (x_end - x_start) * BAND_ROWS * sizeof(*pixels));
    frame->windows[band_index].left = x_start;
    frame->windows[band_index].right = x_end;
}

void display_present(void)
{
    frame_t *frame = &s_frames[s_staging];
    bool staged = false;
    for (int b = 0; b < BAND_COUNT; ++b) staged |= frame->windows[b].right > frame->windows[b].left;
    if (!staged) return;

    xQueueSend(s_ready, &s_staging, portMAX_DELAY);
    s_staging ^= 1;
    frame = &s_frames[s_staging];
    xSemaphoreTake(frame->available, portMAX_DELAY);
    memset(frame->windows, 0, sizeof(frame->windows));
}

display_stats_t display_take_stats(void)
{
    taskENTER_CRITICAL(&s_stats_lock);
    const display_stats_t stats = s_stats;
    s_stats = (display_stats_t){0};
    taskEXIT_CRITICAL(&s_stats_lock);
    return stats;
}

// Waits until at least t using the configured 1 ms tick and a two-tick margin.
// Task scheduling may delay resumption beyond the requested interval.
static void wait_until(uint32_t t)
{
    const int32_t d = (int32_t)(t - (uint32_t)esp_timer_get_time());
    if (d > 0) vTaskDelay(pdMS_TO_TICKS(d / 1000) + 2);
}

// Schedules each band after its estimated scan interval. Writes must finish before
// the next scan reaches that band to avoid tearing. Starts beyond half a refresh
// wait for a new TE edge, subject to TE_TIMEOUT_US, to provide transfer margin.
// A missing TE signal disables scan waits for that frame.
static void send_frame(const frame_t *frame)
{
    const uint32_t start = (uint32_t)esp_timer_get_time();
    uint32_t rise = s_te_rise_us;
    if ((int32_t)(start - rise - s_scan_offset_us) > (int32_t)(s_te_period_us / 2)) {
        while (s_te_rise_us == rise && (uint32_t)esp_timer_get_time() - start < TE_TIMEOUT_US) vTaskDelay(1);
        rise = s_te_rise_us;
    }
    uint32_t waited = (uint32_t)esp_timer_get_time() - start;
    const bool synced = (uint32_t)esp_timer_get_time() - rise < TE_TIMEOUT_US;
    static int was_synced = -1;
    if (synced != was_synced) {
        if (synced) ESP_LOGI(TAG, "TE synchronization active");
        else ESP_LOGW(TAG, "TE lost; sending without scan synchronization");
        was_synced = synced;
    }
    const uint32_t scan = rise + s_scan_offset_us, band_us = s_scan_us / BAND_COUNT;

    for (int b = 0; b < BAND_COUNT; ++b) {
        const int left = frame->windows[b].left, right = frame->windows[b].right;
        if (right <= left) continue;
        if (synced) {
            const uint32_t before = (uint32_t)esp_timer_get_time();
            wait_until(scan + (b + 1) * band_us + SCAN_MARGIN_US);
            waited += (uint32_t)esp_timer_get_time() - before;
        }
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(s_panel, left, b * BAND_ROWS, right, (b + 1) * BAND_ROWS,
                                                  frame->pixels + b * BAND_PIXELS));
    }
    // A write with no command or parameter returns once every queued DMA transfer has finished.
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(s_io, -1, NULL, 0));
    const uint32_t elapsed = (uint32_t)esp_timer_get_time() - start;

    taskENTER_CRITICAL(&s_stats_lock);
    s_stats.frames++;
    s_stats.scan_wait_us += waited;
    s_stats.transfer_us += elapsed - waited;
    s_stats.te_timeouts += !synced;
    taskEXIT_CRITICAL(&s_stats_lock);
}

static void display_task(void *arg)
{
    unsigned slot;
    for (;;) {
        xQueueReceive(s_ready, &slot, portMAX_DELAY);
        send_frame(&s_frames[slot]);
        xSemaphoreGive(s_frames[slot].available);
    }
}

esp_err_t display_set_brightness(uint8_t level)
{
    // QSPI parameter writes encode opcode 0x02 and the register in a 32-bit command.
    return esp_lcd_panel_io_tx_param(s_io, (0x02 << 24) | (0x51 << 8), &level, 1);
}
