#include <string.h>
#include "unity.h"
#include "esp_heap_caps.h"
#include "psram_cache.h"

TEST_CASE("psram_cache: init allocates buffer in external SPIRAM", "[psram_cache]")
{
    // 驗證非法參數擋回
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, psram_cache_init(0U));

    // 分配 32,768 筆 * 16 Bytes = 524,288 Bytes (512 KB)
    const uint32_t target_records = 32768U;
    const size_t expected_bytes   = (size_t)target_records * sizeof(sensor_record_t);

    size_t spiram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    TEST_ASSERT_EQUAL(ESP_OK, psram_cache_init(target_records));
    size_t spiram_after  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    // 確認確實從外部 PSRAM 扣除記憶體，而非內部 SRAM
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32((uint32_t)expected_bytes,
                                        (uint32_t)(spiram_before - spiram_after));
}

TEST_CASE("psram_cache: ring wrap-around overwrite and window stats", "[psram_cache]")
{
    // 建立容量僅 4 筆的環形緩衝區，測試寫滿覆寫行為
    TEST_ASSERT_EQUAL(ESP_OK, psram_cache_init(4U));

    // 依序寫入 6 筆資料 (ts = 100 ~ 600)，最舊的 100 與 200 應被覆寫
    for (uint32_t i = 0U; i < 6U; i++) {
        sensor_record_t rec = {
            .timestamp    = (i + 1U) * 100U,
            .temperature  = (int16_t)(180 + (int16_t)(i * 20)), // 18.0, 20.0, 22.0, 24.0, 26.0, 28.0 C
            .humidity     = (uint16_t)(450U + i * 50U),         // 45.0% ~ 70.0%
            .lux          = (i + 1U) * 100U,                    // 100 ~ 600 lux
            .status_flags = REC_FLAG_DHT22_VALID | REC_FLAG_GY30_VALID | REC_FLAG_RTC_VALID,
            .crc16        = 0U,
        };
        TEST_ASSERT_EQUAL(ESP_OK, psram_cache_push(&rec));
    }

    // 1. 查詢全視窗 (window_seconds = 0)，應只剩最後 4 筆 (ts=300, 400, 500, 600)
    env_stats_t stats_all;
    TEST_ASSERT_EQUAL(ESP_OK, psram_cache_get_stats(0U, 0U, &stats_all));

    TEST_ASSERT_EQUAL_UINT32(4U, stats_all.valid_count);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 22.0f, stats_all.temp_min);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 28.0f, stats_all.temp_max);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 25.0f, stats_all.temp_avg);
    TEST_ASSERT_EQUAL_INT8(1, stats_all.temp_trend); // 溫度持續上升，趨勢應為 1

    TEST_ASSERT_EQUAL_UINT32(300U, stats_all.lux_min);
    TEST_ASSERT_EQUAL_UINT32(600U, stats_all.lux_max);
    TEST_ASSERT_EQUAL_UINT32(450U, stats_all.lux_avg);

    // 2. 查詢最近 150 秒視窗 (now_ts = 600 -> 只包含 ts=500 與 ts=600)
    env_stats_t stats_win;
    TEST_ASSERT_EQUAL(ESP_OK, psram_cache_get_stats(150U, 600U, &stats_win));
    TEST_ASSERT_EQUAL_UINT32(2U, stats_win.valid_count);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 26.0f, stats_win.temp_min);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 28.0f, stats_win.temp_max);
}

TEST_CASE("psram_cache: invalid sensor flags and summary formatting", "[psram_cache]")
{
    TEST_ASSERT_EQUAL(ESP_OK, psram_cache_init(4U));

    // 寫入一筆感測器無效的紀錄
    sensor_record_t bad_rec = {
        .timestamp    = 1000U,
        .temperature  = INVALID_TEMP_VALUE,
        .humidity     = INVALID_HUMI_VALUE,
        .lux          = 999U,
        .status_flags = 0U, // DHT22 與 GY30 皆未標記 VALID
        .crc16        = 0U,
    };
    TEST_ASSERT_EQUAL(ESP_OK, psram_cache_push(&bad_rec));

    env_stats_t stats;
    TEST_ASSERT_EQUAL(ESP_OK, psram_cache_get_stats(0U, 0U, &stats));
    TEST_ASSERT_EQUAL_UINT32(1U, stats.valid_count);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, stats.temp_avg);
    TEST_ASSERT_EQUAL_UINT32(0U, stats.lux_avg);

    char summary[160];
    size_t len = psram_cache_format_summary(&stats, summary, sizeof(summary));
    TEST_ASSERT_GREATER_THAN_UINT32(0U, (uint32_t)len);
    TEST_ASSERT_NOT_NULL(strstr(summary, "Samples:1"));
}