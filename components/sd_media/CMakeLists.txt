idf_component_register(
    SRCS "sd_media.c"
    INCLUDE_DIRS "include"
    REQUIRES board_config board_bsp fatfs sdmmc driver esp_driver_i2s freertos heap
)