#!/usr/bin/env python3
"""Exercise the real solver and band renderer with host-only platform substitutes."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
headers = {
    "esp_log.h": "#define ESP_LOGI(...) ((void)0)\n#define ESP_LOGE(...) ((void)0)\n",
    "esp_timer.h": "#include <stdint.h>\nextern int64_t host_time_us;\nstatic inline int64_t esp_timer_get_time(void) { return host_time_us; }\n",
    "freertos/FreeRTOS.h": "#include <assert.h>\n#define configASSERT assert\n#define portMAX_DELAY 0\n",
    "freertos/semphr.h": "typedef int SemaphoreHandle_t;\n#define xSemaphoreCreateMutex() 1\n#define xSemaphoreTake(a,b) ((void)0)\n#define xSemaphoreGive(a) ((void)0)\n",
    "esp_err.h": "#include <assert.h>\ntypedef int esp_err_t;\n#define ESP_ERROR_CHECK(x) assert((x)==0)\n",
}
source = r'''
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "sim.h"
#include "render.h"
#include "display.h"
int64_t host_time_us;  // Shared clock gives both renderers the same animation time.
static int transfers;
static uint16_t screen[2][LCD_V_RES][LCD_H_RES];
static int target;
static unsigned bytes[2];
void reference_init(void);
void reference_set_mode(render_mode_t);
render_stats_t reference_frame(bool);
void display_present(void) {}
void display_stage_band(int b, int left, int right, const uint16_t *p) {
    assert(b >= 0 && b < BAND_COUNT);
    assert(left >= 0 && left < right && right <= LCD_H_RES);
    assert(left % 16 == 0 && right % 16 == 0);  // Require 16-pixel window alignment for packed DMA bands.
    for (int y = 0; y < BAND_ROWS; ++y)
        memcpy(&screen[target][b * BAND_ROWS + y][left], p + y * (right-left), (right-left) * 2);
    bytes[target] += (right-left) * BAND_ROWS * 2;
    ++transfers;
}
int main(void) {
    sim_init(); render_init(); reference_init();
    render_frame(false);
    assert(transfers == BAND_COUNT);
    transfers = 0;
    render_frame(false);
    assert(transfers == 0);
    target = 1; reference_frame(true); target = 0;
    sim_particle_view_t view[PARTICLE_MAX];
    static const struct { int step; render_mode_t mode; } switches[] = {
        {100, RENDER_WATER}, {600, RENDER_UKIYOE}, {800, RENDER_LAVA}, {900, RENDER_LIQUID}, {1000, RENDER_PARTICLES},
    };
    for (int step = 0; step < 1200; ++step) {
        sim_forces_t f = {.gravity = {0, GRAVITY_GAIN * GRAVITY_MPS2 * PX_PER_METER, 0}};
        if (step >= 200 && step < 400) {
            f.gravity[0] = f.gravity[1]; f.gravity[1] = 0;
        }
        for (size_t k = 0; k < sizeof(switches) / sizeof(switches[0]); ++k)
            if (step == switches[k].step) { render_set_mode(switches[k].mode); reference_set_mode(switches[k].mode); }
        if (step == 1000) sim_reset();
        host_time_us += 16667;
        sim_step(step % 5 == 0 ? 0.05f : (step % 2 ? 0.007f : 0.012f), &f);
        int n = sim_snapshot(view, PARTICLE_MAX);
        assert(n == PARTICLE_COUNT);
        for (int i = 0; i < n; ++i) {
            assert(isfinite(view[i].x) && isfinite(view[i].y) && isfinite(view[i].z));
            assert(isfinite(view[i].speed));
            assert(view[i].x >= 0 && view[i].x <= BOX_W);
            assert(view[i].y >= 0 && view[i].y <= BOX_H);
            assert(view[i].z >= 0 && view[i].z <= BOX_D);
        }
        transfers = 0; render_frame(false);
        assert(transfers > 0 && transfers <= BAND_COUNT);
        target = 1; reference_frame(true); target = 0;
        assert(memcmp(screen[0], screen[1], sizeof(screen[0])) == 0);
    }
    // Constant forces isolate solver agitation from sensor noise and input filtering.
    for (int pose = 0; pose < 3; ++pose) {
        sim_reset();
        sim_forces_t fixed = {0};
        const float g = GRAVITY_GAIN * GRAVITY_MPS2 * PX_PER_METER;
        if (pose == 0) fixed.gravity[2] = g;
        if (pose == 1) fixed.gravity[1] = g;
        if (pose == 2) fixed.gravity[0] = fixed.gravity[1] = g * 0.70710678f;
        double residual_speed = 0, clamps = 0;
        for (int step = 0; step < 1800; ++step) {
            sim_step(0.020f, &fixed);
            if (step >= 1500) {
                sim_stats_t stats;
                sim_stats(&stats);
                residual_speed += stats.mean_speed;
                clamps += stats.clamped;
            }
        }
        printf("Static pose %d: residual speed %.1f px/s, clamps %.1f/step\n",
               pose, residual_speed / 300, clamps / 300);
        assert(residual_speed / 300 < 700);
        assert(clamps / 300 < 1);
    }
    assert(bytes[0] < bytes[1]);
    printf("Transfer bytes: cropped=%u full=%u saved=%.1f%%\n", bytes[0], bytes[1], 100.0 * (1.0 - (double)bytes[0]/bytes[1]));
    printf("PASS: pixel-identical cropped/full frames, %d particles, 1200 steps, particle/water/ukiyo-e/lava/liquid switches, tilt, reset/stall stability and band bounds (ASan/UBSan)\n", PARTICLE_COUNT);
}
'''
with tempfile.TemporaryDirectory() as tmp:
    path = Path(tmp)
    for name, text in headers.items():
        file = path / name
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_text("#pragma once\n" + text)
    (path / "check.c").write_text(source)
    (path / "reference.c").write_text('#define render_set_mode reference_set_mode\n#define render_set_gravity reference_set_gravity\n#define render_init reference_init\n#define render_frame reference_frame\n#include "' + str(root / 'main/render.c') + '"\n')
    subprocess.run(["cc", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
                    "-I" + tmp, "-I" + str(root / "main"), str(path / "check.c"),
                    str(root / "main/sim.c"), str(root / "main/render.c"), str(path / "reference.c"), "-lm",
                    "-o", str(path / "check")], check=True)
    subprocess.run([str(path / "check")], check=True)
