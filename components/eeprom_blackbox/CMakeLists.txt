idf_component_register(
    SRCS "eeprom_blackbox.c"
    INCLUDE_DIRS "include"
    REQUIRES board_config board_bsp freertos heap
)