#include <stdio.h>
#include <stdatomic.h>
#include "board.h"
#include "config.h"
#include "display.h"
#include "imu.h"
#include "render.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_console.h"
#include "tinyusb_default_config.h"

static atomic_uint s_steps, s_render_us, s_imu_misses;
// Frame totals are updated and reset together, so every average covers the same frames.
static portMUX_TYPE s_frame_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_frames;
static render_stats_t s_frame_sum;

static void render_task(void *arg)
{
    for (;;) {
        int64_t start = esp_timer_get_time();
        const render_stats_t st = render_frame(false);
        atomic_store(&s_render_us, esp_timer_get_time() - start);
        taskENTER_CRITICAL(&s_frame_lock);
        s_frame_sum.cpu_us += st.cpu_us; s_frame_sum.prep_us += st.prep_us;
        s_frame_sum.stage_us += st.stage_us; s_frame_sum.slot_wait_us += st.slot_wait_us;
        s_frame_sum.bytes += st.bytes; s_frame_sum.bands += st.bands;
        ++s_frames;
        taskEXIT_CRITICAL(&s_frame_lock);
        vTaskDelay(1);
    }
}

static void sim_task(void *arg)
{
    sim_forces_t forces = {.gravity = {0, GRAVITY_GAIN * GRAVITY_MPS2 * PX_PER_METER, 0}};
    render_mode_t mode = RENDER_PARTICLES;
    int64_t previous = esp_timer_get_time();
    int64_t last_sample = previous;
    for (;;) {
        int64_t now = esp_timer_get_time();
        float dt = (now - previous) * 1e-6f;
        previous = now;
        if (dt > 0.05f) dt = 0.05f;
        if (dt < 0.0001f) dt = 0.0001f;
        if (imu_read((now - last_sample) * 1e-6f, &forces)) {
            last_sample = now;
        } else {
            atomic_fetch_add(&s_imu_misses, 1);
            if (now - last_sample > 250000) {
                forces = (sim_forces_t){.gravity = {0, GRAVITY_GAIN * GRAVITY_MPS2 * PX_PER_METER, 0}};
            }
        }
        if (board_take_wave_toggle()) {
            static const char *const names[RENDER_MODE_COUNT] = {"particles", "liquid", "water", "lava", "ukiyoe"};
            mode = (render_mode_t)((mode + 1) % RENDER_MODE_COUNT);
            render_set_mode(mode);
            ESP_LOGI("fluidbox", "mode: %s", names[mode]);
        }
        if (board_take_reset()) sim_reset();
        render_set_gravity(forces.gravity);
        // Reduce the lava simulation time scale to produce slower apparent flow.
        sim_step(mode == RENDER_LAVA ? dt * LAVA_TIME_SCALE : dt, &forces);
        atomic_fetch_add(&s_steps, 1);
        vTaskDelay(1);
    }
}

void app_main(void)
{
    tinyusb_config_t usb = TINYUSB_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(tinyusb_driver_install(&usb));
    const tinyusb_config_cdcacm_t cdc = {.cdc_port = TINYUSB_CDC_ACM_0};
    ESP_ERROR_CHECK(tinyusb_cdcacm_init(&cdc));
    ESP_ERROR_CHECK(tinyusb_console_init(TINYUSB_CDC_ACM_0));
    setvbuf(stdout, NULL, _IONBF, 0);
    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(board_init(&bus));
    ESP_ERROR_CHECK(display_init());
    ESP_ERROR_CHECK(display_set_brightness(DISPLAY_BRIGHTNESS_PCT * 255 / 100));
    esp_err_t imu_status = imu_init(bus);
    if (imu_status != ESP_OK) ESP_LOGW("fluidbox", "BMI270 unavailable: %s; fixed gravity", esp_err_to_name(imu_status));
    sim_init();
    render_init();
    ESP_ERROR_CHECK(xTaskCreatePinnedToCore(sim_task, "simulation", 6144, NULL, 5, NULL, 1) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreatePinnedToCore(render_task, "renderer", 4096, NULL, 5, NULL, 0) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    // Retain cumulative underruns to diagnose intermittent display faults.
    uint32_t last_steps = 0, underruns_since_boot = 0;
    int64_t previous = esp_timer_get_time();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        int64_t now = esp_timer_get_time();
        float elapsed = (now - previous) * 1e-6f;
        previous = now;
        taskENTER_CRITICAL(&s_frame_lock);
        const uint32_t frames = s_frames;
        const render_stats_t sum = s_frame_sum;
        s_frames = 0;
        s_frame_sum = (render_stats_t){0};
        taskEXIT_CRITICAL(&s_frame_lock);
        const display_stats_t sent = display_take_stats();
        underruns_since_boot += sent.underruns;
        const uint32_t steps = atomic_load(&s_steps);
        sim_stats_t st;
        sim_stats(&st);
        ESP_LOGD("fluidbox", "%.1f fps | %.1f Hz | %d particles | grid/density/relax %d/%d/%d us | render %u us | rho %.2f/%.2f | SRAM %u | IMU misses %u | pairs %d | clamped %d | speed %.0f/%.0f",
                 frames / elapsed, (steps - last_steps) / elapsed, PARTICLE_COUNT,
                 st.us_grid, st.us_density, st.us_relax, atomic_load(&s_render_us),
                 st.mean_density, st.rest_density, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), atomic_load(&s_imu_misses), st.pairs, st.clamped, st.mean_speed, st.max_speed);
        const float drawn = frames ? frames : 1, shown = sent.frames ? sent.frames : 1;
        ESP_LOGD("profile", "CPU %.0f us (prep %.0f) | stage %.0f us | slot wait %.0f us | %.0f bytes/frame | %.1f bands/frame"
                 " | sent %lu: scan wait %.0f us, transfer %.0f us, TE timeouts %lu, underruns %lu (%lu since boot) | QSPI %d MHz",
                 sum.cpu_us / drawn, sum.prep_us / drawn, sum.stage_us / drawn, sum.slot_wait_us / drawn,
                 sum.bytes / drawn, sum.bands / drawn, (unsigned long)sent.frames, sent.scan_wait_us / shown,
                 sent.transfer_us / shown, (unsigned long)sent.te_timeouts, (unsigned long)sent.underruns,
                 (unsigned long)underruns_since_boot, LCD_PIXEL_CLOCK_HZ / 1000000);
        last_steps = steps;
    }
}
