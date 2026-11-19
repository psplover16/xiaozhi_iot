#include <string.h>
#include "unity.h"
#include "eeprom_blackbox.h"
#include "system_types.h"

// 模擬 32KB AT24C256 實體記憶體陣列
static uint8_t  s_mock_eeprom[EEPROM_TOTAL_BYTES];
static bool     s_page_wrap_violated = false;
static uint32_t s_write_call_count   = 0U;

// 模擬 flush callback 狀態
static bool     s_flush_should_succeed = true;
static uint32_t s_flushed_record_count = 0U;
static uint32_t s_flushed_first_ts     = 0U;
static uint32_t s_flushed_last_ts      = 0U;

static void reset_mock_eeprom(void)
{
    memset(s_mock_eeprom, 0xFF, sizeof(s_mock_eeprom));
    s_page_wrap_violated   = false;
    s_write_call_count     = 0U;
    s_flush_should_succeed = true;
    s_flushed_record_count = 0U;
    s_flushed_first_ts     = 0U;
    s_flushed_last_ts      = 0U;
}

static esp_err_t mock_eeprom_write(uint16_t mem_addr, const uint8_t *data, size_t len)
{
    if ((uint32_t)mem_addr + (uint32_t)len > EEPROM_TOTAL_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }

    s_write_call_count++;

    // 偵測是否違反 AT24C256 64-Byte 頁邊界紅線：若跨頁則模擬實體晶片頁首回捲覆寫錯亂
    uint16_t page_base   = (uint16_t)(mem_addr & ~(EEPROM_PAGE_SIZE - 1U));
    uint16_t page_offset = (uint16_t)(mem_addr % EEPROM_PAGE_SIZE);

    if (len > EEPROM_PAGE_SIZE || (page_offset + len) > EEPROM_PAGE_SIZE) {
        s_page_wrap_violated = true;
        for (size_t i = 0U; i < len; i++) {
            uint16_t wrapped_addr = (uint16_t)(page_base + ((page_offset + i) % EEPROM_PAGE_SIZE));
            s_mock_eeprom[wrapped_addr] = data[i];
        }
        return ESP_OK;
    }

    memcpy(&s_mock_eeprom[mem_addr], data, len);
    return ESP_OK;
}

