#include "board.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "esp_timer.h"

static const char *TAG = "board";
static bool s_new_board;

int board_lcd_clock_pin(void) { return s_new_board ? 42 : 44; }
int board_lcd_reset_pin(void) { return s_new_board ? 44 : 42; }

esp_err_t board_init(i2c_master_bus_handle_t *bus)
{
    uint16_t version = 0;
    ESP_RETURN_ON_ERROR(esp_efuse_read_field_blob(ESP_EFUSE_USER_DATA, &version, 16), TAG, "read hardware revision");
    ESP_RETURN_ON_FALSE(version == 0x0100 || version == 0x0101 || version == 0x0102,
                        ESP_ERR_NOT_SUPPORTED, TAG, "unsupported board revision 0x%04x", version);
    s_new_board = version != 0x0100;
    ESP_LOGI(TAG, "Mosaico v%u.%u", version >> 8, version & 255);
    const i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0, .sda_io_num = s_new_board ? 56 : 0,
        .scl_io_num = s_new_board ? 3 : 1, .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&cfg, bus), TAG, "create I2C bus");
    const gpio_config_t button = {
        .pin_bit_mask = (1ULL << 61) | (1ULL << 7), .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    return gpio_config(&button);
}

static bool take_press(int pin, int index)
{
    static bool raw_last[2], stable[2];
    static int64_t changed[2];
    const bool raw = gpio_get_level(pin) == 0;
    const int64_t now = esp_timer_get_time();
    if (raw != raw_last[index]) { raw_last[index] = raw; changed[index] = now; }
    if (raw != stable[index] && now - changed[index] >= 30000) {
        stable[index] = raw;
        return stable[index];
    }
    return false;
}

bool board_take_reset(void) { return take_press(61, 0); }
bool board_take_wave_toggle(void) { return take_press(7, 1); }
