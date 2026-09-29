#pragma once

#include <stdint.h>

#include "esp_err.h"

// Owns the panel and two PSRAM frame slots. The renderer stages a frame band by
// band and presents it; a display task schedules transfers using the panel's TE
// signal while the next frame is drawn. Only the renderer stages or presents.

esp_err_t display_init(void);

// Copies a packed [x_start, x_end) window of one band into the frame being staged.
// Columns must be 16-pixel aligned; each band may be staged at most once per frame.
void display_stage_band(int band_index, int x_start, int x_end, const uint16_t *pixels);

// Queues a nonempty frame and waits for the other slot to become available.
// An empty frame retains its slot. Queued frames are not dropped because cropped
// windows also erase pixels from the previous frame. Transfers follow the estimated
// panel scan; without a TE edge for 25 ms, the sender proceeds unsynchronized.
void display_present(void);

// Accumulated counters since the previous read; display_take_stats() resets them.
// scan_wait_us includes initial TE and per-band waits. transfer_us is the remaining
// sender elapsed time, including driver overhead. underruns counts transfer callbacks
// that observed an empty DMA output FIFO; it may include a frame still being sent.
typedef struct { uint32_t frames, scan_wait_us, transfer_us, te_timeouts, underruns; } display_stats_t;
display_stats_t display_take_stats(void);

// Sets AMOLED brightness through panel register 0x51, using a level from 0 to 255.
esp_err_t display_set_brightness(uint8_t level);