static esp_err_t mock_eeprom_read(uint16_t mem_addr, uint8_t *data, size_t len)
{
    if ((uint32_t)mem_addr + (uint32_t)len > EEPROM_TOTAL_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(data, &s_mock_eeprom[mem_addr], len);
    return ESP_OK;
}

static esp_err_t mock_overflow_flush_cb(const sensor_record_t *records, size_t count)
{
    if (!s_flush_should_succeed) {
        return ESP_FAIL;
    }
    s_flushed_record_count = (uint32_t)count;
    if (count > 0U && records != NULL) {
        s_flushed_first_ts = records[0].timestamp;
        s_flushed_last_ts  = records[count - 1U].timestamp;
    }
    return ESP_OK;
}

TEST_CASE("eeprom_blackbox: 64-byte page boundary & power-loss config recovery", "[eeprom_blackbox]")
{
    reset_mock_eeprom();

    // 1. 首次開機全 0xFF，應自動載入預設設定且完全不跨越 64-Byte 頁邊界
    TEST_ASSERT_EQUAL(ESP_OK, eeprom_bb_init(mock_eeprom_write, mock_eeprom_read));
    TEST_ASSERT_FALSE(s_page_wrap_violated);
    TEST_ASSERT_EQUAL_UINT16(0U, eeprom_bb_get_record_count());

    sys_config_t cfg;
    TEST_ASSERT_EQUAL(ESP_OK, eeprom_bb_load_config(&cfg));
    TEST_ASSERT_EQUAL_UINT32(SYS_CONFIG_MAGIC, cfg.magic);
    TEST_ASSERT_EQUAL_UINT16(10U, cfg.sample_interval_sec);
    TEST_ASSERT_EQUAL_UINT8(80U, cfg.eeprom_warn_pct);

    // 2. 寫入自訂 Wi-Fi 設定（240 Bytes 橫跨 4 個 64-Byte 頁）
    strncpy(cfg.wifi_ssid, "Xiaozhi_Lab_AP", sizeof(cfg.wifi_ssid) - 1U);
    strncpy(cfg.server_url, "http://192.168.1.100:8080/api", sizeof(cfg.server_url) - 1U);
    cfg.sample_interval_sec = 30U;
    TEST_ASSERT_EQUAL(ESP_OK, eeprom_bb_save_config(&cfg));
    TEST_ASSERT_FALSE(s_page_wrap_violated);

    // 3. 寫入 10 筆感測紀錄（160 Bytes 橫跨 0x0400、0x0440、0x0480 多個頁邊界）
    for (uint16_t i = 0U; i < 10U; i++) {
        sensor_record_t rec = {
            .timestamp    = 1700000000U + i * 30U,
            .temperature  = (int16_t)(245 + i),
            .humidity     = (uint16_t)(600U + i),
            .lux          = 350U + i * 10U,
            .status_flags = REC_FLAG_DHT22_VALID | REC_FLAG_GY30_VALID | REC_FLAG_RTC_VALID,
            .crc16        = 0U,
        };
        uint8_t alert = STORAGE_ALERT_NONE;
        TEST_ASSERT_EQUAL(ESP_OK, eeprom_bb_push_record(&rec, &alert));
        TEST_ASSERT_EQUAL_UINT8(STORAGE_ALERT_NONE, alert);
    }
    TEST_ASSERT_FALSE(s_page_wrap_violated);
    TEST_ASSERT_EQUAL_UINT16(10U, eeprom_bb_get_record_count());

    // 4. 模擬主設定區寫入瞬間斷電毀損（破壞 0x0000 前 16 Bytes），重新初始化應從備份區完整救回
    memset(&s_mock_eeprom[EEPROM_CONFIG_ADDR], 0x00, 16U);
    TEST_ASSERT_EQUAL(ESP_OK, eeprom_bb_init(mock_eeprom_write, mock_eeprom_read));

    sys_config_t recovered_cfg;
    TEST_ASSERT_EQUAL(ESP_OK, eeprom_bb_load_config(&recovered_cfg));
    TEST_ASSERT_EQUAL_STRING("Xiaozhi_Lab_AP", recovered_cfg.wifi_ssid);
    TEST_ASSERT_EQUAL_UINT16(30U, recovered_cfg.sample_interval_sec);
    TEST_ASSERT_EQUAL_UINT16(10U, eeprom_bb_get_record_count());

    // 5. 讀回分區 B 紀錄驗證資料完整性與自動補算的 CRC16
    sensor_record_t read_buf[10];
    TEST_ASSERT_EQUAL(ESP_OK, eeprom_bb_read_records(0U, read_buf, 10U));
    TEST_ASSERT_EQUAL_UINT32(1700000000U, read_buf[0].timestamp);
    TEST_ASSERT_EQUAL_INT16(254, read_buf[9].temperature);
    TEST_ASSERT_NOT_EQUAL_UINT16(0U, read_buf[9].crc16);
}

TEST_CASE("eeprom_blackbox: 80% warning flag & 100% overflow atomic flush", "[eeprom_blackbox]")
{
    reset_mock_eeprom();
    TEST_ASSERT_EQUAL(ESP_OK, eeprom_bb_init(mock_eeprom_write, mock_eeprom_read));
    eeprom_bb_register_flush_cb(mock_overflow_flush_cb);

    // 1. 寫滿 1984 筆（EEPROM_MAX_RECORDS），並驗證 80% 警戒位元觸發
    const uint16_t warn_threshold = (uint16_t)((EEPROM_MAX_RECORDS * 80U + 99U) / 100U); // 1588
    bool warn_triggered = false;

    for (uint16_t i = 0U; i < EEPROM_MAX_RECORDS; i++) {
        sensor_record_t rec = {
            .timestamp    = 10000U + i,
            .temperature  = 250,
            .humidity     = 550U,
            .lux          = 200U,
            .status_flags = REC_FLAG_DHT22_VALID | REC_FLAG_GY30_VALID,
            .crc16        = 0U,
        };
        uint8_t alert = STORAGE_ALERT_NONE;
        TEST_ASSERT_EQUAL(ESP_OK, eeprom_bb_push_record(&rec, &alert));

        if ((i + 1U) >= warn_threshold) {
            TEST_ASSERT_TRUE((alert & STORAGE_ALERT_EEPROM_80) != 0U);
            warn_triggered = true;
        }
    }
    TEST_ASSERT_TRUE(warn_triggered);
    TEST_ASSERT_FALSE(s_page_wrap_violated);
    TEST_ASSERT_EQUAL_UINT16(EEPROM_MAX_RECORDS, eeprom_bb_get_record_count());

    // 2. 模擬 SD 卡轉存失敗（Callback 回傳 ESP_FAIL）：嚴禁淨空分區 B，1984 筆資料必須完整保留
    s_flush_should_succeed = false;
    sensor_record_t overflow_rec = {
        .timestamp    = 99999U,
        .temperature  = 265,
        .humidity     = 500U,
        .lux          = 400U,
        .status_flags = REC_FLAG_DHT22_VALID,
        .crc16        = 0U,
    };
    uint8_t alert_after = STORAGE_ALERT_NONE;
    TEST_ASSERT_EQUAL(ESP_FAIL, eeprom_bb_push_record(&overflow_rec, &alert_after));
    TEST_ASSERT_EQUAL_UINT16(EEPROM_MAX_RECORDS, eeprom_bb_get_record_count());
    TEST_ASSERT_EQUAL_UINT32(0U, s_flushed_record_count);

    // 3. 模擬 SD 卡轉存成功（Callback 回傳 ESP_OK）：淨空分區 B 並將第 1985 筆寫入索引 0
    s_flush_should_succeed = true;
    TEST_ASSERT_EQUAL(ESP_OK, eeprom_bb_push_record(&overflow_rec, &alert_after));
    TEST_ASSERT_EQUAL_UINT32(EEPROM_MAX_RECORDS, s_flushed_record_count);
    TEST_ASSERT_EQUAL_UINT32(10000U, s_flushed_first_ts);
    TEST_ASSERT_EQUAL_UINT32(10000U + EEPROM_MAX_RECORDS - 1U, s_flushed_last_ts);
    TEST_ASSERT_EQUAL_UINT16(1U, eeprom_bb_get_record_count());

    sensor_record_t latest;
    TEST_ASSERT_EQUAL(ESP_OK, eeprom_bb_read_records(0U, &latest, 1U));
    TEST_ASSERT_EQUAL_UINT32(99999U, latest.timestamp);
}