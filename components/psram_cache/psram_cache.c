#include "psram_cache.h"

#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <float.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "psram_cache";

typedef struct {
    sensor_record_t  *buffer;
    uint32_t          capacity;
    uint32_t          head;
    uint32_t          count;
    SemaphoreHandle_t mutex;
} psram_ring_ctx_t;

static psram_ring_ctx_t s_ctx = {
    .buffer   = NULL,
    .capacity = 0,
    .head     = 0,
    .count    = 0,
    .mutex    = NULL,
};

esp_err_t psram_cache_init(uint32_t max_capacity_records)
{
    if (max_capacity_records == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_ctx.mutex == NULL) {
        s_ctx.mutex = xSemaphoreCreateMutex();
        if (s_ctx.mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create PSRAM cache mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);

    if (s_ctx.buffer != NULL) {
        heap_caps_free(s_ctx.buffer);
        s_ctx.buffer   = NULL;
        s_ctx.capacity = 0;
        s_ctx.head     = 0;
        s_ctx.count    = 0;
    }

    size_t alloc_bytes = (size_t)max_capacity_records * sizeof(sensor_record_t);
    sensor_record_t *buf = (sensor_record_t *)heap_caps_malloc(
        alloc_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (buf == NULL) {
        xSemaphoreGive(s_ctx.mutex);
        ESP_LOGE(TAG, "Failed to allocate %zu bytes in SPIRAM", alloc_bytes);
        return ESP_ERR_NO_MEM;
    }

    memset(buf, 0, alloc_bytes);
    s_ctx.buffer   = buf;
    s_ctx.capacity = max_capacity_records;
    s_ctx.head     = 0;
    s_ctx.count    = 0;

    xSemaphoreGive(s_ctx.mutex);

    ESP_LOGI(TAG, "Initialized PSRAM ring cache: %" PRIu32 " records (%zu bytes)",
             max_capacity_records, alloc_bytes);
    return ESP_OK;
}

esp_err_t psram_cache_push(const sensor_record_t *rec)
{
    if (rec == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ctx.buffer == NULL || s_ctx.mutex == NULL || s_ctx.capacity == 0U) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);

    s_ctx.buffer[s_ctx.head] = *rec;
    s_ctx.head = (s_ctx.head + 1U) % s_ctx.capacity;

    if (s_ctx.count < s_ctx.capacity) {
        s_ctx.count++;
    }

    xSemaphoreGive(s_ctx.mutex);
    return ESP_OK;
}

esp_err_t psram_cache_get_stats(uint32_t window_seconds, uint32_t now_ts, env_stats_t *out_stats)
{
    if (out_stats == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ctx.buffer == NULL || s_ctx.mutex == NULL || s_ctx.capacity == 0U) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(out_stats, 0, sizeof(env_stats_t));

    xSemaphoreTake(s_ctx.mutex, portMAX_DELAY);

    if (s_ctx.count == 0U) {
        xSemaphoreGive(s_ctx.mutex);
        return ESP_OK;
    }

    uint32_t min_ts = (now_ts > window_seconds) ? (now_ts - window_seconds) : 0U;
    uint32_t oldest_idx = (s_ctx.head + s_ctx.capacity - s_ctx.count) % s_ctx.capacity;

    double   temp_sum = 0.0;
    float    temp_min = FLT_MAX;
    float    temp_max = -FLT_MAX;
    uint32_t temp_cnt = 0U;

    double   humi_sum = 0.0;
    float    humi_min = FLT_MAX;
    float    humi_max = -FLT_MAX;
    uint32_t humi_cnt = 0U;

    uint64_t lux_sum  = 0ULL;
    uint32_t lux_min  = UINT32_MAX;
    uint32_t lux_max  = 0U;
    uint32_t lux_cnt  = 0U;

    // 記錄視窗內前半段與後半段溫度總和，用於計算溫度變化趨勢
    double   first_half_temp_sum  = 0.0;
    uint32_t first_half_temp_cnt  = 0U;
    double   second_half_temp_sum = 0.0;
    uint32_t second_half_temp_cnt = 0U;

    // 先找出視窗起始索引，計算視窗內總筆數以切分前後半段
    uint32_t start_offset = s_ctx.count;
    for (uint32_t i = 0U; i < s_ctx.count; i++) {
        uint32_t idx = (oldest_idx + i) % s_ctx.capacity;
        uint32_t ts  = s_ctx.buffer[idx].timestamp;
        if (window_seconds == 0U || (ts >= min_ts && (now_ts == 0U || ts <= now_ts))) {
            start_offset = i;
            break;
        }
    }

    uint32_t window_total = s_ctx.count - start_offset;
    uint32_t half_point   = window_total / 2U;

    for (uint32_t i = start_offset; i < s_ctx.count; i++) {
        uint32_t idx = (oldest_idx + i) % s_ctx.capacity;
        const sensor_record_t *r = &s_ctx.buffer[idx];

        if (window_seconds > 0U) {
            if (r->timestamp < min_ts || (now_ts > 0U && r->timestamp > now_ts)) {
                continue;
            }
        }

        out_stats->valid_count++;
        uint32_t rel_pos = i - start_offset;

        if ((r->status_flags & REC_FLAG_DHT22_VALID) != 0U) {
            if (r->temperature != INVALID_TEMP_VALUE) {
                float t = (float)r->temperature / 10.0f;
                temp_sum += (double)t;
                if (t < temp_min) {
                    temp_min = t;
                }
                if (t > temp_max) {
                    temp_max = t;
                }
                temp_cnt++;

                if (rel_pos < half_point) {
                    first_half_temp_sum += (double)t;
                    first_half_temp_cnt++;
                } else {
                    second_half_temp_sum += (double)t;
                    second_half_temp_cnt++;
                }
            }

            if (r->humidity != INVALID_HUMI_VALUE) {
                float h = (float)r->humidity / 10.0f;
                humi_sum += (double)h;
                if (h < humi_min) {
                    humi_min = h;
                }
                if (h > humi_max) {
                    humi_max = h;
                }
                humi_cnt++;
            }
        }

        if ((r->status_flags & REC_FLAG_GY30_VALID) != 0U) {
            lux_sum += (uint64_t)r->lux;
            if (r->lux < lux_min) {
                lux_min = r->lux;
            }
            if (r->lux > lux_max) {
                lux_max = r->lux;
            }
            lux_cnt++;
        }
    }

    xSemaphoreGive(s_ctx.mutex);

    if (temp_cnt > 0U) {
        out_stats->temp_avg = (float)(temp_sum / (double)temp_cnt);
        out_stats->temp_min = temp_min;
        out_stats->temp_max = temp_max;

        if (first_half_temp_cnt > 0U && second_half_temp_cnt > 0U) {
            double avg1 = first_half_temp_sum / (double)first_half_temp_cnt;
            double avg2 = second_half_temp_sum / (double)second_half_temp_cnt;
            double diff = avg2 - avg1;
            if (diff > 0.3) {
                out_stats->temp_trend = 1;
            } else if (diff < -0.3) {
                out_stats->temp_trend = -1;
            } else {
                out_stats->temp_trend = 0;
            }
        }
    }

    if (humi_cnt > 0U) {
        out_stats->humi_avg = (float)(humi_sum / (double)humi_cnt);
        out_stats->humi_min = humi_min;
        out_stats->humi_max = humi_max;
    }

    if (lux_cnt > 0U) {
        out_stats->lux_avg = (uint32_t)(lux_sum / (uint64_t)lux_cnt);
        out_stats->lux_min = lux_min;
        out_stats->lux_max = lux_max;
    }

    return ESP_OK;
}

size_t psram_cache_format_summary(const env_stats_t *stats, char *out_buf, size_t buf_len)
{
    if (stats == NULL || out_buf == NULL || buf_len == 0U) {
        return 0U;
    }

    int written = 0;
    if (stats->valid_count == 0U) {
        written = snprintf(out_buf, buf_len, "No sensor records in window.");
    } else {
        const char *trend_str = "flat";
        if (stats->temp_trend > 0) {
            trend_str = "rising";
        } else if (stats->temp_trend < 0) {
            trend_str = "falling";
        }

        written = snprintf(
            out_buf,
            buf_len,
            "Samples:%" PRIu32 " | Temp:%.1fC(%.1f~%.1f,%s) | Humi:%.1f%%(%.1f~%.1f) | Lux:%" PRIu32 "(%" PRIu32 "~%" PRIu32 ")",
            stats->valid_count,
            (double)stats->temp_avg, (double)stats->temp_min, (double)stats->temp_max, trend_str,
            (double)stats->humi_avg, (double)stats->humi_min, (double)stats->humi_max,
            stats->lux_avg, stats->lux_min, stats->lux_max
        );
    }

    if (written < 0) {
        out_buf[0] = '\0';
        return 0U;
    }
    return ((size_t)written >= buf_len) ? (buf_len - 1U) : (size_t)written;
}