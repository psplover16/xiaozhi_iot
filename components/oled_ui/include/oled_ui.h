#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "system_types.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t oled_ui_init(void);
void      oled_ui_update_sensor(const sensor_record_t *rec, const env_stats_t *stats);
void      oled_ui_set_alert_flags(uint8_t alert_flags); // 置頂顯示於 Page 0~1
void      oled_ui_set_net_status(net_mode_t mode, const char *ip_or_ssid);
void      oled_ui_adjust_contrast_by_lux(uint32_t lux);

#ifdef __cplusplus
}
#endif
