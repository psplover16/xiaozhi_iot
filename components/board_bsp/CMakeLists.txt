idf_component_register(SRCS "board_bsp.c" "test/test_board_bsp.c"
                       INCLUDE_DIRS "include" "../board_config/include"
                       REQUIRES driver esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_driver_spi esp_driver_ledc esp_psram unity
                       WHOLE_ARCHIVE)
