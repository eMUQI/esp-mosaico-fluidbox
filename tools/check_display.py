#!/usr/bin/env python3
"""Run the real frame queue and scan-paced sender from display.c against threaded RTOS, TE and DMA substitutes."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
display = (root / "main/display.c").read_text()
# Extract constants, shared state, callbacks and frame-transfer code, excluding hardware initialization.
defines = display[display.index("#define LCD_HOST"):display.index("// Panel initialization sequence")]
state = display[display.index("static const char *TAG"):display.index("esp_err_t display_init(void)")]
runtime = display[display.index("void display_stage_band"):display.index("esp_err_t display_set_brightness")]

prelude = r'''
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "config.h"
#include "display.h"

#define IRAM_ATTR
#define pdFALSE 0
#define pdTRUE 1
#define portMAX_DELAY (-1)
#define pdMS_TO_TICKS(ms) (ms)
#define portYIELD_FROM_ISR() ((void)0)
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define taskENTER_CRITICAL(lock) pthread_mutex_lock(lock)
#define taskEXIT_CRITICAL(lock) pthread_mutex_unlock(lock)
#define taskENTER_CRITICAL_ISR taskENTER_CRITICAL
#define taskEXIT_CRITICAL_ISR taskEXIT_CRITICAL
#define ESP_ERROR_CHECK(x) assert((x) == 0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
typedef int BaseType_t;
typedef pthread_mutex_t portMUX_TYPE;
typedef void *esp_lcd_panel_handle_t;
typedef void *esp_lcd_panel_io_handle_t;
typedef void esp_lcd_panel_io_event_data_t;
static struct { struct { unsigned dma_outfifo_empty_err_int_raw; } dma_int_raw; } GPSPI2;

// A counting semaphore; with limit 1 and a value it doubles as a one-element queue.
typedef struct { pthread_mutex_t m; pthread_cond_t c; unsigned count, limit, value; } sync_t;
typedef sync_t *SemaphoreHandle_t, *QueueHandle_t;
static sync_t *make(unsigned limit, unsigned count)
{
    sync_t *s = calloc(1, sizeof(*s));
    pthread_mutex_init(&s->m, 0);
    pthread_cond_init(&s->c, 0);
    s->limit = limit;
    s->count = count;
    return s;
}
static int take(sync_t *s, bool wait, unsigned *value)
{
    pthread_mutex_lock(&s->m);
    while (!s->count && wait) pthread_cond_wait(&s->c, &s->m);
    const int taken = s->count > 0;
    if (taken) {
        --s->count;
        if (value) *value = s->value;
        pthread_cond_broadcast(&s->c);
    }
    pthread_mutex_unlock(&s->m);
    return taken;
}
static int give(sync_t *s, const unsigned *value)
{
    pthread_mutex_lock(&s->m);
    while (value && s->count == s->limit) pthread_cond_wait(&s->c, &s->m);
    assert(s->count < s->limit);
    ++s->count;
    if (value) s->value = *value;
    pthread_cond_broadcast(&s->c);
    pthread_mutex_unlock(&s->m);
    return 1;
}
static int xSemaphoreTake(sync_t *s, int wait) { return take(s, wait != 0, NULL); }
static void check_release(sync_t *s);
static int xSemaphoreGive(sync_t *s)
{
    check_release(s);
    return give(s, NULL);
}
static int xSemaphoreGiveFromISR(sync_t *s, BaseType_t *woken) { return give(s, NULL); }
static int xQueueSend(sync_t *s, const unsigned *value, int wait) { return give(s, value); }
static int xQueueReceive(sync_t *s, unsigned *value, int wait) { return take(s, true, value); }
static int64_t esp_timer_get_time(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000LL + t.tv_nsec / 1000;
}
static void vTaskDelay(int ticks) { usleep(ticks * 1000); }
static int gpio_get_level(int pin) { return 0; }
static int esp_lcd_panel_draw_bitmap(void *panel, int left, int y0, int right, int y1, const void *pixels);
static int esp_lcd_panel_io_tx_param(void *io, int cmd, const void *param, size_t size);
'''

harness = r'''
// One band in flight: the CO5300 driver's window command waits for the previous band's DMA.
static pthread_mutex_t dma_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t dma_cond = PTHREAD_COND_INITIALIZER;
static struct { const uint16_t *pixels; int count; bool busy; } dma;
static unsigned received, last_band, overlap, unsynced;

// The panel: TE rises every PERIOD_US, the scan starts OFFSET_US later and reads the bands top to bottom.
enum { PERIOD_US = 16667, OFFSET_US = 500, SCAN_US = PERIOD_US - OFFSET_US, BAND_SCAN_US = SCAN_US / BAND_COUNT };
static volatile bool te_paused;
static pthread_mutex_t te_lock = PTHREAD_MUTEX_INITIALIZER;
static int64_t te_count;  // Identify refreshes by count because host scheduling can extend sleep intervals.
static void *te_source(void *arg)
{
    for (;;) {
        pthread_mutex_lock(&te_lock);
        if (!te_paused) { on_te(NULL); ++te_count; }
        pthread_mutex_unlock(&te_lock);
        usleep(PERIOD_US);
    }
}

// Number of the scan that will show band b if it is written at now, or -1 if the scan is inside band b.
static int64_t showing_scan(unsigned b, uint32_t now)
{
    pthread_mutex_lock(&te_lock);
    const int32_t phase = (int32_t)(now - (s_te_rise_us + OFFSET_US));
    const int64_t count = te_count;
    pthread_mutex_unlock(&te_lock);
    if (phase < (int32_t)(b * BAND_SCAN_US)) return count;
    if (phase >= (int32_t)((b + 1) * BAND_SCAN_US)) return count + 1;
    return -1;
}

static void wait_dma_idle(void)
{
    pthread_mutex_lock(&dma_lock);
    while (dma.busy) pthread_cond_wait(&dma_cond, &dma_lock);
    pthread_mutex_unlock(&dma_lock);
}

static int esp_lcd_panel_draw_bitmap(void *panel, int left, int y0, int right, int y1, const void *data)
{
    wait_dma_idle();
    const uint16_t *pixels = data;
    const unsigned band = y0 / BAND_ROWS, id = pixels[0] / BAND_COUNT;
    assert(right > left && left % 16 == 0 && right % 16 == 0 && y1 - y0 == BAND_ROWS);
    assert(pixels[0] == id * BAND_COUNT + band);
    // Frames arrive complete and in order, bands top to bottom.
    if (band == 0) assert(id == ++received);
    else assert(id == received && band > last_band);
    last_band = band;
    // Check that synchronized band writes start outside the modeled scan interval
    // and target the same refresh. Classify synchronization at the first band.
    static int64_t frame_scan;
    static bool frame_synced;
    const uint32_t now = (uint32_t)esp_timer_get_time();
    if (band == 0) {
        frame_synced = now - s_te_rise_us < TE_TIMEOUT_US;
        unsynced += !frame_synced;
    }
    if (frame_synced) {
        const int64_t shown = showing_scan(band, now);
        if (shown < 0 || (band > 0 && shown != frame_scan))
            fprintf(stderr, "frame %u band %u at phase %d us: shown %lld, frame %lld\n", id, band,
                    (int)(now - s_te_rise_us - OFFSET_US), (long long)shown, (long long)frame_scan);
        assert(shown >= 0);
        if (band == 0) frame_scan = shown;
        else assert(shown == frame_scan);
    }
    pthread_mutex_lock(&dma_lock);
    dma.pixels = pixels;
    dma.count = (right - left) * BAND_ROWS;
    dma.busy = true;
    pthread_cond_broadcast(&dma_cond);
    pthread_mutex_unlock(&dma_lock);
    return 0;
}

static int esp_lcd_panel_io_tx_param(void *io, int cmd, const void *param, size_t size)
{
    assert(cmd == -1 && !param && !size);
    wait_dma_idle();
    return 0;
}

// Validate slot contents after draw_bitmap returns to detect modification during DMA.
static void *dma_engine(void *arg)
{
    for (;;) {
        pthread_mutex_lock(&dma_lock);
        while (!dma.busy) pthread_cond_wait(&dma_cond, &dma_lock);
        const uint16_t *pixels = dma.pixels;
        const int count = dma.count;
        pthread_mutex_unlock(&dma_lock);
        // Model 1.2 ms per full-width band, exceeding its scan interval.
        for (int pass = 0; pass < 2; ++pass) {
            for (int i = 0; i < count; ++i) assert(pixels[i] == pixels[0]);
            usleep(count / BAND_ROWS * 600 / LCD_H_RES);
        }
        pthread_mutex_lock(&dma_lock);
        dma.busy = false;
        pthread_cond_broadcast(&dma_cond);
        pthread_mutex_unlock(&dma_lock);
    }
}

// A frame slot may return to the renderer only after DMA has stopped reading it.
static void check_release(sync_t *s)
{
    if (s != s_frames[0].available && s != s_frames[1].available) return;
    pthread_mutex_lock(&dma_lock);
    assert(!dma.busy);
    pthread_mutex_unlock(&dma_lock);
}

static void *run_display_task(void *arg)
{
    display_task(arg);
    return 0;
}

int main(void)
{
    enum { FRAMES = 160 };
    s_ready = make(1, 0);
    s_scan_offset_us = OFFSET_US;
    s_scan_us = SCAN_US;
    for (int i = 0; i < 2; ++i) {
        s_frames[i].pixels = aligned_alloc(64, LCD_H_RES * LCD_V_RES * sizeof(uint16_t));
        s_frames[i].available = make(1, i == 1);  // Match display_init(): only slot one is initially available.
    }
    pthread_t thread;
    assert(!pthread_create(&thread, 0, te_source, 0));
    usleep(2 * PERIOD_US);
    assert(!pthread_create(&thread, 0, run_display_task, 0));
    assert(!pthread_create(&thread, 0, dma_engine, 0));

    display_present();
    assert(s_staging == 0);  // an empty frame is not queued and keeps its slot

    static uint16_t band[BAND_PIXELS];
    for (unsigned id = 1; id <= FRAMES; ++id) {
        te_paused = id >= 100 && id < 106;  // Suspend TE pulses to exercise timeout-based unsynchronized transfer.
        for (unsigned b = 0; b < BAND_COUNT; ++b) {
            if (b && (id + b) % 3 == 0) continue;
            const int left = (id + b) % 5 * 16, right = LCD_H_RES - (id + b) % 7 * 16;
            for (int i = 0; i < (right - left) * BAND_ROWS; ++i) band[i] = id * BAND_COUNT + b;
            usleep(100);
            pthread_mutex_lock(&dma_lock);
            overlap += dma.busy;
            pthread_mutex_unlock(&dma_lock);
            display_stage_band(b, left, right, band);
        }
        display_present();
    }
    // Wait for the final queued slot to be released after transfer completion.
    xSemaphoreTake(s_frames[s_staging ^ 1].available, portMAX_DELAY);
    const display_stats_t stats = display_take_stats();
    assert(received == FRAMES && stats.frames == FRAMES);
    assert(stats.te_timeouts > 0 && stats.te_timeouts < FRAMES && stats.te_timeouts == unsynced);
    assert(overlap > 0);
    puts("PASS: 160 ordered frames, each shown whole by one scan with bands slower than the scan, cropped/skipped "
         "bands, drawing during DMA, in-flight pixels untouched, slot released after DMA, empty frames, TE loss");
    return 0;
}
'''

with tempfile.TemporaryDirectory() as tmp:
    path = Path(tmp)
    (path / "esp_err.h").write_text("#pragma once\ntypedef int esp_err_t;\n")
    (path / "check.c").write_text(prelude + defines + state + runtime + harness)
    subprocess.run(["cc", "-std=c11", "-D_DEFAULT_SOURCE", "-O1", "-g", "-fsanitize=address,undefined", "-pthread",
                    "-I" + tmp, "-I" + str(root / "main"), str(path / "check.c"), "-o", str(path / "check")],
                   check=True)
    subprocess.run([str(path / "check")], check=True)
