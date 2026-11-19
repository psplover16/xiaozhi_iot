#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "system_types.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t psram_cache_init(uint32_t max_capacity_records); // 配置於 MALLOC_CAP_SPIRAM
esp_err_t psram_cache_push(const sensor_record_t *rec);
esp_err_t psram_cache_get_stats(uint32_t window_seconds, uint32_t now_ts, env_stats_t *out_stats);
size_t    psram_cache_format_summary(const env_stats_t *stats, char *out_buf, size_t buf_len);

#ifdef __cplusplus
}
#endif
