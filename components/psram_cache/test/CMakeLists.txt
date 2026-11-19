idf_component_register(
    SRCS "test_psram_cache.c"
    INCLUDE_DIRS "."
    REQUIRES unity psram_cache board_config heap
)