idf_component_register(
    SRCS "psram_cache.c"
    INCLUDE_DIRS "include"
    REQUIRES board_config freertos esp_psram
)