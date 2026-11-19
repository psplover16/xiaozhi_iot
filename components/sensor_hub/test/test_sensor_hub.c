#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "unity.h"

#include "system_types.h"
#include "board_bsp.h"
#include "env_sensors_drv.h"
#include "sensor_hub.h"

static volatile uint32_t s_test_cb_count = 0;
static sensor_record_t   s_test_last_rec = {0};

static void test_on_sensor_record(const sensor_record_t *rec)
{
    if (rec != NULL) {
        s_test_last_rec = *rec;
        s_test_cb_count++;
    }
}

TEST_CASE("sensor_hub_get_date_yyyymmdd converts unix timestamp accurately", "[sensor_hub][phase4]")
{
    setenv("TZ", "CST-8", 1);
    tzset();

    /* 2026-11-19 00:00:00 CST (UTC+8) = 2026-11-18 16:00:00 UTC = 1795017600 */
    TEST_ASSERT_EQUAL_UINT32(20261119U, sensor_hub_get_date_yyyymmdd(1795017600U));

    /* 2024-02-29 12:00:00 CST (閏年測試) = 1709179200 */
    TEST_ASSERT_EQUAL_UINT32(20240229U, sensor_hub_get_date_yyyymmdd(1709179200U));

    /* 2026-01-01 00:30:00 CST (跨年邊界測試) = 1767198600 */
    TEST_ASSERT_EQUAL_UINT32(20260101U, sensor_hub_get_date_yyyymmdd(1767198600U));
}

TEST_CASE("env_sensors drivers validate NULL arguments safely", "[env_sensors][phase4]")
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, env_ds3231_get_time(NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, env_ds3231_set_time(NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, env_gy30_read_lux(NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, env_dht22_read(NULL, NULL));
}

TEST_CASE("sensor_hub init and Core 1 polling task produce valid CRC16 records", "[sensor_hub][phase4]")
{
    s_test_cb_count = 0;
    memset(&s_test_last_rec, 0, sizeof(s_test_last_rec));

    TEST_ASSERT_EQUAL(ESP_OK, sensor_hub_init(test_on_sensor_record));
    TEST_ASSERT_EQUAL(ESP_OK, sensor_hub_start_task());

    /* 等待至少一個 2 秒採集週期完成 */
    vTaskDelay(pdMS_TO_TICKS(2300));

    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(1U, s_test_cb_count);

    /* 驗證 16-Byte 紀錄的 CRC16 校驗碼 */
    uint16_t expected_crc = board_calc_crc16((const uint8_t *)&s_test_last_rec,
                                             offsetof(sensor_record_t, crc16));
    TEST_ASSERT_EQUAL_UINT16(expected_crc, s_test_last_rec.crc16);

    /* 驗證 DHT22 旗標與降級數值一致性 */
    if ((s_test_last_rec.status_flags & REC_FLAG_DHT22_VALID) == 0U) {
        TEST_ASSERT_EQUAL_INT16(INVALID_TEMP_VALUE, s_test_last_rec.temperature);
        TEST_ASSERT_EQUAL_UINT16(INVALID_HUMI_VALUE, s_test_last_rec.humidity);
    } else {
        TEST_ASSERT_GREATER_OR_EQUAL_INT16(-400, s_test_last_rec.temperature);
        TEST_ASSERT_LESS_OR_EQUAL_INT16(800, s_test_last_rec.temperature);
        TEST_ASSERT_LESS_OR_EQUAL_UINT16(1000U, s_test_last_rec.humidity);
    }
}