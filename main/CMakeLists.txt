idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES board_bsp psram_cache unity nvs_flash esp_netif esp_event esp_psram
                       WHOLE_ARCHIVE)
