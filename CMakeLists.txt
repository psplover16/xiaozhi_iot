cmake_minimum_required(VERSION 3.16)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(TEST_COMPONENTS
    "board_config"
    "psram_cache"
    "sd_media"
    "eeprom_blackbox"
    "sensor_hub"
    "oled_ui"
    "audio_engine"
    "net_manager"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(xiaozhi_iot)
