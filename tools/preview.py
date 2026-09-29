#!/usr/bin/env python3
"""Render the real simulation and renderer on the host, driven by main/scenario.c.

Render one mode or two modes side by side from the same particle state.
Write an MP4, or PNG stills at chosen scenario times:

    tools/preview.py out.mp4 --modes liquid,water
    tools/preview.py stills/ --modes liquid --at 2,10,14,19
"""
from pathlib import Path
import argparse
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
MODES = ["particles", "liquid", "water", "lava", "ukiyoe"]
FPS, SIM_HZ = 30, 90  # Fixed preview rates; device rates depend on workload.

headers = {
    "esp_log.h": "#define ESP_LOGI(...) ((void)0)\n#define ESP_LOGE(...) ((void)0)\n",
    "esp_timer.h": "#include <stdint.h>\nextern int64_t host_time_us;\nstatic inline int64_t esp_timer_get_time(void) { return host_time_us; }\n",
    "freertos/FreeRTOS.h": "#include <assert.h>\n#define configASSERT assert\n#define portMAX_DELAY 0\n",
    "freertos/semphr.h": "typedef int SemaphoreHandle_t;\n#define xSemaphoreCreateMutex() 1\n#define xSemaphoreTake(a,b) ((void)0)\n#define xSemaphoreGive(a) ((void)0)\n",
    "esp_err.h": "typedef int esp_err_t;\n",
}

harness = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
// argv[4...] are the scenario seconds of every rendered frame; frames marked '!' are written out.
#include "display.h"
#include "render.h"
#include "scenario.h"
int64_t host_time_us;
static uint16_t screen[2][LCD_V_RES][LCD_H_RES];  // Simulated panel memory; unstaged pixels retain their values.
static int target;
void second_init(void);
void second_set_mode(render_mode_t);
void second_set_gravity(const float *);
render_stats_t second_frame(bool);
void display_present(void) {}
void display_stage_band(int b, int left, int right, const uint16_t *p) {
    for (int y = 0; y < BAND_ROWS; ++y)
        memcpy(&screen[target][b * BAND_ROWS + y][left], p + y * (right - left), (right - left) * 2);
}
int main(int argc, char **argv) {
    const int mode_a = atoi(argv[1]), mode_b = atoi(argv[2]), frames = atoi(argv[3]);
    const int panels = mode_b < 0 ? 1 : 2;
    sim_init(); render_init(); second_init();
    render_set_mode(mode_a);
    if (panels == 2) second_set_mode(mode_b);
    static unsigned char rgb[LCD_V_RES][2 * LCD_H_RES][3];
    for (int f = 0; f < frames; ++f) {
        const float t = atof(argv[4 + f]);  // scenario seconds of this frame
        sim_forces_t forces;
        // Advance from the previous simulation time; start with one second of settling.
        static float sim_t = -1.0f;
        if (t < sim_t) sim_t = t - 1.0f;
        for (; sim_t < t; sim_t += 1.0f / SIM_HZ) {
            scenario_forces(sim_t < 0 ? 0 : sim_t, &forces);
            sim_step(1.0f / SIM_HZ, &forces);
        }
        host_time_us = (int64_t)(t * 1e6);
        render_set_gravity(forces.gravity);
        target = 0; render_frame(false);
        if (panels == 2) { second_set_gravity(forces.gravity); target = 1; second_frame(false); }
        for (int p = 0; p < panels; ++p)
            for (int y = 0; y < LCD_V_RES; ++y) for (int x = 0; x < LCD_H_RES; ++x) {
                const uint16_t raw = screen[p][y][x], c = (uint16_t)(raw >> 8 | raw << 8);
                unsigned char *o = rgb[y][p * LCD_H_RES + x];
                o[0] = ((c >> 11) & 31) * 255 / 31; o[1] = ((c >> 5) & 63) * 255 / 63; o[2] = (c & 31) * 255 / 31;
            }
        if (strchr(argv[4 + f], '!')) for (int y = 0; y < LCD_V_RES; ++y) fwrite(rgb[y], 3, panels * LCD_H_RES, stdout);
    }
    return 0;
}
'''

parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument("out", type=Path)
parser.add_argument("--modes", default="liquid,water", help="one or two of " + ", ".join(MODES))
parser.add_argument("--at", help="comma-separated scenario seconds for PNG stills")
parser.add_argument("--seconds", type=float, default=24.0, help="video length, from scenario time 0")
args = parser.parse_args()
modes = [MODES.index(m) for m in args.modes.split(",")]
stills = [float(t) for t in args.at.split(",")] if args.at else []
# Render preceding frames for still images so stateful effects advance along the same timeline.
end = max(stills) if stills else args.seconds
times = [f / FPS for f in range(int(end * FPS) + 1)]
marks = {min(range(len(times)), key=lambda i: abs(times[i] - t)) for t in stills} if stills else set(range(len(times)))

with tempfile.TemporaryDirectory() as tmp:
    path = Path(tmp)
    for name, text in headers.items():
        file = path / name
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_text("#pragma once\n" + text)
    (path / "harness.c").write_text(f"#define SIM_HZ {SIM_HZ}\n" + harness)
    (path / "second.c").write_text("".join(f"#define render_{n} second_{n}\n" for n in ("init", "set_mode", "set_gravity", "frame"))
                                   + f'#include "{root / "main/render.c"}"\n')
    exe = path / "preview"
    subprocess.run(["cc", "-std=c11", "-O2", "-I" + tmp, "-I" + str(root / "main"), str(path / "harness.c"),
                    str(root / "main/sim.c"), str(root / "main/render.c"), str(root / "main/scenario.c"),
                    str(path / "second.c"), "-lm", "-o", str(exe)], check=True)
    width = 480 * len(modes)
    mode_args = [str(modes[0]), str(modes[1] if len(modes) > 1 else -1), str(len(times))]
    mode_args += [f"{t}{'!' if i in marks else ''}" for i, t in enumerate(times)]
    frames = subprocess.run([str(exe)] + mode_args, check=True, stdout=subprocess.PIPE).stdout
    raw = ["-f", "rawvideo", "-pix_fmt", "rgb24", "-s", f"{width}x480"]
    if args.at:
        args.out.mkdir(parents=True, exist_ok=True)
        size = width * 480 * 3
        for i, t in enumerate(stills):
            subprocess.run(["ffmpeg", "-v", "error", "-y", *raw, "-i", "-", str(args.out / f"t{t:05.1f}.png")],
                           input=frames[i * size:(i + 1) * size], check=True)
    else:
        subprocess.run(["ffmpeg", "-v", "error", "-y", *raw, "-r", str(FPS), "-i", "-", "-c:v", "libx264",
                        "-pix_fmt", "yuv420p", "-crf", "18", str(args.out)], input=frames, check=True)
    print(args.out)
