#pragma once

#include <stdbool.h>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "sim.h"

// Converts BMI270 samples into simulation forces. Low-pass acceleration estimates
// the direction opposite gravity; the residual estimates device acceleration.

esp_err_t imu_init(i2c_master_bus_handle_t bus);

// Fills gravity, angular velocity and angular acceleration using the elapsed
// time dt in seconds since the previous successful sample. Returns false and
// leaves out unchanged if the sensor is unavailable, a read fails, or data is stale.
bool imu_read(float dt, sim_forces_t *out);
