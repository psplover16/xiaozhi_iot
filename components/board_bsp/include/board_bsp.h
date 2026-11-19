#pragma once

#include "../../board_config/include/board_config.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t board_bsp_init(void);
bool      board_btn_is_pressed(void);
esp_err_t board_set_backlight(uint8_t duty);

#ifdef __cplusplus
}
#endif
