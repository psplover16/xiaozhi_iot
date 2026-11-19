idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES
                           board_bsp
                           psram_cache
                           eeprom_blackbox
                           sd_media
                           env_sensors
                           sensor_hub
                           oled_ui
                           audio_engine
                           unity
                           nvs_flash
                           esp_netif
                           esp_event
                           esp_psram
                       WHOLE_ARCHIVE)