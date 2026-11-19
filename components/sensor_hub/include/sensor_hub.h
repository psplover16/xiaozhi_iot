#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "system_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*sensor_new_record_cb)(const sensor_record_t *rec);

esp_err_t sensor_hub_init(sensor_new_record_cb on_record_cb);
esp_err_t sensor_hub_start_task(void); // Pin to Core 1
esp_err_t sensor_hub_sync_ntp(void);   // Mode 1 聯網後透過 esp_sntp.h 校準 DS3231
uint32_t  sensor_hub_get_date_yyyymmdd(uint32_t unix_ts);

#ifdef __cplusplus
}
#endif
