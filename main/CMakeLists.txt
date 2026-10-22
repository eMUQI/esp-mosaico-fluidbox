idf_component_register(SRCS "main.c" "board.c" "display.c" "imu.c" "sim.c" "render.c"
    INCLUDE_DIRS "." REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_spi esp_lcd esp_timer efuse esp_mosaico_app_recovery)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
