idf_component_register(
    SRCS "test_eeprom_blackbox.c"
    INCLUDE_DIRS "."
    REQUIRES unity eeprom_blackbox board_config
)