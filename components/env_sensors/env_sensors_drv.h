#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 感測器硬體與匯流排位址定義 */
#define ENV_DS3231_I2C_ADDR     0x68
#define ENV_GY30_I2C_ADDR       0x23
#define ENV_DHT22_GPIO_NUM      GPIO_NUM_14

/* DS3231 RTC 驅動介面 (T-04-01) */
esp_err_t env_ds3231_init(void);
esp_err_t env_ds3231_get_time(struct tm *out_tm);
esp_err_t env_ds3231_set_time(const struct tm *in_tm);
esp_err_t env_ds3231_sync_to_system(void);

/* GY-30 (BH1750) 光照感測器驅動介面 (T-04-01) */
esp_err_t env_gy30_init(void);
esp_err_t env_gy30_read_lux(uint16_t *out_lux);

/* DHT22 單匯流排溫濕度驅動介面 (T-04-02) */
esp_err_t env_dht22_init(void);
esp_err_t env_dht22_read(int16_t *out_temp_x10, uint16_t *out_humi_x10);

#ifdef __cplusplus
}
#endif