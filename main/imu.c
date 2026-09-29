#include "imu.h"

#include <math.h>
#include <string.h>

#include "config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bmi270.h"
#include "esp_rom_sys.h"

#define DEG_TO_RAD 0.01745329252f
#define TWO_PI 6.28318530718f
static const char *TAG = "imu";
static struct bmi2_dev s_dev;
static i2c_master_dev_handle_t s_i2c;
static bool s_ready;

static float s_lp[3];  // Low-pass acceleration estimate in simulation axes, m/s^2.
static bool s_lp_primed;
static float s_prev_omega[3];


static BMI2_INTF_RETURN_TYPE sensor_read(uint8_t reg, uint8_t *data, uint32_t len, void *ctx)
{
    return i2c_master_transmit_receive(s_i2c, &reg, 1, data, len, 100) == ESP_OK ? 0 : -1;
}

static BMI2_INTF_RETURN_TYPE sensor_write(uint8_t reg, const uint8_t *data, uint32_t len, void *ctx)
{
    uint8_t payload[33];
    if (len > sizeof(payload) - 1) return -1;
    payload[0] = reg;
    memcpy(payload + 1, data, len);
    return i2c_master_transmit(s_i2c, payload, len + 1, 100) == ESP_OK ? 0 : -1;
}

static void sensor_delay(uint32_t us, void *ctx)
{
    esp_rom_delay_us(us);
}

esp_err_t imu_init(i2c_master_bus_handle_t bus)
{
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x69, .scl_speed_hz = 400000,
    };
    esp_err_t ret = i2c_master_bus_add_device(bus, &cfg, &s_i2c);
    if (ret != ESP_OK) return ret;
    s_dev = (struct bmi2_dev){
        .intf = BMI2_I2C_INTF, .read = sensor_read, .write = sensor_write,
        .delay_us = sensor_delay, .read_write_len = 32, .intf_ptr = &s_i2c,
    };
    if (bmi270_init(&s_dev) != BMI2_OK || bmi2_set_adv_power_save(BMI2_DISABLE, &s_dev) != BMI2_OK) goto fail;
    struct bmi2_sens_config conf[2] = {{.type = BMI2_ACCEL}, {.type = BMI2_GYRO}};
    if (bmi2_get_sensor_config(conf, 2, &s_dev) != BMI2_OK) goto fail;
    conf[0].cfg.acc.odr = BMI2_ACC_ODR_200HZ;
    conf[0].cfg.acc.range = BMI2_ACC_RANGE_8G;
    conf[0].cfg.acc.bwp = BMI2_ACC_NORMAL_AVG4;
    conf[0].cfg.acc.filter_perf = BMI2_PERF_OPT_MODE;
    conf[1].cfg.gyr.odr = BMI2_GYR_ODR_200HZ;
    conf[1].cfg.gyr.range = BMI2_GYR_RANGE_500;
    conf[1].cfg.gyr.bwp = BMI2_GYR_NORMAL_MODE;
    conf[1].cfg.gyr.noise_perf = BMI2_PERF_OPT_MODE;
    conf[1].cfg.gyr.filter_perf = BMI2_PERF_OPT_MODE;
    uint8_t sensors[] = {BMI2_ACCEL, BMI2_GYRO};
    if (bmi2_set_sensor_config(conf, 2, &s_dev) != BMI2_OK ||
        bmi270_sensor_enable(sensors, 2, &s_dev) != BMI2_OK) goto fail;
    s_ready = true;
    ESP_LOGI(TAG, "BMI270 ready: 200 Hz, 8g, 500 dps");
    return ESP_OK;
fail:
    i2c_master_bus_rm_device(s_i2c);
    s_i2c = NULL;
    return ESP_FAIL;
}

bool imu_read(float dt, sim_forces_t *out)
{
    if (!s_ready) {
        return false;
    }

    struct bmi2_sens_data data;
    if (bmi2_get_sensor_data(&data, &s_dev) != BMI2_OK) return false;
    if ((data.status & (BMI2_DRDY_ACC | BMI2_DRDY_GYR)) != (BMI2_DRDY_ACC | BMI2_DRDY_GYR)) return false;
    const float accelX = data.acc.x * (8.0f * GRAVITY_MPS2 / 32768.0f);
    const float accelY = data.acc.y * (8.0f * GRAVITY_MPS2 / 32768.0f);
    const float accelZ = data.acc.z * (8.0f * GRAVITY_MPS2 / 32768.0f);
    const float gyroX = data.gyr.x * (500.0f / 32768.0f);
    const float gyroY = data.gyr.y * (500.0f / 32768.0f);
    const float gyroZ = data.gyr.z * (500.0f / 32768.0f);

    // Move into simulation axes: x right, y down the screen, z into the case.
    const float ax = IMU_MAP_X(accelX, accelY, accelZ);
    const float ay = IMU_MAP_Y(accelX, accelY, accelZ);
    const float az = IMU_MAP_Z(accelX, accelY, accelZ);

    const float gx = IMU_MAP_X(gyroX, gyroY, gyroZ) * DEG_TO_RAD;
    const float gy = IMU_MAP_Y(gyroX, gyroY, gyroZ) * DEG_TO_RAD;
    const float gz = IMU_MAP_Z(gyroX, gyroY, gyroZ) * DEG_TO_RAD;

    if (!s_lp_primed || dt > 0.25f) {
        s_prev_omega[0] = gx; s_prev_omega[1] = gy; s_prev_omega[2] = gz;
        s_lp[0] = ax;
        s_lp[1] = ay;
        s_lp[2] = az;
        s_lp_primed = true;
    } else {
        // Derive the one-pole filter coefficient from elapsed time to maintain
        // the configured cutoff frequency across changes in sample rate.
        const float k = 1.0f - expf(-TWO_PI * GRAVITY_LP_HZ * dt);
        s_lp[0] += k * (ax - s_lp[0]);
        s_lp[1] += k * (ay - s_lp[1]);
        s_lp[2] += k * (az - s_lp[2]);
    }

    // Negate the low-pass acceleration to estimate the gravity direction.
    float dx = -s_lp[0], dy = -s_lp[1], dz = -s_lp[2];
    const float mag = sqrtf(dx * dx + dy * dy + dz * dz);
    if (mag > 0.5f) {
        dx /= mag;
        dy /= mag;
        dz /= mag;
    } else {
        // Use screen-down gravity when the acceleration magnitude is too small
        // to provide a reliable direction, including during free fall.
        dx = 0.0f;
        dy = 1.0f;
        dz = 0.0f;
    }

    const float g_px = GRAVITY_GAIN * GRAVITY_MPS2 * PX_PER_METER;

    // Apply the residual device acceleration as an opposing inertial
    // acceleration in the device reference frame.
    const float shake_x = ax - s_lp[0];
    const float shake_y = ay - s_lp[1];
    const float shake_z = az - s_lp[2];
    const float shake_px = SHAKE_GAIN * PX_PER_METER;

    out->gravity[0] = dx * g_px - shake_x * shake_px;
    out->gravity[1] = dy * g_px - shake_y * shake_px;
    out->gravity[2] = dz * g_px - shake_z * shake_px;

    out->omega[0] = gx;
    out->omega[1] = gy;
    out->omega[2] = gz;

    const float inv_dt = (dt > 1e-6f) ? (1.0f / dt) : 0.0f;
    out->alpha[0] = (gx - s_prev_omega[0]) * inv_dt;
    out->alpha[1] = (gy - s_prev_omega[1]) * inv_dt;
    out->alpha[2] = (gz - s_prev_omega[2]) * inv_dt;

    s_prev_omega[0] = gx;
    s_prev_omega[1] = gy;
    s_prev_omega[2] = gz;

    return true;
}
